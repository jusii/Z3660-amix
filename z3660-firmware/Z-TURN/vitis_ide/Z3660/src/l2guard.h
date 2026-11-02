/*
 * l2guard.h -- core 0's whole-L2 clean+invalidate, and the record an abort leaves behind.
 *
 * Xil_L2CacheFlush() starts the PL310's clean+invalidate by way, polls it to the end and
 * restores the debug control register, all with interrupts enabled. Core 0 calls it from the
 * foreground and from the vblank interrupt (video.c handle_cache_flush), so an interrupt can
 * start a second by-way operation inside the first and clear the write-back inhibit under it.
 * Every caller goes through z3660_l2_flush(site), which masks IRQ and FIQ for the duration
 * (as the BSP's Xil_DCacheFlush and Xil_L2CacheFlushRange already do) and notes the call site
 * and nesting depth.
 *
 * Core 0 has taken an asynchronous external abort (DFSR 0x1C06) inside that poll; the fault
 * address and direction of an asynchronous abort are UNKNOWN, so the abort handlers capture
 * what does tell the causes apart -- the flush site and depth, the PL310's raw interrupt
 * status (DECERR/SLVERR on its own master port), its live way-operation and debug-control
 * state, SPSR_abt (which mode was interrupted) and the auxiliary fault status -- into a
 * record in DDR that the C runtime does not clear. The next boot prints it after the banner.
 *
 * The logic here is header-only so the host tests can drive it; the includer supplies
 * L2G_GET_CPSR(), L2G_SET_CPSR(v) and L2G_FLUSH() before calling l2g_flush().
 */
#ifndef Z3660_L2GUARD_H
#define Z3660_L2GUARD_H

#include <stdint.h>
#include <stdio.h>

/* Call sites of the whole-L2 flush. Stored with the CPU mode at entry in bits 20..16. */
enum l2g_site {
   L2G_NONE = 0,
   L2G_VIDEO_FLUSH,     /* video.c handle_cache_flush: vblank interrupt and mode set */
   L2G_VIDEO_PLAY,      /* video.c play_init */
   L2G_MAIN_INIT,       /* main.c after the piscsi/pifloppy init */
   L2G_LOAD_ROM,        /* cpu_emulator.c load_rom */
   L2G_LOAD_ROMEXT,     /* cpu_emulator.c load_romext */
   L2G_CPU_FREQ,        /* set_cpu_freq.c set_cpu_frequency */
   L2G_FLOPPY_INIT,     /* floppy.c pifloppy_init */
   L2G_FLOPPY_REG,      /* floppy.c handle_pifloppy_reg_write */
   L2G_SCSI_INIT,       /* scsi.c piscsi_init */
   L2G_SCSI_FSHD,       /* scsi.c file system header walk */
   L2G_SCSI_DRIVER,     /* scsi.c PISCSI_CMD_DRIVER partition table */
   L2G_SCSI_SETFSH_NONE,/* scsi.c PISCSI_CMD_SETFSH, no handler for the DOS type */
   L2G_SCSI_SETFSH,     /* scsi.c PISCSI_CMD_SETFSH, handler set */
   L2G_SCSI_LOADFS,     /* scsi.c PISCSI_CMD_LOADFS */
   L2G_STRESS,          /* l2guard.c z3660_l2_stress, test builds only */
   L2G_SITES
};

static const char *const l2g_site_names[L2G_SITES] = {
   "none", "video-flush", "video-play", "main-init", "load-rom", "load-romext", "cpu-freq",
   "floppy-init", "floppy-reg", "scsi-init", "scsi-fshd", "scsi-driver", "scsi-setfsh-none",
   "scsi-setfsh", "scsi-loadfs", "stress"
};

static inline const char *l2g_site_name(uint32_t s)
{
   s &= 0xFFFFU;
   return s < L2G_SITES ? l2g_site_names[s] : "?";
}

/* CPU mode field of a PSR, as text */
static inline const char *l2g_mode_name(uint32_t mode)
{
   switch (mode & 0x1FU) {
   case 0x10: return "usr";
   case 0x11: return "fiq";
   case 0x12: return "irq";
   case 0x13: return "svc";
   case 0x17: return "abt";
   case 0x1B: return "und";
   case 0x1F: return "sys";
   default:   return "?";
   }
}

/* core 0 (l2guard.c) */
void z3660_l2_flush(uint32_t site);                  /* every whole-L2 flush goes here */
void z3660_abort_capture(uint32_t kind, uint32_t pc); /* first thing in an abort handler */
void z3660_abort_print(void);                         /* the record just captured */
void z3660_abort_report(void);                        /* the previous boot's, after the banner */
void z3660_l2_stress(void);                           /* main loop; does nothing unless L2G_DIAG_STRESS */

/*
 * Test builds only (make ... L2G_DIAG="..."), never shipped:
 *   -DL2G_DIAG_UNMASKED  the flush runs with interrupts enabled, as Xil_L2CacheFlush alone did;
 *   -DL2G_DIAG_STRESS    for the first 120 s after the 060 starts the main loop flushes the
 *                        whole L2 on every pass, so the vblank interrupt's own flush lands
 *                        inside one almost every frame; at the end it prints the counts.
 * Unmasked + stress makes the nesting thousands of times a boot; masked + stress shows the
 * same load with no nesting.
 */

#define L2G_IRQ_FIQ_MASK  0xC0U

struct l2g_state {
   uint32_t depth;      /* guarded flushes in progress on this core */
   uint32_t site;       /* the innermost one in progress, | mode << 16; 0 when none */
   uint32_t last;       /* the last one entered, | mode << 16 */
   uint32_t count;      /* entered since boot */
   uint32_t nested;     /* entered while another was in progress on this core */
};

