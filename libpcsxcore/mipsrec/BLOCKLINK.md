# mipsrec block linking (EROSQ, 2026-10-17)

Status: **phase 1 implemented; offline differential PASS (research hlink 4000/4000, mutations caught); device checks in progress** on branch `blocklink` (off `dt2fix` 720f492). The kill-switch is
`EROSQ_BLOCKLINK=0`. Default (env unset) = mode 3, the shared trampoline.

## Why
The PC sampler on the shipped df17906f puts the dispatch loop (`loop3212` = the 16-instruction body
of recExecute_direct_return_lut, plus `execute_block3212` = its `jr`) at 5-9% of all CPU:
Ape 8.7+2.2, MGS 6.5, Crash 5.8+1.9, Spyro 5.1+1.6, GT2 2.3. Method: periodic PC sampling on the
device. That is a separate question from the old "the loop has no interlock stalls" verdict:
every block return pays a `j loop`, the LUT walk (srl/sll/addu/lw/andi/addu/lw) and the `jr`.

## How a block exits today (direct-return mode, `block_ret_addr != 0`)
`emitBlockReturnPC(target)` (v0 = target PC, cached) ... `rec_recompile_end_part2()` =
`j loop ; li v1, cycles` (or `j fastpath_loop` when target == the block's own start).

The loop then:
- adds v1 to psxRegs.cycle;
- does the wrap-safe horizon check `(s32)(cycle - io_cycle_counter) >= 0` → `psxBranchTest()`, then
  checks `psxRegs.stop`, then reloads pc;
- stores psxRegs.pc = v0;
- looks up `*PC_REC(v0)`; 0 → recRecompile;
- saves the block address at 16($sp) (the fastpath slot); `jr`.

Blocks expect on entry: v0 = their PC, psxRegs.pc = their PC, psxRegs.cycle already charged,
16($sp) = their own code address (used if they take the fastpath return).

## Phase 1: slot-direct exits (no code patching)
At exits whose target PC is a compile-time constant, emit the loop's work inline, specialised for
that PC:

```
  lw    t3, cycle(fp)
  lw    t1, io_cycle_counter(fp)
  lui   t2, %hi(PC_REC(target))      # slot address is a compile-time constant
  lw    t0, %lo(PC_REC(target))(t2)  # current code pointer for target (0 = invalid)
  addiu t3, t3, cycles
  subu  t1, t3, t1                   # wrap-safe horizon, identical to the loop
  bgez  t1, slow
  nop
  beqz  t0, slow
  sw    v0, pc(fp)                   # <BD> harmless on both paths (loop stores pc too)
  sw    t3, cycle(fp)
  jr    t0
  sw    t0, 16(sp)                   # <BD> fastpath slot, as the loop does
slow:
  j     loop                         # unchanged old exit: the loop re-adds cycles from v1
  li    v1, cycles
```

**Invariants, and why each design-review item is covered:**
- **SMC invalidation:** no link state exists. The target's recRAM/recROM slot is re-read on every
  pass, and both the JIT's inline SMC store and recClear() zero exactly that slot. So a stale
  target falls to `slow` → loop → recRecompile, exactly as today. No back-pointer lists, no unlink.
- **Code-cache reset / flush:** recReset clears recRAM/recROM slots (which is what the loop
  already relies on). Nothing else holds code pointers.
- **I-cache coherency:** no emitted word is ever modified after emission, so there's nothing to flush
  beyond what recRecompile already does.
- **Events / interrupts:** the horizon check is the loop's own wrap-safe `subu`+`bgez` (July
  livelock fix, u32 cycle-counter wrap), done before every linked entry. When it
  fires, cycles are NOT stored and control goes to the loop with v1 = cycles, so accounting is
  identical.
- **psxRegs.stop (menu/save/quit):** stop is only consulted after psxBranchTest() in the loop
  today. The linked path takes `slow` whenever the horizon fires, so stop latency is unchanged.
- **faultmem:** the inline sequence touches only psxRegs (fp), the recRAM/recROM slot (always
  mapped, outside the guest reservation: recRAM is malloc'd while Part B is off; Part B's REC_RAM_VADDR is 0x21000000 (static_assert: above RESV_HI 0x20010000)) and 16($sp).
  None of these fault, so fm_sigsegv is never entered with a PC inside the sequence.
  fm_tbl block ranges include the exit code as part of the block (it's emitted before block end).
- **Security:** the slot address is a constant derived from a compile-time PC through psxRecLUT
  (host pointers set at init). The guest controls no part of it. Targets whose LUT entry is
  null are not linked.
- **Fastpath:** exits that target the block's own start keep `j fastpath_loop` (already the cheapest).
- **Cycles > 0x7fff:** not linked (ADDIU range).
- **Indirect exits** (JR/JALR/SYSCALL/RFE, the dt==2 workaround path) are not linked. Only call
  sites that pass the constant target explicitly (`rec_recompile_end_part2_link(fp, target)`) are.

Cost per linked exit: +13 words of code. In exchange each pass saves the `j loop`, the LUT walk
(6 insns, incl. a LUT load) and the loop's branch overhead.

Modes: `EROSQ_BLOCKLINK=1` links. `=2` also emits a taken-counter increment
(`mipsrec_link_taken`, for the differential harness only).

## Phase 2 (only if phase 1 shows the remaining loop share is worth it): patched direct links
Patch `j target_code` into the exit after the first successful lookup, with per-target back-pointer
lists, unlinking on recClear/SMC (the SMC store would then need a C call or a "has inbound links"
bit) and an I$ flush (`cacheflush`/synci) of patched words. This saves the slot load + beqz over
phase 1, at a much higher correctness cost. Not started.

## Verification plan (all before any speed claim)
1. Offline: lsu_bounds 0/5520; hexec 300/300 (faultmem 0/1); research gen_link.py differential,
   BLOCKLINK=0 vs 2: SMC, cross-block chains, horizon mid-chain, stop flag, wrap-adjacent budgets.
   It fails if mipsrec_link_taken == 0.
2. Device: gate (MGS + GT2 4/4, PaRappa XA), all 16 SD games boot, menu open/resume ×3,
   savestate save/load, ≥30 min soak of Crash + GT2 attract loops, TR4 path.
3. A/B in one binary (BLOCKLINK 0 vs 1) + PMON + sampler dispatcher share before/after.

## Results (device, 1200 MHz, same binary, median r1/r2)
- Mode 1 (inline, +13 words per exit) vs off: Ape +2.3/+1.9, MGS +1.4/+1.2, Crash +0.6/+2.3,
  Spyro +0.7/+3.1, TR +2.0/+0.4, **GT2 -1.7/-2.5** (ITLB +20%).
- Mode 3 (trampoline, +2 words per exit) vs off: GT2 -0.4/+1.7, Ape +4.6/+1.3, MGS +0.7/+0.9,
  Crash +2.6/+0.7, Spyro +0.2/+1.8, TR +1.2/+0.1. PMON cycles/frame: Crash -1.3%, GT2 -0.5%.
  GT2 mode 3 vs mode 1: +2.8/+0.4.
- Linked exits taken: ~1.87M/s on Crash (~31k per frame).
- Correctness: 16/16 SD games boot; menu x3; gate PASS (MGS/GT2 hashes 4/4, PaRappa XA);
  60-min soak (mode 1 code) PASS; hlink (research) 4000/4000 + controls; lsu_bounds 0/5520.
