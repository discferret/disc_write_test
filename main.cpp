#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <unistd.h>

#include "discferret/discferret.h"

using namespace std;

typedef enum { 
	WRGATE_WRITE,
	WRGATE_READ
} WRITE_GATE;
enum {
	CMD_WAIT_TIMER_N	= 0x80,
	CMD_WAIT_INDEX_N	= 0x40,
	CMD_STOP			= 0x3F,
	CMD_WAIT_HSTMD		= 0x03,
	CMD_TRANSITION		= 0x02,
	CMD_WR_GATE_N		= 0x00
};

// CRC-16/CCITT implementation
// Based on code from http://www.sanity-free.org/133/crc_16_ccitt_in_csharp.html
class CRC16 {
	private:
		static const unsigned int poly = 4129;
		unsigned int table[256];
		unsigned int crcval, initval;
	public:
		CRC16(const unsigned int initval = 0xFFFF) {
			this->initval = this->crcval = initval;

			unsigned int temp, a;
			for (size_t i=0; i<(sizeof(this->table)/sizeof(this->table[0])); ++i) {
				temp = 0;

				a = (unsigned int) i << 8;
				
				for (int j=0; j < 8; ++j) {
					if(((temp ^ a) & 0x8000) != 0) {
						temp = (unsigned int)((temp << 1) ^ poly);
					} else {
						temp <<= 1;
					}
					a <<= 1;
				}

				table[i] = temp;
			}
		}

		// calculate crc without updating internal state
		unsigned int calculate(const void *buf, const size_t len) {
			unsigned int crc = this->crcval;

			for (size_t i=0; i<len; i++) {
				char x = ((unsigned char *)buf)[i];
				crc = ((crc << 8) ^ this->table[((crc >> 8) ^ x) & 0xff]) & 0xffff;
			}

			return crc;
		}

		// calculate crc and update internal state afterwards
		// used for partial crcs
		unsigned int update(const void *buf, const size_t len) {
			this->crcval = this->calculate(buf, len);
			return this->crcval;
		}

		// calculate crc and update internal state afterwards
		// used for partial crcs
		unsigned int update(const uint8_t ch) {
			this->crcval = this->calculate(&ch, 1);
			return this->crcval;
		}


		// reset crc to initialisation default
		void reset(void) {
			this->crcval = this->initval;
		}

		// reset crc to arbitrary value and change init default accordingly
		void reset(unsigned int initval) {
			this->crcval = this->initval = initval;
		}

		unsigned int crc() {
			return this->crcval;
		}
};

class Track {
	private:
		vector<uint8_t> _buf;
		WRITE_GATE _gate_state = WRGATE_READ;
		uint32_t _timestep = 0;

	public:
		void reset(void)
		{
			_buf.clear();
			_timestep = 0;
			_gate_state = WRGATE_READ;
		}

		uint32_t get_time(void)
		{
			return _timestep;
		}

		size_t get_buf(uint8_t *dest)
		{
			if (dest != NULL) {
				for (size_t i=0; i < _buf.size(); i++) {
					dest[i] = _buf[i];
				}
			}
			return _buf.size();
		}

		void emit_wrgate(WRITE_GATE state)
		{
			_gate_state = state;
			if (state == WRGATE_WRITE) {
				_buf.push_back(CMD_WR_GATE_N + 1);
			} else {
				_buf.push_back(CMD_WR_GATE_N + 0);
			}
			_timestep++;
		}

		void emit_flux(uint32_t time)
		{
			uint32_t t = time;

			while (t > 0) {
				if (t > 129) {
					_buf.push_back(CMD_WAIT_TIMER_N + 127);
					t = t - 129;
				} else if (t >= 2) {
					_buf.push_back(CMD_WAIT_TIMER_N + (t - 2));
					t = 0;
				} else {
					this->emit_wrgate(this->_gate_state);
					t = t - 1;
				}
			}
			_buf.push_back(CMD_TRANSITION);
			_timestep += time + 1;
		}

