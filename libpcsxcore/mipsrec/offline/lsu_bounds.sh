#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# Build + run the containment test against the current pcsx object files.
# usage: lsu_bounds.sh [SCRATCH_DIR]   (needs qemu-mipsel-static)
set -e
S=${1:-${TMPDIR:-/tmp}/erosq-es}
W=$(cd "$(dirname "$0")/../../.." && pwd)
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
cd $W
OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c libpcsxcore/mipsrec/offline/lsu_bounds.c -o $S/lsu_bounds.o
mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c libpcsxcore/mipsrec/offline/stubs.c -o $S/stubs.o
mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/lsu_bounds $S/lsu_bounds.o $S/stubs.o $OBJS -lz -lm -lpthread 2>/dev/null
cd $S && timeout 3000 qemu-mipsel-static -L $EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot ./lsu_bounds > lsu_bounds.out 2>&1 || true
grep -v "^mipsrec\|^WARNING\|^cases\|^note" lsu_bounds.out | sed -E 's/^([a-zA-Z]+) +([a-z0-9]+) .*: ([a-zA-Z-]+).*/\1 \3/' | sort | uniq -c
echo "VIOLATIONS: $(grep -c VIOLATION lsu_bounds.out)  cases: $(grep -o 'cases=[0-9]*' lsu_bounds.out)"
