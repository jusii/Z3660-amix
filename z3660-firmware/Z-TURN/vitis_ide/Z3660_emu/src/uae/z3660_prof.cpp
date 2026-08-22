/* CYBERSTORM C1 -- the interpreter profiler engine.
 *
 * The contract, the wire format and the hot-path probes live in z3660_prof.h; this file
 * is the machinery behind them: PMU bring-up, ARM-clock calibration, the sample ring, the
 * serial dumps, and the cross-core command pickup.
 *
 * Nothing here is on the per-instruction path except z3660_prof_sample_slow(), which runs
 * once per tick (1 kHz by default, against a guest running some millions of instructions
 * per second).
 *
 * In a non-profiling build the whole file collapses to inert stubs so that the serial
 * console and the host harness can call the API unconditionally -- an #if at every call
 * site is how a build variant acquires the second bug that only it has.
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "../main.h"      /* the cross-core SHARED mailbox (prof_ctl / prof_hz) */
#include "z3660_prof.h"

#include <string.h>

extern SHARED *shared;

/* Name tables are compiled in EVERY variant. They cost ~400 bytes of rodata and they are
 * what makes a dump self-describing; a symbolizer reading a capture from an unknown
 * firmware can check the names against the ids it knows. Unsized on purpose -- the
 * tripwires below turn a forgotten name into a compile error instead of a NULL. */
extern "C" const char * const z3660_prof_bucket_names[] = {
   "LOOP", "FETCHOP", "FETCHEX", "READ", "WRITE",
   "XLATE", "WALK", "HANDLER", "TAILADV", "FAULT", "PROF",
   "TAILSAMP", "TAILPOLL", "TAILSPEC"
};
extern "C" const char * const z3660_prof_counter_names[] = {
   "INSNS", "INSNS_SUPER", "FETCH", "READ", "WRITE",
   "IPAGE_HIT", "IPAGE_MISS", "DPAGE_RHIT", "DPAGE_RMISS",
   "DPAGE_WHIT", "DPAGE_WMISS", "XLATE", "XLATE_OK", "ATC_MISS",
   "MISALIGN_R", "MISALIGN_W", "FAULTS", "TRANSITIONS", "STACK_OVF",
   "ATC_HIT"
};
typedef char z3660_prof_bucket_names_complete[
   (sizeof(z3660_prof_bucket_names)/sizeof(z3660_prof_bucket_names[0]) == Z3660_PROF_B_COUNT) ? 1 : -1];
typedef char z3660_prof_counter_names_complete[
   (sizeof(z3660_prof_counter_names)/sizeof(z3660_prof_counter_names[0]) == Z3660_PROF_C_COUNT) ? 1 : -1];

/* The record layout is a wire format shared with a host-side symbolizer, so pin it here
 * rather than trusting that no one ever adds a field. */
typedef char z3660_prof_rec_size_pinned[(sizeof(z3660_prof_sample_t) == Z3660_PROF_REC_SIZE) ? 1 : -1];
typedef char z3660_prof_hdr_size_pinned[(sizeof(z3660_prof_hdr_t)    == Z3660_PROF_HDR_SIZE) ? 1 : -1];

#if Z3660_EMU_PROF

#include "xtime_l.h"          /* ARM global timer -- calibration + the PMU wrap cross-check */
#include "runloop_build.h"    /* which run-loop instruments this binary also carries         */

extern "C" { extern volatile int amix_mmu_on; }

/* ------------------------------------------------------------------ state */

int      z3660_prof_bucket_on = 0;
int      z3660_prof_cur       = Z3660_PROF_B_LOOP;
uint32_t z3660_prof_last      = 0;
int      z3660_prof_sp        = 0;
uint8_t  z3660_prof_stack[Z3660_PROF_STACK_DEPTH];
uint64_t z3660_prof_acc[Z3660_PROF_B_COUNT];
uint64_t z3660_prof_cnt[Z3660_PROF_C_COUNT];

int      z3660_prof_sample_on = 0;
uint32_t z3660_prof_deadline  = 0;
uint32_t z3660_prof_period    = 0;

#if !(defined(__arm__) || defined(__thumb__))
uint32_t z3660_prof_host_cycles = 0;   /* host harness drives the clock by hand   */
uint32_t z3660_prof_host_step   = 0;   /* ...or lets it run, at this rate per read */
#endif

/* The ring lives in .bss. That is not an implementation detail -- .bss is zeroed by the
 * FSBL and occupies NOTHING in BOOT.BIN, so 512 KB of ring costs nothing in the image at
 * all. (The old "~12 MB SD/TFTP write threshold" this used to cite is retired; every
 * variant has exceeded it since the beta-21 resync and deploys CRC-clean. The ring's
 * freeness is a property of .bss, not of that rule, so the reasoning stands either way.) */
static z3660_prof_sample_t z3660_prof_ring[Z3660_PROF_RING_ENTRIES];
static uint32_t  z3660_prof_head;      /* next slot to write, free-running & masked */
static uint64_t  z3660_prof_samples;   /* total taken since zero                     */
static uint64_t  z3660_prof_drops;     /* overwritten by wrap                        */

