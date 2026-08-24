/* WARP ENGINE -- the block engine. Round 2: the first round that generates code.
 *
 * z3660_warp.h is the contract and docs/WARP-ENGINE.md §0.8 is the pre-registration, both
 * written before this file. The short version, because everything here follows from it:
 *
 *   Blocks are discovered at CONTROL-TRANSFER TARGETS, keyed on guest logical PC plus the
 *   supervisor bit in a 2048-set 4-way table whose index HASHES THE HIGH PC BITS, compiled
 *   to real ARM for generation one's frozen family set, cached, invalidated from the
 *   decoded-op cache's own choke point, and evicted by throwing the whole code cache away.
 *
 * WHAT ROUND 1 MEASURED AND ROUND 2 SPENDS. Sixty ARM instructions on the entry path at a
 * measured IPC of 0.46, paid in full for ONE guest instruction every time (WARP_BLK / INSNS
 * = 0.999878), and 3 066 944 compiles of a working set of order 10^3 with 95.2 % of the
 * recompiles attributable to neither invalidation nor eviction. The first is fixed by
 * amortisation (discover at targets, so a block is worth entering) and by deleting the four
 * shapes §0.7.3 named; the second by the lookup geometry in z3660_warp.h.
 *
 * THE DESIGN DECISION THAT MAKES THE BAIL CONTRACT CHEAP is stated in the header and is
 * worth repeating where the emitters are: THE GUEST REGISTER FILE STAYS IN MEMORY. Nothing
 * is ever dirty, so there is nothing to flush at a faultable site, a bail materialises the
 * PC and nothing else, and every instruction boundary inside a block already IS the
 * interpreter's boundary state.
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "cpummu.h"
#include "events.h"
#include "z3660_prof.h"
#include "z3660_dopc.h"
#define Z3660_WARP_ENGINE 1
#include "z3660_warp.h"

#include <string.h>
#include <stdint.h>
#ifdef HOST_TEST_HARNESS
#include <stdlib.h>
#endif

#ifndef HOST_TEST_HARNESS
/* The two CP15 line loops flush_cpu_icache() composes, declared with the signature
 * jit/compemu_midfunc_arm.cpp defines them with. Not reachable through a header: the
 * primitive itself is a STATIC_INLINE in a file that is textually #included into exactly
 * one translation unit, so the engine composes the sequence itself -- which it has to
 * anyway, because Q3 adds three instructions the shipped one does not have. */
void arm_flush_cache(uintptr_t addr, uint32_t length);
void arm_icache_invalidate(uintptr_t addr, uint32_t length);
#endif

/* ---------------------------------------------------------------- the arm flag ------- */

/* Read by the run loop's hook on EVERY dispatch, so its disarmed cost is a shipped cost
 * until the M5 gate. A plain int in one translation unit, deliberately not a getter: the
 * pre-registration prices this at 3 ARM instructions per guest instruction (§0.8.4
 * G-R2.8, down from round 1's measured 5) and the number is read off the lean objdump,
 * never estimated.
 *
 * Initialised DISARMED. That is the SECOND of the two mechanisms that keep an arm from
 * surviving a reboot -- core0 republishes the OFF sentinel unconditionally on every boot
 * (config_file.c), and this initialiser means a core1 running before core0 has published
 * anything is disarmed rather than reading whatever the DDR happened to hold. */
extern "C" { struct z3660_warp_gate_s z3660_warp_gate = { 0, Z3660_WARP_NO_LAST }; }

/* ---------------------------------------------------------------- the block table ---- */

struct z3660_warp_set z3660_warp_tab[Z3660_WARP_SETS];
struct z3660_warp_ctx z3660_warp_ctx;

/* 64 bytes per set on the target: four tags, the generation stamp, the victim, four 32-bit
 * code pointers and four lengths -- exactly one Cortex-A9 line, so a lookup touches one
 * line and never two. Guarded on 32-bit pointers only, because the host harness builds the
 * same source on x86-64 where an entry pointer is eight bytes and the set is legitimately
 * bigger; the harness asserts the field arithmetic instead, and this line is what refuses
 * the build that would actually ship. Same shape of tripwire, and same reasoning, as the
 * decoded-op cache's. */
typedef char z3660_warp_set_is_one_line[
   (sizeof(void *) != 4 || sizeof(struct z3660_warp_set) == 64) ? 1 : -1];

/* Generation 0 is RESERVED and never live, so a zeroed table -- BSS at power-on, and the
 * memset on wrap below -- is entirely stale rather than entirely valid. */
unsigned int z3660_warp_gen = 1;

static int warp_inited = 0;

/* ---------------------------------------------------------------- the code cache ----- */

static uae_u8 *warp_arena  = NULL;   /* base of the arena WARP owns                      */
static uae_u8 *warp_cp     = NULL;   /* the bump pointer                                 */
static unsigned warp_bytes = 0;      /* the arena's size                                 */
static unsigned warp_limit = 0;      /* usable size; the harness can lower it            */

#ifndef HOST_TEST_HARNESS
/* THE ARENA IS THE CODE CACHE'S OWN, AND IT COSTS NO NEW HEAP. jit/compemu_support_arm.cpp
 * already calls cache_alloc(POPALLSPACE_SIZE + MAX_JIT_CACHE * 1024) -- 32 MB + 2 KB of
 * malloc -- from create_popalls(), which build_cpufunctbl() reaches unconditionally under
 * #ifdef JIT, on every boot of every image including the MMU modes. Under the MMU
 * alloc_cache() returns early on cache_size == 0, so the 32 MB behind the trampolines is
 * allocated, zeroed, executable and entirely unused. WARP takes exactly that, and the
 * allocation's success is therefore PROVEN on this card every boot rather than assumed:
 * the serial line `Cache_Alloc of 33556480 at 0x........` is it.
 *
 * The accessor lives beside the allocation rather than here, and it REFUSES if the classic
 * JIT owns the arena -- which it does in the UAEJIT_* bootmodes. Two engines bump-allocating
 * one arena would be a wild-write generator, and refusing is how that is made impossible
 * rather than merely unlikely. */
extern uae_u8 *z3660_warp_cache_arena(unsigned *bytes);
#endif

/* ---------------------------------------------------------------- counters ----------- */

/* THE ARM ATTESTATION, as a SPLIT 32-BIT PAIR rather than a 64-bit counter.
 *
 * Compiled into EVERY variant, unlike the profiler's counter block, because the wall-clock
 * arm of every verdict is a LEAN boot with no profiler in it -- and an arm BANNER is not
 * admissible, because two cores share one unlocked UART and a banner arrives spliced. This
 * is the same increment site as WARP_BLK (prof id 48): one site, not two, so the lean
 * confession and the profiled counter can never disagree.
 *
 * §0.7.3 measured EIGHT ARM instructions for the 64-bit increment, on an entry path of
 * sixty. Split, the common arm is a load, an add, a store and a not-taken branch; and in
 * round 2 it is paid once per BLOCK rather than once per guest instruction, which is where
 * "bind the attestation where it costs nothing" actually lands. */
static uae_u32 warp_blk_lo = 0;
static uae_u32 warp_blk_hi = 0;

#ifdef HOST_TEST_HARNESS
static unsigned long long warp_stats[Z3660_WARP_STAT_COUNT];
#define WARP_STAT(i)      (warp_stats[(i)]++)
#define WARP_STAT_ADD(i,n) (warp_stats[(i)] += (unsigned long long)(n))
int z3660_warp_mutant = Z3660_WARP_MUT_NONE;
/* Harness-only side table: was the block in this way published through the I-cache
 * sequence? A side table and not a struct field, so the shipping structure the target
 * compiles is byte-for-byte the structure the harness tests. */
static unsigned char warp_published[Z3660_WARP_SETS][Z3660_WARP_WAYS];
static unsigned long long warp_bad_unpublished = 0;
static unsigned long long warp_bad_retired = 0;
static unsigned long long warp_bad_fixup = 0;
static unsigned long long warp_bad_super = 0;
#define WARP_MUTANT(m)  (z3660_warp_mutant == (m))
#else
#define WARP_STAT(i)       do { } while (0)
#define WARP_STAT_ADD(i,n) do { } while (0)
#define WARP_MUTANT(m)     (0)
#endif

/* Blocks currently live in the table. Not a scan: it is maintained at the sites that can
 * change it, because WARP_DISC is "blocks discarded by THIS invalidation" and a scan after
 * the generation has already moved would count nothing. */
static unsigned warp_live = 0;

/* ---------------------------------------------------------------- the key ------------ */

/* One function, so "what is a block keyed on?" is answered by reading one line -- and so
 * the mutation arm has exactly one place to bite. M-5 drops the supervisor bit, which makes
 * a supervisor block and a user block at the same virtual address alias; the corpus catches
 * it through z3660_warp_peek(), which uses this same function. */
static __inline__ uae_u32 warp_key(unsigned pc, int super)
{
#ifdef HOST_TEST_HARNESS
   if (WARP_MUTANT(Z3660_WARP_MUT_KEY_NOSUP))
      return (uae_u32)((unsigned)pc & ~1u);          /* the defect, never shipped */
#endif
   return (uae_u32)Z3660_WARP_KEY(pc, super);
}

/* THE INDEX, and the one place M-CONFLICT bites. The mutation restores round 1's geometry
 * exactly -- the low bits only, and the table folded back to 1024 sets -- so the corpus can
 * show the conflict term going from zero to a storm and back, which is the difference
 * between "we fixed it" and "we believe we fixed it". */
static __inline__ unsigned warp_index(unsigned pc)
{
#ifdef HOST_TEST_HARNESS
   if (WARP_MUTANT(Z3660_WARP_MUT_CONFLICT))
      return ((unsigned)pc >> 1) & 1023u;            /* the round-1 index, never shipped */
#endif
   return Z3660_WARP_INDEX(pc);
}

/* ==================================================================================== */
/* ============================ THE ARM EMITTER ======================================= */
/* ==================================================================================== */

/* Route C keeps "the raw ARMv7 emitters" (§0.1 Q1), and what that inheritance is worth is
 * the ENCODINGS, not the midfunc layer above them: jit/codegen_arm.h's macros write through
 * a global target pointer and a `#include`-once discipline that binds them to
 * compemu_support_arm.cpp's block manager. The encodings below are the same instruction
 * forms with the state made explicit, so the compiler is reentrant, bounded, and -- the
 * point -- compilable into the HOST harness, which executes what it emits (§0.8.5).
 *
 * THE EMITTED STREAM CONTAINS NO HOST ADDRESS. Generated code is handed the two base
 * pointers it needs (the guest register file and the block context) plus the two memory
 * helpers, in r0..r3 at entry, and materialises nothing else. That is what makes the same
 * words runnable on a 64-bit host, and it is a design constraint rather than a convenience:
 * an emitter that bakes addresses in can only ever be asserted, never run. */

#define R_REGS  4   /* &regs.regs[0]                                                     */
#define R_RD    5   /* the read helper                                                   */
#define R_WR    6   /* the write helper                                                  */
#define R_CTX   7   /* &z3660_warp_ctx                                                   */
#define R_KEEP  8   /* callee-saved scratch that survives a helper call                  */

#define CC_EQ 0u
#define CC_NE 1u
#define CC_CS 2u
#define CC_CC 3u
#define CC_MI 4u
#define CC_PL 5u
#define CC_VS 6u
#define CC_VC 7u
#define CC_HI 8u
#define CC_LS 9u
#define CC_GE 10u
#define CC_LT 11u
#define CC_GT 12u
#define CC_LE 13u
#define CC_AL 14u

#define OP_AND 0u
#define OP_EOR 1u
#define OP_SUB 2u
#define OP_ADD 4u
#define OP_TST 8u
#define OP_CMP 10u
#define OP_ORR 12u
#define OP_MOV 13u
#define OP_BIC 14u

#define CTX_PC    0u
#define CTX_NZCV  4u
#define CTX_XFLG  8u
#define CTX_TMP  12u
#define CTX_IPC     16u
#define CTX_RESTART 20u

struct warp_cc {
   uae_u32 *w;        /* where the next word goes                                       */
   unsigned n;        /* words emitted so far                                           */
   unsigned cap;      /* capacity in words                                              */
   unsigned head;     /* the block's head guest pc                                      */
   unsigned insns;    /* guest instructions compiled so far                             */
   unsigned cfetch;   /* guest instruction words the compiler read (Q6's counter)       */
   /* THE RESTART BOOKKEEPING THE LAST COMPILED INSTRUCTION WOULD HAVE LEFT. Only the last
    * RETIRED instruction's values survive to the boundary the oracle compares at, so a bail
    * arm carries the PREVIOUS instruction's -- which is exactly what these hold when a
    * faultable site inside the current instruction emits its exit. */
   unsigned last_ipc;
   unsigned last_restart;
   int      ok;       /* cleared the moment anything cannot be emitted                  */
};

