# JITSCHED — post-emission instruction scheduler (REOPENED for shipping 2026-10-19)

`jitsched.h` reorders the host words of each freshly emitted block inside
straight-line regions to hide X1000 load-use / mul-use interlocks. Default ON;
`EROSQ_JITSCHED=0` is the kill-switch.

Status: closed on the cross-binary criterion, then REOPENED on the
in-binary evidence (=0 -> =1 +0.85/+0.38/+0.69% GT2, PMON discarded -6..-7.7%):
the cross-binary deltas were inside the per-file noise floor that psx later
measured (a byte-identical copy of shipped reads -0.64..+1.17% vs shipped).
Ship validation is in-binary (=0 vs =1, ABBA) on a clean-recipe build.

## History of the verdict

The scheduler works but does not beat the shipped binary by more than noise,
so it does not ship.

| measure (GT2 race) | result |
|---|---|
| stallmodel, JIT stall per JIT cycle | 0.132 -> 0.051 |
| stallmodel, total modelled cycles incl. scheduler cost | -0.76% |
| device, same binary, =0 -> =1 | +0.85% (9bf33c0), +0.38% (aac4763), +0.69% (a2eb81f) |
| device PMON "discarded"/cycles (interlocks) | -6 .. -7.7%; "freeze" (memory) flat |
| device, a2eb81f =1 vs shipped e63a4f7a | **+0.08% GT2 (noise)**, Ape +0.95% (Ape swings -0.8..+1.5% run to run) |
| device, a2eb81f =0 vs shipped | -0.61% GT2, with every pre-existing symbol at the same address |

The in-binary gain (+0.4..0.85%) is eaten by a binary-to-binary offset
(-0.4..-0.6%) that is not in the executed code: under qemu, a2eb81f =0 and a
master build byte-identical to shipped (.text/.data/.plt) execute the same GT2
window to within 64k of 11.97G instructions, with the same frames and RAM. The
ELF differs only by one extra read-only/exec page (the appended `.jitsched`
section) and `.bss` +40 B at its end (same page). Untested hypothesis: per-file
physical page placement in the page cache (physically indexed L2) gives each
binary file a fixed bias for a session. Control: an arm that runs a
byte-identical COPY of the shipped binary, and/or drop caches before each run.

## Design (what a reopen starts from)

- Hook: once per emitted block, after the emit loop (in place, same size):
  fm_tbl (block end only), PC_REC (block start) and the already-resolved
  forward branch offsets are unaffected. Nothing stores mid-block host
  addresses; nothing is patched after emission.
- Regions: maximal runs between barriers. Barriers, pinned at their address
  together with the following word: every branch/jump/JAL/JR, COP0/COP2/MXU,
  trapping ADD/ADDI/SUB, syscall/break/sync, writes to $sp/$fp, any unknown
  opcode. Every in-block branch target starts a region. Regions <= 32 words.
- Dependencies: RAW/WAR/WAW on GPRs and HI/LO (one pseudo-register). Memory:
  guest / fault-capable accesses keep order with every access; only $fp/$sp
  accesses with disjoint byte ranges may swap. faultmem stays exact: its
  handler emulates the one faulting insn from live registers and resumes at
  pc+4, and no access crosses another fault-capable access.
- Greedy list scheduling, lookahead 16, X1000-calibrated costs (tenths of a
  cycle: load 34, mul 40, div 360); a region's new order is kept only if its
  modelled cost is lower; regions modelling < 1 cycle of stall are skipped.
- Compile cost ~24k host insns/block vs ~12k to compile it. 32-bit masks only:
  64-bit ctz/shift are libgcc calls through the PLT on MIPS32 (was 60% of the
  first version's cost).
- Layout: `jitsched.o` (no unwind tables, no .data, .text/.rodata renamed) is
  appended after `.plt` by `jitsched.ld`; recRecompile calls `jitsched_tail()`
  and is nop-padded back to its pre-jitsched size (`JITSCHED_RECOMPILE_PAD`,
  re-derive after ANY recompiler.cpp change; `offline/layout_diff.py`). A new
  function inside recompiler.cpp would add an .eh_frame FDE and move .plt.

## Tests

- `tests/test_jitsched.c` (host): independent interpreter, original vs
  scheduled over random blocks, entered at every branch target; pinned words
  must not move; `js_preds` closure == pairwise rule. `-DJITSCHED_MUTANT=1`
  (drop WAR) and `=2` (guest load passes store) must FAIL.
- Offline bar green on 9bf33c0, aac4763 and a2eb81f: lsu_bounds 0/5520 (FM and
  classic), hexec 300/300 x4 incl. `HX_SCHED=1` (in-process off vs on), hlink
  1000 / ctl / reset / reset-both, sched-off vs sched-on hlink differential in
  link modes 0/2/3 (`HL_A_MODE`; mode 3 needs `EXPECT=0`), smcrun 200/200.
  faultmem: 1087/2298 faults landed on moved accesses, sequences identical.
- Not run (master has no pageprot): pphandler, hlink coh, smcrun pp/storm.

## Found on the way

- Ship recipe: `make clean` without `USE_ASYNC_CDROM=1` never cleaned the
  USE_RTHREADS objects, so shipped binaries linked July gcc-14.3.0 copies and
  no clean tree reproduced them (fixed on master separately).
- stallmodel `WIN` lines are rounded to 0.1% (+-12M insns on GT2): use
  `tbdump=1` per-TB counts for deltas.
