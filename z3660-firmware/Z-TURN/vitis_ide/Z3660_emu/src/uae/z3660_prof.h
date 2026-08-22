/* CYBERSTORM C1 -- the interpreter profiler: contract, wire format, and hot-path probes.
 *
 * WHAT THIS IS FOR
 *
 * C3/C3b made the firmware's diagnostic overhead switchable and then compiled it out
 * (`make lean`). What is left in the per-guest-instruction budget is the interpreter
 * itself, and nothing in this firmware has ever measured WHERE inside the interpreter
 * that time goes. C2 is meant to rank candidate attacks -- fetch/dispatch specialisation,
 * a decoded-op cache, accessor inlining, an ATC fast path, a JIT study -- and a ranking
 * built on anything but measurement is a guess with a table around it.
 *
 * So this is a THIRD build variant, `make profile`, and it exists to be measured, not
 * shipped. It costs what it costs; the point is that the cost is stated, calibrated, and
 * subtractable rather than unknown.
 *
 * THREE VARIANTS, ONE TREE (see also runloop_build.h, which owns the other two)
 *
 *   BOOT.BIN        default  -- diagnostics compiled in, runtime-switchable (PROBES/PERFS).
 *   BOOT-lean.BIN   lean     -- diagnostics compiled out; the production image.
 *   BOOT-prof.BIN   profile  -- the LEAN hot loop plus this profiler.
 *
 * The profiling variant is built ON TOP OF LEAN on purpose (`make profile` passes
 * -DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0 -DZ3660_EMU_PROF=1). A profile of the default
 * build would be a map of a binary nobody runs; the map that is worth having describes
 * the loop that actually ships. The C3 calibration priced the difference at 9.1 % of
 * guest throughput, which is larger than several of the effects C2 will be asked to rank.
 *
 * TWO INSTRUMENTS, DELIBERATELY SEPARATE
 *
 *   THE SAMPLER answers "where in the GUEST". A wall-clock tick captures the guest PC and
 *   opcode into a ring. The TICK is cheap -- one coprocessor register read and a compare
 *   per emulated instruction -- and it is the instrument that says whether a decoded-op
 *   cache has an upside at all, that question being exactly "is the guest PC distribution
 *   concentrated?". A cheap tick is not a cheap IMAGE, though: see bias note 1b.
 *
 *   THE BUCKETS answer "where in the INTERPRETER". A phase machine charges every ARM
 *   cycle to exactly one stage -- opcode fetch, extension fetch, data read, data write,
 *   MMU translate, table walk, handler body, loop tail, fault. It is EXPENSIVE (see BIAS
 *   below) and it changes the absolute instruction rate. Its shares are the answer; its
 *   rate is not.
 *
 * They are switchable independently (`PROF` / `PROFB` on the serial console) precisely so
 * a session can take an undistorted PC map and a distorted stage map and not confuse one
 * for the other.
 *
 *   THE COUNTERS are a third thing and are NOT switchable: in a profiling build they
 *   always run. Each is a 64-bit increment on an accessor that was going to be called
 *   anyway, they are exact regardless of what the timing instruments are doing, and they
 *   are the numbers C2 needs most (page-cache and ATC hit rates, misalign frequency,
 *   supervisor share). Making them switchable would add a predicate to buy back an
 *   increment -- which is precisely the trade C3b measured and rejected. In the DEFAULT
 *   and LEAN builds they do not exist at all; see WHAT DOES NOT RIDE THE DEFAULT BUILD.
 *
 * ==========================================================================
 * BIAS -- read this before believing a number out of here
 * ==========================================================================
 *
 * 1. THE SAMPLER FIRES AT INSTRUCTION BOUNDARIES, NOT ON AN INTERRUPT. There is no timer
 *    IRQ on core1; the tick is a deadline compared against the ARM PMU cycle counter in
 *    the run-loop tail. Consequences, in order of how much they matter:
 *
 *      - A sample is attributed to the instruction that was EXECUTING when the deadline
 *        expired, because the tail still holds that instruction's start PC. This is the
 *        right attribution, and it is why the poll is in the tail and not before the
 *        fetch.
 *      - Time spent OFF the run loop -- inside do_specialties()'s STOP spin, inside
 *        Exception(), inside the CATCH, before the loop is entered at all -- accrues no
 *        samples while it lasts. The first tail after re-entry then takes a sample whose
 *        WEIGHT field says how many tick periods it stands for. A weight of 40 at 1 kHz
 *        means that PC was reached after 40 ms in which the sampler could not fire. The
 *        weight is not a correction; it is a disclosure. A symbolizer MUST treat weight
 *        as the sample's multiplicity, and a histogram of weights is the honest measure
 *        of how much of the run this instrument could not see.
 *      - A guest instruction that takes longer than one tick period (a mailbox round trip
 *        to core0 through an emulated device) produces ONE sample of high weight, not
 *        several. Correct total, coarse resolution.
 *
 * 2. THE BUCKETS CHARGE THE PROBE'S OWN COST TO THE BUCKETS. Each phase transition is a
 *    cycle-counter read, a subtract, a 64-bit accumulate and a stack push/pop. That cost
 *    lands partly in the bucket being left and partly in the one being entered, and there
 *    are twelve to eighteen transitions per emulated instruction (v2's tail split added
 *    four to v1's eight-to-fourteen). The profiler MEASURES this at arm time and reports
 *    it as PROBE_CYC together with the total transition count, so a reader can compute
 *    the inflation and, if they care, subtract transitions x PROBE_CYC from each bucket.
 *    The instrumented instruction rate will be well below the lean rate. That is the
 *    instrument, not the interpreter.
 *
 *    THE UNIT IS ONE TRANSITION. Say it out loud, because v1 did not and the mistake
 *    survived a whole metal session: TRANSITIONS increments in enter() AND in exit(), so
 *    the cost that multiplies it has to be the cost of ONE of them. v1 calibrated a
 *    PAIR, multiplied it by a per-transition count, and reported a probe overhead of
 *    82.65 % where the honest figure was 41.33 % -- an over-subtraction so large that it
 *    demanded more probe cycles out of LOOP than LOOP contained, and inverted the top of
 *    the ranking the instrument exists to produce. Two numbers of the right type and the
 *    wrong unit multiply to a plausible answer, and nothing in an arithmetic check ever
 *    objects. z3660_prof_calibrate_probe() now times a batch and divides by 2 x N.
 *
 *    AND IT IS PRICED OUT OF LINE, because that is how the interpreter pays for it.
 *    v1 calibrated with enter/exit INLINED into the calibration loop while the real
 *    accessors reach them through a `bl` (bias note 2b) -- so the calibrated figure was
 *    an under-estimate of the production cost by an unknown amount, and "unknown" was
 *    load-bearing: at 45.5 cyc/transition LOOP holds 9.82 % of guest time and at 58.24
 *    it holds none, which is the difference between a rung worth attacking and one that
 *    does not exist. The calibration now calls through __attribute__((noinline))
 *    wrappers, so what is timed is a real call to a real out-of-line body -- the same
 *    shape the accessors pay. It is still not identical to an accessor's call site (a
 *    different register pressure, a different branch-predictor history), so treat it as
 *    a good estimate rather than an exact price; it is no longer a KNOWN under-estimate,
 *    which is the part that mattered.
 *
 * 2b. AND THE CALLS ARE PAID EVEN WHEN THE BUCKETS ARE OFF. Counted out of the generated
 *    ARM, not estimated: every instrumented accessor makes THREE out-of-line calls --
 *    z3660_prof_enter, z3660_prof_count, z3660_prof_exit -- and it makes them whichever
 *    way PROFB is set, because PROFB gates the BODIES of enter and exit (51 and 44
 *    instructions) and not the calls to them. So the floor of this image's distortion is
 *    three calls per memory access, at roughly two accesses per emulated 68k instruction,
 *    paid with the sampler and the buckets both off.
 *
 *    ARM instructions are not ARM cycles -- the interpreter's own work stalls on memory
 *    and the probes do not -- so the time ratio is better than the instruction ratio by an
 *    amount only metal can state. NEVER quote an absolute instruction rate out of this
 *    image and never compare one to the lean figure. docs/profiler.md carries the
 *    four-point A/B (lean, profiling idle, sampling, buckets) that turns the static counts
 *    into percentages; until that is run, the overhead here is bounded, not known.
 *
 * 3. BUCKET TIME IS EXCLUSIVE, NEVER NESTED-DOUBLE-COUNTED. The phase machine charges the
 *    running bucket at every transition, so an MMU translate inside a data read shows up
 *    in XLATE and is NOT also in READ. Sum(buckets) == total measured cycles by
 *    construction, which is the invariant the host harness asserts.
 *
 * 4. A GUEST PAGE FAULT IS A C++ THROW and unwinds past every pending exit(). The run
 *    loop's CATCH therefore resets the phase stack and charges the abandoned span to
 *    FAULT. FAULT is thus "cycles from the last probe transition to the CATCH", which is
 *    the fault path plus a fragment of whatever stage was interrupted -- deliberately not
 *    split finer, because the split would be a guess.
 *
 * 5. PMCCNTR IS 32 BITS and wraps every 2^32 ARM cycles (~3.9 s at 1.1 GHz). Every span
 *    this profiler measures is bounded by a run-loop iteration, so modular 32-bit
 *    subtraction is exact for the buckets. The long-run cycle base is extended to 64 bits
 *    at each sample AND at every z3660_prof_poll_cmd() -- i.e. from do_specialties(),
 *    which a running guest reaches on every interrupt -- so the base stays fresh even
 *    with the sampler off, which is the case a bucket-only run puts it in. If core1 ever
 *    leaves the run loop for longer than one wrap the base still loses 2^32 cycles
 *    silently. That is why the dump reports BOTH the PMU span and the independent ARM
 *    global-timer (XTime) span: if they disagree by more than a percent, a wrap was
 *    missed and the cycle totals are short by a multiple of 2^32.
 *
 *    v2 STAMPS cyc_span AT DUMP TIME, not at the last sample. v1 reported the cycle base
 *    as it stood after the final sample while wall_ticks ran on to the dump, so the two
 *    spans covered different intervals and the "divergence over 1 % means a missed wrap"
 *    rule fired on the gap between the workload ending and a human typing PROFR. The
 *    firmware closes the gap itself now rather than asking the reader to allow for it.
 *
 * 6. THE ARM CLOCK IS TAKEN FROM THE RUNNING CONFIGURATION, AND THE RATIO IS MEASURED.
 *    These are two different claims and v1 conflated them, at a cost of 65 %.
 *
 *    What z3660_prof_calibrate() can actually measure is the RATIO of PMCCNTR to the ARM
 *    global timer. On this part that ratio is a FIXED 2:1 in hardware -- the CPU runs at
 *    CPU_6x4x and the global timer at CPU_3x2x -- so the measurement always returns ~2.0
 *    and carries no information about the absolute rate. The absolute rate has to come
 *    from somewhere else, and v1's somewhere-else was COUNTS_PER_SECOND: a BSP constant
 *    baked at XPAR_CPU_CORTEXA9_CORE_CLOCK_FREQ_HZ / 2 = 333 333 343, frozen at the
 *    666.67 MHz default. But arm_frequency is a CONFIG KEY and ps7_init_custom retunes
 *    the ARM PLL at boot -- to 1100 MHz on the C1 rig -- so the constant was wrong by
 *    1.65x and every Hz, every seconds figure and every actual sample rate in that
 *    session's captures was low by exactly that factor.
 *
 *    WHY NOTHING CAUGHT IT, which is the part worth remembering. The wrap cross-check in
 *    note 5 compares cyc_span against wall_ticks / wall_hz x cpu_hz. Because CPU:global
 *    timer is a fixed 2:1, cyc_span and wall_ticks scale TOGETHER and the erroneous
 *    constant cancels out of the ratio: the check passes, to three decimal places, while
 *    the clock is 65 % wrong. A WRAP CHECK IS NOT A CLOCK CHECK. The only tell was that
 *    the boot line said 666.67 MHz seven seconds after the board's own log said 1100.
 *
 *    So core0 -- which is where arm_frequency is resolved and where the PLL is actually
 *    retuned -- publishes the resulting global-timer rate in SHARED.arm_wall_hz, and this
 *    profiler scales its measured ratio by THAT. The header reports the rate and its
 *    source (clk_src / the `clk=` key), so a future stale-constant is visible in the
 *    capture instead of being inferred from a boot log two minutes earlier. If the
 *    mailbox says nothing the BSP constant is still the fallback, and the dump says
 *    `clk=bsp` when it is.
 *
 * ==========================================================================
 * WHAT DOES *NOT* RIDE THE DEFAULT BUILD
 * ==========================================================================
 *
 * Nothing here does. Every counter, probe and ring in this file is behind Z3660_EMU_PROF,
 * which only `make profile` sets. The hit-rate counters were the tempting exception --
 * upstream already has the branch scaffolding (CACHE_HIT_COUNT) and an increment looks
 * free. It is not: the C3b metal calibration priced the per-instruction 64-bit
 * read-modify-write of the perf accumulator at 2.0 % of guest throughput all by itself,
 * and a hit counter is the same shape of work on every memory access rather than every
 * instruction. Putting them in the default build would re-make the mistake C3b just
 * finished correcting. They stay in the profiling variant.
 *
 * ==========================================================================
 * WIRE FORMAT -- the symbolizer's contract (see docs/profiler.md)
 * ==========================================================================
 *
 * The firmware emits raw records; symbolization is the port lane's business. Both the
 * binary layout below and the ASCII serial rendering are versioned by Z3660_PROF_VERSION
 * and pinned by the host harness, so a symbolizer written against this header does not
 * have to re-read the firmware to stay correct.
 */
