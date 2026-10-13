/* UM8881F DRAM/L2 model, a port of the 86Box fork's chipset/umc_hb4.c (same names and decode), plus the
   normal-mode L2 that the fork does not model. The SIMM data is kept as cells, which equals the fork's
   flat RAM wherever its decode is one-to-one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "um8881_model.h"

#define MA_ROW 0x10
#define MA_NC 0xff
#define MIN(a, b) ((a) < (b) ? (a) : (b))

const struct simm simm_types[SIMM_TYPES] = {
	{ 0, 0, 0, 0 }, { 10, 10, 1, 0 }, { 10, 10, 2, 0 }, { 11, 11, 1, 0 }, { 10, 12, 1, 0 },
	{ 11, 11, 2, 0 }, { 10, 12, 2, 0 }, { 11, 12, 2, 0 }, { 12, 12, 1, 0 }, { 12, 12, 2, 0 }
};

static const uint8_t ma_high[2][5] = {
	{ 9, MA_ROW | 10, 10, MA_NC, MA_NC },
	{ MA_ROW | 10, MA_ROW | 11, 9, 10, 11 }
};

static struct region hb4_region(const uint8_t *regs, int upper)
{
	struct region r;
	int code_l = regs[0] & 7;
	int code = upper ? ((regs[0] >> 4) & 7) : code_l;
	int cfg = regs[1] >> (upper ? 2 : 0);

	r.base = (upper && code_l) ? (1u << (20 + code_l)) : 0;
	r.size = (code && code_l) ? (1u << (20 + code)) : 0;
	r.banks = (cfg & 1) ? 2 : 1;
	r.sides = (cfg & 2) ? 2 : 1;
	r.bits = 20 + code - r.banks - r.sides + 2;
	r.first = (upper ^ !!(regs[0] & 8)) * 2;
	r.swap = (regs[1] >> (upper ? 5 : 4)) & 1;
	r.ma = ma_high[(regs[2] >> (upper ? 5 : 4)) & 1];
	return r;
}

uint32_t model_region_size(const uint8_t regs[3], int upper)
{
	return hb4_region(regs, upper).size;
}

static int hb4_ma_present(const struct simm *simm, uint8_t ma)
{
	if (ma == MA_NC)
		return 0;
	return (ma & MA_ROW) ? ((ma & 0x0f) < simm->rows) : (ma < simm->cols);
}

static int64_t hb4_dram_decode(const struct model *m, const struct region regions[2], uint32_t addr)
{
	struct region r = regions[0];

	if (addr - r.base >= r.size)
		r = regions[1];
	if (addr - r.base >= r.size)
		return -1;

	uint32_t offset = addr - r.base;
	uint32_t above = offset >> r.bits;
	int bank = ((r.banks == 2) ? (above >> (r.sides - 1)) : 0) ^ r.swap;
	uint32_t side = above & (r.sides - 1);
	const struct simm *simm = &m->simm[r.first + bank];
	uint32_t col = (offset >> 2) & 0x1ff;
	uint32_t row = (offset >> 11) & 0x3ff;

	if (side >= simm->sides)
		return -1;
	for (int bit = 21; bit < MIN(r.bits, 26); bit++) {
		uint8_t ma = r.ma[bit - 21];
		if (((offset >> bit) & 1) && (ma != MA_NC)) {
			if (ma & MA_ROW)
				row |= 1 << (ma & 0x0f);
			else
				col |= 1 << ma;
		}
	}
	col &= (1u << simm->cols) - 1;
	row &= (1u << simm->rows) - 1;
	return simm->offset + ((uint64_t)side << (simm->cols + simm->rows + 2)) +
	       ((((row << simm->cols) | col) << 2) | (addr & 3));
}

/* hb4_dram_decode with the MA loop as tables: for the current registers, cross-checked against the
   loop by the exact runs. */
