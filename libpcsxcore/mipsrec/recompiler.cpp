/*
 * Mips-to-mips recompiler for pcsx4all
 *
 * Copyright (c) 2009 Ulrich Hecht
 * Copyright (c) 2017 modified by Dmitry Smagin, Daniel Silsby
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/* EROSQ/rearmed port: compat.h must come first — it redirects the
 * pcsx4all-relative includes below onto rearmed's libpcsxcore headers and
 * defines the memory/cycle/writeok seams these files expect. See
 * libpcsxcore/mipsrec/compat.h for details. */
#include "compat.h"

#include <stddef.h>
#include "../psxcommon.h"
#include "../psxhle.h"
#include "../psxmem.h"
#include "../psxhw.h"
#include "../r3000a.h"
#include "../gte.h"

/* For direct HW I/O */
#include "../mdec.h"
#include "../cdrom.h"

/* EROSQ Phase 6 (Part A): POSIX shared-memory + mmap for the mirrored METHOD-1
 * RAM mapping (mipsrec_init_mmap below). */
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <ucontext.h>

/* Standard console logging */
#define REC_LOG(...) printf("mipsrec: " __VA_ARGS__)
#ifndef REC_LOG
#define REC_LOG(...)
#endif

/* Verbose console logging (uncomment next line to enable) */
//#define REC_LOG_V REC_LOG
#ifndef REC_LOG_V
#define REC_LOG_V(...)
#endif

/* Use inlined-asm version of block dispatcher: */
#define ASM_EXECUTE_LOOP

/* Scan for and skip useless code in PS1 executable: */
#define USE_CODE_DISCARD

/* If HLE emulated BIOS is not in use, blocks return to dispatch loop directly */
#define USE_DIRECT_BLOCK_RETURN_JUMPS

/* If a block jumps backwards to the top of itself, use fast dispatch path.
 *  Every block's recompiled start address is saved before entry. To
 *  return to the dispatch loop, it jumps to a shorter version of dispatch
 *  loop that skips code table lookups and code invalidation checks.
 * This could *theoretically* cause a problem  with poorly-behaved
 *  self-modifying code, but so far no issues have been found.
 * NOTE: Option only has effect if USE_DIRECT_BLOCK_RETURN_JUMPS is enabled,
 *  which itself only has effect when HLE emulated BIOS is not in use.
 */
#define USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS

/* Const propagation is applied to addresses */
#define USE_CONST_ADDRESSES

/* Const propagation is extended to optimize 'fuzzy' non-const addresses */
#define USE_CONST_FUZZY_ADDRESSES

/* Generate inline memory access or call psxMemRead/Write C functions.
 *
 * EROSQ/rearmed Phase 6 (fastmem): direct inline RAM loads/stores are now
 * ENABLED. Because SHMEM_MIRRORING/TMPFS_MIRRORING stay undefined below,
 * psx_mem_mapped is constant-false, so emitAddressConversion() takes METHOD 2
 * (rec_lsu.cpp.h:274): host_addr = psxRegs.ptrs.psxM + (guest & 0x1fffff),
 * a masked base-relative access against rearmed's own psxM. Hardware-I/O /
 * scratchpad / BIOS accesses still take the indirect psxMemRead/Write callout
 * (chosen per-series by the runtime range check + the const/fuzzy-addr
 * classifiers). SMC invalidation is emitted inline (SW 0, recRAM[...]) — the
 * same recRAM slot the LUT dispatch reads. Cache-isolation safety: direct
 * stores skip the writeok guard (SKIP_WRITEOK_CHECK), so recNotify() backs up
 * & restores the low 64KB of RAM across isolation (see ram_bak below), matching
 * pcsx4all's psxMemWrite32_CacheCtrlPort mechanism. */
#define USE_DIRECT_MEM_ACCESS

/* EROSQ Phase 6 (Part A): turn on METHOD-1 virtual RAM mapping. Defining the
 * mirroring umbrella enables USE_VIRTUAL_PSXMEM_MAPPING (mirrored psxM,
 * inlined scratchpad, 2-instr address conversion, no same-2MB check). We then
 * #undef USE_VIRTUAL_RECRAM_MAPPING (Part B — block-dispatch mmap) right after
 * the block: it touches recRAM/PC_REC and the SMC-invalidation path and is
 * deliberately deferred. rec_mmap_psx_mem()/rec_mmap_rec_mem() below stay inert;
 * the real mapping is done in mipsrec_init_mmap(), called from psxMemInit() so
 * rearmed's memRLUT/memWLUT build against the mapped psxM (correct ordering). */
#define SHMEM_MIRRORING

/* Virtual memory mapping options: */
#if defined(SHMEM_MIRRORING) || defined(TMPFS_MIRRORING)
	/* 2MB of PSX RAM (psxM) is now mapped+mirrored virtually, much like
	 *  a real PS1. We can skip mirror-region checks, the 'Einhander' game
	 *  fix. We can skip masking RAM addresses. We also map 0x1fxx_xxxx
	 *  regions (psxP,psxH) into this space, inlining scratchpad accesses.
	 *
	 * IMPORTANT: Don't enable if 'USE_DIRECT_MEM_ACCESS' isn't also enabled.
	 *            There'd be no benefit to a virtual mapping: all mem access
	 *            would be through indirect psxMemWrite/psxMemRead C funcs.
	 *            Even worse, we support a mapping starting at address 0, and
	 *            that is not compatible with psxMemWrite/psxMemRead funcs
	 *            because of how they handle NULL addresses in their LUTs.
	 */
	#ifdef USE_DIRECT_MEM_ACCESS
		#define USE_VIRTUAL_PSXMEM_MAPPING
	#else
		#warning "USE_DIRECT_MEM_ACCESS is undefined! Dynarec will emit slower C memory accesses."
	#endif

	/* Prefer virtually mapped/mirrored code block pointer array over using
	 *  psxRecLUT[]. This removes a layer of indirection from PC->block-ptr lookups,
	 *  allows faster block dispatch loops, and reduces cache/TLB pressure.
	 */
	#define USE_VIRTUAL_RECRAM_MAPPING

#else
	#warning "Neither SHMEM_MIRRORING nor TMPFS_MIRRORING are defined! Dynarec will use slower block dispatch loop and emit slower memory accesses. Check your Makefile!"
#endif // defined(SHMEM_MIRRORING) || defined(TMPFS_MIRRORING)

/* EROSQ Phase 6: Part B (block-dispatch mmap) is IMPLEMENTED (rec_mmap_rec_mem
 * below is a faithful port; recRAM/recROM would map+mirror at REC_RAM_VADDR and
 * recInit would use the mmap dispatch loops, dropping the psxRecLUT[] indirection
 * from PC->block-ptr lookups). It is HW-validated correct (Crash boots/renders
 * pixel-perfect, SMC + cache-isolation coherent via the 4x recRAM mirror), but
 * left DEFAULT-OFF: A/B on Crash gameplay showed no measurable gain over Part A
 * (uncapped 52.7 vs 52.4 vsps, within noise) because mipsrec's block chaining +
 * self-loop fastpath already bypass psxRecLUT on the hot paths — Part B only
 * saves the indirection on rare non-chained dispatches. Flip this #undef to a
 * no-op to enable (may help block-churn-heavy titles). */
#undef USE_VIRTUAL_RECRAM_MAPPING

//#define WITH_DISASM
//#define DEBUGG printf

/* EROSQ/rearmed port: mem_mapping.h is Phase 6 (virtual mmap/mirroring of
 * PSX RAM and the recompiled-block pointer tables) and is deliberately not
 * vendored yet. SHMEM_MIRRORING/TMPFS_MIRRORING are not defined by this
 * build, so USE_VIRTUAL_PSXMEM_MAPPING/USE_VIRTUAL_RECRAM_MAPPING above are
 * never defined either, and rec_mmap_*()/rec_munmap_*() (mem_mapping.h's
 * actual mmap API) are never referenced below. What *is* referenced
 * unconditionally (PC_REC* macros, recRAM/recROM sizing, recClear()) are
 * just size/address constants, reproduced here rather than pulling in the
 * whole pcsx4all mem_mapping.h/.cpp pair. PSX_MEM_VADDR / rec_mmap_*() will
 * come back with the real Phase 6 header. */
#define REC_RAM_PTR_SIZE sizeof(uptr)
#define REC_RAM_SIZE (0x200000 / 4 * REC_RAM_PTR_SIZE)
#define REC_ROM_SIZE ( 0x80000 / 4 * REC_RAM_PTR_SIZE)
#define REC_RAM_VADDR 0x20000000ULL
#define REC_ROM_VADDR (REC_RAM_VADDR + (0x00c00000ULL / 4 * REC_RAM_PTR_SIZE))
#ifdef MMAP_TO_ADDRESS_ZERO
#define PSX_MEM_VADDR 0ULL
#else
#define PSX_MEM_VADDR 0x10000000ULL
#endif
/* Declared but not defined in this phase: only reachable via the
 * USE_VIRTUAL_RECRAM_MAPPING/USE_VIRTUAL_PSXMEM_MAPPING dead branches below
 * (never taken — SHMEM_MIRRORING/TMPFS_MIRRORING aren't defined), except
 * recShutdown()'s unconditional-but-runtime-dead calls guarded by
 * psx_mem_mapped/rec_mem_mapped (always false here). Real Phase 6 mem_mapping.*
 * will replace these. */
int  rec_mmap_psx_mem();
void rec_munmap_psx_mem();
int  rec_mmap_rec_mem();
void rec_munmap_rec_mem();

/* EROSQ/rearmed port (Phase 3): inert definitions of the Phase-6 mmap API.
 * SHMEM_MIRRORING/TMPFS_MIRRORING are undefined, so psx_mem_mapped and
 * rec_mem_mapped are constant-false: rec_mmap_*() are only *referenced* under
 * the never-compiled USE_VIRTUAL_*_MAPPING branches (recInit), and
 * rec_munmap_*() only under recShutdown()'s always-false runtime guards. Until
 * Phase 3 wired recShutdown() live (via mipsrec_rec_shutdown), --gc-sections
 * dropped recShutdown() entirely and these stayed unreferenced; now the
 * symbols must resolve. Bodies are deliberately inert (never executed) and get
 * replaced wholesale by the real mem_mapping.* in Phase 6. */
int  rec_mmap_psx_mem()   { return -1; }
void rec_munmap_psx_mem() { }

/* EROSQ Phase 6 (Part B): map the recRAM block-ptr table at REC_RAM_VADDR
 * (0x20000000), mirrored 4x (2MB each) so the PS1's RAM mirroring folds
 * through the TLB, and recROM at REC_ROM_VADDR. Ported from pcsx4all's
 * mem_mapping.cpp rec_mmap_rec_mem(). Called from recInit() (dynarec-private,
 * no ordering constraint with psxMemInit). On failure everything is unwound
 * and -1 returned so recInit falls back to malloc'd recRAM + the LUT dispatch
 * loops (correct, just slower). The 4x MAP_SHARED mirror is load-bearing: the
 * emitted SMC store writes recRAM+(addr&0x1fffff) (first copy) while the mmap
 * dispatch reads mirror copies — they must alias the same physical slot. */
