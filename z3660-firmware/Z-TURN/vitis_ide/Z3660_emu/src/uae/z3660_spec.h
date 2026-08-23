/* z3660_spec.h -- CYBERSTORM C2: the HANDLER discriminator.
 *
 * ==========================================================================
 * WHAT THIS IS, AND WHAT IT IS NOT
 * ==========================================================================
 *
 * It is NOT a handler-specialisation design. It is the cheap §30.3 experiment whose
 * measured sign decides whether that design is ever written -- see
 * docs/handler-discriminator.md, which carries the pre-registered bands and must not be
 * edited after the metal row is taken.
 *
 * C2 attack map v3.2 §33.3 note 1 put `HANDLER` at the top of the ladder BY ELIMINATION:
 *
 *     "HANDLER leads because the bucket above it was measured hidden and the buckets
 *      below it are small or empty -- not because anything says its work is recoverable.
 *      ... Its first move is not a design; it is the §30.3 discriminator on one handler
 *      family."
 *
 * and §34.2 says why the classification cannot simply be reasoned out:
 *
 *     "Two rungs, opposite signs, one week, one rig. That is enough to refute
 *      'instruction counts predict wall clock' and enough to name the mechanism, and it
 *      is NOT enough to predict a third rung's sign from armchair reasoning."
 *
 * ==========================================================================
 * THE FAMILY, AND WHY IT IS THIS ONE
 * ==========================================================================
 *
 * MOVE / MOVEA (opcode lines 1, 2 and 3) is the largest single family in the workload by
 * a factor of two. Time-weighted from the 2026-08-22 `DHRY` sampler rings -- the sampler
 * fires at the run-loop tail, so a record's probability is proportional to that
 * dispatch's elapsed time, which makes a ring dump a share-of-runtime histogram:
 *
 *     family                       09-DHRY-BLKON     05-DHRY-ring-full
 *     MOVE / MOVEA (lines 1,2,3)      31.51 %            27.50 %
 *     Bcc / BRA / BSR                 13.93 %            12.57 %
 *     ADDQ / SUBQ                      6.96 %            11.03 %
 *
 * Restricting BOTH effective addresses to the six simple modes -- Dn, An, (An), (An)+,
 * -(An), d16(An) -- keeps the specialised body to two six-arm switches and still covers
 * 83.7 % / 83.3 % of the family, i.e. 26.36 % / 22.91 % of dispatch time. Everything
 * else falls through to the unmodified dispatch, byte for byte.
 *
 * THE MIX IS A PROPERTY OF THE WINDOW, NOT A CONSTANT -- finding F5's standing warning.
 * On a kernel-dominated ring the same restriction covers only 57.6 %, because
 * `(xxx).L -> (xxx).L` alone is 37 % of the family there. Read `SPEC_HIT / INSNS` off the
 * capture being interpreted; do not re-pin 26 %.
 *
 * ==========================================================================
 * THE MECHANISM: ONE BIT IN THE DECODED-OP ENTRY, NO NEW TABLE
 * ==========================================================================
 *
 * Z3660_DOPC_M_SPEC joins M_IDIOM and M_EXT0 in the entry's existing `meta` byte, which
 * the hit path has already loaded into a register and already tests twice. So the added
 * work on a dispatch that is NOT specialised is two ARM instructions -- a `tst` and a
 * not-taken branch -- and nothing else anywhere. That is rung 0's idiom-verdict field
 * pattern, reused for the same reason: a static verdict computed once at fill time so
 * the hot path pays a bit test instead of a decision.
 *
 * THE BIT IS ONLY WRITTEN WHILE ARMED, so z3660_spec_set()/z3660_spec_arm() INVALIDATE
 * THE DECODED-OP CACHE on a change, exactly as z3660_dopc_set() does: an entry filled in
 * one state must not be believed in the other. DFAST does not need this and this does --
 * DFAST's switch changes what a table MEANS, this one changes what an entry SAYS.
 *
 * WHAT THE SPECIALISED BODY REMOVES, per §30.3 step 1, and the latency each site sits
 * next to:
 *
 *   A  the indirect call+return through handler[way], plus the handler's own frame
 *         -> next to the guest PC dependency of the next dispatch. Nothing hides it.
 *   B  the opcode field re-extraction the run loop already performed
 *         -> the same chain, one step earlier.
 *   C  the `bl uae_mmu040_get_iword` for a d16(An) displacement, which on a hit is
 *      answered from z3660_dopc_ext_val without touching guest DRAM at all
 *         -> next to NOTHING. This is the site that decides the classification.
 *   D  the accessor's call frame, if gcc inlines it here
 *         -> next to the guest-DRAM access itself, which is where rung 3's answer
 *            plausibly repeats.
 *
 * THE MEMORY ACCESS IS THE EXISTING ACCESSOR. The body calls get_/put_*_mmu040(), the
 * same static header functions every generated handler calls -- so rung 2's fast path,
 * the fault semantics, the write-back contract, the M-bit re-walk, the device decline
 * and the misalignment split are NOT reimplemented and therefore cannot drift. The whole
 * safety argument of rungs 2 and 3 is kept: the fallback is the original code.
 *
 * ONE ATTRIBUTION CHANGE, DECLARED IN ADVANCE. A served d16(An) displacement no longer
 * calls the fetch accessor, so FETCH / IFETCH_CALLS and the FETCHEX bucket FALL. Rung 3
 * was careful that "the bucket gets cheaper; it does not get emptier"; this rung
 * deliberately does the opposite at that one site. IPAGE_HIT / IPAGE_MISS are unaffected
 * -- a window hit returns before the instruction-page cache is consulted, in both arms.
 */