static void region_tables(struct region *r)
{
	int hi_bits = MIN(r->bits, 26) - 21;

	r->hi_mask = hi_bits > 0 ? (1u << hi_bits) - 1 : 0;
	for (uint32_t hi = 0; hi < 32; hi++) {
		r->col_hi[hi] = r->row_hi[hi] = 0;
		for (int bit = 0; bit < 5; bit++) {
			uint8_t ma = r->ma[bit];
			if (!((hi & r->hi_mask) >> bit & 1) || ma == MA_NC)
				continue;
			if (ma & MA_ROW)
				r->row_hi[hi] |= 1 << (ma & 0x0f);
			else
				r->col_hi[hi] |= 1 << ma;
		}
	}
}

static int64_t fast_decode(const struct model *m, uint32_t addr, int *row0_shift)
{
	const struct region *r = &m->region[0];

	if (addr - r->base >= r->size)
		r = &m->region[1];
	if (addr - r->base >= r->size)
		return -1;

	uint32_t offset = addr - r->base;
	uint32_t above = offset >> r->bits;
	int bank = ((r->banks == 2) ? (above >> (r->sides - 1)) : 0) ^ r->swap;
	uint32_t side = above & (r->sides - 1);
	const struct simm *simm = &m->simm[r->first + bank];
	uint32_t hi = (offset >> 21) & r->hi_mask;
	uint32_t col = (((offset >> 2) & 0x1ff) | r->col_hi[hi]) & ((1u << simm->cols) - 1);
	uint32_t row = (((offset >> 11) & 0x3ff) | r->row_hi[hi]) & ((1u << simm->rows) - 1);

	if (side >= simm->sides)
		return -1;
	*row0_shift = simm->cols + 2;
	return simm->offset + ((uint64_t)side << (simm->cols + simm->rows + 2)) +
	       ((((row << simm->cols) | col) << 2) | (addr & 3));
}

/* Within a 4 KiB page, address bits 0-10 are the cell's (column MA0-8) and bit 11 is row MA0. */
static int64_t dram_decode(struct model *m, uint32_t addr)
{
	if (m->exact_runs)
		return hb4_dram_decode(m, m->region, addr);
	if (addr >> 12 != m->decode_page) {
		m->decode_page = addr >> 12;
		m->decode_base = fast_decode(m, addr & ~0xfffu, &m->decode_row0);
	}
	if (m->decode_base < 0)
		return -1;
	return m->decode_base + (addr & 0x7ff) + ((int64_t)((addr >> 11) & 1) << m->decode_row0);
}

int64_t model_dram_decode(const struct model *m, const uint8_t regs[3], uint32_t addr)
{
	const struct region regions[2] = { hb4_region(regs, 0), hb4_region(regs, 1) };

	return hb4_dram_decode(m, regions, addr);
}

int model_region_flat(const struct model *m, const uint8_t regs[3], int upper)
{
	struct region r = hb4_region(regs, upper);

	for (int bank = 0; r.size && (bank < 2); bank++) {
		const struct simm *simm = &m->simm[r.first + bank];
		if ((r.banks == 1) && (bank != r.swap))
			continue;
		if (simm->sides < r.sides)
			return 0;
		for (int bit = 21; bit < r.bits; bit++)
			if ((bit >= 26) || !hb4_ma_present(simm, r.ma[bit - 21]))
				return 0;
	}
	return 1;
}

static int hb4_l2_forced(const struct model *m)
{
	return m->hb[0x5a] & 0x40;
}

static int64_t hb4_l2_byte(const struct model *m, uint32_t addr)
{
	uint8_t r50 = m->hb[0x50];
	uint32_t size = m->l2_eff;	/* MIN(installed, 0x8000 << (r50 & 7)) */

	if (r50 & 0x08) {
		if ((m->l2.banks == 1) && (addr & 4))
			return -1;
	} else if (m->l2.banks == 2)
		addr &= ~4u;
	return size ? (int64_t)(addr & (size - 1)) : -1;
}

static void fatal(const char *what, uint32_t value)
{
	fprintf(stderr, "um8881_model: %s %08x\n", what, value);
	exit(2);
}