static __inline__ void wemit(struct warp_cc *c, uae_u32 word)
{
   if (c->n >= c->cap) { c->ok = 0; return; }
   c->w[c->n++] = word;
}

/* The ARM data-processing immediate: an 8-bit value rotated right by an even amount.
 * Returns 1 and the encoded rot/imm8 when the constant is representable, 0 otherwise --
 * and every caller checks, because "it did not fit" must end the compile rather than emit
 * something almost right. */
static int warp_imm8(uae_u32 v, unsigned *rot, unsigned *imm8)
{
   unsigned r;
   for (r = 0; r < 32; r += 2) {
      uae_u32 x = (v << r) | (v >> ((32 - r) & 31));
      if (r == 0) x = v;
      if ((x & ~0xFFu) == 0) { *rot = r / 2; *imm8 = (unsigned)x; return 1; }
   }
   return 0;
}

static void e_dp_reg(struct warp_cc *c, unsigned cc, unsigned op, unsigned s,
                     unsigned rd, unsigned rn, unsigned rm,
                     unsigned shtype, unsigned shimm)
{
   wemit(c, (cc << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
            | ((shimm & 31u) << 7) | (shtype << 5) | rm);
}

static void e_dp_imm(struct warp_cc *c, unsigned cc, unsigned op, unsigned s,
                     unsigned rd, unsigned rn, uae_u32 imm)
{
   unsigned rot, i8;
   if (!warp_imm8(imm, &rot, &i8)) { c->ok = 0; return; }
   wemit(c, (cc << 28) | (1u << 25) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
            | (rot << 8) | i8);
}

static void e_mov(struct warp_cc *c, unsigned rd, unsigned rm)
{  e_dp_reg(c, CC_AL, OP_MOV, 0, rd, 0, rm, 0, 0); }

/* MOV rd, rm LSL/LSR #imm -- with S when the caller wants N and Z out of it. shtype:
 * 0 = LSL, 1 = LSR, 2 = ASR. LSR/ASR #32 is encoded as #0, which is why a 0 shift always
 * goes out as a plain MOV instead. */
static void e_shift(struct warp_cc *c, unsigned s, unsigned rd, unsigned rm,
                    unsigned shtype, unsigned shimm)
{
   if (shimm == 0) { e_dp_reg(c, CC_AL, OP_MOV, s, rd, 0, rm, 0, 0); return; }
   e_dp_reg(c, CC_AL, OP_MOV, s, rd, 0, rm, shtype, shimm);
}

static void e_movw(struct warp_cc *c, unsigned rd, uae_u32 imm16)
{
   wemit(c, 0xE3000000u | (((imm16 >> 12) & 0xFu) << 16) | (rd << 12) | (imm16 & 0xFFFu));
}
static void e_movt(struct warp_cc *c, unsigned rd, uae_u32 imm16)
{
   wemit(c, 0xE3400000u | (((imm16 >> 12) & 0xFu) << 16) | (rd << 12) | (imm16 & 0xFFFu));
}
/* A full 32-bit constant. Always two words, never a literal pool: this target builds with
 * -DARMV6T2 and §0.3(c) confirmed the data-buffer machinery is not compiled at all. */
static void e_const(struct warp_cc *c, unsigned rd, uae_u32 v)
{
   e_movw(c, rd, v & 0xFFFFu);
   e_movt(c, rd, (v >> 16) & 0xFFFFu);
}

static void e_ldr(struct warp_cc *c, unsigned rd, unsigned rn, int off)
{
   unsigned u = (off >= 0) ? 1u : 0u;
   unsigned a = (unsigned)(off >= 0 ? off : -off);
   if (a > 4095u) { c->ok = 0; return; }
   wemit(c, 0xE4100000u | (1u << 24) | (u << 23) | (rn << 16) | (rd << 12) | a);
}
static void e_str(struct warp_cc *c, unsigned rd, unsigned rn, int off)
{
   unsigned u = (off >= 0) ? 1u : 0u;
   unsigned a = (unsigned)(off >= 0 ? off : -off);
   if (a > 4095u) { c->ok = 0; return; }
   wemit(c, 0xE4000000u | (1u << 24) | (u << 23) | (rn << 16) | (rd << 12) | a);
}

static void e_push(struct warp_cc *c, unsigned list) { wemit(c, 0xE92D0000u | list); }
static void e_pop (struct warp_cc *c, unsigned list) { wemit(c, 0xE8BD0000u | list); }

static void e_mrs(struct warp_cc *c, unsigned rd)  { wemit(c, 0xE10F0000u | (rd << 12)); }
static void e_msr(struct warp_cc *c, unsigned rm)  { wemit(c, 0xE128F000u | rm); }

static void e_sxth(struct warp_cc *c, unsigned rd, unsigned rm)
{  wemit(c, 0xE6BF0070u | (rd << 12) | rm); }

static void e_ubfx(struct warp_cc *c, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{  wemit(c, 0xE7E00050u | ((width - 1u) << 16) | (rd << 12) | (lsb << 7) | rn); }
static void e_bfi(struct warp_cc *c, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{  wemit(c, 0xE7C00010u | ((lsb + width - 1u) << 16) | (rd << 12) | (lsb << 7) | rn); }

/* A branch whose target is `words` words past the instruction AFTER this one. Every branch
 * this compiler emits is a forward skip over a known-length sequence, so the offset is
 * known at emit time and there is no patch list to get wrong. */
static void e_b(struct warp_cc *c, unsigned cc, int words)
{
   /* `words` is the number of words to SKIP -- the target is the instruction `words` words
    * after this one. ARM's branch target is (this instruction + 2 words) + imm24, so the
    * encoded field is words - 1, and getting that wrong is the classic off-by-one this
    * compiler has no patch list to hide behind. The harness decodes every branch it emits
    * and checks the target lands on an instruction boundary inside the block. */
   wemit(c, (cc << 28) | 0x0A000000u | ((uae_u32)(words - 1) & 0x00FFFFFFu));
}
static void e_blx(struct warp_cc *c, unsigned rm) { wemit(c, 0xE12FFF30u | rm); }

#define WARP_PUSH_LIST  0x41F0u   /* r4,r5,r6,r7,r8,lr -- 24 bytes, keeps sp 8-aligned   */
#define WARP_POP_LIST   0x81F0u   /* r4,r5,r6,r7,r8,pc                                   */

/* ---- exits ---------------------------------------------------------------------------
 *
 * THERE IS ONLY ONE KIND OF EXIT. A block leaves through this sequence whether it ran to
 * the end of its compiled span or bailed at a faultable site, and both write the guest pc,
 * the interpreter's restart bookkeeping, and a COMPILE-TIME CONSTANT count of guest
 * instructions fully retired. That is shape (ii) of §0.3(b) in its cheapest form: nothing
 * else is ever dirty, so nothing else has to be materialised.
 *
 * WHY THE BOOKKEEPING IS HERE AND NOT PER INSTRUCTION. `regs.instruction_pc` and
 * `mmu_restart` are in §0.5.5's comparison set, and two generation-one shapes move them:
 * a memory-DESTINATION MOVE commits its flags, advances the pc, sets instruction_pc and
 * CLEARS mmu_restart before it stores (§2e's write-back shape), and DBcc's two non-branch
 * exits use m68k_setpci(), which moves instruction_pc with the pc. Only the LAST retired
 * instruction's values survive to the boundary the oracle compares at, so an exit carries
 * them and the path between exits pays nothing. The differential sweep in the harness found
 * this gap on its first run, which is the entire argument for sweeping all 65 536 opcodes
 * rather than picking a corpus. */
static unsigned e_exit_words(unsigned pc, unsigned ipc)
{
   return (ipc == pc) ? 8u : 10u;
}

static void e_exit(struct warp_cc *c, unsigned pc, unsigned ipc, unsigned restart,
                   unsigned count)
{
   e_const(c, 1, pc);
   e_str(c, 1, R_CTX, (int)CTX_PC);
   if (ipc != pc)
      e_const(c, 1, ipc);
   e_str(c, 1, R_CTX, (int)CTX_IPC);
   e_dp_imm(c, CC_AL, OP_MOV, 0, 1, 0, restart);
   e_str(c, 1, R_CTX, (int)CTX_RESTART);
   e_dp_imm(c, CC_AL, OP_MOV, 0, 0, 0, count);
   e_pop(c, WARP_POP_LIST);
}

/* A conditional bail: if `cc` holds, leave through an exit at `pc` having retired `count`.
 * Emitted as the inverted branch over a straight-line exit, so there is no patch list and
 * no label bookkeeping -- the exit's length is a function of its two arguments and nothing
 * else. */
static void e_bail_if(struct warp_cc *c, unsigned cc, unsigned pc, unsigned count)
{
   /* M-3, in the one place it can bite: the bail stores the BLOCK-HEAD pc instead of the
    * per-site one -- shape (ii)'s per-site constant delta dropped. The block still runs and
    * still bails; the interpreter then resumes several instructions too early and re-runs
    * work the block already retired, which is a silent double execution rather than a
    * crash. That is why it is a lockstep case and not an assertion. */
   if (WARP_MUTANT(Z3660_WARP_MUT_BAILPC))
      pc = c->head;
   e_b(c, cc ^ 1u, (int)e_exit_words(pc, c->last_ipc));
   e_exit(c, pc, c->last_ipc, c->last_restart, count);
}

/* ==================================================================================== */
/* ============================ THE MEMORY HELPERS ==================================== */
/* ==================================================================================== */

/* THE jnf_MEM_* REPLACEMENT, and §0.8.3 records the one deviation from the round's brief
 * that it is: the verdict-table fast path is reached through ONE SHARED HELPER rather than
 * open-coded into the instruction stream.
 *
 * The reason is equivalence by construction rather than by transcription. This calls the
 * SAME always_inline resolver the interpreter's accessors call, so "the two paths agree" is
 * a property of there being one path. An open-coded sequence would be about thirty-five
 * emitted instructions reproducing mmu_pagemaski, mmu_pageshift1m, the ATC tag compare,
 * mem_banks[] and addrbank_is_direct_* by hand -- five separate opportunities to ship a
 * divergence under the banner of a speed change. And in this design a call is nearly free,
 * because nothing is dirty and there is no state to flush before it. The open-coded
 * expansion is priced in this round's objdump and becomes a round-3 item with a measured
 * saving in front of it rather than an assumption behind it.
 *
 * EVERY FENCE IS A REFUSAL AND EVERY FENCE IS ABOVE EVERY COUNTER. Misalignment bails; an
 * access that crosses a guest page bails; a SLOW verdict, an ATC miss and a non-direct bank
 * all bail inside the resolver. The fences run BEFORE the resolver is called, because the
 * resolver counts DFAST_HIT when it serves and a fence that rejected afterwards would have
 * charged a hit the interpreter is about to charge again.
 *
 * READ and WRITE (prof ids 3 and 4) are incremented ONLY WHEN THE HELPER SERVES. §0.5.6
 * pre-registers them as "unchanged", and they are: on a bail the helper counts nothing and
 * the interpreter's own re-execution supplies the count. Counting on the bail arm too would
 * have double-charged exactly the accesses the engine declined.
 *
 * G3 -- ZERO throw SITES REACHABLE FROM GENERATED CODE -- is met by construction here and
 * is the reason the helper declines instead of translating: z3660_dfast_read/write return 0
 * rather than calling mmu_translate(), so the reachable call graph from generated code
 * contains no __cxa_throw, no __cxa_allocate_exception, no _Unwind_RaiseException and no
 * TRY block. Q2 measured what getting this wrong costs: not a fault, std::terminate. */

static __inline__ int warp_fence(uae_u32 addr, uae_u32 size)
{
#ifdef HOST_TEST_HARNESS
   if (WARP_MUTANT(Z3660_WARP_MUT_FENCE))
      return 1;                                       /* the defect, never shipped */
#endif
   if ((addr & (size - 1u)) != 0)
      return 0;                                       /* misaligned -> the interpreter   */
   /* The structural rule, stated as a test rather than as an argument: a naturally aligned
    * access cannot cross a page, so this never fires on a served access -- and M-FENCE
    * removes it to prove the corpus would notice if it did. */
   if (((addr ^ (addr + size - 1u)) & (uae_u32)mmu_pagemaski) != 0)
      return 0;
   return 1;
}

extern "C" int z3660_warp_rd(uae_u32 addr, uae_u32 size, uae_u32 unused)
{
   uaecptr pa; uae_u8 cs;

   (void)unused;
   if (!warp_fence(addr, size))
      goto decline;
   if (!z3660_dfast_read(addr, &pa, &cs))
      goto decline;
#ifdef HOST_TEST_HARNESS
   if (WARP_MUTANT(Z3660_WARP_MUT_VERDICT))
      pa = pa;                                        /* see the note in z3660_warp_wr   */
#endif
   mmu_cache_state = cs;
   switch (size) {
   case 1:  z3660_warp_ctx.tmp = (uae_u32)direct_get_byte(pa); break;
   case 2:  z3660_warp_ctx.tmp = (uae_u32)direct_get_word(pa); break;
   default: z3660_warp_ctx.tmp = (uae_u32)direct_get_long(pa); break;
   }
   Z3660_PROF_CNT(Z3660_PROF_C_READ);
   WARP_STAT(Z3660_WARP_STAT_MEMHIT);
   return 1;
decline:
   WARP_STAT(Z3660_WARP_STAT_MEMDECL);
   return 0;
}

extern "C" int z3660_warp_wr(uae_u32 addr, uae_u32 size, uae_u32 val)
{
   uaecptr pa; uae_u8 cs;

   if (!warp_fence(addr, size))
      goto decline;
#ifdef HOST_TEST_HARNESS
   /* M-VERDICT: the decline arm made permissive. A write whose verdict table says SLOW --
    * which is what TTR_NO_WRITE produces, the asymmetry §0.5.5's G4 exists to protect -- is
    * served anyway, using the READ resolver's answer. It is the exact defect that would
    * make a read-only TTR region writable from generated code and nothing else. */
   if (WARP_MUTANT(Z3660_WARP_MUT_VERDICT)) {
      if (z3660_dfast_read(addr, &pa, &cs)) goto serve;
      goto decline;
   }
#endif
   if (!z3660_dfast_write(addr, &pa, &cs))
      goto decline;
#ifdef HOST_TEST_HARNESS
serve:
#endif
   mmu_cache_state = cs;
   switch (size) {
   case 1:  direct_put_byte(pa, (uae_u8)val); break;
   case 2:  direct_put_word(pa, (uae_u16)val); break;
   default: direct_put_long(pa, val); break;
   }
   Z3660_PROF_CNT(Z3660_PROF_C_WRITE);
   WARP_STAT(Z3660_WARP_STAT_MEMHIT);
   return 1;
decline:
   WARP_STAT(Z3660_WARP_STAT_MEMDECL);
   return 0;
}

/* ==================================================================================== */
/* ============================ GENERATION ONE'S COMPILERS ============================ */
/* ==================================================================================== */

/* THE SCOPE IS §0.5.1'S, FROZEN, AND ROUND 2 DOES NOT WIDEN IT. Bcc/BRA, DBcc, MOVE/MOVEA
 * with both EAs in Dn/An/(An)/d16(An), and register-destination ALU. MOVEQ is M1a item 1
 * and is not built here; (An)+ and -(An) are excluded in either operand because they arm
 * mmufixup and §0.3(b)'s invariant is that only the interpreter ever arms it; BSR is a
 * fully memory-touching family; every write-back family, every FPU opcode, every privileged
 * or mode-changing instruction, MOVE16, every uncompiled opcode and every rung-0 PC bail.
 *
 * THE AUDIT §0.5.1 DEMANDED, and its answer is why none of the existing compiler bodies
 * appear below. §0.3(a) calls Bcc (45/45) and DBcc (16/16) "inheritable as-is" and §0.5.1
 * already narrowed that to "a statement about the memory model, and only about it". Read
 * against the bail contract the answer is stronger: the existing bodies compute targets and
 * chain blocks through regs.pc_p and cache_tags -- the host-pointer keying §0.3(c) refuted
 * under the MMU -- and they operate on compemu's lazy virtual register file, whose three
 * lazinesses are exactly the state a bail would have to materialise. What is inherited is
 * the raw ARMv7 encoding layer and nothing above it.
 *
 * SIZES ARE DONE BY SHIFTING THE SIZE'S SIGN BIT UP TO BIT 31, not by sign-extending the
 * operands. Sign extension gets N and Z wrong for .W and .B -- (0x7FFF + 1) has bit 15 set
 * and bit 31 clear, and (0x8000 + 0x8000) is zero at 16 bits and not at 32 -- so the
 * operands are shifted left by 16 or 24 instead, the ARM flags then mean what the 68k flags
 * mean at that size, and the result is shifted back. The reference is the generated
 * interpreter in cpuemu_31.cpp, which computes flags explicitly and is what the lockstep
 * oracle compares against; it is not machdep/m68k.h's optflag_* macros, which this build
 * does not use. */

#define W_SZ_B 0
#define W_SZ_W 1
#define W_SZ_L 2
static const unsigned warp_shift[3] = { 24u, 16u, 0u };
static const unsigned warp_width[3] = {  8u, 16u, 32u };
static const unsigned warp_bytes_of[3] = { 1u, 2u, 4u };

/* 68k condition -> ARM condition, with the stored carry taken to ARM's sense first.
 *
 * The engine stores the 68k C in bit 29, and for SUB/CMP the 68k C is the BORROW while
 * ARM's is its complement. Every condition evaluation therefore inverts bit 29 before
 * pushing the word into the flags, after which N, Z and V mean the same thing in both
 * architectures and C means ARM's -- so CC/CS swap and everything else maps straight
 * across. Doing it this way costs one EOR and lets the silicon evaluate GE/LT/GT/LE, which
 * are the conditions a hand-rolled bit test gets wrong. */
static const unsigned warp_cc_map[16] = {
   CC_AL, CC_AL,          /* 0 T (BRA), 1 F/BSR -- both handled before this table */
   CC_HI, CC_LS,          /* 2 HI, 3 LS  */
   CC_CS, CC_CC,          /* 4 CC -> ARM CS, 5 CS -> ARM CC  (the inversion) */
   CC_NE, CC_EQ,          /* 6 NE, 7 EQ  */
   CC_VC, CC_VS,          /* 8 VC, 9 VS  */
   CC_PL, CC_MI,          /* 10 PL, 11 MI */
   CC_GE, CC_LT,          /* 12 GE, 13 LT */
   CC_GT, CC_LE           /* 14 GT, 15 LE */
};

/* Load the guest flags into the host's, in ARM sense. Leaves the working register free. */
static void e_load_flags(struct warp_cc *c, unsigned rt)
{
   e_ldr(c, rt, R_CTX, (int)CTX_NZCV);
   e_dp_imm(c, CC_AL, OP_EOR, 0, rt, rt, 0x20000000u);
   e_msr(c, rt);
}

/* Commit N,Z,C,V from the host flags. `inv` inverts the carry back to 68k sense (SUB, CMP);
 * `keep` is the mask of flag bits the 68k instruction defines -- 0xF0000000 for arithmetic,
 * 0xC0000000 for the MOVE/logical families, whose C and V are cleared outright. */
static void e_store_flags(struct warp_cc *c, unsigned rt, int inv, uae_u32 keep)
{
   e_mrs(c, rt);
   if (inv && !WARP_MUTANT(Z3660_WARP_MUT_FLAG_SUB))
      e_dp_imm(c, CC_AL, OP_EOR, 0, rt, rt, 0x20000000u);
   if (WARP_MUTANT(Z3660_WARP_MUT_FLAG_MOVE) && keep == 0xC0000000u)
      keep = 0xF0000000u;                            /* the defect, never shipped */
   e_dp_imm(c, CC_AL, OP_AND, 0, rt, rt, keep);
   e_str(c, rt, R_CTX, (int)CTX_NZCV);
}

/* COPY_CARRY(): the 68k X flag takes the value of C. One UBFX and one store, and it is the
 * whole of what "X" costs generation one. */
static void e_store_x(struct warp_cc *c, unsigned rt, unsigned rsrc)
{
   if (WARP_MUTANT(Z3660_WARP_MUT_FLAG_ADD))
      return;                                        /* the defect, never shipped */
   e_ubfx(c, rt, rsrc, 29, 1);
   e_str(c, rt, R_CTX, (int)CTX_XFLG);
}

/* Merge a computed value into a guest register, honouring the size. .L overwrites; .W and
 * .B leave the upper bits of the destination alone, which is the 68k rule and the reason
 * every narrow ALU handler in cpuemu_31 reads the destination back before storing it. */
static void e_put_reg(struct warp_cc *c, unsigned n, unsigned sz, unsigned rval,
                      unsigned rold, unsigned rtmp)
{
   if (sz == W_SZ_L) { e_str(c, rval, R_REGS, (int)(n * 4u)); return; }
   e_mov(c, rtmp, rold);
   e_bfi(c, rtmp, rval, 0, warp_width[sz]);
   e_str(c, rtmp, R_REGS, (int)(n * 4u));
}

/* ---- effective addresses ------------------------------------------------------------
 *
 * Four modes and no more: Dn, An, (An) and d16(An). The two the plan drops -- (An)+ and
 * -(An) -- are dropped because they arm mmufixup, and §0.3(b) states the freeze's own
 * invariant: mmufixup is disarmed at every block head because only the interpreter ever
 * arms it. Whether a predecrement/postincrement SOURCE can be admitted is a named M1a
 * scoping question with a static answer, and it is not answered here. */
#define EA_DREG 0
#define EA_AREG 1
#define EA_IND  2
#define EA_D16  5

static int warp_ea_ok(unsigned mode, unsigned sz, int is_dst)
{
   if (mode == EA_DREG) return 1;
   if (mode == EA_AREG) return (sz != W_SZ_B);   /* An is never a byte operand           */
   if (mode == EA_IND)  return 1;
   if (mode == EA_D16)  return 1;
   (void)is_dst;
   return 0;
}

/* Compute a memory EA into r0. Only reached for EA_IND and EA_D16. */
static void e_ea_addr(struct warp_cc *c, unsigned mode, unsigned reg, uae_u32 d16)
{
   e_ldr(c, 0, R_REGS, (int)((8u + reg) * 4u));
   if (mode == EA_D16) {
      unsigned rot, i8;
      uae_u32 v = (uae_u32)(uae_s32)(uae_s16)d16;
      if (warp_imm8(v, &rot, &i8))
         e_dp_imm(c, CC_AL, OP_ADD, 0, 0, 0, v);
      else if (warp_imm8((uae_u32)(-(uae_s32)v), &rot, &i8))
         e_dp_imm(c, CC_AL, OP_SUB, 0, 0, 0, (uae_u32)(-(uae_s32)v));
      else {
         e_const(c, 1, v);
         e_dp_reg(c, CC_AL, OP_ADD, 0, 0, 0, 1, 0, 0);
      }
   }
}

/* ==================================================================================== */

/* The compiler's own instruction fetch. It runs inside the TRY of warp_compile(), so an
 * unmapped page during compilation ends the compile rather than the boot -- and Q6's
 * WARP_CFETCH counts here and NOT in the interpreter's ifetch bracket, which is precisely
 * what makes §0.5.6's declared fall of FETCH/IFETCH_CALLS arithmetically possible. */
static uae_u16 warp_iword(struct warp_cc *c, unsigned addr)
{
   c->cfetch++;
   /* mmu_get_iword() AND NOT uae_mmu040_get_iword(), and §0.5.6 is the authority for the
    * choice rather than performance: the wrapper carries the interpreter's ifetch bracket,
    * and FETCH / IFETCH_CALLS are pre-registered to FALL with the covered share. A compiler
    * wired through that bracket would make the declared fall arithmetically impossible --
    * "a counter reached only after the branch being deleted cannot fall when it is
    * deleted". The compiler's reads are counted by WARP_CFETCH and nowhere else, which is
    * exactly what makes Q6's counter mean something. The harness catches the mistake: with
    * the wrapper in place FETCH read 8 armed against 0 disarmed. */
   return mmu_get_iword((uaecptr)addr, sz_word);
}

/* Compile ONE guest instruction at `pc`. Returns its length in bytes, or 0 if the opcode is
 * outside generation one's scope. `terminates` is set when the instruction ends the block.
 *
 * `count` is the number of guest instructions ALREADY retired when this one starts, which
 * is what every exit inside it returns -- the per-site constant of shape (ii). */
static unsigned warp_emit_insn(struct warp_cc *c, unsigned pc, unsigned count,
                               int *terminates)
{
   uae_u16 op = warp_iword(c, pc);
   unsigned sz, smode, sreg, dmode, dreg, len;
   uae_u32 sd16 = 0, dd16 = 0;

   *terminates = 0;

   /* c->last_ipc / c->last_restart HOLD THE PREVIOUS INSTRUCTION'S POST-STATE FOR THE WHOLE
    * OF THIS FUNCTION, and that is load-bearing rather than incidental: a bail site inside
    * this instruction leaves with the instruction UNEXECUTED, so the state it must present
    * is the one the instruction before it left. Every success path below writes the new
    * values at its own end, after the last site that could have declined.
    *
    * Writing them on entry instead is a defect the differential caught within one run --
    * a bail four instructions into the corpus reported instruction_pc two bytes high. */

   /* ------------------------------------------------ MOVE / MOVEA ------------------- */
   if ((op & 0xC000u) == 0x0000u && (op & 0x3000u) != 0x0000u) {
      unsigned szf = (op >> 12) & 3u;
      sz = (szf == 1u) ? (unsigned)W_SZ_B : (szf == 3u) ? (unsigned)W_SZ_W : (unsigned)W_SZ_L;
      smode = (op >> 3) & 7u; sreg = op & 7u;
      dmode = (op >> 6) & 7u; dreg = (op >> 9) & 7u;
      if (!warp_ea_ok(smode, sz, 0) || !warp_ea_ok(dmode, sz, 1))
         return 0;
      if (sz == W_SZ_B && (smode == EA_AREG || dmode == EA_AREG))
         return 0;                                   /* MOVE.B has no An operand        */
      len = 2;
      if (smode == EA_D16) { sd16 = warp_iword(c, pc + len); len += 2; }
      if (dmode == EA_D16) { dd16 = warp_iword(c, pc + len); len += 2; }

      /* 1. the source. A memory source may decline, and nothing architectural has
       *    happened yet when it does. */
      if (smode == EA_DREG)      e_ldr(c, 2, R_REGS, (int)(sreg * 4u));
      else if (smode == EA_AREG) e_ldr(c, 2, R_REGS, (int)((8u + sreg) * 4u));
      else {
         e_ea_addr(c, smode, sreg, sd16);
         e_dp_imm(c, CC_AL, OP_MOV, 0, 1, 0, warp_bytes_of[sz]);
         e_blx(c, R_RD);
         e_dp_imm(c, CC_AL, OP_CMP, 1, 0, 0, 0);
         e_bail_if(c, CC_EQ, pc, count);
         e_ldr(c, 2, R_CTX, (int)CTX_TMP);
      }

      /* 2. the flags, COMPUTED BUT NOT COMMITTED. r8 survives the helper call, which is
       *    the whole reason it is pushed: if the destination write declines, the flags
       *    must not have moved. The interpreter would not have moved them either. */
      if (dmode != EA_AREG) {
         if (WARP_MUTANT(Z3660_WARP_MUT_FLAG_MOVE))
            e_load_flags(c, 3);                      /* the defect, never shipped        */
         e_shift(c, 1, 3, 2, 0, warp_shift[sz]);     /* movs r3, r2 lsl #k              */
         e_mrs(c, R_KEEP);
         e_dp_imm(c, CC_AL, OP_AND, 0, R_KEEP, R_KEEP,
                  WARP_MUTANT(Z3660_WARP_MUT_FLAG_MOVE) ? 0xF0000000u : 0xC0000000u);
      }

      /* 3. the destination. */
      if (dmode == EA_DREG) {
         if (sz == W_SZ_L) e_str(c, 2, R_REGS, (int)(dreg * 4u));
         else {
            e_ldr(c, 3, R_REGS, (int)(dreg * 4u));
            e_bfi(c, 3, 2, 0, warp_width[sz]);
            e_str(c, 3, R_REGS, (int)(dreg * 4u));
         }
      } else if (dmode == EA_AREG) {
         if (sz == W_SZ_W) e_sxth(c, 2, 2);          /* MOVEA.W sign-extends to 32       */
         e_str(c, 2, R_REGS, (int)((8u + dreg) * 4u));
      } else {
         e_ea_addr(c, dmode, dreg, dd16);
         e_dp_imm(c, CC_AL, OP_MOV, 0, 1, 0, warp_bytes_of[sz]);
         e_blx(c, R_WR);
         e_dp_imm(c, CC_AL, OP_CMP, 1, 0, 0, 0);
         e_bail_if(c, CC_EQ, pc, count);
      }

      /* 4. commit the flags, after the last thing that could have declined. */
      if (dmode != EA_AREG)
         e_str(c, R_KEEP, R_CTX, (int)CTX_NZCV);
      /* 5. the write-back shape, recorded for whatever exit comes next. A MOVE to memory
       *    is §2e's write-back family: the interpreter advances the pc, sets
       *    regs.instruction_pc to the ADVANCED pc and clears mmu_restart BEFORE it stores,
       *    so a fault there is a write-back fault on an architecturally complete
       *    instruction. Generated code never takes that fault -- it bails first -- but it
       *    must leave the same state behind when the store SUCCEEDS. */
      if (dmode == EA_IND || dmode == EA_D16) {
         c->last_ipc = pc + len;
         c->last_restart = 0;
      } else {
         c->last_ipc = pc;
         c->last_restart = 1;
      }
      return len;
   }

   /* ------------------------------------------------ ADDQ / SUBQ -------------------- */
   if ((op & 0xF000u) == 0x5000u && ((op >> 6) & 3u) != 3u) {
      unsigned data = (op >> 9) & 7u;
      unsigned issub = (op >> 8) & 1u;
      unsigned mode  = (op >> 3) & 7u;
      unsigned reg   = op & 7u;
      if (data == 0) data = 8;
      sz = (unsigned)((op >> 6) & 3u);               /* 0=B 1=W 2=L, and the table order
                                                      * is the same as W_SZ_*           */
      if (mode == EA_AREG) {
         if (sz == W_SZ_B) return 0;                 /* ADDQ.B to An is illegal          */
         e_ldr(c, 0, R_REGS, (int)((8u + reg) * 4u));
         e_dp_imm(c, CC_AL, issub ? OP_SUB : OP_ADD, 0, 0, 0, data);
         e_str(c, 0, R_REGS, (int)((8u + reg) * 4u));
         c->last_ipc = pc; c->last_restart = 1;
         return 2;                                   /* An destinations set no flags     */
      }
      if (mode != EA_DREG) return 0;
      e_ldr(c, 0, R_REGS, (int)(reg * 4u));
      e_shift(c, 0, 2, 0, 0, warp_shift[sz]);
      e_dp_imm(c, CC_AL, issub ? OP_SUB : OP_ADD, 1, 2, 2,
               (uae_u32)data << warp_shift[sz]);
      e_store_flags(c, 12, (int)issub, 0xF0000000u);
      e_store_x(c, 3, 12);
      if (sz != W_SZ_L) e_shift(c, 0, 2, 2, 1, warp_shift[sz]);
      e_put_reg(c, reg, sz, 2, 0, 3);
      c->last_ipc = pc; c->last_restart = 1;
      return 2;
   }

   /* ------------------------------------------------ DBcc --------------------------- */
   if ((op & 0xF0F8u) == 0x50C8u) {
      unsigned cond = (op >> 8) & 0xFu;
      unsigned reg  = op & 7u;
      uae_u16  offs = warp_iword(c, pc + 2);
      unsigned pc_fall = pc + 4u;
      unsigned pc_take = (unsigned)(pc + 2u + (uae_u32)(uae_s32)(uae_s16)offs);
      unsigned n = count + 1u;

      if (offs & 1u) return 0;                       /* an odd target is exception 3     */
      *terminates = 1;
      c->last_ipc = pc; c->last_restart = 1;

      /* cond TRUE -> the loop is over: pc = pc + 4, and m68k_setpci() means
       * regs.instruction_pc moves with it. */
      if (cond == 0) {                               /* DBT: always true                 */
         e_exit(c, pc_fall, pc_fall, 1, n);
         return 4;
      }
      if (cond != 1u) {                              /* DBF/DBRA never evaluates         */
         e_load_flags(c, 0);
         e_b(c, warp_cc_map[cond] ^ 1u, (int)e_exit_words(pc_fall, pc_fall));
         e_exit(c, pc_fall, pc_fall, 1, n);
      }
      /* cond FALSE -> decrement Dn.w and branch if the OLD value was non-zero. The order
       * is the interpreter's: the pc is set first, then the register is decremented, then
       * the OLD value decides -- so the test is emitted before the write-back. */
      e_ldr(c, 0, R_REGS, (int)(reg * 4u));
      if (WARP_MUTANT(Z3660_WARP_MUT_FLAG_DBCC)) {
         e_dp_imm(c, CC_AL, OP_SUB, 0, 2, 0, 1);     /* the defect: test AFTER the        */
         e_shift(c, 1, 3, 2, 0, 16);                 /* decrement, never shipped          */
         e_bfi(c, 0, 2, 0, 16);
      } else {
         e_shift(c, 1, 3, 0, 0, 16);                 /* movs r3, r0 lsl #16 -- the OLD Dn.w */
         e_dp_imm(c, CC_AL, OP_SUB, 0, 2, 0, 1);
         e_bfi(c, 0, 2, 0, 16);
      }
      e_str(c, 0, R_REGS, (int)(reg * 4u));
      e_b(c, CC_NE, (int)e_exit_words(pc_fall, pc_fall));  /* old != 0 -> branch          */
      e_exit(c, pc_fall, pc_fall, 1, n);             /* counter expired: pc + 4, ipc too  */
      e_exit(c, pc_take, pc, 1, n);                  /* taken: m68k_incpci, ipc unmoved   */
      return 4;
   }

   /* ------------------------------------------------ Bcc / BRA ---------------------- */
   if ((op & 0xF000u) == 0x6000u) {
      unsigned cond = (op >> 8) & 0xFu;
      unsigned d8   = op & 0xFFu;
      unsigned n    = count + 1u;
      uae_u32  disp;
      unsigned pc_fall, pc_take;

      if (cond == 1u) return 0;                      /* BSR: a memory-touching family    */
      if (d8 == 0xFFu) return 0;                     /* the 32-bit form: M1a, not here   */
      if (d8 == 0) {
         uae_u16 w = warp_iword(c, pc + 2);
         if (w & 1u) return 0;
         disp = (uae_u32)(uae_s32)(uae_s16)w;
         len = 4;
      } else {
         if (d8 & 1u) return 0;
         disp = (uae_u32)(uae_s32)(uae_s8)d8;
         len = 2;
      }
      pc_fall = pc + len;
      pc_take = (unsigned)(pc + 2u + disp);
      *terminates = 1;
      c->last_ipc = pc; c->last_restart = 1;         /* m68k_incpci: ipc does not move    */

      if (cond == 0) { e_exit(c, pc_take, pc, 1, n); return len; }
      e_load_flags(c, 0);
      {
         unsigned acc = warp_cc_map[cond];
         if (WARP_MUTANT(Z3660_WARP_MUT_FLAG_BCC)) acc ^= 1u;   /* never shipped */
         e_b(c, acc ^ 1u, (int)e_exit_words(pc_take, pc));
      }
      e_exit(c, pc_take, pc, 1, n);
      e_exit(c, pc_fall, pc, 1, n);
      return len;
   }

   /* ------------------------------------------------ register ALU ------------------- */
   {
      unsigned fam = (op >> 12) & 0xFu;
      unsigned opm = (op >> 6) & 7u;
      unsigned mode = (op >> 3) & 7u;
      unsigned src  = op & 7u;
      unsigned dst  = (op >> 9) & 7u;
      unsigned aop, inv = 0, logical = 0, wb = 1, xcopy = 0;

      if (fam != 0x8u && fam != 0x9u && fam != 0xBu && fam != 0xCu && fam != 0xDu)
         return 0;

      /* ADDA / SUBA / CMPA -- opmode 011 (.W) and 111 (.L), destination An. */
      if (opm == 3u || opm == 7u) {
         unsigned asz = (opm == 3u) ? (unsigned)W_SZ_W : (unsigned)W_SZ_L;
         if (fam == 0xCu || fam == 0x8u) return 0;   /* no ANDA / ORA exist              */
         if (mode != EA_DREG && mode != EA_AREG) return 0;
         e_ldr(c, 1, R_REGS, (int)(((mode == EA_AREG ? 8u : 0u) + src) * 4u));
         if (asz == W_SZ_W) e_sxth(c, 1, 1);
         e_ldr(c, 0, R_REGS, (int)((8u + dst) * 4u));
         if (fam == 0xBu) {                          /* CMPA: flags at LONG size, no X   */
            e_dp_reg(c, CC_AL, OP_CMP, 1, 0, 0, 1, 0, 0);
            e_store_flags(c, 12, 1, 0xF0000000u);
            c->last_ipc = pc; c->last_restart = 1;
            return 2;
         }
         e_dp_reg(c, CC_AL, (fam == 0x9u) ? OP_SUB : OP_ADD, 0, 0, 0, 1, 0, 0);
         e_str(c, 0, R_REGS, (int)((8u + dst) * 4u));
         c->last_ipc = pc; c->last_restart = 1;
         return 2;                                   /* ADDA/SUBA set no flags           */
      }

      sz = opm & 3u;
      if (sz == 3u) return 0;
      if (opm < 4u) {
         /* <ea>,Dn -- and for the 0xB family this range is CMP, not EOR. */
         if (mode != EA_DREG && !(mode == EA_AREG && (fam == 0x9u || fam == 0xDu
                                                      || fam == 0xBu) && sz != W_SZ_B))
            return 0;
      } else {
         /* Dn,<ea> -- mode 0 and 1 in this range are ADDX/SUBX/CMPM, never the register
          * form, so only the 0xB family (EOR Dn,Dn) survives it. */
         if (fam != 0xBu || mode != EA_DREG) return 0;
      }

      switch (fam) {
      case 0x8u: aop = OP_ORR; logical = 1; break;
      case 0x9u: aop = OP_SUB; inv = 1; xcopy = 1; break;
      case 0xBu: if (opm < 4u) { aop = OP_CMP; inv = 1; wb = 0; }
                 else          { aop = OP_EOR; logical = 1; }
                 break;
      case 0xCu: aop = OP_AND; logical = 1; break;
      default:   aop = OP_ADD; xcopy = 1; break;     /* 0xD: ADD                         */
      }
      if (fam == 0x8u && mode == EA_AREG) return 0;
      if (fam == 0xCu && mode == EA_AREG) return 0;

      {
         unsigned rsrc = (mode == EA_AREG ? 8u : 0u) + src;
         unsigned rdst = dst;
         if (opm >= 4u) { rsrc = dst; rdst = src; }  /* EOR Dn,Dn: the roles swap         */
         e_ldr(c, 0, R_REGS, (int)(rdst * 4u));
         e_ldr(c, 1, R_REGS, (int)(rsrc * 4u));
         if (logical) {
            e_dp_reg(c, CC_AL, aop, 0, 2, 0, 1, 0, 0);
            e_shift(c, 1, 3, 2, 0, warp_shift[sz]);
            e_store_flags(c, 12, 0, 0xC0000000u);
            e_put_reg(c, rdst, sz, 2, 0, 3);
         } else {
            e_shift(c, 0, 2, 0, 0, warp_shift[sz]);
            e_shift(c, 0, 3, 1, 0, warp_shift[sz]);
            e_dp_reg(c, CC_AL, aop, 1, wb ? 2u : 0u, 2, 3, 0, 0);
            e_store_flags(c, 12, (int)inv, 0xF0000000u);
            if (xcopy) e_store_x(c, 3, 12);
            if (wb) {
               if (sz != W_SZ_L) e_shift(c, 0, 2, 2, 1, warp_shift[sz]);
               e_put_reg(c, rdst, sz, 2, 0, 3);
            }
         }
      }
      c->last_ipc = pc; c->last_restart = 1;
      return 2;
   }
}

/* ==================================================================================== */

/* THE I-CACHE PUBLISH, and it is Q3's augmented sequence rather than the shipped one.
 *
 * The shipped pair is arm_flush_cache (DCCIMVAC per line + DSB) then arm_icache_invalidate
 * (ICIMVAU per line + DSB), which is what flush_cpu_icache() composes and what every
 * generated byte in this firmware has been published with. Q3 decided to add the ISB and
 * the branch-predictor invalidate BEFORE the engine raises flush volume, not after a symptom
 * appears -- and this is byte-for-byte the sequence stub B of the Q2 unwind probe ran under
 * on metal on 2026-08-23, which is the only volume evidence that exists for it.
 *
 * The standing platform caveat rides with it unchanged and is not softened here: ADDING
 * BARRIERS IS NOT EVIDENCE OF A FIX. Only a soak on metal is, and Q3's UAEJIT A/B is still
 * a separate session.
 *
 * Core1 generates and core1 executes, so no cross-core maintenance is owed for the engine's
 * own output. That is NOT true of guest instruction bytes, which core0 can write by DMA --
 * and that is what the IV_DMA arm of the invalidation contract is for. */
static void warp_publish(void *p, unsigned bytes)
{
#ifndef HOST_TEST_HARNESS
   arm_flush_cache((uintptr_t)p, bytes);
   arm_icache_invalidate((uintptr_t)p, bytes);
   __asm__ __volatile__("mcr p15, 0, %0, c7, c5, 6" :: "r"(0) : "memory");   /* BPIALL */
   __asm__ __volatile__("dsb" ::: "memory");
   __asm__ __volatile__("isb" ::: "memory");
#else
   (void)p; (void)bytes;
#endif
   WARP_STAT(Z3660_WARP_STAT_PUBLISH);
}

/* ---------------------------------------------------------------- invalidation ------- */

/* Mark the whole table stale. O(1): validity is a generation stamp, so "discard everything"
 * is affordable enough to be the policy rather than an admission of defeat -- the decoded-op
 * cache's own argument, and the code cache needs it more, because a discarded BLOCK costs
 * one to three orders of magnitude more to rebuild than a discarded decode.
 *
 * WARP_DISC is charged BEFORE the generation moves, because after it moves there is nothing
 * left to count. */
static void warp_discard_all(int cause)
{
   WARP_STAT_ADD(Z3660_WARP_STAT_DISC, warp_live);
   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_DISC, warp_live);
   if (cause == Z3660_DOPC_INV_ROOT) {
      WARP_STAT_ADD(Z3660_WARP_STAT_DISCROOT, warp_live);
      Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_DISC_ROOT, warp_live);
   }
   warp_live = 0;

   if (++z3660_warp_gen == 0) {
      /* 2^32 discards later. Every stamp must be made unequal to the generation we are
       * about to resume at, and zeroing does that for generation 1 -- so this is a real
       * clear and not a wrap that silently revalidates the whole table. */
      memset(z3660_warp_tab, 0, sizeof z3660_warp_tab);
      z3660_warp_gen = 1;
   }
   z3660_warp_last_pc = Z3660_WARP_NO_LAST;
}

/* THE CODE CACHE'S HOOK ON THE DOPC'S CHOKE POINT, called for EVERY cause.
 *
 * It does not invent its own call sites: mmu_flush_cache() and its nine siblings already
 * funnel into z3660_dopc_invalidate(), and joining there is what makes a NEW upstream cause
 * have to be opted OUT of invalidating the code cache rather than into it. That property is
 * the whole reason the choke point exists, and a second enumeration would lose it.
 *
 * Q5 -- whether a per-address-space generation is worth funding -- is answered by
 * WARP_DISC_ROOT against WARP_DISC, and the counters are live from day one precisely so the
 * question is settled by a ratio rather than by an argument. Nothing is narrowed here: for
 * the code cache the two largest causes name PHYSICAL addresses while every tag is LOGICAL,
 * and a range test across that namespace wall ships corruption. */
extern "C" void z3660_warp_invalidate(int cause)
{
   if (!z3660_warp_on)
      return;   /* nothing was compiled while disarmed, so there is nothing to discard */
   warp_discard_all(cause);
}

/* The code cache is full. Throw everything away and reset the bump pointer: §2b's decision
 * for generation one, kept because it is simple, provably correct, and composes with the
 * generation counter -- a full flush is one generation bump plus a pointer reset.
 *
 * Round 1 never reached this path: WARP_CBYTES filled 73 % of the 32 MiB arena in one 34 s
 * window and the window ended before the arena wrapped, which is "not yet" and not "never".
 * The harness reaches it deliberately, with the limit lowered, because a path that has
 * never run is not a path that works. */
static void warp_flush_cache(void)
{
   WARP_STAT(Z3660_WARP_STAT_FLUSH);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_FLUSH);
   warp_cp = warp_arena;
   warp_discard_all(-1);
}

