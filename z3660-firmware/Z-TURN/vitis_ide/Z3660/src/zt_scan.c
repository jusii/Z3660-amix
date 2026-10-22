/*
 * zt_scan.c -- console primitives for the bench timing scanner (build flag ZT_SCAN).
 *
 * The host steps the clock phases of one timings row, applies them with the 68060 parked
 * or held in reset, and reads the test program's mailbox in card RAM. Nothing here writes
 * the SD: the staged row lives in cd[] only, and SCT is refused while it differs from the
 * row the card booted with. Every reply is one line starting "ZT ".
 *
 *   TPH <pclk> <clken> <bclk> <cpuclk> <clk90>   stage the phases of the current row
 *   TROW <mhz>                                    stage another row (config.cpufreq)
 *   TAPPLY PARK|RESET|LIVE                        program the clock wizard
 *   TMB                                           decode the mailbox at ZT_MB
 *   WM <addr> <v1> [v2 ...]                       write params (68k longwords, hex)
 *   TSTAT                                         restart and clock state
 *   TREST                                         restore the boot row, then apply
 *
 * Built only with -DZT_SCAN; without it this file is empty and the image is unchanged.
 */
#ifdef ZT_SCAN
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "main.h"
#include "config_clk.h"
#include "config_file.h"
#include "xtime_l.h"

#define ZT_MB          0x0FFF0000u   /* mailbox: TESTPROG.md section 5 */
#define ZT_MB_MIRROR   0x0FFF0800u
#define ZT_MB_LEN      0x200u
#define ZT_PARAM_LO    0x0FFF0200u
#define ZT_PARAM_HI    0x0FFF02FCu
#define ZT_MB_MAGIC    0x5A544D42u   /* 'ZTMB' */
#define ZT_WHERE_PARKED 0xFFFF0000u
#define ZT_NPATHS      8

#define ZT_VCO_MIN     600
#define ZT_VCO_MAX     1600
#define ZT_LOCK_TIMEOUT_US 20000u
#define ZT_GRANT_TIMEOUT_US 1000u

extern clock_data cd[];
extern CONFIG config;
extern XGpioPs GpioPs;
extern volatile int read_reset;
extern int state68k;
extern int timing_selected;
extern uint32_t counts_per_second;
uint32_t get_current_cpu_frequency(void);

#define ZT_NROWS 15
static clock_data zt_snap[ZT_NROWS];
static int zt_snap_taken;
static int zt_snap_freq;

static struct {
   uint32_t park_ok, nogrant, notparked, lockto, reset, live, refused;
   uint32_t last_lock_us;
} zt_n;

/* ---------- small helpers ---------- */

static uint32_t zt_us(void)
{
   XTime t;
   XTime_GetTime(&t);
   return (uint32_t)(t / (counts_per_second / 1000000u));
}
/* die temperature in tenths of a degree (XADC, adc.c) and seconds since the last PS reset: every TSTAT and TMB
 * carries both, so a scan can be read against the card's own warming */
extern float xadc_get_temperature(void);
static int zt_temp10(void) { return (int)(xadc_get_temperature() * 10.0f); }
static unsigned long zt_up_s(void)
{
   XTime t;
   XTime_GetTime(&t);
   return (unsigned long)(t / counts_per_second);
}
static void zt_wait_us(uint32_t us)
{
   uint32_t t0 = zt_us();
   while ((uint32_t)(zt_us() - t0) < us) {}
}
static int zt_emu(void)
{
   return boot_mode_is_emulated(config.boot_mode);
}
static void zt_snapshot(void)
{
   if (zt_snap_taken) return;
   memcpy(zt_snap, cd, sizeof(zt_snap));
   zt_snap_freq = config.cpufreq;
   zt_snap_taken = 1;
}
static int zt_row_live(void)
{
   return zt_snap_taken &&
          (memcmp(zt_snap, cd, sizeof(zt_snap)) != 0 || zt_snap_freq != config.cpufreq);
}
static uint32_t zt_rd68k(uint32_t a) { return __builtin_bswap32(*(volatile uint32_t *)a); }

