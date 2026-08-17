// SPDX-License-Identifier: MIT
/* --- core0 cooperative-loop per-task duration accounting (C0STAT) ------------
 *
 * WHY THIS EXISTS
 *
 * The piscsi instrumentation in scsi/scsi.c (PISCSI_STATS) measures the request
 * itself -- seek, io, cache -- plus a `gap` figure: the wall time from the end of
 * one serviced request to the start of the next. On the 2026-08-17 metal install
 * that split came back unambiguous:
 *
 *     228278 requests, gap avg 18384us, gap MAX 118019148us (118 seconds)
 *     io R avg 338us, io W avg 1382us  -- flat, all run long
 *
 * So the SD card and FatFs are innocent, and something in core0's cooperative
 * main loop (main.c, the CPU_EMULATOR arm) occasionally blocks for tens of
 * SECONDS. `gap` cannot say which callee, and it cannot even say whether the
 * stall was firmware-side at all -- a long gap also happens if the guest simply
 * stopped asking. This module answers both: it times EVERY callee in the loop's
 * dispatch chain individually, so a 118 s stall is either attributed to a named
 * callee (firmware) or to none of them (the guest went quiet).
 *
 * DESIGN NOTES THAT MATTER
 *
 * - Measured AROUND each call, never sampled. A max-duration record is written
 *   the instant the call returns, so it survives a reporting window that was
 *   stalled for the entire block. A single call at or above
 *   CORE0_STATS_BLOCK_US additionally gets its own console line immediately,
 *   which is the whole point: naming the offender must not depend on the window.
 *
 * - Chained stamps, not begin/end pairs. The end timestamp of callee N is the
 *   start timestamp of callee N+1, so a loop iteration costs ONE
 *   XTime_GetTime() per timed callee instead of two. The bookkeeping between
 *   calls (a counter bump, an if) is charged to the following span; it is a
 *   handful of instructions.
 *
 * - Parent and child spans both count, and both are reported. `>` rows are
 *   CONTAINED IN the row above them (EMU>SCSIW is inside EMU). The worst-call
 *   scan walks the table backwards so the most specific row wins a tie against
 *   its parent.
 *
 * - Console output is discounted. Emitting a report is tens of milliseconds of
 *   blocking UART, which would otherwise be charged to whatever ran next and
 *   fake a block. Bracketed output adds to c0stat_excl, and every open span
 *   subtracts the delta at close.
 *
 * - Interrupt time is NOT discounted. isr_video (~60 Hz) and isr_audio_tx land
 *   in whichever span happens to be open. That is a small error spread evenly
 *   over all spans; it cannot manufacture a multi-second max, so the ISRs are
 *   deliberately left untouched.
 *
 * - Timebase: same caveat as PISCSI_STATS. The Cortex-A9 global timer runs at
 *   CPU_CLK/2 and this board retunes its ARM clock at runtime, so microseconds
 *   come from the live `counts_per_second`. Ratios and step changes are what
 *   discriminate; absolute values are uncalibrated.
 *
 * SERIAL COMMANDS (debug_console.c)
 *   PSTAT   toggle the periodic reports (piscsi AND core0, together)
 *   PSTATC  dump core0 per-task cumulative totals -- non-destructive
 *   PSTATZ  zero the core0 cumulative totals and the worst-ever latch
 *   PSTATR  (pre-existing) dump piscsi cumulative totals and reset them
 *
 * Set CORE0_STATS to 0 to compile every span, counter and report out.
 */
#ifndef CORE0_STATS_H
#define CORE0_STATS_H

#include <stdint.h>

#define CORE0_STATS            1

/* Periodic reporting window, seconds. */
#define CORE0_STATS_PERIOD_S   5
/* A window is printed when the guest moved piscsi traffic in it, OR when the
 * window's worst single call reached this. A healthy idle box hits neither and
 * stays silent -- same discipline as PISCSI_STATS. */
#define CORE0_STATS_NOTABLE_US 2000u
/* A single call at or above this gets its own console line the moment it
 * returns, independent of the window. */
