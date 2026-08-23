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
 * RUNGS 1b AND 1c -- THE NARROWING THAT WAS BUILT, MEASURED, AND THEN PARTLY REMOVED
 * ------------------------------------------------------------------------------------
 *
 * This is one story with two commits in it, and the second is a REVERT of most of the
 * first. Both halves are recorded because the removed half is the more instructive one:
 * it was correct, it did what it was designed to do, and it still had to go.
 *
 * WHAT RUNG 1b SET OUT TO FIX. The 2026-08-23 metal verdict measured the conservative
 * policy's price: **274-292 misses per invalidation across three unrelated workloads**
 * (boot, dhrystone, raw disk). A constant that does not move with the workload's code
 * footprint is not capacity -- and 285 is far below the 4096 entries the cache holds, so
 * the guest never comes close to filling it between flushes. The misses are re-warm, and
 * that diagnosis was CONFIRMED by rung 1b's own numbers (see ROW D below).
 *
 * THE NAMESPACE WALL, which is the finding that outlived everything else here. **This
 * cache is tagged by guest LOGICAL address. The two largest invalidation causes name
 * PHYSICAL ones.**
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
 * translation on the fill path -- a rung of its own, and the attribution below is what
 * would fund it.
 *
 * ---- WHAT THE ATTRIBUTION SAID, BEFORE ANY TIMING (metal 2026-08-23) ----------------
 *
 * The IV_ counters were added precisely so this question could be answered, and the
 * answer is why two of rung 1b's three arms are gone:
 *
 *   cause                    boot      dhrystone   raw disk   narrowable?
 *   IV_FLUSH (CPUSHL/CINV)   63.83 %     51.16 %     46.63 %   NO  -- physical
 *   IV_PFLUSHA               27.15 %     36.37 %     35.66 %   no  -- names no page
 *   IV_PFLUSH                 6.15 %      9.14 %      6.85 %   YES -- the only one
 *   IV_DMA                    1.55 %      0.06 %      8.45 %   NO  -- physical
 *   IV_ROOT                   1.32 %      3.27 %      2.41 %   no  -- global
 *
 * **IV_FLUSH + IV_PFLUSHA is 82-91 % of every workload's invalidation requests and
 * neither can be narrowed against a logical tag.** The one cause the page filter was
 * built for is 6-9 %.
 *
 * ---- THE THREE ARMS, AND WHICH SURVIVED ---------------------------------------------
 *
 *  1. MOVEC to CACR NARROWED TO WRITES THAT CHANGE THE REGISTER -- **KEPT**. On a 68040
 *     the retained bits are the two cache ENABLES and nothing else (mask 0x80008000; the
 *     68040 has no clear-cache bits in CACR, it uses CINV/CPUSH, which arrive at
 *     mmu_flush_cache()), so if neither enable moved the cache configuration is
 *     bit-identical and nothing a cached decode depends on has changed. It cost one
 *     compare at ONE call site, nothing on the hit path and nothing on the fill path, and
 *     it removed **~93 % of all invalidations** on the dhrystone capture (about 41 000 of
 *     the 44 247 that went away). See newcpu_common.cpp for the exactness argument and
 *     for the cacr_mask == 0 tripwire.
 *
 *  2. THE EMPTY-CACHE SHORT-CIRCUIT -- **REVERTED in rung 1c**. "Nothing has been filled
 *     since the last invalidation, so there is nothing to invalidate" is exactly true and
 *     was never wrong. What it was justified on is what failed: rung 1's observation that
 *     AMIX's CPUSHL loop invalidates once per instruction with nothing dispatched in
 *     between (18x more invalidations, 2.94 M against 160 k) was measured with the
 *     block-idiom recognizer OFF. **With BLK ON -- the shipping posture -- rung 0's fast
 *     path already collapses that whole loop into one flush_mmu040().** The storm the
 *     short-circuit exists to absorb has already been eliminated before it is reached, so
 *     it paid a flag store on every fill to catch a case that no longer occurs.
 *
 *  3. THE PAGE-PRESENCE FILTER on PFLUSH -- **REVERTED in rung 1c**. 4096 hashed bits
 *     over a 4 KB logical granule, marked on every fill; sound, mutation-tested, and
 *     aimed at a 6-9 % cause. It skipped 5.6-8.2 % of requests for two filter marks and
 *     512 B of BSS on the fill path, and the metal arithmetic below says those marks cost
 *     more than the skips saved.
 *
 * ---- WHY A CORRECT NARROWING WAS STILL A REGRESSION ---------------------------------
 *
 * Rung 1b did what it was designed to do and it did not pay:
 *
 *   invalidations performed, dhrystone   80 719 -> 36 472   (-54.8 %)
 *   hit rate, dhrystone                  77.48 % -> 84.56 %  (+7.08 pp)
 *   dhrystone, lean, cadence 1           5432.0 -> 5306.0    (-2.32 %)
 *   same-boot ON/OFF speedup             +41.57 % -> +40.52 % (-1.05 pp)
 *
 * **+7.08 pp of hit rate bought -1.05 pp of speedup**, which is this rung's own
 * pre-registered failure mode ("a high hit rate with a low speedup") arriving exactly
 * where it said it would. The mechanism follows from what was removed: the misses the
 * narrowing eliminated are RE-WARM misses -- refills of a page the machine has just
 * flushed, whose bytes are still hot in ipagecache (91.61 % on that capture) and in the
 * ARM's own caches. **They were the CHEAP misses.** The design priced every avoided miss
 * at the marginal ~180 ARM cycles; the metal says the ones actually avoided are worth a
 * fraction of that, while the per-fill marks were paid on every fill regardless.
 *
 * ROW D, and the reason it is not evidence against the diagnosis: misses per invalidation
 * moved 274.3 -> 413.2, outside the "unchanged" band rung 1b pre-registered as a
 * falsifier. But that ratio has the invalidation count in its denominator and the
 * narrowing's whole job is to cut it: misses -31.9 %, invalidations -54.8 %, so the ratio
 * moves x(0.681/0.452) = x1.506, i.e. 274.3 -> 413.6 predicted against 413.2 measured.
 * **The row moved BECAUSE the narrowing worked.** Misses FELL when invalidations fell,
 * which is the signature of re-warm; a capacity-bound cache's miss count would have
 * stayed pinned to its workload's footprint. The pre-registration mis-specified the
 * falsifier -- the correct one is "misses do NOT fall when invalidations fall".
 *
 * ---- WHAT IS LEFT, AND WHAT IS OWED -------------------------------------------------
 *
 * Invalidation is whole-cache again for every cause, exactly as in rung 1; the only
 * narrowing in the firmware is the CACR compare, which happens before this file is
 * reached at all. The next narrowing worth attempting is a PHYSICAL INDEX, because that
 * is the only thing the namespace wall leaves standing and IV_FLUSH's measured 46.70-63.89 %
 * is what would fund it. It is its own design round: it costs a translation on the fill
 * path, which is the cost class that just failed here, so it needs its own pre-registration
 * with the fill-path price measured rather than assumed.
 *
 * AND THAT PRICE IS NOW MEASURABLE. C2 rung 1d gives the fill its own profiler bucket
 * (DOPCFILL, id 15) and the lookup its own (DOPCFIND, id 14), so "what the fill path costs"
 * stopped being an argument from ARM instruction counts and became a share of the
 * interpreter, with tcnt[DOPCFILL] == DOPC_MISS as an exact identity the dump checks. Write
 * the physical index's pre-registration against a capture, not against this comment.
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
 * NEITHER RUNG 1b NOR 1c CHANGED THAT. 1b's narrowing decided WHETHER an invalidation
 * happened, never how much of the cache it covered, and 1c removed most of the deciding.
 * Every invalidation is still the whole cache, still one generation increment, and still
 * read by one per-set compare.
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
/* The HANDLER discriminator's static shape verdict (z3660_spec.h): this opcode is a
 * covered MOVE/MOVEA shape AND the switch was armed when the entry was filled. It shares
 * this byte deliberately -- the hit path already holds it in a register, so the added
 * work on a dispatch that is not specialised is a `tst` and a not-taken branch. Because
 * the bit records the switch state at fill time, moving the switch invalidates. */