static uint32_t  z3660_prof_hz;        /* nominal rate the ring was filled at        */
static uint32_t  z3660_prof_cpuhz;     /* ARM clock, ratio measured x runtime rate   */
static uint32_t  z3660_prof_wallhz;    /* global-timer rate ACTUALLY in force        */
static uint32_t  z3660_prof_clksrc = Z3660_PROF_CLK_BSP;  /* ...and where it came from */
static uint32_t  z3660_prof_ratio_q8; /* measured PMCCNTR:global-timer, x256         */
static int       z3660_prof_probecyc = -1;  /* cost of ONE transition, not of a pair */
static int       z3660_prof_inited;

/* Span bookkeeping. cyc_base extends PMCCNTR to 64 bits by accumulating the delta at
 * every sample; see bias note 5 in the header for when that is not enough, and why the
 * independent XTime span is reported next to it. */
static uint32_t  z3660_prof_cyc_last;
static uint64_t  z3660_prof_cyc_base;
static XTime     z3660_prof_wall0;

/* One-shot hint: the next instruction-stream fetch is the run loop's OPCODE fetch, not a
 * handler's extension word. Both arrive at the same accessor (cpuemu_31 fetches extension
 * words with get_iword_mmu040(N), not next_iword_mmu040), so the run loop marks its own
 * and the accessor consumes the mark. */
int z3660_prof_opfetch_hint = 0;

/* 64-bit decimal, without depending on the C library's long-long printf support.
 *
 * Two separate reasons, and the second is the dangerous one. The Xilinx standalone BSP
 * can be linked against a reduced printf that drops %llu; that failure at least looks
 * like a failure. The other is that `unsigned long` is 32 bits on this ABI, so the
 * obvious `(unsigned long)cyc_span` would print the low 32 bits of a 4e10 cycle span as a
 * perfectly plausible number and nothing anywhere would say so. Every 64-bit quantity in
 * a dump goes through here.
 *
 * Four rotating buffers so one printf can carry four of them, which is the most any line
 * below needs. Single-threaded on core1 by construction. */
static const char *z3660_prof_u64(uint64_t v)
{
   static char buf[4][24];
   static int  which = 0;
   char *p;
   which = (which + 1) & 3;
   p = &buf[which][23];
   *p = 0;
   if (!v) { *--p = '0'; return p; }
   while (v) { *--p = (char)('0' + (int)(v % 10u)); v /= 10u; }
   return p;
}

/* ------------------------------------------------------------------ PMU */

#if defined(__arm__) || defined(__thumb__)
static void z3660_prof_pmu_enable(void)
{
   uint32_t v;
   /* PMCR: E (enable all counters) | P (reset event counters) | C (reset PMCCNTR).
    * D is deliberately LEFT CLEAR: with the /64 divider on, PMCCNTR would count every
    * 64th cycle and every span shorter than 64 cycles -- which is most of them here --
    * would quantise to 0 or 64 and the bucket shares would be noise. */
   __asm__ __volatile__("mrc p15, 0, %0, c9, c12, 0" : "=r"(v));
   v = (v & ~0x08u) | 0x07u;
   __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
   /* PMCNTENSET bit 31 = the cycle counter. */
   v = 0x80000000u;
   __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" :: "r"(v));
   /* PMOVSR: clear a pending overflow so the first span is not measured across one. */
   __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 3" :: "r"(v));
}
#else
static void z3660_prof_pmu_enable(void) { }
#endif

/* Establish the ARM clock: measure what can be measured, take the rest from the running
 * configuration, and report which is which.
 *
 * THE PART THAT CAN BE MEASURED is the RATIO of PMCCNTR to the ARM global timer, and it
 * is timed here over ~10 ms. THE PART THAT CANNOT is the absolute rate: on this silicon
 * the global timer is hard-wired at CPU/2, so the ratio comes back ~2.0 no matter what
 * the PLL is doing and contains no information about it.
 *
 * v1 got the absolute rate from COUNTS_PER_SECOND -- and that is a COMPILE-TIME BSP
 * constant, baked at the 666.67 MHz default, while `arm_frequency` is a config key that
 * ps7_init_custom() applies to the PLL at boot. On a board configured for 1100 MHz the
 * constant is wrong by 1.65x, so every rate and every duration the v1 instrument printed
 * was low by 65 % -- and the header's own wrap cross-check could not notice, because a
 * fixed 2:1 ratio cancels the error out of exactly that comparison (bias note 6).
 *
 * So the absolute rate comes from core0, which is where arm_frequency is resolved and
 * where the PLL is retuned: SHARED.arm_wall_hz is the global-timer rate that is really
 * in force. The BSP constant remains the fallback for a core0 that never published one
 * (an older image, or the host harness), and the dump says `clk=bsp` when that happens
 * instead of leaving the reader to infer it from a boot log. */
