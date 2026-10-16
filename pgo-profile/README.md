# PGO profile for the EROSQ pcsx build

`*.gcda` here is a gcov profile of the **Crash Bandicoot slot-0 heavy scene**,
applied by `../build-erosq-pgo.sh` (`-fprofile-use`). Measured **+3.6%** on that
scene, ~+1% on other heavy games, no regressions, bit-exact vs canonical.

## Why the profile is collected under the interpreter

The mipsrec dynarec **SIGBUSes** under gcov instrumentation: JIT'd MIPS code
calls gcov-instrumented C helpers, and the 8-byte arc-counter stores trap on
the dynarec's `-mno-abicalls/-G0` ABI. So we instrument the whole tree but run
it under the **interpreter** (which never enters that ABI context). The hot
code PGO actually optimizes — the core C helpers (gte/mem/hw/events) — executes
the identical guest code either way, so an interpreter-collected profile is
valid for the shipping dynarec binary.

PGO's payoff scales with how core-bound the scene is (it reshapes the core
helpers, not the rasterizer). Crash (~50/50 gpu/core) wins +3.6%; GPU-bound
heavy games (Tomb Raider etc, ~59% gpu) gain only ~1%. Do NOT blend games or
add FMV into the profile — measured to dilute the target back to 0%. Profile
the scene you are trying to save.

## Regenerate

Three env-guarded hooks in the tree enable profiling (all inert in normal
builds): `EROSQ_FORCE_INTERP` (r3000a.c), `EROSQ_NOMIRROR` (recompiler.cpp,
forces the plain METHOD-2 map), `-DEROSQ_GCOV_DUMP` (main.c SignalExit flushes
gcov on SIGINT, since `_exit` bypasses the atexit dumper).

```sh
# 1. instrumented build (value profiling off -> avoids a MIPS TLS reloc error)
make clean
make -j USE_ASYNC_CDROM=1 \
  CFLAGS_OPT="-Ofast -fprofile-generate=$PWD/pgo-profile -fno-profile-values -DEROSQ_GCOV_DUMP"
# 2. on device: run the target scene ~45s under the interpreter, SIGINT to flush
#    GCOV_PREFIX=/media/sd/gcda GCOV_PREFIX_STRIP=0 EROSQ_NOMIRROR=1 \
#    EROSQ_FORCE_INTERP=1 ./pcsx -nolimit -cdfile crash.cue -load 1
#    kill -INT <pid>   # -> 74 .gcda under /media/sd/gcda/<abs>/pgo-profile/
# 3. pull the .gcda back into pgo-profile/, then: ./build-erosq-pgo.sh
```