#define CORE0_STATS_BLOCK_US   100000u
/* Rows per periodic report: top-N by cumulative window time (the task holding
 * the window's worst single call is always added if it is not already in). */
#define CORE0_STATS_TOP        3

/* Task ids. Order is loop order, and a parent is immediately followed by its
 * children (the worst-call scan relies on that). Keep C0T_NUM <= 32: the
 * top-N picker uses a uint32_t bitmap. */
typedef enum {
   C0T_DEBUG = 0,     /* debug_thread()             -- serial console + PSTAT* */
   C0T_MEASURES,      /* measures_thread()          -- XADC + LTC2990 over I2C */
   C0T_INTERRUPT,     /* interrupt_thread()                                    */
   C0T_EMULATOR,      /* emulator_thread()          -- the mailbox service     */
   C0T_EMU_RTG_WR,    /*    write_rtg_register()     (RTG blits live here)     */
   C0T_EMU_RTG_RD,    /*    read_rtg_register()                               */
   C0T_EMU_SCSI_WR,   /*    handle_piscsi_reg_write()  -- f_read/f_write       */
   C0T_EMU_SCSI_RD,   /*    handle_piscsi_read()                              */
   C0T_EMU_RSTE,      /*    cpu_emulator_reset()                              */
   C0T_EMU_HOLD,      /*    the core0_hold usleep(1000) spin                  */
   C0T_EMURESET,      /* emulator_reset_thread()    -- 1 iteration in 40       */
   C0T_ETHERNET,      /* ethernet_thread()                                     */
   C0T_USB,           /* usb_thread()               -- usb_proxy_handle_command*/
   C0T_OTHER,         /* other_tasks()                                         */
   C0T_OT_MPEG,       /*    pl_mpeg_arm_decode_progressive() -- EVERY iteration*/
   C0T_OT_ETHTASK,    /*    ethernet_task()         -- 1 iteration in 300000   */
   C0T_OT_AUDIO,      /*    audio_silence/audio_init_i2s re-init               */
   C0T_NUM
} C0_TASK;

/* Always linked, so debug_console.c can call them unconditionally. */
void core0_stats_dump(void);        /* PSTATC */
void core0_stats_zero(void);        /* PSTATZ */
void core0_stats_set_report(int on);

#if CORE0_STATS

#include "xtime_l.h"

/* An open span: when it started, and how much excluded (console) time had been
 * charged off at that moment. */
typedef struct { XTime t; uint64_t x; } C0_SPAN_T;

extern uint64_t c0stat_excl;        /* ticks charged to nobody (console output) */

void c0stat_init(void);
void c0stat_acc(int id, uint64_t ticks);
void c0stat_chain_stamp(int id);
void c0stat_span_open(C0_SPAN_T *s);
void c0stat_span_close(const C0_SPAN_T *s, int id);
void c0stat_iter_end(void);
void c0stat_excl_open(C0_SPAN_T *s);
void c0stat_excl_close(const C0_SPAN_T *s);

/* Loop chain: arm once before the loop, then one stamp per timed callee. */
#define C0_INIT()          c0stat_init()
#define C0_STAMP(id)       c0stat_chain_stamp(id)
#define C0_ITER()          c0stat_iter_end()
/* Nested span for a callee reached from inside a timed callee. */
#define C0_SPAN(v)         C0_SPAN_T v; c0stat_span_open(&(v))
#define C0_SPAN_END(v,id)  c0stat_span_close(&(v),(id))
/* Bracket blocking console output so it is charged to no span at all. */
#define C0_EXCL(v)         C0_SPAN_T v; c0stat_excl_open(&(v))
#define C0_EXCL_END(v)     c0stat_excl_close(&(v))

#else  /* !CORE0_STATS */

#define C0_INIT()          ((void)0)
#define C0_STAMP(id)       ((void)0)
#define C0_ITER()          ((void)0)
#define C0_SPAN(v)         ((void)0)
#define C0_SPAN_END(v,id)  ((void)0)
#define C0_EXCL(v)         ((void)0)
#define C0_EXCL_END(v)     ((void)0)

#endif /* CORE0_STATS */

#endif /* CORE0_STATS_H */
