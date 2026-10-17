#!/bin/sh
# relink + run the emit-size harness against the current worktree objects
set -e
S=/tmp/es
cd pcsx_rearmed-ideas
export PATH=buildroot-2026.08/output/host/bin:$PATH
OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/harness $S/harness.o $S/stubs.o $OBJS -lz -lm -lpthread 2>/dev/null
cd $S && qemu-mipsel-static -L buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot ./harness ${1:-corpus.bin} ${2:-funcs.txt} 2>&1 | grep EMITSTAT | tail -17