#ifndef SRC_UAE_Z3660_PROF_H_
#define SRC_UAE_Z3660_PROF_H_

#include <stdint.h>

/* 1 = the profiler is compiled in. 0 = it is not present at all -- no counters, no ring,
 * and (this is the point) no predicates on the hot path either. The default is 0 and is
 * load-bearing exactly as runloop_build.h's defaults are: an unflagged build, and the
 * lean build, must not change a single instruction because this file exists. */
#ifndef Z3660_EMU_PROF
#define Z3660_EMU_PROF 0
#endif

/* Bumped whenever a record layout, a bucket id, a counter id or the ASCII dump grammar
 * changes. A symbolizer must refuse a version it does not know rather than guess.
 *
 * ==========================================================================
 * VERSION 2 -- what changed, and why a v1 tool must refuse a v2 capture
 * ==========================================================================
 *
 * The C1 metal session took four captures with the v1 instrument and the C2 map audit
 * (amix-040-060-port/docs/C2-ATTACK-MAP.md) found three labelling defects in them. All
 * three are fixed here, and every one of them changes what a printed number MEANS
 * without changing its type -- which is precisely the failure a version number exists
 * to catch, because nothing else would.
 *
 *   1. probe_cyc IS NOW PER TRANSITION, NOT PER PAIR. v1 calibrated one enter+exit PAIR
 *      and then multiplied that figure by TRANSITIONS -- a count that increments on
 *      EACH of enter and exit. Every v1 capture therefore over-priced the instrument by
 *      exactly 2x, which was large enough to invert the bucket ranking (LOOP came out
 *      first at 26.35 % raw and lands fifth at 9.8 % corrected). v2 reports the cost of
 *      ONE transition and the multiplication is honest. The calibration also now runs
 *      the probes OUT OF LINE, which is how the interpreter actually pays for them.
 *
 *   2. cpu_hz AND wall_hz ARE NOW THE RUNTIME CLOCK. v1 scaled its measurement by
 *      COUNTS_PER_SECOND, a BSP constant frozen at the 666.67 MHz default, while
 *      ps7_init retunes the ARM PLL at boot -- on the C1 rig to 1100 MHz. Every v1 Hz
 *      and every v1 seconds figure is low by exactly 1.65 on that rig. See the clock
 *      section below for why the header's own wrap cross-check is structurally unable
 *      to notice this.
 *
 *   3. COUNTER 12 IS `XLATE_OK`, NOT `ATC_HIT`. It counts translates that SUCCEEDED,
 *      which is not an ATC lookup outcome at all, so `ATC_HIT + ATC_MISS` was a
 *      meaningless sum in v1. The genuine ATC-hit counter is new id 19.
 *
 * And one addition rather than a correction:
 *
 *   4. TAIL IS SPLIT. v1's `TAIL` (id 8) was the whole run-loop tail, 20 % of guest time
 *      with no internal breakdown -- the largest un-attackable share in the map. v2
 *      names its parts: id 8 is now `TAILADV` (the PC-advance / cycle-accounting
 *      residue) and ids 11/12/13 are the sampler hook, the interrupt poll and
 *      do_specialties(). The v1 whole-tail figure is TAILADV+TAILSAMP+TAILPOLL+TAILSPEC.
 *
 * Ids are APPEND-ONLY as always: nothing was renumbered, two were renamed to say what
 * they always counted, and four are new. The SAMPLE RECORD AND ITS FLAG BITS ARE
 * UNCHANGED -- a v1 symbolizer's per-sample decoding is still correct; it is the header
 * and the PROFD block that moved.
 */
