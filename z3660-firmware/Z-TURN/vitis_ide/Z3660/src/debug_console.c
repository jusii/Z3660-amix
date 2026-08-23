
#define DEBUG_CONSOLE_ENABLED 1
#ifdef DEBUG_CONSOLE_ENABLED
#include "debug_console.h"
#include "main.h"
#include <xil_printf.h>
#include <xparameters.h>
#include "xuartps_hw.h"
#include "config_clk.h"
#include "config_file.h"
#include <stdlib.h>
#include <string.h>

void debug_console_help(void);
void dump_mmu(void);
uint32_t read_rtg_register(uint32_t zaddr);
void write_rtg_register(uint32_t zaddr,uint32_t zdata);
int write_timings(void);
void enable_overlay(void);
void disable_overlay(void);

DEBUG_CONSOLE debug_console;
extern SHARED *shared;
extern int eth_backlog_nag_counter_max;
/* piscsi service-rate/latency instrumentation (scsi/scsi.c; also in scsi/scsi.h,
 * declared locally to avoid pulling the FatFs-dependent header in here). */
void piscsi_stats_toggle(void);
void piscsi_stats_dump(void);
int  piscsi_stats_reporting(void);
/* core0 cooperative-loop per-task duration accounting */
#include "core0_stats.h"
/* CYBERSTORM C1: the profiler command word core0 writes into SHARED.prof_ctl. */
#include "prof_config.h"
/* CYBERSTORM C2 rung 0. Included DIRECTLY rather than mirrored into a core0 copy the way
 * prof_config.h mirrors z3660_prof.h -- z3660_prof.h drags in the emulator's headers and
 * core0 cannot compile it, but z3660_blk.h is #defines and one static inline over
 * `unsigned` and nothing else. A mirrored copy would only be a second place for the OFF
 * sentinel to drift away from the one core1 tests against. */
#include "../../Z3660_emu/src/uae/z3660_blk.h"
#include "../../Z3660_emu/src/uae/z3660_dopc.h"
/* CYBERSTORM C2 rung 2: same reasoning again -- z3660_dfast.h is deliberately
 * self-contained (plain types, no emulator headers) so the ONE rule that turns a mailbox
 * word into a state lives in one file that both cores compile. */
#include "../../Z3660_emu/src/uae/z3660_dfast.h"
/* CYBERSTORM C2, the HANDLER discriminator: same reasoning once more -- z3660_spec.h's
 * core0-visible half is #defines and two static inlines over plain unsigned, no emulator
 * types at all, so the switch rule has exactly one definition for both cores. */
#include "../../Z3660_emu/src/uae/z3660_spec.h"
typedef enum {
   A,

   DRTG,    DEBUG_RTG,
   DUSB,    DEBUG_USB,
   DSCSI,   DEBUG_SCSI,
   DAUDIO,  DEBUG_AUDIO,
   DI2C,    DEBUG_I2C,
   DENET,   DEBUG_ENET,
   DSOFT3D, DEBUG_SOFT3D,
   H,       HELP,
   SD,      STOP_DEBUG,
   DMMU,    DUMP_MMU,
   DM,      DUMP_MEM,
   DMS,     DUMP_MEM_SWAP,
   S,       STEP,
   INW,     INC_NOPS_WRITE,
   DNW,     DEC_NOPS_WRITE,
   INR,     INC_NOPS_READ,
   DNR,     DEC_NOPS_READ,
   INE,     INC_NAG_ETH,
   DNE,     DEC_NAG_ETH,
   DIS,     DISASSEMBLE,
   DISS,    DISASSEMBLE_STEP,
   DISR,    DISASSEMBLE_RUN,
   RSTE,    RESET_EMULATOR,
   SI2C,    STOP_I2C,
   TRB,     TOGGLE_READ_BURST,
   TWB,     TOGGLE_WRITE_BURST,
   CCP,     CHANGE_CLOCK_PHASE,
   DCP,     CHANGE_DUTY_CYCLE,
   CCF,     CHANGE_CLOCK_FREQ,
   RCPLD,   RESET_CPLD,
   RFPGA,   RESET_FPGA,
   RAMIGA,  RESET_AMIGA,
   RARM,    RESET_ARM,
   JIT,     TOGGLE_JIT,
   SCT,     SAVE_CURRENT_TIMINGS,
   PHD,     PRINT_HIST_DATAABORT,
   VMR,     VIDEO_MODE_RESET,
   OVL,     TOGGLE_OVERLAY,
   DEMU,    DEBUG_EMU,
   PERF,    PERF_REPORT,
   SERV,    SERVICE_CADENCE,
   PSTAT,   PISCSI_STATS_REPORT,
   PSTATR,  PISCSI_STATS_RESET,
   PSTATC,  CORE0_LOOP_DUMP,
   PSTATZ,  CORE0_LOOP_ZERO,
   PERFS,   PERF_STATS,
   PROBES,  EMU_PROBES,
   CSTAT,   CORE0_LOOP_STATS,
   /* CYBERSTORM C1 profiler. APPEND-ONLY: command_names[] is indexed by this enum. */
   PROF,    PROF_ARM,
   PROFB,   PROF_BUCKETS,
   PROFD,   PROF_DUMP,
   PROFR,   PROF_RING,
   PROFZ,   PROF_ZERO,
   PROFO,   PROF_OFF,      /* C2-prep: the lossless stop. See the case below. */
   /* CYBERSTORM C2 rung 0: the block-idiom fast path. APPEND-ONLY, like everything above. */
   BLK,     BLOCK_IDIOM,
   /* CYBERSTORM C2 rung 1: the decoded-op cache. APPEND-ONLY, like everything above. */
   DOPC,    DECODED_OP_CACHE,
   /* CYBERSTORM C2 rung 2: the data-accessor fast path. APPEND-ONLY. */
   DFAST,   ACCESSOR_FAST_PATH,
   /* CYBERSTORM C2: the HANDLER discriminator. APPEND-ONLY. */
   SPEC,    HANDLER_SPECIALISE,

   NUM_COMMANDS
} COMMANDS;
/* Deliberately UNSIZED -- see the completeness tripwire below. */
const char *command_names[] = {
      "A",

      "DRTG",    "DEBUG RTG",
      "DUSB",    "DEBUG USB",
      "DSCSI",   "DEBUG SCSI",
      "DAUDIO",  "DEBUG AUDIO",
      "DI2C",    "DEBUG I2C",
      "DENET",   "DEBUG ENET",
      "DSOFT3D", "DEBUG SOFT3D",
      "H",       "HELP",
      "SD",      "STOP DEBUG",
      "DMMU",    "DUMP MMU",
      "DM",      "DUMP MEM",
      "DMS",     "DUMP MEM SWAP",
      "S",       "STEP",
      "INW",     "INC NOPS WRITE",
      "DNW",     "DEC NOPS WRITE",
      "INR",     "INC NOPS READ",
      "DNR",     "DEC NOPS READ",
      "INE",     "INC NAG ETH",
      "DNE",     "DEC NAG ETH",
      "DIS",     "DISASEMBLE",
      "DISS",    "DISASEMBLE STEP",
      "DISR",    "DISASEMBLE RUN",
      "RSTE",    "RESET EMULATOR",
      "SI2C",    "STOP I2C",
      "TRB",     "TOGGLE READ BURST",
      "TWB",     "TOGGLE WRITE BURST",
      "CCP",     "CHANGE CLOCK PHASE",
      "DCP",     "CHANGE DUTY CYCLE",
      "CCF",     "CHANGE CLOCK FREQ",
      "RCPLD",   "RESET CPLD",
      "RFPGA",   "RESET FPGA",
      "RAMIGA",  "RESET AMIGA",
      "RARM",    "RESET ARM",
      "JIT",     "TOOGGLE_JIT",
      "SCT",     "SAVE_CURRENT_TIMINGS",
      "PHD",     "PRINT_HIST_DATAABORT",
      "VMR",     "VIDEO_MODE_RESET",
      "OVL",     "TOGGLE_OVERLAY",
      "DEMU",    "DEBUG EMU",
      "PERF",    "PERF REPORT",
      "SERV",    "SERVICE CADENCE",
      "PSTAT",   "PISCSI STATS REPORT",
      "PSTATR",  "PISCSI STATS RESET",
      "PSTATC",  "CORE0 LOOP DUMP",
      "PSTATZ",  "CORE0 LOOP ZERO",
      "PERFS",   "PERF STATS",
      "PROBES",  "EMU PROBES",
      "CSTAT",   "CORE0 LOOP STATS",
      "PROF",    "PROF ARM",
      "PROFB",   "PROF BUCKETS",
      "PROFD",   "PROF DUMP",
      "PROFR",   "PROF RING",
      "PROFZ",   "PROF ZERO",
      "PROFO",   "PROF OFF",
      "BLK",     "BLOCK IDIOM",
      "DOPC",    "DECODED OP CACHE",
      "DFAST",   "ACCESSOR FAST PATH",
      "SPEC",    "HANDLER SPECIALISE",
};
/* Tripwire: command_names[] is indexed by COMMANDS. A new enum entry with no matching
 * string shifts every command after it -- typing one name would run a different one. */