/* ---------------------------------------------------------------- init --------------- */

/* ROUND 1'S KNOWN GAP, FIXED IN THREE LINES AND WORTH THE PARAGRAPH. Round 1 latched
 * `warp_inited` on the first call and never asked for the arena again, so an engine that
 * asked before the classic JIT had released it -- or before create_popalls() had run at all
 * -- stayed permanently declined for the rest of the boot, reporting the fact in
 * WARP_DECLINE and looking exactly like a correctly-refusing engine. The arena acquisition
 * is now RETRIED on every arm, and the table initialisation is idempotent. */
static void warp_init(void)
{
   unsigned i;

   if (!warp_inited) {
      for (i = 0; i < Z3660_WARP_SETS; i++) {
         z3660_warp_tab[i].tag[0] = z3660_warp_tab[i].tag[1] =
         z3660_warp_tab[i].tag[2] = z3660_warp_tab[i].tag[3] =
            (unsigned int)Z3660_WARP_TAG_EMPTY(i);
         z3660_warp_tab[i].stamp  = 0;             /* generation 0 is never live */
         z3660_warp_tab[i].victim = 0;
      }
      warp_live   = 0;
      warp_inited = 1;
   }

#ifdef HOST_TEST_HARNESS
   if (warp_arena == NULL) {
      warp_bytes = 1024u * 1024u;
      warp_arena = (uae_u8 *)malloc(warp_bytes);
   }
   memset(warp_published, 0, sizeof warp_published);
#else
   if (warp_arena == NULL)
      warp_arena = z3660_warp_cache_arena(&warp_bytes);   /* RETRIED, never latched */
#endif
   if (warp_arena == NULL)
      warp_bytes = 0;
   if (warp_limit == 0 || warp_limit > warp_bytes)
      warp_limit = warp_bytes;
   warp_cp = warp_arena;
}

