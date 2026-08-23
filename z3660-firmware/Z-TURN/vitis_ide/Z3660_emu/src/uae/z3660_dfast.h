/* z3660_dfast.h -- CYBERSTORM C2 rungs 2 and 3: the accessor fast paths.
 *
 * The file is named for rung 2, which built it. It now carries BOTH directions -- the
 * data accessors (rung 2, `z3660_dfv_r` / `_w`, switch DFAST) and the instruction-fetch
 * accessors (rung 3, `z3660_ifv`, switch IFAST). They share one verdict vocabulary, one
 * table-build replay and one set of rebuild sites, which is the whole reason they are not
 * two files: two copies of "what does a TTR match mean" is the divergence this fork has
 * paid for elsewhere. Renaming the file would churn core0's console include for nothing.
 * Rung 3's own design note is the second banner, below the fallback section.
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
 * the original path. It gives the host harness a genuine reference to run the equivalence
 * corpus against, and metal a `PROFZ`-fenced same-boot A/B without a second flash.
 *
 * IT IS NOT FREE, AND THE FIRST VERSION OF THIS PARAGRAPH SAID IT WAS. Measured across
 * two flashes on 2026-08-23: lean cadence 4, DFAST off, 6294.5 dhrystones against the
 * pre-rung-2 build's 6539.0 -- **-3.74 %**. The claim is true of the ARMED table and
 * false of the disarmed one: with every verdict SLOW an access still pays the table load
 * and the branch and THEN runs the unmodified accessor. So the same-boot switch OVERSTATES
 * the shipping gain, exactly and arithmetically:
 *
 *     shipping (cross-build)  8184.7 / 6539.0 = 1.2517
 *     disarm penalty          6539.0 / 6294.5 = 1.0388
 *     product                                   1.3003 = the same-boot A/B, to 4 digits
 *
 * Both numbers are honest and they answer different questions. "What does the switch do
 * on this boot" is +30.03 %; "what did shipping this rung buy" is +25.17 %, and the second
 * is the one to quote. THE SAME CAVEAT APPLIES TO IFAST and is priced in its own banner.
 *
 * ==========================================================================
 * CYBERSTORM C2 RUNG 3 -- THE SAME RUNG, ON THE INSTRUCTION SIDE
 * ==========================================================================
 *
 * Rung 2 removed the out-of-line `mmu_match_ttr()` call from the data accessors. The
 * INSTRUCTION accessors carry the same call -- `mmu_match_ttr_ins()` -- and pay it on
 * every fetch, and after rung 2 landed, `FETCHOP`+`FETCHEX` is the largest attackable
 * group left on the C2 ladder (18.06-19.98 % of cadence-4 against READ+WRITE's now
 * 14.58-15.87 %). The s19 capture priced the population: 53.3 M of 61.3 M instruction
 * fetches -- 87 % -- are ITT-matched, because AMIX-040's `pstart040.s` sets
 *
 *     ITT0 = 0x003fc000   0-1 GB code, BOTH privilege levels
 *
 * and the kernel's text lives under 1 GB. So the fetch side is the MIRROR of the data
 * side, with the arms weighted the other way round: where data accesses mostly fall
 * through to the page cache (PAGE) and take the transparent arm about a quarter of the
 * time, fetches take the transparent arm ~87 % of the time. `likely()` points the other
 * way here, deliberately, and that is the only structural difference in the resolver.
 *
 * ONE VERDICT DIFFERENCE, AND IT IS REAL. On the data side a write-protected window
 * (`TTR_NO_WRITE`) means "reads transparent, writes take the original path". A fetch is
 * a read, and `mmu_get_iword()`'s guard tests `== TTR_NO_MATCH` -- so `TTR_NO_WRITE` is
 * transparent for a fetch, with the window's own cache state installed, exactly as
 * `TTR_OK_MATCH` is. The instruction table therefore maps BOTH match results to `XPAR`.
 * (`W` in an ITTR is architecturally meaningless anyway; the table replays what this
 * emulator does, not what the manual implies.)
 *
 * THE PAGE ARM IS THE *INSTRUCTION* PAGE CACHE, WHICH IS ONE ENTRY, NOT 256. Rung 2's
 * PAGE arm indexes a 256-entry array; `MMU_IPAGECACHE` is a single
 * `atc_last_ins_laddr`/`_paddr`/`_cache` triple. The fast path uses those three variables
 * with the tag expression copied from `mmu_get_iword()` byte for byte -- so `IPAGE_HIT`
 * and `IPAGE_MISS` keep counting the same events, and the pre-registration that the fetch
 * hit rates must not move is satisfied by construction rather than by hope. An
 * invalidation needs no hook for the same reason: `flush_shortcut_cache()` writes
 * `atc_last_ins_laddr = mmu_pagemask` (an impossible tag) and the fast path reads that
 * variable, so it goes stale in exactly the same instant the original does.
 *
 * ALIGNMENT IS GATED AT THE CALL SITE, and this is the one place rung 3 needs a test that
 * rung 2 got for free. `uae_mmu040_get_ilong()` already tests `is_unaligned_bus(addr, 4)`
 * above the fast path (ISSUE-52), so everything reaching the resolver from there is
 * aligned. `uae_mmu040_get_iword()` has NEVER had such a test -- an odd instruction
 * address is an address error the 68040 raises elsewhere -- so the fast path gates itself
 * on `!(addr & 1)` there. Not to re-route the access (the original path for an odd fetch
 * must stay exactly what it was) but so that "an access served by the fast path cannot
 * cross a page or a bank window" stays STRUCTURAL rather than probabilistic. A word at
 * `page_end - 1` is the whole reason: served, it would read one byte out of an unrelated
 * physical frame -- the cdfs page-crossing DMA, again, in a hotter loop.
 *
 * WHAT RUNG 3 DOES NOT TOUCH. The decoded-op cache's extension-word window sits ABOVE the
 * fetch bracket in `uae_mmu040_get_iword()` and is untouched: a word served from it costs
 * no fetch at all and must not be charged to one. Rung 3 serves what the window and the
 * dopc lookup DO NOT -- dopc misses, and the extension words of a hit whose window has
 * already been consumed. `Z3660_PROF_ENTER_IFETCH()` and the `FETCH` counter stay exactly
 * where they are, above the fast path, so `FETCHOP`/`FETCHEX` still bracket every fetch
 * and the profile stays comparable across the rung. The bucket gets cheaper; it does not
 * get emptier.
 */

