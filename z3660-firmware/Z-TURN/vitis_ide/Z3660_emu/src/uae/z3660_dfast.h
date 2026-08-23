/* z3660_dfast.h -- CYBERSTORM C2 rung 2: the data-accessor fast path.
 *
 * ==========================================================================
 * WHAT THIS REMOVES, AND WHY IT IS NOT TRANSLATION
 * ==========================================================================
 *
 * The C2 attack map v3.1 measured a guest data access at 127.6 corrected ARM cycles for
 * a read and 142.1 for a write -- 19.59-20.35 % of the interpreter at cadence 1 and
 * 23.12-24.13 % in the standing posture, the largest attackable group on the ladder --
 * while the tier-0 `dpagecache` was hitting 99.21 % on reads and 99.35 % on writes. Its
 * conclusion is the premise of this file:
 *
 *     "At a 99.2-99.4 % tier-0 page-cache hit rate, essentially every guest access is
 *      already being served without calling mmu_translate, and it still costs ~134 ARM
 *      cycles. The cost is therefore not translation; it is the scaffolding around a
 *      hit."
 *
 * Counted out of the generated ARM (`arm-none-eabi-objdump -d` of the lean
 * `Z3660_emu.elf`), the scaffolding around a hit was:
 *
 *   uae_mmu040_get_long()            an out-of-line call (one copy per TU, ~920 call
 *                                    sites into it from cpuemu_31 alone)
 *     mmu_match_ttr()                ANOTHER out-of-line call, on EVERY data access
 *       mmu_do_match_ttr(dtt0)       and another
 *       mmu_do_match_ttr(dtt1)       and another, when dtt0 does not match
 *     the dpagecache probe           reached only if no TTR matched
 *     x_phys_get_long()              an INDIRECT call through a function pointer
 *       phys_get_long()              a branch
 *         memory_get_long()          an out-of-line call: an indexed load out of the
 *                                    65 536-entry (256 KB) mem_banks[] table, a load of
 *                                    the addrbank's direct pointer, and only then the
 *                                    guest load itself
 *
 * ==========================================================================
 * THE MEASUREMENT THAT REDIRECTED THE DESIGN: THE TTR CALL IS ON EVERY ACCESS
 * ==========================================================================
 *
 * The map's model of this bucket was "a page-cache hit plus scaffolding". The counters
 * say the page cache is not even reached by a large minority of accesses:
 *
 *     19-STAGE-c1-profd.txt:  READ 39 738 605 data reads,
 *                             DPAGE_RHIT + DPAGE_RMISS = 27 760 860
 *
 * A misaligned read SPLITS into two or four sub-accesses that each probe the page cache,
 * so the page-cache count should EXCEED the read count. It is 30 % below it. The only
 * way out of the accessor before the page cache is a TTR match -- and the AMIX-040
 * kernel's own `pstart040.s` says exactly why:
 *
 *     ITT0 = 0x003fc000   0-1GB code, both privilege levels
 *     DTT0 = 0x003fc060   0-1GB data, both privilege levels, cache-inhibited
 *     DTT1 = 0x807fa060   0x80000000+, SUPERVISOR ONLY (user VAs live up there and must
 *                         translate through the URP tables)
 *
 * So every SUPERVISOR data access is transparently translated (DTT0 below 1 GB, DTT1
 * above 2 GB) and never touches the page cache at all, while user accesses fall through
 * both windows to it. `mmu_ttr_enabled` is therefore true for the whole run, and
 * `mmu_get_long()`'s guard calls `mmu_match_ttr()` out of line on 100 % of data
 * accesses. On the supervisor arm that call is ~39 instructions; on the user arm, where
 * it walks both windows and matches neither, ~48. For a WRITE it is wrapped in a second
 * out-of-line call (`mmu_match_ttr_write`) and costs ~55/69.
 *
 * That is the largest single piece of the scaffolding, it is paid on every access, and
 * it was not in the map's model of the bucket. It is also why writes measured 142.1 cyc
 * against reads' 127.6: the extra wrapper is on the write side only.
 *
 * ==========================================================================
 * THE DESIGN: ONE DERIVED TABLE, NO NEW CACHE
 * ==========================================================================
 *
 * A 68040 TTR match depends on the address ONLY through its top 8 bits
 * (`MMU_TTR_LOGICAL_BASE` is 0xff000000), plus the privilege level and the register's
 * own enable/S/W/CM bits. So the entire trichotomy -- transparent, no-match, or
 * write-protected -- is a pure function of (super, addr >> 24) and four registers, and
 * it fits in a 2 x 256 table of 16-bit verdicts per direction. **One byte-pair load
 * replaces the call chain.**
 *
 * THIS IS NOT A NEW CACHE. It holds no translation, no physical address and no guest
 * data; it is a memoized truth table over four control registers, in the same class as
 * `mmu_ttr_enabled` itself -- which is derived from the same four registers in the same
 * function. It cannot go stale in any way `mmu_ttr_enabled` cannot, because it is
 * rebuilt at exactly the sites that recompute `mmu_ttr_enabled`:
 *
 *     mmu_tt_modified()   every MOVEC to ITT0/ITT1/DTT0/DTT1, and CPU reset
 *     mmu_set_tc()        the only writer of regs.mmu_enabled, and of the page geometry
 *
 * The page-cache arm then uses the EXISTING `atc_data_cache_read/write` with the
 * EXISTING index and tag arithmetic -- byte for byte the expression in cpummu.h -- so
 * `DPAGE_RHIT` / `DPAGE_WHIT` keep counting exactly what they counted before. The map's
 * pre-registration requires the hit rates not to move; sharing the tag arithmetic is how
 * that is guaranteed rather than hoped for.
 *
 * ==========================================================================
 * THE FALLBACK IS THE ORIGINAL CODE, UNMODIFIED
 * ==========================================================================
 *
 * Every arm that is not "transparent, or a page-cache hit, into a direct RAM bank, from
 * an aligned address" returns 0 and the caller runs `mmu_get_long()` / `mmu_put_long()`
 * verbatim -- the same text, in the same place, as before this file existed. Miss
 * paths, fault paths, the write-back contract, `mmu_bus_error`'s frame, the M-bit
 * re-walk, `TTR_NO_WRITE`, the `TCR_DWO` bus error, device banks: none of them are
 * reimplemented here, so none of them can drift. The fast path either answers exactly
 * what the original would have answered, or it does not answer.
 *
 * Two consequences worth stating outright:
 *
 *  - **It never fast-paths across a page boundary.** The caller tests
 *    `is_unaligned_bus()` FIRST and misaligned accesses are gone before this is reached,
 *    so every access that gets here is naturally aligned -- and a naturally aligned 1-,
 *    2- or 4-byte access cannot cross a 4 KB or 8 KB page, nor a 64 KB bank window. The
 *    map's hard rule is satisfied structurally, not by a check that could be wrong.
 *  - **It never fast-paths a device.** `baseaddr_direct_r/_w` is consulted LIVE out of
 *    `mem_banks[]` on every access, exactly as `memory_get_long()` does, so a bank that
 *    is not plain memory declines and the original path issues the access with its side
 *    effects intact. Nothing about the bank map is cached here, which is why
 *    `map_banks()` needs no new hook.
 *
 * ==========================================================================
 * THE SWITCH IS FREE, AND IT IS THE REFERENCE ARM
 * ==========================================================================
 *
 * `z3660_dfast_arm(0)` rebuilds the tables all-SLOW, so every access falls through to
 * the original path. That costs NOTHING on the hot path -- the disarm lives in the table
 * the hot path was going to read anyway -- and it gives the host harness a genuine
 * reference to run the equivalence corpus against, and metal a `PROFZ`-fenced same-boot
 * A/B without a second flash.
 */

