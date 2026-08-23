/* WARP ENGINE -- the block engine's contract: the switch rule, the block key, the
 * geometry, and the invariant that decides what every counter in it means.
 *
 * docs/WARP-ENGINE.md is the plan; §0.5 is the M1 pre-registration and §0.6 is round 1's,
 * both committed before the code they describe. Read §0.6 before changing anything here.
 *
 * WHAT THIS IS. A JIT for the guest 68040 while its MMU is on: guest basic blocks are
 * discovered at the interpreter's dispatch site, compiled to ARM, cached, and executed in
 * place of the interpreter -- with the interpreter kept permanently as both the fallback
 * path and the differential oracle.
 *
 * WHAT ROUND 1 IS. This round generates NO CODE. Every block is a NULL BLOCK: a generated
 * stub that syncs nothing, retires nothing and returns to the interpreter at the PC it was
 * entered at. The point is to prove the plumbing -- switch, counters, block cache,
 * invalidation, eviction, cadence feed, bail accounting, lockstep -- BEFORE there is a
 * compiler whose numbers could distract from it. Coverage is 0 by construction, so no
 * performance claim is admissible from this round and the M1 kill line cannot fire on it.
 *
 * ------------------------------------------------------------------------------------
 * THE NO-EMPTY-PASS INVARIANT, and it is the reason the counters mean what they say
 * ------------------------------------------------------------------------------------
 *
 *   A block entry either RETIRES >= 1 guest instruction and CONSUMES the run-loop pass,
 *   or RETIRES 0 and FALLS THROUGH to the interpreter INSIDE THE SAME PASS.
 *   There is no third outcome, and in particular no pass that retires nothing.
 *
 * Without it a null block at every dispatched PC would burn one whole run-loop pass per
 * guest instruction: INSNS would double, the engine would need a re-entry guard to avoid
 * looping forever at a bailed PC, and the wall clock would halve. With it, progress is
 * guaranteed by construction -- a pass always retires at least one instruction, whoever
 * retires it -- and no guard is needed at all.
 *
 * WHAT IT DECIDES ABOUT THE COUNTERS. WARP_ENTER (prof id 47) is pre-registered as "the
 * loop hook -- the site that costs exactly one INSNS", with the standing warning that
 * incrementing it anywhere that does NOT cost an INSNS over-subtracts and reports coverage
 * high. A fall-through entry costs no INSNS of its own; the interpreted instruction in the
 * same pass costs it. So:
 *
 *   WARP_ENTER (47)  counts PASS-CONSUMING entries only.  In round 1 it is 0, always.
 *   WARP_BLK   (48)  counts EVERY block execution, null ones included.
 *
 * and the arm attestation -- z3660_warp_entered(), compiled into every variant because a
 * lean verdict boot has no profiler to read -- is the SAME INCREMENT SITE as id 48. One
 * site, not two, exactly as the pre-registration requires; the id it is bound to moved from
 * 47 to 48 because a null block does not cost an INSNS. Binding it to 47 would have made a
 * lean attestation read zero on an armed boot, which is the one thing an attestation may
 * not do.
 *
 * The identity that falls out, and it holds EXACTLY rather than approximately:
 *
 *     DOPC_HIT + DOPC_MISS  ==  (INSNS - WARP_ENTER) + FAULTS
 *
 * With the engine off WARP_ENTER is 0 and this is the identity the firmware has always had.
 * In round 1 WARP_ENTER is 0 ARMED as well, so the identity is the same in both arms --
 * which is what makes an armed capture readable with tools that predate the engine.
 *
 * ------------------------------------------------------------------------------------
 * THE RULE LIVES IN A HEADER
 * ------------------------------------------------------------------------------------
 *
 * Same reason amix_ram.h, overhead_switches.h, z3660_blk.h, z3660_dopc.h and z3660_dfast.h
 * give: core0's serial console and core1's knob resync must not be able to disagree about
 * what a mailbox word means, so the mapping lives here and nowhere else -- and the host
 * harness compiles it directly and asserts every branch, including the ones no metal
 * session will ever exercise. Plain C and plain `unsigned` throughout, because core0's
 * debug console and config parser both include it.
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
 * Direct-mapped, one block per set, in round 1. The dopc needed four ways because it is
 * consulted once per INSTRUCTION; a block table is consulted once per BLOCK HEAD, and a
 * way count is a decision to take against a measured head distribution rather than by
 * analogy. Associativity is a round-2 question with WARP_DISC and the hit rate to answer
 * it.
 *
 * THE EMPTY MARKER IS SET-RELATIVE, and it is inherited whole because the dopc paid for it
 * with a crash. The obvious sentinel is a fixed odd constant -- 1, say, on the argument
 * that a key is an even pc so an odd one is impossible. It is not impossible: bit 0 is the
 * supervisor bit, so key 1 is "pc 0 in supervisor mode", and a guest reaches exactly that
 * by taking an exception through an unplanted vector. Built from the set index instead,
 * the marker cannot equal any key that could legitimately live in this set, for any pc and
 * either address space -- a property of the encoding rather than an argument about which
 * addresses a guest is likely to reach. Mutation M-5 (the key drops the supervisor bit) is
 * the arm that keeps this honest. */