#ifndef Z3660_DFAST_H
#define Z3660_DFAST_H

/* SELF-CONTAINED ON PURPOSE, and spelled in plain types rather than uae_u*.
 *
 * core0's serial console includes this header DIRECTLY -- exactly as it includes
 * z3660_blk.h and z3660_dopc.h -- so that z3660_dfast_resolve() below is the single place
 * a mailbox word becomes a state. core0 cannot compile the emulator's headers, so this one
 * must not need them. A mirrored core0 copy would only be a second place for the OFF
 * sentinel to drift away from the one core1 tests against.
 */

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
#define Z3660_DFAST_CSTATE(v)  ((unsigned char)((v) >> 8))

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
extern "C" unsigned long z3660_ifast_serves;
#else
extern unsigned long z3660_dfast_serves;
extern unsigned long z3660_ifast_serves;
#endif
#define Z3660_DFAST_SERVED() (z3660_dfast_serves++)
/* Rung 3 gets its OWN counter rather than sharing rung 2's. The two paths are separately
 * switchable, so a corpus case that pins "the FETCH fast path served this and the data
 * one did not" is only expressible with two counters -- and that case is most of rung 3's
 * engagement evidence, because the interpreted tests necessarily do both at once. */
#define Z3660_IFAST_SERVED() (z3660_ifast_serves++)
#else
#define Z3660_DFAST_SERVED() do { } while (0)
#define Z3660_IFAST_SERVED() do { } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Verdict tables, indexed [super][addr >> 24]. 16 bits: the verdict in the low byte, the
 * `mmu_cache_state` a match installs in the high byte. Two directions because the W bit
 * and the write-fault rules differ, and because the read and write page caches are
 * separate arrays anyway. 2 KB in total. */