/* Assumptions beyond umc_hb4.c, one per line:
   - fake or absent data SRAM: reads float (FFh, or the last value on the CPU bus), writes are lost;
   - fake or absent tag RAM: tag reads float to all ones, writes are lost;
   - normal mode: direct mapped, 16-byte lines, tag = address bits above the 50h size, 7 bits + dirty
     (51h bit 0 clear) or 8 bits, compared without a valid bit;
   - addresses at or above size << tag bits, above the DRAM top or in 640K-1M are not cached;
   - a read miss fills the whole line from DRAM whatever CR0.CD is (the CPU's cache state is not on
     the bus); a write miss does not allocate;
   - write-through (50h bit 6 clear): a write hit updates the line and DRAM; a fill keeps the dirty bit
     (hypothesis fitting the real board: random power-on dirty bits survived a write-through tag fill and
     crashed it once write-back was on);
   - write-back: a write hit updates only the line and sets its dirty bit (tag RAM bit 7; with 8-bit
     tags there is none and every replaced line is written back), a read miss first writes a dirty line
     back to the address its tag and the current index give;
   - wbinvd and CR0 leave the L2 as it is;
   - the tag RAM is indexed like the data SRAM: wraps at the installed and at the programmed size. */
static void hb4_ram_update(struct model *m)
{
	uint8_t regs[3] = { m->hb[0x52], m->hb[0x53], m->hb[0x61] };
	uint8_t r50 = m->hb[0x50];

	m->region[0] = hb4_region(regs, 0);
	m->region[1] = hb4_region(regs, 1);
	region_tables(&m->region[0]);
	region_tables(&m->region[1]);
	m->decode_page = UINT32_MAX;
	m->top = m->region[0].size + m->region[1].size;
	m->flat = (m->top >= 0x100000) && (m->top <= m->mem_size) && model_region_flat(m, regs, 0) &&
		  model_region_flat(m, regs, 1);
	m->l2_on = (r50 & 0x80) && (r50 & 7) && !hb4_l2_forced(m);
	m->l2_prog = 0x8000u << (r50 & 7);
	m->l2_eff = MIN(m->l2_size, m->l2_prog);
	m->l2_mask = (m->hb[0x51] & 1) ? 0xff : 0x7f;
	uint64_t end = (uint64_t)m->l2_prog * (m->l2_mask + 1u);
	m->l2_end = end < m->top ? (uint32_t)end : m->top;
	m->l2_wb = m->l2_on && (r50 & 0x40);
}

/* DRAM cells start zeroed, as the fork's; absent cells float high. */
static uint32_t dram_read(const struct model *m, int64_t cell)
{
	if (cell < 0)
		return 0xffffffff;
	uint32_t page = cell >> 12;
	return m->dram_gen[page] == m->gen ? m->dram_page[page][(cell >> 2) & 1023] : 0;
}

static void dram_write(struct model *m, int64_t cell, uint32_t val)
{
	if (cell < 0)
		return;
	uint32_t page = cell >> 12;
	if (m->dram_gen[page] != m->gen) {
		if (m->dram_used == DRAM_POOL)
			fatal("too many DRAM pages written, cell", cell);
		m->dram_page[page] = m->dram_pool + 1024 * m->dram_used++;
		memset(m->dram_page[page], 0, 4096);
		m->dram_gen[page] = m->gen;
	}
	m->dram_page[page][(cell >> 2) & 1023] = val;
}

/* The 4 cells of a line, which share a page. */
static void dram_read_line(const struct model *m, int64_t cell, uint32_t val[4])
{
	uint32_t page = cell >> 12;

	for (int dword = 0; dword < 4; dword++)
		val[dword] = cell < 0 ? 0xffffffff :
			     m->dram_gen[page] == m->gen ? m->dram_page[page][((cell >> 2) & 1023) + dword] : 0;
}

static uint32_t l2_data_read(const struct model *m, int64_t index)
{
	if (index < 0 || m->l2.data == L2_DATA_FLOAT_FF)
		return 0xffffffff;
	return m->l2.data == L2_DATA_FLOAT_BUS ? m->last_bus : m->l2_data[index >> 2];
}

static void l2_data_write(struct model *m, int64_t index, uint32_t val)
{
	if (index >= 0 && m->l2.data == L2_DATA_REAL)
		m->l2_data[index >> 2] = val;
}

