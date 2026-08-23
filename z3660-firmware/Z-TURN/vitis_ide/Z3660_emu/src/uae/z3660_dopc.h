/* CYBERSTORM C2 rung 1 -- the decoded-op cache: the geometry, the switch rule, and the
 * one window the instruction-word accessor is allowed to answer from.
 *
 * WHAT THE CACHE IS. The C2 map measured the interpreter's instruction fetch at
 * 78.2 ARM cycles per opcode word and 85.2 per extension word, at 1.5983 fetches per
 * guest instruction -- 28.65 % of the whole interpreter (bracket 25.6-32.9), the largest
 * rung on the map and the one every correction makes bigger. A PC-indexed cache holding
 * the result of fetch-plus-decode removes both the fetch and the decode for any PC it
 * holds.
 *
 * THE SIZING IS A MEASUREMENT, NOT A GUESS. Map §9.1's table of the measured guest-PC
 * distribution over three lossless rings:
 *
 *     entries    BOOT (busy)   DHRY (busy)   IO (busy)
 *      1024        77.50 %       74.57 %      68.56 %
 *      2048        84.92 %       85.99 %      79.55 %
 *      4096        92.40 %          --        90.40 %
 *
 * At 2048 the IO workload sits at 79.55 %, and a cache that misses one access in five is
 * a materially different proposition from one that misses one in ten. **4096 entries,
 * 4-way, 64 KB.** The IO workload is the one that decides it, and it is also the one that
 * stresses this cache and the tier-0 page caches for L2 at the same time -- which is why
 * the pre-registered hit rates are per-workload.
 *
 * THE SET IS ONE CACHE LINE. 1024 sets x 64 B: the four tags are the first four words, so
 * the way scan is four loads out of a single Cortex-A9 line rather than four lines. That
 * is the difference between a hit path that pays for itself and the map's own
 * pre-registered failure mode -- "a high hit rate with a low speedup", which is what
 * happens when the tag check, the bounds check and the validity check cost as much as the
 * fetch they replace.
 *
 * THE IDIOM VERDICT RIDES ALONG. Rung 0 (the block-idiom fast path, z3660_blk.h) pays a
 * recognizer test on EVERY dispatch and is repaid only in the idiom stream -- metal
 * 2026-08-22 measured that as boot -43.6 % and dhrystone -1 %. So the entry carries the
 * recognizer's STATIC shape verdict beside the decode: on a hit that says "not an idiom",
 * the run loop skips the recognizer entirely and rung 0's per-dispatch cost is zero. The
 * bit is CONSERVATIVE-TRUE -- it is only ever cleared when the recognizer has positively
 * established that the words at this PC are not an idiom head, so an error in either
 * direction costs a wasted test and never a lost fast path.
 *
 * COHERENCE IS THE WHOLE RISK SURFACE, and it is asymmetric: a cache that invalidates too
 * eagerly is merely slower, one that invalidates too little executes instructions the
 * guest has already overwritten -- a working system that goes wrong hours later. So this
 * one INVALIDATES EVERYTHING on any event that could plausibly matter, which is O(1) here
 * because validity is a generation stamp rather than a state per entry.
 *
 * WHAT COUNTS AS "COULD PLAUSIBLY MATTER" is enumerated at z3660_dopc_invalidate() in
 * newcpu.cpp, and the DMA arm is the one that is specific to this board rather than to
 * the architecture: core0 writes guest DRAM directly on a piscsi or floppy read, and the
 * interpreter never sees those bytes go past.
 *
 * ------------------------------------------------------------------------------------
 * RUNG 1b -- NARROWING, AND THE NAMESPACE WALL THAT DECIDES WHAT CAN BE NARROWED
 * ------------------------------------------------------------------------------------
 *
 * The 2026-08-23 metal verdict measured the cost of the conservative policy and it is
 * the dominant term: **274-292 misses per invalidation across three unrelated workloads**
 * (boot, dhrystone, raw disk). A constant that does not move with the workload's code
 * footprint is not capacity -- and 285 is far below the 4096 entries the cache holds, so
 * the guest never comes close to filling it between flushes. The misses are re-warm.
 *
 * THE OBVIOUS NARROWING IS NOT AVAILABLE, and the reason is worth stating first because
 * it is the fact that shapes everything below. **This cache is tagged by guest LOGICAL
 * address. The two largest invalidation causes name PHYSICAL ones.**
 *
 *   CPUSHL / CINV   The 68040's caches are physically tagged, so `CPUSHL <cache>,(An)`
 *                   takes a PHYSICAL address in An. (Upstream's flush_mmu040() discards
 *                   the argument for exactly this reason.)
 *   DMA             core0 is handed the guest driver's DMA target, which is a physical
 *                   address by construction -- a descriptor a device could execute.
 *
 * Scoping either one against a logical tag would compare two different numbers and skip
 * an invalidation that was required. The host corpus already proves it: `dopc_m_dma()`
 * invalidates DOPC_PCODE (0x001C0000) for an entry tagged at DOPC_VCODE (0x00800000), so
 * a naive range test refuses the flush and the stale decode runs. Narrowing those two
 * needs a PHYSICAL index this cache does not maintain, and building one costs a
 * translation on the fill path. That is a separate rung with its own evidence, not this
 * one; **PFLUSH is the only cause that names a logical page**, and it is narrowed here.
 *
 * SO RUNG 1b NARROWS TWO THINGS, and both are namespace-free or logical:
 *
 *  1. THE EMPTY-CACHE SHORT-CIRCUIT, which applies to EVERY cause including the two
 *     above. If nothing has been filled since the last invalidation, no entry is live,
 *     so the invalidation has nothing to do and does not move the generation. This is
 *     the one that addresses the measured storm: with the block-idiom recognizer off the
 *     verdict measured 18x more invalidations (2.94 M against 160 k), because AMIX's
 *     CPUSHL loop invalidates once per instruction -- and consecutive iterations of that
 *     loop dispatch nothing in between, so all but the first have an empty cache to
 *     clear. It cannot be wrong: "no entry exists" is not an approximation.
 *
 *  2. PFLUSH OF ONE PAGE, through a PAGE-PRESENCE FILTER (below) -- a conservative
 *     summary of which logical pages the cache holds entries from. If the filter says no
 *     entry can lie in the flushed page, the flush is skipped; if it says one might, the
 *     whole cache goes exactly as before. The filter is allowed to say "maybe" when the
 *     answer is "no" (that is today's behaviour, merely slower) and is NEVER allowed to
 *     say "no" when the answer is "yes" -- which is the same asymmetry the policy above
 *     is built on, moved one level down.
 *
 * WHY A FILTER AND NOT A TAG WALK. A 4 KB guest page spans pc[11:1], and the set index is
 * pc[10:1] -- so a page touches EVERY ONE of the 1024 sets. "Invalidate just this page"
 * is therefore a full 4096-tag walk over 32 KB of tag lines, which is the whole Cortex-A9
 * L1 D-cache; it would evict the interpreter's working set on every PFLUSH to save a
 * re-warm of ~285 entries. The filter answers the same question with one load and one
 * test, and adds NOTHING to the hit path -- which is the constraint this rung exists
 * under ("a high hit rate with a low speedup" is its pre-registered failure mode).
 *
 * WHAT IS DELIBERATELY LEFT WHOLE-CACHE: MOVEC to URP/SRP, MOVEC to TC, PFLUSHA,
 * build_cpufunctbl() and map_banks(). Root and table changes are genuinely global and
 * genuinely rare, and a narrowing there would buy a rounding error at real risk.
 *
 * THE RULE LIVES IN A HEADER, and it is a RULE rather than a measurement, for the reason
 * amix_ram.h, overhead_switches.h and z3660_blk.h give: the host harness compiles it
 * directly and asserts every branch, including the ones no metal session will exercise.
 * Plain C and plain `unsigned` throughout, because core0's debug console includes it too.
 */
