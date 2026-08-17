// SPDX-License-Identifier: MIT
/* core0 cooperative-loop per-task duration accounting. See core0_stats.h for
 * the rationale, the measurement discipline and the console commands. */

#include <stdio.h>
#include <string.h>
#include "core0_stats.h"

#if CORE0_STATS

extern uint32_t counts_per_second;      /* tracks the live ARM clock (main.c) */

/* Row labels. Must line up 1:1 with C0_TASK. A leading "..>" marks a span that
 * is CONTAINED IN the row above it. Longest label sets the %-11s width. */
static const char * const c0_name[C0T_NUM] = {
   "DEBUG",
   "MEASURES",
   "INTERRUPT",
   "EMU",
   "EMU>RTGW",
   "EMU>RTGR",
   "EMU>SCSIW",
   "EMU>SCSIR",
   "EMU>RSTE",
   "EMU>HOLD",
   "EMURESET",
   "ETHERNET",
   "USB",
   "OTHER",
   "OTHER>MPEG",
   "OTHER>ETHT",
   "OTHER>AUDIO",
};

typedef struct {
   uint32_t calls,   calls_tot;
   uint64_t cum,     cum_tot;
   uint64_t max,     max_tot;        /* worst SINGLE call, window / all time  */
} C0_ROW;

static C0_ROW   c0[C0T_NUM];

uint64_t        c0stat_excl;         /* console-output ticks, charged to nobody */
static C0_SPAN_T c0chain;            /* the chained loop-stamp cursor           */

static XTime    c0_up_start, c0_win_start, c0_iter_start;
static uint64_t c0_iter_excl;
static uint32_t c0_win_iters;
static uint64_t c0_iters_tot;
static uint64_t c0_win_iter_cum, c0_win_iter_max;
static uint64_t c0_iter_cum_tot, c0_iter_max_tot;
static uint32_t c0_win_bounds;        /* span boundaries this window            */
static uint64_t c0_bounds_tot;
static uint64_t c0_probe;             /* measured cost of one XTime_GetTime     */
static uint64_t c0_bound_cost;        /* measured cost of a WHOLE span boundary */

static uint64_t c0_worst_ticks;       /* worst single call ever, and who/when   */
static int      c0_worst_id = -1;
static uint64_t c0_worst_iter;
static uint32_t c0_block_n, c0_block_lines;

static int      c0_armed;             /* c0stat_init() has run                  */
int             core0_stats_report = 1;

/* Derived from counts_per_second, refreshed whenever the ARM clock is retuned. */
static uint32_t c0_cps = 1;
static uint64_t c0_block_ticks, c0_notable_ticks, c0_period_ticks;

/* noinline for the same reason as the reporting helpers: this runs at most once
 * per ARM-clock retune, but inlining its three 64-bit divides into c0stat_acc()
 * would cost every span boundary a much larger stack frame. */
static __attribute__((noinline)) void c0_recalc(void)
{
   uint32_t cps = counts_per_second;
   if(cps < 2u) cps = 2u;                        /* never divide by zero */
   c0_cps           = cps;
   c0_block_ticks   = (uint64_t)cps * (uint64_t)CORE0_STATS_BLOCK_US   / 1000000ULL;
   c0_notable_ticks = (uint64_t)cps * (uint64_t)CORE0_STATS_NOTABLE_US / 1000000ULL;
   c0_period_ticks  = (uint64_t)cps * (uint64_t)CORE0_STATS_PERIOD_S;
}

/* Microseconds. Clamped rather than wrapped: a single span can legitimately be
 * two minutes, and 0xFFFFFFFF us is ~4295 s, so only an absurd value clamps. */
static uint32_t c0_us(uint64_t t)
{
   uint64_t v;
   if(t > 4000000000000ULL) return 0xFFFFFFFFu;  /* ~3.3h at 333MHz */
   v = t * 1000000ULL / (uint64_t)c0_cps;
   return (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)v;
}

