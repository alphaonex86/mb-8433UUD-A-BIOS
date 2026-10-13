#ifndef UM8881_MODEL_H
#define UM8881_MODEL_H

#include <stdint.h>

#define SIMM_TYPES 10
#define L2_MAX (1 << 20)
#define NO_HIT 0xffffffffu
#define DRAM_PAGES (512u << 20 >> 12)	/* 4 KiB pages of the largest SIMM set */
#define DRAM_POOL 2048			/* pages written per case */

struct simm {
	uint8_t cols;
	uint8_t rows;
	uint8_t sides;
	uint32_t offset;
};

struct region {
	uint32_t base;
	uint32_t size;
	int banks;
	int sides;
	int bits;
	int first;
	int swap;
	const uint8_t *ma;
	uint32_t hi_mask;		/* address bits 21-25 within a side, as an index of: */
	uint16_t col_hi[32], row_hi[32];	/* their MA lines */
};

enum l2_data { L2_DATA_REAL, L2_DATA_FLOAT_FF, L2_DATA_FLOAT_BUS };

struct l2_hw {
	unsigned kb;		/* 0: no SRAM at all */
	int banks;
	int data;		/* enum l2_data */
	int fake_tags;
	unsigned seed;		/* 0: SRAM zeroed (as the fork), else random power-on contents */
};

struct model {
	struct simm simm[4];
	uint32_t capacity;
	struct l2_hw l2;
	uint32_t l2_size;
	uint32_t mem_size;	/* 86Box machine RAM: flat decode only up to it */
	int flat;
	uint32_t top;
	struct region region[2];	/* of the current 52h/53h/61h */
	uint32_t decode_page;		/* last 4 KiB page decoded, its base cell and row MA0 bit */
	int64_t decode_base;
	int decode_row0;
	int l2_on;			/* normal (not forced) L2 caching */
	int l2_wb;			/* and write-back */
	uint32_t l2_prog;		/* 50h size */
	uint32_t l2_eff;		/* lines that exist: min(installed, 50h size) */
	uint8_t l2_mask;		/* tag bits */
	uint32_t l2_end;		/* cached below */
	uint32_t car_base, car_size;	/* L1 cache-as-RAM window while car_size != 0 */
	uint32_t last_bus;
	int exact_runs;			/* apply queued reads one by one (cross-check of the bulk path) */
	uint8_t hb[256];
	uint8_t isa[256];
	uint32_t gen;
	uint32_t *l2_data;
	uint8_t *l2_tag;
	uint32_t car[0x4000 / 4];
	uint32_t *dram_page[DRAM_PAGES];
	uint32_t dram_gen[DRAM_PAGES];
	uint32_t *dram_pool;
	unsigned dram_used;
	unsigned random_seed[4];	/* random power-on SRAM contents, generated once per seed */
	uint32_t *random_data[4];
	uint8_t *random_tag[4];
};

extern const struct simm simm_types[SIMM_TYPES];

struct model *model_new(void);
void model_reset(struct model *m, const int types[4], const struct l2_hw *l2);
void model_hb_write(struct model *m, int reg, uint8_t val);
uint32_t model_region_size(const uint8_t regs[3], int upper);
int64_t model_dram_decode(const struct model *m, const uint8_t regs[3], uint32_t addr);
int model_region_flat(const struct model *m, const uint8_t regs[3], int upper);
uint32_t model_read32(struct model *m, uint32_t addr);
void model_read_run(struct model *m, uint32_t start, uint32_t end);
void model_write32(struct model *m, uint32_t addr, uint32_t val);
uint32_t model_dram_peek(struct model *m, uint32_t addr);
uint32_t model_l2_lines(const struct model *m);
uint32_t model_l2_false_hit(struct model *m, uint32_t line);
uint32_t model_l2_cacheable_end(const struct model *m);
uint32_t model_digest(const struct model *m);

#endif
