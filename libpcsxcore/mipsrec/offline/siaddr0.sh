#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# siaddr0.sh BIN EXE : the device kernel's address-error SIGBUS has si_addr = 0,
# qemu-user fills it in. Run EXE (an exception-heavy test such as Amidog
# psxtest_cpu) headless twice, once with EROSQ_TEST_SIADDR0=1 (ua_sigbus zeroes
# si_addr, as on the device), and require identical guest state hashes (to a
# 500M-cycle budget: psxtest_cpu flips only ~24 frames).
# BIN = a headless build (offline/hx_build.sh). HOMET = a home dir with
# .pcsx/bios/scph1001.bin. Mutant: build jitsched with -DSIADDR_MUTANT (the
# pre-ff97856 si_addr test): must FAIL.
BIN=${1:?BIN}; EXE=${2:?EXE}
HOMET=${HOMET:-${TMPDIR:-/tmp}/erosq-idlesize-home-gt2}
Q=${Q:-qemu-mipsel}
SYSROOT=$EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot
T=$(mktemp -d); trap 'rm -rf $T' EXIT
run() {  # tag env...
	local t=$1; shift; cp -a $HOMET $T/h-$t
	env "$@" HOME=$T/h-$t PCSX_NOAUDIO=1 EROSQ_PROF=1 HX_NODUMP=1 HX_FRAMES=${FRAMES:-900} HX_CYCLES=${CYCLES:-500000000} HX_HASH_EVERY=10 \
	    HX_OUT=$T/o-$t timeout ${TMO:-600} $Q -L $SYSROOT $BIN -psxout "$EXE" > $T/$t.log 2>&1
	grep -a '^\[hx\] \(hash\|end\)' $T/$t.log > $T/$t.h
}
run ref &  run zero EROSQ_TEST_SIADDR0=1 & wait
n=$(wc -l < $T/ref.h)
echo "ref:  $(grep -a 'unaligned: exact' $T/ref.log | cut -c1-60)"
echo "zero: $(grep -a 'unaligned: exact' $T/zero.log | cut -c1-60)"
if [ "$n" -gt 0 ] && cmp -s $T/ref.h $T/zero.h; then echo "siaddr0: PASS ($n hashes identical)"; exit 0; fi
echo "siaddr0: FAIL (ref $n hashes, zero $(wc -l < $T/zero.h); first diff:)"; diff $T/ref.h $T/zero.h | head -4; exit 1
