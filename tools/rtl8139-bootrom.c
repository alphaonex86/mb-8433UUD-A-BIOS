/* rtl8139-bootrom [SIZE]: the boot ROM size of each RTL8139 card, as root. A RTL8139 shows the ROM in its socket (e.g.
   Etherboot) to the BIOS only when its 93C46 EEPROM selects a size (CONFIG0 BS2-0, EEPROM byte 14h): with 000 the
   card has no expansion ROM BAR at all, whatever the BIOS (the chip ignores BS2-0 written at run time). SIZE (0, 8,
   16, 32, 64 or 128 KiB) rewrites that EEPROM word, verified; it takes effect at the next reset or power on. The
   whole EEPROM is printed before. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>
#include <unistd.h>

#define EE_CS 0x08
#define EE_SK 0x04
#define EE_DI 0x02
#define EE_DO 0x01
#define EE_PROGRAM 0x80   /* 9346CR: EEPROM programming mode */
#define CONFIG0_WORD 0x0a

static unsigned cr;       /* 9346CR port */

static void ee_out(unsigned v)
{
	outb(EE_PROGRAM | v, cr);
	inb(cr);
}

static void ee_bits(unsigned bits, int n)   /* chip selected (CS low first), n bits sent MSB first */
{
	ee_out(0);
	ee_out(EE_CS);
	for (int i = n - 1; i >= 0; i--) {
		unsigned d = (bits >> i) & 1 ? EE_DI : 0;
		ee_out(EE_CS | d);
		ee_out(EE_CS | d | EE_SK);
	}
	ee_out(EE_CS);
}

static void ee_end(void)
{
	ee_out(0);
	outb(0, cr);
}

static unsigned ee_read(int addr)
{
	unsigned v = 0;
	ee_bits(0x180 | addr, 9);             /* start bit, READ (10), 6-bit address */
	for (int i = 0; i < 16; i++) {
		ee_out(EE_CS | EE_SK);
		v = (v << 1) | (inb(cr) & EE_DO);
		ee_out(EE_CS);
	}
	ee_end();
	return v;
}

static int ee_write(int addr, unsigned v)
{
	int ready = 0;
	ee_bits(0x130, 9);                    /* EWEN: write enable */
	ee_end();
	ee_bits((0x140 | addr) << 16 | v, 25);  /* WRITE */
	ee_out(0);
	ee_out(EE_CS);                        /* DO high when the write is done */
	for (int t = 0; t < 100 && !ready; t++) {
		ready = inb(cr) & EE_DO;
		if (!ready)
			usleep(1000);
	}
	ee_end();
	ee_bits(0x100, 9);                    /* EWDS: write disable */
	ee_end();
	return ready ? 0 : -1;
}

static int card(const char *dev, int size)
{
	char path[300];
	unsigned long io = 0, start, end, flags;
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", dev);
	FILE *fp = fopen(path, "r");
	while (fp && fscanf(fp, "%lx %lx %lx", &start, &end, &flags) == 3 && !io)
		if (flags & 0x100)                /* IORESOURCE_IO */
			io = start;
	if (fp)
		fclose(fp);
	if (!io || ioperm(io, 0x100, 1)) {
		fprintf(stderr, "%s: no I/O ports (%s)\n", dev, io ? "ioperm failed" : "no I/O BAR");
		return 1;
	}
	cr = io + 0x50;
	if (ee_read(0) != 0x8129) {
		fprintf(stderr, "%s: no 93C46 EEPROM (word 0 is not 8129h)\n", dev);
		return 1;
	}
	printf("%s at I/O %lx, EEPROM:", dev, io);
	for (int w = 0; w < 64; w++)
		printf("%s%02x:%04x", w % 8 ? " " : "\n  ", w, ee_read(w));
	unsigned word = ee_read(CONFIG0_WORD), bs = word & 7;
	printf("\n  boot ROM: %s", bs ? "" : "none (no expansion ROM BAR)");
	if (bs)
		bs <= 5 ? printf("%d KiB", 4 << bs) : printf("reserved value %u", bs);
	printf(" (CONFIG0 %02xh)\n", word & 0xff);
	if (size < 0)
		return 0;
	unsigned want = 0;
	while (size && (4u << ++want) != (unsigned)size)
		;
	unsigned new = (word & ~7u) | want;
	if (new == word) {
		printf("  unchanged\n");
		return 0;
	}
	if (ee_write(CONFIG0_WORD, new) || ee_read(CONFIG0_WORD) != new) {
		fprintf(stderr, "%s: EEPROM write of word %02xh = %04x failed (now %04x)\n", dev, CONFIG0_WORD, new,
			ee_read(CONFIG0_WORD));
		return 1;
	}
	printf("  EEPROM word %02xh: %04x -> %04x; reset or power cycle the PC to apply it\n", CONFIG0_WORD, word, new);
	return 0;
}

int main(int argc, char **argv)
{
	int size = -1, found = 0, rc = 0;
	if (argc > 1) {
		size = atoi(argv[1]);
		if (argc > 2 || (size && size != 8 && size != 16 && size != 32 && size != 64 && size != 128)) {
			fprintf(stderr, "usage: rtl8139-bootrom [0|8|16|32|64|128]  (boot ROM size in KiB)\n");
			return 2;
		}
	}
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[300], id[16] = "";
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
		FILE *fp = fopen(path, "r");
		if (!fp)
			continue;
		unsigned vendor = fgets(id, sizeof(id), fp) ? strtoul(id, NULL, 16) : 0;
		fclose(fp);
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", e->d_name);
		fp = fopen(path, "r");
		unsigned device = fp && fgets(id, sizeof(id), fp) ? strtoul(id, NULL, 16) : 0;
		if (fp)
			fclose(fp);
		if (vendor == 0x10ec && (device == 0x8139 || device == 0x8039)) {
			found++;
			rc |= card(e->d_name, size);
		}
	}
	if (d)
		closedir(d);
	if (!found) {
		fprintf(stderr, "no RTL8139 card\n");
		return 1;
	}
	return rc;
}
