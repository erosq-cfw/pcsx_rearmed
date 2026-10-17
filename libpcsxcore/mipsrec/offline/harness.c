#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"
int mipsrec_rec_init(void); void mipsrec_rec_reset(void);
unsigned mipsrec_rec_compile_at(unsigned pc); void mipsrec_emitstat_dump(void); void *mipsrec_rec_cur(void); void mipsrec_rec_harness_direct_returns(unsigned long, unsigned long); void mipsrec_rec_harness_faultmem(void);
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "harness code.bin funcs.txt\n"); return 2; }
	setenv("EROSQ_EMITSTAT", "1", 1);
	if (psxMemInit()) { fprintf(stderr, "psxMemInit failed\n"); return 1; }
	if (mipsrec_rec_init()) { fprintf(stderr, "recInit failed\n"); return 1; }
	mipsrec_rec_reset();
	if (getenv("FM")) mipsrec_rec_harness_faultmem();
	mipsrec_rec_harness_direct_returns((unsigned long)mipsrec_rec_cur() + 0x100000, (unsigned long)mipsrec_rec_cur() + 0x100040);
	FILE *f = fopen(argv[1], "rb");
	size_t n = fread(psxRegs.ptrs.psxM + 0x10000, 1, 0x1f0000, f); fclose(f);
	fprintf(stderr, "loaded %zu bytes at 0x80010000\n", n);
	FILE *fl = fopen(argv[2], "r");
	unsigned a, e, nf = 0, nb = 0;
	const char *dump = getenv("DUMP_AT");   /* hex guest addr of a function to dump */
	unsigned dumpa = dump ? strtoul(dump, 0, 16) : 0;
	while (fscanf(fl, "%x %x", &a, &e) == 2) {
		unsigned p = a;
		unsigned char *h0 = mipsrec_rec_cur();
		nf++;
		while (p < e) { p = mipsrec_rec_compile_at(p); nb++; }
		if (a == dumpa) {
			unsigned char *h1 = mipsrec_rec_cur();
			FILE *o = fopen("dump.bin", "wb"); fwrite(h0, 1, h1 - h0, o); fclose(o);
			fprintf(stderr, "dumped %ld host bytes of guest fn %08x..%08x (host base %p)\n", (long)(h1 - h0), a, e, h0);
		}
	}
	fprintf(stderr, "functions %u, blocks %u\n", nf, nb);
	mipsrec_emitstat_dump();
	return 0;
}
