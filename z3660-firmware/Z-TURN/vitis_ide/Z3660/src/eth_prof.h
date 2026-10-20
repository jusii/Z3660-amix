/*
 * eth_prof.h -- the ethernet frame-path profile (the z3660eth timing plan, 2026-10-19, section 7).
 *
 * Build flag ETH_PROF (default 0): with it off every macro below is empty and nothing of the profile is compiled,
 * so the image is the release build. With it on, eth_prof_on (console EPROF, default 1) switches the stamps at run
 * time; the counts that cost no timer read keep counting either way. Intervals keep n, sum and max in global-timer
 * counts (XTime, CPU_CLK/2); ESTAT prints them with the live counts_per_second, ESTATZ zeroes them.
 *
 * Nothing here makes a bus cycle on the Amiga side or changes what the frame path does: every stamp is a read of
 * the SCU global timer, every count a store to core0 RAM, and the GEM statistics registers are read only at ESTAT /
 * ESTATZ (they clear on read; no other code reads them).
 */
#ifndef ETH_PROF_H
#define ETH_PROF_H

#ifndef ETH_PROF
#define ETH_PROF 0
#endif

#if ETH_PROF

#include <stdint.h>
#include "xtime_l.h"

/* natural layout whatever packing an includer leaves on (rtg/gfx.h ends in an unpaired #pragma pack(4)) */
#pragma pack(push, 8)
typedef struct { uint32_t n, max; uint64_t sum; } EP_ACC;

typedef enum {
   /* item 1: ethernet_send_frame() */
   EP_TX_TOTAL,      /* entry -> return, every call */
   EP_TX_FLUSH,      /* entry -> after the L1+L2 flush of the 10 KiB TxFrame */
   EP_TX_QUEUE,      /* flush -> after XEmacPs_Transmit (BD alloc/fill/to-HW) */
   EP_TX_WAIT,       /* Transmit -> the usleep(100) loop sees FramesTx move */
   EP_TX_TAIL,       /* that -> return */
   /* item 2 */
   EP_TX_DONE,       /* XEmacPs_Transmit -> xEmacPsSendHandler at FramesTx++ (DMA + wire, no poll quantum) */
   EP_TX_ISR,        /* xEmacPsSendHandler, whole */
   /* item 4 */
   EP_RX_ISR,        /* xEmacPsRecvHandler, whole */
   EP_RX_COPY,       /* its memcpy into the backlog slot, per frame */
   EP_RX_RES,        /* arrival in the backlog slot -> released by ETH_RX (ethernet_receive_frame) */
   EP_ERR_ISR,       /* xEmacPsErrorHandler, whole (it printf()s on BUFFNA) */
   /* item 3a: the handler inside write_rtg_register / read_rtg_register (both CPU arms) */
   EP_H_TX_W,        /* 0x190 W: ethernet_send_frame */
   EP_H_TX_R,        /* 0x190 R: the send result */
   EP_H_RX_W,        /* 0x194 W: ethernet_receive_frame */
   EP_H_RXA_R,       /* 0x1A4 R: the receive pointer */
   /* item 3c: rtg_thread on the real CPU, per ethernet register: detection -> handler done, handler done -> ack done */
   EP_A_TX_W_SVC, EP_A_TX_W_ACK,
   EP_A_TX_R_SVC, EP_A_TX_R_ACK,
   EP_A_RX_W_SVC, EP_A_RX_W_ACK,
   EP_A_RXA_R_SVC, EP_A_RXA_R_ACK,
   EP_A_OTHER,       /* every other register-page access: detection -> ack done */
   EP_LOOP_PASS,     /* one pass of the real-CPU main loop (main.c): the bound on how long a held 060 cycle waits */
   /* the core0 ISRs that can hold the GEM RX ISR off (handlers do not nest) */
   EP_V_ISR,         /* isr_video, whole */
   EP_V_FLUSH_L1,    /* handle_cache_flush: the whole-L1 flush */
   EP_V_FLUSH_L2,    /* handle_cache_flush: the whole-L2 clean+invalidate by way */
   EP_AUD_ISR,       /* isr_audio_tx, whole */
   EP_NUM
} EP_ID;

/* Sources that run with IRQs masked on core0, for the RX-bunch attribution: the ISRs, and the TX path's L1+L2 range
   flush (the BSP range functions mask IRQs). A bunch is an RX handler entry that drains >= EP_BUNCH frames. */
