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
	uint32_t clock = 200e6;
	// Start preparing write data
	Track t;

	// Start with a DC erase
	t.emit_wait_index();
	t.emit_wrgate(WRGATE_WRITE);
	t.emit_wait_index();
	t.emit_wait_index();

	// Some flux transitions...
	//for (int i=0; i<20000; i++) {
	while (t.get_buf(NULL) < 500*1024) {
		t.emit_flux(300);
		//t.emit_flux(300);
		//t.emit_flux(200);
	}

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
