/*
 * flashbios: rewrite the BIOS flash of the MB-8433UUD-A from Linux (root). The BIOS runs from its shadow RAM copy,
 * so the chip is free. Only a Winbond W49F002A/U (2 Mbit, programmed at 5 V by the chip itself: JP13 at 5 V);
 * the board's original Intel 28F001BX-T needs 12 V and a programmer.
 *   flashbios -n IMAGE   identify the chip and compare with IMAGE (256 KiB, e.g. out/release/newbios-2mbit.rom)
 *   flashbios IMAGE      rewrite the blocks that differ (the boot block last) and verify them
 *   flashbios -r FILE    read the chip into FILE
 * Flash writes are enabled as the BIOS does for its ESCD: UM8886 47h bit 6 off, 57h bits 5 and 2 on (and 7-6:
 * the chip's lower half decoded at FFFC0000), UM8881 55h bit 6 on; all restored at the end. If anything fails
 * after an erase, the board may not boot: program the image with a programmer (minipro -p W49F002A@DIP32).
 * Build (static, i486): i486-buildroot-linux-uclibc-gcc -O2 -static -o flashbios tools/flashbios.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHIP_SIZE 0x40000
#define CHIP_BASE 0xfffc0000u

/* W49F002A/U, top boot: two main blocks, two 8K parameter blocks, the 16K boot block last */
static const uint32_t Blocks[6] = { 0x00000, 0x20000, 0x38000, 0x3a000, 0x3c000, 0x40000 };

static volatile uint8_t *Flash;
static int HostBridge = -1, IsaBridge = -1;
static uint8_t Saved47, Saved57, Saved55;

/* A UMC device (one of two IDs) at dev: its config space, open for reading and writing */
static int pci_open(const char *dev, uint16_t device, uint16_t device2)
{
	char path[64];
	uint8_t id[4];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/0000:%s/config", dev);
	int fd = open(path, O_RDWR);
	uint16_t found = 0;
	if (fd < 0 || pread(fd, id, 4, 0) != 4 || (id[0] | id[1] << 8) != 0x1060
	    || ((found = id[2] | id[3] << 8) != device && found != device2)) {
		fprintf(stderr, "flashbios: %s is not UMC %04x (not the MB-8433UUD-A, or not root)\n", dev, device);
		exit(1);
	}
	return fd;
}

static uint8_t cfg_read(int fd, int reg)
{
	uint8_t val;
	if (pread(fd, &val, 1, reg) != 1) {
		perror("flashbios: PCI config read");
		exit(1);
	}
	return val;
}

static void cfg_write(int fd, int reg, uint8_t val)
{
	if (pwrite(fd, &val, 1, reg) != 1) {
		perror("flashbios: PCI config write");
		exit(1);
	}
}

static void writes_enable(void)
{
	Saved47 = cfg_read(IsaBridge, 0x47);
	Saved57 = cfg_read(IsaBridge, 0x57);
	Saved55 = cfg_read(HostBridge, 0x55);
	cfg_write(IsaBridge, 0x47, Saved47 & ~0x40);
	cfg_write(IsaBridge, 0x57, Saved57 | 0xe4);
	cfg_write(HostBridge, 0x55, Saved55 | 0x40);
}

