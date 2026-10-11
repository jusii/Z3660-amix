/* WARP ENGINE -- the block engine's contract: the switch rule, the block key, the
 * geometry, the block ABI, and the invariant that decides what every counter in it means.
 *
 * docs/WARP-ENGINE.md is the plan; §0.5 is the M1 pre-registration, §0.6 is round 1's and
 * §0.8 is round 2's, each committed before the code it describes. Read §0.8 before changing
 * anything here.
 *
 * WHAT THIS IS. A JIT for the guest 68040 while its MMU is on: guest basic blocks are
 * discovered at control-transfer targets, compiled to ARM, cached, and executed in place of
 * the interpreter -- with the interpreter kept permanently as both the fallback path and the
 * differential oracle.
 *
 * WHAT ROUND 2 IS. The first round that generates code. Round 1 proved the plumbing with a
 * null block and priced the entry at 60 ARM instructions per guest instruction at a MEASURED
 * IPC of 0.46; §0.7.3 named four shapes in that 60 that were "not the engine" and refused to
 * fix them in the round they were scored in. Round 2 fixes them, restricts discovery to
 * control-transfer targets so the entry amortises over a block, replaces the lookup structure
 * that recompiled 3.07 million times for a working set of order 10^3, and compiles
 * generation one's frozen family set.
 *
 * ------------------------------------------------------------------------------------
 * THE NO-EMPTY-PASS INVARIANT, and it is the reason the counters mean what they say
 * ------------------------------------------------------------------------------------
 *
 *   A block entry either RETIRES >= 1 guest instruction and CONSUMES the run-loop pass,
 *   or RETIRES 0 and FALLS THROUGH to the interpreter INSIDE THE SAME PASS.
 *   There is no third outcome, and in particular no pass that retires nothing.
 *
 * Round 1 had only the fall-through arm, because every block was null. Round 2 has both:
 *
 *   WARP_ENTER (47)  counts PASS-CONSUMING entries only -- now non-zero for the first time,
 *                    which is what makes the coverage identity load-bearing rather than
 *                    vacuous:  DOPC_HIT + DOPC_MISS  ==  (INSNS - WARP_ENTER) + FAULTS
 *   WARP_BLK   (48)  counts EVERY block execution, bailing ones included, and it is the
 *                    quantity the arm attestation rides on -- one increment site, not two.
 *
 * ------------------------------------------------------------------------------------
 * THE BAIL CONTRACT, IN THE FORM ROUND 2 MAKES IT CHEAP
 * ------------------------------------------------------------------------------------
 *
 * §0.3(b) offered two shapes for the guest logical PC and §0.5.4 pre-registered shape (ii):
 * block-head PC plus a per-site constant delta, materialised ONLY on the bail arm. Round 2
 * takes it and takes one further decision that makes the whole contract nearly free:
 *
 *   THE GUEST REGISTER FILE STAYS IN MEMORY. Generated code loads its operands from
 *   regs.regs[] and stores its results back inside the same guest instruction. No guest
 *   register is ever live in a host register across an instruction boundary.
 *
 * So at EVERY instruction boundary in a block the architectural state already IS the
 * interpreter's boundary state, on-path state-sync per faultable site is 0 emitted
 * instructions, and a bail materialises the PC and nothing else. The price is named rather
 * than hidden: generation one buys its speed from dispatch, fetch and decode, not from
 * register allocation, and Route B's allocator stays what §0.1 Q1 said it was -- a redesign
 * to be funded by §0.5.4's two measured conditions, never by taste.
 *
 * Every exit from a block -- normal or bail -- writes the guest PC and returns the number of
 * guest instructions FULLY RETIRED, which is a compile-time constant at every exit site.
 *
 * ------------------------------------------------------------------------------------
 * THE RULE LIVES IN A HEADER
 * ------------------------------------------------------------------------------------
 *
 * Same reason amix_ram.h, overhead_switches.h, z3660_blk.h, z3660_dopc.h and z3660_dfast.h
 * give: core0's serial console and core1's knob resync must not be able to disagree about
 * what a mailbox word means, so the mapping lives here and nowhere else -- and the host
 * harness compiles it directly and asserts every branch, including the ones no metal
 * session will ever exercise. The switch half is plain C and plain `unsigned`, because
 * core0's debug console and config parser both include it; the engine half is guarded so
 * core0 never sees it.
 */
#ifndef SRC_UAE_Z3660_WARP_H_
#define SRC_UAE_Z3660_WARP_H_

#include "z3660_warp_ablate.h"

/* ---- Mailbox word values (SHARED.warp_on, 0xFFFF00F0) -------------------------------
 *
 * THE POLARITY IS THE OPPOSITE OF blk_chunk / dopc_on / dfast_on, ON PURPOSE, and it is
 * the decision recorded as Q9 in the plan. Those three read "unset -> the built-in default
 * -> ON", because for them the shipped behaviour IS the optimisation and a core0 built
 * before the field existed must still run it. The reasoning here is identical and the
 * value is inverted: unset means the SHIPPED behaviour, and an engine that is not shipped
 * behaviour until the M5 gate has a shipped behaviour of OFF.
 *
 * It is a deliberate inversion of the house rule and it expires on the user's word at M5,
 * not silently and not by a later reading of this comment.
 *
 * ARMING TAKES AN EXACT MAGIC, not `1` and not "anything non-zero" -- the withdrawn IVSUP
 * switch's precedent, adopted for the same reason it was adopted there. SHARED is
 * hand-maintained in TWO headers that a beta resync has already mis-merged once
 * (docs/piscsi-service-path.md), so a neighbouring field's value bleeding into this one
 * must DISARM. A magic-word test guarantees that; a truthiness test does not. The value is
 * ASCII "WARP" and collides with nothing any neighbouring field can hold: not 0, not 1,
 * not 0xFFFFFFFF, not 64/256/1024, not a cadence, not a RAM size in MiB.
 *
 * AND IT MUST NOT SURVIVE A REBOOT. The mailbox is DDR that survives a warm reset --
 * config_file.c says so about prof_ctl -- so "the field defaults to OFF" is not enough on
 * its own: a word the console wrote in one boot is still sitting there in the next. core0
 * therefore PUBLISHES THE OFF SENTINEL UNCONDITIONALLY from both config-resolve paths on
 * every boot, which is resolve_prof_config()'s pattern verbatim and for the same stated
 * reason: writing it every time makes "not armed" an explicit instruction rather than the
 * absence of one. There is deliberately NO config-file key -- the console command WARP is
 * the only way in, and an arm lasts one boot. */
