/* THE DISARM-RESIDUAL ABLATION KIT -- docs/WARP-ENGINE.md §0.14.1.
 *
 * WHY THIS FILE EXISTS. The disarm penalty -- what a DISARMED image with the engine compiled
 * in costs against the clean interpreter lineage -- has been measured three times (-2.276 %,
 * -5.848 %, -4.256 %) and explained none. TWO named mechanisms have been refuted ON METAL:
 * §0.10.6's L1I footprint (round 3 measured a SMALLER loop costing MORE) and §0.12.1's
 * dependency depth (round 4 landed the depth-1 gate exactly as designed and got 0.2 pp
 * worse). Reading a shape off an objdump, naming it, and buying a metal row to test it is a
 * method that has now failed twice, and §0.14.0 retires it for this problem.
 *
 * What replaces it is a BISECTION. Each `-D` below removes exactly one layer of what WARP
 * adds to a disarmed image, from ONE source tree, so a metal session can measure the same
 * dhrystone against the same reference with one layer missing at a time and read which layer
 * carries the cost. The variants are built into their own directories, because a `-D` is not
 * a tracked dependency and sharing a build dir would let one variant's objects survive into
 * another's link -- a hazard this repository has already been bitten by twice.
 *
 * THE TRUTH TABLE, and it is here rather than in the Makefile so that a variant cannot mean
 * two different things in two places:
 *
 *   level  removes                         reads the edge
 *   -----  ------------------------------  ------------------------------------------------
 *     0    nothing -- THE SHIPPING LEAN     the control; its penalty is what is decomposed
 *     1    the on-target self-test          A0->A1 = cold code and link layout
 *     2    the arm attestation              A0->A2 = THE NOISE FLOOR (provably off the
 *                                           disarmed path, so whatever it measures is noise)
 *     3    the run-loop hook                A0->A3 = the hot-path gate
 *     4    the run-loop hook AND the
 *          `regstruct` field                A3->A4 = STRUCT DISPLACEMENT, ALONE
 *     5    the engine entirely              A0->A5 = closure
 *
 * ROUND 6 CHANGED WHAT LEVELS 3 AND 4 MEAN, AND IT CHANGED IT BY MAKING THEM UNNECESSARY.
 * The kit's own measurement (§0.16.2) attributed 1.967 pp to L-HOOK and 0.929 pp to L-REG,
 * and §0.17.1 removed both from the shipping image: the run loop is now instantiated TWICE
 * (newcpu_mmu040_body.h) so a disarmed image runs a loop with no hook compiled in, and
 * `warp_on` moved to the END of `struct regstruct` so no existing member's displacement
 * moves. So on a post-fix tree:
 *
 *   - level 3 no longer removes a cost from the disarmed path -- there is none to remove.
 *     What it removes now is the HOOKED loop, i.e. the ability to arm at all.
 *   - level 4's field removal moves nothing, because a member appended after the last one
 *     changes no other member's offset.
 *
 * THE EDGES A0->A3 AND A3->A4 ARE THEREFORE PREDICTED TO COLLAPSE INTO THE NOISE FLOOR,
 * which turns the kit into the fix's own verifier (§0.17.3 G-R6.10). Levels 1, 2 and 5 are
 * unchanged and are the three a re-measurement session actually needs: the control, the
 * floor, and closure.
 *
 * The pre-fix reading is kept above, byte-untouched, because it is what §0.14.1
 * pre-registered and §0.16.2 scored.
 *
 * NO VARIANT EXCEPT LEVEL 0 IS EVER A CANDIDATE TO SHIP. Level 0 must also be byte-for-byte
 * what the tree builds with this file's flags absent entirely -- gate G-R5.10 -- so that the
 * kit cannot have changed the thing it exists to measure.
 */
#ifndef Z3660_WARP_ABLATE_H
#define Z3660_WARP_ABLATE_H

#ifndef Z3660_WARP_ABLATE
#define Z3660_WARP_ABLATE 0
#endif

#define Z3660_WARP_ABL_NO_SELFTEST (Z3660_WARP_ABLATE == 1 || Z3660_WARP_ABLATE == 5)
#define Z3660_WARP_ABL_NO_ATTEST   (Z3660_WARP_ABLATE == 2 || Z3660_WARP_ABLATE == 5)
#define Z3660_WARP_ABL_NO_HOOK     (Z3660_WARP_ABLATE >= 3)
#define Z3660_WARP_ABL_NO_REGFIELD (Z3660_WARP_ABLATE >= 4)
#define Z3660_WARP_ABL_NO_ENGINE   (Z3660_WARP_ABLATE == 5)

#endif /* Z3660_WARP_ABLATE_H */