#ifndef SRC_UAE_Z3660_DOPC_H_
#define SRC_UAE_Z3660_DOPC_H_

/* ---- Mailbox word values (SHARED.dopc_on, 0xFFFF00E8) -------------------------------
 *
 * Same contract as z3660_blk.h: SHARED is plain memory, a core0 built before this field
 * existed leaves it 0, and if 0 meant "off" the shipped default would silently depend on
 * which core0 is in the image. So 0 is UNSET and resolves to the DEFAULT, and "off" gets
 * an explicit all-ones sentinel. */
#define Z3660_DOPC_UNSET    0u            /* nobody wrote it -> the default below      */
#define Z3660_DOPC_OFF      0xFFFFFFFFu   /* never consulted, never filled: passthrough */

/* The shipped default. ON: this is an optimisation, and the lean production image is the
 * one that most wants it -- the same call z3660_blk.h makes and for the same reason. */
#define Z3660_DOPC_DEFAULT  1u

/* requested -> 1 (cache live) or 0 (pure passthrough).
 *
 * A resolved 0 must be a PURE PASSTHROUGH: the lookup returns before it has read the
 * table, nothing is filled, and the extension-word window stays disarmed -- so the run
 * loop dispatches exactly the instruction it would have dispatched without this feature.
 * That is what makes the harness's state-equivalence corpus meaningful: the off state is
 * the reference implementation, not a second implementation that happens to agree. */
