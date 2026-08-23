/* WARP ENGINE -- the block engine. Round 1: the plumbing, and no code generation.
 *
 * z3660_warp.h is the contract and docs/WARP-ENGINE.md §0.6 is the pre-registration, both
 * written before this file. The short version, because everything here follows from it:
 *
 *   Every block is a NULL BLOCK -- two generated ARM words that sync nothing, retire
 *   nothing and return to the interpreter at the PC the block was entered at. Blocks are
 *   discovered at the interpreter's dispatch site, keyed on guest logical PC plus the
 *   supervisor bit, cached, invalidated from the decoded-op cache's own choke point, and
 *   evicted by throwing the whole code cache away. Coverage is 0 by construction and NO
 *   PERFORMANCE CLAIM IS ADMISSIBLE FROM THIS ROUND.
 *
 * What it exists to prove is that the machinery is right before there is a compiler whose
 * numbers could distract from it -- and to price ONE thing: what it costs to look a PC up,
 * enter a block and come straight back out. §30's pricing law says work prepended to the
 * path arrives in full, and this round measures the arrival before anything is built on it.
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "cpummu.h"
#include "z3660_prof.h"
#include "z3660_dopc.h"
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
 * pre-registration prices this at 3..6 ARM instructions per guest instruction and the
 * number is read off the lean objdump, never estimated.
 *
 * Initialised DISARMED. That is the SECOND of the two mechanisms that keep an arm from
 * surviving a reboot -- core0 republishes the OFF sentinel unconditionally on every boot
 * (config_file.c), and this initialiser means a core1 running before core0 has published
 * anything is disarmed rather than reading whatever the DDR happened to hold. */
extern "C" { int z3660_warp_on = 0; }

/* ---------------------------------------------------------------- the block table ---- */

typedef unsigned (*z3660_warp_fn)(void);

struct z3660_warp_block {
   uae_u32       tag;     /* Z3660_WARP_KEY(pc, super), or TAG_EMPTY(index)              */
   uae_u32       stamp;   /* == warp_gen while this block is live                        */
   z3660_warp_fn entry;   /* the generated stub                                          */
   uae_u32       insns;   /* guest instructions the block retires. 0 for a null block    */
};

static struct z3660_warp_block warp_tab[Z3660_WARP_SETS];

/* 16 bytes per entry on the target: tag, stamp, a 32-bit code pointer and the retired
 * count, i.e. half a Cortex-A9 line, so a lookup touches one line and never two. Guarded on
 * 32-bit pointers only, because the host harness builds the same source on x86-64 where the
 * entry pointer is eight bytes and the struct is legitimately 24 -- the harness asserts the
 * field arithmetic instead, and this line is what refuses the build that would actually
 * ship. Same shape of tripwire, and same reasoning, as the decoded-op cache's. */
typedef char z3660_warp_block_is_half_a_line[
   (sizeof(void *) != 4 || sizeof(struct z3660_warp_block) == 16) ? 1 : -1];

/* Generation 0 is RESERVED and never live, so a zeroed table -- BSS at power-on, and the
 * memset on wrap below -- is entirely stale rather than entirely valid. */
static uae_u32 warp_gen = 1;
static int     warp_inited = 0;

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

/* The arm attestation. Compiled into EVERY variant, unlike the profiler's counter block,
 * because the wall-clock arm of every verdict is a LEAN boot with no profiler in it -- and
 * an arm BANNER is not admissible, because two cores share one unlocked UART and a banner
 * arrives spliced. This is the same increment site as WARP_BLK (prof id 48): one site, not
 * two, so the lean confession and the profiled counter can never disagree. */
static uae_u64 warp_blk_count = 0;