#define Z3660_PROF_VERSION      2u

/* 'Z3P' + version. Little-endian on the wire, because the ARM writes it and the host
 * reads it; the ASCII dump spells it out as a string so a serial capture is greppable.
 *
 * The magic tracks the version deliberately. A symbolizer that checks only the magic
 * (the cheap grep) still refuses a capture whose grammar it does not know, instead of
 * parsing v2 numbers with v1 meanings and printing a plausible wrong answer. */
#define Z3660_PROF_MAGIC        0x5A335032u   /* "Z3P2" */
#define Z3660_PROF_MAGIC_STR    "Z3P2"        /* the ASCII dump's spelling of the above */

/* Ring capacity, in samples. 64K x 8 B = 512 KB and it lives in core1's .bss, which the
 * FSBL zeroes and which contributes NOTHING to BOOT.BIN -- so the ring costs nothing in
 * the image at all. At 1 kHz it holds 65 s of history; the limit on a profiling session
 * is the serial dump, not this. Must stay a power of two: the head advances with a mask,
 * not a modulo.
 *
 * SIZE THE CAPTURE AGAINST THE RING, NOT AGAINST THE CLOCK YOU MEANT TO SET. Both C1
 * workload rings wrapped and retained a tail that landed AFTER the benchmark had
 * finished, on the scheduler idle loop -- 86 % and 93 % of their samples overwritten --
 * so two of the three captures described an idle machine and had to be discarded. The
 * arithmetic was done against the nominal rate, which the v1 clock defect made 1.65x
 * too slow; a ring believed to hold 65 s held 40. With v2's true clock the nominal
 * numbers are the real ones again, but the rule stands on its own: the wrap-free window
 * is RING_ENTRIES / hz seconds, arm at the LOWEST rate that still resolves the question,
 * and confirm `drops=0` in the header before believing a share. */
#define Z3660_PROF_RING_ENTRIES 65536u
#define Z3660_PROF_RING_MASK    (Z3660_PROF_RING_ENTRIES - 1u)

/* Sample-rate bounds. The floor exists so the tick period stays well inside PMCCNTR's
 * 2^32-cycle wrap (bias note 5); the ceiling is where the poll starts distorting what it
 * measures. 0 means "sampler off" and is not a rate. */
#define Z3660_PROF_HZ_MIN       1u
#define Z3660_PROF_HZ_MAX       20000u
#define Z3660_PROF_HZ_DEFAULT   1000u

/* ---------------------------------------------------------------------------
 * Sample record -- 8 bytes, and the reason it is 8 and not 12.
 *
 * A 512 KB ring is free; a serial dump is not. Eight bytes renders as 20 printable
 * characters, so the newest 4096 samples cost ~90 KB of serial, about 8 s at 115200.
 * Everything that is not per-sample (rates, spans, totals) lives in the header instead of
 * being repeated 65536 times.
 * --------------------------------------------------------------------------- */
typedef struct {
   uint32_t pc;       /* guest PC of the instruction that was executing at the tick     */
   uint16_t opcode;   /* its opcode word, as fetched (0 if the fetch itself faulted)    */
   uint16_t flags;    /* Z3660_PROF_F_* below                                           */
} z3660_prof_sample_t;

#define Z3660_PROF_REC_SIZE     8u   /* sizeof(z3660_prof_sample_t); pinned by the harness */

/* flags, bit by bit. Bits 4..7 are reserved and MUST be written zero: a symbolizer that
 * masks them off today keeps working when they are given a meaning. */
#define Z3660_PROF_F_SUPER      0x0001u  /* SR.S -- supervisor mode                      */
#define Z3660_PROF_F_MMU        0x0002u  /* regs.mmu_enabled -- translation was on       */
#define Z3660_PROF_F_AMIX       0x0004u  /* the AMIX memory contract was armed           */
#define Z3660_PROF_F_CPU040     0x0008u  /* 1 = 68040 run loop, 0 = 68030 run loop       */
#define Z3660_PROF_F_RSVD_MASK  0x00F0u  /* reserved, always 0 in version 1              */
#define Z3660_PROF_F_WEIGHT_SH  8u
#define Z3660_PROF_F_WEIGHT_MAX 255u
/* WEIGHT: how many tick periods this one sample stands for, 1..255, saturating. 1 is the
 * ordinary case. Anything above 1 means the sampler could not fire for that long -- see
 * bias note 1. A symbolizer MUST weight each sample by this field; treating every sample
 * as 1 silently deletes exactly the stalls a profiler exists to find. */
#define Z3660_PROF_F_WEIGHT(f)  (((f) >> Z3660_PROF_F_WEIGHT_SH) & 0xFFu)

/* ---------------------------------------------------------------------------
 * Ring header -- 64 bytes, emitted once before the records.
 * --------------------------------------------------------------------------- */