static void writes_restore(void)
{
	cfg_write(HostBridge, 0x55, Saved55);
	cfg_write(IsaBridge, 0x57, Saved57);
	cfg_write(IsaBridge, 0x47, Saved47);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void command(uint8_t cmd)
{
	Flash[0x5555] = 0xaa;
	Flash[0x2aaa] = 0x55;
	Flash[0x5555] = cmd;
}

/* Waits until DQ6 stops toggling (the embedded algorithm ended): 0, or -1 after 'seconds'. */
static int wait_done(uint32_t addr, double seconds)
{
	double end = now() + seconds;
	uint8_t last = Flash[addr];
	for (;;) {
		uint8_t val = Flash[addr];
		if (!((val ^ last) & 0x40))
			return 0;
		last = val;
		if (now() > end)
			return -1;
	}
}

static int block_matches(const uint8_t *image, int b)
{
	for (uint32_t a = Blocks[b]; a < Blocks[b + 1]; a++)
		if (Flash[a] != image[a])
			return 0;
	return 1;
}

static int block_write(const uint8_t *image, int b)
{
	command(0x80);
	Flash[0x5555] = 0xaa;
	Flash[0x2aaa] = 0x55;
	Flash[Blocks[b]] = 0x30;
	if (wait_done(Blocks[b], 10)) {
		fprintf(stderr, "flashbios: erase of block %05x did not end\n", Blocks[b]);
		return -1;
	}
	for (uint32_t a = Blocks[b]; a < Blocks[b + 1]; a++) {
		if (image[a] == 0xff)
			continue;
		command(0xa0);
		Flash[a] = image[a];
		if (wait_done(a, 0.01)) {
			fprintf(stderr, "flashbios: programming %05x did not end\n", a);
			return -1;
		}
	}
	return block_matches(image, b) ? 0 : -1;
}

int main(int argc, char **argv)
{
	int dry = argc == 3 && !strcmp(argv[1], "-n"), dump = argc == 3 && !strcmp(argv[1], "-r");
	const char *file = argv[argc - 1];
	static uint8_t image[CHIP_SIZE];

	if (argc != 2 && !dry && !dump) {
		fprintf(stderr, "usage: flashbios [-n] IMAGE | -r FILE\n");
		return 1;
	}
	HostBridge = pci_open("00:10.0", 0x8881, 0x8881);
	IsaBridge = pci_open("00:12.0", 0x886a, 0x888a);   /* UM8886BF: the board's; 86Box's */
	if (!(cfg_read(HostBridge, 0x55) & 0x80)) {
		fprintf(stderr, "flashbios: segment F is not read from shadow RAM: the BIOS would run from the chip\n");
		return 1;
	}
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0) {
		perror("flashbios: /dev/mem");
		return 1;
	}
	Flash = mmap(NULL, CHIP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem, CHIP_BASE);
	if (Flash == MAP_FAILED) {
		perror("flashbios: mmap of the flash");
		return 1;
	}
	if (!dump) {
		FILE *f = fopen(file, "rb");
		if (!f || fread(image, 1, CHIP_SIZE, f) != CHIP_SIZE || fgetc(f) != EOF) {
			fprintf(stderr, "flashbios: %s: not a 256 KiB image\n", file);
			return 1;
		}
		fclose(f);
		if (image[CHIP_SIZE - 16] != 0xea && image[CHIP_SIZE - 16] != 0xe9) {
			fprintf(stderr, "flashbios: %s has no jump at the reset vector: not a BIOS image\n", file);
			return 1;
		}
	}

	writes_enable();
	uint8_t array[2] = { Flash[0], Flash[1] };
	command(0x90);
	uint8_t maker = Flash[0], device = Flash[1], locked = Flash[2] & 1;
	command(0xf0);
	Flash[0] = 0xf0;
	if (maker == array[0] && device == array[1]) {
		writes_restore();
		fprintf(stderr, "flashbios: the chip did not answer its ID command (writes blocked?): nothing written\n");
		return 1;
	}
	printf("chip %02x:%02x, boot block %s\n", maker, device, locked ? "LOCKED" : "unlocked");
	if (maker != 0xda || (device != 0x0b && device != 0xae)) {
		writes_restore();
		fprintf(stderr, "flashbios: not a Winbond W49F002A/U: nothing written\n");
		return 1;
	}

	if (dump) {
		FILE *f = fopen(file, "wb");
		for (uint32_t a = 0; a < CHIP_SIZE; a++)
			image[a] = Flash[a];
		writes_restore();
		if (!f || fwrite(image, 1, CHIP_SIZE, f) != CHIP_SIZE || fclose(f)) {
			perror("flashbios: writing the dump");
			return 1;
		}
		printf("%s: the chip, 256 KiB\n", file);
		return 0;
	}
	int differ[5], count = 0;
	for (int b = 0; b < 5; b++) {
		differ[b] = !block_matches(image, b);
		count += differ[b];
		printf("block %05x-%05x: %s\n", Blocks[b], Blocks[b + 1] - 1, differ[b] ? "differs" : "same");
	}
	if (locked && differ[4]) {
		writes_restore();
		fprintf(stderr, "flashbios: the boot block differs but is locked: nothing written\n");
		return 1;
	}
	if (dry || !count) {
		writes_restore();
		printf(count ? "dry run: nothing written\n" : "the chip already holds this image\n");
		return 0;
	}
	for (int b = 0; b < 5; b++) {
		if (!differ[b])
			continue;
		printf("rewriting block %05x... ", Blocks[b]);
		fflush(stdout);
		if (block_write(image, b) && block_write(image, b)) {
			writes_restore();
			printf("FAILED\n");
			fprintf(stderr, "flashbios: block %05x does not verify: program the chip with a programmer before the "
				"next reset\n", Blocks[b]);
			return 1;
		}
		printf("verified\n");
	}
	writes_restore();
	printf("done: the new BIOS runs at the next power on or reset\n");
	return 0;
}