#define Z3660_DOPC_M_SPEC   0x04u   /* dispatch this entry through z3660_spec_move()      */

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
 * It counts the CAUSE (rung 1b's attribution, which is kept) and then performs the
 * invalidation. Every request that reaches here performs one, unless the cache is SWITCHED
 * OFF -- so on a cache-on capture
 *
 *     sum of DOPC_IV_*  ==  DOPC_INVAL
 *
 * and a dump in which they differ has a cause id out of range or a call site bypassing the
 * attribution. The harness checks it; the firmware warns on the dump.
 *
 * WITH THE CACHE OFF it returns without touching the generation, so DOPC_INVAL reads
 * exactly 0 while the IV_ block keeps counting what the GUEST asked for. That is not
 * bookkeeping -- it makes a cache-off arm a free, uncontaminated measurement of the
 * guest's own invalidation rate, which is the denominator every future narrowing decision
 * needs. (Rung 1 had no switch test here at all and metal read DOPC_INVAL 164 311 and
 * 2 951 874 in its two cache-off arms while the docs claimed zero.)
 *
 * IT IS SAFE BECAUSE THE OFF STATE IS A PURE PASSTHROUGH IN BOTH DIRECTIONS: with the
 * switch off nothing is filled and no lookup consults the table, so its contents cannot
 * reach the guest; and the switch can only come back on through z3660_dopc_set() or the
 * knob resync, both of which set z3660_dopc_on FIRST and then invalidate -- so the
 * transition back to ON always performs the invalidation that kills whatever went stale
 * while the cache was off. */
void z3660_dopc_invalidate(int cause);

/* Rung 1c: a request a CALL SITE proved inert, counted and not performed.
 *
 * MOVEC to CACR that changes no retained bit is the one narrowing rung 1b's metal verdict
 * kept, and it decides at the call site -- so z3660_dopc_invalidate() is never reached and
 * IV_CACR never counts it. The verdict found that defect the hard way: it measured a 55 %
 * fall in invalidations while the instrument could only account for 7 % of it, because
 * "requests that reached the counter" and "requests the guest made" had silently become
 * two different quantities.
 *
 * So the rule, and it is general: A NARROWING THAT DECIDES BEFORE z3660_dopc_invalidate()
 * MUST BRING ITS OWN REQUEST COUNTER, or it is invisible to the row that scores it. This
 * is CACR's, and a second call-site narrowing brings a second one rather than sharing it,
 * because a counter that aggregates two causes cannot attribute either. */
void z3660_dopc_cacr_skipped(void);

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
 *                       invalidations
 *   set_bytes()/        the REAL sizeof() of the table and one set, so the geometry is
 *   table_bytes()       asserted against the struct rather than against the constants it
 *                       was supposed to be built from
 *   peek()              the cached opcode at (pc, space), or -1 for "not cached". A
 *                       coherence test that can ask this directly asserts the mechanism;
 *                       one that can only infer it from execution asserts a consequence. */
unsigned int z3660_dopc_generation(void);
void         z3660_dopc_force_generation(unsigned int gen);
unsigned     z3660_dopc_set_bytes(void);
unsigned     z3660_dopc_table_bytes(void);
int          z3660_dopc_peek(unsigned int pc, int super);

#ifdef __cplusplus
}
#endif

#endif /* SRC_UAE_Z3660_DOPC_H_ */