typedef struct {
   uint32_t magic;         /* Z3660_PROF_MAGIC                                        */
   uint32_t version;       /* Z3660_PROF_VERSION                                      */
   uint32_t rec_size;      /* Z3660_PROF_REC_SIZE -- so a reader can skip unknown recs*/
   uint32_t rec_count;     /* records that follow this header                         */
   uint32_t ring_entries;  /* Z3660_PROF_RING_ENTRIES                                 */
   uint32_t hz;            /* nominal sample rate the ring was filled at              */
   uint32_t period_cyc;    /* that rate expressed in ARM cycles (cpu_hz / hz)         */
   uint32_t cpu_hz;        /* ARM clock MEASURED at arm time, not configured          */
   uint64_t samples;       /* samples taken since zero; > rec_count if the ring wrapped*/
   uint64_t drops;         /* samples overwritten by wrap = samples the dump lost      */
   uint64_t cyc_span;      /* PMU cycles from arm to dump                             */
   uint64_t wall_ticks;    /* ARM global-timer ticks over the same span (wrap check)   */
   uint32_t wall_hz;       /* global-timer rate -- RUNTIME in v2, not the BSP constant */
   uint32_t build_flags;   /* Z3660_PROF_BUILD_* -- which variant produced this        */
   uint32_t probe_cyc;     /* v2: ARM cycles per ONE transition (v1 reported a PAIR)   */
   uint32_t clk_src;       /* v2: Z3660_PROF_CLK_* -- where wall_hz came from          */
} z3660_prof_hdr_t;

/* Where the absolute clock came from. This is reported rather than assumed because the
 * v1 instrument got it wrong silently for a whole metal session: it is exactly the kind
 * of fact whose absence looks identical to its presence. */
#define Z3660_PROF_CLK_BSP      0u   /* the compile-time BSP constant -- STALE if the
                                      * firmware retuned the ARM PLL, which it does     */
#define Z3660_PROF_CLK_CFG      1u   /* the rate core0 published after retuning it      */

/* Which absolute rate to believe, and what to call it. A rule rather than a measurement,
 * so it lives in the header where the host harness can test every branch -- the
 * measurement wrapped around it needs a running global timer and cannot be tested at all.
 *
 * `published` is SHARED.arm_wall_hz. Anything below 1 MHz is not a clock rate, it is an
 * unwritten mailbox word (a core0 from before this field existed, or the host harness),
 * and the BSP constant takes over with clk_src saying so. The threshold is deliberately
 * far below any real Zynq global-timer rate -- the slowest supported ARM frequency here
 * is 667 MHz, i.e. a 333 MHz timer -- so it separates "absent" from "plausible" rather
 * than trying to validate the value. */
#ifdef __cplusplus
extern "C" {
#endif
static __inline__ uint32_t z3660_prof_pick_wall_hz(uint32_t published, uint32_t bsp,
                                                   uint32_t *src)
{
   if (published >= 1000000u) {
      if (src) *src = Z3660_PROF_CLK_CFG;
      return published;
   }
   if (src) *src = Z3660_PROF_CLK_BSP;
   return bsp;
}
#ifdef __cplusplus
}
#endif

/* 80 and not a rounder-looking 64: the four uint64_t fields force 8-byte alignment, so
 * eight leading uint32_t put them at offset 32 and the struct ends at 80 on both ARM EABI
 * and x86-64. Pinned by a compile-time assertion in z3660_prof.cpp AND by the host
 * harness, because this is a wire format read by a symbolizer that will never be
 * rebuilt in step with the firmware.
 *
 * v2 spent the two `reserved` words rather than resizing, which is what they were put
 * there for. The struct is still 80 bytes. */
#define Z3660_PROF_HDR_SIZE     80u

/* build_flags: the low two bits mirror runloop_build.h's, so one word identifies the
 * binary completely. A profiling build of the DEFAULT loop is a legal thing to compile
 * and would report 0x07 -- and must not be mistaken for the shipped combination. */
#define Z3660_PROF_BUILD_PROBES 0x01u
#define Z3660_PROF_BUILD_PERF   0x02u
#define Z3660_PROF_BUILD_PROF   0x04u

/* ---------------------------------------------------------------------------
 * Buckets. Exclusive ARM cycles per interpreter stage.
 *
 * Order is the wire order of the dump and is APPEND-ONLY: bucket ids appear in the ASCII
 * dump and any reordering silently relabels a previous capture.
 * --------------------------------------------------------------------------- */
enum {
   Z3660_PROF_B_LOOP    = 0,  /* run-loop bookkeeping outside every named stage        */
   Z3660_PROF_B_FETCHOP = 1,  /* the run loop's own opcode fetch (x_prefetch)          */
   Z3660_PROF_B_FETCHEX = 2,  /* extension / EA words fetched by the handler           */
   Z3660_PROF_B_READ    = 3,  /* data reads, excluding translation                     */
   Z3660_PROF_B_WRITE   = 4,  /* data writes, excluding translation                    */
   Z3660_PROF_B_XLATE   = 5,  /* mmu_translate/ATC search, excluding the table walk    */
   Z3660_PROF_B_WALK    = 6,  /* the descriptor table walk itself                      */
   Z3660_PROF_B_HANDLER = 7,  /* handler body remainder (dispatch minus the above)     */
   Z3660_PROF_B_TAIL    = 8,  /* v2 "TAILADV": the tail RESIDUE -- see the split below */
   Z3660_PROF_B_FAULT   = 9,  /* last transition -> the run loop's CATCH (see bias 4)  */
   Z3660_PROF_B_PROF    = 10, /* the profiler's own dump work, kept out of the rest    */
   /* --- v2: the tail split. The three below NEST INSIDE id 8, so id 8 keeps only the
    * residue and the whole tail is the sum of all four. ------------------------------ */
   Z3660_PROF_B_TAILSAMP = 11, /* the profiler's OWN sampler hook + INSNS counters     */
   Z3660_PROF_B_TAILPOLL = 12, /* the service-cadence interrupt poll                   */
   Z3660_PROF_B_TAILSPEC = 13, /* do_specialties(): trace / STOP / mode change         */
   Z3660_PROF_B_COUNT   = 14
};

/* WHY THE TAIL IS SPLIT, AND WHAT ID 8 NOW MEANS.
 *
 * The C2 map measured the tail at 110.8 ARM cycles per guest instruction -- the most
 * disproportionate figure in the whole table, for what ought to be a PC advance and a
 * cheap event test -- and could not act on it, because a 20 % bucket with no internal
 * breakdown is a 20 % bucket nobody can attack safely. Naming the parts is the cheapest
 * measurement that turns it into a target.
 *
 * The four parts, in the order the tail runs them:
 *
 *   id  8  TAILADV   the residue: adjust_cycles() + the cadence counter's own test +
 *                    whatever the loop does between the handler and the next fetch.
 *                    This is the "PC advance" share.
 *   id 11  TAILSAMP  the profiler's sampler tick and the INSNS/INSNS_SUPER counters.
 *                    THIS IS INSTRUMENT COST, NOT INTERPRETER COST. It is bucketed
 *                    separately precisely so it can be subtracted outright rather than
 *                    quietly inflating the tail -- in v1 it was charged to LOOP, so no
 *                    v1 capture could see it at all.
 *   id 12  TAILPOLL  check_uae_int_request() when the service cadence lets it through
 *                    (and, on the DEFAULT build only, the perf accumulator).
 *   id 13  TAILSPEC  do_specialties(), entered only when regs.spcflags != 0 -- so this
 *                    is rare per instruction and is the trace/STOP/mode-change share.
 *
 * COST OF THE SPLIT, stated rather than discovered: two of the three sub-stages fire on
 * EVERY instruction, so the split adds four phase transitions per instruction to the
 * eight-to-fourteen already there. The instrument therefore distorts MORE in v2 than in
 * v1. That is the trade -- the shares inside the tail are unobtainable any other way,
 * and shares survive the distortion (bias note 2), which is the whole reason the
 * transition count is reported.
 *
 * The id-8 RENAME is deliberate and is the lesson of the ATC_HIT defect: a bucket whose
 * name does not say what it counts is a trap that survives every test. v1's `TAIL` was
 * the whole tail; v2's id 8 is the residue. Rather than leave one name covering two
 * meanings across a version boundary, the name moves with the meaning -- and because
 * the name tables are emitted in every dump, a capture says which it is. */