/* ---------------------------------------------------------------- compile ------------ */

static __inline__ void warp_decline(int reason)
{
   (void)reason;
   WARP_STAT(Z3660_WARP_STAT_DECLINE);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_DECLINE);
}

/* Fill one way of the set, evicting a live block if that is what the victim pointer lands
 * on -- and CHARGING IT to WARP_EVICT, which is the term round 1 had no way to see. A
 * recompile budget that cannot separate conflict from invalidation from capacity is what
 * left 95.2 % of 3 066 944 compiles unattributed. */
static struct z3660_warp_set *warp_fill(unsigned idx, uae_u32 key, void *code, unsigned len,
                                        unsigned *out_way)
{
   struct z3660_warp_set *s = &z3660_warp_tab[idx];
   unsigned w;

   if (s->stamp != z3660_warp_gen) {
      /* A set from an older generation is reclaimed here rather than by a sweep: the cost
       * of clearing a set is paid by the first fill that wants it, and a set nothing
       * touches again is never cleared at all. The dopc's own argument, inherited. */
      s->stamp  = z3660_warp_gen;
      s->tag[0] = s->tag[1] = s->tag[2] = s->tag[3] = (unsigned int)Z3660_WARP_TAG_EMPTY(idx);
      s->victim = 0;
   }
   for (w = 0; w < Z3660_WARP_WAYS; w++)
      if (s->tag[w] == (unsigned int)Z3660_WARP_TAG_EMPTY(idx) || s->tag[w] == key) break;
   if (w == Z3660_WARP_WAYS) {
      w = s->victim & (Z3660_WARP_WAYS - 1u);
      s->victim = (s->victim + 1u) & (Z3660_WARP_WAYS - 1u);
      WARP_STAT(Z3660_WARP_STAT_EVICT);
      Z3660_PROF_CNT(Z3660_PROF_C_WARP_EVICT);
      if (warp_live) warp_live--;
   } else if (s->tag[w] != key) {
      warp_live++;                                  /* an empty way becomes a live one   */
   }

   s->entry[w] = (z3660_warp_fn)code;
   s->len[w]   = (unsigned short)len;
   s->tag[w]   = key;                               /* THE TAG LAST: everything above it
                                                     * is dead until this makes the way
                                                     * findable, so no lookup can ever
                                                     * see a half-built entry.           */
   *out_way = w;
   return s;
}

