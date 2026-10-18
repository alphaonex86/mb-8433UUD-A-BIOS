/*
 * cachetest: is the L2 cache of the MB-8433UUD-A real and working? Run it under Linux with each BIOS
 * (the original Award and mb-8433UUD) and compare.
 *  - the UM8881F registers the BIOS left (50h: L2 enable/size/banks/write-back, 55h: BIOS cacheable),
 *    read from /sys (root);
 *  - memory read speed by block size: the 486's L1 (8 or 16 KB) and a working L2 show as steps; a fake
 *    or disabled L2 shows none between the L1 size and 2 MB.
 * Build for the 486 (static, uClibc: a host glibc uses i686 instructions), e.g. with the HDD image's toolchain:
 *   i486-buildroot-linux-uclibc-gcc -O2 -static -o cachetest tools/cachetest.c
 * test/cachetest.sh checks the verdict on synthetic speed curves (-DSELFTEST, host gcc).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_BLOCK (2u << 20)

static double now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) {
		perror("clock_gettime");
		exit(1);
	}
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* Reads 'bytes' from buf: 16 unrolled loads per 64 bytes, so the CPU waits on memory, not on its own
   instructions (a C loop ran at the same speed from the L1 and from DRAM). */
static void read_block(const volatile uint32_t *buf, uint32_t bytes)
{
	uint32_t n = bytes / 64, addr = (uint32_t)(uintptr_t)buf, val;
	__asm__ volatile("1: movl (%1), %0; movl 4(%1), %0; movl 8(%1), %0; movl 12(%1), %0\n"
			 "movl 16(%1), %0; movl 20(%1), %0; movl 24(%1), %0; movl 28(%1), %0\n"
			 "movl 32(%1), %0; movl 36(%1), %0; movl 40(%1), %0; movl 44(%1), %0\n"
			 "movl 48(%1), %0; movl 52(%1), %0; movl 56(%1), %0; movl 60(%1), %0\n"
			 "addl $64, %1; decl %2; jnz 1b" : "=&r"(val), "+r"(addr), "+r"(n) : : "memory");
}

/* MB/s reading 'bytes' again and again for about 0.1 s, the best of 5 runs (interrupts only slow a run down;
   the 486 has no TSC: the clock is coarse). */
static double read_speed(const volatile uint32_t *buf, uint32_t bytes)
{
	double best = 0;

	for (int run = 0; run < 5; run++) {
		double start = now(), elapsed;
		uint64_t total = 0;
		read_block(buf, bytes);                 /* warm up */
		do {
			for (uint32_t done = 0; done < (1u << 20); done += bytes)   /* the clock read costs ~10 us */
				read_block(buf, bytes);
			total += bytes < (1u << 20) ? (1u << 20) / bytes * bytes : bytes;
			elapsed = now() - start;
		} while (elapsed < 0.1);
		if (total / elapsed / (1 << 20) > best)
			best = total / elapsed / (1 << 20);
	}
	return best;
}

/* UM8881F 50h, -1 when not readable */
static int chipset(void)
{
	unsigned char conf[256];
	FILE *f = fopen("/sys/bus/pci/devices/0000:00:10.0/config", "rb");

	if (!f || fread(conf, 1, sizeof(conf), f) != sizeof(conf) || conf[0] != 0x60 || conf[1] != 0x10) {
		printf("UM8881F registers: not readable (run as root on the MB-8433UUD-A)\n");
		if (f)
			fclose(f);
		return -1;
	}
	fclose(f);
	unsigned r50 = conf[0x50], r55 = conf[0x55], size = (r50 & 7) && (r50 & 7) <= 5 ? 32u << (r50 & 7) : 0;
	printf("UM8881F 50h=%02x: L2 %u KB, %s, %s, %d bank(s); 51h=%02x; 55h=%02x: BIOS %scacheable\n",
	       r50, size, r50 & 0x80 ? "enabled" : "disabled", r50 & 0x40 ? "write-back" : "write-through",
	       r50 & 8 ? 2 : 1, conf[0x51], r55, r55 & 0x20 ? "" : "not ");
	return r50;
}

/* Index of the L1 size (last block within 75% of the fastest) and of the L2 size (last block 1.5 times main
   memory or more, -1: none; userspace paging slows big blocks down by itself). An L2 (64 KB or more) spans at
   least two blocks above the L1: one alone is the L1's partial hits. */
static void verdict(const double *speed, int n, int *l1, int *l2)
{
	int above = 0;
	*l1 = 0;
	*l2 = -1;
	for (int i = 0; i < n; i++)
		if (speed[i] >= 0.75 * speed[0])
			*l1 = i;
	for (int i = *l1 + 1; i < n - 1; i++)
		if (speed[i] >= 1.5 * speed[n - 1]) {
			*l2 = i;
			above++;
		}
	if (above < 2)
		*l2 = -1;
}

#ifdef SELFTEST
int main(void)
{
	/* 4K..2M: a 16K L1 and a 256K L2; the same without L2; L1 only up to 8K and no L2 */
	static const double real[10] = { 100, 98, 97, 50, 49, 48, 47, 20, 20, 20 };
	static const double none[10] = { 100, 98, 97, 21, 20, 20, 20, 20, 20, 20 };
	static const double small[10] = { 100, 99, 30, 21, 20, 20, 20, 20, 20, 20 };
	int l1, l2, fails = 0;
	verdict(real, 10, &l1, &l2);
	fails += l1 != 2 || l2 != 6;
	verdict(none, 10, &l1, &l2);
	fails += l1 != 2 || l2 != -1;
	verdict(small, 10, &l1, &l2);
	fails += l1 != 1 || l2 != -1;
	printf("%s [cachetest]\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
#else
int main(void)
{
	uint32_t *buf = malloc(MAX_BLOCK);
	double speed[16];
	uint32_t block[16];
	int n = 0;

	if (!buf) {
		perror("malloc");
		return 1;
	}
	for (uint32_t i = 0; i < MAX_BLOCK / 4; i++)
		buf[i] = i;
	int r50 = chipset();
	printf("block     MB/s\n");
	for (uint32_t b = 4096; b <= MAX_BLOCK; b *= 2, n++) {
		block[n] = b;
		speed[n] = read_speed(buf, b);
		printf("%5u K  %7.1f\n", b >> 10, speed[n]);
	}

	int l1, l2;
	double dram = speed[n - 1];
	verdict(speed, n, &l1, &l2);
	printf("L1: about %u KB\n", block[l1] >> 10);
	if (r50 >= 0 && !(r50 & 0x80))
		printf("L2: off (UM8881F 50h bit 7 clear: the BIOS did not enable it)\n");
	else if (l2 >= 0)
		printf("L2: works, about %u KB (%.1f MB/s vs %.1f from main memory)\n", block[l2] >> 10, speed[l2], dram);
	else
		printf("L2: no speed-up above the L1: disabled, fake or broken (%.1f MB/s from main memory)\n", dram);
	free(buf);
	return 0;
}
#endif
