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

class Track {
	private:
		std::vector<uint8_t> _buf;
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

