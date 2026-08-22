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
 *    are eight to fourteen transitions per emulated instruction. The profiler MEASURES
 *    this at arm time (z3660_prof_calibrate() times an empty enter/exit pair) and reports
 *    it as PROBE_CYC together with the total transition count, so a reader can compute
 *    the inflation and, if they care, subtract transitions x PROBE_CYC from each bucket.
 *    The instrumented instruction rate will be well below the lean rate. That is the
 *    instrument, not the interpreter.
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
 * 5. PMCCNTR IS 32 BITS and wraps every 2^32 ARM cycles (~6.4 s at 666 MHz). Every span
 *    this profiler measures is bounded by a run-loop iteration, so modular 32-bit
 *    subtraction is exact for the buckets. The long-run cycle base is extended to 64 bits
 *    at each sample; if core1 ever leaves the run loop for longer than one wrap the base
 *    loses 2^32 cycles silently. That is why the dump reports BOTH the PMU span and the
 *    independent ARM global-timer (XTime) span: if they disagree by more than a percent,
 *    a wrap was missed and the cycle totals are short by a multiple of 2^32.
 *
 * 6. THE ARM CLOCK IS MEASURED, NOT ASSUMED. z3660_prof_calibrate() times PMCCNTR against
 *    the global timer over ~10 ms and reports the result as CPU_HZ. A firmware that
 *    changed the ARM clock after arming would invalidate the Hz-to-cycles conversion; the
 *    dump reports the clock it calibrated at so that is visible rather than assumed.
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
 * changes. A symbolizer must refuse a version it does not know rather than guess. */
#define Z3660_PROF_VERSION      1u

/* 'Z3P' + version. Little-endian on the wire, because the ARM writes it and the host
 * reads it; the ASCII dump spells it out as a string so a serial capture is greppable. */
#define Z3660_PROF_MAGIC        0x5A335031u   /* "Z3P1" */

/* Ring capacity, in samples. 64K x 8 B = 512 KB and it lives in core1's .bss, which the
 * FSBL zeroes and which contributes NOTHING to BOOT.BIN -- so the ring is free in the one
 * budget that is actually tight here (the ~12 MB SD/TFTP write threshold). At 1 kHz it
 * holds 65 s of history; the limit on a profiling session is the serial dump, not this.
 * Must stay a power of two: the head advances with a mask, not a modulo. */
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
   uint32_t wall_hz;       /* COUNTS_PER_SECOND for wall_ticks                         */
   uint32_t build_flags;   /* Z3660_PROF_BUILD_* -- which variant produced this        */
   uint32_t reserved[2];   /* zero in version 1 -- room for a v2 field without a resize*/
} z3660_prof_hdr_t;

/* 80 and not a rounder-looking 64: the four uint64_t fields force 8-byte alignment, so
 * eight leading uint32_t put them at offset 32 and the struct ends at 80 on both ARM EABI
 * and x86-64. Pinned by a compile-time assertion in z3660_prof.cpp AND by the host
 * harness, because this is a wire format read by a symbolizer that will never be
 * rebuilt in step with the firmware. */
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
   Z3660_PROF_B_TAIL    = 8,  /* service poll + do_specialties                         */
   Z3660_PROF_B_FAULT   = 9,  /* last transition -> the run loop's CATCH (see bias 4)  */
   Z3660_PROF_B_PROF    = 10, /* the profiler's own dump work, kept out of the rest    */
   Z3660_PROF_B_COUNT   = 11
};

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
   Z3660_PROF_C_ATC_HIT     = 12, /* ... satisfied by the 4-way ATC                    */
   Z3660_PROF_C_ATC_MISS    = 13, /* ... that had to walk the tables                   */
   Z3660_PROF_C_MISALIGN_R  = 14, /* misaligned reads split into aligned sub-accesses  */
   Z3660_PROF_C_MISALIGN_W  = 15, /* ... writes                                        */
   Z3660_PROF_C_FAULTS      = 16, /* run-loop CATCH entries                            */
   Z3660_PROF_C_TRANSITIONS = 17, /* phase transitions -- multiply by PROBE_CYC        */
   Z3660_PROF_C_STACK_OVF   = 18, /* phase-stack overflows; MUST be 0                  */
   Z3660_PROF_C_COUNT       = 19
};

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
static __inline__ uint32_t z3660_prof_cycles(void) { return z3660_prof_host_cycles; }
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
#define Z3660_PROF_MARK_OPFETCH() do { z3660_prof_opfetch_hint = 1; } while (0)
/* Opcode fetch and extension fetch share one accessor; the mark decides which bucket. */
#define Z3660_PROF_ENTER_IFETCH()                                             \
   do {                                                                       \
      int z3660_b_ = z3660_prof_opfetch_hint ? Z3660_PROF_B_FETCHOP           \
                                             : Z3660_PROF_B_FETCHEX;          \
      z3660_prof_opfetch_hint = 0;                                            \
      z3660_prof_enter(z3660_b_);                                             \
   } while (0)

#else /* !Z3660_EMU_PROF -- the default and lean builds see none of the above */

#define Z3660_PROF_ENTER(b)     do { } while (0)
#define Z3660_PROF_EXIT()       do { } while (0)
#define Z3660_PROF_CNT(c)       do { } while (0)
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
int      z3660_prof_probe_cyc(void);            /* measured cost of one enter/exit pair  */
int      z3660_prof_present(void);              /* 1 in a profiling build, else 0        */

#ifdef __cplusplus
}
#endif

/* Cross-core command word (SHARED.prof_ctl). Core0's serial console writes it; core1 acts
 * on it from do_specialties(), which costs nothing per instruction. One word rather than
 * one field per command, because SHARED is hand-maintained in two headers and every new
 * field is another chance to mis-merge it (docs/piscsi-service-path.md 5). */
#define Z3660_PROF_CTL_NONE      0u
#define Z3660_PROF_CTL_ARM       1u   /* arm at SHARED.prof_hz                           */
#define Z3660_PROF_CTL_STOP      2u
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