#ifndef Z3660_SPEC_H
#define Z3660_SPEC_H

/* ---- The covered shapes ------------------------------------------------------------
 *
 * A pure function of the opcode word, so it can be evaluated at fill time, asserted by
 * the harness, and compiled by core0's console (which has no emulator types at all).
 *
 * Excluded on purpose, and each exclusion is one line rather than a special case inside
 * the body:
 *   - any EA mode above 5: (d8,An,Xn) needs the brief-extension decoder, and the mode-7
 *     forms need a second extension word the decoded-op entry does not hold.
 *   - MOVE.B with An as either operand: that is not a MOVE at all.
 *   - MOVE.B through (A7)+ or -(A7): the stack-alignment special case (areg_byteinc) is
 *     the classic trap in this family, so the specialised body never enters it. */
#define Z3660_SPEC_SIZE_B  1u
#define Z3660_SPEC_SIZE_L  2u
#define Z3660_SPEC_SIZE_W  3u

static __inline__ int z3660_spec_covered(unsigned op)
{
   unsigned line = (op >> 12) & 0xFu;
   unsigned dstreg, dstmode, srcmode, srcreg;

   if (line != Z3660_SPEC_SIZE_B && line != Z3660_SPEC_SIZE_L && line != Z3660_SPEC_SIZE_W)
      return 0;

   dstreg  = (op >> 9) & 7u;
   dstmode = (op >> 6) & 7u;
   srcmode = (op >> 3) & 7u;
   srcreg  =  op       & 7u;

   if (dstmode > 5u || srcmode > 5u) return 0;

   if (line == Z3660_SPEC_SIZE_B) {
      if (dstmode == 1u || srcmode == 1u) return 0;
      if ((srcmode == 3u || srcmode == 4u) && srcreg == 7u) return 0;
      if ((dstmode == 3u || dstmode == 4u) && dstreg == 7u) return 0;
   }
   return 1;
}

/* ---- The engagement signal, and why it is not only the profiler's counter -----------
 *
 * Same reasoning as z3660_dfast.h's: "the two paths agree" is trivially true when the
 * specialised body never fired, so every corpus case has to declare whether it MUST or
 * MUST NOT serve, and something has to check it -- in the LEAN build too, which is the
 * one whose correctness ships. SPEC_HIT exists only in the profiling build; this counter
 * exists in all three harness variants and in no firmware binary. */
#ifdef HOST_TEST_HARNESS
#ifdef __cplusplus
extern "C" unsigned long z3660_spec_serves;
#else
extern unsigned long z3660_spec_serves;
#endif
#define Z3660_SPEC_SERVED() (z3660_spec_serves++)
#ifdef __cplusplus
extern "C" {
#endif
/* The specialised body, callable directly, so the sweep compares it against
 * cpufunctbl[op] rather than against a second run-loop pass -- and the mutation arm that
 * proves the sweep is a guard. Neither is in any firmware binary. */
int  z3660_spec_move_probe(unsigned short op, unsigned short ext0, int have_ext0);
void z3660_spec_set_mutant(int on);
#ifdef __cplusplus
}
#endif
#else
#define Z3660_SPEC_SERVED() do { } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The cross-core knob -----------------------------------------------------------
 *
 * Same shape as z3660_dopc_resolve() and z3660_dfast_resolve(), and for the same reason:
 * core0's serial console and core1's knob resync must not be able to disagree about what
 * a word means, so the mapping lives here and nowhere else. UNSET (0) MEANS ON -- a core0
 * built before the field existed publishes a zeroed struct, and that must run the shipped
 * behaviour rather than silently disarming the experiment. */
#define Z3660_SPEC_OFF     0xFFFFFFFFu
#define Z3660_SPEC_DEFAULT 1u
static __inline__ unsigned z3660_spec_resolve(unsigned requested)
{
   return (requested == Z3660_SPEC_OFF) ? 0u : 1u;
}

/* Arm (1) or disarm (0). Returns the previous state. A CHANGE invalidates the decoded-op
 * cache, because the verdict bit is stored in its entries. Disarmed is the reference
 * machine: every MOVE takes the generated handler through the ordinary indirect call. */
int  z3660_spec_arm(int on);
int  z3660_spec_armed(void);

/* Read-back + direct set for the console and the host harness. The setter takes a RAW
 * mailbox value and runs it through z3660_spec_resolve(), so a console command and a
 * mailbox word cannot mean two different things. */
unsigned z3660_spec_get(void);
unsigned z3660_spec_set(unsigned requested);

#ifdef __cplusplus
}
#endif

#endif /* Z3660_SPEC_H */