/* Compile the block at `pc`. Returns the way it landed in, or -1.
 *
 * IT RUNS INSIDE A TRY. The compiler reads guest instruction words through the SAME
 * accessor the interpreter uses -- which is what keeps the translation identical, and the
 * ISSUE-52 I-versus-D finding is why that matters -- and that accessor can fault. A fault
 * during compilation is not the guest's fault to take here: the compile is abandoned, the
 * head is declined, and the interpreter dispatches the same PC and takes the fault properly
 * through the run loop's own CATCH. There is no generated frame on the stack while this
 * runs, so Q2's TERMINATED finding does not apply to it. */
static int warp_compile(unsigned pc, uae_u32 key, unsigned idx, unsigned *out_way,
                        unsigned *out_len, unsigned *out_bytes)
{
   struct warp_cc c;
   uae_u8 *dst;
   unsigned delta = 0, bytes, pagelim;
   int failed = 0, ended_at_terminator = 0;

   if (warp_arena == NULL || warp_limit < 64u) {
      warp_decline(Z3660_WARP_DECL_NOCACHE);
      return -1;
   }
   if ((unsigned)(warp_cp - warp_arena) + Z3660_WARP_MAX_BYTES > warp_limit)
      warp_flush_cache();

   dst = warp_cp;
   memset(&c, 0, sizeof c);
   c.w    = (uae_u32 *)dst;
   c.cap  = Z3660_WARP_MAX_BYTES / 4u;
   c.head = pc;
   c.last_ipc = pc;
   c.last_restart = 1;
   c.ok   = 1;

   /* THE BLOCK NEVER SPANS A PAGE. One instruction-fetch validity gate then covers the
    * whole block, which is the structural rule §2a states and the reason a block's own
    * bytes can never straddle two translations. */
   pagelim = (unsigned)((pc | (unsigned)mmu_pagemask) + 1u);

   e_push(&c, WARP_PUSH_LIST);
   e_mov(&c, R_REGS, 0);
   e_mov(&c, R_RD,   1);
   e_mov(&c, R_WR,   2);
   e_mov(&c, R_CTX,  3);

   TRY(prb) {
      while (c.ok && c.insns < Z3660_WARP_MAX_INSNS) {
         unsigned before = c.n;
         unsigned keep_ipc = c.last_ipc, keep_restart = c.last_restart;
         int term = 0;
         unsigned len;

         if (pc + delta + 2u > pagelim) break;
         len = warp_emit_insn(&c, pc + delta, c.insns, &term);
         /* A REFUSED INSTRUCTION MUST LEAVE NO TRACE, and the restart bookkeeping is part
          * of the trace: warp_emit_insn() writes its default post-state before it knows
          * whether it can compile the opcode at all, so an opcode outside the scope would
          * otherwise hand the block's exit the post-state of an instruction that never ran.
          * The differential sweep caught exactly that, as `MOVE.B D0,D0` leaving
          * instruction_pc two bytes high. */
         if (len == 0 || !c.ok) {
            c.n = before; c.last_ipc = keep_ipc; c.last_restart = keep_restart; break;
         }
         if (pc + delta + len > pagelim) {
            c.n = before; c.last_ipc = keep_ipc; c.last_restart = keep_restart; break;
         }
         delta += len;
         c.insns++;
         if (term) { ended_at_terminator = 1; break; }
      }
   } CATCH(prb) {
      /* An unmapped or protected instruction page during compilation. Abandon; the
       * interpreter is about to fetch the same word and take the fault properly. */
      failed = 1;
   } ENDTRY

   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_CFETCH, c.cfetch);

   if (failed || !c.ok) {
      warp_decline(failed ? Z3660_WARP_DECL_FETCH : Z3660_WARP_DECL_HEAD);
      c.n = 0;
      c.insns = 0;
   }

   if (c.insns == 0) {
      /* A DECLINED HEAD IS CACHED, and that is one of round 2's four fixes. Round 1
       * re-ran the whole discovery at every arrival of a PC it had already refused; a
       * decline is a property of the PC until the next invalidation, so it is compiled to
       * exactly round 1's two-word null block -- `mov r0,#0` then `bx lr`. The fast path
       * needs no NULL test, the decline costs one lookup, and the two words the harness
       * has always asserted are still the two words that go out. */
      if (!failed) warp_decline(Z3660_WARP_DECL_HEAD);
      c.n = 0;
      c.w[c.n++] = 0xE3A00000u;   /* mov r0, #0 */
      c.w[c.n++] = 0xE12FFF1Eu;   /* bx  lr     */
   } else {
      /* Only when the block ran off the end of its compiled span. A block that ended at a
       * terminator has already exited through the terminator's own arms, and emitting a
       * fall-through exit after them is unreachable code in a cache whose size is a
       * first-class metric. */
      if (!ended_at_terminator)
         e_exit(&c, pc + delta, c.last_ipc, c.last_restart, c.insns);
      if (!c.ok) {                                   /* the exit did not fit: refuse      */
         c.n = 0;
         c.w[c.n++] = 0xE3A00000u;
         c.w[c.n++] = 0xE12FFF1Eu;
         c.insns = 0;
      }
   }

   bytes = c.n * 4u;
   warp_cp += bytes;

   /* PUBLISHED BEFORE IT IS REACHABLE, and the ordering is the whole discipline: the block
    * is not written into the table until the I-cache sequence has run, so there is no window
    * in which a lookup could hand the run loop a pointer to bytes the I-cache has not seen.
    * `8be8949` is this repository's own receipt for what the other ordering costs -- the
    * first pushall_call_handler() fetched stale I-cache lines and the board rebooted with an
    * ARM Undefined Instruction, caused by a compiler-flag change. */