#define Z3660_WARP_OFF   0u            /* unset AND the shipped state: DISARMED          */
#define Z3660_WARP_ARM   0x57415250u   /* ASCII "WARP" -- the one value that arms         */

/* THE ON-TARGET SELF-TEST REQUEST, and it is a SECOND MAGIC ON THE SAME WORD rather than a
 * new SHARED field -- SHARED is hand-maintained in two headers and a beta resync has already
 * mis-merged one, so the cheapest correct thing is to add no field at all.
 *
 * IT DOES NOT ARM AND CANNOT: z3660_warp_resolve() below is UNCHANGED and returns 0 for it,
 * because only Z3660_WARP_ARM arms. core1 notices the word at its next knob resync, CONSUMES
 * it by writing the OFF sentinel back -- z3660_prof.cpp's prof_ctl pattern verbatim, "consume
 * before acting", and for the same reason: the test takes a visible amount of time -- runs the
 * families, prints the report, and leaves the engine disarmed.
 *
 * WHY IT SHIPS IN EVERY VARIANT. The wall-clock arm of every verdict is a LEAN boot, so a
 * self-test that existed only in a debug variant would attest a DIFFERENT BINARY from the one
 * armed -- which is exactly the class of error round 3 is a bug report about (plan 0.10.2).
 * It is cold .text that never executes unless the console asks for it. */
#define Z3660_WARP_SELFTEST 0x57545354u /* ASCII "WTST" -- run the on-target self-test     */

/* requested -> 1 (engine live) or 0 (disarmed).
 *
 * A resolved 0 must be a PURE PASSTHROUGH: the run loop's hook returns before it has
 * looked at anything, nothing is discovered, nothing is compiled and no block is entered,
 * so the loop dispatches exactly the instruction it would have dispatched without this
 * feature. That is what makes the harness's state-equivalence corpus meaningful -- the off
 * state is the reference implementation, not a second implementation that happens to
 * agree. */
static inline unsigned z3660_warp_resolve(unsigned requested)
{
   return (requested == Z3660_WARP_ARM) ? 1u : 0u;
}

/* ---- THE PUBLISH RANGE, and it is where round 2 died --------------------------------
 *
 * ROUND 3'S ROOT CAUSE, in one sentence: the firmware's two CP15 line loops round their
 * LENGTH up and never round their ADDRESS down, so a block whose bytes reach past the last
 * line the loop touches is executed from memory the loop never maintained.
 *
 *   jit/compemu_midfunc_arm.cpp:566 and :578, both the same shape:
 *      length = (length + 31) & ~31;
 *      while (length) { <CP15 op on addr>; addr += 32; length -= 32; }
 *
 * With k = ceil(len/32) iterations the loop touches k CONSECUTIVE LINES starting at the line
 * containing `addr`, so its byte coverage is [addr & ~31, (addr & ~31) + 32k) -- and a block
 * occupies [addr, addr + bytes). The last line is left unmaintained whenever
 *
 *      (addr & 31) + bytes  >  32 * ceil(bytes / 32)
 *
 * THE D-CACHE HALF IS THE LOAD-BEARING HALF. arm_flush_cache() is DCCIMVAC -- clean and
 * invalidate to PoC -- so a line it does not touch stays DIRTY IN L1D AND NEVER REACHES PoC.
 * The Cortex-A9 has no I/D coherency: the instruction-side linefill reads PoC, which still
 * holds what was there before the compiler wrote the block. The core executes the block
 * correctly up to its last line and then executes stale memory -- zeros (which decode as
 * `andeq r0,r0,r0`, so it walks PAST the block's own `pop {r4-r8,pc}`) on a virgin arena, or
 * a previous block's bytes after the bump pointer has been reset over written arena.
 *
 * ROUND 1 WAS STRUCTURALLY IMMUNE AND ROUND 2 IS A COIN FLIP, and that is the discriminator
 * the R2 metal session measured without knowing what it was measuring. Round 1's blocks were
 * ALWAYS eight bytes and the bump advanced by eight, so `addr & 31` was one of {0,8,16,24} and
 * (addr&31)+8 <= 32 always. Round 2's blocks are 4-aligned and variable: over the eight
 * reachable offsets and the eight reachable length residues, 28 of the 64 pairs are uncovered
 * -- 43.75 % of every block published.
 *
 * THE FIX IS AT WARP'S CALL SITE AND NOT IN THE SHARED LOOPS. The two primitives are also the
 * classic JIT's, reached through flush_cpu_icache(), and changing them would move behaviour
 * outside this engine in a round that is not about that. WARP rounds the range OUT to whole
 * lines at both ends before it calls them, and separately keeps every block on a line of its
 * own so the rounding is exact rather than merely safe.
 *
 * The predicate lives HERE, in plain C outside the engine guard, because the host harness
 * asserts it against hand-computed cases -- including round 1's geometry, which is the arm
 * that keeps the discriminator honest -- and a rule the test re-derives is not the rule the
 * firmware runs. */
#define Z3660_WARP_LINE  32u    /* Cortex-A9 L1 cache line, and the CP15 loops' stride     */

/* The byte range the CP15 line loop ACTUALLY maintains when called with (addr, len).
 * Transcribed from the loops above and from nowhere else. */
static inline unsigned z3660_warp_line_lo(unsigned addr)
{
   return addr & ~(Z3660_WARP_LINE - 1u);
}
static inline unsigned z3660_warp_line_hi(unsigned addr, unsigned len)
{
   unsigned k = (len + (Z3660_WARP_LINE - 1u)) / Z3660_WARP_LINE;   /* the loop's own k */
   return z3660_warp_line_lo(addr) + k * Z3660_WARP_LINE;
}
/* Does a CP15 line loop called with (call_addr, call_len) reach every byte of the block
 * [blk_addr, blk_addr + blk_bytes)? Round 2 called it with (blk_addr, blk_bytes) and the
 * answer is NO for 28 of the 64 reachable (offset, length-residue) pairs. */
static inline int z3660_warp_publish_covers(unsigned call_addr, unsigned call_len,
                                            unsigned blk_addr, unsigned blk_bytes)
{
   return (z3660_warp_line_lo(call_addr) <= blk_addr)
       && (z3660_warp_line_hi(call_addr, call_len) >= blk_addr + blk_bytes);
}
/* The length WARP passes, so that the answer above is yes for every (addr, bytes). */
static inline unsigned z3660_warp_publish_len(unsigned addr, unsigned bytes)
{
   return ((addr & (Z3660_WARP_LINE - 1u)) + bytes + (Z3660_WARP_LINE - 1u))
          & ~(Z3660_WARP_LINE - 1u);
}