uint32_t model_l2_cacheable_end(const struct model *m)
{
	return m->l2_end;
}

static int l2_cached(const struct model *m, uint32_t addr)
{
	return m->l2_on && addr < m->l2_end && (addr < 0xa0000 || addr >= 0x100000);
}

static uint32_t l2_tag_index(const struct model *m, uint32_t addr)
{
	return m->l2_eff ? (addr & (m->l2_eff - 1)) >> 4 : 0;
}

static uint8_t l2_tag_raw(const struct model *m, uint32_t addr)
{
	return (m->l2.fake_tags || !m->l2_size) ? 0xff : m->l2_tag[l2_tag_index(m, addr)];
}

static uint8_t l2_tag_read(const struct model *m, uint32_t addr)
{
	return l2_tag_raw(m, addr) & m->l2_mask;
}

static void l2_set_dirty(struct model *m, uint32_t addr)
{
	if (!m->l2.fake_tags && m->l2_size)
		m->l2_tag[l2_tag_index(m, addr)] |= 0x80;
}

/* Write-back of the line that `addr`'s slot holds, when dirty. */
static void l2_write_back(struct model *m, uint32_t addr)
{
	if (m->l2_mask != 0xff && !(l2_tag_raw(m, addr) & 0x80))
		return;
	uint32_t index = addr & (m->l2_prog - 1) & ~15u;
	uint32_t victim = l2_tag_read(m, addr) * m->l2_prog + index;
	for (uint32_t dword = 0; dword < 4; dword++)
		dram_write(m, dram_decode(m, victim + 4 * dword), l2_data_read(m, hb4_l2_byte(m, index + 4 * dword)));
}

static uint8_t l2_tag_of(const struct model *m, uint32_t addr)
{
	return (addr / m->l2_prog) & m->l2_mask;
}

/* Address bits 2-3 are column bits 0-1: a line is 4 consecutive DRAM cells. */
static void l2_fill(struct model *m, uint32_t addr)
{
	uint32_t line = addr & ~15u, val[4];

	dram_read_line(m, dram_decode(m, line), val);
	for (uint32_t dword = 0; dword < 4; dword++)
		l2_data_write(m, hb4_l2_byte(m, line + 4 * dword), val[dword]);
	if (m->l2.fake_tags || !m->l2_size)
		return;
	uint8_t *tag = &m->l2_tag[l2_tag_index(m, addr)];
	uint8_t keep = m->l2_mask != 0xff && !(m->hb[0x50] & 0x40) ? *tag & 0x80 : 0;
	*tag = l2_tag_of(m, addr) | keep;
}

/* The fork's hb4_ram_* mappings: 0-640K, and 1M up to 256M (flat: up to the RAM top); the rest floats. */
static int served(const struct model *m, uint32_t addr)
{
	return addr < 0xa0000 || (addr >= 0x100000 && addr < (m->flat ? m->top : 0x10000000u));
}

static int in_car(const struct model *m, uint32_t addr)
{
	return addr - m->car_base < m->car_size;
}

uint32_t model_read32(struct model *m, uint32_t addr)
{
	uint32_t val;

	if (addr & 3)
		fatal("unaligned read at", addr);
	if (in_car(m, addr))
		return m->car[(addr - m->car_base) >> 2];
	if (!served(m, addr))
		val = 0xffffffff;
	else if (hb4_l2_forced(m))
		val = l2_data_read(m, hb4_l2_byte(m, addr));
	else if (!l2_cached(m, addr))
		val = dram_read(m, dram_decode(m, addr));
	else if (l2_tag_read(m, addr) == l2_tag_of(m, addr))
		val = l2_data_read(m, hb4_l2_byte(m, addr));
	else {
		val = dram_read(m, dram_decode(m, addr));
		if (m->l2_wb)
			l2_write_back(m, addr);
		l2_fill(m, addr);
	}
	m->last_bus = val;
	return val;
}

static int l2_visit(const struct model *m, uint32_t line)
{
	return !in_car(m, line) && l2_cached(m, line);
}