static inline unsigned z3660_dopc_resolve(unsigned requested)
{
   if (requested == Z3660_DOPC_UNSET) return Z3660_DOPC_DEFAULT;
   if (requested == Z3660_DOPC_OFF)   return 0u;
   return 1u;
}

/* ---- Geometry ----------------------------------------------------------------------
 * 4 ways x 1024 sets = 4096 entries at 16 B/entry = 64 KB, laid out as 1024 sets of 64 B
 * so one set is one cache line. Both numbers are asserted against sizeof() in the harness:
 * a set that grows past 64 bytes silently doubles the hit path's line traffic. */
#define Z3660_DOPC_WAYS       4u
#define Z3660_DOPC_SETS       1024u
#define Z3660_DOPC_ENTRIES    (Z3660_DOPC_WAYS * Z3660_DOPC_SETS)
#define Z3660_DOPC_SET_BYTES  64u
#define Z3660_DOPC_BYTES      (Z3660_DOPC_SETS * Z3660_DOPC_SET_BYTES)

/* Index on (pc >> 1): guest instructions are word-aligned, so bit 0 carries no
 * information and using it would halve the effective set count. */
#define Z3660_DOPC_INDEX(pc)  (((unsigned)(pc) >> 1) & (Z3660_DOPC_SETS - 1u))

/* The tag is the FULL guest pc with the address space in its otherwise-dead bit 0, so a
 * supervisor and a user entry for the same VA cannot alias -- the map's "tag on the full
 * guest PC plus an address-space stamp so a context switch cannot alias". The generation
 * half of that stamp is per-SET (z3660_dopc_set::stamp) and not per-entry, because every
 * invalidation this cache performs is whole-cache: a per-entry stamp would cost three
 * more compares on the hit path and could never disagree with its neighbours.
 *
 * RUNG 1b DOES NOT CHANGE THAT, and the distinction is the reason it costs nothing on the
 * hit path: the narrowing decides WHETHER an invalidation happens, never how much of the
 * cache it covers. Every invalidation that does happen is still the whole cache, still
 * one generation increment, and still read by one per-set compare.
 *
 * An ODD pc would collide with the supervisor tag of the instruction before it, so the
 * lookup refuses one outright rather than encoding it. Nothing can execute at an odd pc
 * on a 68040 (it raises before any fetch completes), so the branch is never taken on a
 * running guest -- it is there because "never taken" and "cannot happen" are different
 * claims and only one of them survives a future caller. */
#define Z3660_DOPC_KEY(pc, super)  (((unsigned)(pc) & ~1u) | ((super) ? 1u : 0u))

