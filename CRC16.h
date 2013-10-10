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

