#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <unistd.h>

#include "discferret/discferret.h"

#include "CRC16.h"
#include "Track.h"
#include "BitTrack.h"
#include "imagedisk.h"

using namespace std;

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

	// Load ImageDisk file
	fstream f("01_Diagnostic_Disk_Ver_3.51.IMD", ios::in | ios::binary);
	IMDImage imd(f);

	return 0;

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

	// write DC to EOT
	//t.emit_wait_index();
	t.emit_wrgate(WRGATE_READ);

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