#ifdef HOST_TEST_HARNESS
   if (!WARP_MUTANT(Z3660_WARP_MUT_NO_ICACHE))
      warp_publish(dst, bytes);
#else
   warp_publish(dst, bytes);
#endif

   warp_fill(idx, key, dst, c.insns, out_way);
#ifdef HOST_TEST_HARNESS
   warp_published[idx][*out_way] = WARP_MUTANT(Z3660_WARP_MUT_NO_ICACHE) ? 0 : 1;
#endif

   WARP_STAT(Z3660_WARP_STAT_COMPILE);
   WARP_STAT_ADD(Z3660_WARP_STAT_CBYTES, bytes);
   WARP_STAT_ADD(Z3660_WARP_STAT_CINSNS, c.insns);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_COMPILE);
   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_CINSNS, c.insns);
   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_CBYTES, bytes);
   *out_len   = c.insns;
   *out_bytes = bytes;
   return 0;
}

/* ---------------------------------------------------------------- the flag seam ------ */

/* PACK AND COMMIT THE GUEST CCR, ONCE PER BLOCK -- and on the target it is a COPY.
 *
 * `flag_struct` has four different shapes across the machdep arms: nzcv in ARM CPSR bit
 * positions on ARM/AArch64/RISC-V, cznv on x86, five separate ints in the portable one.
 * Generated code works in the ARM layout because that is what the silicon hands it, so on
 * the arm this firmware actually ships the pack and the commit are a masked load and a
 * masked store. Everywhere else -- which in practice means the x86-64 host harness -- they
 * go through the architectural accessors, which is what makes the SAME emitted words
 * correct in both places.
 *
 * IT IS WORTH THE #if, MEASURED: written through the accessors unconditionally, gcc
 * vectorised the read-modify-write of regs.ccrflags into a NEON load/and/or/store pair and
 * charged the block entry about twenty ARM instructions to move four bits. The masked form
 * is six. Clearing nzcv's low bits is not a loss: CLEAR_CZNV() zeroes the whole word, every
 * reader takes bits 31..28, and cctrue() masks before it tests. */
#if defined(CPU_arm) || defined(CPU_AARCH64) || defined(CPU_riscv64)
#define WARP_FLAGS_PACK()                                                       \
   do {                                                                         \
      z3660_warp_ctx.nzcv = (uae_u32)regs.ccrflags.nzcv & 0xF0000000u;          \
      z3660_warp_ctx.xflg = (uae_u32)regs.ccrflags.x & 1u;                      \
   } while (0)
#define WARP_FLAGS_COMMIT()                                                     \
   do {                                                                         \
      regs.ccrflags.nzcv = z3660_warp_ctx.nzcv;                                 \
      regs.ccrflags.x    = z3660_warp_ctx.xflg;                                 \
   } while (0)
#else
#define WARP_FLAGS_PACK()                                                       \
   do {                                                                         \
      z3660_warp_ctx.nzcv = (uae_u32)((GET_NFLG() ? 0x80000000u : 0u)           \
                                    | (GET_ZFLG() ? 0x40000000u : 0u)           \
                                    | (GET_CFLG() ? 0x20000000u : 0u)           \
                                    | (GET_VFLG() ? 0x10000000u : 0u));         \
      z3660_warp_ctx.xflg = (uae_u32)(GET_XFLG() ? 1u : 0u);                    \
   } while (0)
#define WARP_FLAGS_COMMIT()                                                     \
   do {                                                                         \
      SET_NFLG((z3660_warp_ctx.nzcv & 0x80000000u) != 0);                       \
      SET_ZFLG((z3660_warp_ctx.nzcv & 0x40000000u) != 0);                       \
      SET_CFLG((z3660_warp_ctx.nzcv & 0x20000000u) != 0);                       \
      SET_VFLG((z3660_warp_ctx.nzcv & 0x10000000u) != 0);                       \
      if (!WARP_MUTANT(Z3660_WARP_MUT_BAILX))                                   \
         SET_XFLG((z3660_warp_ctx.xflg & 1u) != 0);                             \
   } while (0)
#endif

/* ---------------------------------------------------------------- entry -------------- */

#ifdef HOST_TEST_HARNESS
static unsigned warp_host_exec(const uae_u32 *code, void *regbase, void *rd, void *wr,
                               void *ctx);
#endif

/* Enter a cached block and commit what it did. Everything that is expensive about a block
 * entry that is not the block itself lives here, and it is called from the inlined fast
 * path so that the STEADY-STATE path pays no AAPCS prologue -- §0.7.3 measured nine of
 * sixty instructions on exactly that. */
extern "C" unsigned z3660_warp_run(unsigned idx, unsigned way)
{
   struct z3660_warp_set *s = &z3660_warp_tab[idx];
   unsigned n;

#ifdef HOST_TEST_HARNESS
   if (!warp_published[idx][way])
      warp_bad_unpublished++;
   if (regs.s)
      warp_bad_super++;
   /* §0.3(b)'s invariant, ASSERTED rather than maintained: mmufixup is disarmed at every
    * block head, because only the interpreter ever arms it and generation one compiles no
    * instruction that can. M-4 removes the assertion. */
   if (!WARP_MUTANT(Z3660_WARP_MUT_NOFIXUP)
       && (mmufixup[0].reg >= 0 || mmufixup[1].reg >= 0))
      warp_bad_fixup++;
#endif

   /* THE ARM ATTESTATION AND THE PROFILED COUNTER, ONE SITE, and now a split pair rather
    * than a 64-bit increment. Non-zero z3660_warp_entered() is what a lean capture reads
    * back to prove the engine was armed; WARP_BLK is the same quantity in the profiling
    * build's counter block. They cannot disagree because they are the same increment. */
   if (++warp_blk_lo == 0) warp_blk_hi++;
   WARP_STAT(Z3660_WARP_STAT_BLK);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_BLK);

   /* Hand the block the two bases and the two helpers it needs, and nothing else. The
    * emitted stream materialises no address of its own, which is what makes the same words
    * runnable in the host harness (§0.8.5). */
   z3660_warp_ctx.pc      = z3660_warp_last_pc;
   z3660_warp_ctx.ipc     = z3660_warp_last_pc;
   z3660_warp_ctx.restart = 1;
   WARP_FLAGS_PACK();

#ifdef HOST_TEST_HARNESS
   n = warp_host_exec((const uae_u32 *)(void *)s->entry[way],
                      (void *)&regs.regs[0], (void *)&z3660_warp_rd,
                      (void *)&z3660_warp_wr, (void *)&z3660_warp_ctx);
#else
   n = (*s->entry[way])((void *)&regs.regs[0], (void *)&z3660_warp_rd,
                        (void *)&z3660_warp_wr, (void *)&z3660_warp_ctx);
#endif

   /* COMMIT, ONCE PER BLOCK. The flags are unpacked through the architectural macros
    * rather than stored into regs.ccrflags directly, because `flag_struct` has four
    * different shapes across the machdep arms and generated code works in the one the
    * silicon hands it. Once per block, not once per instruction. */
   if (n) {
      regs.pc = z3660_warp_ctx.pc;
      WARP_FLAGS_COMMIT();
      regs.instruction_pc = z3660_warp_ctx.ipc;
      mmu_restart = z3660_warp_ctx.restart ? true : false;

      WARP_STAT(Z3660_WARP_STAT_ENTER);
      WARP_STAT_ADD(Z3660_WARP_STAT_INSNS, n);
      Z3660_PROF_CNT(Z3660_PROF_C_WARP_ENTER);
      Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_INSNS, n);