int rec_mmap_rec_mem()
{
	int   fd = -1;
	int   n_maps = 0;
	void *rom = MAP_FAILED;

	/* memfd, not a named /dev/shm object: a fixed shm name let any local
	 * process open (or pre-create) the backing of guest RAM / block ptrs. */
	fd = memfd_create("mipsrec_recram", MFD_CLOEXEC);
	if (fd < 0) { perror("rec_mmap_rec_mem: memfd_create"); return -1; }
	if (ftruncate(fd, REC_RAM_SIZE) < 0) { perror("rec_mmap_rec_mem: ftruncate"); goto fail; }

	for (n_maps = 0; n_maps < 4; n_maps++) {
		void *want = (void *)(uintptr_t)(REC_RAM_VADDR + (uintptr_t)REC_RAM_SIZE * n_maps);
		void *got  = mmap(want, REC_RAM_SIZE, PROT_READ | PROT_WRITE,
		                  MAP_SHARED | MAP_FIXED, fd, 0);
		if (got == MAP_FAILED || got != want) {
			printf("rec_mmap_rec_mem: recRAM mirror %d at %p failed\n", n_maps, want);
			goto fail;
		}
	}

	rom = mmap((void *)(uintptr_t)REC_ROM_VADDR, REC_ROM_SIZE, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (rom == MAP_FAILED || rom != (void *)(uintptr_t)REC_ROM_VADDR) {
		printf("rec_mmap_rec_mem: recROM at 0x%llx failed\n", (unsigned long long)REC_ROM_VADDR);
		goto fail;
	}

	close(fd);
	printf("mipsrec: recRAM mirrored at 0x%llx (mmap block dispatch)\n",
	       (unsigned long long)REC_RAM_VADDR);
	return 0;

fail:
	while (n_maps-- > 0)
		munmap((void *)(uintptr_t)(REC_RAM_VADDR + (uintptr_t)REC_RAM_SIZE * n_maps), REC_RAM_SIZE);
	if (rom != MAP_FAILED) munmap(rom, REC_ROM_SIZE);
	if (fd >= 0) close(fd);
	return -1;
}

void rec_munmap_rec_mem()
{
	munmap((void *)(uintptr_t)REC_RAM_VADDR, (size_t)REC_RAM_SIZE * 4);
	munmap((void *)(uintptr_t)REC_ROM_VADDR, REC_ROM_SIZE);
}

/* Bit vector indicating which PS1 RAM pages contain the start of blocks.
 *  Used to determine when code invalidation in recClear() can be skipped.
 */
static u8 code_pages[0x200000/4096/8];

/* Pointers to the recompiled blocks go here. psxRecLUT[] uses upper 16 bits of
 *  a PC value as an index to lookup a block pointer stored in recRAM/recROM.
 */
static s8 *recRAM;
static s8 *recROM;
static uptr psxRecLUT[0x10000];

/* fastmem: direct stores skip the writeok/cache-isolation guard, so we back up
 * the low 64KB of PS1 RAM on cache-isolate and restore it on un-isolate (the
 * i-cache-flush idiom trashes low RAM while isolated). Mirrors pcsx4all's
 * mem_bak in psxMemWrite32_CacheCtrlPort(); handled in recNotify(). */
static u32 ram_bak[0x10000 / 4];

#undef PC_REC
#undef PC_REC8
#undef PC_REC16
#undef PC_REC32
#define PC_REC(x)	((uptr)psxRecLUT[(x) >> 16] + (((x) & 0xffff) * (REC_RAM_PTR_SIZE / 4)))
#define PC_REC8(x)	(*(u8 *)PC_REC(x))
#define PC_REC16(x)	(*(u16*)PC_REC(x))
#define PC_REC32(x)	(*(u32*)PC_REC(x))
/* Version of PC_REC() that uses faster virtual block ptr mapping */
#define PC_REC_MMAP(x)	(REC_RAM_VADDR | (((x) & 0x00ffffff) * (REC_RAM_PTR_SIZE / 4)))

#include "mips_codegen.h"
#ifdef WITH_DISASM
/* disasm.h/mips_disasm.cpp are debug-only and not vendored (see task brief);
 * WITH_DISASM is left undefined (see the commented-out #define above), so
 * this branch is inert. */
#include "disasm.h"
#endif
#include "host_asm.h"


/* Const-propagation data and functions */
typedef struct {
	u32  constval;
	bool is_const;

	bool is_fuzzy_ram_addr;        /* GPR is not known-const, but at least known
	                                   to be address somewhere in RAM? */
	bool is_fuzzy_nonram_addr;     /* GPR is not known-const, but at least known
	                                   to be address somewhere outside RAM? */
	bool is_fuzzy_scratchpad_addr; /* GPR is not known-const, but at least known
	                                   to be address somewhere in 1KB scratcpad? */
} iRegisters;
static iRegisters iRegs[32];
static inline void ResetConsts()
{
	memset(&iRegs, 0, sizeof(iRegs));
	iRegs[0].is_const = true;  // $r0 is always zero val
}
static inline bool IsConst(const u32 reg)  { return iRegs[reg].is_const; }
static inline u32  GetConst(const u32 reg) { return iRegs[reg].constval; }
static inline void SetUndef(const u32 reg)
{
	if (reg) {
		iRegs[reg].is_const = false;
		iRegs[reg].is_fuzzy_ram_addr        = false;
		iRegs[reg].is_fuzzy_nonram_addr     = false;
		iRegs[reg].is_fuzzy_scratchpad_addr = false;
	}
}
static inline void SetConst(const u32 reg, const u32 val)
{
	if (reg) {
		iRegs[reg].constval = val;
		iRegs[reg].is_const = true;
		iRegs[reg].is_fuzzy_ram_addr        = false;
		iRegs[reg].is_fuzzy_nonram_addr     = false;
		iRegs[reg].is_fuzzy_scratchpad_addr = false;
	}
}
static inline void SetFuzzyRamAddr(const u32 reg)        { iRegs[reg].is_fuzzy_ram_addr = true; }
static inline bool IsFuzzyRamAddr(const u32 reg)         { return iRegs[reg].is_fuzzy_ram_addr; }
static inline void SetFuzzyNonramAddr(const u32 reg)     { iRegs[reg].is_fuzzy_nonram_addr = true; }
static inline bool IsFuzzyNonramAddr(const u32 reg)      { return iRegs[reg].is_fuzzy_nonram_addr; }
static inline void SetFuzzyScratchpadAddr(const u32 reg) { iRegs[reg].is_fuzzy_scratchpad_addr = true; }
static inline bool IsFuzzyScratchpadAddr(const u32 reg)  { return iRegs[reg].is_fuzzy_scratchpad_addr; }


/* Code cache buffer
 *  Keep this statically allocated! This keeps it close to the
 *  .text section, so C code and recompiled code lie in the same 256MiB region.
 *  MIPS JAL/J opcodes in recompiled code that jump to C code require this.
 *  Dynamic allocation would get an anonymous mmap'ing, locating the recompiled
 *  code *far* too high in virtual address space.
 */
#define RECMEM_SIZE         (12 * 1024 * 1024)
#define RECMEM_SIZE_MAX     (RECMEM_SIZE-(512*1024))
static u8 recMemBase[RECMEM_SIZE] __attribute__((aligned(4)));

u32        *recMem;                /* Where does next emitted opcode in block go? */

/* ponytail: expose the code-cache range for the EROSQ_SAMPLE PC sampler
 * (plat_erosq.c) so it can bucket samples as JIT (emitted block code) vs native. */
extern "C" void mipsrec_code_range(uintptr_t *lo, uintptr_t *hi)
{
	*lo = (uintptr_t)recMemBase;
	*hi = (uintptr_t)recMemBase + RECMEM_SIZE;
}

static u32 *recMemStart;           /* Where did first emitted opcode in block go? */
static u32 pc;                     /* Recompiler pc */
static u32 oldpc;                  /* Recompiler pc at start of block */
u32 cycle_multiplier = 0x200;      /* Cycle advance per emulated instruction
                                      Default is 0x200 == 2.00 (24.8 fixed-pt) */

/* See comments in recExecute() regarding direct block returns */
static uptr block_ret_addr;                /* Non-zero when blocks are using direct return jumps */
static uptr block_fast_ret_addr;           /* Non-zero when blocks are using direct return jumps &
                                              dispatch loop fastpath is enabled */

static bool psx_mem_mapped;                /* PS1 RAM mmap'd+mirrored at fixed address? (psxM) */
static bool rec_mem_mapped;                /* Code ptr arrays mmap'd+mirrored at fixed address? (recRAM,recROM) */

/* EROSQ Phase 6 (Part A): set up the METHOD-1 mirrored RAM mapping and repoint
 * rearmed's psxM/psxP/psxH/psxR into it. Called from psxMemInit() (via the
 * MIPSREC_CUSTOM_MAP branch, parallel to lightrec_init_mmap) BEFORE rearmed
 * builds memRLUT/memWLUT, so the LUTs and the emitted METHOD-1 direct accesses
 * both address the same physical RAM.
 *
 * Layout matches emitAddressConversion()'s host = PSX_MEM_VADDR|(guest&0x0fffffff):
 *   - 2MB RAM shared-mapped + mirrored 4x at PSX_MEM_VADDR (0x10000000..0x107fffff);
 *     the TLB folds the PS1's four RAM mirrors, so no address masking is emitted.
 *   - scratchpad+HW (psxH) fixed at 0x1f800000 so inlined scratchpad stores land
 *     on the same pages psxMemRead/Write use (only scratchpad is accessed direct;
 *     HW regs stay indirect via the runtime range check).
 *   - expansion (psxP) and BIOS (psxR) are indirect-only; plain anonymous maps.
 * On any failure everything is unwound and -1 returned, so psxMemInit() can fall
 * back to the plain (METHOD-2) map and the emulator still runs, just slower. */
/* EROSQ: anonymous shared memory object for a mapping that must exist
 * twice. memfd_create() if the kernel has it; else shm_open() under a
 * random name with O_EXCL and mode 0600, unlinked right away (no window in
 * which another process can open it; retried on EEXIST). */
#include <sys/syscall.h>
static int mipsrec_anon_shm(size_t size)
{
	int fd = -1;
#ifdef SYS_memfd_create
	fd = (int)syscall(SYS_memfd_create, "mipsrec", 0u);
#endif
	if (fd < 0) {
		for (int tries = 0; tries < 16 && fd < 0; tries++) {
			unsigned r = 0;
			int ur = open("/dev/urandom", O_RDONLY);
			if (ur >= 0) { if (read(ur, &r, sizeof r) != sizeof r) r = 0; close(ur); }
			r ^= (unsigned)getpid() * 2654435761u ^ (unsigned)tries;
			char name[48];
			snprintf(name, sizeof name, "/mipsrec.%08x.%u", r, (unsigned)tries);
			fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
			if (fd >= 0) shm_unlink(name);
			else if (errno != EEXIST) break;
		}
	}
	if (fd >= 0 && ftruncate(fd, size) < 0) { close(fd); fd = -1; }
	return fd;
}

#define RESV_LO 0x0fe00000u
#define RESV_HI 0x20010000u

extern "C" int mipsrec_init_mmap(void)
{
	int   fd = -1;

	/* ponytail: PGO profiling only — skip the fixed-VA MAP_FIXED mirror map
	 * (collides with libgcov's runtime), fall back to plain METHOD-2 map. */
	if (getenv("EROSQ_NOMIRROR")) return -1;

	void *ram = MAP_FAILED, *hw = MAP_FAILED, *exp = MAP_FAILED, *bios = MAP_FAILED;
	void *resv = MAP_FAILED;
	int   n_ram_maps = 0;
	int   n_guard_mirrors = 0;

	if (sysconf(_SC_PAGESIZE) > 65536) {
		printf("mipsrec_init_mmap: page size > 64KB unsupported\n");
		return -1;
	}

	/* EROSQ containment: every METHOD-1 fast-path access computes
	 * 0x10000000 | (base & mask) + sext(imm16), i.e. a host address in
	 * [0x0fff8000, 0x20007fff]. Reserve the whole span (rounded out to
	 * [0x0fe00000, 0x20010000)) as PROT_NONE before mapping the PSX regions
	 * into it, so that no other host mapping can ever live where a guest
	 * address can reach. MAP_FIXED_NOREPLACE: if anything already occupies
	 * the span we refuse METHOD-1 rather than clobber it. Costs VA only. */
	resv = mmap((void *)RESV_LO, RESV_HI - RESV_LO, PROT_NONE,
	            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (resv != (void *)RESV_LO) {
		if (resv != MAP_FAILED) munmap(resv, RESV_HI - RESV_LO);
		resv = MAP_FAILED;
		printf("mipsrec_init_mmap: guest window [%#x,%#x) not free\n", RESV_LO, RESV_HI);
		goto fail;
	}

	/* Shared backing object for the 2MB RAM + its 3 mirrors. */
	/* memfd, not a named /dev/shm object: a fixed shm name let any local
	 * process open (or pre-create) the backing of guest RAM / block ptrs. */
	fd = memfd_create("mipsrec_psxmem", MFD_CLOEXEC);
	if (fd < 0) { perror("mipsrec_init_mmap: memfd_create"); goto fail; }
	if (ftruncate(fd, 0x200000) < 0) { perror("mipsrec_init_mmap: ftruncate"); goto fail; }

	for (n_ram_maps = 0; n_ram_maps < 4; n_ram_maps++) {
		void *want = (void *)(uintptr_t)(PSX_MEM_VADDR + 0x200000ULL * n_ram_maps);
		void *got  = mmap(want, 0x200000, PROT_READ | PROT_WRITE,
		                  MAP_SHARED | MAP_FIXED, fd, 0);
		if (got == MAP_FAILED || got != want) {
			printf("mipsrec_init_mmap: RAM mirror %d at %p failed\n", n_ram_maps, want);
			goto fail;
		}
		if (n_ram_maps == 0) ram = got;
	}

	/* One more RAM mirror directly below and above the 8MB mirror block:
	 * RAM-masked conversions (23 bits) plus a negative / positive imm16 then
	 * still land in PSX RAM instead of faulting. */
	{
		void *lo = mmap((void *)(uintptr_t)(PSX_MEM_VADDR - 0x200000), 0x200000,
		                PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
		void *hi = mmap((void *)(uintptr_t)(PSX_MEM_VADDR + 0x800000), 0x200000,
		                PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
		if (lo == MAP_FAILED || hi == MAP_FAILED) {
			printf("mipsrec_init_mmap: guard RAM mirrors failed\n"); goto fail;
		}
		n_guard_mirrors = 2;
	}

	/* scratchpad + HW I/O region, fixed at 0x1f800000 (METHOD-1 expects it there).
	 * EROSQ faultmem: psxH is a shared object mapped twice -- the fixed
	 * METHOD-1 view (JIT direct accesses) keeps only the scratchpad page
	 * readable/writable and makes the HW I/O pages PROT_NONE, so a direct
	 * access that lands on a HW register faults into the faultmem handler;
	 * the C code (psxHw*, memRLUT, const-HW JIT accesses that bake psxH)
	 * uses a full read/write alias of the same pages. Coherent by sharing.
	 * The object is anonymous (memfd, or a random O_EXCL 0600 shm name that
	 * is unlinked immediately: mipsrec_anon_shm). */
	{
		int hfd = mipsrec_anon_shm(0x10000);
		if (hfd < 0) {
			perror("mipsrec_init_mmap: psxH shm");
			goto fail;
		}
		void *fixed = mmap((void *)0x1f800000, 0x10000, PROT_READ | PROT_WRITE,
		                   MAP_SHARED | MAP_FIXED, hfd, 0);
		hw = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, hfd, 0);
		close(hfd);
		if (fixed != (void *)0x1f800000 || hw == MAP_FAILED) {
			printf("mipsrec_init_mmap: psxH at 0x1f800000 failed\n");
			if (fixed != MAP_FAILED) munmap(fixed, 0x10000);
			goto fail;
		}
		if (!getenv("EROSQ_FAULTMEM") || getenv("EROSQ_FAULTMEM")[0] != '0')
			mprotect((void *)0x1f801000, 0xf000, PROT_NONE);
	}

	/* Expansion region (psxP) — fixed at 0x1f000000, the address METHOD-1's
	 * emitted conversion (host = 0x10000000 | (guest & 0x0fffffff)) produces
	 * for guest 0x1f00_0000. The direct-path range check ADMITS the whole
	 * expansion window (bits 23:12 < 0x801), so a non-const, non-fuzzy base
	 * reg pointing there took the direct path into unmapped VA -> SIGSEGV.
	 * Map the full 8MB window: anonymous pages cost no RSS unless touched,
	 * and no real game touches expansion space through a computed pointer. */
	exp = mmap((void *)0x1f000000, 0x800000, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (exp == MAP_FAILED || exp != (void *)0x1f000000) {
		printf("mipsrec_init_mmap: psxP at 0x1f000000 failed\n"); goto fail;
	}

	/* BIOS (psxR) — indirect-only. */
	bios = mmap(NULL, 0x80000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (bios == MAP_FAILED) { perror("mipsrec_init_mmap: psxR"); goto fail; }

	close(fd);

	psxRegs.ptrs.psxM = (u8 *)ram;
	psxH = (u8 *)hw;   /* compat.h macro-aliases the member name 'psxH' */
	psxRegs.ptrs.psxP = (u8 *)exp;
	psxRegs.ptrs.psxR = (u8 *)bios;
	psx_mem_mapped = true;
	printf("mipsrec: PSX RAM mirrored at %p (METHOD-1 fastmem)\n", ram);
	return 0;

fail:
	if (resv != MAP_FAILED) {
		/* drops the PSX regions mapped inside the reservation too */
		munmap((void *)RESV_LO, RESV_HI - RESV_LO);
		n_ram_maps = 0; hw = MAP_FAILED; exp = MAP_FAILED;
	}
	(void)n_guard_mirrors;
	while (n_ram_maps-- > 0)
		munmap((void *)(uintptr_t)(PSX_MEM_VADDR + 0x200000ULL * n_ram_maps), 0x200000);
	if (hw   != MAP_FAILED) { munmap(hw, 0x10000); munmap((void *)0x1f800000, 0x10000); }
	if (exp  != MAP_FAILED) munmap(exp,  0x800000);
	if (bios != MAP_FAILED) munmap(bios, 0x80000);
	if (fd >= 0) close(fd);
	psx_mem_mapped = false;
	printf("mipsrec_init_mmap: failed, falling back to non-mirrored map (slower)\n");
	return -1;
}

/* Flags used during a recompilation phase */
static bool branch;                        /* Current instruction lies in a BD slot? */
static bool end_block;                     /* Has recompilation phase ended? */
static bool skip_emitting_next_mflo;       /* Was a MULT/MULTU converted to 3-op MUL? See rec_mdu.cpp.h */
static bool emit_code_invalidations;       /* Emit code invalidation for store instructions? */
static bool flush_code_on_dma3_exe_load;   /* Flush code cache when psxDma3() detects EXE load? */

/* Flags/vals used to cache common values in temp regs in emitted code */
static bool lsu_tmp_cache_valid;           /* LSU vals are cached in $at,$v1. See rec_lsu.cpp.h */
static bool host_v0_reg_is_const;          /* PCs are cached in $v0. See rec_bcu.cpp.h */
static u32  host_v0_reg_constval;
static bool host_ra_reg_has_block_retaddr; /* Indirect-return address is cached in $ra. */

/* EROSQ faultmem (see the block above recRecompile()): may the block being
 * compiled turn dual-path load/store series into direct-only ones? */
static bool faultmem_block;
static bool faultmem_block_used;


#ifdef WITH_DISASM
char	disasm_buffer[512];
#endif

/* Defined in rec_lsu.cpp.h (same TU, included later via opcodes.h):
 * invalidates cached METHOD-1 converted addresses when a guest reg is
 * written. Called from regMipsChanged(). */
static void lsu_note_guest_reg_write(u32 regpsx);

#include "emitstat.h"
#include "regcache.h"

static void recReset();
static void recRecompile();
static void recClear(u32 Addr, u32 Size);
static void recNotify(int note, void *data);

extern void (*recBSC[64])();
extern void (*recSPC[64])();
extern void (*recREG[32])();
extern void (*recCP0[32])();
extern void (*recCP2[64])();
extern void (*recCP2BSC[32])();


#ifdef WITH_DISASM

#define make_stub_label(name) \
 { (void *)name, (char*)#name }

disasm_label stub_labels[] =
{
  make_stub_label(gteMFC2),
  make_stub_label(gteMTC2),
  make_stub_label(gteLWC2),
  make_stub_label(gteSWC2),
  make_stub_label(gteRTPS),
  make_stub_label(gteOP),
  make_stub_label(gteNCLIP),
  make_stub_label(gteDPCS),
  make_stub_label(gteINTPL),
  make_stub_label(gteMVMVA),
  make_stub_label(gteNCDS),
  make_stub_label(gteNCDT),
  make_stub_label(gteCDP),
  make_stub_label(gteNCCS),
  make_stub_label(gteCC),
  make_stub_label(gteNCS),
  make_stub_label(gteNCT),
  make_stub_label(gteSQR),
  make_stub_label(gteDCPL),
  make_stub_label(gteDPCT),
  make_stub_label(gteAVSZ3),
  make_stub_label(gteAVSZ4),
  make_stub_label(gteRTPT),
  make_stub_label(gteGPF),
  make_stub_label(gteGPL),
  make_stub_label(gteNCCT),
  make_stub_label(psxMemRead8),
  make_stub_label(psxMemRead16),
  make_stub_label(psxMemRead32),
  make_stub_label(psxMemWrite8),
  make_stub_label(psxMemWrite16),
  make_stub_label(psxMemWrite32),
  make_stub_label(psxHwRead8),
  make_stub_label(psxHwRead16),
  make_stub_label(psxHwRead32),
  make_stub_label(psxHwWrite8),
  make_stub_label(psxHwWrite16),
  make_stub_label(psxHwWrite32),
  make_stub_label(psxException),
  // Direct HW I/O:
  make_stub_label(cdrRead0),
  make_stub_label(cdrRead1),
  make_stub_label(cdrRead2),
  make_stub_label(cdrRead3),
  make_stub_label(cdrWrite0),
  make_stub_label(cdrWrite1),
  make_stub_label(cdrWrite2),
  make_stub_label(cdrWrite3),
  make_stub_label(GPU_writeData),
  make_stub_label(mdecRead0),
  make_stub_label(mdecRead1),
  make_stub_label(mdecWrite0),
  make_stub_label(mdecWrite1),
  make_stub_label(psxRcntRcount),
  make_stub_label(psxRcntRmode),
  make_stub_label(psxRcntRtarget),
  make_stub_label(psxRcntWcount),
  make_stub_label(psxRcntWmode),
  make_stub_label(psxRcntWtarget),
  make_stub_label(sioRead8),
  make_stub_label(sioRead16),
  make_stub_label(sioRead32),
  make_stub_label(sioReadBaud16),
  make_stub_label(sioReadCtrl16),
  make_stub_label(sioReadMode16),
  make_stub_label(sioReadStat16),
  make_stub_label(sioWrite8),
  make_stub_label(sioWrite16),
  make_stub_label(sioWrite32),
  make_stub_label(sioWriteBaud16),
  make_stub_label(sioWriteCtrl16),
  make_stub_label(sioWriteMode16),
  make_stub_label(SPU_writeRegister),
};

const u32 num_stub_labels = sizeof(stub_labels) / sizeof(disasm_label);

#define DISASM_INIT() \
do { \
	printf("Block PC %x (MIPS) -> %p\n", pc, recMemStart); \
} while (0)

#define DISASM_PSX(_PC_) \
do { \
	u32 opcode = *(u32 *)((char *)PSXM(_PC_)); \
	disasm_mips_instruction(opcode, disasm_buffer, _PC_, 0, 0); \
	printf("%08x: %08x %s\n", _PC_, opcode, disasm_buffer); \
} while (0)

#define DISASM_HOST() \
do { \
	printf("\n"); \
	u8 *tr_ptr = (u8*)recMemStart; \
	for (; (u32)tr_ptr < (u32)recMem; tr_ptr += 4) { \
		u32 opcode = *(u32*)tr_ptr; \
		disasm_mips_instruction(opcode, disasm_buffer, \
					(u32)tr_ptr, stub_labels, \
					num_stub_labels); \
		printf("%08x: %s\t(0x%08x)\n", \
			(u32)tr_ptr, disasm_buffer, opcode); \
	} \
	printf("\n"); \
} while (0)

#define DISASM_MSG printf

#else

#define DISASM_PSX(_PC_)
#define DISASM_HOST()
#define DISASM_INIT()
#define DISASM_MSG(...)

#endif


#include "opcodes.h"

#ifndef HAVE_MIPS32R2_CACHE_OPS
#include <sys/cachectl.h>
#endif

static inline void clear_insn_cache(void *start, void *end, int flags)
{
#ifdef HAVE_MIPS32R2_CACHE_OPS
	// MIPS32r2 added fine-grained usermode cache flush ability (yes, please!)
	MIPS32R2_MakeCodeVisible(start, (char *)end - (char *)start);
#else
	// Use Linux system call (ends up flushing entire cache)
	#ifdef DYNAREC_SKIP_DCACHE_FLUSH
		// Faster, but only works if host's ICACHE pulls from DCACHE, not RAM
		int cache_to_flush = ICACHE;
	#else
		// Slower, but most compatible (flush both caches)
		int cache_to_flush = BCACHE; // ICACHE|DCACHE
	#endif

	cacheflush(start, (char *)end - (char *)start, cache_to_flush);
#endif
}


/* Set default recompilation options, and any per-game settings */
static void rec_set_options()
{
	// Default options
	emit_code_invalidations = true;
	flush_code_on_dma3_exe_load = false;

	// Per-game options
	// -> Use case-insensitive comparisons! Some CDs have lowercase CdromId.

	// 'Studio 33' game workarounds (other Studio 33 games seem to be OK)
	//  See comments in recNotify(), psxDma3().
	if (strncasecmp(CdromId, "SCES03886", 9) == 0  ||  // Formula 1 Arcade
	    strncasecmp(CdromId, "SLUS00870", 9) == 0  ||  // Formula 1 '99  NTSC US
	    strncasecmp(CdromId, "SCPS10101", 9) == 0  ||  // Formula 1 '99  NTSC J (untested)
	    strncasecmp(CdromId, "SCES01979", 9) == 0  ||  // Formula 1 '99  PAL  E (requires .SBI subchannel file)
	    strncasecmp(CdromId, "SLES01979", 9) == 0  ||  // Formula 1 '99  PAL  E (unknown revision, couldn't test)
	    strncasecmp(CdromId, "SCES03404", 9) == 0  ||  // Formula 1 2001 PAL  E,Fi (fixes broken AI/controls)
	    strncasecmp(CdromId, "SCES03423", 9) == 0)     // Formula 1 2001 PAL  Fr,G (fixes broken AI/controls)
	{
		REC_LOG("Using Icache workarounds for trouble games 'Formula One 99/2001/etc'.\n");
		emit_code_invalidations = false;
		flush_code_on_dma3_exe_load = true;
	}
}


/* EROSQ: block-entry guest-register preload.
 *
 * The register cache loads a guest reg the moment an emitter first asks for
 * it (regMipsToHostHelper: LW host, offGPR(psx)) and the emitter consumes it
 * in the very next host instruction. On the X1000 a load result consumed by
 * the next instruction costs +3 cycles (+2 / +1 at distance 2 / 3; X1000 PM
 * section 2.3). Every block starts with an empty cache (regReset), so each
 * distinct guest reg a block reads pays that stall once.
 *
 * Instead, scan the block's first basic block (up to and including the first
 * branch/jump and its delay slot), collect the regs that are READ BEFORE
 * BEING WRITTEN, and load up to N of them back-to-back before any code is
 * emitted: lw;lw;lw;lw -> each first use is >= 3 instructions after its
 * load. The loads are the ones the emitters would have issued anyway (every
 * collected reg is read in the first basic block), they map clean (an evict
 * costs no store), and nothing else about the cache changes.
 *
 * EROSQ_PRELOAD=<n> overrides N (0 = off, max 6, default 4) for A/B within
 * one binary. */
static int rec_preload_max = -1;

static bool rec_preload_ends_scan(u32 op, bool *is_branch)
{
	*is_branch = false;
	switch (op >> 26) {
	case 0x00:
		switch (op & 0x3f) {
		case 0x08: case 0x09: *is_branch = true; return false; /* JR/JALR */
		case 0x0c: case 0x0d: return true;                     /* SYSCALL/BREAK */
		}
		return false;
	case 0x01: case 0x02: case 0x03: case 0x04:
	case 0x05: case 0x06: case 0x07:
		*is_branch = true; return false;
	case 0x10:                                          /* COP0: MTC0/RFE end blocks */
		return true;
	}
	return false;
}

static void rec_preload_regs(u32 start_pc)
{
	if (rec_preload_max < 0) {
		const char *e = getenv("EROSQ_PRELOAD");
		rec_preload_max = e ? atoi(e) : 4;
		if (rec_preload_max > 6) rec_preload_max = 6;
		if (rec_preload_max < 0) rec_preload_max = 0;
	}
	if (rec_preload_max == 0)
		return;

	u32 written = 1;       /* $zero never needs loading */
	u32 want[6];
	int n = 0;
	bool in_bd = false;
	u32 p = start_pc;

	for (int i = 0; i < 16 && n < rec_preload_max; i++, p += 4) {
		const u32 *ptr = (const u32 *)PSXM(p);
		if (ptr == INVALID_PTR || ptr == NULL)
			break;
		const u32 op = SWAP32(*ptr);
		bool is_branch;
		if (rec_preload_ends_scan(op, &is_branch))
			break;
		const u64 r64 = opcodeGetReads(op);
		if (r64 == ~(u64)0)
			break;          /* unknown opcode: stop, don't guess */
		u32 rd = (u32)r64 & ~written;
		while (rd && n < rec_preload_max) {
			const int b = __builtin_ctz(rd);
			rd &= rd - 1;
			bool dup = false;
			for (int k = 0; k < n; k++)
				if (want[k] == (u32)b) { dup = true; break; }
			if (!dup)
				want[n++] = b;
		}
		written |= (u32)opcodeGetWrites(op);
		if (in_bd)
			break;          /* delay slot done */
		if (is_branch)
			in_bd = true;
	}

	for (int k = 0; k < n; k++) {
		const u32 h = regMipsToHost(want[k], REG_LOAD, REG_REGISTER);
		regUnlock(h);
	}
}

/* ===================================================================
 * EROSQ faultmem: let the MMU do the address range check.
 *
 * A load/store series whose base reg is not known to be RAM/scratchpad
 * normally emits a range check (4-5 insns) + the direct accesses + an
 * inline indirect copy (JAL psxMemRead/Write per access, ~4.7 insns) --
 * the EROSQ_EMITSTAT offline corpus puts range checks at 16.9% and indirect
 * slow paths at 17.6% of all emitted bytes, and the X1000 I$ is capacity-
 * bound. With faultmem, such series are emitted DIRECT-ONLY. RAM (4 mirrors
 * at 0x10000000) and the scratchpad page (0x1f800000) are mapped; every
 * other METHOD-1 host address (HW I/O pages 0x1f801000-0x1f80ffff are now
 * PROT_NONE, BIOS, KSEG2, unmapped space) faults. The SIGSEGV handler
 * decodes the faulting host load/store, performs it through
 * psxMemReadN/psxMemWriteN (exactly what the inline slow path called),
 * writes the result into the saved register, and resumes at pc+4. It then
 * marks the block's guest start PC "slow" and invalidates it, so the block
 * is recompiled with the classic dual path: a spin-polled HW register costs
 * one or two signals per site, not one per access.
 *
 * Constraints kept by the emitter: faultmem accesses are never placed in a
 * host branch delay slot (no exit branch is emitted for direct-only series).
 * Blocks are found from a host PC through fm_tbl (host end address + guest
 * start, appended in emission order, binary-searched); when it is full,
 * new blocks simply don't use faultmem until the next code-cache reset.
 * EROSQ_FAULTMEM=0 turns the whole thing off (classic dual path).
 * =================================================================== */
#define FM_TBL_N  8192
#define FM_SLOW_N 2048
static int  fm_on = -1;
static u32  fm_tbl_end[FM_TBL_N], fm_tbl_pc[FM_TBL_N];
static int  fm_tbl_n;
static u32  fm_slow[FM_SLOW_N];             /* guest block start PCs, 0 = empty */
static u32  fm_exit_stub[8] __attribute__((aligned(32)));
static volatile unsigned long fm_faults, fm_slow_n;
static struct sigaction fm_old_sa;
static bool fm_installed;

static inline u32 fm_hash(u32 pc) { return (pc >> 2) * 2654435761u; }

static bool fm_slow_has(u32 pc)
{
	for (u32 i = fm_hash(pc) % FM_SLOW_N, n = 0; n < FM_SLOW_N; n++, i = (i + 1) % FM_SLOW_N) {
		if (fm_slow[i] == pc) return true;
		if (fm_slow[i] == 0) return false;
	}
	return false;
}

static void fm_slow_add(u32 pc)
{
	if (fm_slow_n >= FM_SLOW_N * 3 / 4) return;   /* keep probes short */
	for (u32 i = fm_hash(pc) % FM_SLOW_N, n = 0; n < FM_SLOW_N; n++, i = (i + 1) % FM_SLOW_N) {
		if (fm_slow[i] == pc) return;
		if (fm_slow[i] == 0) { fm_slow[i] = pc; fm_slow_n++; return; }
	}
}

static int fm_enabled(void)
{
	if (fm_on < 0) {
		const char *e = getenv("EROSQ_FAULTMEM");
		fm_on = (e && e[0] == '0') ? 0 : 1;
	}
	return fm_on;
}

/* guest address for a faulting METHOD-1 host address (host = 0x10000000 |
 * (guest & 0x0fffffff)): HW/BIOS/expansion are identical in KUSEG/KSEG0/1,
 * so the KUSEG form works; 0x1ffexxxx can only be the KSEG2 cache-control
 * window (0xfffe0130). */
static inline u32 fm_guest_addr(u32 h)
{
	return ((h & 0xfffe0000u) == 0x1ffe0000u) ? (0xf0000000u | (h & 0x0fffffffu)) : h;
}

static void fm_chain(int sig, siginfo_t *si, void *uc)
{
	if (fm_old_sa.sa_flags & SA_SIGINFO) {
		if (fm_old_sa.sa_sigaction) { fm_old_sa.sa_sigaction(sig, si, uc); return; }
	} else if (fm_old_sa.sa_handler != SIG_DFL && fm_old_sa.sa_handler != SIG_IGN && fm_old_sa.sa_handler) {
		fm_old_sa.sa_handler(sig); return;
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void fm_sigsegv(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = (ucontext_t *)ucv;
	const uptr pc = (uptr)uc->uc_mcontext.pc;
	if (fm_on <= 0 || pc < (uptr)recMemBase || pc >= (uptr)recMemBase + RECMEM_SIZE || (pc & 3))
		return fm_chain(sig, si, ucv);

	const u32 insn = *(const u32 *)pc;
	const u32 op = insn >> 26, base = (insn >> 21) & 31, rt = (insn >> 16) & 31;
	if (op < 0x20 || op > 0x2e || op == 0x27 || op == 0x2c || op == 0x2d)
		return fm_chain(sig, si, ucv);
	const u32 h = (u32)uc->uc_mcontext.gregs[base] + (u32)(s32)(s16)insn;
	if (h < RESV_LO || h >= RESV_HI)   /* anything a fast path can reach */
		return fm_chain(sig, si, ucv);

	const u32 g = fm_guest_addr(h);
	const u32 r = (u32)uc->uc_mcontext.gregs[rt];
	const u32 sh = (g & 3) * 8;
	u32 v = 0;
	bool load = true;
	switch (op) {
	case 0x20: v = (u32)(s32)(s8)psxMemRead8(g); break;           /* LB  */
	case 0x24: v = psxMemRead8(g) & 0xff; break;                    /* LBU */
	case 0x21: v = (u32)(s32)(s16)psxMemRead16(g); break;         /* LH  */
	case 0x25: v = psxMemRead16(g) & 0xffff; break;                 /* LHU */
	case 0x23: v = psxMemRead32(g); break;                          /* LW  */
	case 0x22: {                                                    /* LWL */
		const u32 w = psxMemRead32(g & ~3u);
		v = (w << (24 - sh)) | (r & (0x00ffffffu >> sh));
		break; }
	case 0x26: {                                                    /* LWR */
		const u32 w = psxMemRead32(g & ~3u);
		v = (w >> sh) | (sh ? (r & (0xffffff00u << (24 - sh))) : 0);
		break; }
	/* SB/SH pass the FULL register, exactly like the inline slow path
	 * (MOV a1, rt) and the interpreter: psxHwWrite16/8 'forced write32'
	 * ports (0x1014, 0x1060, DMA MADR...) store all 32 bits. */
	case 0x28: load = false; psxMemWrite8(g, r); break;             /* SB  */
	case 0x29: load = false; psxMemWrite16(g, r); break;            /* SH  */
	case 0x2b: load = false; psxMemWrite32(g, r); break;            /* SW  */
	case 0x2a: {                                                    /* SWL */
		load = false;
		const u32 w = psxMemRead32(g & ~3u);
		psxMemWrite32(g & ~3u, (sh == 24 ? 0 : (w & (0xffffff00u << sh))) | (r >> (24 - sh)));
		break; }
	case 0x2e: {                                                    /* SWR */
		load = false;
		const u32 w = psxMemRead32(g & ~3u);
		psxMemWrite32(g & ~3u, (sh ? (w & (0x00ffffffu >> (24 - sh))) : 0) | (r << sh));
		break; }
	default:
		return fm_chain(sig, si, ucv);
	}
	if (load && rt)
		uc->uc_mcontext.gregs[rt] = (greg_t)(s32)v;
	uc->uc_mcontext.pc = pc + 4;
	fm_faults++;
	{
		static int dbg = -1;
		if (dbg < 0) dbg = getenv("EROSQ_FAULTMEM_DEBUG") ? 1 : 0;
		if (dbg) {
			char b[96];
			int n = snprintf(b, sizeof b, "faultmem: host %08x insn %08x guest %08x\n",
					 (u32)pc, insn, g);
			if (n > 0) { ssize_t w = write(2, b, n); (void)w; }
		}
	}

	/* recompile the block with the classic dual path */
	int lo = 0, hi = fm_tbl_n;
	while (lo < hi) {                      /* first entry with end > pc */
		const int mid = (lo + hi) / 2;
		if (fm_tbl_end[mid] > (u32)pc) hi = mid; else lo = mid + 1;
	}
	if (lo < fm_tbl_n) {
		const u32 gpc = fm_tbl_pc[lo];
		fm_slow_add(gpc);
		*(u32 *)PC_REC(gpc) = 0;
		/* a block that returns to the dispatcher's self-loop fastpath
		 * never looks itself up again: send that return through the stub,
		 * which re-enters the main loop (lookup -> recompile). */
		if (block_fast_ret_addr) {
			u32 *slot = (u32 *)((uptr)uc->uc_mcontext.gregs[29] + 16);
			*slot = (u32)(uptr)fm_exit_stub;
		}
	}
}

static void fm_install(void)
{
	if (fm_installed || !fm_enabled() || !psx_mem_mapped)
		return;
	/* stub: v0 = psxRegs.pc, v1 = 0 (cycles already added), j main loop */
	u32 *save = recMem;
	recMem = fm_exit_stub;
	LUI(MIPSREG_T0, ADR_HI(&block_ret_addr));
	LW(MIPSREG_T0, MIPSREG_T0, ADR_LO(&block_ret_addr));
	LW(MIPSREG_V0, PERM_REG_1, off(pc));
	JR(MIPSREG_T0);
	MOV(MIPSREG_V1, 0);
	recMem = save;
	clear_insn_cache(fm_exit_stub, fm_exit_stub + 8, 0);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = fm_sigsegv;
	sa.sa_flags = SA_SIGINFO;        /* NOT SA_ONSTACK: psxMemWrite may run DMA/GPU work */
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, &fm_old_sa) == 0) {
		fm_installed = true;
		printf("mipsrec: faultmem on (EROSQ_FAULTMEM=0 disables)\n");
	} else {
		fm_on = 0;
	}
}

extern "C" void mipsrec_faultmem_stats(unsigned long *faults, unsigned long *slow_blocks, int *tbl)
{
	*faults = fm_faults; *slow_blocks = fm_slow_n; *tbl = fm_tbl_n;
}

static void recRecompile()
{
	// Notify plugin_lib that we're recompiling (affects frameskip timing)
	pl_dynarec_notify();

	/* ponytail: EROSQ_RECSTAT=1 reports how much code cache a game actually
	 * uses and how often it wraps -- the two numbers needed to decide whether
	 * RECMEM_SIZE (12MB) can be shrunk on a 24MB device. Diagnostic only. */
	{
		static int recstat = -1;
		static unsigned long resets;
		static size_t peak;
		size_t used = (uptr)recMem - (uptr)recMemBase;
		if (recstat < 0) recstat = getenv("EROSQ_RECSTAT") ? 1 : 0;
		if (recstat && used > peak) {
			peak = used;
			if ((peak & ~0xfffffu) != ((peak - 1) & ~0xfffffu))   /* each new MB */
				fprintf(stderr, "RECSTAT peak=%uKB resets=%lu\n",
					(unsigned)(peak >> 10), resets);
		}
		if (used >= RECMEM_SIZE_MAX) {
			resets++;
			if (recstat)
				fprintf(stderr, "RECSTAT cache full -> flush #%lu\n", resets);
			REC_LOG("Code cache size limit exceeded: flushing code cache.\n");
			recReset();
		}
	}

	recMemStart = recMem;

	regReset();

	PC_REC32(psxRegs.pc) = (u32)recMem;
	oldpc = pc = psxRegs.pc;

	// If 'pc' is in PS1 RAM, mark the page of RAM as containing the start of
	//  a block. For the range check, bit 27 is interpreted as a sign bit.
	if ((s32)(pc << 4) >= 0) {
		u32 masked_pc = pc & 0x1fffff;
		code_pages[masked_pc/4096/8] |= (1 << ((masked_pc/4096) & 7));
	}

	DISASM_INIT();

	rec_recompile_start();

	// Reset const-propagation
	ResetConsts();

	// Flag indicates when recompilation should stop
	end_block = false;

	// See convertMultiplyTo3Op() and recMFLO() in rec_mdu.cpp.h
	skip_emitting_next_mflo = false;

	// Flag indicates when values are cached by load/store emitters in $at,$v1
	lsu_tmp_cache_valid = false;

	// Flag indicates when a PC value is cached in $v0. All dispatch loops set
	//  $v0 to block start PC before entry. See rec_bcu.cpp.h
	host_v0_reg_is_const = true;
	host_v0_reg_constval = psxRegs.pc;

	// Flag indicates when $ra holds block return address. This is only used
	//  by blocks returning indirectly. The indirect-return dispatch loops
	//  set $ra before block entry. See rec_recompile_end_part1().
	host_ra_reg_has_block_retaddr = (block_ret_addr == 0);

	// Number of discardable instructions we are currently skipping
	int discard_cnt = 0;

	// EROSQ: issue the first basic block's guest-reg loads up front
	faultmem_block = fm_installed && fm_on > 0 && fm_tbl_n < FM_TBL_N && !fm_slow_has(pc);
	faultmem_block_used = false;

	u8 *es_blk0 = (u8 *)recMem;
	const u32 es_pc0 = pc;
	rec_preload_regs(pc);
	if (emitstat_enabled()) es_prologue += (u8 *)recMem - es_blk0;

	do {
		// Flag indicates if next instruction lies in a BD slot
		branch = false;

		psxRegs.code = OPCODE_AT(pc);

#ifdef USE_CODE_DISCARD
		// If we are not already skipping past discardable code, scan
		//  for PS1 code sequence we can discard.
		if (discard_cnt == 0) {
			int discard_type = 0;
			discard_cnt = rec_discard_scan(pc, &discard_type);
			if (discard_cnt > 0)
				DISASM_MSG(" ->BEGIN code discard: %s\n", rec_discard_type_str(discard_type));
		}
#endif

		DISASM_PSX(pc);
		pc += 4;

#ifdef USE_CODE_DISCARD
		// Skip next discardable instruction in any sequence found.
		if (discard_cnt > 0) {
			--discard_cnt;
			if (discard_cnt == 0)
				DISASM_MSG(" ->END code discard.\n");
			continue;
		}
#endif

		// Recompile next instruction.
		if (__builtin_expect(es_on > 0, 0)) {
			const u32 es_op = psxRegs.code, es_ipc = pc;
			u8 *es_b = (u8 *)recMem;
			recBSC[es_op>>26]();
			const int c = es_classify(es_op);
			es_bytes[c] += (u8 *)recMem - es_b;
			es_calls[c]++;
			es_guest[c] += (pc - es_ipc) / 4 + 1;  /* series/branches consume more */
		} else
			recBSC[psxRegs.code>>26]();
		regUpdate();
	} while (!end_block);

	if (faultmem_block_used) {
		fm_tbl_end[fm_tbl_n] = (u32)(uptr)recMem;
		fm_tbl_pc[fm_tbl_n] = oldpc;
		fm_tbl_n++;
	}

	if (es_on > 0) {
		es_blocks++;
		es_block_bytes += (u8 *)recMem - es_blk0;
		es_block_guest += (pc - es_pc0) / 4;
		if ((es_blocks & 4095) == 0)
			mipsrec_emitstat_dump_(stderr);
	}

	DISASM_HOST();
	clear_insn_cache(recMemStart, recMem, 0);
}


static int recInit()
{
	REC_LOG("Initializing\n");

	recMem = (u32*)recMemBase;

	/* ponytail: the upstream pcsx4all code did
	 *     memset(recMemBase, 0xff, RECMEM_SIZE_MAX + 0x1000);
	 * here, to "allocate the RAM we need in advance" and to make an accidental
	 * jump into not-yet-emitted cache trap on an illegal opcode. On this 24MB
	 * device that prefault is actively harmful: it dirties 11.5MB of BSS at
	 * startup, all of which lands in the 16MB zram swap and stays there for the
	 * process's life (measured: VmSwap 12MB, MemAvailable pinned at 0 with GT2
	 * loaded, which is one allocation away from an OOM kill). Left as demand-zero
	 * the cache costs only what the recompiler actually emits.
	 * The 0xff trap property is not worth 11.5MB here, and it was already only
	 * half-true: recReset() rewinds recMem without refilling, so after the first
	 * cache wrap the tail holds stale code rather than 0xff anyway. */

	// The tables recRAM and recROM hold block code pointers for all valid PC
	//  values for a PS1 program, after masking away banking and/or mirroring.
	//  Originally, the dynarec used a LUT, psxRecLUT[], to accomplish this
	//  masking, which added a second layer of indirection to code lookups.
	//  Instead, we now virtually map/mirror recRAM and map recROM.
	//  This offloads the indirection onto the host's TLB.
	//  NOTE: psxRecLUT[] still is used inside recExecuteBlock(): no sense in
	//        making two versions of a function that hardly gets any real use.
#ifdef USE_VIRTUAL_RECRAM_MAPPING
	if (!rec_mem_mapped && !recRAM && !recROM) {
		if (rec_mmap_rec_mem() >= 0) {
			rec_mem_mapped = true;
			recRAM = (s8*)REC_RAM_VADDR;
			recROM = (s8*)REC_ROM_VADDR;
		}
	}
#endif

	if (!rec_mem_mapped) {
		// Oops, somehow the virtual mapping/mirroring failed. We'll allocate
		//  recRAM/recROM here and use slower versions of recExecute*() that
		//  use psxRecLUT[] to indirectly access recRAM/recROM.
		recRAM = (s8*)malloc(REC_RAM_SIZE);
		recROM = (s8*)malloc(REC_ROM_SIZE);
		printf("WARNING: Recompiler is using slower non-virtual block ptr lookups.\n");
	}

	recReset();

	if (recRAM == NULL || recROM == NULL || recMemBase == NULL || psxRecLUT == NULL) {
		printf("Error allocating memory\n"); return -1;
	}

	for (int i = 0; i < 0x80; i++)
		psxRecLUT[i + 0x0000] = (uptr)recRAM + (((i & 0x1f) << 16) * (REC_RAM_PTR_SIZE/4));

	memcpy(&psxRecLUT[0x8000], psxRecLUT, 0x80 * sizeof(psxRecLUT[0]));
	memcpy(&psxRecLUT[0xa000], psxRecLUT, 0x80 * sizeof(psxRecLUT[0]));

	for (int i = 0; i < 0x08; i++)
		psxRecLUT[i + 0xbfc0] = (uptr)recROM + ((i << 16) * (REC_RAM_PTR_SIZE/4));

	// Map/mirror PSX RAM, other regions, i.e. psxM, psxP, psxH
	// NOTE: if mapping fails or isn't enabled at compile-time, PSX mem will be
	//       allocated using traditional methods in psxmem.cpp
#ifdef USE_VIRTUAL_PSXMEM_MAPPING
	if (!psx_mem_mapped)
		psx_mem_mapped = (rec_mmap_psx_mem() >= 0);
#endif

	if (!psx_mem_mapped)
		printf("WARNING: Recompiler is emitting slower non-virtual mem access code.\n");

	return 0;
}


static void recShutdown()
{
	REC_LOG("Shutting down\n");

	if (psx_mem_mapped)
		rec_munmap_psx_mem();
	if (rec_mem_mapped)
		rec_munmap_rec_mem();
	psx_mem_mapped = rec_mem_mapped = false;
}


/* It seems there's no way to tell GCC that something is being called inside
 * asm() blocks and GCC doesn't bother to save temporaries to stack.
 * That's why we have two options:
 * 1. Call recompiled blocks via recFunc() trap which is strictly noinline and
 * saves registers $s[0-7], $fp and $ra on each call, or
 * 2. Code recExecute() and recExecuteBlock() entirely in assembler taking into
 * account that no registers except $ra are saved in recompiled blocks and
 * thus put all temporaries to stack. In this case $s[0-7], $fp and $ra are saved
 * in recExecute() and recExecuteBlock() only once.
 *
 * IMPORTANT: Functions containing inline ASM should have attribute 'noinline'.
 *            Crashes at callsites can occur otherwise, at least with GCC 4.xx.
 */
#ifndef ASM_EXECUTE_LOOP
__attribute__((noinline)) static void recFunc(void *fn)
{
	/* This magic code calls fn address saving registers $s[0-7], $fp and $ra. */
	/*                                                                         */
	/* Focus here is on clarity, not speed. This is the bare minimum needed to */
	/*  call blocks from within C code, which is:                              */
	/* Blocks expect $fp to be set to &psxRegs.                                */
	/* Blocks expect return address to be stored at 16($sp) and in $ra.        */
	/* Blocks expect $v0 to contain psxRegs.pc value (helps addr generation).  */
	/* Stack should have 16 bytes free at 0($sp) for use by called functions.  */
	/* Stack should be 8-byte aligned to satisfy MIPS ABI.                     */
	/*                                                                         */
	/* Blocks return these values, which are handled here:                     */
	/*  $v0 is new value for psxRegs.pc                                        */
	/*  $v1 is number of cycles to increment psxRegs.cycle by                  */
	__asm__ __volatile__ (
		"addiu  $sp, $sp, -24                   \n"
		"la     $fp, %[psxRegs]                 \n" // $fp = &psxRegs
		"lw     $v0, %[psxRegs_pc_off]($fp)     \n" // Blocks expect $v0 to contain PC val on entry
		"la     $ra, block_return_addr%=        \n" // Load $ra with block_return_addr
		"sw     $ra, 16($sp)                    \n" // Put 'block_return_addr' on stack
		"jr     %[fn]                           \n" // Execute block

		"block_return_addr%=:                   \n"
		"sw     $v0, %[psxRegs_pc_off]($fp)     \n" // psxRegs.pc = $v0
		"lw     $t1, %[psxRegs_cycle_off]($fp)  \n" //
		"addu   $t1, $t1, $v1                   \n" //
		"sw     $t1, %[psxRegs_cycle_off]($fp)  \n" // psxRegs.cycle += $v1
		"addiu  $sp, $sp, 24                    \n"
		: // Output
		: // Input
		  [fn]                   "r" (fn),
		  [psxRegs]              "i" (&psxRegs),
		  [psxRegs_pc_off]       "i" (off(pc)),   // Offset of psxRegs.pc in psxRegs
		  [psxRegs_cycle_off]    "i" (off(cycle)) // Offset of psxRegs.cycle in psxRegs
		: // Clobber - No need to list anything but 'saved' regs
		  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "fp", "ra", "memory"
	);
}
#endif


/* Execute blocks starting at psxRegs.pc
 * Blocks return indirectly, to address stored at 16($sp)
 * Block pointers are looked up using psxRecLUT[].
 * Called only from recExecute(), see notes there.
 *
 * IMPORTANT: Functions containing inline ASM should have attribute 'noinline'.
 *            Crashes at callsites can occur otherwise, at least with GCC 4.xx.
 */
__attribute__((noinline)) void recExecute_indirect_return_lut()
{
	// Set block_ret_addr to 0, so generated code uses indirect returns
	block_ret_addr = block_fast_ret_addr = 0;

#ifndef ASM_EXECUTE_LOOP
	for (;;) {
		u32 *p = (u32*)PC_REC(psxRegs.pc);
		if (*p == 0)
			recRecompile();

		recFunc((void *)*p);

		if ((s32)(psxRegs.cycle - psxRegs.io_cycle_counter) >= 0) // wrap-safe (see subu/bgez in asm loop)
			mipsrec_branch_test();
	}
#else
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set push                                    \n"
".set noreorder                               \n"

// $fp/$s8 remains set to &psxRegs across all calls to blocks
"la    $fp, %[psxRegs]                        \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                  24        \n"
".equ  f_off_temp_var1,             20        \n"
".equ  f_off_block_ret_addr,        16        \n" // NOTE: blocks assume this is at 16($sp)!
"addiu $sp, $sp, -frame_size                  \n"

// Store const block return address at fixed location in stack frame
"la    $t0, loop%=                            \n"
"sw    $t0, f_off_block_ret_addr($sp)         \n"

// Load $v0 once with psxRegs.pc, blocks will assign new value when returning
"lw    $v0, %[psxRegs_pc_off]($fp)            \n"

// Load $v1 once with zero. It is the # of cycles psxRegs.cycle should be
// incremented by when a block returns.
"move  $v1, $0                                \n"

// Align loop on cache-line boundary
".balign 32                                   \n"

////////////////////////////
//       LOOP CODE:       //
////////////////////////////

// NOTE: Blocks return to top of loop.
// NOTE: Loop expects following values to be set:
// $v0 = new value for psxRegs.pc
// $v1 = # of cycles to increment psxRegs.cycle by

// The loop pseudocode is this, interleaving ops to reduce load stalls:
//
// loop:
// $t2 = psxRecLUT[pxsRegs.pc >> 16] + (psxRegs.pc & 0xffff)
// $t0 = *($t2)
// psxRegs.cycle += $v1
// if (psxRegs.cycle >= psxRegs.io_cycle_counter)
//    goto call_psxBranchTest;
// psxRegs.pc = $v0
// if ($t0 == 0)
//    goto recompile_block;
// $ra = block return address
// goto $t0;
// /* Code at addr $t0 will run and return having set $v0 to new psxRegs.pc */
// /*  value and $v1 to the number of cycles to increment psxRegs.cycle by. */

// Infinite loop, blocks return here
"loop%=:                                      \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lui   $t1, %%hi(%[psxRecLUT])                \n"
"srl   $t2, $v0, 16                           \n"
"sll   $t2, $t2, 2                            \n" // sizeof() psxRecLUT[] elements is 4
"addu  $t1, $t1, $t2                          \n"
"lw    $t1, %%lo(%[psxRecLUT])($t1)           \n" // $t1 = psxRecLUT[psxRegs.pc >> 16]
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter
"addu  $t3, $t3, $v1                          \n" // $t3 = psxRegs.cycle + $v1
"andi  $t0, $v0, 0xffff                       \n"
"addu  $t2, $t0, $t1                          \n"
"lw    $t0, 0($t2)                            \n" // $t0 = address of start of block code, or
                                                  //       or 0 if block needs recompilation
                                                  // IMPORTANT: leave block ptr in $t2, it gets
                                                  // saved & re-used if recRecompile() is called.


// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest%=              \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Use BD slot to store new psxRegs.pc val

// Execute already-compiled block. It will return at top of loop.
"execute_block%=:                             \n"
"jr    $t0                                    \n"
"lw    $ra, 16($sp)                           \n" // <BD> Load block return address

////////////////////////////
//     NON-LOOP CODE:     //
////////////////////////////

// Call psxBranchTest() and go back to top of loop
"call_psxBranchTest%=:                        \n"
"jal   %[psxBranchTest]                       \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Use BD slot to store new psxRegs.pc val,
"lbu   $t0, %[psxRegs_stop_off]($fp)          \n" // .stop: frontend requests break (menu/save/quit)
"bnez  $t0, exit%=                            \n" //  leave dispatch loop so Execute() returns to C
"nop                                          \n" // <BD>
                                                  //  as psxBranchTest() might issue an exception.
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // After psxBranchTest() returns, load psxRegs.pc
                                                  //  back into $v0, which could be different than
                                                  //  before the call if an exception was issued.
"b     loop%=                                 \n" // Go back to top to process psxRegs.pc again..
"move  $v1, $0                                \n" // <BD> ..using BD slot to set $v1 to 0, since
                                                  //  psxRegs.cycle shouldn't be incremented again.

// Recompile block and return to normal codepath.
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block ptr across call
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block ptr upon return
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // Blocks expect $v0 to contain PC val on entry
"b     execute_block%=                        \n" // Resume normal code path, but first we must..
"lw    $t0, 0($t2)                            \n" // <BD> ..load $t0 with ptr to block code

// Destroy stack frame, exiting inlined ASM block
// NOTE: We'd never reach this point because the block dispatch loop is
//  currently infinite. This could change in the future.
// TODO: Could add a way to reset a game or load a new game from within
//  the running emulator by setting a global boolean, resetting
//  psxRegs.io_cycle_counter to 0, and checking if it's been set before
//  calling pxsBranchTest() here. If set, you must jump here to
//  exit the loop, to ensure that stack frame is adjusted before return!
"exit%=:                                      \n"
"addiu $sp, $sp, frame_size                   \n"
".set pop                                     \n"

: // Output
: // Input
  [psxRegs]                    "i" (&psxRegs),
  [psxRegs_pc_off]             "i" (off(pc)),
  [psxRegs_cycle_off]          "i" (off(cycle)),
  [psxRegs_io_cycle_ctr_off]   "i" (off(io_cycle_counter)),
  [recRecompile]               "i" (&recRecompile),
  [psxBranchTest]              "i" (&mipsrec_branch_test),
  [psxRegs_stop_off]           "i" (off(stop)),
  [psxRecLUT]                  "i" (psxRecLUT)
: // Clobber - No need to list anything but 'saved' regs
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "fp", "ra", "memory"
);
#endif
}


/* Execute blocks starting at psxRegs.pc
 * Blocks return indirectly, to address stored at 16($sp)
 * Block pointers are looked up using virtual address mapping of recRAM/recROM.
 * Called only from recExecute(), see notes there.
 *
 * IMPORTANT: Functions containing inline ASM should have attribute 'noinline'.
 *            Crashes at callsites can occur otherwise, at least with GCC 4.xx.
 */
__attribute__((noinline)) static void recExecute_indirect_return_mmap()
{
	// Set block_ret_addr to 0, so generated code uses indirect returns
	block_ret_addr = block_fast_ret_addr = 0;

#ifndef ASM_EXECUTE_LOOP
	for (;;) {
		u32 *p = (u32*)PC_REC(psxRegs.pc);
		if (*p == 0)
			recRecompile();

		recFunc((void *)*p);

		if ((s32)(psxRegs.cycle - psxRegs.io_cycle_counter) >= 0) // wrap-safe (see subu/bgez in asm loop)
			mipsrec_branch_test();
	}
#else
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set push                                    \n"
".set noreorder                               \n"

// $fp/$s8 remains set to &psxRegs across all calls to blocks
"la    $fp, %[psxRegs]                        \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                  24        \n"
".equ  f_off_temp_var1,             20        \n"
".equ  f_off_block_ret_addr,        16        \n" // NOTE: blocks assume this is at 16($sp)!
"addiu $sp, $sp, -frame_size                  \n"

// Store const block return address at fixed location in stack frame
"la    $t0, loop%=                            \n"
"sw    $t0, f_off_block_ret_addr($sp)         \n"

// Load $v0 once with psxRegs.pc, blocks will assign new value when returning
"lw    $v0, %[psxRegs_pc_off]($fp)            \n"

// Load $v1 once with zero. It is the # of cycles psxRegs.cycle should be
// incremented by when a block returns.
"move  $v1, $0                                \n"

// Align loop on cache-line boundary
".balign 32                                   \n"

////////////////////////////
//       LOOP CODE:       //
////////////////////////////

// NOTE: Blocks return to top of loop.
// NOTE: Loop expects following values to be set:
// $v0 = new value for psxRegs.pc
// $v1 = # of cycles to increment psxRegs.cycle by

// The loop pseudocode is this, interleaving ops to reduce load stalls:
//
// loop:
// $t2 = REC_RAM_VADDR | ($v0 & 0x00ffffff)
// $t0 = *($t2)
// psxRegs.cycle += $v1
// if (psxRegs.cycle >= psxRegs.io_cycle_counter)
//    goto call_psxBranchTest;
// psxRegs.pc = $v0
// if ($t0 == 0)
//    goto recompile_block;
// $ra = block return address
// goto $t0;
// /* Code at addr $t0 will run and return having set $v0 to new psxRegs.pc */
// /*  value and $v1 to the number of cycles to increment psxRegs.cycle by. */

// Infinite loop, blocks return here
"loop%=:                                      \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter

// The block ptrs are mapped virtually to address space allowing lower
//  24 bits of PS1 PC address to lookup start of any RAM or ROM code block.
"lui   $t2, %[REC_RAM_VADDR_UPPER]            \n"
#ifdef HAVE_MIPS32R2_EXT_INS
"ins   $t2, $v0, 0, 24                        \n"
#else
"sll   $t1, $v0, 8                            \n"
"srl   $t1, $t1, 8                            \n"
"or    $t2, $t2, $t1                          \n"
#endif
"lw    $t0, 0($t2)                            \n" // $t0 = address of start of block code, or
                                                  //       or 0 if block needs recompilation
                                                  // IMPORTANT: leave block ptr in $t2, it gets
                                                  // saved & re-used if recRecompile() is called.

"addu  $t3, $t3, $v1                          \n" // $t3 = psxRegs.cycle + $v1

// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest%=              \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Use BD slot to store new psxRegs.pc val

// Execute already-compiled block. It will return at top of loop.
"execute_block%=:                             \n"
"jr    $t0                                    \n"
"lw    $ra, 16($sp)                           \n" // <BD> Load block return address

////////////////////////////
//     NON-LOOP CODE:     //
////////////////////////////

// Call psxBranchTest() and go back to top of loop
"call_psxBranchTest%=:                        \n"
"jal   %[psxBranchTest]                       \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Use BD slot to store new psxRegs.pc val,
"lbu   $t0, %[psxRegs_stop_off]($fp)          \n" // .stop: frontend requests break (menu/save/quit)
"bnez  $t0, exit%=                            \n" //  leave dispatch loop so Execute() returns to C
"nop                                          \n" // <BD>
                                                  //  as psxBranchTest() might issue an exception.
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // After psxBranchTest() returns, load psxRegs.pc
                                                  //  back into $v0, which could be different than
                                                  //  before the call if an exception was issued.
"b     loop%=                                 \n" // Go back to top to process psxRegs.pc again..
"move  $v1, $0                                \n" // <BD> ..using BD slot to set $v1 to 0, since
                                                  //  psxRegs.cycle shouldn't be incremented again.

// Recompile block and return to normal codepath.
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block ptr across call
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block ptr upon return
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // Blocks expect $v0 to contain PC val on entry
"b     execute_block%=                        \n" // Resume normal code path, but first we must..
"lw    $t0, 0($t2)                            \n" // <BD> ..load $t0 with ptr to block code

// Destroy stack frame, exiting inlined ASM block
// NOTE: We'd never reach this point because the block dispatch loop is
//  currently infinite. This could change in the future.
// TODO: Could add a way to reset a game or load a new game from within
//  the running emulator by setting a global boolean, resetting
//  psxRegs.io_cycle_counter to 0, and checking if it's been set before
//  calling pxsBranchTest() here. If set, you must jump here to
//  exit the loop, to ensure that stack frame is adjusted before return!
"exit%=:                                      \n"
"addiu $sp, $sp, frame_size                   \n"
".set pop                                     \n"

: // Output
: // Input
  [psxRegs]                    "i" (&psxRegs),
  [psxRegs_pc_off]             "i" (off(pc)),
  [psxRegs_cycle_off]          "i" (off(cycle)),
  [psxRegs_io_cycle_ctr_off]   "i" (off(io_cycle_counter)),
  [recRecompile]               "i" (&recRecompile),
  [psxBranchTest]              "i" (&mipsrec_branch_test),
  [psxRegs_stop_off]           "i" (off(stop)),
  [REC_RAM_VADDR_UPPER]        "i" (REC_RAM_VADDR >> 16)
: // Clobber - No need to list anything but 'saved' regs
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "fp", "ra", "memory"
);
#endif
}


/* Execute blocks starting at psxRegs.pc
 * Blocks return directly (not indirectly via stack address var).
 * Block pointers are looked up using psxRecLUT[].
 * Called only from recExecute(), see notes there.

 * IMPORTANT: Functions containing inline ASM should have attribute 'noinline'.
 *            Crashes at callsites can occur otherwise, at least with GCC 4.xx.
 */
__attribute__((noinline)) static void recExecute_direct_return_lut()
{
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set push                                    \n"
".set noreorder                               \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                        32  \n"
".equ  f_off_branchtest_fastpath_ra,      28  \n"
".equ  f_off_temp_var1,                   24  \n"
".equ  f_off_block_start_addr,            16  \n"
"addiu $sp, $sp, -frame_size                  \n"

#ifdef USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS
// Set the global var 'block_fast_ret_addr'.
"la    $t0, fastpath_loop%=                   \n"
"lui   $t1, %%hi(%[block_fast_ret_addr])      \n"
"sw    $t0, %%lo(%[block_fast_ret_addr])($t1) \n"

// We need a stack var to hold a return address for a 'fastpath' call to
//  psxBranchTest. This helps reduce size of code through sharing.
"la    $t0, branchtest_fastpath_retaddr%=     \n"
"sw    $t0, f_off_branchtest_fastpath_ra($sp) \n"

#endif

// Set the global var 'block_ret_addr'.
"la    $t0, loop%=                            \n"
"lui   $t1, %%hi(%[block_ret_addr])           \n"
"sw    $t0, %%lo(%[block_ret_addr])($t1)      \n"

// $fp/$s8 remains set to &psxRegs across all calls to blocks
"la    $fp, %[psxRegs]                        \n"

// Load $v0 once with psxRegs.pc, blocks will assign new value when returning
"lw    $v0, %[psxRegs_pc_off]($fp)            \n"

// Load $v1 once with zero. It is the # of cycles psxRegs.cycle should be
// incremented by when a block returns.
"move  $v1, $0                                \n"

// Align loop on cache-line boundary
".balign 32                                   \n"

////////////////////////////
//       LOOP CODE:       //
////////////////////////////

// NOTE: Blocks return to top of loop.
// NOTE: Loop expects following values to be set:
// $v0 = new value for psxRegs.pc
// $v1 = # of cycles to increment psxRegs.cycle by

// The loop pseudocode is this, interleaving ops to reduce load stalls:
//
// loop:
// $t2 = psxRecLUT[pxsRegs.pc >> 16] + (psxRegs.pc & 0xffff)
// $t0 = *($t2)
// psxRegs.cycle += $v1
// if (psxRegs.cycle >= psxRegs.io_cycle_counter)
//    goto call_psxBranchTest;
// psxRegs.pc = $v0
// if ($t0 == 0)
//    goto recompile_block;
// tmp_block_start_addr = $t0
// goto $t0;
// /* Code at addr $t0 will run and return having set $v0 to new psxRegs.pc */
// /*  value and $v1 to the number of cycles to increment psxRegs.cycle by. */
// /* If block branches back to its beginning, it will return to the        */
// /*  fastpath version of loop that skips looking up a code pointer.       */

// Infinite loop, blocks return here
"loop%=:                                      \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lui   $t1, %%hi(%[psxRecLUT])                \n"
"srl   $t2, $v0, 16                           \n"
"sll   $t2, $t2, 2                            \n" // sizeof() psxRecLUT[] elements is 4
"addu  $t1, $t1, $t2                          \n"
"lw    $t1, %%lo(%[psxRecLUT])($t1)           \n" // $t1 = psxRecLUT[psxRegs.pc >> 16]
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter
"addu  $t3, $t3, $v1                          \n" // $t3 = psxRegs.cycle + $v1
"andi  $t0, $v0, 0xffff                       \n"
"addu  $t2, $t0, $t1                          \n"
"lw    $t0, 0($t2)                            \n" // $t0 = address of start of block code, or
                                                  //       or 0 if block needs recompilation
                                                  // IMPORTANT: leave block ptr in $t2, it gets
                                                  // saved & re-used if recRecompile() is called.


// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest%=              \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Use BD slot to store new psxRegs.pc val

// Execute already-compiled block. It returns to top of 'fastpath' loop if it
//  jumps to its own beginning PC. Otherwise, it returns to top of main loop.
"execute_block%=:                             \n"
"jr    $t0                                    \n"
#ifdef USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS
"sw    $t0, f_off_block_start_addr($sp)       \n" // <BD> Save code address, in case block jumps back
                                                  //      to its beginning and takes 'fastpath'
#else
"nop                                          \n" // <BD>
#endif

////////////////////////////
//   BRANCH-TEST CODE:    //
////////////////////////////

// Call psxBranchTest() and go back to top of loop
"call_psxBranchTest%=:                        \n"
"jal   %[psxBranchTest]                       \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Store new psxRegs.pc val before calling C
"branchtest_fastpath_retaddr%=:               \n" // Next 3 instructions shared with 'fastpath' code..
"lbu   $t0, %[psxRegs_stop_off]($fp)          \n" // .stop: frontend requests break (menu/save/quit)
"bnez  $t0, exit%=                            \n" //  leave dispatch loop so Execute() returns to C
"nop                                          \n" // <BD>
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // After psxBranchTest() returns, load psxRegs.pc
                                                  //  back into $v0, which could be different than
                                                  //  before the call if an exception was issued.
"b     loop%=                                 \n" // Go back to top to process new psxRegs.pc value..
"move  $v1, $0                                \n" // <BD> ..using BD slot to set $v1 to 0, since
                                                  //  psxRegs.cycle shouldn't be incremented again.

#ifdef USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS
////////////////////////////
//   FASTPATH LOOP CODE:  //
////////////////////////////

// When a block's new PC (jump target) is the same as its own beginning PC,
//  i.e. it branches back to its own top, it will return to this 'fast path'
//  version of above loop. It assumes that the block is already compiled and
//  unmodified, saving cycles versus the general loop. The main loop was
//  careful to save the block start address at location in stack frame.
//  NOTE: Blocks returning this way don't bother to set $v0 to any PC value.
"fastpath_loop%=:                             \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter
"lw    $t0, f_off_block_start_addr($sp)       \n" // Load block code addr saved in main loop
"addu  $t3, $t3, $v1                          \n" // $t3 = psxRegs.cycle + $v1

// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest_fastpath%=     \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Execute already-compiled block. It returns to top of 'fastpath' loop if it
//  jumps to its own beginning PC. Otherwise, it returns to top of main loop.
"jr    $t0                                    \n"
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Blocks expect $v0 to contain PC val on entry

////////////////////////////
//   BRANCH-TEST CODE:    //
////////////////////////////

// Call psxBranchTest(). We don't have a new PC in $v0 to store, so call it
//  directly, and have it return to code shared with main 'call_psxBranchTest'.
"call_psxBranchTest_fastpath%=:               \n"
"j     %[psxBranchTest]                       \n"
"lw    $ra, f_off_branchtest_fastpath_ra($sp) \n" // <BD>


#endif // USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS

////////////////////////////
// RECOMPILE BLOCK CODE:  //
////////////////////////////

// Recompile block and return to normal codepath.
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block ptr across call
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block ptr upon return
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // Blocks expect $v0 to contain PC val on entry
"b     execute_block%=                        \n" // Resume normal code path, but first we must..
"lw    $t0, 0($t2)                            \n" // <BD> ..load $t0 with ptr to block code


// Destroy stack frame, exiting inlined ASM block
// NOTE: During block executions, we'd never reach this point because the block
//  dispatch loop is currently infinite. This could change in the future.
// TODO: Could add a way to reset a game or load a new game from within
//  the running emulator by setting a global boolean, resetting
//  psxRegs.io_cycle_counter to 0, and checking if it's been set before
//  calling pxsBranchTest() here. If set, you must jump here to
//  exit the loop, to ensure that stack frame is adjusted before return!
"exit%=:                                      \n"
"addiu $sp, $sp, frame_size                   \n"
".set pop                                     \n"

: // Output
: // Input
  [block_ret_addr]             "i" (&block_ret_addr),
  [block_fast_ret_addr]        "i" (&block_fast_ret_addr),
  [psxRegs]                    "i" (&psxRegs),
  [psxRegs_pc_off]             "i" (off(pc)),
  [psxRegs_cycle_off]          "i" (off(cycle)),
  [psxRegs_io_cycle_ctr_off]   "i" (off(io_cycle_counter)),
  [recRecompile]               "i" (&recRecompile),
  [psxBranchTest]              "i" (&mipsrec_branch_test),
  [psxRegs_stop_off]           "i" (off(stop)),
  [psxRecLUT]                  "i" (psxRecLUT)
: // Clobber - No need to list anything but 'saved' regs
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "fp", "ra", "memory"
);
}


/* Execute blocks starting at psxRegs.pc
 * Blocks return directly (not indirectly via stack address var).
 * Block pointers are looked up using virtual address mapping of recRAM/recROM.
 * Called only from recExecute(), see notes there.
 *
 * IMPORTANT: Functions containing inline ASM should have attribute 'noinline'.
 *            Crashes at callsites can occur otherwise, at least with GCC 4.xx.
 */
__attribute__((noinline)) static void recExecute_direct_return_mmap()
{
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set push                                    \n"
".set noreorder                               \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                        32  \n"
".equ  f_off_branchtest_fastpath_ra,      28  \n"
".equ  f_off_temp_var1,                   24  \n"
".equ  f_off_block_start_addr,            16  \n"
"addiu $sp, $sp, -frame_size                  \n"

#ifdef USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS
// Set the global var 'block_fast_ret_addr'.
"la    $t0, fastpath_loop%=                   \n"
"lui   $t1, %%hi(%[block_fast_ret_addr])      \n"
"sw    $t0, %%lo(%[block_fast_ret_addr])($t1) \n"

// We need a stack var to hold a return address for a 'fastpath' call to
//  psxBranchTest. This helps reduce size of code through sharing.
"la    $t0, branchtest_fastpath_retaddr%=     \n"
"sw    $t0, f_off_branchtest_fastpath_ra($sp) \n"
#endif

// Set the global var 'block_ret_addr'.
"la    $t0, loop%=                            \n"
"lui   $t1, %%hi(%[block_ret_addr])           \n"
"sw    $t0, %%lo(%[block_ret_addr])($t1)      \n"

// $fp/$s8 remains set to &psxRegs across all calls to blocks
"la    $fp, %[psxRegs]                        \n"

// Load $v0 once with psxRegs.pc, blocks will assign new value when returning
"lw    $v0, %[psxRegs_pc_off]($fp)            \n"

// Load $v1 once with zero. It is the # of cycles psxRegs.cycle should be
// incremented by when a block returns.
"move  $v1, $0                                \n"

// Align loop on cache-line boundary
".balign 32                                   \n"

////////////////////////////
//       LOOP CODE:       //
////////////////////////////

// NOTE: Blocks return to top of loop.
// NOTE: Loop expects following values to be set:
// $v0 = new value for psxRegs.pc
// $v1 = # of cycles to increment psxRegs.cycle by

// The loop pseudocode is this, interleaving ops to reduce load stalls:
//
// loop:
// $t2 = REC_RAM_VADDR | ($v0 & 0x00ffffff)
// $t0 = *($t2)
// psxRegs.cycle += $v1
// if (psxRegs.cycle >= psxRegs.io_cycle_counter)
//    goto call_psxBranchTest;
// psxRegs.pc = $v0
// if ($t0 == 0)
//    goto recompile_block;
// tmp_block_start_addr = $t0
// goto $t0;
// /* Code at addr $t0 will run and return having set $v0 to new psxRegs.pc */
// /*  value and $v1 to the number of cycles to increment psxRegs.cycle by. */
// /* If block branches back to its beginning, it will return to the        */
// /*  fastpath version of loop that skips looking up a code pointer.       */

// Infinite loop, blocks return here
"loop%=:                                      \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter

// The block ptrs are mapped virtually to address space allowing lower
//  24 bits of PS1 PC address to lookup start of any RAM or ROM code block.
"lui   $t2, %[REC_RAM_VADDR_UPPER]            \n"
#ifdef HAVE_MIPS32R2_EXT_INS
"ins   $t2, $v0, 0, 24                        \n"
#else
"sll   $t1, $v0, 8                            \n"
"srl   $t1, $t1, 8                            \n"
"or    $t2, $t2, $t1                          \n"
#endif
"lw    $t0, 0($t2)                            \n" // $t0 = address of start of block code, or
                                                  //       or 0 if block needs recompilation
                                                  // IMPORTANT: leave block ptr in $t2, it gets
                                                  // saved & re-used if recRecompile() is called.

"addu  $t3, $t3, $v1                          \n" // $t3 = psxRegs.cycle + $v1

// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest%=              \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Use BD slot to store new psxRegs.pc val

// Execute already-compiled block. It returns to top of 'fastpath' loop if it
//  jumps to its own beginning PC. Otherwise, it returns to top of main loop.
"execute_block%=:                             \n"
"jr    $t0                                    \n"
#ifdef USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS
"sw    $t0, f_off_block_start_addr($sp)       \n" // <BD> Save code address, in case block jumps back
                                                  //      to its beginning and takes 'fastpath'
#else
"nop                                          \n" // <BD>
#endif

////////////////////////////
//   BRANCH-TEST CODE:    //
////////////////////////////

// Call psxBranchTest() and go back to top of loop
"call_psxBranchTest%=:                        \n"
"jal   %[psxBranchTest]                       \n"
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Store new psxRegs.pc val before calling C
"branchtest_fastpath_retaddr%=:               \n" // Next 3 instructions shared with 'fastpath' code..
"lbu   $t0, %[psxRegs_stop_off]($fp)          \n" // .stop: frontend requests break (menu/save/quit)
"bnez  $t0, exit%=                            \n" //  leave dispatch loop so Execute() returns to C
"nop                                          \n" // <BD>
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // After psxBranchTest() returns, load psxRegs.pc
                                                  //  back into $v0, which could be different than
                                                  //  before the call if an exception was issued.
"b     loop%=                                 \n" // Go back to top to process new psxRegs.pc value..
"move  $v1, $0                                \n" // <BD> ..using BD slot to set $v1 to 0, since
                                                  //  psxRegs.cycle shouldn't be incremented again.

#ifdef USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS
////////////////////////////
//   FASTPATH LOOP CODE:  //
////////////////////////////

// When a block's new PC (jump target) is the same as its own beginning PC,
//  i.e. it branches back to its own top, it will return to this 'fast path'
//  version of above loop. It assumes that the block is already compiled and
//  unmodified, saving cycles versus the general loop. The main loop was
//  careful to save the block start address at location in stack frame.
//  NOTE: Blocks returning this way don't bother to set $v0 to any PC value.
"fastpath_loop%=:                             \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter
"lw    $t0, f_off_block_start_addr($sp)       \n" // Load block code addr saved in main loop
"addu  $t3, $t3, $v1                          \n" // $t3 = psxRegs.cycle + $v1

// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest_fastpath%=     \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Execute already-compiled block. It returns to top of 'fastpath' loop if it
//  jumps to its own beginning PC. Otherwise, it returns to top of main loop.
"jr    $t0                                    \n"
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> Blocks expect $v0 to contain PC val on entry

////////////////////////////
//   BRANCH-TEST CODE:    //
////////////////////////////

// Call psxBranchTest(). We don't have a new PC in $v0 to store, so call it
//  directly, and have it return to code shared with main 'call_psxBranchTest'.
"call_psxBranchTest_fastpath%=:               \n"
"j     %[psxBranchTest]                       \n"
"lw    $ra, f_off_branchtest_fastpath_ra($sp) \n" // <BD>

#endif // USE_DIRECT_FASTPATH_BLOCK_RETURN_JUMPS

////////////////////////////
// RECOMPILE BLOCK CODE:  //
////////////////////////////

// Recompile block and return to normal codepath.
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block ptr across call
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block ptr upon return
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // Blocks expect $v0 to contain PC val on entry
"b     execute_block%=                        \n" // Resume normal code path, but first we must..
"lw    $t0, 0($t2)                            \n" // <BD> ..load $t0 with ptr to block code


// Destroy stack frame, exiting inlined ASM block
// NOTE: During block executions, we'd never reach this point because the block
//  dispatch loop is currently infinite. This could change in the future.
// TODO: Could add a way to reset a game or load a new game from within
//  the running emulator by setting a global boolean, resetting
//  psxRegs.io_cycle_counter to 0, and checking if it's been set before
//  calling pxsBranchTest() here. If set, you must jump here to
//  exit the loop, to ensure that stack frame is adjusted before return!
"exit%=:                                      \n"
"addiu $sp, $sp, frame_size                   \n"
".set pop                                     \n"

: // Output
: // Input
  [block_ret_addr]             "i" (&block_ret_addr),
  [block_fast_ret_addr]        "i" (&block_fast_ret_addr),
  [psxRegs]                    "i" (&psxRegs),
  [psxRegs_pc_off]             "i" (off(pc)),
  [psxRegs_cycle_off]          "i" (off(cycle)),
  [psxRegs_io_cycle_ctr_off]   "i" (off(io_cycle_counter)),
  [recRecompile]               "i" (&recRecompile),
  [psxBranchTest]              "i" (&mipsrec_branch_test),
  [psxRegs_stop_off]           "i" (off(stop)),
  [REC_RAM_VADDR_UPPER]        "i" (REC_RAM_VADDR >> 16)
: // Clobber - No need to list anything but 'saved' regs
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "fp", "ra", "memory"
);
}


/* Execute blocks starting at psxRegs.pc until 'target_pc' is reached.
 * Blocks return indirectly, to address stored at 16($sp)
 *
 * IMPORTANT: Functions containing inline ASM should have attribute 'noinline'.
 *            Crashes at callsites can occur otherwise, at least with GCC 4.xx.
 */
__attribute__((noinline)) static void recExecuteBlock(unsigned target_pc)
{
	// Set block_ret_addr to 0, so generated code uses indirect returns
	block_ret_addr = block_fast_ret_addr = 0;

#ifndef ASM_EXECUTE_LOOP
	do {
		u32 *p = (u32*)PC_REC(psxRegs.pc);
		if (*p == 0)
			recRecompile();

		recFunc((void *)*p);

		if ((s32)(psxRegs.cycle - psxRegs.io_cycle_counter) >= 0) // wrap-safe (see subu/bgez in asm loop)
			mipsrec_branch_test();
	} while (psxRegs.pc != target_pc);
#else
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set push                                    \n"
".set noreorder                               \n"

// $fp/$s8 remains set to &psxRegs across all calls to blocks
"la     $fp, %[psxRegs]                       \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                  32        \n"
".equ  f_off_target_pc,             24        \n"
".equ  f_off_temp_var1,             20        \n"
".equ  f_off_block_ret_addr,        16        \n" // NOTE: blocks assume this is at 16($sp)!
"addiu $sp, $sp, -frame_size                  \n"

// Store const copy of 'target_pc' parameter in stack frame
"sw    %[target_pc], f_off_target_pc($sp)     \n"

// Store const block return address at fixed location in stack frame
"la    $t0, return_from_block%=               \n"
"sw    $t0, f_off_block_ret_addr($sp)         \n"

// Load $v0 once with psxRegs.pc, blocks will assign new value when returning
"lw    $v0, %[psxRegs_pc_off]($fp)            \n"

// Load $v1 once with zero. It is the # of cycles psxRegs.cycle should be
// incremented by when a block returns.
"move  $v1, $0                                \n"

// Align loop on cache-line boundary
".balign 32                                   \n"

////////////////////////////
//       LOOP CODE:       //
////////////////////////////

// NOTE: Loop expects following values to be set:
// $v0 = new value for psxRegs.pc
// $v1 = # of cycles to increment psxRegs.cycle by

// The loop pseudocode is this, interleaving ops to reduce load stalls:
//
// loop:
// $t2 = psxRecLUT[pxsRegs.pc >> 16] + (psxRegs.pc & 0xffff)
// $t0 = *($t2)
// psxRegs.cycle += $v1
// if (psxRegs.cycle >= psxRegs.io_cycle_counter)
//    goto call_psxBranchTest;
// if ($t0 == 0)
//    goto recompile_block;
// $ra = block return address
// goto $t0;
// /* Code at addr $t0 will run and return having set $v0 to new psxRegs.pc */
// /*  value and $v1 to the number of cycles to increment psxRegs.cycle by. */
// psxRegs.pc = $v0
// if (psxRegs.pc == target_pc)
//    goto exit;
// else
//    goto loop;

// Loop until psxRegs.pc == target_pc
"loop%=:                                      \n"
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"lui   $t1, %%hi(%[psxRecLUT])                \n"
"srl   $t2, $v0, 16                           \n"
"sll   $t2, $t2, 2                            \n" // sizeof() psxRecLUT[] elements is 4
"addu  $t1, $t1, $t2                          \n"
"lw    $t1, %%lo(%[psxRecLUT])($t1)           \n" // $t1 = psxRecLUT[psxRegs.pc >> 16]
"lw    $t4, %[psxRegs_io_cycle_ctr_off]($fp)  \n" // $t4 = psxRegs.io_cycle_counter
"addu  $t3, $t3, $v1                          \n" // $t3 = new psxRegs.cycle val
"andi  $t0, $v0, 0xffff                       \n"
"addu  $t2, $t0, $t1                          \n"
"lw    $t0, 0($t2)                            \n" // $t0 now points to beginning of recompiled
                                                  //  block, or is 0 if block needs compiling

// Must call psxBranchTest() when psxRegs.cycle >= psxRegs.io_cycle_counter
"subu  $t4, $t3, $t4                          \n" // wrap-safe horizon: $t4 = (cycle+v1) - next_interupt
"bgez  $t4, call_psxBranchTest%=              \n" // reached iff (s32)(cycle-next)>=0 (survives u32 cycle wrap)
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> IMPORTANT: store new psxRegs.cycle val,
                                                  //  whether or not we are branching here

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"nop                                          \n" // <BD>

"execute_block%=:                             \n"
"jalr  $ra, $t0                               \n" // Block is returning here, so safe to
"nop                                          \n" // <BD> set $ra using 'jalr'

"return_from_block%=:                         \n"
// Return point for all executed blocks, which will have set:
// $v0 to new value for psxRegs.pc
// $v1 to the # of cycles to increment psxRegs.cycle by

// Check if target_pc has been reached, looping if not
"lw    $t0, f_off_target_pc($sp)              \n" // $t0 = target_pc
"bne   $v0, $t0, loop%=                       \n" // Loop if target_pc hasn't been reached, using..
"sw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD> ..BD slot to store new psxRegs.pc val

// Before cleanup/exit, ensure psxRegs.cycle is incremented by block's $v1 retval
"lw    $t3, %[psxRegs_cycle_off]($fp)         \n" // $t3 = psxRegs.cycle
"addu  $t3, $t3, $v1                          \n"
"b     exit%=                                 \n" // Goto cleanup/exit code, using BD slot..
"sw    $t3, %[psxRegs_cycle_off]($fp)         \n" // <BD> ..to store new psxRegs.cycle val

////////////////////////////
//     NON-LOOP CODE:     //
////////////////////////////

// Call psxBranchTest() and go back to top of loop
"call_psxBranchTest%=:                        \n"
"jal   %[psxBranchTest]                       \n"
"nop                                          \n" // <BD>
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // After psxBranchTest() returns, load psxRegs.pc
                                                  //  back into $v0, which could be different than
                                                  //  before the call if an exception was issued.
"b     loop%=                                 \n" // Go back to top to process psxRegs.pc again..
"move  $v1, $0                                \n" // <BD> ..using BD slot to set $v1 to 0, since
                                                  //  psxRegs.cycle shouldn't be incremented again.

// Recompile block and return to normal codepath
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block ptr across call
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block ptr upon return
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // Blocks expect $v0 to contain PC val on entry
"b     execute_block%=                        \n" // Resume normal code path, but first we must..
"lw    $t0, 0($t2)                            \n" // <BD> ..load $t0 with ptr to block code

// Destroy stack frame, exiting inlined ASM block
"exit%=:                                      \n"
"addiu $sp, $sp, frame_size                   \n"
".set pop                                     \n"

: // Output
: // Input
  [target_pc]                  "r" (target_pc),
  [psxRegs]                    "i" (&psxRegs),
  [psxRegs_pc_off]             "i" (off(pc)),
  [psxRegs_cycle_off]          "i" (off(cycle)),
  [psxRegs_io_cycle_ctr_off]   "i" (off(io_cycle_counter)),
  [psxRecLUT]                  "i" (psxRecLUT),
  [recRecompile]               "i" (&recRecompile),
  [psxBranchTest]              "i" (&mipsrec_branch_test)
: // Clobber - No need to list anything but 'saved' regs
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "fp", "ra", "memory"
);
#endif
}


static void recExecute()
{
	fm_install();

	// Clear code cache so that all emitted code from this point forward uses
	//  the correct block-return method. This also clears out any now-dead code
	//  emitted during BIOS startup. Non-dead BIOS code gets recompiled fresh.
	recReset();

	// By default, emit code that returns to dispatch loop indirectly, using
	//  address kept on stack. This return method is safe for use with both
	//  HLE BIOS and real BIOS. In fact, HLE BIOS requires indirect return
	//  jumps: During main execution, i.e. recExecute(), HLE BIOS calls
	//  recExecuteBlock() for certain 'softcalls', so in effect, two separate
	//  dispatch loops can be active, even simultaneously! Therefore, blocks
	//  under HLE BIOS cannot know at compile-time where to return to.
	//
	// To force indirect return code, set 'block_ret_addr' to 0 here.
	bool use_indirect_return_dispatch_loop = true;
	block_ret_addr = block_fast_ret_addr = 0;

#if defined(ASM_EXECUTE_LOOP) && defined(USE_DIRECT_BLOCK_RETURN_JUMPS)
	// When using a real BIOS, recExecuteBlock() is only ever called for
	//  initial BIOS startup, and when a certain PC is reached, it returns.
	//  Soon after that, this function is called to begin real execution.
	//  At this point, blocks will always be returning to the same address.
	//  We now emit faster, denser code that returns via direct jumps.
	//
	// NOTE: On entry, direct-return versions of dispatch loops set vars
	//       'block_ret_addr' and 'block_fast_ret_addr' to correct value.
	// IMPORTANT: All existing code needs to have been cleared at this point.
	use_indirect_return_dispatch_loop = Config.HLE;
#endif

	if (use_indirect_return_dispatch_loop) {
		if (rec_mem_mapped)
			recExecute_indirect_return_mmap();
		else
			recExecute_indirect_return_lut();
	} else {
		if (rec_mem_mapped)
			recExecute_direct_return_mmap();
		else
			recExecute_direct_return_lut();
	}
}


/* Invalidate 'Size' code block pointers at word-aligned PS1 address 'Addr'. */
static void recClear(u32 Addr, u32 Size)
{
	const u32 masked_ram_addr = Addr & 0x1ffffc;

	// Callers pass unsanitized sizes: misc.c passes size/4 (0 for tiny EXE
	//  sections), and cdrom.c's psxDma3 passes the raw DMA word count
	//  unclamped (up to 0x10000 words -- "GS CDX" spams exactly that). A
	//  Size of 0 would wrap (Size-1) below; an unclamped end would run the
	//  page scan and memset past the 2MB RAM's code_pages[]/recRAM bounds.
	if (Size == 0)
		return;
	const u32 max_words = (0x200000 - masked_ram_addr) / 4;
	if (Size > max_words)
		Size = max_words;

	// Check if the page(s) of PS1 RAM that 'Addr','Size' target contain the
	//  start of any blocks. If not, invalidation would have no effect and is
	//  skipped. This eliminates 99% of large unnecessary invalidations that
	//  occur when many games stream CD data in-game.
	u32 page = masked_ram_addr/4096;
	u32 end_page = ((masked_ram_addr + (Size-1)*4)/4096) + 1;
	bool has_code = false;
	do {
		u32 pflag = 1 << (page & 7);  // Each byte in code_pages[] represents 8 pages
		has_code = code_pages[page/8] & pflag;
	} while ((++page != end_page) && !has_code);

	// NOTE: If PS1 mem is mapped/mirrored, make PC_REC_MMAP use the same virtual
	//       mirror region the game is already using, reducing TLB pressure.
	uptr dst_base = !rec_mem_mapped ? (uptr)recRAM : (uptr)PC_REC_MMAP(Addr & ~0x1fffff);

	if (has_code) {
		void *dst = (void*)(dst_base + (masked_ram_addr * REC_RAM_PTR_SIZE/4));
		memset(dst, 0, Size*REC_RAM_PTR_SIZE);
	}
}


/* Notification from emulator. */
static void recNotify(int note, void *data __attribute__((unused)))
{
	switch (note)
	{
		/* R3000ACPU_NOTIFY_CACHE_ISOLATED,
		 * R3000ACPU_NOTIFY_CACHE_UNISOLATED
		 *  Sent from psxMemWrite32_CacheCtrlPort(). Also see notes there.
		 */
		case R3000ACPU_NOTIFY_CACHE_ISOLATED:
			/*  Back up lower 64KB PS1 RAM. Emitted direct stores skip the
			 * 'writeok' check (SKIP_WRITEOK_CHECK), so the i-cache-flush idiom
			 * trashes low RAM while the cache is isolated; we revert it below.
			 * (rearmed has no psxMemWrite32_CacheCtrlPort; we do it here.)
			 */
			memcpy(ram_bak, psxRegs.ptrs.psxM, sizeof(ram_bak));
			REC_LOG_V("R3000ACPU_NOTIFY_CACHE_ISOLATED\n");
			break;
		case R3000ACPU_NOTIFY_CACHE_UNISOLATED:
			/*  Flush entire code cache, game has loaded new code:
			 * BIOS or routine has finished invalidating cache lines.
			 * psxMemWrite32_CacheCtrlPort() has restored lower 64KB PS1 RAM.
			 *  Using this coarse invalidation method fixes some games that
			 * previously needed hacks in recClear(), 'Buster Bros. Collection'.
			 * It's also part of a fix/hack for certain games that did Icache
			 * trickery (see DMA3 stuff further below).
			 * TODO: With this new method, it should eventually be possible to
			 *  provide an option to allow emitted code to skip most code
			 *  invalidations after stores, boosting speed.
			 */
			memcpy(psxRegs.ptrs.psxM, ram_bak, sizeof(ram_bak));  /* revert isolated-write trash */
			recClear(0, 0x200000/4);
			REC_LOG_V("R3000ACPU_NOTIFY_CACHE_UNISOLATED\n");
			break;

		/* Sent from psxDma3(). Also see notes there. */
		case R3000ACPU_NOTIFY_DMA3_EXE_LOAD:
			/* Game has begun reading from a CDROM file whose first sector is
			 * a standard 'PS-X EXE' header. Part of a hack by senquack to fix:
			 *  'Formula One Arcade' (crash on load)
			 *  'Formula One 99'     (crash on load) (NOTE: PAL version requires .SBI file)
			 *  'Formula One 2001'   (in-game controls, AI broken.. even the PS3
			 *                        supposedly has trouble emulating this.)
			 *
			 *  The workaround is enabled on a per-game basis (using CdromId).
			 *
			 *  These games do Icache trickery and merely doing the usual
			 * code-flush when Icache is unisolated is not enough. The fix is
			 * admittedly just a lucky hack, and requires these things:
			 *  1.)  As usual, invalidate code whenever emu calls recClear(),
			 *      typically after a DMA transfer.
			 *      *However*, emit no code invalidations whatsoever.
			 *      Fixes the in-game AI/controls in 'Formula One 2001'.
			 *  2.)  As usual, flush code cache when Icache is unisolated.
			 *      *However*, also flush cache when psxDma3() notifies us here.
			 *      This fixes crashes.
			 */
			if (flush_code_on_dma3_exe_load) {
				recClear(0, 0x200000/4);
				REC_LOG_V("R3000ACPU_NOTIFY_DMA3_EXE_LOAD .. Flushing dynarec cache\n");
			} else {
				REC_LOG_V("R3000ACPU_NOTIFY_DMA3_EXE_LOAD\n");
			}
			break;

		default:
			break;
	}
}


static void recReset()
{
	memset(code_pages, 0, sizeof(code_pages));
	memset(recRAM, 0, REC_RAM_SIZE);
	memset(recROM, 0, REC_ROM_SIZE);

	recMem = (u32*)recMemBase;
	fm_tbl_n = 0;      /* faultmem host->guest table: recMem was rewound */

	regReset();

	// Set default recompilation options and any per-game options
	rec_set_options();
}


/* EROSQ/rearmed port: pcsx4all's original file ended by assembling these
 * functions into its own `R3000Acpu psxRec` global. rearmed's R3000Acpu has a
 * different shape (8 fields incl. ApplyConfig; Execute/ExecuteBlock/Notify
 * take rearmed-shaped args) and, more importantly, libpcsxcore/mipsrec/plugin.c
 * (prior task) already OWNS the one-and-only `psxRec` global for this build —
 * a second definition here would be a duplicate-symbol link error, not just
 * an expected-undefined one. Wiring recInit/recReset/recExecute/
 * recExecuteBlock/recClear/recNotify/recShutdown into plugin.c's psxRec is
 * Task 1.3 / Phase 3, not this task. All seven stay file-static (matching
 * the task brief's "Produces" list) until then. */

/* Phase 3 (GO LIVE): the seven recompiler entry points above are file-static.
 * Expose them to plugin.c (plain C, unmangled) via C-linkage wrappers so the
 * psxRec vtable can drive the real recompiler. Same-TU calls, so the static
 * targets are visible here. Declared in compat.h + plugin.c. */
extern "C" {
	int  mipsrec_rec_init(void)                     { return recInit(); }
	void mipsrec_rec_reset(void)                    { recReset(); }
	void mipsrec_rec_execute(void)                  { recExecute(); }
	void mipsrec_rec_execute_block(unsigned pc)     { fm_install(); recExecuteBlock(pc); }
	void mipsrec_rec_clear(u32 addr, u32 size)      { recClear(addr, size); }
	void mipsrec_rec_notify(int note, void *data)   { recNotify(note, data); }
	void mipsrec_rec_shutdown(void)                 { recShutdown(); }
}

/* EROSQ: compile one block at 'start' without executing it (offline
 * emit-size harness, see erosq-emitstat/). Returns the guest PC after the
 * block. Not used by the emulator. */
extern "C" u32 mipsrec_rec_compile_at(u32 start)
{
	psxRegs.pc = start;
	recRecompile();
	return pc;
}
extern "C" void *mipsrec_rec_cur(void) { return recMem; }
/* offline harness only: emit blocks as the live dispatcher would (direct
 * returns to a fixed loop address) instead of the boot-time indirect mode */
extern "C" void mipsrec_rec_harness_direct_returns(uintptr_t loop, uintptr_t fast)
{
	block_ret_addr = loop;
	block_fast_ret_addr = fast;
}
extern "C" void mipsrec_rec_harness_faultmem(void) { fm_install(); }
extern "C" void mipsrec_faultmem_clear_slow(void) { memset(fm_slow, 0, sizeof fm_slow); fm_slow_n = 0; }
extern "C" void mipsrec_faultmem_set(int on) { fm_enabled(); fm_on = on; }