/* ---- Block-cache geometry and keying -------------------------------------------------
 *
 * KEYED ON GUEST LOGICAL PC WITH THE SUPERVISOR BIT, and that sentence is the correction
 * M0 bought. The plan assumed the existing UAE block table (cache_tags[65536], two slots
 * per block head) could be reused; census (c) refuted the KEYING while confirming the
 * STRUCTURE: cacheline(x) is ((uintptr)x) & 0xFFFF applied to regs.pc_p, a HOST pointer,
 * and under the MMU there is no pc_p. So the table is rebuilt on the dopc's encoding,
 * which is the one this firmware has already proven against a real guest.
 *
 * ---- THE ROUND-2 GEOMETRY, AND THE MEASUREMENT THAT FORCED IT ------------------------
 *
 * Round 1 was DIRECT MAPPED over 1024 sets with INDEX(pc) = (pc >> 1) & 1023, and wrote
 * down that a way count was "a decision to take against a measured head distribution
 * rather than by analogy". The measured head distribution came back and it says take the
 * ways: 3 066 944 compiles against a static working set of order 10^3 distinct PCs, with
 * WARP_DISC explaining 4.8 % of them and WARP_FLUSH explaining none -- 95.2 % unattributed.
 *
 * The diagnosis is structural and reads off the round-1 source without a capture: an index
 * built from PC bits 1..10 alone addresses a 2 KiB WINDOW OF GUEST TEXT. Two block heads
 * 2 048 bytes apart share one slot, and with one way each arrival evicts the other. The
 * decoded-op cache uses a bit-for-bit identical index and does NOT thrash -- because it has
 * four ways, and because it caches a DECODE, which costs one to three orders of magnitude
 * less to rebuild than a BLOCK. Round 1 inherited the index and not the associativity.
 *
 * So round 2 changes three things and instruments the fourth:
 *
 *   WAYS   1 -> 4        the dopc's geometry, now with a measured distribution behind it.
 *   SETS   1024 -> 2048  2048 x 4 x 16 B = 128 KiB, and a SET IS 64 B = ONE Cortex-A9 LINE,
 *                        so a lookup still touches one line and never two. That is the
 *                        dopc's own Z3660_DOPC_SET_BYTES 64 constraint, inherited.
 *   INDEX  hashed        (pc>>1) ^ (pc>>12) ^ (pc>>23): HIGH PC BITS PARTICIPATE, so two
 *                        heads 2 KiB, 4 KiB or 1 MiB apart no longer collide by
 *                        construction. It costs TWO EORS ON A PATH THAT RUNS ONCE PER
 *                        BLOCK -- which is exactly the arithmetic §0.5.6 says reverses for
 *                        a block table and does not for the dopc.
 *   WARP_EVICT (id 60)   §0.7's own words: "a table-occupancy counter settles it in round
 *                        2". It counts a LIVE block overwritten in its set by a different
 *                        key -- the one term that separates conflict from invalidation and
 *                        from flush, so a recompile budget can be decomposed rather than
 *                        argued about.
 *
 * THE EMPTY MARKER IS SET-RELATIVE, and it is inherited whole because the dopc paid for it
 * with a crash. The obvious sentinel is a fixed odd constant -- 1, say, on the argument
 * that a key is an even pc so an odd one is impossible. It is not impossible: bit 0 is the
 * supervisor bit, so key 1 is "pc 0 in supervisor mode", and a guest reaches exactly that
 * by taking an exception through an unplanted vector. Built from the set index instead,
 * the marker cannot equal any key that could legitimately live in this set: INDEX() of the
 * marker of set i is i^1, never i, for every one of the 2048 sets and under the hashed
 * index as much as under the direct one. Mutation M-5 (the key drops the supervisor bit)
 * is the arm that keeps this honest. */
#define Z3660_WARP_SETS           2048u
#define Z3660_WARP_WAYS           4u
#define Z3660_WARP_INDEX(pc)      (((((unsigned)(pc)) >> 1) ^ (((unsigned)(pc)) >> 12) \
                                    ^ (((unsigned)(pc)) >> 23)) & (Z3660_WARP_SETS - 1u))
#define Z3660_WARP_KEY(pc, super) (((unsigned)(pc) & ~1u) | ((super) ? 1u : 0u))
#define Z3660_WARP_TAG_EMPTY(idx) ((((unsigned)(idx)) ^ 1u) << 1)

/* ---- Block shape -------------------------------------------------------------------
 *
 * A block ENDS at the first of: an uncovered opcode (WARP_DECLINE), a control transfer, a
 * write-back-family exclusion (§2e), the guest page boundary (the structural rule -- a
 * block never spans two pages, so one instruction-fetch validity gate covers the whole
 * block), or the length cap below. The cap is not a tuning knob: it bounds the compiler's
 * inline work against rung 0's measured ~42 us budget (Q4) and bounds the emitted bytes a
 * single fill can take out of the arena. */
#define Z3660_WARP_MAX_INSNS      32u
#define Z3660_WARP_MAX_BYTES      4096u   /* emitted ARM bytes, per block                 */

/* ---- Decline reasons, for the WARP_DECLINE counter ----------------------------------
 *
 * The supervisor gate is NOT in this list and its absence is a decision. Generation one is
 * user-mode only, so a supervisor PC is declined BEFORE discovery is attempted -- and
 * counting one decline per supervisor instruction would make WARP_DECLINE a second copy of
 * INSNS_SUPER and say nothing that counter does not already say. The supervisor rate is
 * read from INSNS_SUPER. */
#define Z3660_WARP_DECL_TRACE   0   /* regs.t0 || regs.t1 -- trace on implies interpret   */
#define Z3660_WARP_DECL_ODDPC   1   /* an odd pc cannot be a block head                   */
#define Z3660_WARP_DECL_NOCACHE 2   /* no code-cache arena, so nothing can be compiled    */
#define Z3660_WARP_DECL_HEAD    3   /* the head opcode is not in generation one's scope   */
#define Z3660_WARP_DECL_FETCH   4   /* the head's instruction words could not be read
                                     * without faulting -- the compiler never faults      */
#define Z3660_WARP_DECL_COUNT   5

