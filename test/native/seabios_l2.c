/* SeaBIOS setup.c compiled unchanged for setup_l2_cache, the External Cache enable at POST: the link keeps
   only what that needs (--gc-sections), and setup.c's asm l2_fill is weakened so that the model's runs. */
#include "setup.c"

void seabios_l2_settings(int l2_cache, int defaulted)
{
    Setting[NB_L2_CACHE] = l2_cache;
    SetupDefaulted = defaulted;
}