#ifndef Z3660_DFAST_H
#define Z3660_DFAST_H

#include "uae/types.h"

/* THE INLINE IS NOT DECORATION -- it is the rung.
 *
 * `ALWAYS_INLINE` in this tree is bare `__inline` (mmu_common.h): a HINT, and at the -Os
 * the firmware builds with gcc declines it. Measured, on the first build of this rung:
 * the resolver came out as an out-of-line `bl` with both results returned through the
 * stack, so the change ADDED a call to the path whose entire purpose is to remove calls.
 * Rung 1's z3660_dopc_find() carries the same note for the same reason.
 *
 * `__attribute__((always_inline))` is a command. It applies to ~10 code sites (the single
 * out-of-line copy of each uae_mmu040_ accessor per translation unit), not to the ~3000
 * handler call sites, so the I-cache footprint the map warns about is bounded by
 * construction. */
#define Z3660_DFAST_INLINE static __inline__ __attribute__((always_inline))

/* The three verdicts. Deliberately small and deliberately ordered so that "0 means run
 * the original path" is the value a zeroed table produces: a table that was never built
 * must behave like a disarmed one, not like a transparent one. */
#define Z3660_DFAST_SLOW  0u   /* run the unmodified accessor -- no claim is made        */
#define Z3660_DFAST_XPAR  1u   /* a TTR matched: the physical address IS the logical one */
#define Z3660_DFAST_PAGE  2u   /* no TTR matched and the MMU is on: consult the page cache */