#define Z3660_WARP_SETS           1024u
#define Z3660_WARP_INDEX(pc)      (((unsigned)(pc) >> 1) & (Z3660_WARP_SETS - 1u))
#define Z3660_WARP_KEY(pc, super) (((unsigned)(pc) & ~1u) | ((super) ? 1u : 0u))
#define Z3660_WARP_TAG_EMPTY(idx) ((((unsigned)(idx)) ^ 1u) << 1)

/* ---- The generated null block -------------------------------------------------------
 *
 * Two ARM words, hand-encoded, no assembler involved:
 *
 *     e3a00000   mov r0, #0    the retired-instruction count: a null block retires none
 *     e12fff1e   bx  lr        back to the interpreter, at the PC the block was entered at
 *
 * It syncs nothing because nothing is dirty: no guest register was ever moved into a host
 * one. That is the whole bail contract in its smallest true form, and it is what makes the
 * armed and disarmed arms IDENTICAL rather than merely equivalent -- the round's own
 * correctness gate.
 *
 * The words are emitted by portable C, so the HOST harness produces the same two words and
 * asserts them byte-exactly even though it can execute neither. §3.2's honest limit stands:
 * generated code is proven on target and only on target. */
#define Z3660_WARP_NULL_WORD0  0xE3A00000u   /* mov r0, #0 */
#define Z3660_WARP_NULL_WORD1  0xE12FFF1Eu   /* bx  lr     */
#define Z3660_WARP_NULL_BYTES  8u

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
#define Z3660_WARP_DECL_COUNT   3