#ifdef HOST_TEST_HARNESS
static unsigned long long warp_stats[Z3660_WARP_STAT_COUNT];
#define WARP_STAT(i)      (warp_stats[(i)]++)
#define WARP_STAT_ADD(i,n) (warp_stats[(i)] += (unsigned long long)(n))
static int warp_mutant = Z3660_WARP_MUT_NONE;
/* Harness-only side table: was the block in this set published through the I-cache
 * sequence? A side table and not a struct field, so the shipping structure the target
 * compiles is byte-for-byte the structure the harness tests. */
static unsigned char warp_published[Z3660_WARP_SETS];
/* The two invariant violations that have no other place to land: a block reached from the
 * run loop without the publish sequence having run, and a block that retired something in a
 * round with no pass-consuming arm. Both are correctness failures rather than statistics. */
static unsigned long long warp_bad_unpublished = 0;
static unsigned long long warp_bad_retired = 0;
#else
#define WARP_STAT(i)       do { } while (0)
#define WARP_STAT_ADD(i,n) do { } while (0)
#endif

/* Blocks currently live in the table. Not a scan: it is maintained at the two sites that
 * can change it, because WARP_DISC is "blocks discarded by THIS invalidation" and a scan
 * after the generation has already moved would count nothing. */
static unsigned warp_live = 0;

/* ---------------------------------------------------------------- the key ------------ */

/* One function, so "what is a block keyed on?" is answered by reading one line -- and so
 * the mutation arm has exactly one place to bite. M-5 drops the supervisor bit, which makes
 * a supervisor block and a user block at the same virtual address alias; the corpus catches
 * it through z3660_warp_peek(), which uses this same function. */
static __inline__ uae_u32 warp_key(unsigned pc, int super)
{
#ifdef HOST_TEST_HARNESS
   if (warp_mutant == Z3660_WARP_MUT_KEY_NOSUP)
      return (uae_u32)((unsigned)pc & ~1u);          /* the defect, never shipped */
#endif
   return (uae_u32)Z3660_WARP_KEY(pc, super);
}

/* ---------------------------------------------------------------- emission ----------- */

/* THE NULL BLOCK, emitted by portable C so the host harness produces the same two words and
 * can assert them byte-exactly even though it can execute neither of them. §3.2's honest
 * limit stands: the DECISIONS are proven on the host, the emitted sequence is proven on
 * target and only on target. */
static unsigned warp_emit_null(void *dst)
{
   uae_u32 *w = (uae_u32 *)dst;
   w[0] = Z3660_WARP_NULL_WORD0;   /* mov r0, #0 -- a null block retires nothing */
   w[1] = Z3660_WARP_NULL_WORD1;   /* bx  lr     -- back to the interpreter      */
   return Z3660_WARP_NULL_BYTES;
}

#ifdef HOST_TEST_HARNESS
/* The host cannot execute ARM, so a cached block's entry point is this C function -- which
 * returns exactly what the two words above return, and nothing else. It is the ONE seam
 * between the harness and the target, it is named so it cannot be mistaken for anything
 * else, and the words are still emitted and still asserted beside it. */
static unsigned warp_host_null_entry(void)
{
   return 0;
}
#endif

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

   if (++warp_gen == 0) {
      /* 2^32 discards later. Every stamp must be made unequal to the generation we are
       * about to resume at, and zeroing does that for generation 1 -- so this is a real
       * clear and not a wrap that silently revalidates the whole table. The tags are
       * re-marked EMPTY on the way out so the set-relative marker keeps its promise even
       * here. */
      unsigned i;
      memset(warp_tab, 0, sizeof warp_tab);
      for (i = 0; i < Z3660_WARP_SETS; i++)
         warp_tab[i].tag = (uae_u32)Z3660_WARP_TAG_EMPTY(i);
      warp_gen = 1;
   }
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
 * generation counter -- a full flush is one generation bump plus a pointer reset. The flush
 * RATE per boot is a counter before it is a design input. */
static void warp_flush_cache(void)
{
   WARP_STAT(Z3660_WARP_STAT_FLUSH);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_FLUSH);
   warp_cp = warp_arena;
   warp_discard_all(-1);
}

