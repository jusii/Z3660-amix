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
struct z3660_warp_gate_s {
   int      on;         /* the arm flag the run loop's hook tests                       */
   unsigned last_pc;    /* the previously dispatched pc -- the control-transfer test     */
};
extern struct z3660_warp_gate_s z3660_warp_gate;
#define z3660_warp_on       (z3660_warp_gate.on)
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
unsigned z3660_warp_arm(unsigned requested);
int      z3660_warp_armed(void);

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

#ifdef HOST_TEST_HARNESS
/* Declared here as well as in the harness block below, because z3660_warp_step() is
 * inlined ABOVE that block and M-XFER lives inside it. In no firmware binary. */
extern int z3660_warp_mutant;
#define Z3660_WARP_MUT_XFER 4
#endif

/* NO `super` ARGUMENT, and its absence is measured rather than stylistic: passing
 * `regs.s ? 1 : 0` at the call site made gcc evaluate it BEFORE the inlined body, so the
 * supervisor byte, its compare and a predicated store all landed above the
 * control-transfer test and were paid on every armed dispatch. Read inside, after the test
 * that leaves, they are paid only when a block might actually be entered -- four ARM
 * instructions off the armed non-target path, off the lean objdump. */
static __inline__ __attribute__((always_inline))
unsigned z3660_warp_step(unsigned pc)
{
   unsigned d = pc - z3660_warp_last_pc;
   struct z3660_warp_set *s;
   unsigned key, i;

   z3660_warp_last_pc = pc;

   /* THE CHEAPEST TEST FIRST, and the ordering is the difference between ten emitted
    * instructions on the common path and eighteen. The overwhelmingly common case on an
    * armed dispatch is "not a control-transfer target", so it leaves before the supervisor
    * byte, the odd-pc test and the trace pair are ever loaded. M-XFER removes the test,
    * which restores round 1's "discover at every dispatched PC" policy -- not a correctness
    * defect, which is exactly why it needs its own arm. */
#ifdef HOST_TEST_HARNESS
   if (z3660_warp_mutant != Z3660_WARP_MUT_XFER)
#endif
   if (d - 2u < 15u)
      return 0;

   /* GENERATION ONE IS USER-MODE ONLY: a block is never entered with regs.s set. Gated
    * before discovery, and deliberately NOT counted as a decline -- one decline per
    * supervisor instruction would make WARP_DECLINE a second copy of INSNS_SUPER. An odd
    * pc cannot be a block head either; nothing can execute at one on a 68040. */
   if (regs.s || (pc & 1u))
      return 0;

   /* TRACE ON IMPLIES INTERPRET. One predicate that removes an entire class of divergence:
    * do_specialties() stays per-instruction in the interpreter at every cadence, and a
    * block that retired several instructions under T0/T1 would owe several trace exceptions
    * it has no way to raise. It is tested HERE and not only at compile time, because a
    * cached block must not be entered while tracing either. */
   if (regs.t0 || regs.t1)
      return 0;

   s   = &z3660_warp_tab[Z3660_WARP_INDEX(pc)];
   key = Z3660_WARP_KEY(pc, 0);
   if (s->stamp == z3660_warp_gen) {
      for (i = 0; i < Z3660_WARP_WAYS; i++)
         if (s->tag[i] == key)
            return z3660_warp_run((unsigned)(s - z3660_warp_tab), i);
   }
   return z3660_warp_discover(pc);
}

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
#define Z3660_WARP_STAT_COUNT    17

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
/* Z3660_WARP_MUT_XFER (4) is defined above, beside the inlined step() that carries it. */
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
#define Z3660_WARP_MUT_COUNT     15
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