		void emit_wait_index(int n=1)
		{
			assert(n > 0);

			while (n > 0) {
				int x = (n > 0x3F) ? 0x3F : n;
				n -= x;
				_buf.push_back(CMD_WAIT_INDEX_N + x);
			}
		}
		
		void emit_stop()
		{
			_buf.push_back(CMD_STOP);
		}
};

class BitTrack {
	private:
		bool _last_data_bit;

	public:
		vector<bool> _buf;

		void reset(void)
		{
			_buf.clear();
			_last_data_bit = false;
		}

		void raw(uint32_t data, size_t nbits=16)
		{
			assert(nbits <= 32);

			for (unsigned int i=1; i <= nbits; i++) {
				_buf.push_back(data & (1 << (nbits-i)));
			}
			_last_data_bit = data & 1;
		}

		void mfm(uint8_t data)
		{
			for (unsigned int i=1; i <= 8; i++) {
				bool this_bit = data & (1 << (8 - i));

				// clock = prev NOR curr
				_buf.push_back(!(_last_data_bit | this_bit));
				// data is always data
				_buf.push_back(this_bit);
				_last_data_bit = this_bit;
			}
		}

		void fm(uint8_t data)
		{
			for (unsigned int i=1; i <= 8; i++) {
				bool this_bit = data & (1 << (8 - i));

				// clock is always true
				_buf.push_back(true);
				// data is always data
				_buf.push_back(this_bit);
				_last_data_bit = this_bit;
			}
		}

		/**
		 * @brief Convert the contents of the BitTrack into a track.
		 *
		 * @param t Track class
		 * @param cell_time Time for one bit cell
		 */
		void toTrack(Track &t, uint32_t cell_time, unsigned int precomp = 0)
		{
			uint32_t nzeroes = 0;
			uint8_t shiftreg = 0;
			int n = 0;

			for (size_t pos = 0; pos < _buf.size() + 2; pos++) {
				bool bit = (pos < _buf.size()) ? _buf[pos] : 0;
				shiftreg = (shiftreg << 1) | (bit ? 1 : 0);

				// 2-bit delay through the shift register
				if (n < 2) {
					n++;
					continue;
				}

				// figure out if we need to apply precompensation
				signed int precomp_mul;
				switch (shiftreg & 0x1F) {
					case 0x05:	// 00101 (MFM x011) - causes early shift
						precomp_mul = 1;
						break;
					case 0x14:	// 10100 (MFM x110) - causes late shift
						precomp_mul = -1;
						break;
					default:
						precomp_mul = 0;
						break;
				}

				// grab the bit out of the shift register
				bit = shiftreg & 4;

				if (bit) {
					/***
					 * '1' bit. Calculate time delta and emit a flux transition.
					 * We add 1 here because there's a minimum gap of 1 cell between adjacent flux transitions.
					 * Each element in the _buf vector is 1 if there is a transition in that cell.
					 */
					t.emit_flux((cell_time / 2) + (nzeroes * (cell_time/2)) + (precomp_mul * precomp));
					nzeroes = 0;
				} else {
					// '0' bit -- increment the "number of zeroes seen" value
					nzeroes++;
				}
			}
		}
};