/* ---------------------------------------------------------------- init --------------- */

static void warp_init(void)
{
   unsigned i;

   warp_inited = 1;
   for (i = 0; i < Z3660_WARP_SETS; i++) {
      warp_tab[i].tag   = (uae_u32)Z3660_WARP_TAG_EMPTY(i);
      warp_tab[i].stamp = 0;                 /* generation 0 is never live */
      warp_tab[i].entry = NULL;
      warp_tab[i].insns = 0;
   }
   warp_live = 0;

#ifdef HOST_TEST_HARNESS
   if (warp_arena == NULL) {
      warp_bytes = 64u * 1024u;
      warp_arena = (uae_u8 *)malloc(warp_bytes);
   }
   memset(warp_published, 0, sizeof warp_published);
#else
   if (warp_arena == NULL)
      warp_arena = z3660_warp_cache_arena(&warp_bytes);
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

static struct z3660_warp_block *warp_compile(unsigned pc, int super,
                                             unsigned idx, uae_u32 key)
{
   struct z3660_warp_block *b;
   uae_u8 *dst;
   unsigned bytes;

   if (warp_arena == NULL || warp_limit < Z3660_WARP_NULL_BYTES) {
      warp_decline(Z3660_WARP_DECL_NOCACHE);
      return NULL;
   }
   if ((unsigned)(warp_cp - warp_arena) + Z3660_WARP_NULL_BYTES > warp_limit)
      warp_flush_cache();

   dst   = warp_cp;
   bytes = warp_emit_null(dst);
   warp_cp += bytes;

   /* PUBLISHED BEFORE IT IS REACHABLE, and the ordering is the whole discipline: the block
    * is not written into the table until the I-cache sequence has run, so there is no window
    * in which a lookup could hand the run loop a pointer to bytes the I-cache has not seen.
    * `8be8949` is this repository's own receipt for what the other ordering costs -- the
    * first pushall_call_handler() fetched stale I-cache lines and the board rebooted with an
    * ARM Undefined Instruction, caused by a compiler-flag change. */
#ifdef HOST_TEST_HARNESS
   if (warp_mutant == Z3660_WARP_MUT_NO_ICACHE) {
      warp_published[idx] = 0;               /* the defect, never shipped */
   } else {
      warp_publish(dst, bytes);
      warp_published[idx] = 1;
   }
#else
   warp_publish(dst, bytes);
#endif

   b = &warp_tab[idx];
   if (b->stamp != warp_gen)                 /* a stale slot becomes a live one */
      warp_live++;                           /* a live slot is REPLACED: live is unchanged */
   b->tag   = key;
   b->stamp = warp_gen;
#ifdef HOST_TEST_HARNESS
   b->entry = warp_host_null_entry;
#else
   b->entry = (z3660_warp_fn)(void *)dst;
#endif
   b->insns = 0;

   WARP_STAT(Z3660_WARP_STAT_COMPILE);
   WARP_STAT_ADD(Z3660_WARP_STAT_CBYTES, bytes);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_COMPILE);
   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_CINSNS, 0);   /* a null block compiles no guest insn */
   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_CBYTES, bytes);
   (void)pc; (void)super;
   return b;
}

/* ---------------------------------------------------------------- dispatch ----------- */

