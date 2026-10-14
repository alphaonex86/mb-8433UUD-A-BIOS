/* POST memory test (setup.c memtest()), quick and full, on free RAM ranges of random bounds holding random data,
   with 0-2 stuck DRAM bits on dwords it tests: reports the lowest one exactly, keeps every other dword, and
   flushes the cache twice per 4 KiB block (full) or per 1 MiB (quick: its 1 dword per KiB fill the 4 KiB save
   buffer). */
#include "biosvar.h"
#include "output.h"
#include "util.h"
#include "devfuzz.h"

#define RAM_SIZE (6 << 20)
#define NCLASSES 2
static const char *const ClassNames[NCLASSES] = { "quick", "full" };
static const unsigned ClassCount = 100;

static u32 Ram[RAM_SIZE / 4] __attribute__((aligned(4096)));
static u32 Ref[RAM_SIZE / 4];
static struct { u32 addr, bit, value; } Fault[2];
static int NFaults, Flushes;

static void stuck_bits(void)
{
	Flushes++;
	for (int f = 0; f < NFaults; f++) {
		u32 *d = (u32 *)Fault[f].addr;
		*d = (*d & ~Fault[f].bit) | Fault[f].value;
	}
}

static int class_of(unsigned index)
{
	return index / ClassCount;
}

static unsigned count(void)
{
	return NCLASSES * ClassCount;
}

/* Free ranges, in rising order, and the stuck bits on dwords the test reads (quick: 1 KiB aligned). */
static void build(unsigned index)
{
	int quick = class_of(index) == 0;
	u32 base = (u32)Ram, at = base + (fuzz_rand() % 64) * 4;
	fuzz_nfree = 1 + fuzz_rand() % 3;
	for (int n = 0; n < fuzz_nfree; n++) {
		u32 len = 4 * (fuzz_rand() % ((quick ? 1600 << 10 : 400 << 10) / 4));
		fuzz_free[n][0] = at;
		fuzz_free[n][1] = at + len;
		at += len + 4 * (fuzz_rand() % 2048);
	}
	NFaults = fuzz_rand() % 3;
	for (int f = 0; f < NFaults; f++) {
		int n = fuzz_rand() % fuzz_nfree;
		u32 first = ALIGN(fuzz_free[n][0], 4096), last = ALIGN_DOWN(fuzz_free[n][1], 4096);
		if (last <= first) {
			NFaults = f;
			break;
		}
		u32 unit = quick ? 1024 : 4;
		Fault[f].addr = first + unit * (fuzz_rand() % ((last - first) / unit));
		Fault[f].bit = 1u << fuzz_rand() % 32;
		Fault[f].value = fuzz_rand() % 2 ? Fault[f].bit : 0;
	}
}

static void describe(unsigned index, char *buf, int size)
{
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "free");
	for (int r = 0; r < fuzz_nfree && n < size - 24; r++)
		n += snprintf(buf + n, size - n, " +%x-+%x", fuzz_free[r][0] - (u32)Ram, fuzz_free[r][1] - (u32)Ram);
	for (int f = 0; f < NFaults && n < size - 24; f++)
		n += snprintf(buf + n, size - n, ", +%x bit %x stuck %d", Fault[f].addr - (u32)Ram, Fault[f].bit,
			      !!Fault[f].value);
}

static int faulty(u32 addr)
{
	for (int f = 0; f < NFaults; f++)
		if (Fault[f].addr == addr)
			return 1;
	return 0;
}

static void run(unsigned index)
{
	int quick = class_of(index) == 0;
	u32 chunk = quick ? 1 << 20 : 4096, flushes = 0, expect = 0;
	build(index);
	fuzz_bios_init();
	for (u32 i = 0; i < RAM_SIZE / 4; i++)
		Ref[i] = Ram[i] = fuzz_rand();
	for (int r = 0; r < fuzz_nfree; r++) {
		u32 first = ALIGN(fuzz_free[r][0], 4096), last = ALIGN_DOWN(fuzz_free[r][1], 4096);
		if (last > first)
			flushes += 2 * ((last - first + chunk - 1) / chunk);
	}
	for (int f = 0; f < NFaults; f++)
		if (!expect || Fault[f].addr < expect)
			expect = Fault[f].addr;
	fuzz_wbinvd_hook = stuck_bits;
	u32 bad = fuzz_setup_memtest(quick);
	fuzz_wbinvd_hook = NULL;
	if (bad != expect)
		fuzz_fail("reported bad address %x, expected %x", bad, expect);
	if (Flushes != (int)flushes)
		fuzz_fail("%d cache flushes, expected %u", Flushes, flushes);
	for (u32 i = 0; i < RAM_SIZE / 4; i++)
		if (Ram[i] != Ref[i] && !faulty((u32)&Ram[i]))
			fuzz_fail("dword at +%x changed: %x, was %x", i * 4, Ram[i], Ref[i]);
}

const struct fuzz_area fuzz_memtest = { "memtest", NCLASSES, ClassNames, count, class_of, describe, run };
