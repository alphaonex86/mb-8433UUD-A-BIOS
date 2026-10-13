/* CMOS contents (the SeaBIOS side; coreboot's is cmosfuzz.sh): corrupt or valid-but-out-of-range settings,
   drive types, RTC registers, standard bytes and passwords, through setup_init(), the floppy setup, the
   POST screens and prompts, the clock, the boot password and the setup utility driven by key scripts
   (pages, defaults, save), then a reboot on what was saved. */
#include "biosvar.h"
#include "hw/pcidevice.h"
#include "hw/rtc.h"
#include "newbios_cmos.h"
#include "output.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

#define MAX_KEYS 96

static struct {
	int cls;
	u8 cmos[128], mask[128];                    /* bits the case writes over the defaults */
	int fix_sums, sum_mode, at_bad;
	char password[12];                          /* typed at the password prompts */
	u16 keys[MAX_KEYS];
	int nkeys;
} S;

static const u8 E8[] = { 0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 0x0f, 0x10 };

static void put(int reg, u8 v)
{
	S.cmos[reg] = v;
	S.mask[reg] = 0xff;
}

/* Setting nibble (0-15) of the nibble-packed block. */
static void put_setting(int var, u8 v)
{
	int reg = NB_CMOS_BASE + var / 2, shift = var % 2 * 4;
	S.cmos[reg] = (S.cmos[reg] & ~(0x0f << shift)) | (v & 0x0f) << shift;
	S.mask[reg] |= 0x0f << shift;
}

static void typed(const char *s)
{
	for (; *s && S.nkeys < MAX_KEYS; s++)
		S.keys[S.nkeys++] = 0x1e00 | (u8)*s;
	if (S.nkeys < MAX_KEYS)
		S.keys[S.nkeys++] = 0x1c0d;
}

static void random_keys(int n)
{
	static const u16 keys[] = { 0x011b, 0x4800, 0x5000, 0x4b00, 0x4d00, 0x4900, 0x5100, 0x3f00, 0x4000, 0x4100,
				    0x4400, 0x1c0d, 0x1559, 0x314e, 0x0d2b, 0x0c2d, 0x0b30, 0x0231, 0x0a39, 0x0e08 };
	while (n-- > 0 && S.nkeys < MAX_KEYS)
		S.keys[S.nkeys++] = keys[fuzz_rand() % ARRAY_SIZE(keys)];
}

enum { C_SETTINGS, C_COMBOS, C_SIGNATURE, C_HDD, C_RTC, C_BYTES, C_BOOTFAIL, C_BOOT_ORDER, C_PASSWORD, C_MENU,
       C_RANDOM, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"each setting at 0, max, max+1, 15", "setting combinations", "signature and checksums",
	"HDD types and User geometry", "RTC registers A-D, BCD, 12h, binary", "standard bytes (10h-32h, 7Fh, password)",
	"unfinished-POST counter", "boot order", "passwords", "setup utility key scripts", "random CMOS",
};
static const unsigned ClassCount[NCLASSES] = { NB_COUNT * 4, 1000, 4 * 4 * 2, 11 * 5 * 4 * 4, 4 * 5 * 2 * 4,
					       17 * 8 * 2, 256 * 2, 7 * 7 * 7 * 7, 4 * 3 * 5, 2000, 1000 };

static int class_of(unsigned index)
{
	int c = 0;
	while (index >= ClassCount[c])
		index -= ClassCount[c++];
	return c;
}

static unsigned count(void)
{
	unsigned n = 0;
	for (int c = 0; c < NCLASSES; c++)
		n += ClassCount[c];
	return n;
}

