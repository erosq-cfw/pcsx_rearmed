/* Host test for cdriso_seek_set(): sector reads through a 16 KB-buffered FILE
 * (as cdriso sets it up) must return exactly the file's bytes for sequential
 * runs, random seeks, and when other code moved the position in between.
 *   cc -O2 -o test_cdseek libpcsxcore/test_cdseek.c && ./test_cdseek
 * Against musl (and to count syscalls with -strace):
 *   mipsel-buildroot-linux-musl-gcc -O2 -static -o t libpcsxcore/test_cdseek.c
 *   qemu-mipsel-static [-strace] ./t */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "cdriso_seek.h"
#ifdef TEST_PLAIN_FSEEKO /* baseline for syscall comparison */
#define cdriso_seek_set(f, p) fseeko(f, p, SEEK_SET)
#endif

#define SECT 2352
#define NSECT 600

int main(void)
{
	static char buf[16 * 1024];
	static unsigned char img[NSECT * SECT];
	unsigned char got[SECT];
	char path[] = "/tmp/test_cdseekXXXXXX";
	int fd = mkstemp(path), i, fails = 0, seq = 0;
	srand(1);
	for (i = 0; i < (int)sizeof(img); i++) img[i] = rand();
	if (fd < 0 || write(fd, img, sizeof(img)) != (ssize_t)sizeof(img)) { perror("tmp"); return 1; }
	FILE *f = fdopen(dup(fd), "rb");
	setvbuf(f, buf, _IOFBF, sizeof(buf));

	int sector = 0;
	for (i = 0; i < 20000; i++) {
		int r = rand() % 100;
		if (r < 75 && sector + 1 < NSECT) sector++, seq++;      /* sequential (the common case) */
		else sector = rand() % NSECT;                          /* random seek */
		if (r >= 95) {                                         /* someone else moved the position */
			fseeko(f, rand() % (NSECT * SECT - 24), SEEK_SET);
			if (fread(got, 1, 24, f) != 24) return 2;
		}
		off_t pos = (off_t)sector * SECT;
		if (cdriso_seek_set(f, pos) || fread(got, 1, SECT, f) != SECT ||
		    memcmp(got, img + pos, SECT) || ftello(f) != pos + SECT) {
			if (fails++ < 5) printf("FAIL op %d sector %d\n", i, sector);
		}
	}
	fclose(f);
	unlink(path);
	if (fails) { printf("FAIL: %d of 20000 reads wrong\n", fails); return 1; }
	printf("PASS: 20000 sector reads (%d sequential) byte-exact\n", seq);
	return 0;
}