/* The wizard rounds a phase to the nearest 1/8 VCO cycle: 45/O degrees (mmcm_pll_phase in
 * mmcm_pll_drp_func_7s_mmcm.vh). Returns hundredths of a degree; *trunc set when the 6-bit
 * delay field would overflow. */
static int zt_real_cdeg(int phase, int div, int *trunc)
{
   int p = phase < 0 ? phase + 360 : phase;
   long eighths = ((long)p * div * 8 + 180) / 360;
   if (trunc) *trunc = (eighths >= 64L * 8);
   return (int)(eighths * 36000L / ((long)div * 8));
}

static int zt_parse_int(const char **s, int *v)
{
   char *e;
   while (**s == ' ') (*s)++;
   if (**s == 0) return 0;
   long x = strtol(*s, &e, 10);
   if (e == *s) return 0;
   *v = (int)x;
   *s = e;
   return 1;
}
static int zt_parse_hex(const char **s, uint32_t *v)
{
   char *e;
   while (**s == ' ') (*s)++;
   if (**s == 0) return 0;
   unsigned long x = strtoul(*s, &e, 16);
   if (e == *s) return 0;
   *v = (uint32_t)x;
   *s = e;
   return 1;
}

/* ---------- the wizard ---------- */

static int zt_row_check(int ind, int emu, const char **why)
{
   clock_data *c = &cd[ind];
   int ed = emu ? 2 : 1;
   if (c->M < 2 || c->M > 64)        { *why = "M"; return 0; }
   if (c->D < 1 || c->D > 20)        { *why = "D"; return 0; }   /* PFD >= 10 MHz */
   int vco = 200 * c->M / c->D;
   if (vco < ZT_VCO_MIN || vco > ZT_VCO_MAX) { *why = "VCO"; return 0; }
   int dv[6] = { c->axi.divider, c->pclk.divider * ed, c->clken.divider * ed,
                 c->bclk.divider, c->cpuclk.divider, c->clk90.divider };
   for (int i = 0; i < 6; i++)
      if (dv[i] < 1 || dv[i] > 64)  { *why = "divider"; return 0; }   /* >64 truncates phase */
   int ph[5] = { c->pclk.phase, c->clken.phase, c->bclk.phase,
                 c->cpuclk.phase + (emu ? c->emu_extra_phase : 0),
                 c->clk90.phase + (emu ? c->emu_extra_phase : 0) };
   for (int i = 0; i < 5; i++)
      if (ph[i] < -360 || ph[i] > 360) { *why = "phase"; return 0; }
   return 1;
}

/* configure_clk() (config_clk.c:457) with a bounded lock wait and no printing. The caller
 * masks interrupts: every wizard output stops while it re-locks, AXI_clk included. */
