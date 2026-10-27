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
set -e
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
cd "$(dirname "$0")"

# PROF_DIR=<dir> uses (or, with PGO_GEN=1, records into) another profile dir,
# e.g. pgo-profile-retrain/ -- the checked-in pgo-profile/ stays untouched.
# PGO_GEN=1: instrumented build (pgo-profile/README.md step 1).
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
PROF="$(pwd)/${PROF_DIR:-pgo-profile}"
if [ -z "$PGO_GEN" ]; then
	[ -d "$PROF" ] && [ "$(ls "$PROF"/*.gcda 2>/dev/null | wc -l)" -gt 0 ] || {
		echo "no profile in $PROF -- see pgo-profile/README.md to regenerate"; exit 1; }
	stage_prof "$PROF"
fi

# USE_ASYNC_CDROM=1 here too: it enables USE_RTHREADS, whose objects are only in
# OBJS (and so only cleaned) with it set -- without it shipped builds linked
# never-rebuilt July objects (frontend/pcsxr-threads.o, features_cpu.o)
make clean USE_ASYNC_CDROM=1 >/dev/null 2>&1
[ -n "$XB_CC1_DIR" ] && [ -x "$XB_CC1_DIR/cc1" ] || { echo "set XB_CC1_DIR to a gcc build/gcc dir carrying erosq-gcc/0001-*.patch"; exit 1; }
if [ -n "$PGO_GEN" ]; then
	# value profiling off -> avoids a MIPS TLS reloc error (pgo-profile/README.md)
	make -j"$(nproc)" USE_ASYNC_CDROM=1 \
		CFLAGS_OPT="-Ofast -B$XB_CC1_DIR/ -fprofile-generate=$PROF -fno-profile-values -DEROSQ_GCOV_DUMP --param max-unroll-times=2"
	echo "=== PGO INSTRUMENTED BUILD -> $PROF ==="
else
make -j"$(nproc)" USE_ASYNC_CDROM=1 \
	CFLAGS_OPT="-Ofast -B$XB_CC1_DIR/ -fprofile-use=$STAGED_PROF -fprofile-partial-training -fprofile-correction -Wno-error=coverage-mismatch -Wno-coverage-mismatch --param max-unroll-times=2"
echo "=== PGO BUILD RESULT ==="
fi
ls -la pcsx && file pcsx
