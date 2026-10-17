/* SeaBIOS's own printf engine (src/output.c) for the cases: glibc's printf knows formats that SeaBIOS prints
   wrong, so a page could pass here and be garbage on the board. vsnprintf, snprintf and znprintf are output.c's
   (devfuzz.sh renames them sb_*); it makes its printf, panic, dprintf and warn functions local, the seams' (seams.c)
   stay. */
#include "output.c"

void entry_10(void) { }                 /* only the address, for screenc (output.c) never called */