static void z3660_prof_calibrate(void)
{
   XTime w0, w1;
   uint32_t c0, c1;
   XTime want;

   /* The runtime rate first: the spin window below is expressed in global-timer ticks,
    * so it has to be sized against the rate the timer actually runs at. `shared` is live
    * long before the emulator core starts (init_shared() runs in core1's main()), but
    * guard it anyway -- this is called from the emulator bring-up path and a null
    * dereference here would be a card that boots to nothing. */
   z3660_prof_wallhz = z3660_prof_pick_wall_hz(shared ? (uint32_t)shared->arm_wall_hz : 0u,
                                               (uint32_t)COUNTS_PER_SECOND,
                                               &z3660_prof_clksrc);

   want = (XTime)z3660_prof_wallhz / 100;   /* ~10 ms */
   /* The spin is BOUNDED as well as timed, and the bound is load-bearing rather than
    * defensive. A spin that only ends when the global timer advances is an infinite loop
    * on any board where that timer is not running -- and this runs during core1's
    * emulator bring-up, so the failure would be a card that boots to nothing, in the one
    * image a person flashes precisely because they are already chasing something. The
    * host harness proved the point immediately: its XTime stub returns a constant 0, and
    * the unbounded first version of this loop simply never returned. */
   /* 50M iterations: at ~30 ARM cycles per 64-bit global-timer read that is over a
    * hundred times the ~300k iterations 10 ms actually needs, and about a third of a
    * second on the host if the clock never moves at all. */
   uint32_t guard = 50000000u;

   if (want == 0) want = 1;

   XTime_GetTime(&w0);
   c0 = z3660_prof_cycles();
   do {
      XTime_GetTime(&w1);
   } while ((XTime)(w1 - w0) < want && --guard);
   c1 = z3660_prof_cycles();

   {
      uint64_t dc = (uint64_t)(uint32_t)(c1 - c0);
      uint64_t dw = (uint64_t)(w1 - w0);
      if (dw) {
         z3660_prof_cpuhz    = (uint32_t)(dc * (uint64_t)z3660_prof_wallhz / dw);
         /* The ratio is reported alongside the rate. It should be 2.00 (512 in Q8) on
          * this part; anything else means the CPU:global-timer relation this arithmetic
          * rests on is not what it is believed to be, and THAT is worth seeing rather
          * than averaging away into a Hz figure that looks fine. */
         z3660_prof_ratio_q8 = (uint32_t)(dc * 256u / dw);
      }
   }
   /* No usable clock (the host harness, or a board whose global timer is stopped): fall
    * back to a nominal figure so the arithmetic downstream is exercised rather than
    * divided by zero. The dump reports cpu_hz, so a reader can see this happened -- a
    * suspiciously round 666666666 next to a wall_ticks of 0 is the signature. */
   if (z3660_prof_cpuhz < 1000000u) z3660_prof_cpuhz = 666666666u;
}

/* Price ONE PHASE TRANSITION, out of line, so the inflation the buckets introduce is a
 * reported number rather than an apology -- and so that the number is in the unit it is
 * later multiplied in.
 *
 * TWO THINGS THIS GETS RIGHT THAT v1 DID NOT, both of which changed a conclusion:
 *
 * 1. THE UNIT. The batch below performs N enter/exit PAIRS, which is 2N transitions, and
 *    the divisor is 2N. TRANSITIONS increments in enter() and again in exit(), so
 *    `TRANSITIONS x probecyc` is only an overhead if probecyc is a per-transition cost.
 *    v1 divided by N and reported a pair, doubling every probe subtraction downstream --
 *    enough to demand more probe cycles out of LOOP than LOOP contained.
 *
 * 2. OUT OF LINE. z3660_prof_enter/_exit are `static __inline__`, and in the calibration
 *    loop gcc duly inlined them -- while every real accessor reaches them through a `bl`
 *    to a single out-of-line copy (docs/profiler.md counts 51 and 44 ARM instructions
 *    there). v1 therefore priced a cheaper probe than production pays, by an unknown
 *    margin, and the margin was load-bearing: LOOP's corrected share falls to zero at a
 *    per-transition cost only 28 % above v1's figure, so "is the dispatch loop worth
 *    attacking" had no answer at all. The two wrappers here are noinline, so what is
 *    timed is a genuine call to a genuine out-of-line body -- the same shape, with the
 *    same prologue and epilogue, that the accessors pay.
 *
 *    It is still an ESTIMATE, and honestly so: a wrapper called from this loop has
 *    different register pressure and a friendlier branch-predictor history than an
 *    accessor's call site. What it is no longer is a known under-estimate.
 *
 * The batch is 1024 rather than 256 to swamp the single-read granularity now that the
 * per-iteration cost is divided by twice as much; it runs once, at init, with the buckets
 * temporarily armed and the accumulators wiped by the caller. */
#if defined(__GNUC__)
#define Z3660_PROF_NOINLINE __attribute__((noinline))
#else
#define Z3660_PROF_NOINLINE
#endif

static Z3660_PROF_NOINLINE void z3660_prof_probe_call_enter(void)
{
   z3660_prof_enter(Z3660_PROF_B_PROF);
}
static Z3660_PROF_NOINLINE void z3660_prof_probe_call_exit(void)
{
   z3660_prof_exit();
}

#define Z3660_PROF_CAL_PAIRS 1024

static void z3660_prof_calibrate_probe(void)
{
   int saved_on = z3660_prof_bucket_on;
   uint32_t c0, c1;
   int i;

   z3660_prof_bucket_on = 1;
   z3660_prof_sp = 0;
   z3660_prof_cur = Z3660_PROF_B_LOOP;
   z3660_prof_last = z3660_prof_cycles();

   c0 = z3660_prof_cycles();
   for (i = 0; i < Z3660_PROF_CAL_PAIRS; i++) {
      z3660_prof_probe_call_enter();
      z3660_prof_probe_call_exit();
   }
   c1 = z3660_prof_cycles();

   /* 2 x pairs: one transition per enter, one per exit. */
   z3660_prof_probecyc = (int)(((uint32_t)(c1 - c0)) / (2u * Z3660_PROF_CAL_PAIRS));
   z3660_prof_bucket_on = saved_on;
}

