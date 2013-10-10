#include <iostream>
#include <fstream>
#include <vector>
#include <exception>

/// Sector types
typedef enum {
	IMDS_NONE,			/// Sector data not available, couldn't be read
	IMDS_NORMAL,		/// Normal sector
	IMDS_DELETED,		/// Deleted-data address mark
	IMDS_NORMAL_DERR,	/// Normal sector read with data error
	IMDS_DELETED_DERR	/// Deleted sector read with data error
} IMDSectorType;

typedef enum {
	IMDM_FM_500KBPS,
	IMDM_FM_300KBPS,
	IMDM_FM_250KBPS,
	IMDM_MFM_500KBPS,
	IMDM_MFM_300KBPS,
	IMDM_MFM_250KBPS,
} IMDDataEncoding;

class EIMDNotValid : public std::exception {};

class IMDSector {
	public:
		std::vector<uint8_t> data;
		unsigned int logical_cylinder, logical_head, logical_sector;
		IMDSectorType type;

		IMDSector(std::istream &in, unsigned int cyl, unsigned int head, unsigned int sec, unsigned int ssz)
		{
			// Set the C/H/S values
			logical_cylinder = cyl;
			logical_head = head;
			logical_sector = sec;

			// Read and decode the sector format byte
			char b;
			bool is_compressed = false;
			in.read(&b, 1);
			switch (b) {
				case 0x00:		// Sector data unavailable - could not be read.
					type = IMDS_NONE;
					break;
				case 0x01:		// Normal data
					type = IMDS_NORMAL;
					break;
				case 0x02:		// Normal data -- all bytes have the same value (compressed)
					type = IMDS_NORMAL;
					is_compressed = true;
					break;
				case 0x03:		// Deleted data
					type = IMDS_DELETED;
					break;
				case 0x04:		// Deleted data -- all bytes have the same value
					type = IMDS_DELETED;
					is_compressed = true;
					break;
				case 0x05:		// Normal data read with data error
					type = IMDS_NORMAL_DERR;
					break;
				case 0x06:		// Normal data read with data error -- all with same value
					type = IMDS_NORMAL_DERR;
					is_compressed = true;
					break;
				case 0x07:		// Deleted data read with data error
					type = IMDS_DELETED_DERR;
					break;
				case 0x08:		// Deleted data read with data error -- all with same value
					type = IMDS_DELETED_DERR;
					is_compressed = true;
					break;
				default:
					// unrecognised coding scheme
					throw EIMDNotValid();
			}

			std::cout << "\tchs " << cyl << ":" << head << ":" << sec << " - ";
			std::cout << ssz << " bytes, type " << type << ", " << (is_compressed ? "compressed" : "raw");
			std::cout << std::endl;

			// If there is no sector data, exit.
			if (type == IMDS_NONE) {
				return;
			}

			// Read the sector data
			if (!is_compressed) {
				// Uncompressed data
				char *x = new char[ssz];
				in.read(x, ssz);
				for (size_t i = 0; i < ssz; i++) {
					data.push_back((unsigned char)x[i]);
				}
				delete x;
			} else {
				// Compressed data -- all bytes in the sector have the same value
				in.read(&b, 1);
				for (size_t i = 0; i < ssz; i++) {
					data.push_back((unsigned char)b);
				}
			}
		}
};

class IMDTrack {
	public:
		std::vector<IMDSector> sectors;
		IMDDataEncoding encoding;
		unsigned int phys_cyl;
		unsigned int phys_head;
		unsigned int sector_size;

		IMDTrack(std::istream &in)
		{
			char b;
			bool has_scm, has_shm;
			unsigned int num_sectors;

			// Mode value -- data rate and encoding scheme
			in.read(&b, 1);
			switch (b) {
				case 0:		// 500kbps FM
					encoding = IMDM_FM_500KBPS; break;
				case 1:		// 300kbps FM
					encoding = IMDM_FM_300KBPS; break;
				case 2:		// 250kbps FM
					encoding = IMDM_FM_250KBPS; break;
				case 3:		// 500kbps MFM
					encoding = IMDM_MFM_500KBPS; break;
				case 4:		// 300kbps MFM
					encoding = IMDM_MFM_300KBPS; break;
				case 5:		// 250kbps MFM
					encoding = IMDM_MFM_250KBPS; break;
				default:
					// Unknown encoding scheme
					throw EIMDNotValid();
			}

			// Physical Cylinder
			in.read(&b, 1); phys_cyl = (unsigned char)b;

			// Head and flags
			// The actual head number can only be zero or one; the remaining
			// bits are used for flags.
			in.read(&b, 1); phys_head = (unsigned char)b & 1;
			has_scm = (b & 0x80);
			has_shm = (b & 0x40);

			// Number of Sectors
			in.read(&b, 1); num_sectors = (unsigned char)b;

			// Sector Size Byte
			in.read(&b, 1); sector_size = (unsigned char)b;

			// Sector Numbering Map
			char *sector_num_map = new char[num_sectors];
			in.read(sector_num_map, num_sectors);

			// Optional Sector Cylinder Map
			char *sector_cyl_map = NULL;
			if (has_scm) {
				sector_cyl_map = new char[num_sectors];
				in.read(sector_cyl_map, num_sectors);
			}

			// Optional Head Map
			char *sector_head_map = NULL;
			if (has_shm) {
				sector_head_map = new char[num_sectors];
				in.read(sector_head_map, num_sectors);
			}

			// Convert sector size into bytes
			size_t sector_bytes = (128 << sector_size);

			std::cout << "Track " << phys_cyl << "/" << phys_head << " - encoding " << encoding << std::endl;

			// Sector Data
			for (unsigned int x = 0; x < num_sectors; x++) {
				IMDSector s(in,
						has_scm ? sector_cyl_map[x] : phys_cyl,		// cylinder
						has_shm ? sector_head_map[x] : phys_head,	// head
						sector_num_map[x],							// sector
						sector_bytes);								// sector size in bytes
			}
		}
};

class IMDImage {
	private:
		std::vector<IMDTrack> _tracks;
		std::string _header;
		std::string _comment;
	public:
		IMDImage(std::fstream &in)
		{
			// Get the file size
			in.seekp(0, std::ios::end);
			std::streampos fsize = in.tellp();
			in.seekp(0, std::ios::beg);

			// IMD files start with an "IMD v.vv: " header
			std::getline(in, _header);
			if ((_header.compare(0, 4, "IMD ") != 0) ||
					(_header[5] != '.') || (_header[8] != ':') || (_header[9] != ' ') ||
					!isdigit(_header[4]) || !isdigit(_header[6]) || !isdigit(_header[7])
			   ) {
				throw EIMDNotValid();
			} else {
				// TODO: decode version number and date
				std::cout << "IMD valid --> " << _header << std::endl;
			}

			// If the header is valid, it's fair to assume we have an IMD file. Next read the comment.
			std::getline(in, _comment, '\x1A');
			std::cout << "IMD comment: [" << _comment << "]" << std::endl;

			// Repeat for every track in the image...
			do {
				IMDTrack t(in);
				_tracks.push_back(t);
			} while (in.tellp() < fsize);
		}
};