/* Final state of one L2 line slot after the reads of a run: per read, a line visit misses (and fills)
   when the line's tag differs from the tag RAM's, so the state is the fill of the run's last missing
   visit; `line` is the slot's last line in the run, `first` the run's first line. */
static void l2_settle_slot(struct model *m, uint32_t line, uint32_t first)
{
	uint32_t eff = m->l2_eff;

	while (!l2_visit(m, line)) {
		if (line - first < eff)
			return;
		line -= eff;
	}
	for (;;) {
		uint32_t prev = line;
		int found = 0;
		while (prev - first >= eff) {
			prev -= eff;
			if ((found = l2_visit(m, prev)))
				break;
		}
		/* fake tags never change: the tag compared is always the floating one */
		uint8_t before = (found && !m->l2.fake_tags) ? l2_tag_of(m, prev) : l2_tag_read(m, line);
		if (before != l2_tag_of(m, line)) {
			l2_fill(m, line);
			return;
		}
		if (!found)
			return;
		line = prev;
	}
}

/* The reads of every dword in [start, end), in order. Only the floating-bus data SRAM reads last_bus;
   write-back (dirty lines) takes the one-by-one path. */
void model_read_run(struct model *m, uint32_t start, uint32_t end)
{
	if (m->exact_runs || m->l2.data == L2_DATA_FLOAT_BUS || m->l2_wb) {
		for (uint32_t addr = start; addr != end; addr += 4)
			model_read32(m, addr);
		return;
	}
	if (!m->l2_on || !m->l2_eff || start == end)
		return;
	uint32_t first = start & ~15u, last = (end - 4) & ~15u;
	uint32_t line = (last - first >= m->l2_eff) ? last - m->l2_eff + 16 : first;
	for (;; line += 16) {
		l2_settle_slot(m, line, first);
		if (line == last)
			break;
	}
}

void model_write32(struct model *m, uint32_t addr, uint32_t val)
{
	if (addr & 3)
		fatal("unaligned write at", addr);
	if (in_car(m, addr)) {
		m->car[(addr - m->car_base) >> 2] = val;
		return;
	}
	m->last_bus = val;
	if (!served(m, addr))
		return;
	if (hb4_l2_forced(m)) {
		l2_data_write(m, hb4_l2_byte(m, addr), val);
		return;
	}
	if (l2_cached(m, addr) && l2_tag_read(m, addr) == l2_tag_of(m, addr)) {
		l2_data_write(m, hb4_l2_byte(m, addr), val);
		if (m->l2_wb) {
			l2_set_dirty(m, addr);
			return;
		}
	}
	dram_write(m, dram_decode(m, addr), val);
}

uint32_t model_dram_peek(struct model *m, uint32_t addr)
{
	return dram_read(m, dram_decode(m, addr));
}

uint32_t model_l2_lines(const struct model *m)
{
	return m->l2_on ? m->l2_prog >> 4 : 0;
}

/* What a read of the address that programmed line `line` hits now returns, against DRAM: the first
   differing address, else NO_HIT. A hit changes nothing, so this equals reading it. */
uint32_t model_l2_false_hit(struct model *m, uint32_t line)
{
	uint64_t addr = (uint64_t)l2_tag_read(m, line << 4) * m->l2_prog + (line << 4);

	if (addr >= m->l2_end || !l2_cached(m, (uint32_t)addr))
		return NO_HIT;
	uint32_t val[4];
	dram_read_line(m, dram_decode(m, (uint32_t)addr), val);
	for (uint32_t dword = 0; dword < 4; dword++) {
		uint32_t a = (uint32_t)addr + 4 * dword;
		if (l2_data_read(m, hb4_l2_byte(m, a)) != val[dword])
			return a;
	}
	return NO_HIT;
}

uint32_t model_digest(const struct model *m)
{
	uint32_t hash = 2166136261u;

	for (uint32_t i = 0; i < m->l2_size / 4; i++)
		hash = (hash ^ m->l2_data[i]) * 16777619u;
	for (uint32_t i = 0; i < m->l2_size / 16; i++)
		hash = (hash ^ m->l2_tag[i]) * 16777619u;
	for (int reg = 0x50; reg < 0x63; reg++)
		hash = (hash ^ m->hb[reg]) * 16777619u;
	return hash;
}

