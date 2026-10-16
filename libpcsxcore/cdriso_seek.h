#ifndef CDRISO_SEEK_H
#define CDRISO_SEEK_H

#include <stdio.h>
#include <sys/types.h>

/* musl's fseeko() always throws the read buffer away, and the next fread()
 * then readv()s the request plus a full refill of the stdio buffer. Seeking
 * before every 2352-byte sector therefore cost an lseek + an 18.7 KB copy
 * out of the page cache (16 KB of it discarded by the next seek). Reads are
 * mostly sequential, so only seek when the logical position differs;
 * ftello() is a single lseek(SEEK_CUR) with no copy. */
static inline int cdriso_seek_set(FILE *f, off_t pos)
{
	if (ftello(f) == pos)
		return 0;
	return fseeko(f, pos, SEEK_SET);
}

#endif