#define Z3660_DFAST_VERDICT(v) ((v) & 0xFFu)
#define Z3660_DFAST_CSTATE(v)  ((uae_u8)((v) >> 8))

/* THE ENGAGEMENT SIGNAL, and why it is not the profiler's counter.
 *
 * "The two paths agree" is trivially true when the fast path silently never fired --
 * the failure a state-equivalence corpus is blindest to -- so every case in the corpus
 * has to declare whether the fast path MUST serve or MUST NOT, and something has to
 * check it. `DFAST_HIT` does that, but it only exists in the PROFILING build, and the
 * build whose correctness matters is the LEAN one that ships.
 *
 * So the harness gets its own counter, on the same `HOST_TEST_HARNESS` pattern
 * mmu_unaligned_split_count already uses for the ISSUE-52 routing invariant: present in
 * all three harness variants, absent from every firmware binary, so the corpus can pin
 * engagement in lean and default too. In the profiling variant the two are cross-checked
 * against each other. */
#ifdef HOST_TEST_HARNESS
#ifdef __cplusplus
extern "C" unsigned long z3660_dfast_serves;
#else
extern unsigned long z3660_dfast_serves;
#endif
#define Z3660_DFAST_SERVED() (z3660_dfast_serves++)
#else
#define Z3660_DFAST_SERVED() do { } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Verdict tables, indexed [super][addr >> 24]. 16 bits: the verdict in the low byte, the
 * `mmu_cache_state` a match installs in the high byte. Two directions because the W bit
 * and the write-fault rules differ, and because the read and write page caches are
 * separate arrays anyway. 2 KB in total. */
extern uae_u16 z3660_dfv_r[2][256];
extern uae_u16 z3660_dfv_w[2][256];

/* Rebuild both tables from regs.dtt0/dtt1, regs.mmu_enabled and the arm switch. Called
 * from mmu_tt_modified() and mmu_set_tc(); safe to call at any time. */
void z3660_dfast_rebuild(void);

/* Arm (1) or disarm (0) the fast path and rebuild. Returns the previous state. Disarmed
 * is the reference machine: every access takes the original path. */
int  z3660_dfast_arm(int on);
int  z3660_dfast_armed(void);

/* Harness/console: how many accesses the fast path has served. Prof build only; 0
 * elsewhere. Reads the profiler's own counters, so it cannot disagree with a dump. */
uae_u64 z3660_dfast_served(void);

#ifdef __cplusplus
}
#endif

#endif /* Z3660_DFAST_H */