static void build(unsigned index)
{
	int cls = class_of(index);
	unsigned i = index;
	for (int c = 0; c < cls; c++)
		i -= ClassCount[c];
	memset(&S, 0, sizeof(S));
	S.cls = cls;
	S.fix_sums = 1;
	switch (cls) {
	case C_SETTINGS: {
		int var = i / 4, max = fuzz_setting_max(var);
		put_setting(var, (const int[]){ 0, max, max + 1, 15 }[i % 4]);
		break;
	}
	case C_COMBOS:
		for (int k = 2 + fuzz_rand() % 8; k > 0; k--) {
			int var = fuzz_rand() % NB_COUNT, max = fuzz_rand() % 2 ? fuzz_setting_max(var) : 15;
			put_setting(var, (const int[]){ 0, max, max + 1, 15, fuzz_rand() }[fuzz_rand() % 5]);
		}
		break;
	case C_SIGNATURE:
		S.fix_sums = 0;
		put(NB_CMOS_SIG, (const u8[]){ NB_SIG_VALUE, 0, 0xff, NB_SIG_VALUE ^ 1 }[i % 4]);
		S.sum_mode = i / 4 % 4;                 /* NB checksum: 0 right, 1 off by one, 2 zero, 3 FFFF */
		S.at_bad = i / 16;
		break;
	case C_HDD: {
		static const u8 types[11] = { 0, 1, 2, 16, 17, 47, 48, 49, 0x7f, 0x80, 0xff };
		static const u16 cyl[5] = { 0, 1, 1023, 1024, 0xffff };
		static const u8 heads[4] = { 0, 1, 16, 255 }, spt[4] = { 0, 1, 63, 255 };
		int at = NB_CMOS_HDD + 5 * (fuzz_rand() % 4);
		put(at, types[i % 11]);
		put(at + 1, cyl[i / 11 % 5]), put(at + 2, cyl[i / 11 % 5] >> 8);
		put(at + 3, heads[i / 55 % 4]);
		put(at + 4, spt[i / 220]);
		put_setting(NB_IDE_MODE0 + (at - NB_CMOS_HDD) / 5, fuzz_rand() % 16);
		random_keys(30);
		break;
	}
	case C_RTC: {
		static const u8 a[4] = { 0x26, 0xa6, 0xff, 0x00 }, b[5] = { 0x02, 0x00, 0x06, 0x04, 0xff };
		put(0x0a, a[i % 4]);
		put(0x0b, b[i / 4 % 5]);
		put(0x0d, i / 20 % 2 ? 0x00 : 0x80);
		for (int r = 0; r < 10; r++)            /* time and date: valid BCD, all FF, invalid BCD, PM flag */
			put(r, (const u8[]){ 0x12, 0xff, 0x9a, 0x92 }[i / 40]);
		put(0x32, (const u8[]){ 0x19, 0xff, 0x9a, 0x00 }[i / 40]);
		random_keys(20);
		break;
	}
	case C_BYTES: {
		static const u8 regs[17] = { 0x0f, 0x10, 0x12, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x30, 0x31, 0x32,
					     0x7e, 0x7f, 0x78, 0x79 };
		put(regs[i % 17], E8[i / 17 % 8]);
		S.fix_sums = i / 136 == 0;
		break;
	}
	case C_BOOTFAIL:
		put(NB_CMOS_BOOTFAIL, i % 256);
		put_setting(NB_CHIPSET_AUTO, i / 256);
		break;
	case C_BOOT_ORDER: {
		static const u8 v[7] = { 0, 1, 2, 3, 4, 5, 15 };
		for (int k = 0, n = i; k < 4; k++, n /= 7)
			put_setting(NB_BOOT1 + k, v[n % 7]);
		break;
	}
	case C_PASSWORD: {
		static const char *const words[5] = { "ABC", "", "WRONG", "ABCDEFGHIJK", "AB\bC" };
		u16 hash = (const u16[]){ 0, 1, 0xffff, fuzz_password_hash("ABC") }[i % 4];
		put(NB_CMOS_PASSWORD, hash), put(NB_CMOS_PASSWORD + 1, hash >> 8);
		put_setting(NB_SECURITY, i / 4 % 3);
		strtcpy(S.password, words[i / 12], sizeof(S.password));
		for (int k = 0; k < 6; k++)
			typed(S.password);
		random_keys(10);
		break;
	}
	case C_MENU:
		for (int k = fuzz_rand() % 6; k > 0; k--)
			put_setting(fuzz_rand() % NB_COUNT, fuzz_rand() % 16);
		random_keys(20 + fuzz_rand() % 70);
		break;
	default:
		for (int r = 0x0e; r < 0x80; r++)
			put(r, fuzz_rand() % 3 ? fuzz_rand() : E8[fuzz_rand() % 8]);
		S.fix_sums = fuzz_rand() % 2;
		random_keys(20);
	}
}