extern "C" unsigned z3660_warp_dispatch(unsigned pc, int super)
{
   struct z3660_warp_block *b;
   unsigned idx, n;
   uae_u32 key;

   /* GENERATION ONE IS USER-MODE ONLY: a block is never entered with regs.s set. Gated here,
    * before discovery, and deliberately NOT counted as a decline -- one decline per
    * supervisor instruction would make WARP_DECLINE a second copy of INSNS_SUPER and say
    * nothing that counter does not already say. */
   if (super)
      return 0;

   /* TRACE ON IMPLIES INTERPRET. One predicate at the entry site that removes an entire
    * class of divergence: do_specialties() stays per-instruction in the interpreter at every
    * cadence, and a block that retired several instructions under T0/T1 would owe several
    * trace exceptions it has no way to raise. */
   if (regs.t0 || regs.t1) {
      warp_decline(Z3660_WARP_DECL_TRACE);
      return 0;
   }

   /* An odd pc cannot be a block head. Nothing can execute at one on a 68040 -- it raises
    * before any fetch completes -- so this branch is never taken on a running guest. It is
    * here because "never taken" and "cannot happen" are different claims and only one of
    * them survives a future caller. */
   if (pc & 1u) {
      warp_decline(Z3660_WARP_DECL_ODDPC);
      return 0;
   }

   if (!warp_inited)
      warp_init();

   idx = Z3660_WARP_INDEX(pc);
   key = warp_key(pc, super);
   b   = &warp_tab[idx];

   if (b->stamp != warp_gen || b->tag != key) {
      b = warp_compile(pc, super, idx, key);
      if (b == NULL)
         return 0;
   }

#ifdef HOST_TEST_HARNESS
   /* THE ANTI-VACUITY CHECK ON THE PUBLISH DISCIPLINE, and it is an assertion at the point
    * of use rather than a counter compared afterwards. A block that reached the run loop
    * without the I-cache sequence having run is exactly the `8be8949` failure, and on the
    * target it presents as an Undefined Instruction and a reboot -- which is to say, as
    * anything. Here it presents as a test failure. */
   if (!warp_published[idx])
      warp_bad_unpublished++;
#endif

   /* THE ARM ATTESTATION AND THE PROFILED COUNTER, ONE SITE. Non-zero z3660_warp_entered()
    * is what a lean capture reads back to prove the engine was armed; WARP_BLK is the same
    * quantity in the profiling build's counter block. They cannot disagree because they are
    * the same increment. */
   warp_blk_count++;
   WARP_STAT(Z3660_WARP_STAT_BLK);
   Z3660_PROF_CNT(Z3660_PROF_C_WARP_BLK);

   n = (*b->entry)();

   Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_INSNS, n);

   /* THE CADENCE FEED (Q10, FROZEN). Called unconditionally with what the block retired, so
    * the contract is exercised on every block rather than on the blocks someone remembered.
    * A null block retires 0 and the feed is a no-op; the pass's own tail then supplies the
    * one tick the interpreted instruction earns. */
   z3660_warp_cadence_feed(n);

   if (n == 0) {
      /* THE BAIL, in its smallest true form: nothing was synced because nothing was dirty.
       * WARP_BAIL_ST is the emitted-store count and it is genuinely zero here -- not
       * unmeasured, not omitted. That is the number round 2's state-sync discipline will be
       * measured against. */
      WARP_STAT(Z3660_WARP_STAT_BAIL);
      Z3660_PROF_CNT(Z3660_PROF_C_WARP_BAIL);
      Z3660_PROF_CNT_ADD(Z3660_PROF_C_WARP_BAIL_ST, 0);
   } else {
      /* PASS-CONSUMING ENTRY -- the site that costs exactly one INSNS, and the ONLY site
       * WARP_ENTER may ever be incremented at. It is unreachable in round 1 because no block
       * retires anything; it exists here so that the counter's topology is fixed before the
       * first block that could reach it. */
      WARP_STAT(Z3660_WARP_STAT_ENTER);
      Z3660_PROF_CNT(Z3660_PROF_C_WARP_ENTER);
#ifdef HOST_TEST_HARNESS
      /* ROUND 1'S INVARIANT, CHECKED AT THE ONE PLACE IT COULD BREAK. The run loop has no
       * pass-consuming arm yet -- see the no-empty-pass invariant in z3660_warp.h -- so a
       * block that retired something would be executed and then silently re-executed by the
       * interpreter. The compiler cannot produce one today; this is what turns "cannot" into
       * a checked property, and it comes out in round 2 with the arm it is waiting for. */
      warp_bad_retired++;
#endif
   }
   return n;
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
   if (!warp_inited)
      warp_init();
   warp_discard_all(Z3660_DOPC_INV_KNOB);
   warp_cp = warp_arena;
   return v;
}