/* Milliseconds -- used for every CUMULATIVE figure, because a multi-hour run
 * overflows a 32-bit microsecond count. */
static uint32_t c0_ms(uint64_t t)
{
   uint64_t v = t / (uint64_t)c0_cps;            /* seconds, then scale up */
   if(v > 4000000ULL) return 0xFFFFFFFFu;
   v = t * 1000ULL / (uint64_t)c0_cps;
   return (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)v;
}

/* Measure this instrumentation's own cost, on the metal, at the clock it will
 * actually run at -- so the `ovh=` figure in every report is evidence and not an
 * estimate. Two numbers:
 *   c0_probe      one XTime_GetTime() (3 reads of the SCU global timer)
 *   c0_bound_cost one COMPLETE span boundary: the timer read PLUS everything
 *                 c0stat_acc() does. This is the number that matters, and the
 *                 only honest way to get it is to run boundaries and divide.
 * Called from c0stat_init() BEFORE the counters are zeroed, so the calibration
 * traffic leaves nothing behind. */
static void c0_calibrate(void)
{
   int i;
   uint64_t best = ~0ULL;
   XTime a, b;

   for(i = 0; i < 8; i++)                        /* min of 8 = one timer read */
   {
      XTime_GetTime(&a);
      XTime_GetTime(&b);
      if((uint64_t)(b - a) < best) best = (uint64_t)(b - a);
   }
   c0_probe = best;

   XTime_GetTime(&a);
   for(i = 0; i < 256; i++) c0stat_chain_stamp(C0T_DEBUG);
   XTime_GetTime(&b);
   c0_bound_cost = (uint64_t)(b - a) / 256ULL;
}

/* ------------------------------------------------------------------ reporting */

/* permille of `el` that `part` accounts for */
static uint32_t c0_permille(uint64_t part, uint64_t el)
{
   if(el == 0) return 0;
   if(part > 1000000000000000ULL) return 1000;   /* part*1000 must not wrap u64 */
   return (uint32_t)(part * 1000ULL / el);
}

/* noinline on the three reporting helpers below is load-bearing, not cosmetic.
 * At -O3 gcc happily inlines them into c0stat_acc() / c0stat_iter_end() -- the
 * two functions on the hot path -- and the inlined printf machinery then forces
 * a 52..156 byte stack frame and a vpush/vpop of d8-d9 on EVERY span boundary.
 * Kept out of line, the hot path is a handful of loads, a 64-bit subtract and
 * two compares. Verified in the disassembly. */
static __attribute__((noinline)) void c0_row(int id, uint64_t el)
{
   uint32_t pm = c0_permille(c0[id].cum, el);
   printf("[C0STAT]  %-11s %6luc cum %8lums max %10luus %3lu.%lu%%\n",
      c0_name[id], (unsigned long)c0[id].calls,
      (unsigned long)c0_ms(c0[id].cum), (unsigned long)c0_us(c0[id].max),
      (unsigned long)(pm / 10u), (unsigned long)(pm % 10u));
}