static void describe(unsigned index, char *buf, int size)
{
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "sums %s, password typed \"%s\", CMOS", S.fix_sums ? "fixed" : "as is", S.password);
	for (int r = 0; r < 128 && n < size - 12; r++)
		if (S.mask[r])
			n += snprintf(buf + n, size > n ? size - n : 0, " %02x=%02x/%02x", r, S.cmos[r], S.mask[r]);
	n += snprintf(buf + n, size > n ? size - n : 0, "; keys");
	for (int k = 0; k < S.nkeys && n < size - 6; k++)
		n += snprintf(buf + n, size > n ? size - n : 0, " %04x", S.keys[k]);
}

/****************************************************************
 * Run and checks
 ****************************************************************/

void floppy_setup(void);
void clock_setup(void);

static void apply(void)
{
	for (int r = 0; r < 128; r++)
		fuzz_cmos[r] = (fuzz_cmos[r] & ~S.mask[r]) | (S.cmos[r] & S.mask[r]);
	if (S.fix_sums)
		fuzz_cmos_fix_sums();
	if (S.cls == C_SIGNATURE) {
		fuzz_cmos[NB_CMOS_SUM + 1] += S.sum_mode == 1;
		if (S.sum_mode >= 2)
			fuzz_cmos[NB_CMOS_SUM] = fuzz_cmos[NB_CMOS_SUM + 1] = S.sum_mode == 2 ? 0 : 0xff;
		fuzz_cmos[0x2f] ^= S.at_bad;
	}
}

static void check(const char *when)
{
	char why[160];
	if (fuzz_check_settings(why, sizeof(why)))
		fuzz_fail("after %s: %s", when, why);
}

static void run(unsigned index)
{
	char why[160];
	void *done[5];
	if (fuzz_check_defaults(why, sizeof(why)))
		fuzz_fail("compiled-in defaults: %s", why);
	build(index);
	fuzz_bios_init();
	fuzz_cmos_model();
	fuzz_pci_board();
	fuzz_pci_model();
	fuzz_setup_init();                          /* blank CMOS: defaults written with valid sums */
	apply();
	pci_probe_devices();
	fuzz_budget_us = fuzz_now_us + 600000000;
	fuzz_setup_init();
	check("setup_init");
	floppy_setup();
	clock_setup();
	fuzz_setup_post();
	fuzz_setup_post_warnings();
	fuzz_setup_screens();
	fuzz_post_us = fuzz_now_us;

	fuzz_keys = S.keys;
	fuzz_nkeys = S.nkeys;
	if (!__builtin_setjmp(done)) {
		fuzz_keys_done = done;
		if (!fuzz_setting(NB_SECURITY) || !fuzz_password()
		    || fuzz_password_hash(S.password) == fuzz_password())
			fuzz_setup_boot_password();         /* a wrong one halts the machine, by design */
		fuzz_setup_menu();
	}
	fuzz_note("setup left with %d of %d keys unused", fuzz_nkeys, S.nkeys);
	fuzz_keys_done = NULL;
	fuzz_keys = NULL;
	fuzz_setup_init();                          /* reboot on what the setup saved */
	check("setup and reboot");
	if (!fuzz_cmos_valid())
		fuzz_fail("CMOS invalid after setup_init rewrote it");
}

const struct fuzz_area fuzz_cmos_area = { "cmos", NCLASSES, ClassNames, count, class_of, describe, run };