#ifdef HOST_TEST_HARNESS
      if (n > (unsigned)s->len[way]) warp_bad_retired++;
#endif
   }
   if (n < (unsigned)s->len[way] || n == 0) {
      /* A BAIL: the block returned to the interpreter without retiring its whole compiled
       * span -- either it declined at a faultable site after n instructions, or it is a
       * cached DECLINE STUB whose compiled span is zero. The only store a bail arm emitted
       * is the guest PC, because nothing else was ever dirty, and that is §0.5.4's
       * measurement reported rather than estimated.
       *
       * n == 0 needs no state fix-up at all: the block changed nothing, so regs.pc is
       * already the pc the interpreter is about to dispatch. */
      WARP_STAT(Z3660_WARP_STAT_BAIL);
      WARP_STAT_ADD(Z3660_WARP_STAT_BAILST, 1);
      Z3660_PROF_CNT(Z3660_PROF_C_WARP_BAIL);
      Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_BAIL_ST, 1);
   }

   /* AFTER A BLOCK THAT RETIRED SOMETHING, the exit pc is a head candidate whatever it is:
    * a real branch target, or the uncovered instruction the block ended at -- and the
    * latter gets a CACHED DECLINE rather than a fresh discovery on every arrival.
    *
    * AFTER ONE THAT RETIRED NOTHING, last_pc is left where step() put it, so the next
    * dispatch reads as a fall-through and the interpreter simply continues. Resetting it
    * here instead would make the engine probe every PC of an uncovered run -- round 1's
    * pathology, arrived at from the other direction. */
   if (n)
      z3660_warp_last_pc = Z3660_WARP_NO_LAST;
   return n;
}

/* The miss arm. Out of line on purpose: it runs once per block head and never again, so it
 * is allowed to cost something -- unlike the lookup, which §0.7.3 caught paying a full
 * prologue to answer a question in thirteen instructions. */
extern "C" unsigned z3660_warp_discover(unsigned pc)
{
   unsigned idx, way = 0, len = 0, bytes = 0;
   uae_u32 key;

   if (!warp_inited || warp_arena == NULL) {
      warp_init();
      if (warp_arena == NULL) { warp_decline(Z3660_WARP_DECL_NOCACHE); return 0; }
   }
   idx = warp_index(pc);
   key = warp_key(pc, 0);
   if (warp_compile(pc, key, idx, &way, &len, &bytes) < 0)
      return 0;
   return z3660_warp_run(idx, way);
}

/* ---------------------------------------------------------------- switch ------------- */

extern "C" unsigned z3660_warp_arm(unsigned requested)
{
   unsigned v = z3660_warp_resolve(requested);

   /* BOTH TRANSITIONS DISCARD THE WHOLE CODE CACHE, and the DISARM direction is the one that
    * needs it: an off period is an unwatched period. Nothing is compiled while disarmed and
    * no lookup consults the table, so the guest can rewrite a page with nothing watching --
    * and coming back on must not find the pre-rewrite blocks still stamped live. Discarding
    * on every set removes the question instead of answering it, which is z3660_dopc_set()'s
    * own argument. The state is stored FIRST so the discard is performed while armed. */
   z3660_warp_on = (int)v;
   warp_init();                       /* hoisted out of the dispatch path, and it RETRIES */
   warp_discard_all(Z3660_DOPC_INV_KNOB);
   warp_cp = warp_arena;
   return v;
}

extern "C" int z3660_warp_armed(void) { return z3660_warp_on; }

extern "C" unsigned long long z3660_warp_entered(void)
{
   return ((unsigned long long)warp_blk_hi << 32) | (unsigned long long)warp_blk_lo;
}

/* ==================================================================================== */
/* ================= THE HOST ARM EXECUTOR -- HARNESS ONLY ============================ */
/* ==================================================================================== */

#ifdef HOST_TEST_HARNESS

/* §0.8.5. Round 1's honest limit was that the harness "cannot execute one instruction of
 * ARM codegen", so it proved the decisions and asserted the emitted words without running
 * them. With real code generation that stops being acceptable: a wrong emitted word is now
 * a wrong RESULT, not a wrong constant.
 *
 * So this is a decoder and interpreter for exactly the instruction forms the emitter above
 * produces -- the same bytes, against the same regs structure, calling the same helpers.
 * It is a SECOND IMPLEMENTATION OF ARM and it is honest about what that means: a shared
 * misreading of the architecture is invisible to it, which is why the harness also asserts
 * hand-decoded golden words for each form. And it proves nothing whatever about the
 * SILICON -- I-cache publication, the Q3 barrier sequence, memory ordering and behaviour at
 * volume are proven on target and only on target.
 *
 * Registers are uintptr_t because r4..r7 hold host pointers the caller handed in. Every
 * data-processing result is masked to 32 bits; the ONE exception is a plain register-to-
 * register MOV with no shift and no S, which copies the full stored width -- that is what
 * the four prologue moves are, and on the target a 32-bit move is full width too. */

struct warp_arm {
   uintptr_t r[16];
   int n, z, cv, v;
};

static __inline__ uae_u32 warp_arm_imm(uae_u32 word)
{
   unsigned rot = ((word >> 8) & 0xFu) * 2u;
   uae_u32  v   = word & 0xFFu;
   if (rot == 0) return v;
   return (v >> rot) | (v << (32u - rot));
}

static void warp_arm_flags(struct warp_arm *m, uae_u32 res, int c, int v)
{
   m->n = (res & 0x80000000u) ? 1 : 0;
   m->z = (res == 0) ? 1 : 0;
   m->cv = c; m->v = v;
}

static int warp_arm_cond(const struct warp_arm *m, unsigned cc)
{
   switch (cc) {
   case CC_EQ: return m->z;
   case CC_NE: return !m->z;
   case CC_CS: return m->cv;
   case CC_CC: return !m->cv;
   case CC_MI: return m->n;
   case CC_PL: return !m->n;
   case CC_VS: return m->v;
   case CC_VC: return !m->v;
   case CC_HI: return m->cv && !m->z;
   case CC_LS: return !m->cv || m->z;
   case CC_GE: return m->n == m->v;
   case CC_LT: return m->n != m->v;
   case CC_GT: return !m->z && (m->n == m->v);
   case CC_LE: return m->z || (m->n != m->v);
   default:    return 1;
   }
}