typedef char z3660_command_names_complete[
   (sizeof(command_names)/sizeof(command_names[0]) == NUM_COMMANDS) ? 1 : -1];
extern clock_data cd[];
extern CONFIG config;

void debug_console_init(void)
{
   debug_console.cmd_pointer=0;
   debug_console.debug_rtg=0;
   debug_console.debug_usb=0;
   debug_console.subcmd=0;
   debug_console.debug_scsi=0;
   debug_console.debug_audio=0;
   debug_console.debug_ethernet=0;
   debug_console.debug_soft3d=0;
   debug_console.debug_i2c=0;
   shared->debug_emu=0;         // emulator-core debug spam default OFF (DEMU menu cmd toggles)
   shared->perf_report=0;       // 030-MMU instruction-rate readout default OFF (PERF menu cmd)
   debug_console.stop_i2c=0;
   debug_console.step=0;
   debug_console.hist_pointer=0;
   debug_console.hist_pointer_top=0;
   for(int i=0;i<MAX_HIST;i++)
      debug_console.cmd_hist[i][0]=0;
   debug_console.toggle_read_burst=1;
   debug_console.toggle_write_burst=1;
   int ind=get_clock_index(config.cpufreq);
   debug_console.clocks.clk_index=ind;
   int clkbase=200*cd[ind].M/cd[ind].D;
#define CLKCONFIG(A,a)                                \
      debug_console.clocks.A.clk      =clkbase/cd[ind].a.divider; \
      debug_console.clocks.A.phase    =        cd[ind].a.phase;   \
      debug_console.clocks.A.dutycycle=        cd[ind].a.dutycycle

   CLKCONFIG(AXICLK,axi);
   CLKCONFIG(  PCLK,pclk);
   CLKCONFIG( CLKEN,clken);
   CLKCONFIG(  BCLK,bclk);
   CLKCONFIG(CPUCLK,cpuclk);
   CLKCONFIG( CLK90,clk90);
   debug_console.clocks.emu_etra_phase=cd[ind].emu_extra_phase;

   debug_console.reset_cpld=0;
   debug_console.reset_fpga=0;
   debug_console.overlay_enabled=0;
   debug_console.prof_buckets=0;   /* CYBERSTORM C1: nothing armed at power-on */
}
uint32_t hextoi(char *str)
{
   int index=0;
   uint32_t data=0;
   if(str[0]=='0' && (str[1]=='x' || str[1]=='X'))
      index=2;
   if(str[0]=='h')
      index=1;
   while(str[index]!=0)
   {
      char c=str[index++];
      if(c>='A' && c<='F') c=c-'A'+10;
      else if(c>='0' && c<='9') c=c-'0';
      data=(data<<4)+c;
   }
   return(data);
}
void dump_mem(uint32_t address, uint32_t length, int swap)
{
   if(swap==0)
   {
      for(uint32_t i=0;i<length;i+=4*8)
      {
         printf("0x%08lX ",address+i);
         for(uint32_t j=0;j<4*8;j+=4)
         {
            printf("%08lX ",*((uint32_t *)(address+i+j)));
         }
         printf("\n");
      }
   }
   else
   {
      for(uint32_t i=0;i<length;i+=4*8)
      {
         printf("0x%08lX ",address+i);
         for(uint32_t j=0;j<4*8;j+=4)
         {
            printf("%08lX ",__builtin_bswap32(*((uint32_t *)(address+i+j))));
         }
         printf("\n");
      }
   }
}
/* `PROF <hz>` -- arm the interpreter sampler at an EXPLICIT rate.
 *
 * The command table is an exact-string match over the whole input line, so a command
 * with a numeric argument cannot live in it. This runs from the not-found path instead,
 * which costs nothing until somebody types something the table does not have.
 *
 * It exists for the same reason `PROF OFF` does. The rate-cycling `PROF` offers three
 * rates and no way to ask for a fourth, so a session that wanted the C2 map's
 * recommended 250 Hz (a wrap-free window long enough to CONTAIN a dhrystone run rather
 * than the idle loop after it) had to arm at 1000 and accept a ring that wrapped before
 * the workload ended. Two of the three C1 workload captures were lost exactly that way.
 *
 * Returns 1 if the line was consumed, 0 to let the caller report an unknown command.
 * Rate bounds are core1's (z3660_prof.h, mirrored in prof_config.h) and are CLAMPED
 * rather than refused, matching the config key -- the rate actually used is recorded in
 * the dump header either way, so a typo produces a labelled profile rather than none.
 */
static int prof_try_explicit_rate(const char *cmd)
{
   uint32_t hz=0;
   int i;

   if(strncmp(cmd,"PROF ",5)!=0) return 0;
   for(i=5; cmd[i]!=0; i++)
   {
      if(cmd[i]<'0' || cmd[i]>'9') return 0;      /* not a number -> not ours */
      if(hz > 99999u) hz = 99999u;                /* saturate rather than wrap */
      else hz = hz*10u + (uint32_t)(cmd[i]-'0');
   }
   if(i==5) return 0;                             /* bare "PROF " with no digits */

   if(hz==0)
   {
      /* `PROF 0` is a rate of nothing, which is a stop -- and a stop must not zero. */
      shared->prof_ctl=Z3660_PROF_CFG_CTL_STOP;
      shared->prof_hz=0;
      debug_console.prof_buckets=0;
      xil_printf("PROF OFF requested (via 'PROF 0'): sampler and buckets frozen, data KEPT\r\n");
      return 1;
   }
   if(hz<(uint32_t)Z3660_PROF_CFG_HZ_MIN) hz=(uint32_t)Z3660_PROF_CFG_HZ_MIN;
   if(hz>(uint32_t)Z3660_PROF_CFG_HZ_MAX) hz=(uint32_t)Z3660_PROF_CFG_HZ_MAX;
   shared->prof_hz=hz;
   shared->prof_ctl=Z3660_PROF_CFG_CTL_MAKE(Z3660_PROF_CFG_CTL_ARM,0);
   /* The wrap-free window is printed with the rate, because it is the number that
    * decides whether the capture will contain its workload or the idle loop after it,
    * and nobody computes it in their head at the console. */
   xil_printf("PROF ARM %lu Hz requested -- ZEROES buckets, counters and ring "
              "(PROFILING build only; %lu samples = %lu s before the ring wraps)\r\n",
              (unsigned long)hz, (unsigned long)Z3660_PROF_CFG_RING_ENTRIES,
              (unsigned long)((uint32_t)Z3660_PROF_CFG_RING_ENTRIES/hz));
   return 1;
}