#if !(defined(__arm__) || defined(__thumb__))
/* Host harness only -- see z3660_prof.h. Not reachable on the board at all. */
extern "C" void z3660_prof_recalibrate_probe_for_test(void)
{
   z3660_prof_calibrate_probe();
}
#endif

/* ------------------------------------------------------------------ cycle base */

/* Fold PMCCNTR's elapsed 32 bits into the 64-bit span, and say what time it is now.
 *
 * v1 advanced this ONLY from z3660_prof_sample_slow(), which has two consequences the C2
 * audit had to work around by hand:
 *
 *   - `cyc_span` stopped at the LAST SAMPLE while `wall_ticks` ran on to the dump, so the
 *     two spans covered different intervals. The wrap cross-check the header asks a
 *     symbolizer to perform then fires on the gap between "the workload ended" and "a
 *     human typed PROFR" -- 91 s on one C1 capture -- and reports a missed PMU wrap that
 *     never happened. A cross-check that cries wolf on ordinary operator latency is a
 *     cross-check nobody will believe on the day it is right.
 *   - With the sampler OFF (a bucket-only run) it never advanced at all, so a span longer
 *     than one 32-bit wrap was simply wrong rather than merely late.
 *
 * Calling it from z3660_prof_poll_cmd() fixes both: that runs from do_specialties(),
 * which a live guest reaches on every interrupt -- many times a second, at no
 * per-instruction cost, whatever the instruments are set to. The dumps call it too, so
 * `cyc_span` is stamped at the moment of the dump. */
static uint32_t z3660_prof_cyc_advance(void)
{
   uint32_t now = z3660_prof_cycles();
   z3660_prof_cyc_base += (uint64_t)(uint32_t)(now - z3660_prof_cyc_last);
   z3660_prof_cyc_last  = now;
   return now;
}

/* ------------------------------------------------------------------ sampler */

/* The out-of-line half of the tick. Runs once per period, so it can afford a division.
 *
 * WEIGHT is computed here and is the honest part of this instrument: `elapsed / period`
 * is how many tick periods this one sample stands for. It is 1 whenever the sampler is
 * keeping up. It is large exactly when core1 spent a long time somewhere the run-loop
 * tail could not observe -- a mailbox round trip, a STOP spin, an exception -- which is
 * the information a wall-clock profiler exists to deliver and which a naive "one sample,
 * one tick" ring would silently discard. */
extern "C" void z3660_prof_sample_slow(uint32_t pc, uint16_t opcode, uint16_t flags)
{
   uint32_t now = z3660_prof_cycles();
   uint32_t elapsed = (uint32_t)(now - z3660_prof_deadline) + z3660_prof_period;
   uint32_t weight = z3660_prof_period ? (elapsed / z3660_prof_period) : 1u;

   if (weight < 1u) weight = 1u;
   if (weight > Z3660_PROF_F_WEIGHT_MAX) weight = Z3660_PROF_F_WEIGHT_MAX;

   /* Re-arm from the deadline that just expired, not from `now`: the tick then keeps the
    * nominal rate instead of drifting later by however long the sample took. When the
    * sampler has fallen behind by whole periods, skip forward to the next future deadline
    * rather than firing a burst to catch up -- a burst would be samples of a moment that
    * has passed, all of them attributed to one PC. */
   z3660_prof_deadline += weight * z3660_prof_period;

   /* Extend PMCCNTR to 64 bits. Safe as long as samples arrive more often than once per
    * 2^32 cycles, which the Hz floor guarantees while the run loop is running. */
   z3660_prof_cyc_base += (uint64_t)(uint32_t)(now - z3660_prof_cyc_last);
   z3660_prof_cyc_last  = now;

   {
      uint32_t slot = z3660_prof_head & Z3660_PROF_RING_MASK;
      if (z3660_prof_samples >= (uint64_t)Z3660_PROF_RING_ENTRIES)
         z3660_prof_drops++;
      z3660_prof_ring[slot].pc     = pc;
      z3660_prof_ring[slot].opcode = opcode;
      z3660_prof_ring[slot].flags  = (uint16_t)((flags & ~(uint16_t)0xFF00u)
                                     | (uint16_t)(weight << Z3660_PROF_F_WEIGHT_SH));
      z3660_prof_head++;
      z3660_prof_samples++;
   }
}

/* ------------------------------------------------------------------ control */