extern "C" int z3660_warp_armed(void) { return z3660_warp_on; }

extern "C" unsigned long long z3660_warp_entered(void)
{
   return (unsigned long long)warp_blk_count;
}

/* ---------------------------------------------------------------- harness door ------- */

#ifdef HOST_TEST_HARNESS

/* Read-only, counts nothing, arms nothing -- so a test can ask without perturbing the thing
 * it is asking about. It uses the REAL key function, which is what makes it the place the
 * M-5 keying mutation is caught: with the supervisor bit dropped, a user block answers a
 * supervisor lookup at the same virtual address. */
extern "C" int z3660_warp_peek(unsigned pc, int super)
{
   const struct z3660_warp_block *b;

   if (pc & 1u) return 0;
   b = &warp_tab[Z3660_WARP_INDEX(pc)];
   if (b->stamp != warp_gen) return 0;
   return (b->tag == warp_key(pc, super)) ? 1 : 0;
}

extern "C" unsigned z3660_warp_generation(void)  { return warp_gen; }
extern "C" unsigned z3660_warp_block_bytes(void) { return (unsigned)sizeof(struct z3660_warp_block); }

extern "C" unsigned long long z3660_warp_stat(int which)
{
   if (which == Z3660_WARP_STAT_LIVE) return (unsigned long long)warp_live;
   if ((unsigned)which >= (unsigned)Z3660_WARP_STAT_COUNT) return 0;
   return warp_stats[which];
}

extern "C" void z3660_warp_stats_zero(void)
{
   memset(warp_stats, 0, sizeof warp_stats);
   warp_blk_count = 0;
   /* The two invariant counters go with them. They are cumulative on purpose -- a violation
    * must not be able to age out of a later assertion -- so the ONE place that clears them
    * is the one a test calls deliberately, between corpora. */
   warp_bad_unpublished = 0;
   warp_bad_retired = 0;
}

extern "C" void z3660_warp_set_cache_limit(unsigned bytes)
{
   if (!warp_inited) warp_init();
   warp_limit = (bytes == 0 || bytes > warp_bytes) ? warp_bytes : bytes;
   warp_cp = warp_arena;
   warp_discard_all(-1);
}

extern "C" unsigned z3660_warp_emit_null(void *dst) { return warp_emit_null(dst); }

extern "C" void z3660_warp_set_mutant(int which) { warp_mutant = which; }

extern "C" unsigned long long z3660_warp_bad(int which)
{
   return (which == Z3660_WARP_BAD_UNPUBLISHED) ? warp_bad_unpublished : warp_bad_retired;
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
 * is exact -- not a checkpoint every N with a bisection afterwards. Ten million boundaries
 * cost 40 MB on a host that has gigabytes, and a gate that says "clean over 10^7" should
 * mean it at the granularity it claims.
 *
 * WITH NULL BLOCKS THE TWO ARMS MUST BE IDENTICAL -- not equivalent, identical -- because a
 * null block changes nothing at all. That identity is round 1's correctness gate, and it is
 * the strongest form this gate will ever take: every later round can only weaken it into
 * "equivalent". A single divergence is a stop, not a statistic. */

#define WARP_LS_CAP  (32u * 1024u * 1024u)   /* boundaries; 128 MB of digest at the cap */

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
      /* A LENGTH MISMATCH IS A DIVERGENCE, and it is the one this round is most likely to
       * produce: an engine that burned a run-loop pass without retiring anything would make
       * the armed arm shorter, which is precisely the no-empty-pass invariant failing. */
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

