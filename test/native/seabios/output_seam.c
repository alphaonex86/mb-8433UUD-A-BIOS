/* SeaBIOS's own printf engine (src/output.c) for the cases: glibc's printf knows formats that SeaBIOS prints
   wrong, so a page could pass here and be garbage on the board. snprintf and znprintf are output.c's; devfuzz.sh
   makes its printf, panic, dprintf and warn functions local, the seams' (seams.c) stay. */
#include "output.c"

void entry_10(void) { }                 /* only the address, for screenc (output.c) never called */

int sb_vsnprintf(char *str, size_t size, const char *fmt, va_list args)
{
	if (!size)
		return 0;
	struct snprintfinfo sinfo = { { putc_str }, str, str + size };
	bvprintf(&sinfo.info, fmt, args);
	char *end = sinfo.str < sinfo.end ? sinfo.str : sinfo.end - 1;
	*end = '\0';
	return end - str;
}
