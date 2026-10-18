# Offline mipsrec harnesses (2026-10-17). run.sh: emitted-byte stats on a guest corpus
# (EROSQ_EMITSTAT). hexec.sh N [faultmem]: differential execution classic JIT vs
# faultmem JIT under qemu-user on random load/store programs (gen_lsu.py).
# S= below is the scratch dir holding the corpus and build products; adjust.
#!/bin/bash
# hexec.sh N_SEEDS [FAULTMEM(1/0)] : generate + assemble + run N random tests
S=${S:-/tmp/es}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
export PATH=buildroot-2026.08/output/host/bin:$PATH
cd $W
OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $S/hexec.c -o $S/hexec.o || exit 1
mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/hexec $S/hexec.o $S/stubs.o $OBJS -lz -lm -lpthread 2>/dev/null || exit 1
cd $S; mkdir -p hx; pass=0; fail=0
for i in $(seq 1 $1); do
  python3 gen_lsu.py $i ${NOPS:-60} ${LOOPS:-3} > hx/t.S
  mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c hx/t.S -o hx/t.o 2>hx/as.err || { echo "asm fail $i"; continue; }
  mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o hx/t.elf hx/t.o && mipsel-buildroot-linux-musl-objcopy -O binary -j .text hx/t.elf hx/t.bin
  out=$(EROSQ_FAULTMEM=${2:-1} timeout 60 qemu-mipsel-static -L buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot ./hexec hx/t.bin $i 2>&1 | grep -v "^mipsrec\|^WARNING")
  if echo "$out" | grep -q "^ok"; then pass=$((pass+1)); else fail=$((fail+1)); [ $fail -le 3 ] && { echo "--- seed $i"; echo "$out" | tail -6; cp hx/t.S hx/fail-$i.S; }; fi
  [ -n "$VERBOSE" ] && echo "$out" | tail -1
done
echo "pass=$pass fail=$fail (faultmem=${2:-1})"