typedef enum { EP_SRC_VIDEO, EP_SRC_AUDIO, EP_SRC_GEMTX, EP_SRC_GEMERR, EP_SRC_TXFLUSH, EP_SRC_N } EP_SRC;
#define EP_BUNCH         3
#define EP_ISR_BUCKETS   6   /* < 50, 200, 500, 1000, 2000, >= 2000 us */
#define EP_GEM_N        23

#define EP_TX_SLEEPS    12   /* usleep(100) iterations per frame, 0..11 */
#define EP_TX_RCS        5   /* ethernet_send_frame return codes 0..4 */
#define EP_RES_MS       18   /* residence histogram: 1 ms buckets, the last one is >= 17 ms */
#define EP_PASS_BUCKETS 12   /* loop pass histogram: < 1, 2, 4 ... 1024 us, >= 1024 us */
#define EP_SLOTS        64   /* = FRAME_MAX_BACKLOG */

typedef struct {
   EP_ACC acc[EP_NUM];
   uint32_t tx_sleeps[EP_TX_SLEEPS], tx_rc[EP_TX_RCS];
   uint32_t res_ms[EP_RES_MS], pass_hist[EP_PASS_BUCKETS];
   uint32_t ack_spins_max;                 /* rtg_thread ack loop iterations, worst */
   /* item 5 */
   uint32_t bl_full, bl_full_lost;         /* backlog-full discards, frames lost by them (unfetched + the one in hand) */
   uint32_t buffna, rxovr, hrespnok;       /* the error handler's RXSR bits */
   uint32_t rxsr_or, rxsr_buffna, rxsr_rxovr, rxsr_hresp, rxsr_framerx;   /* RXSR as the recv handler read it */
   uint32_t rx_calls, rx_bufs0, rx_bufs_max, rx_frames, bl_max;
   uint32_t rx_isr, rx_isr0, rx_isr_max;      /* ETH_RXRING: recv handler entries, those with no frame, most frames drained in one */
   uint32_t ien_writes;                    /* writes to the ethernet interrupt enable */
   uint64_t gem[EP_GEM_N];                 /* the GEM statistics, summed over ESTAT reads since ESTATZ */
   uint32_t isr_hist[EP_SRC_N][EP_ISR_BUCKETS];
   /* the window since the previous RX handler entry: the longest masked source in it, and the last one */
   uint32_t win_src, win_last_src; XTime win_dur, win_exit, win_last_exit;
   uint32_t bunch_n, bunch_unattr, bunch_by_longest[EP_SRC_N], bunch_by_last[EP_SRC_N];
   XTime bunch_gap_max, bunch_longest_max;
   XTime t_zero, t_xmit;
   XTime t_slot[EP_SLOTS];
} ETH_PROF_T;
#pragma pack(pop)

extern ETH_PROF_T eth_prof;
extern int eth_prof_on;

static inline XTime ep_now(void) { XTime t; XTime_GetTime(&t); return t; }
static inline void ep_acc(int id, XTime d)
{
   EP_ACC *a = &eth_prof.acc[id];
   uint32_t v = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d;
   a->n++; a->sum += d;
   if(v > a->max) a->max = v;
}

/* a masked source ran from t0 to now: its histogram, and the RX window's longest and last */
static inline void ep_src_done(int src, XTime t0)
{
   if(!eth_prof_on || !t0) return;
   extern uint32_t counts_per_second;
   XTime t = ep_now(), d = t - t0;
   uint32_t us = (uint32_t)(d * 1000000ull / counts_per_second), b;
   b = us < 50 ? 0 : us < 200 ? 1 : us < 500 ? 2 : us < 1000 ? 3 : us < 2000 ? 4 : 5;
   eth_prof.isr_hist[src][b]++;
   if(d > eth_prof.win_dur) { eth_prof.win_dur = d; eth_prof.win_src = src; eth_prof.win_exit = t; }
   eth_prof.win_last_src = src; eth_prof.win_last_exit = t;
}
/* RX handler entry t_in drained `frames`: attribute a bunch to the window's longest and last source, then reset */
static inline void ep_rx_window(int frames, XTime t_in)
{
   if(!eth_prof_on || !t_in) return;
   if(frames >= EP_BUNCH) {
      eth_prof.bunch_n++;
      if(!eth_prof.win_dur) eth_prof.bunch_unattr++;
      else {
         eth_prof.bunch_by_longest[eth_prof.win_src]++; eth_prof.bunch_by_last[eth_prof.win_last_src]++;
         if(eth_prof.win_dur > eth_prof.bunch_longest_max) eth_prof.bunch_longest_max = eth_prof.win_dur;
         if(t_in > eth_prof.win_exit && t_in - eth_prof.win_exit > eth_prof.bunch_gap_max)
            eth_prof.bunch_gap_max = t_in - eth_prof.win_exit;
      }
   }
   eth_prof.win_dur = 0; eth_prof.win_exit = 0; eth_prof.win_last_exit = 0;
}
/* rtg_thread (real CPU): which row an access belongs to. The driver's four: 0x190 W, 0x190 R, 0x194 W, 0x1A4 R;
   -1 = any other access (A_OTHER) */