extern "C" void z3660_prof_init(void)
{
   if (z3660_prof_inited) return;
   z3660_prof_inited = 1;
   z3660_prof_pmu_enable();
   z3660_prof_calibrate();
   z3660_prof_calibrate_probe();
   z3660_prof_zero();
   /* Every unit on this line is spelled out, and the clock's SOURCE is on it. Both are
    * scars: the v1 line said "enter/exit pair 91 cyc" next to a per-transition count, and
    * "ARM clock 666667585 Hz (measured)" seven seconds after the same console printed
    * "ARM PLL successfully configured for 1100 MHz". Neither was catchable from the line
    * itself, which is the only place a reader looks. */
   z3660_printf("[PROF] profiling build v%u: ARM clock %lu Hz (clk=%s, PMU:wall %lu.%02lu), "
                "probe %d cyc/transition (%d cyc/pair), ring %lu x %u B\r\n",
                (unsigned)Z3660_PROF_VERSION,
                (unsigned long)z3660_prof_cpuhz,
                z3660_prof_clksrc == Z3660_PROF_CLK_CFG ? "cfg" : "bsp",
                (unsigned long)(z3660_prof_ratio_q8 / 256u),
                (unsigned long)(((z3660_prof_ratio_q8 % 256u) * 100u) / 256u),
                z3660_prof_probecyc, z3660_prof_probecyc * 2,
                (unsigned long)Z3660_PROF_RING_ENTRIES, (unsigned)Z3660_PROF_REC_SIZE);
   if (z3660_prof_clksrc != Z3660_PROF_CLK_CFG)
      z3660_printf("[PROF] WARNING: clk=bsp -- core0 published no ARM rate, so this is the "
                   "COMPILE-TIME constant. If the board retuned its PLL, every Hz and every "
                   "seconds figure below is wrong by that ratio.\r\n");
}

extern "C" void z3660_prof_zero(void)
{
   memset(z3660_prof_acc, 0, sizeof z3660_prof_acc);
   memset(z3660_prof_cnt, 0, sizeof z3660_prof_cnt);
   z3660_prof_head    = 0;
   z3660_prof_samples = 0;
   z3660_prof_drops   = 0;
   z3660_prof_sp      = 0;
   z3660_prof_cur     = Z3660_PROF_B_LOOP;
   z3660_prof_last    = z3660_prof_cycles();
   z3660_prof_cyc_last = z3660_prof_last;
   z3660_prof_cyc_base = 0;
   XTime_GetTime(&z3660_prof_wall0);
}

extern "C" void z3660_prof_arm(unsigned hz)
{
   z3660_prof_init();
   if (hz < Z3660_PROF_HZ_MIN) hz = Z3660_PROF_HZ_MIN;
   if (hz > Z3660_PROF_HZ_MAX) hz = Z3660_PROF_HZ_MAX;
   z3660_prof_hz     = hz;
   z3660_prof_period = z3660_prof_cpuhz / hz;
   if (z3660_prof_period == 0) z3660_prof_period = 1;
   z3660_prof_zero();
   z3660_prof_deadline   = z3660_prof_cycles() + z3660_prof_period;
   z3660_prof_sample_on  = 1;
   z3660_printf("[PROF] armed: %lu Hz (%lu cyc/tick), buckets %s\r\n",
                (unsigned long)hz, (unsigned long)z3660_prof_period,
                z3660_prof_bucket_on ? "ON" : "off");
}

extern "C" void z3660_prof_stop(void)
{
   /* Close the span before freezing, so a capture stopped here and dumped a minute later
    * reports the cycles it actually covered rather than the cycles up to its last tick. */
   z3660_prof_cyc_advance();
   z3660_prof_sample_on = 0;
   z3660_prof_bucket_on = 0;
   z3660_printf("[PROF] stopped: %s samples, %s dropped (data KEPT -- PROFD/PROFR read it)\r\n",
                z3660_prof_u64(z3660_prof_samples), z3660_prof_u64(z3660_prof_drops));
}

extern "C" void z3660_prof_set_buckets(int on)
{
   z3660_prof_init();
   z3660_prof_sp   = 0;
   z3660_prof_cur  = Z3660_PROF_B_LOOP;
   z3660_prof_last = z3660_prof_cycles();
   z3660_prof_bucket_on = on ? 1 : 0;
   z3660_printf("[PROF] stage buckets %s%s\r\n", on ? "ON" : "OFF",
                on ? " (this costs real time -- shares are the answer, the rate is not)" : "");
}

extern "C" unsigned z3660_prof_cpu_hz(void)  { return z3660_prof_cpuhz; }
extern "C" unsigned z3660_prof_wall_hz(void) { return z3660_prof_wallhz; }
extern "C" int      z3660_prof_probe_cyc_per_transition(void) { return z3660_prof_probecyc; }
extern "C" int      z3660_prof_present(void) { return 1; }

/* ------------------------------------------------------------------ dumps */

static uint32_t z3660_prof_build_flags(void)
{
   return (uint32_t)Z3660_RUNLOOP_BUILD_FLAGS | Z3660_PROF_BUILD_PROF;
}

/* Percent to two decimals without floating point -- the emulator core runs with
 * -ffast-math and a soft/NEON float path that is not worth dragging into a diagnostic. */
static void z3660_prof_pct(uint64_t part, uint64_t whole, unsigned *ip, unsigned *fp)
{
   if (!whole) { *ip = 0; *fp = 0; return; }
   {
      uint64_t h = part * 10000u / whole;
      *ip = (unsigned)(h / 100u);
      *fp = (unsigned)(h % 100u);
   }
}

