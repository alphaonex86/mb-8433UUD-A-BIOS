/* romstage.c compiled unchanged, for its chipset defaults table; its entry point is not run. */
#include <stdio.h>
#include <stdlib.h>

#include "romstage.c"

static void not_modelled(const char *what)
{
	fprintf(stderr, "dramfuzz: %s called, not modelled\n", what);
	exit(2);
}

void um8886_early_init(void)
{
	not_modelled("um8886_early_init");
}

void cbmem_initialize_empty(void)
{
	not_modelled("cbmem_initialize_empty");
}

void romstage_chipset_defaults(void)
{
	umc_registers_init(HOST_BRIDGE, hb_init_table, ARRAY_SIZE(hb_init_table));
}