void model_hb_write(struct model *m, int reg, uint8_t val)
{
	switch (reg) {
	case 0x04: case 0x05: case 0x0c: case 0x0d:
	case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59:
	case 0x5c: case 0x5d: case 0x5e: case 0x5f: case 0x60:
		m->hb[reg] = val;
		break;
	case 0x07:
		m->hb[reg] &= ~(val & 0xf9);
		break;
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x5a: case 0x61:
		m->hb[reg] = val;
		hb4_ram_update(m);
		break;
	case 0x62:
		m->hb[reg] = val & 0x03;
		break;
	}
}

struct model *model_new(void)
{
	struct model *m = calloc(1, sizeof(*m));

	if (!m || !(m->l2_data = malloc(L2_MAX)) || !(m->l2_tag = malloc(L2_MAX / 16)) ||
	    !(m->dram_pool = malloc(DRAM_POOL * 4096)))
		fatal("out of memory, bytes", sizeof(*m) + L2_MAX + L2_MAX / 16 + DRAM_POOL * 4096);
	for (int slot = 0; slot < 4; slot++)
		if (!(m->random_data[slot] = malloc(L2_MAX)) || !(m->random_tag[slot] = malloc(L2_MAX / 16)))
			fatal("out of memory, bytes", 4 * (L2_MAX + L2_MAX / 16));
	return m;
}

void model_reset(struct model *m, const int types[4], const struct l2_hw *l2)
{
	static const uint8_t reset[0x63] = {
		[0x00] = 0x60, [0x01] = 0x10, [0x02] = 0x81, [0x03] = 0x88, [0x07] = 0x02, [0x08] = 0x04,
		[0x0b] = 0x06, [0x52] = 0x01, [0x55] = 0x40, [0x56] = 0xff, [0x57] = 0x0f, [0x58] = 0xff,
		[0x59] = 0x0f, [0x5b] = 0x2c, [0x5d] = 0x0f, [0x5f] = 0xff,
	};

	m->capacity = 0;
	for (int bank = 0; bank < 4; bank++) {
		m->simm[bank] = simm_types[types[bank]];
		m->simm[bank].offset = m->capacity;
		if (types[bank])
			m->capacity += m->simm[bank].sides << (m->simm[bank].cols + m->simm[bank].rows + 2);
	}
	m->l2 = *l2;
	m->l2_size = l2->kb << 10;
	if (m->l2_size > L2_MAX)
		fatal("L2 larger than modelled, bytes", m->l2_size);
	if (l2->seed) {
		unsigned slot = l2->seed % 4;
		if (m->random_seed[slot] != l2->seed) {
			uint64_t x = l2->seed * 0x9e3779b97f4a7c15ULL;
			for (uint32_t i = 0; i < L2_MAX / 4; i++) {
				x ^= x << 13, x ^= x >> 7, x ^= x << 17;
				m->random_data[slot][i] = (uint32_t)x;
				if (i < L2_MAX / 16)
					m->random_tag[slot][i] = (uint8_t)(x >> 32);
			}
			m->random_seed[slot] = l2->seed;
		}
		memcpy(m->l2_data, m->random_data[slot], m->l2_size);
		memcpy(m->l2_tag, m->random_tag[slot], m->l2_size / 16);
	} else {
		memset(m->l2_data, 0, m->l2_size);
		memset(m->l2_tag, 0, m->l2_size / 16);
	}
	m->mem_size = 128 << 20;	/* test/lib.sh: mem_size = 131072 */
	m->car_base = m->car_size = 0;
	m->last_bus = 0;
	m->dram_used = 0;
	memset(m->hb, 0, sizeof(m->hb));
	memcpy(m->hb, reset, sizeof(reset));
	memset(m->isa, 0, sizeof(m->isa));
	m->gen++;
	hb4_ram_update(m);
}