extern unsigned short z3660_dfv_r[2][256];
extern unsigned short z3660_dfv_w[2][256];

/* Rung 3's, indexed the same way, built from regs.itt0/itt1 by the same replay. ONE
 * direction, because a fetch is always a read -- 1 KB, not 2. */
extern unsigned short z3660_ifv[2][256];

/* Rebuild ALL THREE tables from regs.dtt0/dtt1, regs.itt0/itt1, regs.mmu_enabled and the
 * two arm switches. Called from mmu_tt_modified() and mmu_set_tc(); safe to call at any
 * time. One function because the four TT registers are recomputed at exactly the same two
 * sites -- splitting it would create a second place for a rebuild site to be forgotten. */
void z3660_dfast_rebuild(void);

/* Arm (1) or disarm (0) the fast path and rebuild. Returns the previous state. Disarmed
 * is the reference machine: every access takes the original path. */
int  z3660_dfast_arm(int on);
int  z3660_dfast_armed(void);
int  z3660_ifast_arm(int on);
int  z3660_ifast_armed(void);

/* THE CROSS-CORE KNOB, and the one rule that turns a mailbox word into a state.
 *
 * Same shape as z3660_dopc_resolve() and for the same reason: core0's serial console and
 * core1's knob resync must not be able to disagree about what a word means, so the
 * mapping lives here and nowhere else. UNSET (0) MEANS ON -- a core0 built before the
 * field existed publishes a zeroed struct, and that must run the shipped behaviour rather
 * than silently disarming the rung. */
#define Z3660_DFAST_OFF     0xFFFFFFFFu
#define Z3660_DFAST_DEFAULT 1u
static __inline__ unsigned z3660_dfast_resolve(unsigned requested)
{
   return (requested == Z3660_DFAST_OFF) ? 0u : 1u;
}

/* THE SECOND KNOB, AND WHY IT IS A SECOND KNOB.
 *
 * Rung 3 could have folded into DFAST -- one field, one command, one thing to explain.
 * It does not, for a measurement reason that outranks the tidiness:
 *
 *  1. A SHARED switch cannot price rung 3. Toggling it would A/B (rung 2 + rung 3)
 *     against neither, and rung 2's number is already banked. The question this rung has
 *     to answer is what the FETCH side buys ON TOP of the shipped data side, which is
 *     exactly what a second switch measures and a shared one cannot.
 *  2. It makes the 2x2 available in ONE boot. DFAST x IFAST, four arms, one PROFZ fence
 *     each: that is the only cheap way to see whether the two rungs INTERACT -- and there
 *     is a live hypothesis that they might. The 2026-08-23 verdict found DOPCFIND costing
 *     15.5 % more per dispatch after rung 2, with D-cache pressure from rung 2's new
 *     always-hot 2 KB as one of two candidate causes. Rung 3 adds 1 KB more of the same
 *     kind. A shared switch would make that question unaskable without a reflash.
 *  3. Bisection. If the fetch path is ever suspected of a correctness fault on metal, a
 *     separate knob turns off exactly the suspect and leaves the machine otherwise
 *     shipped.
 *
 * Same encoding and the same rule as DFAST, for the same reason: UNSET (0) MEANS ON, so a
 * core0 built before the field existed publishes a zeroed struct and still runs the
 * shipped behaviour instead of silently disarming the rung. */
#define Z3660_IFAST_OFF     0xFFFFFFFFu
#define Z3660_IFAST_DEFAULT 1u
static __inline__ unsigned z3660_ifast_resolve(unsigned requested)
{
   return (requested == Z3660_IFAST_OFF) ? 0u : 1u;
}

/* Harness/console: how many accesses each fast path has served. Prof build only; 0
 * elsewhere. Reads the profiler's own counters, so it cannot disagree with a dump. */
unsigned long long z3660_dfast_served(void);
unsigned long long z3660_ifast_served(void);

#ifdef __cplusplus
}
#endif

#endif /* Z3660_DFAST_H */
