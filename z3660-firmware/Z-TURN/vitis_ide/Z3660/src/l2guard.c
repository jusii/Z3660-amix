/*
 * l2guard.c -- core 0's guarded whole-L2 flush and the abort record (see l2guard.h).
 */
#include <stdio.h>
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_cache_l.h"
#include "xl2cc.h"
#include "xparameters_ps.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#include "xtime_l.h"

#define L2G_GET_CPSR()   mfcpsr()
#define L2G_SET_CPSR(v)  mtcpsr(v)
#define L2G_FLUSH()      Xil_L2CacheFlush()
#include "l2guard.h"

volatile struct l2g_state z3660_l2g;

/* In .noinit: the C runtime does not clear it and the boot image does not load it, so the
 * record written before the abort handler's reboot is still in DDR on the next boot. */
static volatile struct l2g_record l2g_saved __attribute__((section(".noinit"), aligned(32)));

void z3660_l2_flush(uint32_t site)
{
   l2g_flush(&z3660_l2g, site);
}

static uint32_t pl310(uint32_t off)
{
   return Xil_In32(XPS_L2CC_BASEADDR + off);
}

/* Called first thing in the abort handlers, in abort mode */
void z3660_abort_capture(uint32_t kind, uint32_t pc)
{
   volatile struct l2g_record *r = &l2g_saved;
   uint32_t spsr, i;

   r->pl310_ris = pl310(XPS_L2CC_ISR_OFFSET);
   r->pl310_way = pl310(XPS_L2CC_CACHE_INV_CLN_WAY_OFFSET);
   r->pl310_dbg = pl310(XPS_L2CC_DEBUG_CTRL_OFFSET);
   __asm__ __volatile__("mrs %0, spsr" : "=r"(spsr));
   r->kind = kind;
   if (kind == L2G_KIND_DATA) {
      r->fsr = mfcp(XREG_CP15_DATA_FAULT_STATUS);
      r->far = mfcp(XREG_CP15_DATA_FAULT_ADDRESS);
      r->afsr = mfcp(XREG_CP15_AUX_DATA_FAULT_STATUS);
   } else {
      r->fsr = mfcp(XREG_CP15_INST_FAULT_STATUS);
      r->far = mfcp(XREG_CP15_INST_FAULT_ADDRESS);
      r->afsr = mfcp(XREG_CP15_AUX_INST_FAULT_STATUS);
   }
   r->pc = pc;
   r->spsr = spsr;
   r->depth = z3660_l2g.depth;
   r->site = z3660_l2g.site;
   r->last = z3660_l2g.last;
   r->count = z3660_l2g.count;
   r->nested = z3660_l2g.nested;
   l2g_record_seal(r);

   /* to DDR: L1 by address, then L2 by address once no by-way operation is running */
   Xil_L1DCacheFlushRange((u32)r, sizeof(*r));
   for (i = 0; i < 1000000U && (pl310(XPS_L2CC_CACHE_INV_CLN_WAY_OFFSET) & 0xFFFFU); i++)
      ;
   Xil_L2CacheFlushRange((u32)r, sizeof(*r));
}

void z3660_abort_print(void)
{
   char buf[320];
   l2g_record_format(&l2g_saved, buf, sizeof(buf));
   printf("[ABORT] %s", buf);
}

/* After the banner: the previous boot's record, if it ended in an abort */
void z3660_abort_report(void)
{
   char buf[320];

   if (l2g_record_valid(&l2g_saved)) {
      l2g_record_format(&l2g_saved, buf, sizeof(buf));
      printf("[ABORT] The previous boot ended in a %s", buf);
   }
   l2g_saved.magic = 0;
   /* fresh PL310 error bits for this boot */
   Xil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_IAR_OFFSET, 0x1FFU);
}

/* Test builds only: see l2guard.h */
void z3660_l2_stress(void)
{
#ifdef L2G_DIAG_STRESS
   static XTime end;
   static int done;
   XTime now;

   if (done)
      return;
   XTime_GetTime(&now);
   if (!end) {
      end = now + 120ULL * COUNTS_PER_SECOND;
#ifdef L2G_DIAG_UNMASKED
      printf("[L2G] stress: whole-L2 flush every pass for 120 s, interrupts NOT masked\n");
#else
      printf("[L2G] stress: whole-L2 flush every pass for 120 s, interrupts masked\n");
#endif
   }
   if (now < end) {
      z3660_l2_flush(L2G_STRESS);
      return;
   }
   done = 1;
   printf("[L2G] stress done: %lu flushes, %lu nested\n",
          (unsigned long)z3660_l2g.count, (unsigned long)z3660_l2g.nested);
#endif
}