static int zt_configure(int ind, int emu, uint32_t *lock_us)
{
   clock_data *c = &cd[ind];
   int ed = 1, xp = 0;
   if (emu) { ed = 2; xp = c->emu_extra_phase; }
   uint32_t b = XPAR_CLK_WIZ_0_BASEADDR;
   XClk_Wiz_WriteReg(b, 0x200, ((uint32_t)c->M) * 256 + (uint32_t)c->D);
   XClk_Wiz_WriteReg(b, 0x208, (uint32_t)(c->axi.divider));
   XClk_Wiz_WriteReg(b, 0x20C, (uint32_t)(c->axi.phase * 1000));
   XClk_Wiz_WriteReg(b, 0x210, (uint32_t)(c->axi.dutycycle * 1000));
   XClk_Wiz_WriteReg(b, 0x214, (uint32_t)(c->pclk.divider * ed));
   XClk_Wiz_WriteReg(b, 0x218, (uint32_t)(c->pclk.phase * 1000));
   XClk_Wiz_WriteReg(b, 0x21C, (uint32_t)(c->pclk.dutycycle * 1000));
   XClk_Wiz_WriteReg(b, 0x220, (uint32_t)(c->clken.divider * ed));
   XClk_Wiz_WriteReg(b, 0x224, (uint32_t)(c->clken.phase * 1000));
   XClk_Wiz_WriteReg(b, 0x228, (uint32_t)(c->clken.dutycycle * 1000));  /* clken==1 here */
   XClk_Wiz_WriteReg(b, 0x22C, (uint32_t)(c->bclk.divider));
   XClk_Wiz_WriteReg(b, 0x230, (uint32_t)(c->bclk.phase * 1000));
   XClk_Wiz_WriteReg(b, 0x234, (uint32_t)(c->bclk.dutycycle * 1000));
   XClk_Wiz_WriteReg(b, 0x238, (uint32_t)(c->clk90.divider));
   XClk_Wiz_WriteReg(b, 0x23C, (uint32_t)((c->clk90.phase + xp) * 1000));
   XClk_Wiz_WriteReg(b, 0x240, (uint32_t)(c->clk90.dutycycle * 1000));
   XClk_Wiz_WriteReg(b, 0x244, (uint32_t)(c->cpuclk.divider));
   XClk_Wiz_WriteReg(b, 0x248, (uint32_t)((c->cpuclk.phase + xp) * 1000));
   XClk_Wiz_WriteReg(b, 0x24C, (uint32_t)(c->cpuclk.dutycycle * 1000));
   uint32_t t0 = zt_us();
   XClk_Wiz_WriteReg(b, 0x25C, 0x00000003);
   while (XClk_Wiz_ReadReg(b, 0x004) == 0)
      if ((uint32_t)(zt_us() - t0) > ZT_LOCK_TIMEOUT_US) { *lock_us = ZT_LOCK_TIMEOUT_US; return 0; }
   *lock_us = zt_us() - t0;
   timing_selected = ind;
   return 1;
}

static void zt_print_row(const char *tag, int ind, int emu)
{
   clock_data *c = &cd[ind];
   int ed = emu ? 2 : 1, xp = emu ? c->emu_extra_phase : 0;
   int dv[5] = { c->pclk.divider * ed, c->clken.divider * ed, c->bclk.divider,
                 c->cpuclk.divider, c->clk90.divider };
   int ph[5] = { c->pclk.phase, c->clken.phase, c->bclk.phase,
                 c->cpuclk.phase + xp, c->clk90.phase + xp };
   int r[5], t = 0, tr;
   for (int i = 0; i < 5; i++) { r[i] = zt_real_cdeg(ph[i], dv[i], &tr); t |= tr; }
   printf("ZT %s idx=%d mhz=%d emu=%d m=%d d=%d div=%d/%d/%d/%d/%d req=%d/%d/%d/%d/%d "
          "real=%d.%02d/%d.%02d/%d.%02d/%d.%02d/%d.%02d%s\n",
          tag, ind, c->clk, emu, c->M, c->D, dv[0], dv[1], dv[2], dv[3], dv[4],
          ph[0], ph[1], ph[2], ph[3], ph[4],
          r[0] / 100, r[0] % 100, r[1] / 100, r[1] % 100, r[2] / 100, r[2] % 100,
          r[3] / 100, r[3] % 100, r[4] / 100, r[4] % 100, t ? " trunc=1" : "");
}

/* ---------- commands ---------- */

static void zt_tph(const char *s)
{
   int v[5];
   for (int i = 0; i < 5; i++)
      if (!zt_parse_int(&s, &v[i]) || v[i] < -360 || v[i] > 360) { printf("ZT TPH bad\n"); return; }
   zt_snapshot();
   int ind = get_clock_index(config.cpufreq);
   cd[ind].pclk.phase = v[0];
   cd[ind].clken.phase = v[1];
   cd[ind].bclk.phase = v[2];
   cd[ind].cpuclk.phase = v[3];
   cd[ind].clk90.phase = v[4];
   zt_print_row("TPH", ind, zt_emu());
}

