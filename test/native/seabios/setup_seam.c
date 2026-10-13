/* setup.c compiled unchanged; its static POST and setup screens are reached from here. */
#include "setup.c"
#include "devfuzz.h"

u32 LegacyRamSize = 32 << 20;   /* malloc.c's, from the e820 map (setup_l2_cache) */

void fuzz_setup_init(void)
{
    setup_init();
}

/* The setup's disk benchmark on every hard disk the drivers found: read errors end it, results within range;
   then its result and comparison pages are drawn (VGA memory and text calls of the models). */
int fuzz_setup_disk_bench(void)
{
	int ok = 0;
	hd_list();
	for (int h = 0; h < HdCount; h++) {
		int ret = hd_test(h, NULL);
		if (!ret && (!Hd[h].tested || Hd[h].error))
			fuzz_fail("disk benchmark %d: success without results", h);
		if (ret && !Hd[h].error)
			fuzz_fail("disk benchmark %d: failure without an error", h);
		ok += !ret;
		hd_show(h);
	}
	bench_disk_bars();
	return ok;
}

/* Every screen that shows what the drivers found: POST lines and summary, setup pages. */
void fuzz_setup_screens(void)
{
    int i;
    scr_open();
    for (i = 0; i < 4; i++)
        setup_ide_detect(i, 1);
    setup_post_summary();
    values_load();
    draw_std();
    info_ide();
    info_pci();
    info_isa();
    scr_close();
}

/* The USB page as text, one line per row from its first list row; trailing blanks dropped. */
void fuzz_setup_usb_text(char *out, int size)
{
    int row, col, n = 0;
    scr_open();
    info_usb();
    for (row = 5; row <= 19; row++) {
        int last = -1;
        for (col = 0; col < 80; col++)
            if ((u8)Vram[row * 80 + col] > ' ')
                last = col;
        for (col = 3; col <= last && n < size - 2; col++)        /* info_line starts at column 3 */
            out[n++] = (u8)Vram[row * 80 + col] ? (u8)Vram[row * 80 + col] : ' ';
        out[n++] = '\n';
    }
    out[n] = 0;
    scr_close();
}

void fuzz_setup_isapnp(void)
{
    setup_isapnp();
}

void fuzz_setup_pnp(void)
{
    setup_pnp();
}

void fuzz_setup_post_warnings(void)
{
    setup_post_warnings();
}

int fuzz_setting(int index)
{
    return Setting[index];
}

void fuzz_set_setting(int index, int value)
{
    Setting[index] = value;
}

void fuzz_set_hdd(int idx, int type, unsigned cyl, unsigned heads, unsigned spt)
{
    Hdd[idx] = (struct hdd){ type, heads, spt, cyl };
}

/* CMOS image of the settings and drive types, as setup's Save & Exit writes it. */
void fuzz_cmos_store(void)
{
    cmos_write(Setting, Hdd, Password);
}

void fuzz_ideinfo(int idx, struct fuzz_ide *out)
{
    *out = (struct fuzz_ide){ IdeInfo[idx].present, IdeInfo[idx].atapi, IdeInfo[idx].sectors, IdeInfo[idx].cyl,
                              IdeInfo[idx].heads, IdeInfo[idx].spt, IdeInfo[idx].pio, IdeInfo[idx].pio_max,
                              IdeInfo[idx].multiple, IdeInfo[idx].model, IdeInfo[idx].drive };
}

/* CMOS area: the setup's own range checks, the setup utility, the boot password. */
static int item_of(int var, const struct item **out)
{
    int p, i;
    for (p = 0; p < ARRAY_SIZE(Pages); p++)
        for (i = 0; i < Pages[p].count; i++)
            if (Pages[p].items[i].var == var && (var >= NB_COUNT || !(Pages[p].items[i].flags & F_CYL))) {
                *out = &Pages[p].items[i];
                return 1;
            }
    return 0;
}

int fuzz_setting_max(int var)
{
    const struct item *it;
    return item_of(var, &it) ? it->hi : 15;
}

/* NULL when every setting has an item and holds a value the item allows; else what is wrong. */
const char *fuzz_check_values(const u8 *values, char *why, int size)
{
    const struct item *it;
    int var;
    for (var = 0; var < NB_COUNT; var++) {
        if (!item_of(var, &it)) {
            snprintf(why, size, "setting %d has no setup item: never range-checked", var);
            return why;
        }
        if (!item_valid_value(it, values[var])) {
            snprintf(why, size, "setting %d = %d, outside %d-%d of \"%s\"", var, values[var], it->lo, it->hi,
                     it->label ?: "");
            return why;
        }
    }
    return NULL;
}

const char *fuzz_check_settings(char *why, int size)
{
    int i;
    for (i = 0; i < 4; i++)
        if (Hdd[i].type > NB_HDD_USER) {
            snprintf(why, size, "drive %d type %d", i, Hdd[i].type);
            return why;
        }
    return fuzz_check_values(Setting, why, size);
}

/* The compiled-in Setup and BIOS (fail-safe) defaults. */
const char *fuzz_check_defaults(char *why, int size)
{
    u8 failsafe[NB_COUNT];
    int i;
    if (fuzz_check_values(Defaults, why, size))
        return why;
    memcpy(failsafe, Defaults, sizeof(failsafe));
    for (i = 0; i < ARRAY_SIZE(FailSafe); i++)
        failsafe[FailSafe[i].var] = FailSafe[i].value;
    return fuzz_check_values(failsafe, why, size);
}

void fuzz_cmos_fix_sums(void)
{
    cmos_put_sum(NB_CMOS_BASE, NB_CMOS_SIG, NB_CMOS_SUM);
    cmos_put_sum(0x10, 0x2d, CMOS_AT_SUM);
}

int fuzz_cmos_valid(void)
{
    return cmos_valid();
}

u16 fuzz_password_hash(const char *s)
{
    return password_hash(s);
}

u16 fuzz_password(void)
{
    return Password;
}

void fuzz_setup_menu(void)
{
    setup_menu();
}

void fuzz_setup_boot_password(void)
{
    setup_boot_password();
}

void fuzz_setup_post(void)
{
    setup_post_banner();
    setup_boot_delay();
    setup_finish();
}