static inline int ep_reg_idx(uint32_t gpio)
{
   uint32_t a = gpio & 0x1FFFFC;
   if((gpio & 0xF80000) == 0xF80000) return -1;
   if(gpio & 0x80000000) return a == 0x190 ? EP_A_TX_W_SVC : a == 0x194 ? EP_A_RX_W_SVC : -1;
   return a == 0x190 ? EP_A_TX_R_SVC : a == 0x1A4 ? EP_A_RXA_R_SVC : -1;
}
/* rtg_thread after the ack loop: detection -> handler done (svc), handler done -> ack done (ack) */
static inline void ep_ack_done(int reg, XTime td, XTime th, uint32_t spins)
{
   if(spins > eth_prof.ack_spins_max) eth_prof.ack_spins_max = spins;
   if(!eth_prof_on || !td || !th) return;
   XTime tn = ep_now();
   if(reg >= 0) { ep_acc(reg, th - td); ep_acc(reg + 1, tn - th); }
   else ep_acc(EP_A_OTHER, tn - td);
}
/* the real-CPU main loop: one pass, with its log2 histogram in us */
static inline void ep_pass(XTime *last)
{
   if(!eth_prof_on) { *last = 0; return; }
   XTime t = ep_now();
   if(*last) {
      XTime d = t - *last;
      ep_acc(EP_LOOP_PASS, d);
      extern uint32_t counts_per_second;
      uint32_t us = (uint32_t)(d * 1000000ull / counts_per_second), b = 0;
      while(b < EP_PASS_BUCKETS - 1 && us >= (1u << b)) b++;
      eth_prof.pass_hist[b]++;
   }
   *last = t;
}

void eth_prof_dump(void);    /* ESTAT */
void eth_prof_zero(void);    /* ESTATZ */
void eth_prof_toggle(void);  /* EPROF */

/* A stamp: 0 while the switch is off, so an interval opened while off is not charged when it closes. */
#define EP_T(v)              XTime v = eth_prof_on ? ep_now() : 0
#define EP_SET(v)            do { v = eth_prof_on ? ep_now() : 0; } while(0)
#define EP_SPAN(id,a,b)      do { if((a) && (b)) ep_acc((id), (b)-(a)); } while(0)
#define EP_END(id,a)         do { if(eth_prof_on && (a)) ep_acc((id), ep_now()-(a)); } while(0)
#define EP_CNT(x)            do { eth_prof.x++; } while(0)
#define EP_DO(x)             do { x; } while(0)
#define EP_DECL(t,v,init)    t v = (init)
/* the console commands (debug_console.c), appended to its enum, its names and its switch */
#define EP_COMMANDS          ESTAT, ETH_PROF_DUMP, ESTATZ, ETH_PROF_ZERO, EPROF, ETH_PROF_SWITCH,
#define EP_COMMAND_NAMES     "ESTAT", "ETH PROF DUMP", "ESTATZ", "ETH PROF ZERO", "EPROF", "ETH PROF SWITCH",
#define EP_COMMAND_CASES \
   case ESTAT:  case ETH_PROF_DUMP:   eth_prof_dump();   debug_console.subcmd=0; break; \
   case ESTATZ: case ETH_PROF_ZERO:   eth_prof_zero();   debug_console.subcmd=0; break; \
   case EPROF:  case ETH_PROF_SWITCH: eth_prof_toggle(); debug_console.subcmd=0; break;

#else  /* !ETH_PROF */

#define EP_T(v)              ((void)0)
#define EP_SET(v)            ((void)0)
#define EP_SPAN(id,a,b)      ((void)0)
#define EP_END(id,a)         ((void)0)
#define EP_CNT(x)            ((void)0)
#define EP_DO(x)             ((void)0)
#define EP_DECL(t,v,init)    ((void)0)
#define EP_COMMANDS
#define EP_COMMAND_NAMES
#define EP_COMMAND_CASES

#endif /* ETH_PROF */

#endif /* ETH_PROF_H */