static void zt_trow(const char *s)
{
   int mhz;
   const char *why = "";
   if (!zt_parse_int(&s, &mhz) || mhz < CPUFREQ_MIN || mhz > CPUFREQ_MAX) { printf("ZT TROW bad\n"); return; }
   int ind = get_clock_index(mhz);
   if (!zt_row_check(ind, zt_emu(), &why)) { printf("ZT TROW refused %s\n", why); return; }
   zt_snapshot();
   config.cpufreq = cd[ind].clk;
   zt_print_row("TROW", ind, zt_emu());
}

static int zt_wait_grant(void)
{
   uint32_t t0 = zt_us();
   while (XGpioPs_ReadPin(&GpioPs, PS_MIO_15) != 0)
      if ((uint32_t)(zt_us() - t0) > ZT_GRANT_TIMEOUT_US) return 0;
   return 1;
}

/* The reset ISR (interrupt.c:68) latched the falling n040RSTI edge while interrupts were
 * masked; clear it so reset_thread does not run a second reset of its own. */
static void zt_unmask_after_reset(void)
{
   XGpioPs_IntrClearPin(&GpioPs, n040RSTI);
   Xil_ExceptionEnable();
   zt_wait_us(20);
   read_reset = 0;
}

static void zt_tapply(const char *s)
{
   while (*s == ' ') s++;
   int emu = zt_emu();
   int ind = get_clock_index(config.cpufreq);
   const char *why = "";
   uint32_t lock_us = 0;
   int ok;

   if (!zt_row_check(ind, emu, &why)) { zt_n.refused++; printf("ZT TAPPLY refused %s\n", why); return; }

   if (strcmp(s, "LIVE") == 0)
   {
      Xil_ExceptionDisable();
      ok = zt_configure(ind, emu, &lock_us);
      Xil_ExceptionEnable();
      zt_n.live++;
   }
   else if (strcmp(s, "PARK") == 0)
   {
      if (emu) { zt_n.refused++; printf("ZT TAPPLY refused emu\n"); return; }
      if (zt_rd68k(ZT_MB) != ZT_MB_MAGIC || zt_rd68k(ZT_MB + 0x0C) != ZT_WHERE_PARKED)
      {
         zt_n.notparked++;
         printf("ZT TAPPLY notparked where=%08lX\n", (unsigned long)zt_rd68k(ZT_MB + 0x0C));
         return;
      }
      Xil_ExceptionDisable();
      NBR_ARM(0);
      if (!zt_wait_grant())
      {
         NBR_ARM(1);
         Xil_ExceptionEnable();
         zt_n.nogrant++;
         printf("ZT TAPPLY nogrant\n");
         return;
      }
      ok = zt_configure(ind, 0, &lock_us);
      zt_wait_us(1000);
      NBR_ARM(1);
      Xil_ExceptionEnable();
      zt_n.park_ok += ok;
   }
   else if (strcmp(s, "RESET") == 0)
   {
      if (emu) { zt_n.refused++; printf("ZT TAPPLY refused emu\n"); return; }
      /* reset_thread (main.c:711) order, atomic: the console is dead in M68K_RESET.
       * NBR_ARM (MIO8) is never toggled here: its rising edge latches MIO15 into nBOSS
       * while the CPLD is in reset (z3660.vhd:379). */
      Xil_ExceptionDisable();
      CPLD_RESET_ARM(0);
      zt_wait_us(100000);
      ok = zt_configure(ind, 0, &lock_us);
      DiscreteSet(REG0, FPGA_RESET);      /* re-latches the MAPROM enables (z3660.v:407) */
      zt_wait_us(10000);
      DiscreteClear(REG0, FPGA_RESET);
      zt_wait_us(10000);
      CPLD_RESET_ARM(1);
      zt_wait_us(10000);
      zt_unmask_after_reset();
      zt_n.reset++;
   }
   else { printf("ZT TAPPLY bad\n"); return; }

   zt_n.last_lock_us = lock_us;
   if (!ok) { zt_n.lockto++; printf("ZT TAPPLY lockto how=%s\n", s); return; }
   printf("ZT TAPPLY ok how=%s lock_us=%lu\n", s, (unsigned long)lock_us);
   zt_print_row("ROW", ind, emu);
}