/* An empty way, and the marker is SET-RELATIVE for a reason that cost a crash to find.
 *
 * The obvious sentinel is a fixed odd constant -- 1, say, on the argument that a key is
 * an even pc so an odd one is impossible. It is not impossible: the key's bit 0 is the
 * supervisor bit, so key 1 is "pc 0 in supervisor mode", and a guest reaches exactly that
 * state by taking an exception through an unplanted vector. The harness's user-mode
 * CPUSHL case does it on the first run: privilege violation, vector 8 reads as zero, pc
 * becomes 0, S is set, the lookup builds key 1, matches an EMPTY way in set 0 -- because
 * 0x00400000 indexes set 0 too -- and calls a NULL handler out of a way nothing ever
 * filled. A wild pc of 0 is the single most likely wild pc there is, and this repository
 * has already spent a run on one.
 *
 * So the marker is built from the SET INDEX instead of from a constant. Every key stored
 * in set i satisfies ((key >> 1) & (SETS-1)) == i by construction, and (i ^ 1) << 1 has
 * index bits i^1. It therefore cannot equal any key that could legitimately live in this
 * set, for any pc and either address space -- which is a property of the encoding rather
 * than an argument about which addresses a guest is likely to reach. */
#define Z3660_DOPC_TAG_EMPTY(idx)  ((((unsigned)(idx)) ^ 1u) << 1)

/* ---- Entry metadata ---------------------------------------------------------------- */
#define Z3660_DOPC_M_IDIOM  0x01u   /* rung 0's static shape verdict: MAY be an idiom head */
#define Z3660_DOPC_M_EXT0   0x02u   /* ext0 holds the instruction word at pc + 2           */

/* ---- Invalidation causes ----------------------------------------------------------
 * ONE COUNTER PER CAUSE from rung 1b (Z3660_PROF_C_DOPC_IV_*, ids 28..37), because
 * "narrow the invalidation" is not a single decision -- each cause has its own frequency,
 * its own address namespace and its own correctness argument, and the aggregate
 * DOPC_INVAL cannot tell them apart. Measure, then narrow.
 *
 * APPEND-ONLY, and ids 0..6 keep their values. Two of them have a NARROWER MEANING than
 * they did in rung 1, which is the point of the split:
 *   FLUSH  was CPUSHL + PFLUSH + PFLUSHA + TC;  is now CPUSHL / CINV alone.
 *   ROOT   was URP + SRP + CACR;                is now URP / SRP alone.
 * A capture from a rung-1 firmware therefore reports FLUSH and ROOT as sums; the names
 * in the dump are what distinguishes the two, which is why they are compiled in. */
#define Z3660_DOPC_INV_FLUSH   0  /* CPUSHL / CINV -- flush_mmu040/060(). PHYSICAL addr  */
#define Z3660_DOPC_INV_ROOT    1  /* MOVEC to URP / SRP: the address space changed       */
#define Z3660_DOPC_INV_DMA     2  /* core0 wrote guest DRAM (piscsi / floppy). PHYSICAL  */
#define Z3660_DOPC_INV_TABLE   3  /* cpufunctbl rebuilt: every cached handler is stale   */
#define Z3660_DOPC_INV_KNOB    4  /* the switch moved                                    */
#define Z3660_DOPC_INV_WRAP    5  /* the generation counter wrapped (see the memset)     */
#define Z3660_DOPC_INV_MAP     6  /* the emulator's own memory map changed               */
#define Z3660_DOPC_INV_PFLUSH  7  /* PFLUSH of ONE page -- the ONLY cause that names a
                                   * LOGICAL page, and therefore the only one narrowable
                                   * against a logically-tagged cache. Split from FLUSH. */
#define Z3660_DOPC_INV_PFLUSHA 8  /* PFLUSHA, and mmu_set_tc()/mmu_reset() through it    */
#define Z3660_DOPC_INV_CACR    9  /* MOVEC to CACR. Split from ROOT: it is a cache-enable
                                   * write, not an address-space change, and AMIX's rate
                                   * of it was never separately visible.                 */
#define Z3660_DOPC_INV_COUNT  10

