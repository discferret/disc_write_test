class BitTrack {
	private:
		bool _last_data_bit;

	public:
		std::vector<bool> _buf;

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