extern "C" void z3660_prof_dump_stats(void)
{
   uint64_t total = 0;
   XTime wall;
   int i;

   z3660_prof_enter(Z3660_PROF_B_PROF);
   z3660_prof_cyc_advance();

   for (i = 0; i < Z3660_PROF_B_COUNT; i++) total += z3660_prof_acc[i];
   XTime_GetTime(&wall);

   z3660_printf("[PROF] === stage attribution ===\r\n");
   z3660_printf("[PROF] ver=%u build=0x%02X cpu_hz=%lu clk=%s hz=%lu period_cyc=%lu "
                "probe_cyc_per_transition=%d\r\n",
                (unsigned)Z3660_PROF_VERSION, (unsigned)z3660_prof_build_flags(),
                (unsigned long)z3660_prof_cpuhz,
                z3660_prof_clksrc == Z3660_PROF_CLK_CFG ? "cfg" : "bsp",
                (unsigned long)z3660_prof_hz,
                (unsigned long)z3660_prof_period, z3660_prof_probecyc);
   z3660_printf("[PROF] total_cyc=%s wall_ticks=%s wall_hz=%s\r\n",
                z3660_prof_u64(total),
                z3660_prof_u64((uint64_t)(wall - z3660_prof_wall0)),
                z3660_prof_u64((uint64_t)z3660_prof_wallhz));

   if (!z3660_prof_bucket_on && total == 0)
      z3660_printf("[PROF] (stage buckets have not been on -- PROFB arms them)\r\n");

   for (i = 0; i < Z3660_PROF_B_COUNT; i++) {
      unsigned ip, fp;
      z3660_prof_pct(z3660_prof_acc[i], total, &ip, &fp);
      z3660_printf("[PROF] b %-2d %-8s cyc=%-16s %3u.%02u%%\r\n",
                   i, z3660_prof_bucket_names[i],
                   z3660_prof_u64(z3660_prof_acc[i]), ip, fp);
   }

   /* The whole tail, so the v2 split can be compared against a v1 capture without the
    * reader having to know which four ids to add up. */
   if (total) {
      uint64_t tail = z3660_prof_acc[Z3660_PROF_B_TAIL]
                    + z3660_prof_acc[Z3660_PROF_B_TAILSAMP]
                    + z3660_prof_acc[Z3660_PROF_B_TAILPOLL]
                    + z3660_prof_acc[Z3660_PROF_B_TAILSPEC];
      unsigned ip, fp;
      z3660_prof_pct(tail, total, &ip, &fp);
      z3660_printf("[PROF] t whole tail (TAILADV+TAILSAMP+TAILPOLL+TAILSPEC) cyc=%s %3u.%02u%% "
                   "-- this is what v1 reported as TAIL\r\n",
                   z3660_prof_u64(tail), ip, fp);
   }

   /* The instrument's own weight, stated rather than left to be discovered.
    *
    * PER TRANSITION, and the line says so twice: once in the unit and once in the
    * arithmetic it shows. TRANSITIONS counts each enter and each exit, so pairing it
    * with a per-PAIR cost -- which is what v1 did -- doubles the answer, and a doubled
    * probe subtraction is not a conservative error. It moved LOOP from first place to
    * arithmetically impossible. */
   if (z3660_prof_probecyc > 0 && total) {
      uint64_t probe = z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS] * (uint64_t)z3660_prof_probecyc;
      unsigned ip, fp;
      z3660_prof_pct(probe, total, &ip, &fp);
      z3660_printf("[PROF] probe overhead inside the totals: %s cyc = %u.%02u%% "
                   "(%s transitions x %d cyc/TRANSITION, out-of-line calibrated) "
                   "-- subtract per bucket by its share of transitions\r\n",
                   z3660_prof_u64(probe), ip, fp,
                   z3660_prof_u64(z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS]), z3660_prof_probecyc);
      if (probe > total)
         z3660_printf("[PROF] WARNING: the modelled probe cost EXCEEDS the measured total. "
                      "The calibration or the transition count is wrong; do not subtract.\r\n");
   }

   z3660_printf("[PROF] === counters ===\r\n");
   for (i = 0; i < Z3660_PROF_C_COUNT; i++)
      z3660_printf("[PROF] c %-2d %-12s %s\r\n", i, z3660_prof_counter_names[i],
                   z3660_prof_u64(z3660_prof_cnt[i]));

   /* The three derived rates C2 was going to compute by hand anyway. */
   {
      uint64_t ih = z3660_prof_cnt[Z3660_PROF_C_IPAGE_HIT];
      uint64_t im = z3660_prof_cnt[Z3660_PROF_C_IPAGE_MISS];
      uint64_t rh = z3660_prof_cnt[Z3660_PROF_C_DPAGE_RHIT];
      uint64_t rm = z3660_prof_cnt[Z3660_PROF_C_DPAGE_RMISS];
      uint64_t wh = z3660_prof_cnt[Z3660_PROF_C_DPAGE_WHIT];
      uint64_t wm = z3660_prof_cnt[Z3660_PROF_C_DPAGE_WMISS];
      uint64_t xl = z3660_prof_cnt[Z3660_PROF_C_XLATE];
      uint64_t am = z3660_prof_cnt[Z3660_PROF_C_ATC_MISS];
      uint64_t ah = z3660_prof_cnt[Z3660_PROF_C_ATC_HIT];
      uint64_t xo = z3660_prof_cnt[Z3660_PROF_C_XLATE_OK];
      uint64_t in = z3660_prof_cnt[Z3660_PROF_C_INSNS];
      uint64_t su = z3660_prof_cnt[Z3660_PROF_C_INSNS_SUPER];
      unsigned ip, fp;
      z3660_prof_pct(ih, ih + im, &ip, &fp);
      z3660_printf("[PROF] r ipagecache hit %u.%02u%%\r\n", ip, fp);
      z3660_prof_pct(rh, rh + rm, &ip, &fp);
      z3660_printf("[PROF] r dpagecache read hit %u.%02u%%\r\n", ip, fp);
      z3660_prof_pct(wh, wh + wm, &ip, &fp);
      z3660_printf("[PROF] r dpagecache write hit %u.%02u%%\r\n", ip, fp);
      /* The walk rate was correct in v1 and is unchanged. The line beside it is the one
       * that could not be printed before: a genuine ATC hit rate, out of a counter that
       * counts ATC hits. v1's `ATC_HIT` counted successful translates, so the obvious
       * ATC_HIT/(ATC_HIT+ATC_MISS) a reader would form from it was meaningless. */
      z3660_prof_pct(am, xl, &ip, &fp);
      z3660_printf("[PROF] r of %s translates, %u.%02u%% had to walk the tables\r\n",
                   z3660_prof_u64(xl), ip, fp);
      z3660_prof_pct(ah, xl, &ip, &fp);
      z3660_printf("[PROF] r ...and %u.%02u%% were satisfied from the ATC without a walk\r\n",
                   ip, fp);
      /* Silent on the 030, where XLATE_OK has no site (see z3660_prof.h). A zero here
       * would be read as "every translate faulted", which is a worse answer than none. */
      if (xo) {
         z3660_prof_pct(xo, xl, &ip, &fp);
         z3660_printf("[PROF] r ...and %u.%02u%% returned an address (the rest faulted)\r\n",
                      ip, fp);
      }
      /* The identity, computed here so a capture carries its own consistency check
       * rather than relying on a host tool to think of forming it. */
      if (xl && ah + am != xl)
         z3660_printf("[PROF] r note: ATC_HIT+ATC_MISS=%s vs XLATE=%s -- the difference is "
                      "translates that faulted before the walk decision\r\n",
                      z3660_prof_u64(ah + am), z3660_prof_u64(xl));
      z3660_prof_pct(su, in, &ip, &fp);
      z3660_printf("[PROF] r supervisor instructions %u.%02u%% of %s\r\n",
                   ip, fp, z3660_prof_u64(in));
   }
   z3660_printf("[PROF] === end ===\r\n");

   z3660_prof_exit();
}

