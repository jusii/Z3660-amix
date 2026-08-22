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
 * because validity is a generation stamp rather than a state per entry. Narrowing it is a
 * later change, and the DOPC_INVAL counter is what will say whether it is worth making.
 *
 * WHAT COUNTS AS "COULD PLAUSIBLY MATTER" is enumerated at z3660_dopc_invalidate() in
 * newcpu.cpp, and the DMA arm is the one that is specific to this board rather than to
 * the architecture: core0 writes guest DRAM directly on a piscsi or floppy read, and the
 * interpreter never sees those bytes go past.
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
 * Reported as one counter today; the split exists so the narrowing step has somewhere to
 * put its evidence without renumbering anything. APPEND-ONLY. */
#define Z3660_DOPC_INV_FLUSH  0   /* CPUSHL / CINV / PFLUSH / TC write / MMU reset      */
#define Z3660_DOPC_INV_ROOT   1   /* MOVEC to URP / SRP / CACR: address space or caches */
#define Z3660_DOPC_INV_DMA    2   /* core0 wrote guest DRAM (piscsi / floppy read)      */
#define Z3660_DOPC_INV_TABLE  3   /* cpufunctbl rebuilt: every cached handler is stale   */
#define Z3660_DOPC_INV_KNOB   4   /* the switch moved                                    */
#define Z3660_DOPC_INV_WRAP   5   /* the generation counter wrapped (see the memset)     */
#define Z3660_DOPC_INV_MAP    6   /* the emulator's own memory map changed               */

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
 * that has never touched the 68040 run loop. */
void z3660_dopc_invalidate(int cause);

/* The DMA arm, called from core1's side of the cross-core mailbox once core0 has written
 * guest DRAM. Takes the range core0 wrote even though today it invalidates everything --
 * the information is what a narrowed invalidation will need, and a call site that never
 * had it cannot grow one later. */
void z3660_dopc_invalidate_dma(unsigned int addr, unsigned int len);

/* Read-back + direct set for the console and the host harness. The setter takes a RAW
 * mailbox value and runs it through z3660_dopc_resolve(), so a console command and a
 * mailbox word cannot mean two different things. */
unsigned z3660_dopc_get(void);
unsigned z3660_dopc_set(unsigned requested);

/* Harness only. Nothing on the board calls any of these.
 *
 *   generation()        the live stamp, so a test can watch it move
 *   force_generation()  drive it to the wrap boundary without performing 2^32 invalidations
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