#define ESC 27
#include "pt/pt.h"
int debug_thread(struct pt *pt)
{
   PT_BEGIN(pt);

   while(1)
   {
      PT_WAIT_UNTIL(pt,XUartPs_IsReceiveData(STDIN_BASEADDRESS));
      char c = XUartPs_ReadReg(STDIN_BASEADDRESS, XUARTPS_FIFO_OFFSET);
      if(debug_console.subcmd==0)
      {
         if(c>='a' && c<='z') c+='A'-'a';
         if((c>='A' && c<='Z')||
               (c==' ')||(c==8)||(c==ESC)||
               (c>='0' && c<='9'))
         {
            if(c==ESC)
            {
               while(XUartPs_IsReceiveData(STDIN_BASEADDRESS)==0);
               c=XUartPs_ReadReg(STDIN_BASEADDRESS, XUARTPS_FIFO_OFFSET);
               if(c=='[')
               {
                  while(XUartPs_IsReceiveData(STDIN_BASEADDRESS)==0);
                  c=XUartPs_ReadReg(STDIN_BASEADDRESS, XUARTPS_FIFO_OFFSET);
                  if(c=='A')
                  {
                     // UP
                     while(debug_console.cmd_pointer>0)
                     {
                        XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                        XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                        XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                        debug_console.cmd_pointer--;
                     }
                     debug_console.cmd_buf[debug_console.cmd_pointer]=0;
                     int i=0;
                     if(debug_console.hist_pointer>0)
                        debug_console.hist_pointer--;
                     else
                        debug_console.hist_pointer=MAX_HIST-1;
                     strcpy(debug_console.cmd_buf,debug_console.cmd_hist[debug_console.hist_pointer]);
                     while(debug_console.cmd_hist[debug_console.hist_pointer][i]!=0)
                     {
                        XUartPs_SendByte(STDOUT_BASEADDRESS, debug_console.cmd_hist[debug_console.hist_pointer][i++]);
                        debug_console.cmd_pointer++;
                     }
                  }
                  else if(c=='B')
                  {
                     // DOWN
                     while(debug_console.cmd_pointer>0)
                     {
                        XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                        XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                        XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                        debug_console.cmd_pointer--;
                     }
                     debug_console.cmd_buf[debug_console.cmd_pointer]=0;
                     int i=0;
                     debug_console.hist_pointer++;
                     if(debug_console.hist_pointer>=MAX_HIST) debug_console.hist_pointer=0;
                     if(debug_console.hist_pointer!=debug_console.hist_pointer_top)
                     {
                        strcpy(debug_console.cmd_buf,debug_console.cmd_hist[debug_console.hist_pointer]);
                        while(debug_console.cmd_hist[debug_console.hist_pointer][i]!=0)
                        {
                           XUartPs_SendByte(STDOUT_BASEADDRESS, debug_console.cmd_hist[debug_console.hist_pointer][i++]);
                           debug_console.cmd_pointer++;
                        }
                     }
                  }
                  else
                  {
                     debug_console.cmd_buf[debug_console.cmd_pointer]=0;
                  }
               }
               else
               {
                  debug_console.cmd_buf[debug_console.cmd_pointer]=0;
               }
            }
            else
            {
               XUartPs_SendByte(STDOUT_BASEADDRESS, c);
               if(c==8) // backspace
               {
                  if(debug_console.cmd_pointer>0)
                  {
                     XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                     XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                     debug_console.cmd_pointer--;
                  }
                  debug_console.cmd_buf[debug_console.cmd_pointer]=0;
               }
               else
               {
                  debug_console.cmd_buf[debug_console.cmd_pointer++]=c;
                  if(debug_console.cmd_pointer>90) debug_console.cmd_pointer=90;
               }
            }
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer>0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            strcpy(debug_console.cmd_hist[debug_console.hist_pointer_top++],debug_console.cmd_buf);
            if(debug_console.hist_pointer_top>=MAX_HIST) debug_console.hist_pointer_top=0;
            debug_console.hist_pointer=debug_console.hist_pointer_top;
            XUartPs_SendByte(STDOUT_BASEADDRESS, '\r');
            XUartPs_SendByte(STDOUT_BASEADDRESS, '\n');
            uint8_t command_found=0;
            for(int i=0;i<NUM_COMMANDS;i++)
            {
               if(strcmp(debug_console.cmd_buf,command_names[i])==0)
               {
                  command_found=1;
                  switch(i)
                  {
                  case A:
                     xil_printf("test 'A' command\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DRTG:
                  case DEBUG_RTG:
                     debug_console.debug_rtg=!debug_console.debug_rtg;
                     if(debug_console.debug_rtg)
                        xil_printf("DEBUG RTG ON\r\n");
                     else
                        xil_printf("DEBUG RTG OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DUSB:
                  case DEBUG_USB:
                     debug_console.debug_usb=!debug_console.debug_usb;
                     if(debug_console.debug_usb)
                        xil_printf("DEBUG USB ON\r\n");
                     else
                        xil_printf("DEBUG USB OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DSCSI:
                  case DEBUG_SCSI:
                     debug_console.debug_scsi=!debug_console.debug_scsi;
                     if(debug_console.debug_scsi)
                        xil_printf("DEBUG SCSI ON\r\n");
                     else
                        xil_printf("DEBUG SCSI OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DEMU:
                  case DEBUG_EMU:
                     shared->debug_emu=!shared->debug_emu;   // emulator-core debug spam: [PC]/[RTE-B-IF]/fixup (+ AMIX strand trace)
                     xil_printf("DEBUG EMU %s\r\n", shared->debug_emu?"ON":"OFF");
                     debug_console.subcmd=0;
                     break;
                  /* CYBERSTORM C3 overhead switches. These toggle DIAGNOSTIC COST only --
                   * no emulation behaviour changes either way -- so they are safe to
                   * flip live under a running guest during a calibration sweep. */
                  case PERFS:
                  case PERF_STATS:
                     shared->perf_stats=!shared->perf_stats;   // core1 per-instruction perf accounting
                     xil_printf("PERF STATS %s (core1 run-loop accounting; reaches the loop within ~1s)\r\n",
                                shared->perf_stats?"ON":"OFF");
                     if(!shared->perf_stats)
                        xil_printf("  note: [PERF] kIPS needs PERF STATS ON\r\n");
                     debug_console.subcmd=0;
                     break;
                  case PROBES:
                  case EMU_PROBES:
                     shared->emu_probes=!shared->emu_probes;   // core1 run-loop diagnostic probes
                     xil_printf("EMU PROBES %s (ROMENT/RTE/URTE/USTEP on 040, wild-PC ring on 030)\r\n",
                                shared->emu_probes?"ON":"OFF");
                     debug_console.subcmd=0;
                     break;
                  case CSTAT:
                  case CORE0_LOOP_STATS:
                     core0_stats_set_enabled(!core0_stats_is_enabled());   // core0 C0STAT span stamping
                     xil_printf("CORE0 LOOP STATS %s (span stamping; PSTAT only gates the printing)\r\n",
                                core0_stats_is_enabled()?"ON":"OFF");
                     debug_console.subcmd=0;
                     break;
                  case PERF:
                  case PERF_REPORT:
                     shared->perf_report=!shared->perf_report;   // 030-MMU instruction-rate readout (~1Hz) to serial
                     xil_printf("PERF REPORT %s\r\n", shared->perf_report?"ON":"OFF");
                     debug_console.subcmd=0;
                     break;
                  case SERV:
                  case SERVICE_CADENCE:
                     {  // cycle the 030-MMU IPL/cross-core poll cadence: 1->2->4->8->16->32->64->1
                        uint32_t c=shared->service_cadence; c=(c<1)?1:c*2; if(c>64) c=1;
                        shared->service_cadence=c;
                        xil_printf("SERVICE CADENCE %lu (instr between IPL/task polls; 1=every instr)\r\n",(unsigned long)c);
                     }
                     debug_console.subcmd=0;
                     break;
                  case PSTAT:
                  case PISCSI_STATS_REPORT:
                     piscsi_stats_toggle();      // periodic piscsi service-rate/latency summary
                     // ...and the core0 loop report with it: the two halves of the
                     // same measurement (request cost vs. who starved the loop) are
                     // only useful side by side, so one switch drives both.
                     core0_stats_set_report(piscsi_stats_reporting());
                     debug_console.subcmd=0;
                     break;
                  case PSTATR:
                  case PISCSI_STATS_RESET:
                     piscsi_stats_dump();        // cumulative totals + per-unit CLMT state, then reset
                     debug_console.subcmd=0;
                     break;
                  case PSTATC:
                  case CORE0_LOOP_DUMP:
                     core0_stats_dump();         // core0 per-task cumulative totals (non-destructive)
                     debug_console.subcmd=0;
                     break;
                  case PSTATZ:
                  case CORE0_LOOP_ZERO:
                     core0_stats_zero();         // zero core0 cumulative totals + worst-ever latch
                     debug_console.subcmd=0;
                     break;
                  /* CYBERSTORM C1 profiler (core1). Every one of these is a WRITE to the
                   * cross-core command word; core1 consumes it in do_specialties(), so it
                   * costs nothing per emulated instruction and arrives within an
                   * interrupt period.
                   *
                   * Core0 cannot tell which core1 image is running -- there is no
                   * "profiler present" bit and adding one would mean another hand-mirrored
                   * SHARED field for a question that already has an answer: the profiling
                   * image announces itself at boot with a "[PROF] profiling build: ARM
                   * clock ..." line. So these commands say what they asked for, not what
                   * happened, and the absence of a [PROF] reply IS the answer. */
                  case PROF:
                  case PROF_ARM:
                     {  // Cycle the sampler rate: off -> 250 -> 1000 -> 4000 -> off, with
                        // one exception -- an armed rate that is NOT on the ladder came
                        // from z3660cfg.txt or from an explicit 'PROF <hz>', so the press
                        // STOPS it losslessly rather than arming (and zeroing) over a
                        // capture somebody set up deliberately. The rule and the reason
                        // live in prof_config.h, where the host guard can test them; this
                        // is only the keystroke that runs it.
                        int armed=(int)shared->prof_hz;
                        int off_ladder=!z3660_prof_cfg_is_ladder_rate(armed);
                        int hz=z3660_prof_cfg_next_rate(armed);
                        shared->prof_hz=(uint32_t)hz;
                        if(hz) {
                           shared->prof_ctl=Z3660_PROF_CFG_CTL_MAKE(Z3660_PROF_CFG_CTL_ARM,0);
                           xil_printf("PROF ARM %lu Hz requested (zeroes the ring; PROFILING build only)\r\n",
                                      (unsigned long)hz);
                        } else {
                           shared->prof_ctl=Z3660_PROF_CFG_CTL_STOP;
                           debug_console.prof_buckets=0;   // core1's stop clears both
                           if(off_ladder) {
                              xil_printf("PROF STOP requested: armed at %lu Hz, which is not a PROF "
                                         "ladder rate (250/1000/4000) -- stopping rather than arming "
                                         "over a capture somebody set up on purpose.\r\n",
                                         (unsigned long)armed);
                              xil_printf("  data KEPT: PROFD / PROFR / 'PROF RING' read it. "
                                         "'PROF <hz>' re-arms (and zeroes).\r\n");
                           } else {
                              xil_printf("PROF STOP requested (data kept -- PROFD/PROFR still read it)\r\n");
                           }
                        }
                     }
                     debug_console.subcmd=0;
                     break;
                  case PROFO:
                  case PROF_OFF:
                     /* The direct, unambiguous, always-available lossless stop. Freezes
                      * the sampler AND the stage buckets and keeps every byte, from any
                      * armed state whatsoever -- config-armed, ladder-armed or explicitly
                      * armed. This is the command a metal session reaches for when the
                      * workload has just finished and the ring must not move again. */
                     shared->prof_ctl=Z3660_PROF_CFG_CTL_STOP;
                     shared->prof_hz=0;
                     debug_console.prof_buckets=0;
                     xil_printf("PROF OFF requested: sampler and buckets frozen, data KEPT "
                                "(PROFD / PROFR / 'PROF RING' read it)\r\n");
                     debug_console.subcmd=0;
                     break;
                  case PROFB:
                  case PROF_BUCKETS:
                     {  // the interpreter stage buckets, which are NOT free -- see below
                        debug_console.prof_buckets=!debug_console.prof_buckets;
                        shared->prof_ctl=debug_console.prof_buckets
                              ? Z3660_PROF_CFG_CTL_BUCKETS_ON : Z3660_PROF_CFG_CTL_BUCKETS_OFF;
                        xil_printf("PROF BUCKETS %s requested -- these cost real time: "
                                   "the per-stage SHARES are the answer, the instruction RATE is not\r\n",
                                   debug_console.prof_buckets?"ON":"OFF");
                     }
                     debug_console.subcmd=0;
                     break;
                  case PROFD:
                  case PROF_DUMP:
                     shared->prof_ctl=Z3660_PROF_CFG_CTL_DUMPSTATS;   // buckets + counters, ~35 lines
                     xil_printf("PROF DUMP requested (stage buckets + counters)\r\n");
                     debug_console.subcmd=0;
                     break;
                  case PROFR:
                     // The newest 4096 samples: ~90 KB, roughly 8 s of 115200 serial.
                     shared->prof_ctl=Z3660_PROF_CFG_CTL_DUMPRING;    // arg 0 = the default slice
                     xil_printf("PROF RING requested (newest 4096 samples, ~8 s of serial)\r\n");
                     debug_console.subcmd=0;
                     break;
                  case PROF_RING:
                     // The long name dumps the WHOLE ring: 65536 samples, ~1.4 MB, about
                     // two minutes of serial. Deliberately the one that has to be typed out.
                     shared->prof_ctl=Z3660_PROF_CFG_CTL_MAKE(Z3660_PROF_CFG_CTL_DUMPRING,65535);
                     xil_printf("PROF RING (full) requested: up to 65535 samples, ~2 min of serial\r\n");
                     debug_console.subcmd=0;
                     break;
                  case PROFZ:
                  case PROF_ZERO:
                     shared->prof_ctl=Z3660_PROF_CFG_CTL_ZERO;        // buckets, counters and ring
                     xil_printf("PROF ZERO requested (buckets, counters and ring)\r\n");
                     debug_console.subcmd=0;
                     break;
                  case BLK:
                  case BLOCK_IDIOM:
                     {  /* CYBERSTORM C2 rung 0. Cycle the chunk cap OFF -> 64 -> 256 -> 1024 -> OFF.
                        * A cap, not a toggle, because the cap is what bounds interrupt latency
                        * (the run loop's poll runs once per chunk) -- so the one knob answers
                        * both "is it on" and "how long a stall will this guest tolerate", and a
                        * metal session can sweep it against a live workload. The rule that turns
                        * this word into iterations is z3660_blk.h's z3660_blk_resolve_chunk();
                        * core1 picks it up at its next knob resync, well under a second. */
                        uint32_t c=shared->blk_chunk;
                        if(c==Z3660_BLK_OFF)      c=64;
                        else if(c==64)            c=256;
                        else if(c==256)           c=1024;
                        else if(c==1024)          c=Z3660_BLK_OFF;
                        else                      c=Z3660_BLK_OFF;   /* unset/anything else */
                        shared->blk_chunk=c;
                        if(c==Z3660_BLK_OFF)
                           xil_printf("BLOCK IDIOM OFF (bzero/mcpy/CPUSHL loops run one guest instruction at a time)\r\n");
                        else
                           xil_printf("BLOCK IDIOM %lu iterations/chunk (the interrupt poll runs once per chunk)\r\n",
                                      (unsigned long)c);
                     }
                     debug_console.subcmd=0;
                     break;
                  case DOPC:
                  case DECODED_OP_CACHE:
                     {  /* CYBERSTORM C2 rung 1. Toggle the decoded-op cache ON <-> OFF.
                        * A toggle and not a size, unlike BLK: the geometry is fixed by
                        * the map's measured PC distribution (4096 entries, 4-way) and a
                        * runtime way-count knob would put a variable bound on the hit
                        * path, which is the one place this rung cannot afford one. The
                        * rule that turns this word into a state is z3660_dopc.h's
                        * z3660_dopc_resolve(); core1 picks it up at its next knob resync
                        * and invalidates the whole cache when it does. */
                        uint32_t c=shared->dopc_on;
                        c = (z3660_dopc_resolve(c)!=0u) ? Z3660_DOPC_OFF : Z3660_DOPC_DEFAULT;
                        shared->dopc_on=c;
                        if(c==Z3660_DOPC_OFF)
                           xil_printf("DECODED OP CACHE OFF (every instruction is fetched and decoded again)\r\n");
                        else
                           xil_printf("DECODED OP CACHE ON (4096 entries, 4-way, 64 KB; carries the block-idiom verdict)\r\n");
                     }
                     debug_console.subcmd=0;
                     break;
                  case DFAST:
                  case ACCESSOR_FAST_PATH:
                     {  /* CYBERSTORM C2 rung 2. Toggle the data accessors' fast path.
                        * OFF is the REFERENCE MACHINE -- every access takes the original
                        * out-of-line accessor -- so this is what makes a PROFZ-fenced
                        * same-boot A/B of the rung possible without a second flash, which
                        * is the experiment map SS23.1 says a build difference otherwise
                        * needs two flashes for. There is nothing to invalidate on the way
                        * through: the fast path holds no state of its own, only a derived
                        * verdict table that is rebuilt either way. The rule that turns
                        * this word into a state is z3660_dfast.h's z3660_dfast_resolve()
                        * and nothing else re-implements it. */
                        uint32_t c=shared->dfast_on;
                        c = (z3660_dfast_resolve(c)!=0u) ? Z3660_DFAST_OFF : Z3660_DFAST_DEFAULT;
                        shared->dfast_on=c;
                        if(c==Z3660_DFAST_OFF)
                           xil_printf("ACCESSOR FAST PATH OFF (every guest data access takes the original out-of-line accessor -- this is the reference arm)\r\n");
                        else
                           xil_printf("ACCESSOR FAST PATH ON (TTR verdict + page-cache resolve + direct RAM access, inline, no calls)\r\n");
                     }
                     debug_console.subcmd=0;
                     break;
                  case SPEC:
                  case HANDLER_SPECIALISE:
                     {  /* CYBERSTORM C2, the HANDLER discriminator. OFF is the REFERENCE
                        * MACHINE -- every MOVE takes the generated handler through the
                        * ordinary indirect call -- so this is the same-boot arm of the
                        * §30.3 experiment. Unlike DFAST there IS something to invalidate
                        * on the way through: the verdict bit lives in the decoded-op
                        * entries, and core1's z3660_spec_arm() does it when the resync
                        * sees the change. The rule that turns this word into a state is
                        * z3660_spec.h's z3660_spec_resolve() and nothing else. */
                        uint32_t c=shared->spec_on;
                        c = (z3660_spec_resolve(c)!=0u) ? Z3660_SPEC_OFF : Z3660_SPEC_DEFAULT;
                        shared->spec_on=c;
                        if(c==Z3660_SPEC_OFF)
                           xil_printf("HANDLER SPECIALISE OFF (every MOVE takes the generated handler -- this is the reference arm)\r\n");
                        else
                           xil_printf("HANDLER SPECIALISE ON (covered MOVE/MOVEA shapes dispatch through the inline body)\r\n");
                     }
                     debug_console.subcmd=0;
                     break;
                  case DAUDIO:
                  case DEBUG_AUDIO:
                     debug_console.debug_audio=!debug_console.debug_audio;
                     if(debug_console.debug_audio)
                        xil_printf("DEBUG AUDIO ON\r\n");
                     else
                        xil_printf("DEBUG AUDIO OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DENET:
                  case DEBUG_ENET:
                     debug_console.debug_ethernet=!debug_console.debug_ethernet;
                     if(debug_console.debug_ethernet)
                        xil_printf("DEBUG ETHERNET ON\r\n");
                     else
                        xil_printf("DEBUG ETHERNET OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DSOFT3D:
                  case DEBUG_SOFT3D:
                     debug_console.debug_soft3d=!debug_console.debug_soft3d;
                     if(debug_console.debug_soft3d)
                        xil_printf("DEBUG SOFT3D ON\r\n");
                     else
                        xil_printf("DEBUG SOFT3D OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DI2C:
                  case DEBUG_I2C:
                     debug_console.debug_i2c=!debug_console.debug_i2c;
                     if(debug_console.debug_i2c)
                        xil_printf("DEBUG I2C ON\r\n");
                     else
                        xil_printf("DEBUG I2C OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case H:
                  case HELP:
                     debug_console_help();
                     debug_console.subcmd=0;
                     break;
                  case SD:
                  case STOP_DEBUG:
                     debug_console.debug_rtg=0;
                     debug_console.debug_usb=0;
                     debug_console.debug_scsi=0;
                     debug_console.debug_audio=0;
                     debug_console.debug_ethernet=0;
                     debug_console.debug_soft3d=0;
                     debug_console.debug_i2c=0;
                     xil_printf("ALL DEBUG STOPPED\r\n");
                     debug_console.subcmd=0;
                     break;
                  case DMMU:
                  case DUMP_MMU:
                     dump_mmu();
                     debug_console.subcmd=0;
                     break;
                  case DM:
                  case DUMP_MEM:
                     xil_printf("Address (hex): ");
                     debug_console.subcmd=1;
                     debug_console.cmd=DM;
                     break;
                  case DMS:
                  case DUMP_MEM_SWAP:
                     xil_printf("Address (hex): ");
                     debug_console.subcmd=1;
                     debug_console.cmd=DM;
                     break;
                  case S:
                  case STEP:
                     debug_console.subcmd=0;
                     debug_console.step=1;
                     break;
                  case INW:
                  case INC_NOPS_WRITE:
                     shared->nops_write+=1;
                     xil_printf("NOPS WRITE %d\r\n",shared->nops_write);
                     debug_console.subcmd=0;
                     break;
                  case DNW:
                  case DEC_NOPS_WRITE:
                     shared->nops_write-=1;
                     xil_printf("NOPS WRITE %d\r\n",shared->nops_write);
                     debug_console.subcmd=0;
                     break;
                  case INR:
                  case INC_NOPS_READ:
                     shared->nops_read+=1;
                     xil_printf("NOPS READ %d\r\n",shared->nops_read);
                     debug_console.subcmd=0;
                     break;
                  case DNR:
                  case DEC_NOPS_READ:
                     shared->nops_read-=1;
                     xil_printf("NOPS READ %d\r\n",shared->nops_read);
                     debug_console.subcmd=0;
                     break;
                  case INE:
                  case INC_NAG_ETH:
                     eth_backlog_nag_counter_max+=100;
                     xil_printf("NAG ETH %d\r\n",eth_backlog_nag_counter_max);
                     debug_console.subcmd=0;
                     break;
                  case DNE:
                  case DEC_NAG_ETH:
                     eth_backlog_nag_counter_max-=100;
                     if(eth_backlog_nag_counter_max<0) eth_backlog_nag_counter_max=1;
                     xil_printf("NAG ETH %d\r\n",eth_backlog_nag_counter_max);
                     debug_console.subcmd=0;
                     break;
                  case DIS:
                  case DISASSEMBLE:
                     shared->disassemble=!shared->disassemble;
                     if(shared->disassemble)
                        xil_printf("DISASSEMBLE ON\r\n");
                     else
                        xil_printf("DISASSEMBLE OFF\r\n");
                     shared->musashi_step=1;
                     debug_console.subcmd=0;
                     break;
                  case DISS:
                  case DISASSEMBLE_STEP:
                     xil_printf("DISASSEMBLE STEP\r\n");
                     shared->disassemble=1;
                     shared->musashi_step=1;
                     debug_console.subcmd=0;
                     break;
                  case DISR:
                  case DISASSEMBLE_RUN:
                     xil_printf("DISASSEMBLE RUN\r\n");
                     shared->disassemble=1;
                     shared->musashi_step=0xFFFFFFFF;
                     debug_console.subcmd=0;
                     break;
                  case RSTE:
                  case RESET_EMULATOR:
                     xil_printf("RESET EMULATOR\r\n");
                     shared->reset_emulator_dis=1;
                     shared->disassemble=1;
                     shared->musashi_step=1;
                     debug_console.subcmd=0;
                     break;
                  case SI2C:
                  case STOP_I2C:
                     debug_console.stop_i2c=!debug_console.stop_i2c;
                     if(debug_console.stop_i2c)
                        xil_printf("STOP I2C ON\r\n");
                     else
                        xil_printf("STOP I2C OFF\r\n");
                     debug_console.subcmd=0;
                     break;
                  case TRB:
                  case TOGGLE_READ_BURST:
                     debug_console.toggle_read_burst=!debug_console.toggle_read_burst;
                     if(debug_console.toggle_read_burst)
                     {
                        ENABLE_BURST_READ_FPGA;
                        xil_printf("READ BURST ON\r\n");
                     }
                     else
                     {
                        DISABLE_BURST_READ_FPGA;
                        xil_printf("READ BURST OFF\r\n");
                     }
                     debug_console.subcmd=0;
                     break;
                  case TWB:
                  case TOGGLE_WRITE_BURST:
                     debug_console.toggle_write_burst=!debug_console.toggle_write_burst;
                     if(debug_console.toggle_write_burst)
                     {
                        ENABLE_BURST_WRITE_FPGA;
                        xil_printf("WRITE BURST ON\r\n");
                     }
                     else
                     {
                        DISABLE_BURST_WRITE_FPGA;
                        xil_printf("WRITE BURST OFF\r\n");
                     }
                     debug_console.subcmd=0;
                     break;
                  case CCP:
                  case CHANGE_CLOCK_PHASE:
                     xil_printf("PCLK PHASE [%d]: ",debug_console.clocks.PCLK.phase);
                     debug_console.subcmd=3;
                     debug_console.cmd=0;
                     break;
                  case DCP:
                  case CHANGE_DUTY_CYCLE:
                     xil_printf("PCLK DUTY CYCLE [%d]: ",debug_console.clocks.PCLK.dutycycle);
                     debug_console.subcmd=10;
                     debug_console.cmd=0;
                     break;
                  case CCF:
                  case CHANGE_CLOCK_FREQ:
                     xil_printf("PCLK FREQ [%d]: ",config.cpufreq);
                     debug_console.subcmd=9;
                     debug_console.cmd=0;
                     break;
                  case RCPLD:
                  case RESET_CPLD:
                     debug_console.reset_cpld=!debug_console.reset_cpld;
                     if(debug_console.reset_cpld)
                     {
                        xil_printf("RESET CPLD ON\r\n");
                        CPLD_RESET_ARM(0);
                     }
                     else
                     {
                        xil_printf("RESET CPLD OFF\r\n");
                        CPLD_RESET_ARM(1);
                     }
                     debug_console.subcmd=0;
                     break;
                  case RFPGA:
                  case RESET_FPGA:
                     debug_console.reset_fpga=!debug_console.reset_fpga;
                     if(debug_console.reset_fpga)
                     {
                        xil_printf("RESET FPGA ON\r\n");
                        DiscreteSet(REG0, FPGA_RESET);
                     }
                     else
                     {
                        xil_printf("RESET FPGA OFF\r\n");
                        DiscreteClear(REG0, FPGA_RESET);
                     }
                     debug_console.subcmd=0;
                     break;
                  case RAMIGA:
                  case RESET_AMIGA:
                     xil_printf("RESET CPLD ON\r\n");
                     CPLD_RESET_ARM(0);
                     usleep(50000);
                     xil_printf("RESET FPGA ON\r\n");
                     DiscreteSet(REG0, FPGA_RESET);
                     usleep(50000);
                     xil_printf("RESET FPGA OFF\r\n");
                     DiscreteClear(REG0, FPGA_RESET);
                     usleep(50000);
                     // it will be off later
//                     xil_printf("RESET CPLD OFF\r\n");
//                     CPLD_RESET_ARM(1);
//                     usleep(500000);
                     debug_console.subcmd=0;
                     break;
                  case RARM:
                  case RESET_ARM:
                     hard_reboot();
                     debug_console.subcmd=0;
                     break;
                  case JIT:
                  case TOGGLE_JIT:
                     write_rtg_register(REG_ZZ_JIT_ENABLE,!read_rtg_register(REG_ZZ_JIT_ENABLE));
                     printf("JIT %s\n",read_rtg_register(REG_ZZ_JIT_ENABLE)?"ON":"OFF");
                     break;
                  case SCT:
                  case SAVE_CURRENT_TIMINGS:
                     write_timings();
                     break;
                  case PHD:
                  case PRINT_HIST_DATAABORT:
                     shared->printhist_dataabort=1;
                     break;
                  case VMR:
                  case VIDEO_MODE_RESET:
                     printf("Video mode reset...\n");
                     video_mode_reset();
                     break;
                  case OVL:
                  case TOGGLE_OVERLAY:
                     debug_console.overlay_enabled=!debug_console.overlay_enabled;
                     if(debug_console.overlay_enabled)
                     {
                        enable_overlay();
                        xil_printf("OVERLAY ENABLED\r\n");
                     }
                     else
                     {
                        disable_overlay();
                        xil_printf("OVERLAY DISABLED\r\n");
                     }
                     debug_console.subcmd=0;
                     break;
                  default:
                     xil_printf("Not defined command '%s'. Type 'help' or 'h' for help.\r\n",debug_console.cmd_buf);
                     debug_console.subcmd=0;
                  }
                  break;
               }
            }
            if(command_found==0)
               command_found=prof_try_explicit_rate(debug_console.cmd_buf);
            if(command_found==0)
            {
               xil_printf("Unknown command '%s'. Type 'help' or 'h' for help.\r\n",debug_console.cmd_buf);
               debug_console.subcmd=0;
            }
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
      }
      else if(debug_console.subcmd==1)
      {
         if(c>='a' && c<='z') c+='A'-'a';
         if((c>='0' && c<='9')||(c>='A' && c<='F')||
               (c=='H' && debug_console.cmd_pointer==0)||
               (c=='X' && debug_console.cmd_pointer==1)||(c==8))
         {
            if(c=='X') c='x';
            if(c=='H') c='h';
            XUartPs_SendByte(STDOUT_BASEADDRESS, c);
            if(c==8) // backspace
            {
               if(debug_console.cmd_pointer>0)
               {
                  XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                  XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                  debug_console.cmd_pointer--;
               }
               debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            }
            else
            {
               debug_console.cmd_buf[debug_console.cmd_pointer++]=c;
               if(debug_console.cmd_pointer>90) debug_console.cmd_pointer=90;
            }
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer>0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            XUartPs_SendByte(STDOUT_BASEADDRESS, '\r');
            XUartPs_SendByte(STDOUT_BASEADDRESS, '\n');
            debug_console.data1=hextoi(debug_console.cmd_buf);
            xil_printf("Length (hex): ");
            debug_console.subcmd=2;
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
      }
      else if(debug_console.subcmd==2)
      {
         if(c>='a' && c<='z') c+='A'-'a';
         if((c>='0' && c<='9')||(c>='A' && c<='F')||
               (c=='H' && debug_console.cmd_pointer==0)||
               (c=='X' && debug_console.cmd_pointer==1)||(c==8))
         {
            if(c=='X') c='x';
            if(c=='H') c='h';
            XUartPs_SendByte(STDOUT_BASEADDRESS, c);
            if(c==8) // backspace
            {
               if(debug_console.cmd_pointer>0)
               {
                  XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                  XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                  debug_console.cmd_pointer--;
               }
               debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            }
            else
            {
               debug_console.cmd_buf[debug_console.cmd_pointer++]=c;
               if(debug_console.cmd_pointer>90) debug_console.cmd_pointer=90;
            }
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer>0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            XUartPs_SendByte(STDOUT_BASEADDRESS, '\r');
            XUartPs_SendByte(STDOUT_BASEADDRESS, '\n');
            debug_console.data2=hextoi(debug_console.cmd_buf);
            if(debug_console.cmd==DM)
            {
               debug_console.cmd=0;
               debug_console.subcmd=0;
               debug_console.cmd_buf[0]=0;
               debug_console.cmd_pointer=0;
               dump_mem(debug_console.data1,debug_console.data2,0);
            }
            else if(debug_console.cmd==DMS)
            {
               debug_console.cmd=0;
               debug_console.subcmd=0;
               debug_console.cmd_buf[0]=0;
               debug_console.cmd_pointer=0;
               dump_mem(debug_console.data1,debug_console.data2,1);
            }
         }
      }
      else if(debug_console.subcmd>=3 && debug_console.subcmd<=8)
      {
         if((c>='0' && c<='9')||
               (c=='-' && debug_console.cmd_pointer==0)||(c==8))
         {
            XUartPs_SendByte(STDOUT_BASEADDRESS, c);
            if(c==8) // backspace
            {
               if(debug_console.cmd_pointer>0)
               {
                  XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                  XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                  debug_console.cmd_pointer--;
               }
               debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            }
            else
            {
               debug_console.cmd_buf[debug_console.cmd_pointer++]=c;
               if(debug_console.cmd_pointer>90) debug_console.cmd_pointer=90;
            }
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer==0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            switch(debug_console.subcmd-3)
            {
            case 0:
               xil_printf("\r\nCLKEN PHASE [%d]: ",debug_console.clocks.CLKEN.phase);
               debug_console.subcmd++;
               break;
            case 1:
               xil_printf("\r\nBCLK PHASE [%d]: ",debug_console.clocks.BCLK.phase);
               debug_console.subcmd++;
               break;
            case 2:
               xil_printf("\r\nCPUCLK PHASE [%d]: ",debug_console.clocks.CPUCLK.phase);
               debug_console.subcmd++;
               break;
            case 3:
               xil_printf("\r\nCLK90 PHASE [%d]: ",debug_console.clocks.CLK90.phase);
               debug_console.subcmd++;
               break;
            case 4:
               xil_printf("\r\nEMU EXTRA PHASE [%d]: ",debug_console.clocks.emu_etra_phase);
               debug_console.subcmd++;
               break;
            case 5: {
               debug_console.cmd=0;
               debug_console.subcmd=0;
               int emu;
               if(boot_mode_is_emulated(config.boot_mode))   // closed real-CPU list; see config_file.h
                  emu=1;
               else
                  emu=0;
               configure_clk(config.cpufreq,1,emu);
               }
               break;
            }
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer>0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            switch(debug_console.subcmd-3)
            {
            case 0:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.PCLK.phase=debug_console.sdata1;
               debug_console.clocks.clk_index=get_clock_index(config.cpufreq);
//               xil_printf("\r\nCLK INDEX %d",debug_console.clocks.clk_index);
               cd[debug_console.clocks.clk_index].pclk.phase=debug_console.sdata1;
               xil_printf("\r\nCLKEN PHASE [%d]: ",debug_console.clocks.CLKEN.phase);
               debug_console.subcmd++;
               break;
            case 1:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.CLKEN.phase=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].clken.phase=debug_console.sdata1;
               xil_printf("\r\nBCLK PHASE [%d]: ",debug_console.clocks.BCLK.phase);
               debug_console.subcmd++;
               break;
            case 2:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.BCLK.phase=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].bclk.phase=debug_console.sdata1;
               xil_printf("\r\nCPUCLK PHASE [%d]: ",debug_console.clocks.CPUCLK.phase);
               debug_console.subcmd++;
               break;
            case 3:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.CPUCLK.phase=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].cpuclk.phase=debug_console.sdata1;
               xil_printf("\r\nCLK90 PHASE [%d]: ",debug_console.clocks.CLK90.phase);
               debug_console.subcmd++;
               break;
            case 4:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.CLK90.phase=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].clk90.phase=debug_console.sdata1;
               xil_printf("\r\nEMU EXTRA PHASE [%d]: ",debug_console.clocks.emu_etra_phase);
               debug_console.subcmd++;
               break;
            case 5: {
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.emu_etra_phase=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].emu_extra_phase=debug_console.sdata1;
               debug_console.cmd=0;
               debug_console.subcmd=0;
               int emu;
               if(boot_mode_is_emulated(config.boot_mode))   // closed real-CPU list; see config_file.h
                  emu=1;
               else
                  emu=0;
               configure_clk(config.cpufreq,1,emu);
               }
               break;
            }
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
      }
      else if(debug_console.subcmd==9)
      {
         if((c>='0' && c<='9')||
            (c==8))
         {
            XUartPs_SendByte(STDOUT_BASEADDRESS, c);
            if(c==8) // backspace
            {
               if(debug_console.cmd_pointer>0)
               {
                  XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                  XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                  debug_console.cmd_pointer--;
               }
               debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            }
            else
            {
               debug_console.cmd_buf[debug_console.cmd_pointer++]=c;
               if(debug_console.cmd_pointer>90) debug_console.cmd_pointer=90;
            }
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer==0)
         {
            //Do nothing
            debug_console.cmd=0;
            debug_console.subcmd=0;
            int emu;
            if(boot_mode_is_emulated(config.boot_mode))   // closed real-CPU list; see config_file.h
               emu=1;
            else
               emu=0;
            configure_clk(config.cpufreq,1,emu);
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer>0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            debug_console.sdata1=atoi(debug_console.cmd_buf);
            config.cpufreq=debug_console.sdata1;
            debug_console.cmd=0;
            debug_console.subcmd=0;
            int emu;
            if(boot_mode_is_emulated(config.boot_mode))   // closed real-CPU list; see config_file.h
               emu=1;
            else
               emu=0;
            configure_clk(config.cpufreq,1,emu);
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
      }
      else if(debug_console.subcmd>=10 && debug_console.subcmd<=14)
      {
         if((c>='0' && c<='9')||
               (c=='-' && debug_console.cmd_pointer==0)||(c==8))
         {
            XUartPs_SendByte(STDOUT_BASEADDRESS, c);
            if(c==8) // backspace
            {
               if(debug_console.cmd_pointer>0)
               {
                  XUartPs_SendByte(STDOUT_BASEADDRESS, ' ');
                  XUartPs_SendByte(STDOUT_BASEADDRESS, 8);
                  debug_console.cmd_pointer--;
               }
               debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            }
            else
            {
               debug_console.cmd_buf[debug_console.cmd_pointer++]=c;
               if(debug_console.cmd_pointer>90) debug_console.cmd_pointer=90;
            }
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer==0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            switch(debug_console.subcmd-10)
            {
            case 0:
               xil_printf("\r\nCLKEN Duty Cycle [%d]: ",debug_console.clocks.CLKEN.dutycycle);
               debug_console.subcmd++;
               break;
            case 1:
               xil_printf("\r\nBCLK Duty Cycle [%d]: ",debug_console.clocks.BCLK.dutycycle);
               debug_console.subcmd++;
               break;
            case 2:
               xil_printf("\r\nCPUCLK Duty Cycle [%d]: ",debug_console.clocks.CPUCLK.dutycycle);
               debug_console.subcmd++;
               break;
            case 3:
               xil_printf("\r\nCLK90 Duty Cycle [%d]: ",debug_console.clocks.CLK90.dutycycle);
               debug_console.subcmd++;
               break;
            case 4: {
               debug_console.cmd=0;
               debug_console.subcmd=0;
               int emu;
               if(boot_mode_is_emulated(config.boot_mode))   // closed real-CPU list; see config_file.h
                  emu=1;
               else
                  emu=0;
               configure_clk(config.cpufreq,1,emu);
               }
               break;
            }
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
         else if((c=='\r' || c=='\n') && debug_console.cmd_pointer>0)
         {
            debug_console.cmd_buf[debug_console.cmd_pointer]=0;
            switch(debug_console.subcmd-10)
            {
            case 0:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.PCLK.dutycycle=debug_console.sdata1;
               debug_console.clocks.clk_index=get_clock_index(config.cpufreq);
               cd[debug_console.clocks.clk_index].pclk.dutycycle=debug_console.sdata1;
               xil_printf("\r\nCLKEN Duty Cycle [%d]: ",debug_console.clocks.CLKEN.dutycycle);
               debug_console.subcmd++;
               break;
            case 1:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.CLKEN.dutycycle=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].clken.dutycycle=debug_console.sdata1;
               xil_printf("\r\nBCLK Duty Cycle [%d]: ",debug_console.clocks.BCLK.dutycycle);
               debug_console.subcmd++;
               break;
            case 2:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.BCLK.dutycycle=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].bclk.dutycycle=debug_console.sdata1;
               xil_printf("\r\nCPUCLK Duty Cycle [%d]: ",debug_console.clocks.CPUCLK.dutycycle);
               debug_console.subcmd++;
               break;
            case 3:
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.CPUCLK.dutycycle=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].cpuclk.dutycycle=debug_console.sdata1;
               xil_printf("\r\nCLK90 Duty Cycle [%d]: ",debug_console.clocks.CLK90.dutycycle);
               debug_console.subcmd++;
               break;
            case 4: {
               debug_console.sdata1=atoi(debug_console.cmd_buf);
               debug_console.clocks.CLK90.dutycycle=debug_console.sdata1;
               cd[debug_console.clocks.clk_index].clk90.dutycycle=debug_console.sdata1;
               debug_console.cmd=0;
               debug_console.subcmd=0;
               int emu;
               if(boot_mode_is_emulated(config.boot_mode))   // closed real-CPU list; see config_file.h
                  emu=1;
               else
                  emu=0;
               configure_clk(config.cpufreq,1,emu);
               }
               break;
            }
            debug_console.cmd_buf[0]=0;
            debug_console.cmd_pointer=0;
         }
      }
   }
   PT_END(pt);
}
void debug_console_help(void)
{
   xil_printf("Commands:\r\n");
   xil_printf("'H'       or 'HELP' for this help.\r\n");
   xil_printf("'DRTG'    or 'DEBUG RTG' for toggling debug RTG reads/writes.\r\n");
   xil_printf("'DUSB'    or 'DEBUG USB' for toggling debug USB reads/writes.\r\n");
   xil_printf("'DSCSI'   or 'DEBUG SCSI' for toggling debug SCSI commands.\r\n");
   xil_printf("'DAUDIO'  or 'DEBUG AUDIO' for toggling debug AHI/MHI commands.\r\n");
   xil_printf("'DI2C'    or 'DEBUG I2C' for toggling debug i2c (LTC2990)\r\n");
   xil_printf("'DENET'   or 'DEBUG ENET' for toggling debug ETHERNET commands.\r\n");
   xil_printf("'DSOFT3D' or 'DEBUG SOFT3D' for toggling debug SOFT3D commands.\r\n");
   xil_printf("'SD'      or 'STOP DEBUG' for stop all debugging.\r\n");
   xil_printf("'DMMU'    or 'DUMP MMU' for dumping Core0 MMU content.\r\n");
   xil_printf("'DM'      or 'DUMP MEM' for dumping Core0 MEM content.\r\n");
   xil_printf("'DMS'     or 'DUMP MEM SWAP' for dumping Core0 MEM content with swap32.\r\n");
   xil_printf("'S'       or 'STEP' for debugging RTG commands step by step.\r\n"
              "                   'C' cancels step mode, and any other key makes a step\r\n");
   xil_printf("'INW'     or 'INC NOPS WRITE' increments EMU write delay\r\n");
   xil_printf("'DNW'     or 'DEC NOPS WRITE' decrements EMU write delay\r\n");
   xil_printf("'INR'     or 'INC NOPS READ' increments EMU read delay\r\n");
   xil_printf("'DNR'     or 'DEC NOPS READ' decrements EMU read delay\r\n");
   xil_printf("'INE'     or 'INC NAG ETH' increments Ethernet nag counter\r\n");
   xil_printf("'DNE'     or 'DEC NAG ETH' decrements Ethernet nag counter\r\n");
   xil_printf("'DIS'     or 'DISASSEMBLE' enable disassemble (Musashi only)\r\n");
   xil_printf("'DISS'    or 'DISASSEMBLE STEP' step disassemble (Musashi only)\r\n");
   xil_printf("'DISR'    or 'DISASSEMBLE RUN' run disassemble (Musashi only)\r\n");
   xil_printf("'RSTE'    or 'RESET EMULATOR' reset emulator (Musashi only)\r\n");
   xil_printf("'SI2C'    or 'STOP I2C' for toggling stop i2c (LTC2990)\r\n");
   xil_printf("'TRB'     or 'TOGGLE READ BURST' for toggling Read Burst\r\n");
   xil_printf("'TWB'     or 'TOGGLE WRITE BURST' for toggling Write Burst\r\n");
   xil_printf("'CCP'     or 'CHANGE CLOCK PHASE' for changing clock phases\r\n");
   xil_printf("'DCP'     or 'CHANGE DUTY CYCLE' for changing clock duty cycles\r\n");
   xil_printf("'CCF'     or 'CHANGE CLOCK FREQ' for changing clock fequency\r\n");
   xil_printf("'RCPLD'   or 'RESET CPLD' for toggling reset CPLD\r\n");
   xil_printf("'RFPGA'   or 'RESET FPGA' for toggling reset FPGA\r\n");
   xil_printf("'RAMIGA'  or 'RESET AMIGA' for resetting the AMIGA (sequence of the two above)\r\n");
   xil_printf("'RARM'    or 'RESET ARM' for resetting the ARM (reboot the entire system)\r\n");
   xil_printf("'JIT'     or 'TOGGLE_JIT' for toggling ON/OFF JIT CPU emulation\r\n");
   xil_printf("'SCT'     or 'SAVE_CURRENT_TIMINGS' for saving current timings on the timings file\r\n");
   xil_printf("'PHD'     or 'PRINT_HIST_DATAABORT' for printing the data abort histogram (EMU only)\r\n");
   xil_printf("'VMR'     or 'VIDEO_MODE_RESET' for reseting vdma\r\n");
   xil_printf("'OVL'     or 'TOGGLE_OVERLAY' for toggling the debug overlay\r\n");
   xil_printf("'DEMU'    or 'DEBUG EMU' toggles emulator-core debug spam + AMIX strand trace\r\n");
   xil_printf("'PERF'    or 'PERF REPORT' toggles the 030-MMU instr/sec readout (~1Hz, EMU)\r\n");
   xil_printf("'SERV'    or 'SERVICE CADENCE' cycles 030-MMU IPL/task poll cadence 1->2->4..64 (EMU)\r\n");
   xil_printf("'PSTAT'   or 'PISCSI STATS REPORT' toggles the periodic piscsi + core0-loop reports\r\n");
   xil_printf("'PSTATR'  or 'PISCSI STATS RESET' dumps piscsi cumulative totals + CLMT state, resets\r\n");
   xil_printf("'PSTATC'  or 'CORE0 LOOP DUMP' dumps per-task core0 loop totals (who blocks, how long)\r\n");
   xil_printf("'PSTATZ'  or 'CORE0 LOOP ZERO' zeroes the core0 loop totals and worst-call latch\r\n");
   xil_printf("'PERFS'   or 'PERF STATS' toggles core1 per-instruction perf accounting (EMU)\r\n");
   xil_printf("'PROBES'  or 'EMU PROBES' toggles the core1 run-loop diagnostic probes (EMU)\r\n");
   xil_printf("'CSTAT'   or 'CORE0 LOOP STATS' toggles core0 C0STAT span STAMPING (not just printing)\r\n");
   xil_printf("--- CYBERSTORM C1 interpreter profiler (PROFILING build only; that image says so at boot) ---\r\n");
   xil_printf("'PROF'    cycles the guest-PC sampler: off -> 250 -> 1000 -> 4000 Hz -> off (arming zeroes the data)\r\n");
   xil_printf("'PROF <hz>' arms at an EXPLICIT rate, 1..20000 (also zeroes). Prints the wrap-free window\r\n");
   xil_printf("'PROFO'   or 'PROF OFF' freezes sampler AND buckets and KEEPS the data -- the lossless stop\r\n");
   xil_printf("'PROFB'   toggles the interpreter STAGE BUCKETS -- costly: read the shares, not the rate\r\n");
   xil_printf("'PROFD'   dumps stage buckets + counters (~40 lines)\r\n");
   xil_printf("'PROFR'   dumps the newest 4096 PC samples (~8 s of serial); 'PROF RING' dumps all 65535 (~2 min)\r\n");
   xil_printf("'PROFZ'   zeroes buckets, counters and the ring without disarming\r\n");
   xil_printf("'BLK'     or 'BLOCK IDIOM' cycles the C2 block-idiom chunk cap OFF->64->256->1024 (EMU)\r\n");
   xil_printf("'DOPC'    or 'DECODED OP CACHE' toggles the C2 decoded-op cache ON/OFF (EMU)\r\n");
   xil_printf("'DFAST'   or 'ACCESSOR FAST PATH' toggles the C2 data-accessor fast path ON/OFF (EMU); OFF is the reference arm\r\n");
   xil_printf("'SPEC'    or 'HANDLER SPECIALISE' toggles the C2 HANDLER discriminator ON/OFF (EMU); OFF is the reference arm\r\n");
   xil_printf("          config keys prof_hz / prof_buckets arm it from power-on, for profiling the BOOT\r\n");
   xil_printf("          stop a config-armed capture with 'PROF OFF' -- every rung of the PROF ladder ARMS,\r\n");
   xil_printf("          and arming zeroes; turn PSTAT OFF before any long dump (both cores share one UART)\r\n");
}
#endif
