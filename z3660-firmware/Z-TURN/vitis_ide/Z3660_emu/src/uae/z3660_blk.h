/* CYBERSTORM C2 -- the block-idiom fast path: the switch rule, in one testable place.
 *
 * WHAT THE FAST PATH IS. The C2 map measured a family of tight kernel block loops --
 * `bzero`, `mcpy`, the 68040 `CPUSHL` cache-flush loop and their relatives -- at 33.6 % of
 * an AMIX boot, 25.1 % of a raw disk read and 10.6 % of dhrystone. `bzero` alone is
 * 22.1 % of boot, and it is two guest instructions:
 *
 *     0800032c   20c9   MOVE.L A1,(A0)+
 *     0800032e   51c9   DBF    D1,-4
 *
 * Costed from the map's per-event table, those two cost 850 ARM cycles per four bytes
 * moved -- of which the guest store is 163. **80.9 % of the price of zeroing a page is
 * interpreter machinery around a four-byte write.** The recognizer in newcpu.cpp spots
 * the loop at dispatch time and runs many iterations per pass of the run loop, paying the
 * fetch, the dispatch, the tail and the poll once for the chunk instead of once per guest
 * instruction. It does NOT replace the guest store; see newcpu.cpp for why that is the
 * whole safety argument.
 *
 * WHY THE KNOB IS A CHUNK SIZE AND NOT A YES/NO. The chunk is what bounds interrupt
 * latency: the run loop's interrupt poll runs once per chunk, so a chunk of N iterations
 * is a window of N guest instruction-pairs in which no interrupt is noticed. One knob
 * therefore has to express three things a metal session wants -- off, on, and how long a
 * stall it will tolerate -- and a boolean expresses one of them.
 *
 * THE ENCODING, and why "unset" is not "off". Same contract as overhead_switches.h: a
 * mailbox word that nobody wrote must resolve to the behaviour the firmware is supposed to
 * have, not to zero. SHARED is plain memory; a core0 built before this field existed
 * leaves it 0, and if 0 meant "off" the shipped default would silently depend on which
 * core0 is in the image. So 0 is UNSET and resolves to the default, and "off" gets an
 * explicit sentinel. The field is unsigned, which is why the sentinel is all-ones rather
 * than overhead_switches.h's -1.
 *
 * The rule lives in a header, and it is a RULE rather than a measurement, for the reason
 * amix_ram.h and overhead_switches.h give: the host harness can then compile it directly
 * and assert every branch, including the ones no metal session will ever exercise.
 */
#ifndef SRC_UAE_Z3660_BLK_H_
#define SRC_UAE_Z3660_BLK_H_

/* Mailbox word values (SHARED.blk_chunk, 0xFFFF00E4). */
#define Z3660_BLK_UNSET     0u            /* nobody wrote it -> the default below     */
#define Z3660_BLK_OFF       0xFFFFFFFFu   /* recognizer never fires; pure passthrough */

/* Iterations per chunk when the mailbox says nothing.
 *
 * 256 is chosen against interrupt latency, not against throughput. A fast-path iteration
 * is the guest store plus a handful of ARM instructions -- call it 180 ARM cycles, ~164 ns
 * at 1.1 GHz -- so 256 iterations is a ~42 us window with no interrupt poll in it. That
 * sits comfortably inside one 15.6 kHz hsync period (64 us), which is the tightest cadence
 * anything on this poll actually needs: docs/AMIX.md's A3000-SCSI interrupt-delay pump
 * rides on check_uae_int_request(). A full 4 KB page (1024 iterations) would be ~168 us
 * and would skip hsyncs.
 *
 * The throughput cost of stopping at 256 rather than 1024 is the recognizer's own test,
 * amortised: ~2 % of the fast path instead of ~0.5 %. That is a good trade for staying
 * inside a cadence something depends on. */
#define Z3660_BLK_DEFAULT   256u

/* Hard ceiling, applied to whatever the mailbox says. Above a page's worth of longwords
 * the cap stops meaning anything anyway -- the recognizer re-validates its device gate at
 * every page boundary and so never runs further than one page in a single chunk. */
#define Z3660_BLK_MAX       1024u

/* requested -> iterations per chunk, or 0 for "do not fast-path at all".
 *
 * A resolved 0 is the OFF state and it must be a PURE PASSTHROUGH: the recognizer returns
 * before it has looked at anything, so the run loop dispatches exactly the instruction it
 * would have dispatched without this feature. That is what makes the harness's
 * state-equivalence corpus meaningful -- the off state is the reference implementation,
 * not a second implementation that happens to agree. */
static inline unsigned z3660_blk_resolve_chunk(unsigned requested)
{
   if (requested == Z3660_BLK_UNSET) return Z3660_BLK_DEFAULT;
   if (requested == Z3660_BLK_OFF)   return 0u;
   if (requested > Z3660_BLK_MAX)    return Z3660_BLK_MAX;
   return requested;
}

#endif /* SRC_UAE_Z3660_BLK_H_ */