/* ---------------------------------------------------------------------------
 * Counters. All 64-bit: a 32-bit counter overflows in ~7 minutes of hot interpretation,
 * which is shorter than the boot profile this is meant to take.
 *
 * Order is APPEND-ONLY, same reason as the buckets.
 * --------------------------------------------------------------------------- */
enum {
   Z3660_PROF_C_INSNS       = 0,  /* instructions retired through the run-loop tail    */
   Z3660_PROF_C_INSNS_SUPER = 1,  /* ... of those, with SR.S set                       */
   Z3660_PROF_C_FETCH       = 2,  /* instruction-stream fetches (opcode + extension)   */
   Z3660_PROF_C_READ        = 3,  /* data reads                                        */
   Z3660_PROF_C_WRITE       = 4,  /* data writes                                       */
   Z3660_PROF_C_IPAGE_HIT   = 5,  /* MMU_IPAGECACHE  one-entry instruction page cache  */
   Z3660_PROF_C_IPAGE_MISS  = 6,
   Z3660_PROF_C_DPAGE_RHIT  = 7,  /* MMU_DPAGECACHE  256-entry data page cache, read   */
   Z3660_PROF_C_DPAGE_RMISS = 8,
   Z3660_PROF_C_DPAGE_WHIT  = 9,  /* ... write                                         */
   Z3660_PROF_C_DPAGE_WMISS = 10,
   Z3660_PROF_C_XLATE       = 11, /* mmu_translate entries == page-cache misses        */
   Z3660_PROF_C_XLATE_OK    = 12, /* ... that RETURNED an address (v1 called this      *
                                   *     ATC_HIT, which it never was -- see below)     */
   Z3660_PROF_C_ATC_MISS    = 13, /* ... that had to walk the tables                   */
   Z3660_PROF_C_MISALIGN_R  = 14, /* misaligned reads split into aligned sub-accesses  */
   Z3660_PROF_C_MISALIGN_W  = 15, /* ... writes                                        */
   Z3660_PROF_C_FAULTS      = 16, /* run-loop CATCH entries                            */
   Z3660_PROF_C_TRANSITIONS = 17, /* phase transitions -- multiply by PROBE_CYC        */
   Z3660_PROF_C_STACK_OVF   = 18, /* phase-stack overflows; MUST be 0                  */
   Z3660_PROF_C_ATC_HIT     = 19, /* v2: translates satisfied WITHOUT a table walk     */
   Z3660_PROF_C_IFETCH_CALLS= 20, /* entries into the shared FETCHOP/FETCHEX bracket   */
   Z3660_PROF_C_BLK_HIT     = 21, /* block-idiom fast-path CHUNKS serviced             */
   Z3660_PROF_C_BLK_INSNS   = 22, /* ... guest instructions those chunks retired       */
   Z3660_PROF_C_BLK_BYTES   = 23, /* ... bytes zeroed / copied / flushed by them       */
   Z3660_PROF_C_COUNT       = 24
};

/* THE THREE `BLK_` COUNTERS ARE THE UTILISATION VERDICT, and they are three rather than
 * one because each answers a different way for the change to disappoint.
 *
 * `BLK_INSNS / INSNS` is the share of the guest instruction stream the recognizer
 * actually took. The map predicts 22.07 % of an AMIX boot for stage A alone and 31.80 %
 * for A+B+C; a metal capture that shows 2 % is a recognizer that is not firing where the
 * profile says the time is, and the map is explicit that the first thing to check then is
 * the TRIGGER, not the host operation.
 *
 * `BLK_INSNS / BLK_HIT` is the mean chunk length, and it is what separates "firing" from
 * "firing usefully". The fast path pays its recognizer test once per chunk, so a mean of
 * 2 means the test is being paid per iteration and the whole saving is gone -- the exact
 * failure mode §11.2 pre-registers for the decoded-op cache, arriving here first. Against
 * a 256 chunk cap and 1024-longword page fills, healthy is a mean in the high hundreds
 * once the ramp-in is amortised, and never below ~8.
 *
 * `BLK_BYTES` is the one that survives a change of idiom mix. It is comparable across
 * workloads and across future stages in a way an instruction count is not, and it is what
 * a "kernel zero-fill runs at N MB/s" figure is computed from -- the map's 5.17 MB/s
 * being the number this whole rung exists to move.
 *
 * They are counters and not buckets on purpose. A bucket would need a phase transition
 * around the fast path, and the fast path's entire value is that it does not pay
 * per-instruction machinery; measuring it with the machinery it removes would be the
 * §10.1 distortion applied to the one place it would be worst. Appended ids, no version
 * bump -- see IFETCH_CALLS above for that reasoning, which is the same here. */