static __attribute__((noinline)) void c0stat_window(XTime now)
{
   uint64_t el = (uint64_t)(now - c0_win_start);
   uint64_t worst_t = 0;
   int      worst = -1;
   int      i;

   /* Worst single call this window. Backwards, so a child beats its parent when
    * they are (near-)equal -- the deeper row names the actual offender. */
   for(i = C0T_NUM - 1; i >= 0; i--)
      if(c0[i].max > worst_t) { worst_t = c0[i].max; worst = i; }

   /* Idle-silent: speak only if the guest moved piscsi traffic, or something
    * took long enough to be a finding on its own. */
   if(core0_stats_report && el != 0 &&
      (worst_t >= c0_notable_ticks ||
       c0[C0T_EMU_SCSI_WR].calls != 0 || c0[C0T_EMU_SCSI_RD].calls != 0))
   {
      uint32_t ovh = c0_permille((uint64_t)c0_win_bounds * c0_bound_cost, el);
      int pick[CORE0_STATS_TOP + 1];
      int npick = 0;
      uint32_t taken = 0;
      int k;

      C0_EXCL(g);                                /* do not charge this printf */

      printf("[C0STAT] win %lums it=%lu (%lu/s) iter avg %luus max %luus ovh=%lu.%lu%% worst=%s %luus\n",
         (unsigned long)c0_ms(el), (unsigned long)c0_win_iters,
         (unsigned long)((uint64_t)c0_win_iters * (uint64_t)c0_cps / el),
         (unsigned long)(c0_win_iters ? c0_us(c0_win_iter_cum / c0_win_iters) : 0u),
         (unsigned long)c0_us(c0_win_iter_max),
         (unsigned long)(ovh / 10u), (unsigned long)(ovh % 10u),
         (worst >= 0) ? c0_name[worst] : "-", (unsigned long)c0_us(worst_t));

      for(k = 0; k < CORE0_STATS_TOP; k++)       /* top-N by cumulative time */
      {
         int best = -1;
         uint64_t bt = 0;
         for(i = 0; i < C0T_NUM; i++)
            if(((taken & (1u << i)) == 0u) && c0[i].calls != 0 &&
               (best < 0 || c0[i].cum > bt)) { bt = c0[i].cum; best = i; }
         if(best < 0) break;
         taken |= (1u << best);
         pick[npick++] = best;
      }
      if(worst >= 0 && (taken & (1u << worst)) == 0u) pick[npick++] = worst;
      for(k = 0; k < npick; k++) c0_row(pick[k], el);

      C0_EXCL_END(g);
   }

   for(i = 0; i < C0T_NUM; i++) { c0[i].calls = 0; c0[i].cum = 0; c0[i].max = 0; }
   c0_win_iters = 0; c0_win_iter_cum = 0; c0_win_iter_max = 0; c0_win_bounds = 0;
   c0_win_start = now;
}

/* A single call long enough to matter on its own. Printed the moment it returns,
 * so the offender is named even though the window was stalled for the whole of
 * it. Rate limited because this runs non-preemptibly on core0. */
static __attribute__((noinline)) void c0stat_block_line(int id, uint64_t t)
{
   c0_block_n++;
   if(c0_block_n > 32u && (c0_block_n & 15u) != 0u) return;
   c0_block_lines++;
   {
      C0_EXCL(g);
      printf("[C0STAT] *BLOCK* #%lu %s %luus (loop iter %lu, call %lu of that task)\n",
         (unsigned long)c0_block_n, c0_name[id], (unsigned long)c0_us(t),
         (unsigned long)c0_iters_tot, (unsigned long)c0[id].calls_tot);
      C0_EXCL_END(g);
   }
}

/* ---------------------------------------------------------------- hot path */

void c0stat_acc(int id, uint64_t ticks)
{
   C0_ROW *s = &c0[id];

   if(counts_per_second != c0_cps) c0_recalc();  /* ARM clock was retuned */

   s->calls++;      s->calls_tot++;
   s->cum += ticks; s->cum_tot += ticks;
   if(ticks > s->max)     s->max     = ticks;
   if(ticks > s->max_tot) s->max_tot = ticks;
   c0_win_bounds++; c0_bounds_tot++;

   if(ticks > c0_worst_ticks)
   {
      c0_worst_ticks = ticks;
      c0_worst_id    = id;
      c0_worst_iter  = c0_iters_tot;
   }
   if(ticks >= c0_block_ticks) c0stat_block_line(id, ticks);
}

void c0stat_span_open(C0_SPAN_T *s)
{
   XTime_GetTime(&s->t);
   s->x = c0stat_excl;
}

void c0stat_span_close(const C0_SPAN_T *s, int id)
{
   XTime n;
   uint64_t raw, ex;
   XTime_GetTime(&n);
   raw = (uint64_t)(n - s->t);
   ex  = c0stat_excl - s->x;
   c0stat_acc(id, (raw > ex) ? (raw - ex) : 0ULL);
}

