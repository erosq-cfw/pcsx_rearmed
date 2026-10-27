#!/bin/sh
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
# PGO build of the EROSQ pcsx frontend. Applies the checked-in profile in
# pgo-profile/ (see pgo-profile/README.md for how it was collected). Measured
# +3.6% on the Crash slot-0 scene, ~+1% on other heavy games, no regressions,
# bit-exact vs canonical (PGO = block layout/inlining only, same -Ofast FP).
#
# The profile was collected under the INTERPRETER (the mipsrec dynarec SIGBUSes
# under gcov instrumentation), then applied to the dynarec build. -fprofile-
# partial-training keeps the unprofiled dynarec TUs from being pessimized.
# NOTE (2026-10-16): the shipped binary is built with build-erosq-pgo-xb.sh
# (same profile + the X1000-latency gcc scheduler patch in erosq-gcc/, cc1 at
# $EROSQ_ROOT/gcc-xburst-cc1). Building ship binaries with THIS script
# silently drops that scheduler (measured ~3-4%) -- same trap as the PGO one.
set -e
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
cd "$(dirname "$0")"

# pgo-profile/ stores each .gcda under its repo-relative object path ('/' -> '#').
# gcc looks profiles up by the mangled ABSOLUTE object path, so stage a copy
# named for this checkout. (After recording with PGO_GEN=1, strip the
# "$(pwd | tr / '#')#" prefix before checking new profiles in.)
stage_prof() {
	STAGED_PROF=$(mktemp -d "${TMPDIR:-/tmp}/pcsx-pgo.XXXXXX")
	trap 'rm -rf "$STAGED_PROF"' EXIT
	M=$(pwd | tr / '#')
	for f in "$1"/*.gcda; do
		b=$(basename "$f")
		case $b in \#*) cp "$f" "$STAGED_PROF/$b" ;; *) cp "$f" "$STAGED_PROF/$M#$b" ;; esac
	done
}
PROF="$(pwd)/pgo-profile"
[ -d "$PROF" ] && [ "$(ls "$PROF"/*.gcda 2>/dev/null | wc -l)" -gt 0 ] || {
	echo "no profile in $PROF -- see pgo-profile/README.md to regenerate"; exit 1; }
stage_prof "$PROF"

# USE_ASYNC_CDROM=1 here too: it enables USE_RTHREADS, whose objects are only in
# OBJS (and so only cleaned) with it set -- without it shipped builds linked
# never-rebuilt July objects (frontend/pcsxr-threads.o, features_cpu.o)
make clean USE_ASYNC_CDROM=1 >/dev/null 2>&1
make -j"$(nproc)" USE_ASYNC_CDROM=1 \
	CFLAGS_OPT="-Ofast -fprofile-use=$STAGED_PROF -fprofile-partial-training -fprofile-correction -Wno-error=coverage-mismatch -Wno-coverage-mismatch"
echo "=== PGO BUILD RESULT ==="
ls -la pcsx && file pcsx