/* Ring dump. Grammar, VERSION 2 (docs/profiler.md holds the same text):
 *
 *   [PROF] ring hdr magic=Z3P2 version=2 rec_size=8 rec_count=<n> ring_entries=65536
 *                   hz=<nominal> period_cyc=<n> cpu_hz=<n>
 *   [PROF] ring hdr samples=<n> drops=<n> cyc_span=<n> wall_ticks=<n> wall_hz=<n>
 *                   build=0x04 probe_cyc=<n> clk=cfg|bsp
 *   [PROF] ring begin
 *   S pppppppp oooo ffff        <- one per sample, oldest first
 *   ...
 *   [PROF] ring end n=<count>
 *
 * Changes from v1, all on the second header line and all additive except the magic:
 *   - magic Z3P1 -> Z3P2, so a v1 tool refuses rather than mis-reads;
 *   - wall_hz is the RUNTIME global-timer rate, not COUNTS_PER_SECOND;
 *   - cpu_hz is scaled by that, so it is the clock the board is really running;
 *   - probe_cyc is new here and is PER TRANSITION, so a ring capture carries the
 *     calibration instead of requiring the PROFD block or the boot line beside it;
 *   - clk= says where the absolute rate came from.
 * THE SAMPLE LINES ARE UNCHANGED. A v1 decoder for `S` records is still correct.
 *
 * Fixed-width lowercase hex, no 0x prefix, oldest first. Fixed width so a symbolizer can
 * slice by column and a truncated serial capture is detectable rather than merely
 * mis-parsed. */
