#ifndef CONFIG_CLK_H
#define CONFIG_CLK_H

#include "xclk_wiz.h"

typedef struct {
   int divider;
   int phase;
   int dutycycle;
} clock_st;
typedef struct {
   int clk;
   int M;
   int D;
   clock_st axi;
   clock_st pclk;
   clock_st clken;
   clock_st bclk;
   clock_st cpuclk;
   clock_st clk90;
   int emu_extra_phase;
} clock_data;

void configure_clk(int clk, int verbose, int nbr);
unsigned int get_clock_index(int clk);

/* Bench timing scanner hooks (zt_scan.c). Without ZT_SCAN they expand to the original code. */
#ifdef ZT_SCAN
int zt_try_command(const char *cmd);
int zt_sct_allowed(void);
#define ZT_TRY(found,cmd) ((found) || zt_try_command(cmd))
#define ZT_SCT_ALLOWED()  zt_sct_allowed()
#else
#define ZT_TRY(found,cmd) (found)
#define ZT_SCT_ALLOWED()  1
#endif

#endif // CONFIG_CLK_H
