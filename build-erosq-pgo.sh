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

make clean >/dev/null 2>&1
make -j"$(nproc)" USE_ASYNC_CDROM=1 \
	CFLAGS_OPT="-Ofast -fprofile-use=$PROF -fprofile-partial-training -fprofile-correction -Wno-error=coverage-mismatch -Wno-coverage-mismatch"
echo "=== PGO BUILD RESULT ==="
ls -la pcsx && file pcsx