extern "C" void z3660_prof_dump_ring(unsigned n)
{
   uint64_t have = z3660_prof_samples < (uint64_t)Z3660_PROF_RING_ENTRIES
                 ? z3660_prof_samples : (uint64_t)Z3660_PROF_RING_ENTRIES;
   XTime wall;
   uint32_t start;
   unsigned i;

   z3660_prof_enter(Z3660_PROF_B_PROF);
   /* Stamp the cycle span at the DUMP, so it covers the same interval wall_ticks does
    * and the symbolizer's wrap cross-check compares like with like. */
   z3660_prof_cyc_advance();

   if (n == 0 || (uint64_t)n > have) n = (unsigned)have;
   XTime_GetTime(&wall);

   z3660_printf("[PROF] ring hdr magic=%s version=%u rec_size=%u rec_count=%u ring_entries=%lu "
                "hz=%lu period_cyc=%lu cpu_hz=%lu\r\n",
                Z3660_PROF_MAGIC_STR,
                (unsigned)Z3660_PROF_VERSION, (unsigned)Z3660_PROF_REC_SIZE, n,
                (unsigned long)Z3660_PROF_RING_ENTRIES, (unsigned long)z3660_prof_hz,
                (unsigned long)z3660_prof_period, (unsigned long)z3660_prof_cpuhz);
   z3660_printf("[PROF] ring hdr samples=%s drops=%s cyc_span=%s wall_ticks=%s wall_hz=%lu "
                "build=0x%02X probe_cyc=%d clk=%s\r\n",
                z3660_prof_u64(z3660_prof_samples), z3660_prof_u64(z3660_prof_drops),
                z3660_prof_u64(z3660_prof_cyc_base),
                z3660_prof_u64((uint64_t)(wall - z3660_prof_wall0)),
                (unsigned long)z3660_prof_wallhz, (unsigned)z3660_prof_build_flags(),
                z3660_prof_probecyc,
                z3660_prof_clksrc == Z3660_PROF_CLK_CFG ? "cfg" : "bsp");
   z3660_printf("[PROF] ring begin\r\n");

   start = z3660_prof_head - n;
   for (i = 0; i < n; i++) {
      const z3660_prof_sample_t *s = &z3660_prof_ring[(start + i) & Z3660_PROF_RING_MASK];
      z3660_printf("S %08lx %04x %04x\r\n",
                   (unsigned long)s->pc, (unsigned)s->opcode, (unsigned)s->flags);
   }
   z3660_printf("[PROF] ring end n=%u\r\n", n);

   z3660_prof_exit();
}

/* Cross-core command pickup. Called from do_specialties(), which the run loops enter only
 * when regs.spcflags != 0 -- so this costs exactly zero instructions on the
 * per-instruction path, at every setting, which is the same reason z3660_knobs_poll()
 * lives there. Under a running guest spcflags fires on every interrupt, so a serial
 * command reaches core1 in well under a second. */
extern "C" void z3660_prof_poll_cmd(void)
{
   uint32_t w = shared->prof_ctl;
   uint32_t cmd = Z3660_PROF_CTL_CMD(w);

   /* Keep the 64-bit cycle base fresh whether or not anything is armed. This is the one
    * place that runs often (every interrupt) and costs nothing per instruction, which is
    * exactly what a 32-bit counter wrapping every ~3.9 s needs. With the sampler off,
    * nothing else advances it at all -- and a bucket-only run is precisely the case
    * where the span matters and the sampler is off. */
   z3660_prof_cyc_advance();

   if (cmd == Z3660_PROF_CTL_NONE) return;
   shared->prof_ctl = Z3660_PROF_CTL_NONE;      /* consume before acting: a dump takes seconds */

   switch (cmd) {
   case Z3660_PROF_CTL_ARM:
      /* The argument says whether the stage buckets come up with the sampler. Set them
       * BEFORE arming so the arm message reports the state that will actually hold, and
       * so a boot profile requested from z3660cfg.txt starts complete rather than
       * sampler-only for its first few hundred instructions. */
      z3660_prof_set_buckets(Z3660_PROF_CTL_ARG(w) ? 1 : 0);
      z3660_prof_arm((unsigned)shared->prof_hz);
      break;
   case Z3660_PROF_CTL_STOP:        z3660_prof_stop();          break;
   case Z3660_PROF_CTL_ZERO:        z3660_prof_zero();          break;
   case Z3660_PROF_CTL_DUMPSTATS:   z3660_prof_dump_stats();    break;
   case Z3660_PROF_CTL_DUMPRING:
      {
         uint32_t a = Z3660_PROF_CTL_ARG(w);
         z3660_prof_dump_ring(a ? a : Z3660_PROF_DUMP_DEFAULT);
      }
      break;
   case Z3660_PROF_CTL_BUCKETS_ON:  z3660_prof_set_buckets(1);  break;
   case Z3660_PROF_CTL_BUCKETS_OFF: z3660_prof_set_buckets(0);  break;
   default: break;
   }
}

#else /* !Z3660_EMU_PROF ------------------------------------------------------ */

/* Inert stubs. The default and lean binaries carry these (a handful of `bx lr`s) so that
 * uae_emulator.cpp and the serial console can call the API without an #if at each site.
 * z3660_prof_present() returning 0 is what lets a console command say "this firmware is
 * not the profiling build" instead of silently doing nothing. */
extern "C" void     z3660_prof_init(void)            { }
extern "C" void     z3660_prof_arm(unsigned hz)      { (void)hz; }
extern "C" void     z3660_prof_stop(void)            { }
extern "C" void     z3660_prof_zero(void)            { }
extern "C" void     z3660_prof_set_buckets(int on)   { (void)on; }
extern "C" void     z3660_prof_dump_stats(void)      { }
extern "C" void     z3660_prof_dump_ring(unsigned n) { (void)n; }
extern "C" void     z3660_prof_poll_cmd(void)        { }
extern "C" unsigned z3660_prof_cpu_hz(void)          { return 0; }
extern "C" unsigned z3660_prof_wall_hz(void)         { return 0; }
extern "C" int      z3660_prof_probe_cyc_per_transition(void) { return -1; }
extern "C" int      z3660_prof_present(void)         { return 0; }

#endif /* Z3660_EMU_PROF */