/* WHY `IFETCH_CALLS` EXISTS, AND WHY IT IS THE ONE COUNTER WORTH ADDING.
 *
 * The C2 map's landing model reproduces the firmware's own `TRANSITIONS` count and then
 * distributes the measured probe cost across the buckets by that shape. Under v1 it
 * predicted the count to 12 parts per million -- the single strongest piece of evidence
 * in v1. Under v2 it overshoots by +10.25 %, which is inside the tool's tolerance but is
 * a four-order-of-magnitude degradation, and the overshoot is 1.166 transitions per guest
 * instruction: suspiciously close to "one bracket that is entered fewer times than the
 * model thinks".
 *
 * The map (§10.3b) tested two explanations against this source and refuted both: neither
 * TAILSAMP nor TAILPOLL is cadence-gated, so the tail split is not where the shortfall
 * is. The remaining candidate is right here. `FETCHOP` and `FETCHEX` share ONE bracket
 * (Z3660_PROF_ENTER_IFETCH, with a hint flag choosing the bucket), entered once per
 * accessor CALL -- while the model derives their entry counts from `FETCH`, which counts
 * WORDS. A longword extension fetch is one call and two words, and would produce exactly
 * this shape.
 *
 * That is a hypothesis and nothing in a v2 capture could distinguish it from the
 * alternatives, because the quantity it turns on -- calls -- was never measured. So
 * measure it.
 *
 * AND THE SOURCE ALREADY ANSWERS IT, WHICH IS WORTH SAYING BEFORE THE FIRST CAPTURE
 * COMES BACK. There are exactly five ENTER_IFETCH sites in this tree -- two in
 * include/cpummu.h (uae_mmu040_get_iword, uae_mmu040_get_ilong) and three in
 * include/cpummu030.h (uae_mmu030_get_ilong/_iword/_ibyte) -- and every one of them is
 * followed IMMEDIATELY by a single Z3660_PROF_CNT(Z3660_PROF_C_FETCH). One bracket entry,
 * one FETCH increment, at all five. So `FETCH` has never counted words: it counts the
 * same accessor CALLS the bracket counts, and a longword instruction fetch increments it
 * once, not twice. `IFETCH_CALLS == FETCH` identically.
 *
 * So the ifetch reconciliation (map §10.3, recon-B) is REFUTED by this source, in the
 * same way and for the same reason the two cadence-gating candidates before it were: the
 * firmware does not do the thing the model needed it to do. The model's shortfall is
 * elsewhere and is still open. Note the direction this closes off: recon-B was the column
 * that moved rung 1's share UP by 4.3 points, so removing it narrows the bracket rather
 * than raising the estimate.
 *
 * The counter still ships, for three reasons that outlive the question that prompted it.
 * It turns a reading of this file into an identity a capture ASSERTS. It makes the
 * ifetch bucket's entry count directly available to the landing model instead of derived.
 * And on the day the two figures diverge, a fetch site was added to one path and not the
 * other -- which is exactly the drift no reviewer would catch.
 *
 * THE VERSION IS DELIBERATELY NOT BUMPED. The header's rule is to bump when a record
 * layout, a bucket id, a counter id or the dump grammar changes -- and v2 bumped for two
 * RENAMES and a changed probe_cyc unit, i.e. for ids whose MEANING moved under a reader's
 * feet. A pure APPEND moves nothing: every id 0..19 still counts exactly what a v2
 * symbolizer believes it counts, the binary header layout is untouched, and the ASCII
 * counter line carries its own name. The host symbolizer already handles an id past the
 * end of its table by reporting it uninterpreted rather than by mis-labelling it, which
 * is the behaviour that makes appends safe. Bumping to v3 would instead make every
 * existing v2 tool REFUSE a capture that it can read correctly. */

/* THE `ATC_HIT` DEFECT, AND THE THREE IDENTITIES THAT REPLACE IT.
 *
 * v1's id 12 was named ATC_HIT and was incremented on mmu_translate()'s success return.
 * On the 68040 that return is reached BOTH by an ATC hit AND, via `goto atc_retry`, by
 * every translate that had just walked the descriptor tables -- so it counted "the
 * translate returned an address", carried no ATC information whatsoever, and made
 * `ATC_HIT + ATC_MISS` a sum of a thing and part of itself. The C2 audit caught it the
 * only way it could be caught, by noticing that `ATC_HIT + FAULTS == XLATE` held exactly
 * in four independent seven-digit spans.
 *
 * So id 12 is renamed to what it always counted, and a real ATC-hit counter is appended:
 *
 *   XLATE      calls into mmu_translate  ==  tier-0 page-cache misses
 *   XLATE_OK   ... that returned an address        (id 12, renamed, same site)
 *   ATC_MISS   ... that walked the descriptor tables            (id 13, unchanged)
 *   ATC_HIT    ... that returned WITHOUT walking                (id 19, new)
 *
 * Useful identities, all checkable in a dump:
 *
 *   IPAGE_MISS + DPAGE_RMISS + DPAGE_WMISS  ==  XLATE     (tier 0 feeds tier 1)
 *   XLATE_OK + <translates that faulted>    ==  XLATE
 *   ATC_HIT + ATC_MISS                      ==  XLATE     whenever no translate faulted,
 *                                                         and short by exactly the ones
 *                                                         that did otherwise
 *   true ATC hit rate = ATC_HIT / XLATE     walk rate = ATC_MISS / XLATE
 *
 * The walk rate the v1 firmware printed (ATC_MISS / XLATE) was always correct; it was
 * the hit counter beside it that was not.
 *
 * 68030 CAVEAT, and it is not the same caveat as the tier-0 one. On the 030 path the
 * ATC_HIT site is a GENUINE ATC hit and always was, so id 19 is right there for free.
 * But `XLATE_OK` has no single site to hang on -- the 030 translate is a bare
 * `mmu030_logical_is_in_atc()` called from fourteen accessors -- so it is NOT counted on
 * the 030 and reads zero. Absent, not zero-valued by accident. Worse, and worth knowing
 * before comparing an 030 capture to an 040 one: the 030 miss path calls
 * mmu030_logical_is_in_atc() TWICE (once to fail, once after the walk), so an 030 `XLATE`
 * over-counts tier-0 misses by one for every walk. The 030 rates are internally
 * consistent and are NOT comparable to the 040's. */

/* Name tables. Deliberately UNSIZED so a missing name is a compile error rather than a
 * silently NULL-padded array -- the same tripwire config_item_names[] needed. */
#ifdef __cplusplus
extern "C" {
#endif
extern const char * const z3660_prof_bucket_names[];
extern const char * const z3660_prof_counter_names[];
#ifdef __cplusplus
}
#endif

#if Z3660_EMU_PROF

/* ---------------------------------------------------------------------------
 * Cycle source.
 *
 * ARM: the PMU cycle counter, one MRC, no bus access -- which is the whole reason the
 * sampler can afford to be polled per emulated instruction. The ARM global timer would
 * also work and is 64-bit, but it is a load from the SCU region and costs an order of
 * magnitude more; it is used here only for the wrap cross-check and the clock
 * calibration, where it runs once per second rather than 5 million times.
 *
 * Host: an injectable counter, so the harness can hand the phase machine exact cycle
 * values and assert exact bucket totals. Bucket arithmetic that is only ever exercised
 * against a real clock cannot be tested, only observed.
 * --------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

#if defined(__arm__) || defined(__thumb__)
static __inline__ uint32_t z3660_prof_cycles(void)
{
   uint32_t v;
   __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
   return v;
}
#else
extern uint32_t z3660_prof_host_cycles;      /* host harness: set it, then call */
/* ...and, for the one thing a clock you set by hand cannot express, a clock that MOVES.
 *
 * z3660_prof_host_step is added to the counter on every read, so a host test can give
 * the probe itself a known price and then check what the calibration makes of it. It
 * defaults to 0, which is exactly the frozen counter every other test relies on -- the
 * bucket-arithmetic tests set the clock to literal values and assert literal spans, and
 * they must stay that way.
 *
 * This exists because the alternative was a defect that no test could see: v1's
 * calibration divided a batch of enter/exit PAIRS by the number of pairs and reported
 * the result as a per-TRANSITION cost, and every host test still passed, because on a
 * frozen clock the measured span is zero and zero divided by anything is zero. A unit
 * error in a measurement is invisible to a harness that never lets the measurement
 * measure anything. */
extern uint32_t z3660_prof_host_step;
static __inline__ uint32_t z3660_prof_cycles(void)
{
   uint32_t v = z3660_prof_host_cycles;
   z3660_prof_host_cycles += z3660_prof_host_step;
   return v;
}
/* Host harness only: re-run the probe calibration against the current host clock.
 * z3660_prof_init() is idempotent and has long since run by the time a test wants this. */
void z3660_prof_recalibrate_probe_for_test(void);
#endif

/* Phase-machine state. Extern rather than accessor-wrapped because these are touched on
 * the hot path and an out-of-line accessor would cost more than the measurement. */
