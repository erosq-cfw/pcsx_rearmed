#!/bin/sh
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# relink + run the emit-size harness against the current worktree objects
set -e
S=${TMPDIR:-/tmp}/erosq-es
cd "$(dirname "$0")/../../.."
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/harness $S/harness.o $S/stubs.o $OBJS -lz -lm -lpthread 2>/dev/null
cd $S && qemu-mipsel-static -L $EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot ./harness ${1:-corpus.bin} ${2:-funcs.txt} 2>&1 | grep EMITSTAT | tail -17