static uint32_t zt_crc32(const volatile uint8_t *p, uint32_t n)
{
   uint32_t c = 0xFFFFFFFFu;
   for (uint32_t i = 0; i < n; i++)
   {
      c ^= p[i];
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
   }
   return ~c;
}
static int zt_mb_good(uint32_t base)
{
   return zt_rd68k(base) == ZT_MB_MAGIC &&
          zt_crc32((const volatile uint8_t *)base, 0x1FC) == zt_rd68k(base + 0x1FC);
}

static void zt_tmb(void)
{
   uint32_t b = ZT_MB;
   const char *st = "ok";
   if (!zt_mb_good(ZT_MB))
   {
      if (zt_mb_good(ZT_MB_MIRROR)) { b = ZT_MB_MIRROR; st = "mirror"; }
      else st = "bad";
   }
   /* The primary record is live between publishes (its CRC is stale mid-pass), so the mirror
    * is the consistent copy; lwhere is the primary's live marker, the one a hang leaves. */
   printf("ZT TMB crc=%s lwhere=%08lX hb=%lu where=%08lX epoch=%lu st=%08lX rs=%lu id=%08lX",
          st, (unsigned long)zt_rd68k(ZT_MB + 0x0C), (unsigned long)zt_rd68k(b + 0x08), (unsigned long)zt_rd68k(b + 0x0C),
          (unsigned long)zt_rd68k(b + 0x10), (unsigned long)zt_rd68k(b + 0x14),
          (unsigned long)zt_rd68k(b + 0x18), (unsigned long)zt_rd68k(b + 0x1C));
   for (int p = 0; p < ZT_NPATHS; p++)
   {
      uint32_t r = b + 0x20 + p * 0x20;
      printf(" p%d=%lu/%lu/%lu/%08lX/%08lX/%lu", p + 1,
             (unsigned long)zt_rd68k(r + 0x00), (unsigned long)zt_rd68k(r + 0x04),
             (unsigned long)zt_rd68k(r + 0x08), (unsigned long)zt_rd68k(r + 0x0C),
             (unsigned long)zt_rd68k(r + 0x10), (unsigned long)zt_rd68k(r + 0x18));
   }
   printf(" sz=%08lX/%08lX/%08lX/%08lX/%08lX temp10=%d up=%lu\n",
          (unsigned long)zt_rd68k(b + 0x120), (unsigned long)zt_rd68k(b + 0x124),
          (unsigned long)zt_rd68k(b + 0x128), (unsigned long)zt_rd68k(b + 0x12C),
          (unsigned long)zt_rd68k(b + 0x130), zt_temp10(), zt_up_s());
}

static void zt_wm(const char *s)
{
   uint32_t a, v;
   int n = 0;
   if (!zt_parse_hex(&s, &a) || (a & 3)) { printf("ZT WM bad\n"); return; }
   while (zt_parse_hex(&s, &v))
   {
      if (a < ZT_PARAM_LO || a > ZT_PARAM_HI) { printf("ZT WM range %08lX\n", (unsigned long)a); return; }
      *(volatile uint32_t *)a = __builtin_bswap32(v);
      Xil_DCacheFlushRange(a, 4);   /* clean: the 060 reads it through the ACP */
      a += 4;
      n++;
   }
   printf("ZT WM ok n=%d\n", n);
}