/* Close the current chain span and open the next one with the SAME timer read.
 * The cursor is re-armed BEFORE c0stat_acc() runs, because acc() may emit a
 * *BLOCK* line -- whose cost must land in the next span's excluded delta, not
 * in the next span's duration. */
void c0stat_chain_stamp(int id)
{
   XTime n;
   uint64_t raw, ex;
   XTime_GetTime(&n);
   raw = (uint64_t)(n - c0chain.t);
   ex  = c0stat_excl - c0chain.x;
   c0chain.t = n;
   c0chain.x = c0stat_excl;
   c0stat_acc(id, (raw > ex) ? (raw - ex) : 0ULL);
}

/* End of one loop iteration. Costs zero timer reads: the last chain stamp
 * already holds "now". */
void c0stat_iter_end(void)
{
   XTime    now = c0chain.t;
   uint64_t ex  = c0stat_excl;
   uint64_t it  = (uint64_t)(now - c0_iter_start);
   uint64_t exd = ex - c0_iter_excl;

   it = (it > exd) ? (it - exd) : 0ULL;
   c0_iters_tot++; c0_win_iters++;
   c0_win_iter_cum += it; c0_iter_cum_tot += it;
   if(it > c0_win_iter_max) c0_win_iter_max = it;
   if(it > c0_iter_max_tot) c0_iter_max_tot = it;

   c0_iter_start = now;
   c0_iter_excl  = ex;                           /* pre-report value on purpose */

   if((uint64_t)(now - c0_win_start) >= c0_period_ticks) c0stat_window(now);
}

void c0stat_excl_open(C0_SPAN_T *s)
{
   XTime_GetTime(&s->t);
   s->x = 0;
}

void c0stat_excl_close(const C0_SPAN_T *s)
{
   XTime n;
   XTime_GetTime(&n);
   c0stat_excl += (uint64_t)(n - s->t);
}

/* ------------------------------------------------------------------ lifecycle */

void c0stat_init(void)
{
   c0_recalc();
   c0stat_excl = 0;
   XTime_GetTime(&c0chain.t);
   c0chain.x = 0;
   c0_calibrate();                     /* needs an armed chain; zeroed below */

   memset(c0, 0, sizeof c0);
   c0_win_iters = 0; c0_iters_tot = 0;
   c0_win_iter_cum = 0; c0_win_iter_max = 0;
   c0_iter_cum_tot = 0; c0_iter_max_tot = 0;
   c0_win_bounds = 0; c0_bounds_tot = 0;
   c0_worst_ticks = 0; c0_worst_id = -1; c0_worst_iter = 0;
   c0_block_n = 0; c0_block_lines = 0;
   c0stat_excl = 0;
   XTime_GetTime(&c0chain.t);
   c0chain.x     = 0;
   c0_up_start   = c0chain.t;
   c0_win_start  = c0chain.t;
   c0_iter_start = c0chain.t;
   c0_iter_excl  = 0;
   c0_armed      = 1;
   printf("[C0STAT] core0 loop accounting ON: %d spans, window %ds, block>=%luus; "
          "cost/boundary %luns (timer read %luns) -- PSTAT/PSTATC/PSTATZ\n",
      (int)C0T_NUM, (int)CORE0_STATS_PERIOD_S, (unsigned long)CORE0_STATS_BLOCK_US,
      (unsigned long)(c0_bound_cost * 1000000000ULL / (uint64_t)c0_cps),
      (unsigned long)(c0_probe      * 1000000000ULL / (uint64_t)c0_cps));
}

void core0_stats_set_report(int on)
{
   core0_stats_report = on ? 1 : 0;
}

