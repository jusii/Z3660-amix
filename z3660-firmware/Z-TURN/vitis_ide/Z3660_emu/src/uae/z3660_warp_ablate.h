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
 * A3 AND A4 ARE DYNAMICALLY IDENTICAL. With the hook gone nothing on the disarmed path reads
 * the flag at all, so the only difference between those two images is where every member
 * after `warp_on` sits in `struct regstruct` -- 7 899 function bodies by §0.13.3's census.
 * That is the cleanest layout measurement this campaign can construct, and it is why the
 * field is not offered as an independent removal: taking it out while leaving the hook in
 * would put the flag back in a standalone global, which is round 3's arrangement and a
 * different change rather than an ablation.
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