/* ---- The page-presence filter (rung 1b) --------------------------------------------
 *
 * A conservative summary of which logical pages the cache holds entries from, so a
 * page-scoped PFLUSH can be skipped outright when it cannot possibly matter.
 *
 * THE INVARIANT, and it is the whole correctness argument:
 *
 *     for every LIVE entry (a way in a set whose stamp == the current generation, whose
 *     tag is not the empty marker), the filter bit for that entry's pc IS SET.
 *
 * It holds because entries come into existence in exactly one place -- z3660_dopc_fill()
 * -- which sets the bit before it stores the tag that makes the way findable; and bits
 * are cleared in exactly one place, z3660_dopc_invalidate(), which in the same call moves
 * the generation, after which no set is live and the invariant is vacuous. Nothing else
 * writes a tag or a filter bit. `z3660_dopc_pf_audit()` walks the real table and asserts
 * it, so the harness checks the mechanism rather than a consequence of it.
 *
 * CONTRAPOSITIVE, which is what the skip uses: bit clear => no live entry in that page =>
 * an invalidation on account of that page is provably a no-op. A hash COLLISION sets a
 * bit for a page that holds nothing, which produces a needless whole-cache invalidation
 * -- i.e. exactly the rung-1 behaviour. So hash quality is a performance question and
 * never a correctness one.
 *
 * GRANULE 4 KB, the smaller of the two 68040 page sizes, so one guest page is one or two
 * granules and never a fraction of one. 4096 bits = 512 bytes = 16 Cortex-A9 lines: it
 * stays L1-resident, and clearing it costs ~100 cycles against a re-warm this is trying
 * to avoid paying ~50000 for.
 *
 * THE FOLD is A12..A23 xor A24..A35. Without it the AMIX kernel's text at 0x08000000 and
 * a user text page at 0x00000000 would land on the same bit, which is the single most
 * likely collision on this guest and would have made the filter useless on the workload
 * it was built for. */
#define Z3660_DOPC_PF_SHIFT  12u
#define Z3660_DOPC_PF_GRAN   (1u << Z3660_DOPC_PF_SHIFT)
#define Z3660_DOPC_PF_BITS   4096u
#define Z3660_DOPC_PF_WORDS  (Z3660_DOPC_PF_BITS / 32u)
#define Z3660_DOPC_PF_BYTES  (Z3660_DOPC_PF_WORDS * 4u)

static inline unsigned z3660_dopc_pf_slot(unsigned addr)
{
   unsigned pg = (unsigned)addr >> Z3660_DOPC_PF_SHIFT;
   pg ^= pg >> 12;
   return pg & (Z3660_DOPC_PF_BITS - 1u);
}

/* ---- The extension-word window -----------------------------------------------------
 *
 * The map asks the entry to hold "the instruction's extension words", but the generated
 * handlers fetch their own: cpuemu_31 calls get_iword_mmu040(N) roughly 920 times, and
 * nothing in the run loop can hand them a value. So the entry holds ONE extension word --
 * the map measures 0.5983 extension words per instruction, so one slot covers most of a
 * bucket worth 11.31 % -- and the run loop publishes it here, where the single out-of-line
 * copy of uae_mmu040_get_iword() can answer from it.
 *
 * THE WINDOW IS KEYED ON THE ADDRESS, not on the pc. That is what makes it safe: a handler
 * that has already moved the pc computes a different address and falls through to a real
 * fetch, so the window cannot answer for an instruction it does not describe. It is armed
 * only by a validated cache hit and disarmed by the next miss, by any invalidation, and by
 * the run loop's fault CATCH -- so its content has exactly the coherence of the entry it
 * came from, which is the point.
 *
 * NO_PC is odd, and every instruction-word fetch address is even, so a disarmed window can
 * never match. */
#define Z3660_DOPC_NO_PC  0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int   z3660_dopc_ext_pc;    /* Z3660_DOPC_NO_PC when disarmed */
extern unsigned short z3660_dopc_ext_val;