void core0_stats_dump(void)
{
   XTime now;
   uint64_t up;
   int i;

   if(!c0_armed)
   {
      printf("[C0STAT] no data: the instrumented core0 loop has not started "
             "(EMU boot modes only)\n");
      return;
   }
   XTime_GetTime(&now);
   up = (uint64_t)(now - c0_up_start);
   if(up == 0) up = 1;

   printf("[C0STAT] --- core0 loop cumulative --- up %lus it=%lu (%lu/s) cps=%lu\n",
      (unsigned long)(up / (uint64_t)c0_cps), (unsigned long)c0_iters_tot,
      (unsigned long)((uint64_t)c0_iters_tot * (uint64_t)c0_cps / up),
      (unsigned long)c0_cps);
   printf("[C0STAT]  %-11s %7s %10s %12s %6s\n",
      "task", "calls", "cum", "max_single", "%up");
   for(i = 0; i < C0T_NUM; i++)
   {
      uint32_t pm = c0_permille(c0[i].cum_tot, up);
      printf("[C0STAT]  %-11s %6luc %8lums %10luus %3lu.%lu%%\n",
         c0_name[i], (unsigned long)c0[i].calls_tot,
         (unsigned long)c0_ms(c0[i].cum_tot), (unsigned long)c0_us(c0[i].max_tot),
         (unsigned long)(pm / 10u), (unsigned long)(pm % 10u));
   }
   {
      uint32_t pm = c0_permille(c0_iter_cum_tot, up);
      printf("[C0STAT]  %-11s %6luc %8lums %10luus %3lu.%lu%%  (whole loop iteration)\n",
         "ITERATION", (unsigned long)c0_iters_tot,
         (unsigned long)c0_ms(c0_iter_cum_tot), (unsigned long)c0_us(c0_iter_max_tot),
         (unsigned long)(pm / 10u), (unsigned long)(pm % 10u));
   }
   if(c0_worst_id >= 0)
      printf("[C0STAT] worst single call ever: %s %luus at loop iteration %lu\n",
         c0_name[c0_worst_id], (unsigned long)c0_us(c0_worst_ticks),
         (unsigned long)c0_worst_iter);
   printf("[C0STAT] calls >=%luus: %lu (%lu reported, rest rate-limited)\n",
      (unsigned long)CORE0_STATS_BLOCK_US, (unsigned long)c0_block_n,
      (unsigned long)c0_block_lines);
   {
      uint32_t pm = c0_permille(c0_bounds_tot * c0_bound_cost, up);
      printf("[C0STAT] overhead: %lu span boundaries x %luns measured -> %lu.%lu%% of "
             "uptime; console time excluded so far %lums\n",
         (unsigned long)c0_bounds_tot,
         (unsigned long)(c0_bound_cost * 1000000000ULL / (uint64_t)c0_cps),
         (unsigned long)(pm / 10u), (unsigned long)(pm % 10u),
         (unsigned long)c0_ms(c0stat_excl));
   }
   printf("[C0STAT] note: 'A>B' rows are contained in the 'A' row; ISR time "
          "(isr_video ~60Hz) is charged to whichever span was open.\n");
}

void core0_stats_zero(void)
{
   int i;
   if(!c0_armed) { printf("[C0STAT] nothing to zero (loop not started)\n"); return; }
   for(i = 0; i < C0T_NUM; i++)
   {
      c0[i].calls_tot = 0; c0[i].cum_tot = 0; c0[i].max_tot = 0;
   }
   c0_iter_cum_tot = 0; c0_iter_max_tot = 0; c0_iters_tot = 0;
   c0_bounds_tot = 0;
   c0_worst_ticks = 0; c0_worst_id = -1; c0_worst_iter = 0;
   c0_block_n = 0; c0_block_lines = 0;
   XTime_GetTime(&c0_up_start);
   printf("[C0STAT] cumulative totals zeroed\n");
}

#else /* !CORE0_STATS */

#include <stdio.h>

void core0_stats_dump(void)          { printf("[C0STAT] not compiled in (CORE0_STATS=0)\n"); }
void core0_stats_zero(void)          { printf("[C0STAT] not compiled in (CORE0_STATS=0)\n"); }
void core0_stats_set_report(int on)  { (void)on; }

#endif /* CORE0_STATS */
