#!/bin/sh
# PGO build of the EROSQ pcsx frontend. Applies the checked-in profile in
# pgo-profile/ (see pgo-profile/README.md for how it was collected). Measured
# +3.6% on the Crash slot-0 scene, ~+1% on other heavy games, no regressions,
# bit-exact vs canonical (PGO = block layout/inlining only, same -Ofast FP).
#
# The profile was collected under the INTERPRETER (the mipsrec dynarec SIGBUSes
# under gcov instrumentation), then applied to the dynarec build. -fprofile-
# partial-training keeps the unprofiled dynarec TUs from being pessimized.
set -e
export PATH=buildroot-2026.08/output/host/bin:$PATH
cd "$(dirname "$0")"

PROF="$(pwd)/pgo-profile"
[ -d "$PROF" ] && [ "$(ls "$PROF"/*.gcda 2>/dev/null | wc -l)" -gt 0 ] || {
	echo "no profile in $PROF -- see pgo-profile/README.md to regenerate"; exit 1; }

# USE_ASYNC_CDROM=1 here too: it enables USE_RTHREADS, whose objects are only in
# OBJS (and so only cleaned) with it set -- without it shipped builds linked
# never-rebuilt July objects (frontend/pcsxr-threads.o, features_cpu.o)
make clean USE_ASYNC_CDROM=1 >/dev/null 2>&1
[ -n "$XB_CC1_DIR" ] && [ -x "$XB_CC1_DIR/cc1" ] || { echo "set XB_CC1_DIR to a gcc build/gcc dir carrying erosq-gcc/0001-*.patch"; exit 1; }
make -j"$(nproc)" USE_ASYNC_CDROM=1 \
	CFLAGS_OPT="-Ofast -B$XB_CC1_DIR/ -fprofile-use=$PROF -fprofile-partial-training -fprofile-correction -Wno-error=coverage-mismatch -Wno-coverage-mismatch --param max-unroll-times=2"
echo "=== PGO BUILD RESULT ==="
ls -la pcsx && file pcsx