/* Whole-cache invalidation. O(1): it bumps a generation, so "invalidate everything" is
 * affordable enough to be the default policy rather than an admission of defeat. Also
 * disarms the extension-word window. Safe to call from any core1 context, including one
 * that has never touched the 68040 run loop.
 *
 * From rung 1b it counts the CAUSE unconditionally and then does the work only if there
 * is work to do (the empty-cache short-circuit). So the two counters answer two different
 * questions and the difference between them is the narrowing's yield:
 *
 *     sum of DOPC_IV_*   how often the guest ASKED for an invalidation
 *     DOPC_INVAL         how often one actually happened
 *
 * That identity is why the skips need no counter of their own, and it is checked in the
 * harness rather than assumed. */
void z3660_dopc_invalidate(int cause);

/* Narrowed invalidation for a LOGICAL address range -- today, PFLUSH of one page.
 *
 * The range must be the guest page the flush names, in the SAME namespace the tags use.
 * Do not call this with a physical address: see the namespace paragraph at the top of
 * this file, which is the reason CPUSHL and DMA do not come through here.
 *
 * len == 0 means "unknown extent" and is treated as the whole cache, not as an empty
 * range -- an unknown range is the one case where guessing small is the dangerous
 * direction. */
void z3660_dopc_invalidate_log_range(unsigned int addr, unsigned int len, int cause);

/* The DMA arm, called from core1's side of the cross-core mailbox once core0 has written
 * guest DRAM. Takes the range core0 wrote even though it invalidates everything: the
 * range is a PHYSICAL address and the tags are logical, so it cannot be compared against
 * them (top of file). The argument stays because a physical index is the natural next
 * rung and a call site that never carried the range could not grow one later. */
void z3660_dopc_invalidate_dma(unsigned int addr, unsigned int len);

/* Read-back + direct set for the console and the host harness. The setter takes a RAW
 * mailbox value and runs it through z3660_dopc_resolve(), so a console command and a
 * mailbox word cannot mean two different things. */
unsigned z3660_dopc_get(void);
unsigned z3660_dopc_set(unsigned requested);

/* Harness only. Nothing on the board calls any of these.
 *
 *   generation()        the live stamp, so a test can watch it move
 *   force_generation()  drive it to the wrap boundary without performing 2^32
 *                       invalidations. It also marks the cache LIVE: a hand-moved
 *                       generation can land on a stamp still in the table and revive a
 *                       set no fill touched, and the short-circuit would then skip an
 *                       invalidation that was needed. A test helper must not be able to
 *                       construct a state the firmware cannot reach.
 *   set_bytes()/        the REAL sizeof() of the table and one set, so the geometry is
 *   table_bytes()       asserted against the struct rather than against the constants it
 *                       was supposed to be built from
 *   peek()              the cached opcode at (pc, space), or -1 for "not cached". A
 *                       coherence test that can ask this directly asserts the mechanism;
 *                       one that can only infer it from execution asserts a consequence.
 *   live()              has anything been filled since the last invalidation? This is
 *                       the empty-cache short-circuit's own predicate, exposed so a test
 *                       can drive both sides of it deliberately instead of hoping.
 *   pf_test()           the filter's answer for one address: 1 == "an entry MAY lie in
 *                       this page", 0 == "provably none does".
 *   pf_audit()          walks the REAL table and returns the number of live entries whose
 *                       filter bit is clear. Must be 0 -- it is the filter invariant
 *                       stated as a number, and the only check that would catch a fill
 *                       path that forgot to set a bit. */
unsigned int z3660_dopc_generation(void);
void         z3660_dopc_force_generation(unsigned int gen);
unsigned     z3660_dopc_set_bytes(void);
unsigned     z3660_dopc_table_bytes(void);
int          z3660_dopc_peek(unsigned int pc, int super);
int          z3660_dopc_live(void);
int          z3660_dopc_pf_test(unsigned int addr);
int          z3660_dopc_pf_audit(void);

#ifdef __cplusplus
}
#endif

#endif /* SRC_UAE_Z3660_DOPC_H_ */