static unsigned warp_host_exec(const uae_u32 *code, void *regbase, void *rd, void *wr,
                               void *ctx)
{
   struct warp_arm m;
   uae_u32 stack[64];
   int sp = 64;
   unsigned pc = 0;
   int guard = 0;

   memset(&m, 0, sizeof m);
   m.r[0] = (uintptr_t)regbase;
   m.r[1] = (uintptr_t)rd;
   m.r[2] = (uintptr_t)wr;
   m.r[3] = (uintptr_t)ctx;

   for (;;) {
      uae_u32 w = code[pc++];
      unsigned cc = (w >> 28) & 0xFu;

      if (++guard > 4096) return 0;                  /* a runaway block is a test failure */
      if (cc != 0xEu && !warp_arm_cond(&m, cc)) {
         if ((w & 0x0E000000u) == 0x0A000000u) continue;
         continue;
      }

      /* branch */
      if ((w & 0x0E000000u) == 0x0A000000u) {
         int off = (int)((w & 0x00FFFFFFu) << 8) >> 8;
         pc = (unsigned)((int)pc + 1 + off);
         continue;
      }
      /* blx reg */
      if ((w & 0x0FFFFFF0u) == 0x012FFF30u) {
         unsigned rm = w & 0xFu;
         int (*fn)(uae_u32, uae_u32, uae_u32) =
            (int (*)(uae_u32, uae_u32, uae_u32))(void *)m.r[rm];
         int res = fn((uae_u32)m.r[0], (uae_u32)m.r[1], (uae_u32)m.r[2]);
         m.r[0] = (uae_u32)res;
         m.r[1] = m.r[2] = m.r[3] = m.r[12] = 0;     /* AAPCS: caller-saved, clobbered   */
         continue;
      }
      /* bx lr -- the decline stub's second word */
      if (w == 0xE12FFF1Eu)
         return (unsigned)(uae_u32)m.r[0];
      /* push / pop */
      if ((w & 0x0FFF0000u) == 0x092D0000u) {        /* STMDB sp!, {list}                */
         int i;
         for (i = 15; i >= 0; i--)
            if (w & (1u << i)) stack[--sp] = (uae_u32)i;   /* only the shape matters     */
         continue;
      }
      if ((w & 0x0FFF0000u) == 0x08BD0000u) {        /* LDMIA sp!, {list}                */
         int i;
         for (i = 0; i < 16; i++) if (w & (1u << i)) sp++;
         if (w & 0x8000u) return (unsigned)(uae_u32)m.r[0];   /* ... , pc -> return      */
         continue;
      }
      /* movw / movt */
      if ((w & 0x0FF00000u) == 0x03000000u) {
         unsigned rd2 = (w >> 12) & 0xFu;
         uae_u32 imm = ((w >> 16) & 0xFu) << 12 | (w & 0xFFFu);
         m.r[rd2] = imm;
         continue;
      }
      if ((w & 0x0FF00000u) == 0x03400000u) {
         unsigned rd2 = (w >> 12) & 0xFu;
         uae_u32 imm = ((w >> 16) & 0xFu) << 12 | (w & 0xFFFu);
         m.r[rd2] = (uae_u32)((m.r[rd2] & 0xFFFFu) | (imm << 16));
         continue;
      }
      /* mrs rd, cpsr */
      if ((w & 0x0FFF0FFFu) == 0x010F0000u) {
         unsigned rd2 = (w >> 12) & 0xFu;
         m.r[rd2] = (uae_u32)(((uae_u32)m.n << 31) | ((uae_u32)m.z << 30)
                            | ((uae_u32)m.cv << 29) | ((uae_u32)m.v << 28));
         continue;
      }
      /* msr cpsr_f, rm */
      if ((w & 0x0FFFFFF0u) == 0x0128F000u) {
         uae_u32 v = (uae_u32)m.r[w & 0xFu];
         m.n = (v >> 31) & 1; m.z = (v >> 30) & 1;
         m.cv = (v >> 29) & 1; m.v = (v >> 28) & 1;
         continue;
      }
      /* sxth */
      if ((w & 0x0FFF03F0u) == 0x06BF0070u) {
         unsigned rd2 = (w >> 12) & 0xFu, rm = w & 0xFu;
         m.r[rd2] = (uae_u32)(uae_s32)(uae_s16)(uae_u16)m.r[rm];
         continue;
      }
      /* ubfx */
      if ((w & 0x0FE00070u) == 0x07E00050u) {
         unsigned width = ((w >> 16) & 0x1Fu) + 1u;
         unsigned rd2 = (w >> 12) & 0xFu, lsb = (w >> 7) & 0x1Fu, rn = w & 0xFu;
         uae_u32 mask = (width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u);
         m.r[rd2] = ((uae_u32)m.r[rn] >> lsb) & mask;
         continue;
      }
      /* bfi */
      if ((w & 0x0FE00070u) == 0x07C00010u) {
         unsigned msb = (w >> 16) & 0x1Fu, lsb = (w >> 7) & 0x1Fu;
         unsigned rd2 = (w >> 12) & 0xFu, rn = w & 0xFu;
         unsigned width = msb - lsb + 1u;
         uae_u32 mask = (((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << lsb);
         m.r[rd2] = (uae_u32)((m.r[rd2] & ~mask) | ((((uae_u32)m.r[rn]) << lsb) & mask));
         continue;
      }
      /* ldr / str, immediate offset */
      if ((w & 0x0E000000u) == 0x04000000u) {
         unsigned rn = (w >> 16) & 0xFu, rd2 = (w >> 12) & 0xFu;
         int off = (int)(w & 0xFFFu);
         unsigned char *a;
         if (!(w & (1u << 23))) off = -off;
         a = (unsigned char *)m.r[rn] + off;
         if (w & (1u << 20)) { uae_u32 v; memcpy(&v, a, 4); m.r[rd2] = v; }
         else                { uae_u32 v = (uae_u32)m.r[rd2]; memcpy(a, &v, 4); }
         continue;
      }
      /* data processing */
      {
         unsigned op = (w >> 21) & 0xFu, s = (w >> 20) & 1u;
         unsigned rn = (w >> 16) & 0xFu, rd2 = (w >> 12) & 0xFu;
         uae_u32 a = (uae_u32)m.r[rn], b, res = 0;
         int carry = m.cv, ovf = m.v, isimm = (w >> 25) & 1u;
         int plain_mov = 0;

         if (isimm) b = warp_arm_imm(w);
         else {
            unsigned rm = w & 0xFu, st = (w >> 5) & 3u, sh = (w >> 7) & 0x1Fu;
            uae_u32 bv = (uae_u32)m.r[rm];
            if (st == 0 && sh == 0) { b = bv; plain_mov = 1; }
            else if (st == 0) b = bv << sh;
            else if (st == 1) b = sh ? (bv >> sh) : 0u;
            else              b = (uae_u32)(((uae_s32)bv) >> (sh ? sh : 31));
         }

         switch (op) {
         case OP_AND: res = a & b; break;
         case OP_EOR: res = a ^ b; break;
         case OP_SUB: res = a - b; carry = (a >= b);
                      ovf = (((a ^ b) & (a ^ res)) >> 31) & 1; break;
         case OP_ADD: res = a + b; carry = (res < a);
                      ovf = ((~(a ^ b) & (a ^ res)) >> 31) & 1; break;
         case OP_TST: res = a & b; break;
         case OP_CMP: res = a - b; carry = (a >= b);
                      ovf = (((a ^ b) & (a ^ res)) >> 31) & 1; break;
         case OP_ORR: res = a | b; break;
         case OP_MOV: res = b; break;
         case OP_BIC: res = a & ~b; break;
         default: return 0;
         }
         if (op != OP_TST && op != OP_CMP) {
            /* THE ONE FULL-WIDTH CASE, and it is the four prologue moves: a plain
             * register-to-register MOV carries a host pointer on this host and a 32-bit
             * value on the target, and both are "the whole register". */
            if (op == OP_MOV && plain_mov && !isimm && !s)
               m.r[rd2] = m.r[w & 0xFu];
            else
               m.r[rd2] = res;
         }
         if (s) warp_arm_flags(&m, res, carry, ovf);
         continue;
      }
   }
}

#endif /* HOST_TEST_HARNESS */

/* ---------------------------------------------------------------- harness door ------- */

#ifdef HOST_TEST_HARNESS

/* Read-only, counts nothing, arms nothing -- so a test can ask without perturbing the thing
 * it is asking about. It uses the REAL key and index functions, which is what makes it the
 * place the M-5 keying mutation is caught: with the supervisor bit dropped, a user block
 * answers a supervisor lookup at the same virtual address. */
static const struct z3660_warp_set *warp_find(unsigned pc, int super, unsigned *way)
{
   const struct z3660_warp_set *s;
   uae_u32 key;
   unsigned w;

   if (pc & 1u) return NULL;
   s = &z3660_warp_tab[warp_index(pc)];
   if (s->stamp != z3660_warp_gen) return NULL;
   key = warp_key(pc, super);
   for (w = 0; w < Z3660_WARP_WAYS; w++)
      if (s->tag[w] == key) { if (way) *way = w; return s; }
   return NULL;
}

extern "C" int z3660_warp_peek(unsigned pc, int super)
{
   return warp_find(pc, super, NULL) ? 1 : 0;
}

extern "C" unsigned z3660_warp_peek_len(unsigned pc)
{
   unsigned w = 0;
   const struct z3660_warp_set *s = warp_find(pc, 0, &w);
   return s ? (unsigned)s->len[w] : 0u;
}

extern "C" unsigned z3660_warp_generation(void)  { return z3660_warp_gen; }
extern "C" unsigned z3660_warp_set_bytes(void)   { return (unsigned)sizeof(struct z3660_warp_set); }

extern "C" unsigned long long z3660_warp_stat(int which)
{
   if (which == Z3660_WARP_STAT_LIVE) return (unsigned long long)warp_live;
   if ((unsigned)which >= (unsigned)Z3660_WARP_STAT_COUNT) return 0;
   return warp_stats[which];
}

extern "C" void z3660_warp_stats_zero(void)
{
   memset(warp_stats, 0, sizeof warp_stats);
   warp_blk_lo = warp_blk_hi = 0;
   /* The invariant counters go with them. They are cumulative on purpose -- a violation
    * must not be able to age out of a later assertion -- so the ONE place that clears them
    * is the one a test calls deliberately, between corpora. */
   warp_bad_unpublished = 0;
   warp_bad_retired = 0;
   warp_bad_fixup = 0;
   warp_bad_super = 0;
}

extern "C" void z3660_warp_set_cache_limit(unsigned bytes)
{
   warp_init();
   warp_limit = (bytes == 0 || bytes > warp_bytes) ? warp_bytes : bytes;
   warp_cp = warp_arena;
   warp_discard_all(-1);
}

extern "C" void z3660_warp_set_mutant(int which) { z3660_warp_mutant = which; }

extern "C" unsigned long long z3660_warp_bad(int which)
{
   switch (which) {
   case Z3660_WARP_BAD_UNPUBLISHED: return warp_bad_unpublished;
   case Z3660_WARP_BAD_RETIRED:     return warp_bad_retired;
   case Z3660_WARP_BAD_FIXUP:       return warp_bad_fixup;
   default:                         return warp_bad_super;
   }
}

extern "C" unsigned z3660_warp_block_code(unsigned pc, void *dst, unsigned max)
{
   unsigned w = 0, i, n;
   const struct z3660_warp_set *s = warp_find(pc, 0, &w);
   const uae_u32 *code;
   uae_u32 *out = (uae_u32 *)dst;

   if (s == NULL) return 0;
   code = (const uae_u32 *)(const void *)s->entry[w];
   /* The block's length in words is not stored -- it is recovered by scanning to the first
    * terminator, which is exactly what a decoder has to do anyway and is what the golden
    * word assertions consume. */
   for (n = 0; n < max / 4u; n++) {
      out[n] = code[n];
      if (code[n] == 0xE12FFF1Eu || (code[n] & 0x0FFF8000u) == 0x08BD8000u) { n++; break; }
   }
   (void)i;
   return n * 4u;
}

extern "C" int z3660_warp_probe_compile(unsigned pc, unsigned *insns, unsigned *bytes)
{
   unsigned idx, way = 0, len = 0, by = 0;
   uae_u32 key;

   warp_init();
   if (warp_arena == NULL) return -1;
   idx = warp_index(pc);
   key = warp_key(pc, 0);
   if (warp_compile(pc, key, idx, &way, &len, &by) < 0) return -1;
   if (insns) *insns = len;
   if (bytes) *bytes = by;
   return 0;
}

#endif /* HOST_TEST_HARNESS */

/* ---------------------------------------------------------------- lockstep ----------- */

#ifdef Z3660_WARP_LOCKSTEP

/* THE LOCKSTEP ORACLE, host-harness mode, compiled out of every shipping image.
 *
 * It runs the same guest span twice -- disarmed, then armed -- and compares the two streams
 * boundary by boundary. The comparison set is rung 2's, unchanged, plus the JIT's four
 * (§3.1): the sixteen data and address registers, the guest pc, the flags word and X,
 * regs.instruction_pc, mmu_restart and mmufixup[0..1].
 *
 * PER INSTRUCTION, AND LITERALLY SO. Each boundary is folded into one 32-bit digest and the
 * digest is APPENDED to an array, so the comparison is per-boundary and the divergence index
 * is exact -- not a checkpoint every N with a bisection afterwards.
 *
 * ROUND 2 IS WHERE THIS STOPS BEING FREE. With null blocks the two arms were IDENTICAL
 * because a null block changed nothing; now the armed arm genuinely computes, and every
 * divergence is a real defect in an emitter. A single divergence is a stop, not a statistic.
 *
 * THE ARMED ARM PRODUCES FEWER BOUNDARIES THAN THE DISARMED ONE, and that is the invariant
 * rather than a problem: a block that retires n instructions consumes ONE run-loop pass. So
 * the digest is taken at the point the harness steps the machine, and the harness steps by
 * GUEST INSTRUCTION on both arms -- see the corpus driver, which reads WARP_INSNS to know
 * how many boundaries one armed pass stood for. What is compared here is the sequence of
 * ARCHITECTURAL STATES at the boundaries both arms actually expose. */

#define WARP_LS_CAP  (256u * 1024u * 1024u)   /* boundaries */

static uae_u32 *ls_ref = NULL;   static uae_u64 ls_ref_n = 0;
static uae_u32 *ls_cur = NULL;   static uae_u64 ls_cur_n = 0;
static uae_u64  ls_cur_cap = 0;
static uae_u64  ls_insns = 0;
static uae_u64  ls_diverge = 0;
static uae_u64  ls_overflow = 0;

static __inline__ uae_u32 ls_fold(uae_u32 h, uae_u32 v)
{
   h ^= v;
   h *= 16777619u;                 /* FNV-1a, 32-bit */
   return h;
}

extern "C" void z3660_warp_ls_step(void)
{
   uae_u32 h = 2166136261u;
   int i;

   for (i = 0; i < 8; i++) h = ls_fold(h, (uae_u32)m68k_dreg(regs, i));
   for (i = 0; i < 8; i++) h = ls_fold(h, (uae_u32)m68k_areg(regs, i));
   h = ls_fold(h, (uae_u32)m68k_getpc());
   /* THE ARCHITECTURAL CCR, not the host's representation of it. `flag_struct` has four
    * different shapes across the machdep arms -- nzcv on ARM, cznv on x86, five separate
    * ints in the portable one -- and a digest over the host struct would compare a storage
    * layout rather than guest-visible state. These five accessors exist in every arm and
    * mean the same thing in all of them, which is exactly what a differential oracle needs. */
   h = ls_fold(h, (uae_u32)((GET_NFLG() ? 8u : 0u) | (GET_ZFLG() ? 4u : 0u)
                          | (GET_VFLG() ? 2u : 0u) | (GET_CFLG() ? 1u : 0u)));
   h = ls_fold(h, (uae_u32)(GET_XFLG() ? 1u : 0u));
   h = ls_fold(h, (uae_u32)regs.instruction_pc);
   h = ls_fold(h, (uae_u32)(mmu_restart ? 1u : 0u));
   h = ls_fold(h, (uae_u32)mmufixup[0].reg);
   h = ls_fold(h, (uae_u32)mmufixup[0].value);
   h = ls_fold(h, (uae_u32)mmufixup[1].reg);
   h = ls_fold(h, (uae_u32)mmufixup[1].value);

   if (ls_cur_n >= ls_cur_cap) {
      uae_u64 want = ls_cur_cap ? ls_cur_cap * 2u : 65536u;
      uae_u32 *p;
      if (want > WARP_LS_CAP) want = WARP_LS_CAP;
      if (ls_cur_n >= want) { ls_overflow++; return; }
      p = (uae_u32 *)realloc(ls_cur, (size_t)(want * sizeof(uae_u32)));
      if (p == NULL) { ls_overflow++; return; }
      ls_cur = p; ls_cur_cap = want;
   }
   ls_cur[ls_cur_n++] = h;
}

extern "C" unsigned long long z3660_warp_ls_commit(int arm)
{
   uae_u64 delta = 0;

   if (arm == 0) {                       /* this was the reference arm */
      free(ls_ref);
      ls_ref = ls_cur; ls_ref_n = ls_cur_n;
      ls_cur = NULL;   ls_cur_n = 0; ls_cur_cap = 0;
      return 0;
   }

   {
      uae_u64 n = (ls_cur_n < ls_ref_n) ? ls_cur_n : ls_ref_n;
      uae_u64 i;
      for (i = 0; i < n; i++)
         if (ls_cur[i] != ls_ref[i]) delta++;
      /* A LENGTH MISMATCH IS A DIVERGENCE. In round 1 it was the no-empty-pass invariant
       * failing; in round 2 it is the corpus driver having mis-counted how many guest
       * instructions an armed pass stood for, which is the same class of error one level
       * up and is worth catching just as loudly. */
      delta += (ls_cur_n > ls_ref_n) ? (ls_cur_n - ls_ref_n) : (ls_ref_n - ls_cur_n);
      ls_insns   += n;
      ls_diverge += delta;
   }
   free(ls_cur); ls_cur = NULL; ls_cur_n = 0; ls_cur_cap = 0;
   return delta;
}

extern "C" unsigned long long z3660_warp_ls_insns(void)   { return ls_insns; }
extern "C" unsigned long long z3660_warp_diverge(void)    { return ls_diverge; }
extern "C" void z3660_warp_ls_zero(void)
{
   free(ls_ref); free(ls_cur);
   ls_ref = ls_cur = NULL;
   ls_ref_n = ls_cur_n = ls_cur_cap = 0;
   ls_insns = ls_diverge = ls_overflow = 0;
}

#endif /* Z3660_WARP_LOCKSTEP */