#define Z3660_PROF_STACK_DEPTH 16
extern int      z3660_prof_bucket_on;                       /* the PROFB switch          */
extern int      z3660_prof_cur;                             /* bucket currently charged  */
extern uint32_t z3660_prof_last;                            /* stamp of the last change  */
extern int      z3660_prof_sp;
extern uint8_t  z3660_prof_stack[Z3660_PROF_STACK_DEPTH];
extern uint64_t z3660_prof_acc[Z3660_PROF_B_COUNT];
extern uint64_t z3660_prof_cnt[Z3660_PROF_C_COUNT];

/* Sampler state. */
extern int      z3660_prof_sample_on;                       /* the PROFS switch          */
extern uint32_t z3660_prof_deadline;                        /* next tick, in PMU cycles  */
extern uint32_t z3660_prof_period;                          /* tick period, in PMU cycles*/

extern void z3660_prof_sample_slow(uint32_t pc, uint16_t opcode, uint16_t flags);

/* enter/exit: charge the elapsed span to whatever bucket is running, then switch.
 *
 * The subtraction is 32-bit modular ON PURPOSE -- PMCCNTR wraps and every span measured
 * here is bounded by one run-loop iteration, so the wrap cancels exactly. Widening it
 * would cost a second read and buy nothing.
 *
 * On stack overflow the transition is DROPPED (the bucket does not change) and counted.
 * Dropping keeps the stack and the accumulator consistent; the alternative -- clamping
 * the index -- would corrupt the pop order and mis-attribute every later stage. Depth 16
 * against a real maximum nesting of four (handler -> accessor -> translate -> walk) makes
 * this unreachable, and the harness asserts the counter stays zero. */
static __inline__ void z3660_prof_enter(int bucket)
{
   if (!z3660_prof_bucket_on) return;
   {
      uint32_t now = z3660_prof_cycles();
      z3660_prof_acc[z3660_prof_cur] += (uint64_t)(uint32_t)(now - z3660_prof_last);
      z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS]++;
      if (z3660_prof_sp >= Z3660_PROF_STACK_DEPTH) {
         z3660_prof_cnt[Z3660_PROF_C_STACK_OVF]++;
      } else {
         z3660_prof_stack[z3660_prof_sp++] = (uint8_t)z3660_prof_cur;
         z3660_prof_cur = bucket;
      }
      z3660_prof_last = now;
   }
}

static __inline__ void z3660_prof_exit(void)
{
   if (!z3660_prof_bucket_on) return;
   {
      uint32_t now = z3660_prof_cycles();
      z3660_prof_acc[z3660_prof_cur] += (uint64_t)(uint32_t)(now - z3660_prof_last);
      z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS]++;
      if (z3660_prof_sp > 0)
         z3660_prof_cur = (int)z3660_prof_stack[--z3660_prof_sp];
      z3660_prof_last = now;
   }
}

/* Unwind recovery. A guest page fault is a C++ throw: every pending z3660_prof_exit()
 * between the faulting accessor and the run loop's CATCH is skipped. The CATCH calls this
 * to charge the abandoned span to FAULT and put the stack back at the floor. */
static __inline__ void z3660_prof_unwound(void)
{
   if (!z3660_prof_bucket_on) return;
   {
      uint32_t now = z3660_prof_cycles();
      z3660_prof_acc[Z3660_PROF_B_FAULT] += (uint64_t)(uint32_t)(now - z3660_prof_last);
      z3660_prof_sp  = 0;
      z3660_prof_cur = Z3660_PROF_B_LOOP;
      z3660_prof_last = now;
   }
}

/* Run-loop entry. do_specialties() can return non-zero and take the run loop out from
 * under a pending exit() (a mode change), and the loop is re-entered later; without this
 * the stack would deepen by one every time. Charges nothing -- the span since the last
 * transition belongs to whatever ran between the two loop invocations, which is not a
 * stage of this interpreter and must not be attributed to one. */
static __inline__ void z3660_prof_loop_reset(void)
{
   z3660_prof_sp   = 0;
   z3660_prof_cur  = Z3660_PROF_B_LOOP;
   z3660_prof_last = z3660_prof_cycles();
}

static __inline__ void z3660_prof_count(int c) { z3660_prof_cnt[c]++; }

/* One-shot mark: the instruction-stream fetch that follows is the run loop's OPCODE
 * fetch, not a handler's extension word. They arrive at the same accessor -- cpuemu_31
 * fetches extension words with get_iword_mmu040(N), never next_iword_mmu040 -- so the
 * only place that can tell them apart is the run loop, and it says so here. */
extern int z3660_prof_opfetch_hint;

/* The sampler poll. Inline and deliberately tiny: one coprocessor read, one modular
 * compare, one branch that is taken once every few thousand instructions. Everything else
 * -- weight arithmetic, the ring write, the 64-bit cycle base -- is out of line in
 * z3660_prof_sample_slow().
 *
 * The modular compare is `(now - deadline) < 2^31` rather than `now >= deadline` so it
 * stays correct across PMCCNTR's wrap without a 64-bit read. */
static __inline__ void z3660_prof_tick(uint32_t pc, uint16_t opcode, uint16_t flags)
{
   if (!z3660_prof_sample_on) return;
   if ((uint32_t)(z3660_prof_cycles() - z3660_prof_deadline) < 0x80000000u)
      z3660_prof_sample_slow(pc, opcode, flags);
}

#ifdef __cplusplus
}
#endif

/* Hot-path macros.
 *
 * The idiom exists so the NON-profiling expansion is textually the ORIGINAL code, not a
 * predicate that happens to be false. Default and lean must stay byte-identical to the
 * build before this file existed, and the cheapest way to be sure of that is for the
 * preprocessor to hand gcc exactly what it handed it before.
 *
 * Z3660_PROF_RET_* evaluate the accessor's work first, then close the bucket, then
 * return -- so the cycles land in the bucket that did the work, not the caller's. */
#define Z3660_PROF_ENTER(b)     z3660_prof_enter(b)
#define Z3660_PROF_EXIT()       z3660_prof_exit()
#define Z3660_PROF_CNT(c)       z3660_prof_count(c)

/* Z3660_PROF_RET: evaluate the accessor's work, THEN close the bucket, THEN return.
 *
 * The temporary is not a stylistic nicety, it is the entire point. The obvious spelling
 * -- `do { z3660_prof_exit(); return (expr); } while (0)` -- is what this macro said on
 * its first build, and it is wrong in a way no test noticed and no reading caught: C
 * sequences the exit() call BEFORE the expression, so every read and every instruction
 * fetch closed its bucket and then did the work, charging all of it to the enclosing
 * HANDLER. READ and FETCHOP/FETCHEX would have reported near zero, HANDLER would have
 * absorbed the lot, and the resulting map would have been confidently, silently wrong --
 * which is the worst failure a measuring instrument has, because the number still looks
 * like a number. It was found by reading the generated ARM, where enter/count/exit sat in
 * three adjacent `bl`s at the top of the function with the memory access after them.
 * `test_prof_ret_macro_ordering` in the host harness now fails if it ever comes back.
 *
 * decltype is available: the whole tree compiles as C++11. The __typeof__ arm is there so
 * a future C consumer of this header cannot silently fall back to the broken ordering. */
#ifdef __cplusplus
#define Z3660_PROF_RET(expr) \
   do { decltype(expr) z3660_rv_ = (expr); z3660_prof_exit(); return z3660_rv_; } while (0)
