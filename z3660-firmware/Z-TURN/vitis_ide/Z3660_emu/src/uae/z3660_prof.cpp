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
   "XLATE", "WALK", "HANDLER", "TAIL", "FAULT", "PROF"
};
extern "C" const char * const z3660_prof_counter_names[] = {
   "INSNS", "INSNS_SUPER", "FETCH", "READ", "WRITE",
   "IPAGE_HIT", "IPAGE_MISS", "DPAGE_RHIT", "DPAGE_RMISS",
   "DPAGE_WHIT", "DPAGE_WMISS", "XLATE", "ATC_HIT", "ATC_MISS",
   "MISALIGN_R", "MISALIGN_W", "FAULTS", "TRANSITIONS", "STACK_OVF"
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
uint32_t z3660_prof_host_cycles = 0;   /* host harness drives the clock by hand */
#endif

/* The ring lives in .bss. That is not an implementation detail -- .bss is zeroed by the
 * FSBL and occupies NOTHING in BOOT.BIN, so 512 KB of ring costs nothing in the budget
 * that is actually tight here (the ~12 MB SD/TFTP write threshold, CLAUDE.md). */
static z3660_prof_sample_t z3660_prof_ring[Z3660_PROF_RING_ENTRIES];
static uint32_t  z3660_prof_head;      /* next slot to write, free-running & masked */
static uint64_t  z3660_prof_samples;   /* total taken since zero                     */
static uint64_t  z3660_prof_drops;     /* overwritten by wrap                        */

static uint32_t  z3660_prof_hz;        /* nominal rate the ring was filled at        */
static uint32_t  z3660_prof_cpuhz;     /* MEASURED ARM clock (see calibrate)         */
static int       z3660_prof_probecyc = -1;  /* measured cost of one enter/exit pair  */
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

/* Measure the ARM clock rather than believe a config key.
 *
 * The board's ARM frequency is settable (arm_freq_code) and the sweeps this profiler
 * exists to support change it. Timing PMCCNTR against the global timer over ~10 ms costs
 * 10 ms once, at arm time, and makes CPU_HZ a measurement that appears in the dump
 * header -- so a reader can see the clock the periods were computed from instead of
 * assuming one.
 *
 * COUNTS_PER_SECOND is the global timer's rate (CPU/2 on this part); it is a BSP
 * constant, not something this code gets to be wrong about. */
static void z3660_prof_calibrate(void)
{
   XTime w0, w1;
   uint32_t c0, c1;
   XTime want = (XTime)COUNTS_PER_SECOND / 100;   /* ~10 ms */
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
      if (dw)
         z3660_prof_cpuhz = (uint32_t)(dc * (uint64_t)COUNTS_PER_SECOND / dw);
   }
   /* No usable clock (the host harness, or a board whose global timer is stopped): fall
    * back to a nominal figure so the arithmetic downstream is exercised rather than
    * divided by zero. The dump reports cpu_hz, so a reader can see this happened -- a
    * suspiciously round 666666666 next to a wall_ticks of 0 is the signature. */
   if (z3660_prof_cpuhz < 1000000u) z3660_prof_cpuhz = 666666666u;
}