static void zt_tstat(void)
{
   uint32_t b = XPAR_CLK_WIZ_0_BASEADDR;
   uint32_t md = XClk_Wiz_ReadReg(b, 0x200);
   static const uint16_t dr[6] = { 0x208, 0x214, 0x220, 0x22C, 0x244, 0x238 };
   int dv[6], ph[6];
   for (int i = 0; i < 6; i++)
   {
      dv[i] = (int)(XClk_Wiz_ReadReg(b, dr[i]) & 0xFF);
      ph[i] = (int32_t)XClk_Wiz_ReadReg(b, dr[i] + 4) / 1000;
   }
   /* div/ph are the wizard registers (emulated: pclk/clken doubled, cpuclk/clk90 + extra);
    * rowdiv/rowph/xp are cd[] in timings-file units, what TPH stages. */
   clock_data *c = &cd[get_clock_index(config.cpufreq)];
   printf("ZT TSTAT st68k=%d rr=%d rsti=%lu rstc=%lu nbr=%lu mio15=%lu emu=%d mhz=%d idx=%d armmhz=%lu "
          "m=%lu d=%lu div=%d/%d/%d/%d/%d/%d ph=%d/%d/%d/%d/%d/%d "
          "rowdiv=%d/%d/%d/%d/%d rowph=%d/%d/%d/%d/%d xp=%d snap=%d live=%d "
          "park=%lu nogrant=%lu notparked=%lu lockto=%lu reset=%lu applylive=%lu refused=%lu lock_us=%lu "
          "temp10=%d up=%lu\n",
          state68k, read_reset,
          (unsigned long)XGpioPs_ReadPin(&GpioPs, n040RSTI), (unsigned long)XGpioPs_ReadPin(&GpioPs, PS_MIO_13),
          (unsigned long)XGpioPs_ReadPin(&GpioPs, PS_MIO_8), (unsigned long)XGpioPs_ReadPin(&GpioPs, PS_MIO_15),
          zt_emu(), config.cpufreq, get_clock_index(config.cpufreq),
          (unsigned long)(get_current_cpu_frequency() / 1000000u),
          (unsigned long)((md >> 8) & 0xFF), (unsigned long)(md & 0xFF),
          dv[0], dv[1], dv[2], dv[3], dv[4], dv[5], ph[0], ph[1], ph[2], ph[3], ph[4], ph[5],
          c->pclk.divider, c->clken.divider, c->bclk.divider, c->cpuclk.divider, c->clk90.divider,
          c->pclk.phase, c->clken.phase, c->bclk.phase, c->cpuclk.phase, c->clk90.phase, c->emu_extra_phase,
          zt_snap_taken, zt_row_live(),
          (unsigned long)zt_n.park_ok, (unsigned long)zt_n.nogrant, (unsigned long)zt_n.notparked,
          (unsigned long)zt_n.lockto, (unsigned long)zt_n.reset, (unsigned long)zt_n.live,
          (unsigned long)zt_n.refused, (unsigned long)zt_n.last_lock_us, zt_temp10(), zt_up_s());
}

static void zt_trest(void)
{
   if (!zt_snap_taken) { printf("ZT TREST nothing\n"); return; }
   memcpy(cd, zt_snap, sizeof(zt_snap));
   config.cpufreq = zt_snap_freq;
   zt_tapply(zt_emu() ? "LIVE" : "RESET");
}

/* The console's not-found path (debug_console.c:1074). Returns 1 if the line was ours. */
int zt_try_command(const char *cmd)
{
   char u[96];
   int i;
   for (i = 0; cmd[i] && i < (int)sizeof(u) - 1; i++)
      u[i] = (cmd[i] >= 'a' && cmd[i] <= 'z') ? cmd[i] - 'a' + 'A' : cmd[i];
   u[i] = 0;
   if (strncmp(u, "TPH ", 4) == 0)    { zt_tph(u + 4); return 1; }
   if (strncmp(u, "TROW ", 5) == 0)   { zt_trow(u + 5); return 1; }
   if (strncmp(u, "TAPPLY ", 7) == 0) { zt_tapply(u + 7); return 1; }
   if (strncmp(u, "WM ", 3) == 0)     { zt_wm(u + 3); return 1; }
   if (strcmp(u, "TMB") == 0)         { zt_tmb(); return 1; }
   if (strcmp(u, "TSTAT") == 0)       { zt_tstat(); return 1; }
   if (strcmp(u, "TREST") == 0)       { zt_trest(); return 1; }
   return 0;
}

/* SCT (debug_console.c:1040) would save a scan row to the SD. */
int zt_sct_allowed(void)
{
   if (!zt_row_live()) return 1;
   printf("ZT SCT refused (scan row live; TREST first)\n");
   return 0;
}
#endif /* ZT_SCAN */