#ifdef __cplusplus
extern "C" {
#endif

/* THE GATE: the arm flag and the previous dispatched pc, IN ONE STRUCT AND ON PURPOSE.
 *
 * Both are read on the armed dispatch path, and this target has no literal pool
 * (-DARMV6T2, §0.3(c)), so every global's address is materialised by a two-instruction
 * `ldr rX,[pc,#...]` of its own. Two globals cost two materialisations; one struct costs
 * one, and the second field is then a displacement on a base the code already holds. That
 * is four ARM instructions off the armed non-target path, measured rather than assumed --
 * the first arrangement read 18 on the lean objdump against a pre-registration of 3..6.
 *
 * The macros keep every call site reading `z3660_warp_on`, which is what core0's console
 * and three years of comments call it.
 *
 * DISARMED THE HOOK IS ONE ADDRESS MATERIALISATION, ONE LOAD, ONE TEST AND A PERFECTLY
 * PREDICTED BRANCH, and that is a SHIPPED cost until the M5 gate rather than a free one:
 * §30's pricing law says work prepended to the path arrives in full, and the one thing this
 * round must not do is charge itself nothing. */
/* ROUND 4 MOVES THE ARM FLAG OUT OF THIS STRUCT AND INTO `regs`, AND THE REASON IS THE ONE
 * MEASUREMENT ROUND 3 COULD NOT EXPLAIN (plan §0.12.1).
 *
 * Round 3 took the body out of line and the disarmed gate fell from five instructions to
 * four -- and the penalty went the WRONG WAY, -2.276 % to -4.056 %, on a loop 128 bytes
 * SMALLER than round 1's. §0.10.6's L1I-footprint mechanism cannot explain that and §0.11.4
 * left it owed. The objdump of the image that was actually measured does explain it:
 *
 *     3024f490:  blx  r3                  <- the interpreter handler returns here
 *     3024f494:  ldr  r3, [pc, #2028]     <- literal pool: the ADDRESS of the gate
 *     3024f498:  ldr  r3, [r3]            <- the flag, DEPENDENT on the load above
 *     3024f49c:  cmp  r3, #0
 *     3024f4a0:  beq  ...
 *
 * A SERIAL LOAD-TO-LOAD ADDRESS DEPENDENCY WITH ZERO SLACK, issued the instant the handler
 * returns so nothing hides either use latency. Round 1's five were two `movw`/`movt` ALU
 * immediates -- no memory port, freely scheduled -- feeding ONE load. The instruction count
 * fell and the dependency depth rose, and it is the depth that is billed: 8.69 cycles for
 * four instructions against 4.87 for five, on §0.9.3's own 214.2 cycles per guest
 * instruction. gcc chose it because -Os prefers one instruction to two, which is the
 * "an instruction count is not a cost" error §0.4.3 named, made in the other direction.
 *
 * So `on` moves into `regstruct`, beside the pc fields the run loop already reads and writes
 * on EVERY pass, and the gate becomes `ldr rX,[r4,#off]` / `cmp` / `b` -- three
 * instructions, one load, NO address materialisation, on a base that is already live and a
 * line that is already hot. `last_pc` stays here: it is read only on the ARMED path, inside
 * the out-of-line step(), which pays its own materialisation once and does not care.
 *
 * The macro spelling is unchanged, which is the point -- core0's console and three years of
 * comments call it `z3660_warp_on` and none of them had to move. */
struct z3660_warp_gate_s {
   unsigned last_pc;    /* the previously dispatched pc -- the control-transfer test     */
};
extern struct z3660_warp_gate_s z3660_warp_gate;
/* THE ABLATION KIT'S L-REG LAYER (§0.14.1). At level 4 and above the flag leaves
 * `struct regstruct` and becomes a plain global again, which puts the displacement of every
 * member after it back where it sat before round 4 -- 7 899 function bodies by §0.13.3's
 * census. It is paired with L-HOOK ON PURPOSE: with the hook gone nothing on the disarmed
 * path reads the flag at all, so A3 and A4 are DYNAMICALLY IDENTICAL and their difference is
 * layout and nothing else. */
#if Z3660_WARP_ABL_NO_REGFIELD
extern int z3660_warp_on_g;
#define z3660_warp_on       z3660_warp_on_g
#else
#define z3660_warp_on       (regs.warp_on)
#endif
#define z3660_warp_last_pc  (z3660_warp_gate.last_pc)

/* Arm/disarm from a RAW mailbox value, run through z3660_warp_resolve() so a console
 * command and a mailbox word cannot mean two different things. BOTH TRANSITIONS DISCARD
 * THE WHOLE CODE CACHE, and the DISARM direction is the one that needs it: an off period is
 * an unwatched period, so anything compiled before it must not survive into the arm after
 * it. Returns the resolved state.
 *
 * IT ALSO INITIALISES THE ENGINE, which is round 2's fix for round 1's lazy `warp_inited`
 * test on the dispatch path -- four instructions per guest instruction to ask a question
 * whose answer changes once per boot. Arm time is where it belongs. It RETRIES the arena
 * acquisition on every arm rather than latching a first failure, which is round 1's own
 * known gap: an engine that asked for the arena before the classic JIT had released it
 * stayed permanently declined for the rest of the boot. */
#if Z3660_WARP_ABL_NO_ENGINE
/* ABLATION LEVEL 5: z3660_warp.cpp is filtered out of the source list entirely, so the
 * emitter, the block table, the code-cache arena, the self-test and the ARM word executor
 * are not in the image at all. What remains outside that translation unit is the console
 * knob's own once-per-poll bookkeeping, and it gets inline no-ops rather than an #if around
 * each of its four call sites -- a cold empty inline costs nothing, and threading five
 * conditionals through a poll loop to save it would change more than it measured. */
static __inline__ unsigned z3660_warp_arm(unsigned requested) { (void)requested; return 0u; }
static __inline__ int      z3660_warp_armed(void)             { return 0; }
static __inline__ int      z3660_warp_selftest(void)          { return 0; }
static __inline__ unsigned long long z3660_warp_entered(void) { return 0ULL; }
#else
unsigned z3660_warp_arm(unsigned requested);
int      z3660_warp_armed(void);

/* THE ON-TARGET ARMED SELF-TEST (plan 0.10.5). Compiles a corpus of real blocks, publishes
 * them through the real sequence, EXECUTES them on the silicon, and compares each one against
 * the same block INTERPRETED FROM ITS OWN WORDS -- the instruction side against the data side,
 * on the machine that has both. That comparison is the one the host harness cannot make,
 * because the harness compiles warp_publish() out; it is what round 2's four crashes were
 * invisible to; and a publish or coherency defect makes exactly those two disagree.
 *
 * It touches NO GUEST MEMORY -- neither instruction words (the compiler is fed from a
 * core1-resident opcode array) nor operands (the corpus is register-only) -- and it restores
 * regs, the block table, the generation and the bump pointer exactly as it found them, so it
 * is safe to run at any time on a live guest. It never arms anything.
 *
 * Returns 1 if every family passed. THE RULE IT EXISTS FOR: no metal session arms the engine
 * on a live guest until this reports PASS on the image that is about to be armed. */
int      z3660_warp_selftest(void);

/* THE ARM ATTESTATION. Compiled into EVERY variant, unlike the profiler's counters, because
 * the wall-clock arm of every verdict is a LEAN boot with no profiler in it -- and a banner
 * is not admissible, because two cores share one unlocked UART and an arm banner arrives
 * spliced. Non-zero means blocks executed. Same increment site as WARP_BLK (id 48).
 *
 * IT IS A SPLIT 32-BIT PAIR ON THE INCREMENT PATH, not a 64-bit counter: §0.7.3 measured
 * eight ARM instructions for one increment, of the sixty on the entry path. The low word is
 * incremented and the high word only when the low wraps, which is four instructions -- and
 * in round 2 it is paid ONCE PER BLOCK rather than once per guest instruction, which is
 * where "bind the attestation where it costs nothing" actually lands. */
unsigned long long z3660_warp_entered(void);
#endif /* Z3660_WARP_ABL_NO_ENGINE */

/* The out-of-line arms of the run-loop hook. The STEADY-STATE path -- lookup hit, enter,
 * return -- is inlined at the call site below; these are the arms that are allowed to cost
 * something because they do not run per guest instruction. */
unsigned z3660_warp_discover(unsigned pc);          /* miss: compile, then enter          */
unsigned z3660_warp_run(unsigned idx, unsigned way);/* hit:  attest, enter, commit        */

/* The code cache's hook on the dopc's invalidation choke point. Called for EVERY cause, so
 * a new upstream cause has to be opted OUT of invalidating the code cache rather than into
 * it -- the property the choke point exists to provide. */
void z3660_warp_invalidate(int cause);

/* THE CADENCE FEED (Q10, FROZEN). A block feeds the service-cadence counter with the number
 * of guest instructions it RETIRED, so one pump tick stays 256 x service_cadence GUEST
 * instructions and every latency statement in docs/AMIX.md keeps its meaning.
 *
 * It feeds n-1, because the pass's own tail supplies the last one -- so an interpreted pass
 * and a block that retired exactly one instruction advance the counter identically. It
 * polls once per crossing rather than once per block: firing a single poll where
 * interpretation would have fired three would stretch the pump tick silently, and the
 * failure mode of that is a missed interrupt, not a slow number.
 *
 * ROUND 2 CALLS IT ONLY WHEN n > 1, AT THE CALL SITE. Round 1 called it unconditionally
 * with 0 and paid ten ARM instructions -- a call, a prologue, an early-out and a return --
 * to do nothing, which §0.7.3 named as one of the four shapes that were "not the engine".
 * The early-out inside the function stays, because the function is also called directly by
 * the harness. Mutation M-7 is the arm that proves the feed is load-bearing. */
void z3660_warp_cadence_feed(unsigned insns);

/* ---- The engine's own state, exposed because the fast path is inlined ----------------
 *
 * z3660_warp_dispatch() was out of line in round 1, so the hit path paid a full AAPCS
 * prologue and epilogue to look a PC up -- 9 of the 60 instructions §0.7.3 counted. The
 * decoded-op cache's z3660_dopc_find() carries `always_inline` for exactly this reason and
 * its comment says why: at -Os gcc will happily move a hot lookup out of line to save
 * space, "which is the exact trade this rung exists to refuse". Same trade, same refusal.
 *
 * The cost of inlining is that the table and the generation have to be visible here. They
 * are named z3660_* rather than left static, and nothing outside the engine and its harness
 * may write them. */
/* GUARDED, because core0 compiles this header too. debug_console.c and config_file.c want
 * exactly two things from it -- the sentinel values and z3660_warp_resolve() -- and they
 * cannot see `regs`, CYCLE_UNIT or a function-pointer typedef. A translation unit that is
 * part of the emulator core defines Z3660_WARP_ENGINE before including; core0 does not, and
 * the compiler enforces the split rather than a convention doing it. */
#ifdef Z3660_WARP_ENGINE

typedef unsigned (*z3660_warp_fn)(void *regbase, void *rd, void *wr, void *ctx);

/* THE MEMORY ABI GENERATED CODE CALLS. Declared here rather than left file-static so the
 * accessor corpus can be pointed straight at it -- which is what M-VERDICT's owed arm needed
 * (§0.9.6 item 1): the TTR_NO_WRITE asymmetry lives in a read-only TTR window the WARP corpus
 * has no way to build, and the shortest correct wiring is to hand the helper an address from
 * the corpus that does. Return 1 = served (the read leaves its value in ctx.tmp), 0 = declined
 * and the block bails. */
int z3660_warp_rd(unsigned addr, unsigned size, unsigned unused);
int z3660_warp_wr(unsigned addr, unsigned size, unsigned val);

/* One 64-byte set: four ways, one generation stamp, one round-robin victim. Laid out so
 * the four tags are contiguous and land in the first half of the line -- the lookup reads
 * the stamp and the four tags and nothing else. */
struct z3660_warp_set {
   unsigned int  tag[4];
   unsigned int  stamp;
   unsigned int  victim;
   z3660_warp_fn entry[4];
   unsigned short len[4];
   /* Padding to EXACTLY one Cortex-A9 line. 16 + 4 + 4 + 16 + 8 = 48 on a 32-bit target,
    * so sixteen bytes of tail make the set 64 and a four-way lookup touch one line and
    * never two. The static assertion in z3660_warp.cpp is what enforces it -- and it did:
    * the first build refused an eight-byte pad. */
   unsigned char  pad[16];
};

/* THE BLOCK'S SCRATCH CONTEXT, and every field in it is a plain 32-bit word ON PURPOSE.
 *
 * Generated code addresses guest state through exactly two base registers it is HANDED by
 * the caller -- &regs.regs[0] and &this struct -- and never materialises a host address of
 * its own. That is what makes the emitted stream pointer-width-independent, which is what
 * lets the host harness execute the very words the target executes (§0.8.5) instead of
 * asserting them and hoping.
 *
 * The flags live here rather than in regs.ccrflags because `flag_struct` has four different
 * shapes across the machdep arms -- nzcv on ARM, cznv on x86, five separate ints in the
 * portable one. Generated code works in the ARM CPSR layout (which is what the silicon
 * hands it), and the C wrapper packs and unpacks through the architectural GET_/SET_ macros
 * ONCE PER BLOCK. A per-instruction translation would have cost more than the flags do; a
 * direct store into regs.ccrflags would have been correct on the target and wrong in the
 * harness, which is the worst of the three. */
struct z3660_warp_ctx {
   unsigned int pc;     /* guest logical pc: every block exit writes it                   */
   unsigned int nzcv;   /* N,Z,C,V in ARM CPSR bit positions 31..28 -- 68k SENSE for C    */
   unsigned int xflg;   /* the 68k X flag, in bit 0                                       */
   unsigned int tmp;    /* the read helper's out-parameter                                */
   unsigned int ipc;    /* regs.instruction_pc as the last RETIRED instruction left it     */
   unsigned int restart;/* mmu_restart, ditto: 1 = restartable, 0 = past the write-back    */
};

/* THE TWO FIELDS ABOVE ARE THE INTERPRETER'S RESTART BOOKKEEPING, and reproducing them is
 * not optional even though nothing reads them between a block's exit and the next pass.
 *
 * S0.5.5 puts regs.instruction_pc and mmu_restart in the lockstep comparison set, and the
 * differential sweep found the gap immediately: a memory-DESTINATION MOVE commits its
 * flags, advances the pc, sets `regs.instruction_pc = m68k_getpci()` and clears
 * `mmu_restart` BEFORE it stores -- S2e's write-back shape -- and DBcc's two non-branch
 * exits use m68k_setpci(), which moves instruction_pc with the pc. Every other
 * generation-one family leaves both alone.
 *
 * A block could have reproduced them per instruction; it reproduces them PER EXIT instead,
 * because only the LAST retired instruction's values survive to the boundary the oracle
 * compares at. That costs four emitted words at an exit and nothing at all on the path
 * between exits -- and it is why the state-sync numbers in S0.8.4 G-R2.9 are scored against
 * the exit rather than against the faultable site. */

extern struct z3660_warp_set z3660_warp_tab[Z3660_WARP_SETS];
extern struct z3660_warp_ctx z3660_warp_ctx;
extern unsigned int          z3660_warp_gen;

/* THE DISCOVERY GATE, and it is round 2's whole entry-cost story.
 *
 * §0.6.5 named "restricting discovery to control-transfer targets instead of every
 * dispatched PC" as the round-2 lever, and round 1 PRICED it: WARP_BLK / user-mode INSNS
 * came back 0.999878, so the sixty-instruction entry was paid in full for one guest
 * instruction, every time. There was no amortisation to have.
 *
 * A control-transfer TARGET is a pc that is not the sequential successor of the previous
 * dispatched instruction. The engine does not know the previous instruction's length, so
 * the test is `pc - last outside [2, 16]`: an OVER-APPROXIMATION BY CONSTRUCTION -- a
 * forward branch shorter than the longest instruction it could have been reads as a
 * fall-through and its target is not discovered. That is a COVERAGE property and never a
 * correctness one, and it is the reason the test costs four instructions instead of a
 * per-instruction opcode classification that would have cost the DISARMED path something.
 *
 * After a block runs, `last` is set to a value no pc can be the successor of, so the block's
 * exit pc is always a head candidate: it is either a real branch target or the uncovered
 * instruction the block ended at, and the latter gets a cached decline rather than a fresh
 * discovery on every arrival.
 *
 * Fall-through CONTINUES IN THE INTERPRETER until the next target, which is §0.8.1's
 * pre-registered policy stated as code. */
#define Z3660_WARP_NO_LAST  0xFFFFFFFFu

/* THE HOOK, AND IT IS OUT OF LINE AGAIN -- round 3's disarm-penalty change, and the
 * measurement that forces it is on the card right now.
 *
 * Round 2 made this `always_inline` on the decoded-op cache's precedent ("at -Os gcc will
 * happily move a hot lookup out of line to save space, which is the exact trade this rung
 * exists to refuse"). The trade is not the same one, and metal said so: the DOPC's lookup
 * runs on the path that is always live, while this one's body is ARMED-ONLY and the shipped
 * state is DISARMED until the M5 gate. Inlined, the control-transfer test (6 ARM
 * instructions), the supervisor/odd-pc/trace gates (8) and the four-way hashed lookup (18)
 * sat inside m68k_run_mmu040 and were branched over on every disarmed pass -- 32 instructions
 * of L1I footprint doing nothing. m68k_run_mmu040 grew 2 324 -> 2 548 B (+224, G-R2.2, MISSED
 * high) and the measured disarm penalty went from -2.276 % to -5.848 %, which is 2.57x, on a
 * gate whose INSTRUCTION COUNT did not move at all. An instruction count is not a cost, and
 * 0.4.3 named this mechanism before the row existed: "an interpreter dispatch loop in a
 * 32 KB Cortex-A9 L1I is exactly the workload sensitive to that."
 *
 * So the run loop keeps ONE address materialisation, ONE load, ONE test and a perfectly
 * predicted branch, and every byte behind that branch leaves the loop's cache footprint. The
 * price is stated rather than hidden: the ARMED path gains one AAPCS call -- non-target
 * dispatch 11 -> ~15 instructions, per block ~108 -> ~114 -- and that is the correct side to
 * pay on, because the disarmed cost is SHIPPED and the armed cost is not yet a verdict of any
 * kind (plan 0.10.6).
 *
 * NO `super` ARGUMENT, and its absence is still measured rather than stylistic: passing
 * `regs.s ? 1 : 0` at the call site made gcc evaluate it BEFORE the body, so the supervisor
 * byte, its compare and a predicated store were paid on every armed dispatch. It is read
 * inside, after the test that leaves. */
unsigned z3660_warp_step(unsigned pc);

/* One interpreted guest instruction's worth of emulated cycles. EVERY handler in the
 * cpuemu_31 table this loop dispatches returns exactly this constant, so a block that
 * retired n instructions feeds n times it and the cycle accounting is preserved by
 * construction rather than by approximation. Checked in the harness against the value a
 * real handler returns, so a generator change that moved it cannot pass silently. */
#define Z3660_WARP_CYC_PER_INSN  ((1 * 4 * CYCLE_UNIT / 2) * 4)

#endif /* engine-internal view */

#ifdef HOST_TEST_HARNESS
/* ---- Harness only, and in NO firmware binary ---------------------------------------- */

/* Read-backs that count nothing and arm nothing, so a test can ask without perturbing the
 * thing it is asking about. */
int          z3660_warp_peek(unsigned pc, int super);   /* 1 = a live block is cached      */
unsigned     z3660_warp_peek_len(unsigned pc);          /* its compiled length, or 0       */
unsigned     z3660_warp_generation(void);
unsigned     z3660_warp_set_bytes(void);                /* sizeof one table set            */
unsigned long long z3660_warp_stat(int which);

#define Z3660_WARP_STAT_BLK       0   /* block executions (== the attestation)             */
#define Z3660_WARP_STAT_ENTER     1   /* pass-consuming entries                            */
#define Z3660_WARP_STAT_BAIL      2   /* bails to the interpreter                          */
#define Z3660_WARP_STAT_COMPILE   3   /* blocks compiled                                   */
#define Z3660_WARP_STAT_CBYTES    4   /* ARM code bytes emitted                            */
#define Z3660_WARP_STAT_DISC      5   /* blocks discarded by invalidation, all causes      */
#define Z3660_WARP_STAT_DISCROOT  6   /* ... of those, at IV_ROOT                          */
#define Z3660_WARP_STAT_FLUSH     7   /* full code-cache flushes                           */
#define Z3660_WARP_STAT_DECLINE   8   /* block-head PCs declined                           */
#define Z3660_WARP_STAT_PUBLISH   9   /* icache publish sequences run                      */
#define Z3660_WARP_STAT_LIVE     10   /* blocks currently live in the table                */
#define Z3660_WARP_STAT_EVICT    11   /* live blocks overwritten by a conflicting fill     */
#define Z3660_WARP_STAT_INSNS    12   /* guest instructions retired inside blocks          */
#define Z3660_WARP_STAT_CINSNS   13   /* guest instructions compiled                       */
#define Z3660_WARP_STAT_BAILST   14   /* writeback stores executed on bail arms            */
#define Z3660_WARP_STAT_MEMHIT   15   /* memory helper served                              */
#define Z3660_WARP_STAT_MEMDECL  16   /* memory helper declined -> the block bailed        */
#define Z3660_WARP_STAT_DISCEV   17   /* discard-all EVENTS -- round 4's churn split       */
#define Z3660_WARP_STAT_DISCATC  18   /* ... blocks lost at PFLUSH / PFLUSHA               */
#define Z3660_WARP_STAT_DISCCPUSH 19  /* ... blocks lost at CPUSHL / CINV                  */
/* Round 5's bail attribution (§0.14.2), mirroring prof ids 64..68 so the harness can assert
 * the two identities before any card is booked:
 *     BAIL      == BAILSTUB + BAILMEM + BAILOTHER
 *     BAILMEM   == BAILFENCE + BAILVERD
 *     BAILOTHER == 0 */
#define Z3660_WARP_STAT_BAILSTUB  20  /* a cached decline stub executed                    */
#define Z3660_WARP_STAT_BAILMEM   21  /* a real block bailed -- a memory-helper decline    */
#define Z3660_WARP_STAT_BAILFENCE 22  /* ... at the align / page-cross fence               */
#define Z3660_WARP_STAT_BAILVERD  23  /* ... at the verdict table                          */
#define Z3660_WARP_STAT_BAILOTHER 24  /* neither. Pre-registered at zero                   */
#define Z3660_WARP_STAT_COUNT    25

void z3660_warp_stats_zero(void);
/* Lower the code cache's usable size so the eviction path can be reached without emitting
 * a hundred thousand blocks. 0 restores the arena's full size. */
void z3660_warp_set_cache_limit(unsigned bytes);

/* THE MUTATION ARMS. Every one of them is a defect this round's corpus claims to catch, and
 * a claim like that is worth exactly as much as the test that fails when it stops being
 * true. Rung 1b's attribution test once passed a mutation that bypassed the whole
 * attribution, which is how this lane learned to write the arm rather than assume it. */
#define Z3660_WARP_MUT_NONE       0
#define Z3660_WARP_MUT_KEY_NOSUP  1  /* M-5:  the block key drops the supervisor bit       */
#define Z3660_WARP_MUT_NO_ICACHE  2  /* M-ICACHE: publish without the sequence             */
#define Z3660_WARP_MUT_CONFLICT   3  /* M-CONFLICT: round 1's lookup geometry restored     */
#define Z3660_WARP_MUT_XFER       4  /* M-XFER: the discovery gate fires at every PC --
                                      * round 1's policy restored. It lived above the
                                      * mutation list while step() was inlined into the run
                                      * loop; round 3 took step() out of line, so it comes
                                      * home to the list with the rest.                    */
#define Z3660_WARP_MUT_FENCE      5  /* M-FENCE: the helper's align/page-cross fence gone  */
#define Z3660_WARP_MUT_VERDICT    6  /* M-VERDICT: the verdict decline arm made permissive */
#define Z3660_WARP_MUT_BAILPC     7  /* M-3:  the bail stores the block-head pc, not the
                                      *       per-site one -- state-sync omitted           */
#define Z3660_WARP_MUT_NOFIXUP    8  /* M-4:  the mmufixup invariant not asserted          */
#define Z3660_WARP_MUT_BAILX      9  /* M-8:  the bail commits nzcv but not X              */
#define Z3660_WARP_MUT_FLAG_MOVE 10  /* wrong-flag, MOVE family: C/V not cleared           */
#define Z3660_WARP_MUT_FLAG_ADD  11  /* wrong-flag, ADD family: X not copied from C        */
#define Z3660_WARP_MUT_FLAG_SUB  12  /* wrong-flag, SUB/CMP: the carry inversion dropped   */
#define Z3660_WARP_MUT_FLAG_BCC  13  /* wrong-flag, Bcc: the condition sense inverted      */
#define Z3660_WARP_MUT_FLAG_DBCC 14  /* wrong-flag, DBcc: the counter tested after, not
                                      *       before, the decrement                        */
#define Z3660_WARP_MUT_PUBRANGE  15  /* M-PUBRANGE: round 2's publish range restored --
                                      * the raw (dst, bytes) call AND the unaligned bump,
                                      * i.e. THE DEFECT THAT KILLED ROUND 2 ON SILICON,
                                      * put back so the arm that catches it can be shown
                                      * to catch it rather than asserted to               */
/* ROUND 4'S ARMS -- one per new family and one per new DECISION, which is the half that
 * matters. A family arm proves the emitter computes the right flags; a decision arm proves
 * the round's reasoning was load-bearing. M-AUTO-ORDER is the second kind: it puts the
 * auto-adjust write-back BEFORE the access, which is precisely the arrangement §0.12.2's
 * static read exists to forbid, so the corpus has to show that a bail then leaves an
 * already-advanced An behind. */
#define Z3660_WARP_MUT_MOVEQ_FLAG   16  /* MOVEQ keeps C and V instead of clearing them   */
#define Z3660_WARP_MUT_TST_FLAG     17  /* TST commits C and V                            */
#define Z3660_WARP_MUT_SHIFT_C      18  /* a shift drops its carry-out                    */
#define Z3660_WARP_MUT_SHIFT_X      19  /* a ROTATE writes X, which only ROXd may do      */
#define Z3660_WARP_MUT_IMM_CARRY    20  /* SUBI/CMPI lose the carry inversion             */
#define Z3660_WARP_MUT_ALUMEM_EARLY 21  /* a memory-source ALU computes from a value read
                                         * before the source could decline                */
#define Z3660_WARP_MUT_AUTO_BYTEINC 22  /* -(A7)/(A7)+ at byte size steps by 1, not 2     */
#define Z3660_WARP_MUT_AUTO_ORDER   23  /* the auto-adjust write-back emitted BEFORE the
                                         * access -- the defect §0.12.2 forbids           */
/* M1a TRANCHE 2's arms (§0.14.3). One per new family and one per new DECISION, which is the
 * standing rule -- a family arm proves the emitter was exercised, a decision arm proves the
 * decision was load-bearing. */
#define Z3660_WARP_MUT_SHIFTREG_ZERO   24  /* the count==0 tail dropped, so a zero count
                                            * keeps the shift's own C instead of clearing it
                                            * and takes N/Z at the wrong width             */
#define Z3660_WARP_MUT_SHIFTREG_MOD    25  /* the count masked with 31 instead of 63 -- the
                                            * 68k takes the count register MOD 64, and this
                                            * makes a count of 32 behave as a count of 0   */
#define Z3660_WARP_MUT_SHIFTREG_XZERO  26  /* X written even on a zero count, which is the
                                            * one thing a zero count must not do           */
#define Z3660_WARP_MUT_ASLREG_CLAMP    27  /* ASL's variable-width overflow test loses its
                                            * clamp at 31, so every count above 31 reads an
                                            * ARM shift by a byte value it never meant     */
#define Z3660_WARP_MUT_LEA_PCBASE      28  /* LEA d16(PC) bases on pc, not pc+2 -- two bytes
                                            * wrong on a family that sets no flag at all   */
#define Z3660_WARP_MUT_CLR_FLAG        29  /* CLR leaves Z clear instead of setting it     */
#define Z3660_WARP_MUT_EXT_WIDTH       30  /* EXT.W writes the WHOLE register instead of its
                                            * low half. Its FLAG width is deliberately not
                                            * the arm: on a sign-extended byte, bit 15 and
                                            * bit 31 are the same bit, so reading the flags
                                            * at the wrong width is unobservable for every
                                            * operand that exists -- an arm for it could
                                            * only report CAUGHT-shaped silence            */
#define Z3660_WARP_MUT_COUNT        31
void z3660_warp_set_mutant(int which);
/* The mutant word itself, because z3660_warp_step() is INLINED into the run loop and
 * M-XFER lives inside it. In no firmware binary: the whole block is behind
 * HOST_TEST_HARNESS. */
extern int z3660_warp_mutant;

/* THE INVARIANT VIOLATIONS, counted rather than asserted so that a corpus can show them
 * going from zero to non-zero under a mutation and back. They are correctness failures and
 * not statistics: any non-zero value is a test failure. */
#define Z3660_WARP_BAD_UNPUBLISHED 0  /* a block entered without the I-cache sequence       */
#define Z3660_WARP_BAD_RETIRED     1  /* a block retired more than it compiled              */
#define Z3660_WARP_BAD_FIXUP       2  /* mmufixup armed at a block head -- only the
                                       * interpreter may ever arm it (§0.3(b))              */
#define Z3660_WARP_BAD_SUPER       3  /* a block entered with regs.s set                    */
/* ROUND 3'S INVARIANT, and the one the round-2 harness had no way to hold. Every publish is
 * checked against z3660_warp_publish_covers(): did the CP15 line loop, called with the
 * arguments WARP actually passes, reach every byte of the block? Round 2's arguments do not,
 * for 43.75 % of blocks, and this counter is what says so on a host that has no cache at all.
 * M-PUBRANGE restores those arguments and drives it non-zero. */
#define Z3660_WARP_BAD_PUBRANGE    4  /* a block published with a range that misses part of
                                       * it -- the round-2 killer, as an assertion          */
#define Z3660_WARP_BAD_UNALIGNED   5  /* a block that does not start on a cache line         */
unsigned long long z3660_warp_bad(int which);

/* MUTATION M-7 and the counter it lies about, both in newcpu.cpp where the cadence lives.
 * Armed, the feed is given 1 per block instead of the retired count -- Q10's contract
 * inverted. z3660_warp_serv_ctr() is the 040 loop's service counter, so a test can watch
 * the feed advance it rather than infer that it did. */
void z3660_warp_set_cadence_mutant(int on);
int  z3660_warp_serv_ctr(void);

/* THE EMITTED WORDS, so a test can decode and assert the stream the target will execute.
 * Returns the byte count of the block cached at pc, and copies at most `max` bytes out. */
unsigned z3660_warp_block_code(unsigned pc, void *dst, unsigned max);

/* Compile one instruction stream in isolation and hand back what the compiler decided --
 * the length in guest instructions and the emitted byte count -- without entering it. */
int z3660_warp_probe_compile(unsigned pc, unsigned *insns, unsigned *bytes);
#endif /* HOST_TEST_HARNESS */

#ifdef Z3660_WARP_LOCKSTEP
/* ---- The lockstep oracle, gated build only ------------------------------------------
 *
 * Compiled out of every shipping image. It runs the SAME guest span twice -- once with the
 * engine disarmed, once armed -- and compares, after every guest instruction, the whole set
 * §3.1 defines: the sixteen data and address registers, the guest pc, the flags word and X,
 * regs.instruction_pc, mmu_restart and mmufixup[0..1].
 *
 * ROUND 2 IS THE ROUND WHERE THIS STOPS BEING FREE. With null blocks the two arms were
 * IDENTICAL because a null block changed nothing; with real code generation the armed arm
 * genuinely computes, and every divergence is a real defect in an emitter. A single
 * divergence is a stop, not a statistic. */
unsigned long long z3660_warp_ls_insns(void);
unsigned long long z3660_warp_diverge(void);
void               z3660_warp_ls_zero(void);
/* Fold one instruction boundary's comparison set into the running digest. */
void               z3660_warp_ls_step(void);
/* Compare the digest stream of the arm just run against the one before it. Returns the
 * number of boundaries at which they differ. */
unsigned long long z3660_warp_ls_commit(int arm);
#endif /* Z3660_WARP_LOCKSTEP */

#ifdef __cplusplus
}
#endif

#endif /* SRC_UAE_Z3660_WARP_H_ */