#else
#define Z3660_PROF_RET(expr) \
   do { __typeof__(expr) z3660_rv_ = (expr); z3660_prof_exit(); return z3660_rv_; } while (0)
#endif
/* The void form was correct from the start -- `stmt` genuinely precedes the exit -- and is
 * spelled out here next to its sibling so the asymmetry is visible rather than surprising. */
#define Z3660_PROF_RETV(stmt)   do { stmt; z3660_prof_exit(); return; } while (0)
#define Z3660_PROF_LOOP_RESET() z3660_prof_loop_reset()
#define Z3660_PROF_UNWOUND()    z3660_prof_unwound()
/* Add N at once. For an event that services many guest instructions in one go (the C2
 * block-idiom fast path), N increments would price the counter at more than the work it
 * is counting. */
#define Z3660_PROF_CNT_ADD(c, n) do { z3660_prof_cnt[(c)] += (uint64_t)(n); } while (0)
#define Z3660_PROF_MARK_OPFETCH() do { z3660_prof_opfetch_hint = 1; } while (0)
/* Opcode fetch and extension fetch share one accessor; the mark decides which bucket.
 *
 * IFETCH_CALLS counts entries into THIS bracket -- calls, not words. The C2 landing model
 * derives the two ifetch buckets' entry counts from `FETCH`, which counts words, and the
 * gap between the two is the leading candidate for the model's +10.25 % transition
 * overshoot. See the counter enum for why that matters and which way it moves rung 1. */
#define Z3660_PROF_ENTER_IFETCH()                                             \
   do {                                                                       \
      int z3660_b_ = z3660_prof_opfetch_hint ? Z3660_PROF_B_FETCHOP           \
                                             : Z3660_PROF_B_FETCHEX;          \
      z3660_prof_opfetch_hint = 0;                                            \
      z3660_prof_enter(z3660_b_);                                             \
      z3660_prof_count(Z3660_PROF_C_IFETCH_CALLS);                            \
   } while (0)

#else /* !Z3660_EMU_PROF -- the default and lean builds see none of the above */

#define Z3660_PROF_ENTER(b)     do { } while (0)
#define Z3660_PROF_EXIT()       do { } while (0)
#define Z3660_PROF_CNT(c)       do { } while (0)
#define Z3660_PROF_CNT_ADD(c,n) do { } while (0)
#define Z3660_PROF_RET(expr)    return (expr)
#define Z3660_PROF_RETV(stmt)   do { stmt; return; } while (0)
#define Z3660_PROF_LOOP_RESET() do { } while (0)
#define Z3660_PROF_UNWOUND()    do { } while (0)
#define Z3660_PROF_MARK_OPFETCH() do { } while (0)
#define Z3660_PROF_ENTER_IFETCH() do { } while (0)

#endif /* Z3660_EMU_PROF */

/* ---------------------------------------------------------------------------
 * Engine API. Declared unconditionally (the serial console and the harness link against
 * it in every variant); the non-profiling build gets inert stubs from z3660_prof.cpp so
 * a caller never has to be #if-ed.
 * --------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

void     z3660_prof_init(void);                 /* idempotent: PMU on, clock calibrated  */
void     z3660_prof_arm(unsigned hz);           /* zero everything and start at hz       */
void     z3660_prof_stop(void);                 /* stop both instruments, keep the data  */
void     z3660_prof_zero(void);                 /* zero buckets, counters and the ring   */
void     z3660_prof_set_buckets(int on);
void     z3660_prof_dump_stats(void);           /* buckets + counters, ~30 serial lines  */
void     z3660_prof_dump_ring(unsigned n);      /* header + the newest n samples         */
void     z3660_prof_poll_cmd(void);             /* mailbox command pickup (do_specialties)*/
unsigned z3660_prof_cpu_hz(void);               /* the calibrated ARM clock, 0 if unarmed*/
unsigned z3660_prof_wall_hz(void);              /* global-timer rate actually in use     */
/* Cost of ONE phase transition -- not of an enter/exit pair. The name carries the unit
 * because the caller multiplies it by TRANSITIONS, which counts each enter and each exit
 * separately, and v1 got that wrong by naming it after the thing it timed rather than
 * after the thing it is divided into. */
int      z3660_prof_probe_cyc_per_transition(void);
/* The two spans that figure is the DIFFERENCE of: PAIRS enter/exit calls with the buckets
 * ARMED, and the textually identical loop with them OFF. Only their difference is the
 * probe body; everything the two share -- the call, the predicate, the loop scaffolding --
 * survives in the machine the probe subtraction is meant to leave behind, so charging it
 * as "probe" would subtract it once and leave it there. That double count is why the v2
 * figure came back at 59 cyc/transition, above the point at which the C2 map's subtraction
 * stops being arithmetically possible. Reported on the boot line so a reader can check the
 * subtraction rather than take the quotient on faith. Both 0 in a non-profiling build. */
void     z3660_prof_probe_cal_spans(uint32_t *armed, uint32_t *unarmed);
int      z3660_prof_present(void);              /* 1 in a profiling build, else 0        */

#ifdef __cplusplus
}
#endif

/* Cross-core command word (SHARED.prof_ctl). Core0's serial console writes it; core1 acts
 * on it from do_specialties(), which costs nothing per instruction. One word rather than
 * one field per command, because SHARED is hand-maintained in two headers and every new
 * field is another chance to mis-merge it (docs/piscsi-service-path.md 5). */
#define Z3660_PROF_CTL_NONE      0u
#define Z3660_PROF_CTL_ARM       1u   /* arm at SHARED.prof_hz -- ZEROES everything      */
#define Z3660_PROF_CTL_STOP      2u   /* freeze BOTH instruments, keep every byte of data
                                       * -- the only lossless way out of an armed state.
                                       * The console's `PROF OFF` is exactly this, and it
                                       * exists because the rate-cycling `PROF` cannot be
                                       * one: every step of that ladder ARMS, and arming
                                       * zeroes. A boot profile requested from
                                       * z3660cfg.txt is armed at a rate the ladder does
                                       * not contain, so before v2 the only way to stop
                                       * it was to arm over the top of it and destroy the
                                       * capture that was the whole point of arming from
                                       * config in the first place.                      */
#define Z3660_PROF_CTL_ZERO      3u
#define Z3660_PROF_CTL_DUMPSTATS 4u
#define Z3660_PROF_CTL_DUMPRING  5u   /* count in the high 16 bits, 0 = a default slice  */
#define Z3660_PROF_CTL_BUCKETS_ON  6u
#define Z3660_PROF_CTL_BUCKETS_OFF 7u
#define Z3660_PROF_CTL_CMD(w)    ((w) & 0xFFFFu)
#define Z3660_PROF_CTL_ARG(w)    ((w) >> 16)
#define Z3660_PROF_CTL_MAKE(c,a) (((uint32_t)(a) << 16) | ((c) & 0xFFFFu))

/* How many samples a bare PROFDUMP renders. 4096 x 20 printable characters is ~90 KB,
 * about 8 s of 115200 serial -- long enough to be worth capturing, short enough that a
 * session does not lose the console for a quarter of an hour. The full 65536 is available
 * on request and takes about two minutes. */
#define Z3660_PROF_DUMP_DEFAULT  4096u

#endif /* SRC_UAE_Z3660_PROF_H_ */