/* Price one enter/exit pair, so the inflation the buckets introduce is a reported number
 * rather than an apology. Measured with the buckets temporarily armed and the result
 * discarded -- the pair is timed as a batch of 256 to swamp the single-read granularity,
 * then the accumulators are wiped by the caller. */
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
   for (i = 0; i < 256; i++) {
      z3660_prof_enter(Z3660_PROF_B_PROF);
      z3660_prof_exit();
   }
   c1 = z3660_prof_cycles();

   z3660_prof_probecyc = (int)(((uint32_t)(c1 - c0)) / 256u);
   z3660_prof_bucket_on = saved_on;
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
   z3660_printf("[PROF] profiling build: ARM clock %lu Hz (measured), enter/exit pair %d cyc, ring %lu x %u B\r\n",
                (unsigned long)z3660_prof_cpuhz, z3660_prof_probecyc,
                (unsigned long)Z3660_PROF_RING_ENTRIES, (unsigned)Z3660_PROF_REC_SIZE);
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
   z3660_prof_sample_on = 0;
   z3660_prof_bucket_on = 0;
   z3660_printf("[PROF] stopped: %s samples, %s dropped\r\n",
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
extern "C" int      z3660_prof_probe_cyc(void) { return z3660_prof_probecyc; }
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

   for (i = 0; i < Z3660_PROF_B_COUNT; i++) total += z3660_prof_acc[i];
   XTime_GetTime(&wall);

   z3660_printf("[PROF] === stage attribution ===\r\n");
   z3660_printf("[PROF] ver=%u build=0x%02X cpu_hz=%lu hz=%lu period_cyc=%lu probe_cyc=%d\r\n",
                (unsigned)Z3660_PROF_VERSION, (unsigned)z3660_prof_build_flags(),
                (unsigned long)z3660_prof_cpuhz, (unsigned long)z3660_prof_hz,
                (unsigned long)z3660_prof_period, z3660_prof_probecyc);
   z3660_printf("[PROF] total_cyc=%s wall_ticks=%s wall_hz=%s\r\n",
                z3660_prof_u64(total),
                z3660_prof_u64((uint64_t)(wall - z3660_prof_wall0)),
                z3660_prof_u64((uint64_t)COUNTS_PER_SECOND));

   if (!z3660_prof_bucket_on && total == 0)
      z3660_printf("[PROF] (stage buckets have not been on -- PROFB arms them)\r\n");

   for (i = 0; i < Z3660_PROF_B_COUNT; i++) {
      unsigned ip, fp;
      z3660_prof_pct(z3660_prof_acc[i], total, &ip, &fp);
      z3660_printf("[PROF] b %-2d %-8s cyc=%-16s %3u.%02u%%\r\n",
                   i, z3660_prof_bucket_names[i],
                   z3660_prof_u64(z3660_prof_acc[i]), ip, fp);
   }

   /* The instrument's own weight, stated rather than left to be discovered. */
   if (z3660_prof_probecyc > 0 && total) {
      uint64_t probe = z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS] * (uint64_t)z3660_prof_probecyc;
      unsigned ip, fp;
      z3660_prof_pct(probe, total, &ip, &fp);
      z3660_printf("[PROF] probe overhead inside the totals: %s cyc = %u.%02u%% "
                   "(%s transitions x %d cyc) -- subtract per bucket by its share of transitions\r\n",
                   z3660_prof_u64(probe), ip, fp,
                   z3660_prof_u64(z3660_prof_cnt[Z3660_PROF_C_TRANSITIONS]), z3660_prof_probecyc);
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
      uint64_t in = z3660_prof_cnt[Z3660_PROF_C_INSNS];
      uint64_t su = z3660_prof_cnt[Z3660_PROF_C_INSNS_SUPER];
      unsigned ip, fp;
      z3660_prof_pct(ih, ih + im, &ip, &fp);
      z3660_printf("[PROF] r ipagecache hit %u.%02u%%\r\n", ip, fp);
      z3660_prof_pct(rh, rh + rm, &ip, &fp);
      z3660_printf("[PROF] r dpagecache read hit %u.%02u%%\r\n", ip, fp);
      z3660_prof_pct(wh, wh + wm, &ip, &fp);
      z3660_printf("[PROF] r dpagecache write hit %u.%02u%%\r\n", ip, fp);
      z3660_prof_pct(am, xl, &ip, &fp);
      z3660_printf("[PROF] r of %s translates, %u.%02u%% had to walk the tables\r\n",
                   z3660_prof_u64(xl), ip, fp);
      z3660_prof_pct(su, in, &ip, &fp);
      z3660_printf("[PROF] r supervisor instructions %u.%02u%% of %s\r\n",
                   ip, fp, z3660_prof_u64(in));
   }
   z3660_printf("[PROF] === end ===\r\n");

   z3660_prof_exit();
}

/* Ring dump. Grammar, frozen at version 1 (docs/profiler.md holds the same text):
 *
 *   [PROF] ring hdr <16 space-separated key=value pairs, in z3660_prof_hdr_t order>
 *   [PROF] ring begin
 *   S pppppppp oooo ffff        <- one per sample, oldest first
 *   ...
 *   [PROF] ring end n=<count>
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

   if (n == 0 || (uint64_t)n > have) n = (unsigned)have;
   XTime_GetTime(&wall);

   z3660_printf("[PROF] ring hdr magic=Z3P1 version=%u rec_size=%u rec_count=%u ring_entries=%lu "
                "hz=%lu period_cyc=%lu cpu_hz=%lu\r\n",
                (unsigned)Z3660_PROF_VERSION, (unsigned)Z3660_PROF_REC_SIZE, n,
                (unsigned long)Z3660_PROF_RING_ENTRIES, (unsigned long)z3660_prof_hz,
                (unsigned long)z3660_prof_period, (unsigned long)z3660_prof_cpuhz);
   z3660_printf("[PROF] ring hdr samples=%s drops=%s cyc_span=%s wall_ticks=%s wall_hz=%lu build=0x%02X\r\n",
                z3660_prof_u64(z3660_prof_samples), z3660_prof_u64(z3660_prof_drops),
                z3660_prof_u64(z3660_prof_cyc_base),
                z3660_prof_u64((uint64_t)(wall - z3660_prof_wall0)),
                (unsigned long)COUNTS_PER_SECOND, (unsigned)z3660_prof_build_flags());
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
extern "C" int      z3660_prof_probe_cyc(void)       { return -1; }
extern "C" int      z3660_prof_present(void)         { return 0; }

#endif /* Z3660_EMU_PROF */