int main(void)
{
	DISCFERRET_DEVICE_HANDLE *dh;
	DISCFERRET_ERROR e;

	if ((e = discferret_init()) != DISCFERRET_E_OK) {
		printf("Error initialising DiscFerret -- code %d\n", e);
		return EXIT_FAILURE;
	}

	// Open first DiscFerret device
	if ((e = discferret_open_first(&dh)) != DISCFERRET_E_OK) {
		printf("Error opening DiscFerret -- code %d\n", e);
		
	}

	// Load microcode
	printf("downloading microcode...\n");
	assert(discferret_fpga_load_default(dh) == DISCFERRET_E_OK);

	printf("write...\n");
	uint32_t clock = 100e6;
	// Start preparing write data
	Track t;

	// Start with a DC erase
	t.emit_wait_index();
	t.emit_wrgate(WRGATE_WRITE);
	t.emit_wait_index();
	t.emit_wait_index();


	BitTrack bt;
	for (int i=0; i<80; i++)	// Pre-index gap (or something like it?)
		bt.mfm(0x4E);			// "GAP4a"
	for (int i=0; i<12; i++)
		bt.mfm(0x00);			// "SYNC"

	for (int i=0; i<3; i++)
		bt.raw(0x5224);			// 0xC2 with missing clock between 3 and 4 -- before IAM -- Index Mark
	bt.mfm(0xFC);				// IAM -- Index Address Mark

	for (int i=0; i<50; i++)
		bt.mfm(0x4E);			// GAP1 - Postindex Gap

	uint8_t track = 0;
	uint8_t side = 0;
	uint8_t crc0, crc1;
	uint8_t data[512];

	for (int i=0; i<sizeof(data); i++)
		data[i] = rand();

	/**
	 * Sector length byte (N):
	 *   00   128
	 *   01   256
	 *   02   512
	 *   03   1024
	 *   04   2048
	 *   05   4096
	 */

	for (int sector=0; sector<9; sector++) {
		// SYNC
		for (int i=0; i<12; i++)
			bt.mfm(0x00);

		CRC16 crc = CRC16();

		// IDAM
		for (int i=0; i<3; i++) {
			bt.raw(0x4489);		// 0xA1 with missing clock between 4 and 5 -- before IDAM or DAM
			crc.update(0xA1);
		}

/// emit and update crc
#define G(x) { bt.mfm(x); crc.update(x); }

		G(0xFE);		// IDAM -- ID Address Mark

		// ID Record starts here
		G(track);
		G(side);
		G(sector);
		G(0x02);					// sector length
		G(crc.crc() >> 8);			// CRC16
		G(crc.crc() & 0xFF);

		// GAP2
		for (int i=0; i<22; i++)
			bt.mfm(0x4E);

		// SYNC
		for (int i=0; i<12; i++)
			bt.mfm(0x00);

		crc.reset();
		// Data Record starts here
		for (int i=0; i<3; i++) {	// 3x A1-sync
			bt.raw(0x4489);
			crc.update(0xA1);
		}
		G(0xFB);					// Data Address Mark
		for (int i=0; i<sizeof(data); i++) {	// Data payload
			G(data[i]);
		}
		G(crc0);					// CRC16
		G(crc1);					// CRC16

		for (int i=0; i<0x50; i++)	// GAP3 -- Data gap
			bt.mfm(0x4E);
#undef G
	}

	for (int i=0; i<145; i++)		// GAP4b -- Postgap -- ideally this should continue to the index pulse
		bt.mfm(0x4E);

	/***
	 * DiscFerret clock = 100MHz = 10ns/clk
	 *
	 * Data rate = 250kbps = 4us/cell
	 */

	bt.toTrack(t, 400, 13);		// 10ns per clock @ 100MHz
							// 4us per cell = 4000ns;   4000/10 = 200 counts
							// 250ns precomp / 10ns = 25 counts
							// 125ns precomp / 10ns = 12.5 counts (rounds up to 13)

/*
	// Some flux transitions...
	//for (int i=0; i<20000; i++) {
	while (t.get_buf(NULL) < 500*1024) {
		t.emit_flux(800);
		//t.emit_flux(600);
		t.emit_flux(400);
	}
*/
/*
	t.emit_stop();

	for (int i=0; i<20000; i++) {
		t.emit_flux(150);
	}
	for (int i=0; i<20000; i++) {
		t.emit_flux(200);
	}
	for (int i=0; i<20000; i++) {
		t.emit_flux(250);
	}
*/
	// write DC to EOT
	//t.emit_wait_index();
	t.emit_wrgate(WRGATE_READ);

	// pointless wait
//	t.emit_wait_index();
//	t.emit_wait_index();
//	t.emit_wait_index();

	// end of program
	t.emit_stop();

	printf("copybuf len %zu...\n", t.get_buf(NULL));

	if (t.get_buf(NULL) > 512*1024) {
		printf("too big.\n");
		return -1;
	}

	printf("time to write %u clocks (%0.2f ms)\n", t.get_time(), ((float)t.get_time() / (float)clock) * 1000.0);

	// copy buffer to discferret ram
	uint8_t *ram = new uint8_t[t.get_buf(NULL)];
	t.get_buf(ram);

	printf("first few ram bytes: ");
	for (int i=0; i<16; i++) printf("%02X ", ram[i]);
	printf("\n");
	printf("last  few ram bytes: ");
	size_t n=t.get_buf(NULL);
	for (size_t i=n-16; i<n; i++) printf("%02X ", ram[i]);
	printf("\n");

	discferret_ram_addr_set(dh, 0);
	discferret_ram_write(dh, ram, t.get_buf(NULL));
	discferret_ram_addr_set(dh, 0);
	delete ram;

	// Set DF hardware options
	discferret_reg_poke(dh, DISCFERRET_R_ACQ_START_EVT, DISCFERRET_ACQ_EVENT_ALWAYS);
	discferret_reg_poke(dh, DISCFERRET_R_ACQ_START_NUM, 0);
	discferret_reg_poke(dh, DISCFERRET_R_ACQ_STOP_EVT, DISCFERRET_ACQ_EVENT_NEVER);
	discferret_reg_poke(dh, DISCFERRET_R_ACQ_STOP_NUM, 0);
	discferret_reg_poke(dh, DISCFERRET_R_ACQ_CLKSEL, DISCFERRET_ACQ_RATE_100MHZ);

	// set write pulse width
	discferret_reg_poke(dh, 0xD0, 60);

	// Abort any running reads or writes
	discferret_reg_poke(dh, DISCFERRET_R_ACQCON, DISCFERRET_ACQCON_ABORT);
	discferret_reg_poke(dh, DISCFERRET_R_ACQCON, 0);

	// Turn the disc drive motor on
	//discferret_reg_poke(dh, DISCFERRET_R_DRIVE_CONTROL, DISCFERRET_DRIVE_CONTROL_DS0 | DISCFERRET_DRIVE_CONTROL_MOTEN | DISCFERRET_DRIVE_CONTROL_DS2);
	discferret_reg_poke(dh, DISCFERRET_R_DRIVE_CONTROL, DISCFERRET_DRIVE_CONTROL_DS0 | DISCFERRET_DRIVE_CONTROL_DS2);
	//discferret_reg_poke(dh, DISCFERRET_R_DRIVE_CONTROL, DISCFERRET_DRIVE_CONTROL_DS1 | DISCFERRET_DRIVE_CONTROL_MOTEN);
	sleep(2);
	double indx;
	discferret_get_index_frequency(dh, true, &indx);
	printf("Disc rotation rate is %0.3lf RPM\n", indx);

	// Seek to track zero
	printf("recalibrate...\n");
	discferret_seek_set_rate(dh, 8000);	// 8000us = 8ms
	printf("recal: %d\n", discferret_seek_recalibrate(dh, 80));

	// start writing
	printf("start write, s=%lX\n", discferret_get_status(dh));
	printf("DEBUG %d MA %ld\n", discferret_reg_peek(dh, 0xDB), discferret_ram_addr_get(dh));
	discferret_reg_poke(dh, DISCFERRET_R_ACQCON, DISCFERRET_ACQCON_WRITE);
	printf("poke acqcon write done, s=%lX\n", discferret_get_status(dh));
	printf("DEBUG %d MA %ld\n", discferret_reg_peek(dh, 0xDB), discferret_ram_addr_get(dh));

	// wait for write complete
	uint32_t x;
	do {
		x = discferret_get_status(dh);
		//printf("Stat %X debug %d maddr %ld\n", x, discferret_reg_peek(dh, 0xDB), discferret_ram_addr_get(dh));
	} while ((x & DISCFERRET_STATUS_ACQSTATUS_MASK) != DISCFERRET_STATUS_ACQ_IDLE);

	// Turn the disc drive off
	discferret_reg_poke(dh, DISCFERRET_R_DRIVE_CONTROL, 0);

	// Close the DiscFerret and shut down
	discferret_close(dh);
	discferret_done();

	return EXIT_SUCCESS;
}