#ifdef L2G_FLUSH
static inline void l2g_flush(volatile struct l2g_state *g, uint32_t site)
{
   uint32_t cpsr = L2G_GET_CPSR();
   uint32_t outer;

#ifndef L2G_DIAG_UNMASKED
   L2G_SET_CPSR(cpsr | L2G_IRQ_FIQ_MASK);
#endif
   outer = g->site;
   if (g->depth)
      g->nested++;
   g->depth++;
   g->site = (site & 0xFFFFU) | ((cpsr & 0x1FU) << 16);
   g->last = g->site;
   g->count++;
   L2G_FLUSH();
   g->site = outer;
   g->depth--;
   L2G_SET_CPSR(cpsr);
}
#endif

/* ---- the abort record ------------------------------------------------------------------ */
#define L2G_RECORD_MAGIC  0x5A334142U   /* "Z3AB" */
#define L2G_KIND_DATA     1U
#define L2G_KIND_PREFETCH 2U

/* PL310 raw interrupt status bits (xl2cc.h) */
#define L2G_PL310_DECERR  0x100U
#define L2G_PL310_SLVERR  0x080U
#define L2G_PL310_ERRRD   0x040U
#define L2G_PL310_ERRRT   0x020U
#define L2G_PL310_ERRWD   0x010U
#define L2G_PL310_ERRWT   0x008U

struct l2g_record {
   uint32_t magic;
   uint32_t kind;       /* L2G_KIND_* */
   uint32_t fsr;        /* DFSR or IFSR */
   uint32_t far;        /* DFAR or IFAR; UNKNOWN for an asynchronous abort */
   uint32_t afsr;       /* ADFSR or AIFSR */
   uint32_t pc;         /* where the abort was taken (lr_abt - 8 or - 4) */
   uint32_t spsr;       /* SPSR_abt: the mode and mask state that was interrupted */
   uint32_t depth;      /* l2g_state at the abort */
   uint32_t site;
   uint32_t last;
   uint32_t count;
   uint32_t nested;
   uint32_t pl310_ris;  /* 0xF8F0221C raw interrupt status */
   uint32_t pl310_way;  /* 0xF8F027FC clean+invalidate by way: ways still in progress */
   uint32_t pl310_dbg;  /* 0xF8F02F40 debug control */
   uint32_t check;
};

static inline uint32_t l2g_record_sum(const volatile struct l2g_record *r)
{
   const volatile uint32_t *w = (const volatile uint32_t *)r;
   uint32_t i, s = 0x9E3779B9U;
   for (i = 0; i < sizeof(*r) / 4 - 1; i++)
      s = (s ^ w[i]) * 0x01000193U;
   return s;
}

static inline void l2g_record_seal(volatile struct l2g_record *r)
{
   r->magic = L2G_RECORD_MAGIC;
   r->check = l2g_record_sum(r);
}

static inline int l2g_record_valid(const volatile struct l2g_record *r)
{
   return r->magic == L2G_RECORD_MAGIC && r->check == l2g_record_sum(r) &&
          (r->kind == L2G_KIND_DATA || r->kind == L2G_KIND_PREFETCH);
}

/* FS of a short-descriptor DFSR/IFSR: bits 10 and 3..0 */
static inline uint32_t l2g_fs(uint32_t fsr) { return ((fsr >> 6) & 0x10U) | (fsr & 0xFU); }

/* An asynchronous external abort (FS 0b10110) or asynchronous parity error (0b11000):
 * the fault address and direction are not recorded by the CPU */
static inline int l2g_fs_async(uint32_t fsr)
{
   uint32_t fs = l2g_fs(fsr);
   return fs == 0x16U || fs == 0x18U;
}

/* Two lines, no allocation: the record as text, into buf (256 bytes is enough) */
static inline int l2g_record_format(const volatile struct l2g_record *r, char *buf, unsigned len)
{
   uint32_t ris = r->pl310_ris;
   return snprintf(buf, len,
      "%s abort FSR %08lX%s FAR %08lX AFSR %08lX PC %08lX from %s (SPSR %08lX)\n"
      "  L2 flush depth %lu site %s/%s last %s/%s count %lu nested %lu; PL310 RIS %03lX%s%s%s%s%s%s"
      " way %04lX dbg %lX\n",
      r->kind == L2G_KIND_DATA ? "data" : "prefetch", (unsigned long)r->fsr,
      l2g_fs_async(r->fsr) ? " (async: FAR and WnR unknown)" : "", (unsigned long)r->far,
      (unsigned long)r->afsr, (unsigned long)r->pc, l2g_mode_name(r->spsr),
      (unsigned long)r->spsr, (unsigned long)r->depth,
      l2g_site_name(r->site), l2g_mode_name(r->site >> 16),
      l2g_site_name(r->last), l2g_mode_name(r->last >> 16), (unsigned long)r->count, (unsigned long)r->nested,
      (unsigned long)ris, ris & L2G_PL310_DECERR ? " DECERR" : "",
      ris & L2G_PL310_SLVERR ? " SLVERR" : "", ris & L2G_PL310_ERRRD ? " ERRRD" : "",
      ris & L2G_PL310_ERRRT ? " ERRRT" : "", ris & L2G_PL310_ERRWD ? " ERRWD" : "",
      ris & L2G_PL310_ERRWT ? " ERRWT" : "",
      (unsigned long)r->pl310_way, (unsigned long)r->pl310_dbg);
}

#endif