#ifdef __cplusplus
extern "C" {
#endif

/* THE ARM FLAG, read by the run loop's hook on every dispatch.
 *
 * A plain int and not a function call: the hook is on the per-instruction path and its
 * DISARMED cost is a shipped cost until M5 -- pre-registered at 3..6 ARM instructions and
 * measured off the lean objdump, never estimated. */
extern int z3660_warp_on;

/* Arm/disarm from a RAW mailbox value, run through z3660_warp_resolve() so a console
 * command and a mailbox word cannot mean two different things. BOTH TRANSITIONS DISCARD
 * THE WHOLE CODE CACHE, and the DISARM direction is the one that needs it: an off period is
 * an unwatched period, so anything compiled before it must not survive into the arm after
 * it. Returns the resolved state. */
unsigned z3660_warp_arm(unsigned requested);
int      z3660_warp_armed(void);

/* THE ARM ATTESTATION. Compiled into EVERY variant, unlike the profiler's counters, because
 * the wall-clock arm of every verdict is a LEAN boot with no profiler in it -- and a banner
 * is not admissible, because two cores share one unlocked UART and an arm banner arrives
 * spliced. Non-zero means blocks executed. Same increment site as WARP_BLK (id 48). */
unsigned long long z3660_warp_entered(void);

/* THE RUN-LOOP HOOK. Discovers, compiles and executes the block at (pc, super); returns the
 * number of guest instructions it retired.
 *
 * ROUND 1 RETURNS 0, ALWAYS, and that is enforced at the fill site rather than hoped for:
 * the compiler refuses to publish a block whose retired count is non-zero, because the
 * run loop has no pass-consuming arm yet. The arm and the refusal land together in round 2.
 */
unsigned z3660_warp_dispatch(unsigned pc, int super);

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
 * Implemented in newcpu.cpp, where the counter, the cadence and the poll already live.
 * Round 1 always calls it with 0. Mutation M-7 is the arm that proves it is load-bearing. */
void z3660_warp_cadence_feed(unsigned insns);

#ifdef HOST_TEST_HARNESS
/* ---- Harness only, and in NO firmware binary ---------------------------------------- */

/* Read-backs that count nothing and arm nothing, so a test can ask without perturbing the
 * thing it is asking about. */
int          z3660_warp_peek(unsigned pc, int super);   /* 1 = a live block is cached      */
unsigned     z3660_warp_generation(void);
unsigned     z3660_warp_block_bytes(void);              /* sizeof one table entry          */
unsigned long long z3660_warp_stat(int which);

#define Z3660_WARP_STAT_BLK       0   /* block executions (== the attestation)             */
#define Z3660_WARP_STAT_ENTER     1   /* pass-consuming entries -- 0 in round 1            */
#define Z3660_WARP_STAT_BAIL      2   /* bails to the interpreter                          */
#define Z3660_WARP_STAT_COMPILE   3   /* blocks compiled                                   */
#define Z3660_WARP_STAT_CBYTES    4   /* ARM code bytes emitted                            */
#define Z3660_WARP_STAT_DISC      5   /* blocks discarded by invalidation, all causes      */
#define Z3660_WARP_STAT_DISCROOT  6   /* ... of those, at IV_ROOT                          */
#define Z3660_WARP_STAT_FLUSH     7   /* full code-cache flushes                           */
#define Z3660_WARP_STAT_DECLINE   8   /* block-head PCs declined                           */
#define Z3660_WARP_STAT_PUBLISH   9   /* icache publish sequences run                      */
#define Z3660_WARP_STAT_LIVE     10   /* blocks currently live in the table                */
#define Z3660_WARP_STAT_COUNT    11

void z3660_warp_stats_zero(void);
/* Lower the code cache's usable size so the eviction path can be reached without emitting
 * four million null blocks. 0 restores the arena's full size. */
void z3660_warp_set_cache_limit(unsigned bytes);
/* The emitted words, for the byte-exact assertion the host CAN make about code it cannot
 * execute. Returns the byte count written. */
unsigned z3660_warp_emit_null(void *dst);

/* THE MUTATION ARMS. Every one of them is a defect this round's corpus claims to catch, and
 * a claim like that is worth exactly as much as the test that fails when it stops being
 * true. Rung 1b's attribution test once passed a mutation that bypassed the whole
 * attribution, which is how this lane learned to write the arm rather than assume it. */
#define Z3660_WARP_MUT_NONE      0
#define Z3660_WARP_MUT_KEY_NOSUP 1   /* M-5: the block key drops the supervisor bit        */
#define Z3660_WARP_MUT_NO_ICACHE 2   /* M-ICACHE: publish a null block without the sequence */
void z3660_warp_set_mutant(int which);

/* THE TWO INVARIANT VIOLATIONS, counted rather than asserted so that a corpus can show them
 * going from zero to non-zero under a mutation and back. They are correctness failures and
 * not statistics: any non-zero value is a test failure. */
#define Z3660_WARP_BAD_UNPUBLISHED 0  /* a block entered without the I-cache sequence       */
#define Z3660_WARP_BAD_RETIRED     1  /* a block retired something in a round with no
                                       * pass-consuming arm (the no-empty-pass invariant)   */
unsigned long long z3660_warp_bad(int which);

/* MUTATION M-7 and the counter it lies about, both in newcpu.cpp where the cadence lives.
 * Armed, the feed is given 1 per block instead of the retired count -- Q10's contract
 * inverted. z3660_warp_serv_ctr() is the 040 loop's service counter, so a test can watch
 * the feed advance it rather than infer that it did. */
void z3660_warp_set_cadence_mutant(int on);
int  z3660_warp_serv_ctr(void);
#endif /* HOST_TEST_HARNESS */

#ifdef Z3660_WARP_LOCKSTEP
/* ---- The lockstep oracle, gated build only ------------------------------------------
 *
 * Compiled out of every shipping image. It runs the SAME guest span twice -- once with the
 * engine disarmed, once armed -- and compares, after every guest instruction, the whole set
 * §3.1 defines: the sixteen data and address registers, the guest pc, the flags word and X,
 * regs.instruction_pc, mmu_restart and mmufixup[0..1]. With null blocks the two arms must
 * be IDENTICAL, and that identity is round 1's correctness gate.
 *
 * WARP_LS_INSNS and WARP_DIVERGE belong to this build and are not part of the shipping
 * counter block. A single divergence is a stop, not a statistic. */
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
