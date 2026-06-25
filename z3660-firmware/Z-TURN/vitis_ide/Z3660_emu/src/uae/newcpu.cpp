/*
* UAE - The Un*x Amiga Emulator
*
* MC68000 emulation
*
* (c) 1995 Bernd Schmidt
*/

#define MMUOP_DEBUG 2
#define DEBUG_CD32CDTVIO 0
#define EXCEPTION3_DEBUGGER 0
#define CPUTRACE_DEBUG 0

#define VALIDATE_68030_DATACACHE 0
#define VALIDATE_68040_DATACACHE 0
#define DISABLE_68040_COPYBACK 0

#define MORE_ACCURATE_68020_PIPELINE 1

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "events.h"
#define sleep sleep2
#define usleep usleep2
#include <sleep.h>
#undef usleep
#undef sleep
//#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "../main.h"
#include "cpummu.h"
#include "cpummu030.h"
#include "cputbl.h"
#include "cpu_prefetch.h"
#include "uae/uaestring.h"

#include <xil_cache_l.h>
#include <xil_io.h>
#include "xscuwdt.h"
#include "xgpiops.h"
#include "xparameters.h"

//#include "autoconf.h"
//#include "traps.h"
//#include "debug.h"
//#include "debugmem.h"
//#include "gui.h"
//#include "savestate.h"
//#include "blitter.h"
//#include "ar.h"
//#include "gayle.h"
//#include "cia.h"
//#include "inputrecord.h"
//#include "inputdevice.h"
//#include "audio.h"
#include "fpp.h"
//#include "statusline.h"
#ifdef WITH_PPC
#include "uae/ppc.h"
#endif
//#include "cpuboard.h"
//#include "threaddep/thread.h"
#ifdef WITH_X86
#include "x86.h"
#endif
//#include "bsdsocket.h"
//#include "devices.h"
#ifdef WITH_DRACO
#include "draco.h"
#endif
extern volatile int read_reset;
int read_reset_last=1;
extern "C" void cpu_emulator_reset_core0(void);
extern "C" void reset_autoconfig(void);
extern int ovl;
void fill_prefetch_quick (void);
void custom_reset_cpu(bool hardreset, bool keyboardreset);
void hard_reboot(void);
extern "C" { extern volatile int a3000_amix_mode; }   // AMIX (emulated A3000 SCSI) active; gates the warm-reboot guard

/* ---- Z3660 warm-reset real-chipset quiesce -------------------------------------
 * A real Amiga RESET resets Paula/CIA; this emulated warm (guest) reset does NOT, so
 * AMIX leaves chip interrupts enabled+latched and the rebooted Kickstart -- which has
 * not re-installed its handlers yet -- drowns in an EXTER (level-6) interrupt it can
 * never clear (the storm observed at Kickstart 0x00F81212, sampler s=1 msk=2 = a
 * level>2 IRQ held permanently asserted; a cold boot never hits this because the chips
 * start reset).  We run on Core1, the live bus master for the emulated 68k, so these
 * chip accesses are timeout-bounded (ps_*->arm_*_amiga_* wait_*_ack) and do NOT hang
 * -- unlike the abandoned Core0 attempt whose arm_read_amiga spun on an ack that never
 * came in the reset window.  Disable+clear all real Paula interrupts and clear both CIA
 * ICR latches, then force the injected IPL to 0: ipl_main_read() only re-samples the
 * GPIO IPL lines when ipl_read>0, so bump it and re-sample (now de-asserted), then
 * hard-zero the two globals intlev() max()es over (read_irq, a3000_scsi_irq). */
extern "C" void ps_write_16(unsigned int address, unsigned int value);
extern "C" unsigned int ps_read_8(unsigned int address);
extern "C" void ipl_main_read(void);
extern int read_irq;
extern volatile int a3000_scsi_irq;
extern int ipl_read;
static void z3660_quiesce_real_chipset_on_reset(void)
{
   ps_write_16(0x00DFF09A, 0x7FFF);   /* INTENA: clear master + all enable bits */
   ps_write_16(0x00DFF09C, 0x7FFF);   /* INTREQ: clear all pending bits         */
   (void)ps_read_8(0x00BFED01);       /* CIA-A ICR: reading clears the latch    */
   (void)ps_read_8(0x00BFDD00);       /* CIA-B ICR: reading clears the latch    */
   ipl_read = 4;
   for (int i = 0; i < 8; i++) ipl_main_read();   /* re-snapshot now-idle IPL lines */
   read_irq = 0;
   a3000_scsi_irq = 0;
}
#ifdef JIT
#include "jit/compemu.h"
#include <signal.h>
#else
/* Need to have these somewhere */
bool check_prefs_changed_comp (bool checkonly) { return false; }
#endif
/* For faster JIT cycles handling */
//int pissoff = 0;

/* Opcode of faulting instruction */
static uae_u32 last_op_for_exception_3;
/* PC at fault time */
static uaecptr last_addr_for_exception_3;
/* Address that generated the exception */
static uaecptr last_fault_for_exception_3;
/* read (0) or write (1) access */
static bool last_writeaccess_for_exception_3;
/* size */
static bool last_size_for_exception_3;
/* FC */
static int last_fc_for_exception_3;
/* Data (1) or instruction fetch (0) */
static int last_di_for_exception_3;
/* not instruction */
static bool last_notinstruction_for_exception_3;
/* set when writing exception stack frame */
static int exception_in_exception;
/* secondary SR for handling 68040 bug */
static uae_u16 last_sr_for_exception3;

int mmu_enabled, mmu_triggered;
int cpu_cycles;
int hardware_bus_error;
//static int baseclock;
int m68k_pc_indirect;
bool m68k_interrupt_delay;
static bool m68k_reset_delay;
//static bool need_opcode_swap;

//static volatile uae_atomic uae_interrupt;
//static volatile uae_atomic uae_interrupts2[IRQ_SOURCE_MAX];
//static volatile uae_atomic uae_interrupts6[IRQ_SOURCE_MAX];

static int cpu_prefs_changed_flag;

int cpucycleunit;
int cpu_tracer;

const int areg_byteinc[] = { 1, 1, 1, 1, 1, 1, 1, 2 };
const int imm8_table[] = { 8, 1, 2, 3, 4, 5, 6, 7 };

int movem_index1[256];
int movem_index2[256];
int movem_next[256];

cpuop_func *cpufunctbl[65536];
cpuop_func *loop_mode_table[65536];

struct cputbl_data
{
   uae_s16 length;
   uae_s8 disp020[2];
   uae_s8 branch;
};
static struct cputbl_data cpudatatbl[65536];

/* Two elements: index 1 is driven by the imported 68030-MMU table (cpuemu_32.cpp
 * MOVES/CAS2/double-(An) handlers), the m68k_run_mmu030 CATCH path below, and
 * cpummu030.cpp's mmu030fixupreg(1)/mmu030fixupmod(...,1). Upstream WinUAE 4.4.0
 * sizes this [2]; the fork had shrunk it to [1] (UB once MMU mode drives idx 1). */
struct mmufixup mmufixup[2];
static_assert(sizeof(mmufixup) / sizeof(mmufixup[0]) >= 2,
              "mmufixup must hold index 1 (MMU030_REG_FIXUP) used by cpuemu_32 + run-loop CATCH");

static uae_u64 fake_srp_030, fake_crp_030;
static uae_u32 fake_tt0_030, fake_tt1_030, fake_tc_030;
static uae_u16 fake_mmusr_030;

int cpu_last_stop_vpos, cpu_stopped_lines;

static void exception3_notinstruction(uae_u32 opcode, uaecptr addr);

/*

 ok, all this to "record" current instruction state
 for later 100% cycle-exact restoring

 */

static uae_u32 (*x2_next_iword)(void);
static uae_u32 (*x2_next_ilong)(void);
static uae_u32 (*x2_get_iword)(int);
static uae_u32 (*x2_get_long)(uaecptr);
static uae_u32 (*x2_get_word)(uaecptr);
static uae_u32 (*x2_get_byte)(uaecptr);
static void (*x2_put_long)(uaecptr, uae_u32);
static void (*x2_put_word)(uaecptr, uae_u32);
static void (*x2_put_byte)(uaecptr, uae_u32);
static void (*x2_do_cycles)(int);
static void (*x2_do_cycles_pre)(int);
static void (*x2_do_cycles_post)(int, uae_u32);

STATIC_INLINE uae_u32 get_long_ce000 (uaecptr addr);
STATIC_INLINE uae_u32 get_word_ce000 (uaecptr addr);
STATIC_INLINE uae_u32 get_byte_ce000 (uaecptr addr);
STATIC_INLINE uae_u32 get_wordi_ce000 (int offset);
STATIC_INLINE uae_u32 get_word_ce000_prefetch (int o);
STATIC_INLINE void put_long_ce000 (uaecptr addr, uae_u32 v);
STATIC_INLINE void put_word_ce000 (uaecptr addr, uae_u32 v);
STATIC_INLINE void put_byte_ce000 (uaecptr addr, uae_u32 v);


uae_u32 (*x_next_iword)(void);
uae_u32 (*x_next_ilong)(void);
uae_u32 (*x_get_iword)(int);
uae_u32 (*x_get_long)(uaecptr);
uae_u32 (*x_get_word)(uaecptr);
uae_u32 (*x_get_byte)(uaecptr);
void (*x_put_long)(uaecptr, uae_u32);
void (*x_put_word)(uaecptr, uae_u32);
void (*x_put_byte)(uaecptr, uae_u32);

uae_u32 (*x_cp_next_iword)(void);
uae_u32 (*x_cp_next_ilong)(void);
uae_u32 (*x_cp_get_long)(uaecptr);
uae_u32 (*x_cp_get_word)(uaecptr);
uae_u32 (*x_cp_get_byte)(uaecptr);
void (*x_cp_put_long)(uaecptr, uae_u32);
void (*x_cp_put_word)(uaecptr, uae_u32);
void (*x_cp_put_byte)(uaecptr, uae_u32);

void (*x_do_cycles)(int);
void (*x_do_cycles_pre)(int);
void (*x_do_cycles_post)(int, uae_u32);

static void set_x_cp_funcs(void)
{
   x_cp_put_long = x_put_long;
   x_cp_put_word = x_put_word;
   x_cp_put_byte = x_put_byte;
   x_cp_get_long = x_get_long;
   x_cp_get_word = x_get_word;
   x_cp_get_byte = x_get_byte;
   x_cp_next_iword = x_next_iword;
   x_cp_next_ilong = x_next_ilong;
}

static struct cputracestruct cputrace;

STATIC_INLINE void clear_trace (void)
{
   if (cputrace.memoryoffset == MAX_CPUTRACESIZE)
      return;
   struct cputracememory *ctm = &cputrace.ctm[cputrace.memoryoffset++];
   if (cputrace.memoryoffset == MAX_CPUTRACESIZE) {
      write_log(_T("CPUTRACE overflow, stopping tracing.\n"));
      return;
   }
   ctm->mode = 0;
   cputrace.cyclecounter = 0;
   cputrace.cyclecounter_pre = cputrace.cyclecounter_post = 0;
}
static void set_trace (uaecptr addr, int accessmode, int size)
{
   if (cputrace.memoryoffset == MAX_CPUTRACESIZE)
      return;
   struct cputracememory *ctm = &cputrace.ctm[cputrace.memoryoffset++];
   if (cputrace.memoryoffset == MAX_CPUTRACESIZE) {
      write_log(_T("CPUTRACE overflow, stopping tracing.\n"));
      return;
   }
   ctm->addr = addr;
   ctm->data = 0xdeadf00d;
   ctm->mode = accessmode | (size << 4);
   cputrace.cyclecounter_pre = -1;
   if (accessmode == 1)
      cputrace.writecounter++;
   else
      cputrace.readcounter++;
}
static void add_trace (uaecptr addr, uae_u32 val, int accessmode, int size)
{
   if (cputrace.memoryoffset < 1) {
      return;
   }
   int mode = accessmode | (size << 4);
   struct cputracememory *ctm = &cputrace.ctm[cputrace.memoryoffset - 1];
   ctm->addr = addr;
   ctm->data = val;
   if (!ctm->mode) {
      ctm->mode = mode;
      if (accessmode == 1)
         cputrace.writecounter++;
      else
         cputrace.readcounter++;
   }
   cputrace.cyclecounter_pre = cputrace.cyclecounter_post = 0;
}

static bool check_trace (void)
{
   if (!cpu_tracer)
      return true;
   if (!cputrace.readcounter && !cputrace.writecounter && !cputrace.cyclecounter) {
      if (cpu_tracer != -2) {
         write_log (_T("CPU trace: dma_cycle() enabled. %08x %08x NOW=%08x\n"),
            cputrace.cyclecounter_pre, cputrace.cyclecounter_post, get_cycles ());
         cpu_tracer = -2; // dma_cycle() allowed to work now
      }
   }
   if (cputrace.readcounter || cputrace.writecounter ||
      cputrace.cyclecounter || cputrace.cyclecounter_pre || cputrace.cyclecounter_post)
      return false;
   x_get_iword = x2_get_iword;
   x_next_iword = x2_next_iword;
   x_next_ilong = x2_next_ilong;
   x_put_long = x2_put_long;
   x_put_word = x2_put_word;
   x_put_byte = x2_put_byte;
   x_get_long = x2_get_long;
   x_get_word = x2_get_word;
   x_get_byte = x2_get_byte;
   x_do_cycles = x2_do_cycles;
   x_do_cycles_pre = x2_do_cycles_pre;
   x_do_cycles_post = x2_do_cycles_post;
   set_x_cp_funcs();
   write_log(_T("CPU tracer playback complete. STARTCYCLES=%08x NOWCYCLES=%08x\n"), cputrace.startcycles, get_cycles());
   cputrace.needendcycles = 1;
   cpu_tracer = 0;
   return true;
}

static bool get_trace (uaecptr addr, int accessmode, int size, uae_u32 *data)
{
   int mode = accessmode | (size << 4);
   for (int i = 0; i < cputrace.memoryoffset; i++) {
      struct cputracememory *ctm = &cputrace.ctm[i];
      if (ctm->addr == addr && ctm->mode == mode) {
         ctm->mode = 0;
         write_log(_T("CPU trace: GET %d: PC=%08x %08x=%08x %d %d %08x/%08x/%08x %d/%d (%08x)\n"),
            i, cputrace.pc, addr, ctm->data, accessmode, size,
            cputrace.cyclecounter, cputrace.cyclecounter_pre, cputrace.cyclecounter_post,
            cputrace.readcounter, cputrace.writecounter, get_cycles ());
         if (accessmode == 1)
            cputrace.writecounter--;
         else
            cputrace.readcounter--;
         if (cputrace.writecounter == 0 && cputrace.readcounter == 0) {
            if (cputrace.cyclecounter_post) {
               int c = cputrace.cyclecounter_post;
               cputrace.cyclecounter_post = 0;
               x_do_cycles (c);
            } else if (cputrace.cyclecounter_pre) {
               check_trace ();
               *data = ctm->data;
               return true; // argh, need to rerun the memory access..
            }
         }
         check_trace();
         *data = ctm->data;
         return false;
      }
   }
   if (cputrace.cyclecounter_post) {
      int c = cputrace.cyclecounter_post;
      cputrace.cyclecounter_post = 0;
      check_trace();
      x_do_cycles(c);
      return false;
   }
//   gui_message (_T("CPU trace: GET %08x %d %d NOT FOUND!\n"), addr, accessmode, size);
   check_trace();
   *data = 0;
   return false;
}

static uae_u32 cputracefunc_x_next_iword (void)
{
   uae_u32 pc = m68k_getpc ();
   set_trace (pc, 2, 2);
   uae_u32 v = x2_next_iword ();
   add_trace (pc, v, 2, 2);
   return v;
}
static uae_u32 cputracefunc_x_next_ilong (void)
{
   uae_u32 pc = m68k_getpc ();
   set_trace (pc, 2, 4);
   uae_u32 v = x2_next_ilong ();
   add_trace (pc, v, 2, 4);
   return v;
}
static uae_u32 cputracefunc2_x_next_iword (void)
{
   uae_u32 v;
   if (get_trace (m68k_getpc (), 2, 2, &v)) {
      v = x2_next_iword ();
   }
   return v;
}
static uae_u32 cputracefunc2_x_next_ilong (void)
{
   uae_u32 v;
   if (get_trace (m68k_getpc (), 2, 4, &v)) {
      v = x2_next_ilong ();
   }
   return v;
}

static uae_u32 cputracefunc_x_get_iword (int o)
{
   uae_u32 pc = m68k_getpc ();
   set_trace (pc + o, 2, 2);
   uae_u32 v = x2_get_iword (o);
   add_trace (pc + o, v, 2, 2);
   return v;
}
static uae_u32 cputracefunc2_x_get_iword (int o)
{
   uae_u32 v;
   if (get_trace (m68k_getpc () + o, 2, 2, &v)) {
      v = x2_get_iword (o);
   }
   return v;
}

static uae_u32 cputracefunc_x_get_long (uaecptr o)
{
   set_trace (o, 0, 4);
   uae_u32 v = x2_get_long (o);
   add_trace (o, v, 0, 4);
   return v;
}
static uae_u32 cputracefunc_x_get_word (uaecptr o)
{
   set_trace (o, 0, 2);
   uae_u32 v = x2_get_word (o);
   add_trace (o, v, 0, 2);
   return v;
}
static uae_u32 cputracefunc_x_get_byte (uaecptr o)
{
   set_trace (o, 0, 1);
   uae_u32 v = x2_get_byte (o);
   add_trace (o, v, 0, 1);
   return v;
}
static uae_u32 cputracefunc2_x_get_long (uaecptr o)
{
   uae_u32 v;
   if (get_trace (o, 0, 4, &v)) {
      v = x2_get_long (o);
   }
   return v;
}
static uae_u32 cputracefunc2_x_get_word (uaecptr o)
{
   uae_u32 v;
   if (get_trace (o, 0, 2, &v)) {
      v = x2_get_word (o);
   }
   return v;
}
static uae_u32 cputracefunc2_x_get_byte (uaecptr o)
{
   uae_u32 v;
   if (get_trace (o, 0, 1, &v)) {
      v = x2_get_byte (o);
   }
   return v;
}

static void cputracefunc_x_put_long (uaecptr o, uae_u32 val)
{
   clear_trace ();
   add_trace (o, val, 1, 4);
   x2_put_long (o, val);
}
static void cputracefunc_x_put_word (uaecptr o, uae_u32 val)
{
   clear_trace ();
   add_trace (o, val, 1, 2);
   x2_put_word (o, val);
}
static void cputracefunc_x_put_byte (uaecptr o, uae_u32 val)
{
   clear_trace ();
   add_trace (o, val, 1, 1);
   x2_put_byte (o, val);
}
static void cputracefunc2_x_put_long (uaecptr o, uae_u32 val)
{
   uae_u32 v;
   if (get_trace (o, 1, 4, &v)) {
      x2_put_long (o, val);
   }
   if (v != val)
      write_log (_T("cputracefunc2_x_put_long %d <> %d\n"), v, val);
}
static void cputracefunc2_x_put_word (uaecptr o, uae_u32 val)
{
   uae_u32 v;
   if (get_trace (o, 1, 2, &v)) {
      x2_put_word (o, val);
   }
   if (v != val)
      write_log (_T("cputracefunc2_x_put_word %d <> %d\n"), v, val);
}
static void cputracefunc2_x_put_byte (uaecptr o, uae_u32 val)
{
   uae_u32 v;
   if (get_trace (o, 1, 1, &v)) {
      x2_put_byte (o, val);
   }
   if (v != val)
      write_log (_T("cputracefunc2_x_put_byte %d <> %d\n"), v, val);
}

static void cputracefunc_x_do_cycles (int cycles)
{
   while (cycles >= CYCLE_UNIT) {
      cputrace.cyclecounter += CYCLE_UNIT;
      cycles -= CYCLE_UNIT;
      x2_do_cycles (CYCLE_UNIT);
   }
   if (cycles > 0) {
      cputrace.cyclecounter += cycles;
      x2_do_cycles (cycles);
   }
}

static void cputracefunc2_x_do_cycles (int cycles)
{
   if (cputrace.cyclecounter > cycles) {
      cputrace.cyclecounter -= cycles;
      return;
   }
   cycles -= cputrace.cyclecounter;
   cputrace.cyclecounter = 0;
   check_trace ();
   x_do_cycles = x2_do_cycles;
   if (cycles > 0)
      x_do_cycles (cycles);
}

static void cputracefunc_x_do_cycles_pre (int cycles)
{
   cputrace.cyclecounter_post = 0;
   cputrace.cyclecounter_pre = 0;
   while (cycles >= CYCLE_UNIT) {
      cycles -= CYCLE_UNIT;
      cputrace.cyclecounter_pre += CYCLE_UNIT;
      x2_do_cycles (CYCLE_UNIT);
   }
   if (cycles > 0) {
      x2_do_cycles (cycles);
      cputrace.cyclecounter_pre += cycles;
   }
   cputrace.cyclecounter_pre = 0;
}
// cyclecounter_pre = how many cycles we need to SWALLOW
// -1 = rerun whole access
static void cputracefunc2_x_do_cycles_pre (int cycles)
{
   if (cputrace.cyclecounter_pre == -1) {
      cputrace.cyclecounter_pre = 0;
      check_trace ();
      x_do_cycles (cycles);
      return;
   }
   if (cputrace.cyclecounter_pre > cycles) {
      cputrace.cyclecounter_pre -= cycles;
      return;
   }
   cycles -= cputrace.cyclecounter_pre;
   cputrace.cyclecounter_pre = 0;
   check_trace ();
   if (cycles > 0)
      x_do_cycles (cycles);
}

static void cputracefunc_x_do_cycles_post (int cycles, uae_u32 v)
{
   if (cputrace.memoryoffset < 1) {
      return;
   }
   struct cputracememory *ctm = &cputrace.ctm[cputrace.memoryoffset - 1];
   ctm->data = v;
   cputrace.cyclecounter_post = cycles;
   cputrace.cyclecounter_pre = 0;
   while (cycles >= CYCLE_UNIT) {
      cycles -= CYCLE_UNIT;
      cputrace.cyclecounter_post -= CYCLE_UNIT;
      x2_do_cycles (CYCLE_UNIT);
   }
   if (cycles > 0) {
      cputrace.cyclecounter_post -= cycles;
      x2_do_cycles (cycles);
   }
   cputrace.cyclecounter_post = 0;
}
// cyclecounter_post = how many cycles we need to WAIT
static void cputracefunc2_x_do_cycles_post (int cycles, uae_u32 v)
{
   int c;
   if (cputrace.cyclecounter_post) {
      c = cputrace.cyclecounter_post;
      cputrace.cyclecounter_post = 0;
   } else {
      c = cycles;
   }
   check_trace ();
   if (c > 0)
      x_do_cycles (c);
}

static void do_cycles_post (int cycles, uae_u32 v)
{
   do_cycles (cycles);
}
static void do_cycles_ce_post (int cycles, uae_u32 v)
{
   do_cycles_ce (cycles);
}

static void set_x_ifetches(void)
{
   if (m68k_pc_indirect) {
      // indirect via addrbank
      x_get_iword = get_iiword;
      x_next_iword = next_iiword;
      x_next_ilong = next_iilong;
   } else {
      // direct to memory
      x_get_iword = get_diword;
      x_next_iword = next_diword;
      x_next_ilong = next_dilong;
   }
}

// indirect memory access functions
static void set_x_funcs (void)
{
   if (currprefs.m68k_speed < 0 || currprefs.cachesize > 0)
      do_cycles = do_cycles_cpu_fastest;
   else
      do_cycles = do_cycles_cpu_norm;

   if (currprefs.cpu_model < 68020) {
      // 68000/010
      if (currprefs.cpu_cycle_exact) {
         x_get_iword = get_wordi_ce000;
         x_next_iword = NULL;
         x_next_ilong = NULL;
         x_put_long = put_long_ce000;
         x_put_word = put_word_ce000;
         x_put_byte = put_byte_ce000;
         x_get_long = get_long_ce000;
         x_get_word = get_word_ce000;
         x_get_byte = get_byte_ce000;
         x_do_cycles = do_cycles_ce;
         x_do_cycles_pre = do_cycles_ce;
         x_do_cycles_post = do_cycles_ce_post;
      } else if (currprefs.cpu_compatible) {
         // cpu_compatible only
         x_get_iword = get_iiword;
         x_next_iword = NULL;
         x_next_ilong = NULL;
         x_put_long = put_long_compatible;
         x_put_word = put_word_compatible;
         x_put_byte = put_byte_compatible;
         x_get_long = get_long_compatible;
         x_get_word = get_word_compatible;
         x_get_byte = get_byte_compatible;
         x_do_cycles = do_cycles;
         x_do_cycles_pre = do_cycles;
         x_do_cycles_post = do_cycles_post;
      } else {
         x_get_iword = get_diword;
         x_next_iword = next_diword;
         x_next_ilong = next_dilong;
         x_put_long = put_long;
         x_put_word = put_word;
         x_put_byte = put_byte;
         x_get_long = get_long;
         x_get_word = get_word;
         x_get_byte = get_byte;
         x_do_cycles = do_cycles;
         x_do_cycles_pre = do_cycles;
         x_do_cycles_post = do_cycles_post;
      }
   } else if (currprefs.mmu_model == 68030) {
      // UAE_030_MMU: route every CPU memory access through the 68030 MMU
      // translating accessors (cpummu030.h inlines). The generated cpuemu_32
      // table (op_smalltbl_32_ff) reaches memory via the *_mmu030_state inlines
      // -> uae_mmu030_*, NOT via the read_data_030_* pointers. We still repoint
      // read_data_030_*/write_data_030_* from their physical defaults to the
      // translating uae_mmu030_* accessors for completeness (consumed only by the
      // inactive mmu030c cache family). (Verbatim shape from WinUAE 4.4.0.)
      x_prefetch = get_iword_mmu030;     // opcode fetch in m68k_run_mmu030
      x_get_iword = get_iword_mmu030;
      x_next_iword = next_iword_mmu030;
      x_next_ilong = next_ilong_mmu030;
      x_put_long = put_long_mmu030;
      x_put_word = put_word_mmu030;
      x_put_byte = put_byte_mmu030;
      x_get_long = get_long_mmu030;
      x_get_word = get_word_mmu030;
      x_get_byte = get_byte_mmu030;
      x_do_cycles = do_cycles;
      x_do_cycles_pre = do_cycles;
      x_do_cycles_post = do_cycles_post;
      read_data_030_bget = uae_mmu030_get_byte;
      read_data_030_wget = uae_mmu030_get_word;
      read_data_030_lget = uae_mmu030_get_long;
      write_data_030_bput = uae_mmu030_put_byte;
      write_data_030_wput = uae_mmu030_put_word;
      write_data_030_lput = uae_mmu030_put_long;
   } else {
      // 68020+ no ce
      set_x_ifetches();
      if (currprefs.cachesize) {
         x_put_long = put_long_jit;
         x_put_word = put_word_jit;
         x_put_byte = put_byte_jit;
         x_get_long = get_long_jit;
         x_get_word = get_word_jit;
         x_get_byte = get_byte_jit;
      } else {
         x_put_long = put_long;
         x_put_word = put_word;
         x_put_byte = put_byte;
         x_get_long = get_long;
         x_get_word = get_word;
         x_get_byte = get_byte;
      }
      x_do_cycles = do_cycles;
      x_do_cycles_pre = do_cycles;
      x_do_cycles_post = do_cycles_post;
   }
   x2_get_iword = x_get_iword;
   x2_next_iword = x_next_iword;
   x2_next_ilong = x_next_ilong;
   x2_put_long = x_put_long;
   x2_put_word = x_put_word;
   x2_put_byte = x_put_byte;
   x2_get_long = x_get_long;
   x2_get_word = x_get_word;
   x2_get_byte = x_get_byte;
   x2_do_cycles = x_do_cycles;
   x2_do_cycles_pre = x_do_cycles_pre;
   x2_do_cycles_post = x_do_cycles_post;

   if (cpu_tracer > 0) {
      x_get_iword = cputracefunc_x_get_iword;
      x_next_iword = cputracefunc_x_next_iword;
      x_next_ilong = cputracefunc_x_next_ilong;
      x_put_long = cputracefunc_x_put_long;
      x_put_word = cputracefunc_x_put_word;
      x_put_byte = cputracefunc_x_put_byte;
      x_get_long = cputracefunc_x_get_long;
      x_get_word = cputracefunc_x_get_word;
      x_get_byte = cputracefunc_x_get_byte;
      x_do_cycles = cputracefunc_x_do_cycles;
      x_do_cycles_pre = cputracefunc_x_do_cycles_pre;
      x_do_cycles_post = cputracefunc_x_do_cycles_post;
   } else if (cpu_tracer < 0) {
      if (!check_trace ()) {
         x_get_iword = cputracefunc2_x_get_iword;
         x_next_iword = cputracefunc2_x_next_iword;
         x_next_ilong = cputracefunc2_x_next_ilong;
         x_put_long = cputracefunc2_x_put_long;
         x_put_word = cputracefunc2_x_put_word;
         x_put_byte = cputracefunc2_x_put_byte;
         x_get_long = cputracefunc2_x_get_long;
         x_get_word = cputracefunc2_x_get_word;
         x_get_byte = cputracefunc2_x_get_byte;
         x_do_cycles = cputracefunc2_x_do_cycles;
         x_do_cycles_pre = cputracefunc2_x_do_cycles_pre;
         x_do_cycles_post = cputracefunc2_x_do_cycles_post;
      }
   }

   set_x_cp_funcs();
}

#ifdef HOST_TEST_HARNESS
/* Exposes the static set_x_funcs() to the Layer-1 host MMU test harness so it can
 * wire the x_* memory accessors after build_cpufunctbl(). Never compiled into the
 * firmware (HOST_TEST_HARNESS is only defined by test/host/Makefile). */
extern "C" void harness_set_x_funcs(void) { set_x_funcs(); }
#endif

bool can_cpu_tracer (void)
{
   return currprefs.cpu_model == 68000 && currprefs.cpu_memory_cycle_exact;
}

bool is_cpu_tracer (void)
{
   return cpu_tracer > 0;
}
bool set_cpu_tracer (bool state)
{
   if (cpu_tracer < 0)
      return false;
   int old = cpu_tracer;
//   if (input_record)
//      state = true;
   cpu_tracer = 0;
   if (state && can_cpu_tracer ()) {
      cpu_tracer = 1;
      set_x_funcs ();
      if (old != cpu_tracer)
         write_log (_T("CPU tracer enabled\n"));
   }
   if (old > 0 && state == false) {
      set_x_funcs ();
      write_log (_T("CPU tracer disabled\n"));
   }
   return is_cpu_tracer ();
}

static void flush_cpu_caches(bool force)
{
   bool doflush = currprefs.cpu_compatible || currprefs.cpu_memory_cycle_exact;

   if (currprefs.cpu_model == 68020) {
      if ((regs.cacr & 0x08) || force) { // clear instr cache
         regs.cacr &= ~0x08;
      }
      if (regs.cacr & 0x04) { // clear entry in instr cache
         regs.cacr &= ~0x04;
      }
   } else if (currprefs.cpu_model == 68030) {
      if ((regs.cacr & 0x08) || force) { // clear instr cache
         regs.cacr &= ~0x08;
      }
      if (regs.cacr & 0x04) { // clear entry in instr cache
         regs.cacr &= ~0x04;
      }
      if ((regs.cacr & 0x800) || force) { // clear data cache
         regs.cacr &= ~0x800;
      }
      if (regs.cacr & 0x400) { // clear entry in data cache
         regs.cacr &= ~0x400;
      }
   }
//   Xil_L1DCacheFlush();
}

void flush_cpu_caches_040(uae_u16 opcode)
{
   // 0 (1) = data, 1 (2) = instruction
   int cache = (opcode >> 6) & 3;
   int scope = (opcode >> 3) & 3;

   for (int k = 0; k < 2; k++) {
      if (cache & (1 << k)) {
         if (scope == 3) {
            // all
            if (k) {
               // instruction
               flush_cpu_caches(true);
            }
         }
      }
   }
}

void set_cpu_caches (bool flush)
{
#ifdef JIT
   if (currprefs.cachesize) {
      if (currprefs.cpu_model < 68040) {
         set_cache_state (regs.cacr & 1);
         if (regs.cacr & 0x08) {
            flush_icache (3);
         }
      } else {
         set_cache_state ((regs.cacr & 0x8000) ? 1 : 0);
      }
   }
#endif
   flush_cpu_caches(flush);
}

STATIC_INLINE void count_instr (uae_u32 opcode)
{
}

//static uae_u32 opcode_swap(uae_u16 opcode)
//{
//   if (!need_opcode_swap)
//      return opcode;
//   return bswap_16(opcode);
//}

uae_u32 REGPARAM2 op_illg_1 (uae_u32 opcode)
{
//   opcode = opcode_swap(opcode);
   op_illg(opcode);
   return 4;
}

// generic+direct, generic+direct+jit, more compatible, cycle-exact
// UAE_030_MMU: the 68030 MMU instruction table (op_smalltbl_32_ff), generated by
// WinUAE 4.4.0 gencpu (id 32) and living in cpuemu_32.cpp / cpustbl_mmu030.cpp.
extern const struct cputbl op_smalltbl_32_ff[];

// Column 4 (mode 4) is the MMU table; selected when currprefs.mmu_model is set.
static const struct cputbl *cputbls[5][5] =
{
   // 68000  { direct,         jit,           compatible,    cycle-exact,   MMU }
   { op_smalltbl_5, op_smalltbl_45, op_smalltbl_12, op_smalltbl_14, NULL },
   // 68010
   { op_smalltbl_4, op_smalltbl_44, op_smalltbl_11, op_smalltbl_13, NULL },
   // 68020
   { op_smalltbl_3, op_smalltbl_43, NULL, NULL, NULL },
   // 68030  (MMU column = op_smalltbl_32_ff)
   { op_smalltbl_2, op_smalltbl_42, NULL, NULL, op_smalltbl_32_ff },
   // 68040  (68040-MMU table not yet generated; stretch goal)
   { op_smalltbl_1, op_smalltbl_41, NULL, NULL, NULL },
};

void build_cpufunctbl (void)
{
   int i, opcnt;
   uae_u32 opcode;
   const struct cputbl *tbl = NULL;
   int lvl, mode;

   if (!currprefs.cachesize) {
      if (currprefs.mmu_model == 68030) {
         mode = 4;   // UAE_030_MMU: op_smalltbl_32_ff (indirect, fault-restartable)
                     // == 68030 (not just truthy): only the 030 row of cputbls has a
                     // mode-4 table; a non-030 mmu_model must fall through, not index NULL.
      } else if (currprefs.cpu_cycle_exact) {
         mode = 3;
      } else if (currprefs.cpu_compatible && currprefs.cpu_model < 68020) {
         mode = 2;
      } else {
         mode = 0;
      }
      m68k_pc_indirect = mode != 0 ? 1 : 0;
   } else {
      mode = 1;
      m68k_pc_indirect = 0;
   }
   printf("[Core1] Emulation mode %d m68k_pc_indirect %d\n",mode,m68k_pc_indirect);
   lvl = (currprefs.cpu_model - 68000) / 10;
   if (lvl >= 5)
      lvl = 4;
   tbl = cputbls[lvl][mode];

   if (tbl == NULL) {
      write_log (_T("no CPU emulation cores available CPU=%d!"), currprefs.cpu_model);
      abort ();
   }

   for (opcode = 0; opcode < 65536; opcode++)
      cpufunctbl[opcode] = op_illg_1;
   for (i = 0; tbl[i].handler_ff != NULL; i++) {
      opcode = tbl[i].opcode;
      cpufunctbl[opcode] = tbl[i].handler_ff;
   }

   /* hack fpu to 68000/68010 mode */
   if (currprefs.fpu_model && currprefs.cpu_model < 68020) {
      tbl = op_smalltbl_3;
      for (i = 0; tbl[i].handler_ff != NULL; i++) {
         if ((tbl[i].opcode & 0xfe00) == 0xf200) {
            cpufunctbl[tbl[i].opcode] = tbl[i].handler_ff;
         }
      }
   }

   opcnt = 0;
   for (opcode = 0; opcode < 65536; opcode++) {
      cpuop_func *f;
      struct instr *table = &table68k[opcode];

      if (table->mnemo == i_ILLG)
         continue;

      /* unimplemented opcode? */
      if (table->unimpclev > 0 && lvl >= table->unimpclev) {
         cpufunctbl[opcode] = op_illg_1;
         continue;
      }

      if (currprefs.fpu_model && currprefs.cpu_model < 68020) {
         /* more hack fpu to 68000/68010 mode */
         if (table->clev > lvl && (opcode & 0xfe00) != 0xf200)
            continue;
      } else if (table->clev > lvl) {
         continue;
      }

      if (table->handler != -1) {
         int idx = table->handler;
         f = cpufunctbl[idx];
         if (f == op_illg_1)
            abort ();
         cpufunctbl[opcode] = f;
         opcnt++;
      }
   }
   write_log (_T("Building CPU, %d opcodes (%d %d %d)\n"),
      opcnt, lvl,
      currprefs.cpu_cycle_exact ? -2 : currprefs.cpu_memory_cycle_exact ? -1 : currprefs.cpu_compatible ? 1 : 0, currprefs.address_space_24);
#ifdef JIT
   write_log(_T("JIT: &countdown =  %p\n"), &countdown);
   write_log(_T("JIT: &build_comp = %p\n"), &build_comp);
   build_comp ();
#endif

   write_log(_T("CPU=%d, FPU=%d%s, JIT%s=%d."),
      currprefs.cpu_model,
      currprefs.fpu_model, currprefs.fpu_model ? _T(" (host)") : _T(""),
      currprefs.cachesize ? (currprefs.compfpu ? _T("=CPU/FPU") : _T("=CPU")) : _T(""),
      currprefs.cachesize);

   regs.address_space_mask = 0xffffffff;
   if (currprefs.cpu_compatible) {
      if (currprefs.address_space_24 && currprefs.cpu_model >= 68040)
         currprefs.address_space_24 = false;
   }
   m68k_interrupt_delay = false;
   if (currprefs.cpu_cycle_exact) {
      if (tbl == op_smalltbl_14 || tbl == op_smalltbl_13)
         m68k_interrupt_delay = true;
   } else if (currprefs.cpu_compatible) {
      if (currprefs.cpu_model <= 68010 && currprefs.m68k_speed == 0) {
         m68k_interrupt_delay = true;
      }
   }

   if (currprefs.cpu_cycle_exact) {
      if (currprefs.cpu_model == 68000)
         write_log(_T(" prefetch and cycle-exact"));
      else
         write_log(_T(" ~cycle-exact"));
   } else if (currprefs.cpu_memory_cycle_exact) {
         write_log(_T(" ~memory-cycle-exact"));
   } else if (currprefs.cpu_compatible) {
      if (currprefs.cpu_model <= 68020) {
         write_log(_T(" prefetch"));
      } else {
         write_log(_T(" fake prefetch"));
      }
   }
   if (currprefs.m68k_speed < 0)
      write_log(_T(" fast"));
   if (currprefs.int_no_unimplemented && currprefs.cpu_model == 68060) {
      write_log(_T(" no unimplemented integer instructions"));
   }
   if (currprefs.fpu_no_unimplemented && currprefs.fpu_model) {
      write_log(_T(" no unimplemented floating point instructions"));
   }
   if (currprefs.address_space_24) {
      regs.address_space_mask = 0x00ffffff;
      write_log(_T(" 24-bit"));
   }
   write_log(_T("\n"));

   set_cpu_caches (true);
//   target_cpu_speed();
}

static uae_u32 cycles_shift;
static uae_u32 cycles_shift_2;

static void update_68k_cycles (void)
{
   cycles_shift = 0;
   cycles_shift_2 = 0;
   if (currprefs.m68k_speed >= 0) {
      if (currprefs.m68k_speed == M68K_SPEED_14MHZ_CYCLES)
         cycles_shift = 1;
      else if (currprefs.m68k_speed == M68K_SPEED_25MHZ_CYCLES) {
         if (currprefs.cpu_model >= 68040) {
            cycles_shift = 4;
         } else {
            cycles_shift = 2;
            cycles_shift_2 = 5;
         }
      }
   }

   currprefs.cpu_clock_multiplier = changed_prefs.cpu_clock_multiplier;
   currprefs.cpu_frequency = changed_prefs.cpu_frequency;

//   baseclock = (currprefs.ntscmode ? CHIPSET_CLOCK_NTSC : CHIPSET_CLOCK_PAL) * 8;
   cpucycleunit = CYCLE_UNIT / 2;
   if (currprefs.m68k_speed < 0 || currprefs.cachesize > 0)
      do_cycles = do_cycles_cpu_fastest;
   else
      do_cycles = do_cycles_cpu_norm;

   if (cpucycleunit < 1)
      cpucycleunit = 1;

   write_log (_T("CPU cycleunit: %d (%.3f)\n"), cpucycleunit, (float)cpucycleunit / CYCLE_UNIT);
//   set_config_changed ();
}

static void prefs_changed_cpu (void)
{
//   fixup_cpu (&changed_prefs);
   check_prefs_changed_comp(false);
   currprefs.cpu_model = changed_prefs.cpu_model;
   currprefs.fpu_model = changed_prefs.fpu_model;
   if (currprefs.cpu_compatible != changed_prefs.cpu_compatible) {
      currprefs.cpu_compatible = changed_prefs.cpu_compatible;
      flush_cpu_caches(true);
   }
   if (currprefs.cpu_data_cache != changed_prefs.cpu_data_cache) {
      currprefs.cpu_data_cache = changed_prefs.cpu_data_cache;
   }
   currprefs.address_space_24 = changed_prefs.address_space_24;
   currprefs.cpu_cycle_exact = changed_prefs.cpu_cycle_exact;
   currprefs.cpu_memory_cycle_exact = changed_prefs.cpu_memory_cycle_exact;
   currprefs.int_no_unimplemented = changed_prefs.int_no_unimplemented;
   currprefs.fpu_no_unimplemented = changed_prefs.fpu_no_unimplemented;
//   currprefs.blitter_cycle_exact = changed_prefs.blitter_cycle_exact;
//   mman_set_barriers(false);
}

static int check_prefs_changed_cpu2(void)
{
   int changed = 0;

#ifdef JIT
   changed = check_prefs_changed_comp(true) ? 1 : 0;
#endif
   if (changed
      || currprefs.cpu_model != changed_prefs.cpu_model
      || currprefs.fpu_model != changed_prefs.fpu_model
      || currprefs.mmu_model != changed_prefs.mmu_model
      || currprefs.mmu_ec != changed_prefs.mmu_ec
      || currprefs.cpu_data_cache != changed_prefs.cpu_data_cache
      || currprefs.int_no_unimplemented != changed_prefs.int_no_unimplemented
      || currprefs.fpu_no_unimplemented != changed_prefs.fpu_no_unimplemented
      || currprefs.cpu_compatible != changed_prefs.cpu_compatible
      || currprefs.cpu_cycle_exact != changed_prefs.cpu_cycle_exact
      || currprefs.cpu_memory_cycle_exact != changed_prefs.cpu_memory_cycle_exact
      || currprefs.fpu_mode != changed_prefs.fpu_mode) {
         cpu_prefs_changed_flag |= 1;
   }
   if (changed
      || currprefs.m68k_speed != changed_prefs.m68k_speed
      || currprefs.m68k_speed_throttle != changed_prefs.m68k_speed_throttle
      || currprefs.cpu_clock_multiplier != changed_prefs.cpu_clock_multiplier
      || currprefs.reset_delay != changed_prefs.reset_delay
      || currprefs.cpu_frequency != changed_prefs.cpu_frequency) {
         cpu_prefs_changed_flag |= 2;
   }
   return cpu_prefs_changed_flag;
}

void check_prefs_changed_cpu(void)
{
//   if (!config_changed)
//      return;

   currprefs.cpu_idle = changed_prefs.cpu_idle;
//   currprefs.ppc_cpu_idle = changed_prefs.ppc_cpu_idle;
   currprefs.reset_delay = changed_prefs.reset_delay;
//   currprefs.cpuboard_settings = changed_prefs.cpuboard_settings;

   if (check_prefs_changed_cpu2()) {
      set_special(SPCFLAG_MODE_CHANGE);
//      reset_frame_rate_hack();
   }
}

void init_m68k (void)
{
   prefs_changed_cpu ();
   update_68k_cycles ();

   for (int i = 0 ; i < 256 ; i++) {
      int j;
      for (j = 0 ; j < 8 ; j++) {
         if (i & (1 << j)) break;
      }
      movem_index1[i] = j;
      movem_index2[i] = 7 - j;
      movem_next[i] = i & (~(1 << j));
   }

   init_table68k();

   write_log (_T("%d CPU functions\n"), nr_cpuop_funcs);
}

struct regstruct regs;

STATIC_INLINE int in_rom (uaecptr pc)
{
   return (munge24 (pc) & 0xFFF80000) == 0xF80000;
}

STATIC_INLINE int in_rtarea (uaecptr pc)
{
//   return (munge24 (pc) & 0xFFFF0000) == rtarea_base && (uae_boot_rom_type || currprefs.uaeboard > 0);
   return(0);
}

STATIC_INLINE int adjust_cycles (int cycles)
{
   const auto res = cycles >> cycles_shift;
   if (cycles_shift_2)
      return res + (cycles >> cycles_shift_2);
   return res;
}

void m68k_cancel_idle(void)
{
   cpu_last_stop_vpos = -1;
}

static void m68k_set_stop(void)
{
   if (regs.stopped)
      return;
   regs.stopped = 1;
   set_special(SPCFLAG_STOP);
//   if (cpu_last_stop_vpos >= 0) {
//      cpu_last_stop_vpos = vpos;
//   }
}

static void m68k_unset_stop(void)
{
   regs.stopped = 0;
   unset_special(SPCFLAG_STOP);
//   if (cpu_last_stop_vpos >= 0) {
//      cpu_stopped_lines += vpos - cpu_last_stop_vpos;
//      cpu_last_stop_vpos = vpos;
//   }
}

static void activate_trace(void)
{
   unset_special (SPCFLAG_TRACE);
   set_special (SPCFLAG_DOTRACE);
}

// make sure interrupt is checked immediately after current instruction
static void doint_imm(void)
{
   doint();
   if (!currprefs.cachesize && !(regs.spcflags & SPCFLAG_INT) && (regs.spcflags & SPCFLAG_DOINT))
      set_special(SPCFLAG_INT);
}

void REGPARAM2 MakeSR (void)
{
   regs.sr = ((regs.t1 << 15) | (regs.t0 << 14)
      | (regs.s << 13) | (regs.m << 12) | (regs.intmask << 8)
      | (GET_XFLG () << 4) | (GET_NFLG () << 3)
      | (GET_ZFLG () << 2) | (GET_VFLG () << 1)
      |  GET_CFLG ());

}

static void SetSR (uae_u16 sr)
{
   regs.sr &= 0xff00;
   regs.sr |= sr;

   SET_XFLG ((regs.sr >> 4) & 1);
   SET_NFLG ((regs.sr >> 3) & 1);
   SET_ZFLG ((regs.sr >> 2) & 1);
   SET_VFLG ((regs.sr >> 1) & 1);
   SET_CFLG (regs.sr & 1);
}

static void MakeFromSR_x(int t0trace)
{
   int oldm = regs.m;
   int olds = regs.s;
   int oldt0 = regs.t0;
   int oldt1 = regs.t1;

   SET_XFLG ((regs.sr >> 4) & 1);
   SET_NFLG ((regs.sr >> 3) & 1);
   SET_ZFLG ((regs.sr >> 2) & 1);
   SET_VFLG ((regs.sr >> 1) & 1);
   SET_CFLG (regs.sr & 1);
   if (regs.t1 == ((regs.sr >> 15) & 1) &&
      regs.t0 == ((regs.sr >> 14) & 1) &&
      regs.s  == ((regs.sr >> 13) & 1) &&
      regs.m  == ((regs.sr >> 12) & 1) &&
      regs.intmask == ((regs.sr >> 8) & 7))
      return;
   regs.t1 = (regs.sr >> 15) & 1;
   regs.t0 = (regs.sr >> 14) & 1;
   regs.s  = (regs.sr >> 13) & 1;
   regs.m  = (regs.sr >> 12) & 1;
   if(regs.intmask != ((regs.sr >> 8) & 7)) {
      int newimask = (regs.sr >> 8) & 7;
      if(regs.ipl_pin <= regs.intmask && regs.ipl_pin > newimask) {
         if(!currprefs.cachesize) {
            set_special(SPCFLAG_INT);
         } else {
            set_special(SPCFLAG_DOINT);
         }
      }
      regs.intmask = newimask;
   }
   if (currprefs.cpu_model >= 68020) {
      if (olds != regs.s) {
         if (olds) {
            if (oldm)
               regs.msp = m68k_areg (regs, 7);
            else
               regs.isp = m68k_areg (regs, 7);
            m68k_areg (regs, 7) = regs.usp;
         } else {
            regs.usp = m68k_areg (regs, 7);
            m68k_areg (regs, 7) = regs.m ? regs.msp : regs.isp;
         }
      } else if (olds && oldm != regs.m) {
         if (oldm) {
            regs.msp = m68k_areg (regs, 7);
            m68k_areg (regs, 7) = regs.isp;
         } else {
            regs.isp = m68k_areg (regs, 7);
            m68k_areg (regs, 7) = regs.msp;
         }
      }
   } else {
      regs.t0 = regs.m = 0;
      if (olds != regs.s) {
         if (olds) {
            regs.isp = m68k_areg (regs, 7);
            m68k_areg (regs, 7) = regs.usp;
         } else {
            regs.usp = m68k_areg (regs, 7);
            m68k_areg (regs, 7) = regs.isp;
         }
      }
   }

#ifdef JIT
   // if JIT enabled and T1, T0 or M changes: end compile.
   if (currprefs.cachesize && (oldt0 != regs.t0 || oldt1 != regs.t1 || oldm != regs.m)) {
      set_special(SPCFLAG_END_COMPILE);
   }
#endif

   doint_imm();
   if (regs.t1 || regs.t0) {
      set_special (SPCFLAG_TRACE);
   } else {
      /* Keep SPCFLAG_DOTRACE, we still want a trace exception for
      SR-modifying instructions (including STOP).  */
      unset_special (SPCFLAG_TRACE);
   }
   // Stop SR-modification does not generate T0
   // If this SR modification set Tx bit, no trace until next instruction.
   if ((oldt0 && t0trace && currprefs.cpu_model >= 68020) || oldt1) {
      // Always trace if Tx bits were already set, even if this SR modification cleared them.
      activate_trace();
   }
}

void REGPARAM2 MakeFromSR_T0(void)
{
   MakeFromSR_x(1);
}
void REGPARAM2 MakeFromSR(void)
{
   MakeFromSR_x(0);
}

static bool internalexception(int nr)
{
   return nr == 5 || nr == 6 || nr == 7 || (nr >= 32 && nr <= 47);
}

static void exception_check_trace (int nr)
{
   unset_special (SPCFLAG_TRACE | SPCFLAG_DOTRACE);
   if (regs.t1) {
      /* trace stays pending if exception is div by zero, chk,
      * trapv or trap #x. Except if 68040 or 68060.
      */
      if (currprefs.cpu_model < 68040 && internalexception(nr)) {
         set_special(SPCFLAG_DOTRACE);
      }
      // 68010 and RTE format error: trace is not cleared
      if (nr == 14 && currprefs.cpu_model == 68010) {
         set_special(SPCFLAG_DOTRACE);
      }
   }
   regs.t1 = regs.t0 = 0;
}

#ifdef CPUEMU_13

/* cycle-exact exception handler, 68000 only */

/*

68000 Address/Bus Error:

- [memory access causing bus/address error]
- 8 idle cycles (+4 if bus error)
- write PC low word
- write SR
- write PC high word
- write instruction word
- write fault address low word
- write status code
- write fault address high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

68010 Address/Bus Error:

- [memory access causing bus/address error]
- 8 idle cycles (+4 if bus error)
- write word 28
- write word 26
- write word 27
- write word 25
- write word 23
- write word 24
- write word 22
- write word 21
- write word 20
- write word 19
- write word 18
- write word 17
- write word 16
- write word 15
- write word 13
- write word 14
- write instruction buffer
- (skipped)
- write data input buffer
- (skipped)
- write data output buffer
- (skipped)
- write fault address low word
- write fault address high word
- write special status word
- write PC low word
- write SR
- write PC high word
- write frame format
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch


Division by Zero:

- 4 idle cycles (EA + 4 cycles in cpuemu)
- write PC low word
- write SR
- write PC high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

Traps:

- 4 idle cycles
- write PC low word
- write SR
- write PC high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

TrapV:

(- normal prefetch done by TRAPV)
- write PC low word
- write SR
- write PC high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

CHK:

- 4 idle cycles (EA + 4/6 cycles in cpuemu)
- write PC low word
- write SR
- write PC high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

Illegal Instruction:
Privilege violation:
Trace:
Line A:
Line F:

- 4 idle cycles
- write PC low word
- write SR
- write PC high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

Interrupt:

- 6 idle cycles
- write PC low word
- read exception number byte from (0xfffff1 | (interrupt number << 1))
- 4 idle cycles
- write SR
- write PC high word
- read exception address high word
- read exception address low word
- prefetch
- 2 idle cycles
- prefetch

68010:

...
- write SR
- write PC high word
- write frame format
- read exception address high word
...

*/

static int iack_cycle(int nr)
{
   int vector;

   // The autovector IACK is an FC=7 (CPU-space) access on real HW; the 68030 MMU NEVER
   // translates it. iack_cycle runs in Exception_normal BEFORE the supervisor-mode switch,
   // so x_get_byte() here would walk the *current* (user) page tables when an interrupt is
   // taken while a user task runs (e.g. AMIX init) -> bus error at 0x00FFFFFx. Use the bare
   // physical bank accessor (get_byte) so the autovector read bypasses the user MMU. In
   // supervisor/identity context get_byte_mmu030 reaches this same bank, so the value matches.
   vector = get_byte(0x00fffff1 | ((nr - 24) << 1));
   if (currprefs.cpu_compatible)
      x_do_cycles(4 * CYCLE_UNIT / 2);
   return vector;
}

static void Exception_ce000 (int nr)
{
   uae_u32 currpc = m68k_getpc (), newpc;
   int sv = regs.s;
   int start, interrupt;
   int vector_nr = nr;
   int frame_id = 0;

   start = 6;
   interrupt = nr >= 24 && nr < 24 + 8;
   if (!interrupt) {
      start = 4;
      if (nr == 7) { // TRAPV
         start = 0;
      } else if (nr == 3) {
         if (currprefs.cpu_model == 68000)
            start = 8;
         else
            start = 4;
      } else if (nr == 2) {
         if (currprefs.cpu_model == 68000)
            start = 12;
         else
            start = 8;
      }
   }

   if (start)
      x_do_cycles (start * CYCLE_UNIT / 2);

   MakeSR ();

   bool g1 = generates_group1_exception(regs.ir);
   if (!regs.s) {
      regs.usp = m68k_areg (regs, 7);
      m68k_areg (regs, 7) = regs.isp;
      regs.s = 1;
   }
   if (nr == 2 || nr == 3) { /* 2=bus error, 3=address error */
      if ((m68k_areg(regs, 7) & 1) || exception_in_exception < 0) {
         cpu_halt (CPU_HALT_DOUBLE_FAULT);
         return;
      }
      write_log(_T("Exception %d (%08x %x) at %x -> %x!\n"),
         nr, last_op_for_exception_3, last_addr_for_exception_3, currpc, get_long(4 * nr));
      if (currprefs.cpu_model == 68000) {
         // 68000 bus/address error
         uae_u16 mode = (sv ? 4 : 0) | last_fc_for_exception_3;
         mode |= last_writeaccess_for_exception_3 ? 0 : 16;
         mode |= last_notinstruction_for_exception_3 ? 8 : 0;
         // undocumented bits contain opcode
         mode |= last_op_for_exception_3 & ~31;
         m68k_areg(regs, 7) -= 7 * 2;
         exception_in_exception = -1;
         x_put_word(m68k_areg(regs, 7) + 12, last_addr_for_exception_3);
         x_put_word(m68k_areg(regs, 7) + 8, regs.sr);
         x_put_word(m68k_areg(regs, 7) + 10, last_addr_for_exception_3 >> 16);
         x_put_word(m68k_areg(regs, 7) + 6, last_op_for_exception_3);
         x_put_word(m68k_areg(regs, 7) + 4, last_fault_for_exception_3);
         x_put_word(m68k_areg(regs, 7) + 0, mode);
         x_put_word(m68k_areg(regs, 7) + 2, last_fault_for_exception_3 >> 16);
         goto kludge_me_do;
      } else {
         // 68010 bus/address error (partially implemented only)
         uae_u16 in = regs.read_buffer;
         uae_u16 out = regs.write_buffer;
         uae_u16 ssw = (sv ? 4 : 0) | last_fc_for_exception_3;
         ssw |= last_di_for_exception_3 > 0 ? 0x0000 : (last_di_for_exception_3 < 0 ? (0x2000 | 0x1000) : 0x2000);
         ssw |= (!last_writeaccess_for_exception_3 && last_di_for_exception_3) ? 0x1000 : 0x000; // DF
         ssw |= (last_op_for_exception_3 & 0x10000) ? 0x0400 : 0x0000; // HB
         ssw |= last_size_for_exception_3 == 0 ? 0x0200 : 0x0000; // BY
         ssw |= last_writeaccess_for_exception_3 ? 0 : 0x0100; // RW
         if (last_op_for_exception_3 & 0x20000)
            ssw &= 0x00ff;
         m68k_areg(regs, 7) -= (29 - 4) * 2;
         exception_in_exception = -1;
         frame_id = 8;
         for (int i = 0; i < 15; i++) {
            x_put_word(m68k_areg(regs, 7) + 20 + i * 2, ((i + 1) << 8) | ((i + 2) << 0));
         }
         x_put_word(m68k_areg(regs, 7) + 18, 0); // version
         x_put_word(m68k_areg(regs, 7) + 16, regs.irc); // instruction input buffer
         x_put_word(m68k_areg(regs, 7) + 12, in); // data input buffer
         x_put_word(m68k_areg(regs, 7) + 8, out); // data output buffer
         x_put_word(m68k_areg(regs, 7) + 4, last_fault_for_exception_3); // fault addr
         x_put_word(m68k_areg(regs, 7) + 2, last_fault_for_exception_3 >> 16);
         x_put_word(m68k_areg(regs, 7) + 0, ssw); // ssw
      }
   }
   if (currprefs.cpu_model == 68010) {
      // 68010 creates only format 0 and 8 stack frames
      m68k_areg (regs, 7) -= 4 * 2;
      if (m68k_areg(regs, 7) & 1) {
         exception3_notinstruction(regs.ir, m68k_areg(regs, 7) + 4);
         return;
      }
      exception_in_exception = 1;
      x_put_word (m68k_areg (regs, 7) + 4, currpc); // write low address
      if (interrupt)
         vector_nr = iack_cycle(nr);
      x_put_word (m68k_areg (regs, 7) + 0, regs.sr); // write SR
      x_put_word (m68k_areg (regs, 7) + 2, currpc >> 16); // write high address
      x_put_word (m68k_areg (regs, 7) + 6, (frame_id << 12) | (vector_nr * 4));
   } else {
      m68k_areg (regs, 7) -= 3 * 2;
      if (m68k_areg(regs, 7) & 1) {
         exception3_notinstruction(regs.ir, m68k_areg(regs, 7) + 4);
         return;
      }
      exception_in_exception = 1;
      x_put_word (m68k_areg (regs, 7) + 4, currpc); // write low address
      if (interrupt)
         vector_nr = iack_cycle(nr);
      x_put_word (m68k_areg (regs, 7) + 0, regs.sr); // write SR
      x_put_word (m68k_areg (regs, 7) + 2, currpc >> 16); // write high address
   }
kludge_me_do:
   if ((regs.vbr & 1) && currprefs.cpu_model <= 68010) {
      cpu_halt(CPU_HALT_DOUBLE_FAULT);
      return;
   }
   if (interrupt)
      regs.intmask = nr - 24;
   newpc = x_get_word (regs.vbr + 4 * vector_nr) << 16; // read high address
   newpc |= x_get_word (regs.vbr + 4 * vector_nr + 2); // read low address
   exception_in_exception = 0;
   if (newpc & 1) {
      if (nr == 2 || nr == 3) {
         cpu_halt(CPU_HALT_DOUBLE_FAULT);
         return;
      }
      if (currprefs.cpu_model == 68000) {
         // if exception vector is odd:
         // opcode is last opcode executed, address is address of exception vector
         // pc is last prefetch address
         regs.t1 = 0;
         MakeSR();
         m68k_setpc(regs.vbr + 4 * vector_nr);
         if (interrupt) {
            regs.ir = nr;
            exception3_read_access(regs.ir | 0x20000 | 0x10000, newpc, sz_word, 2);
         } else {
            exception3_read_access(regs.ir | 0x40000 | 0x20000 | (g1 ? 0x10000 : 0), newpc, sz_word, 2);
         }
      } else if (currprefs.cpu_model == 68010) {
         // offset, not vbr + offset
         regs.t1 = 0;
         MakeSR();
         regs.write_buffer = 4 * vector_nr;
         regs.read_buffer = newpc;
         regs.irc = regs.read_buffer;
         exception3_read_access(regs.opcode, newpc, sz_word, 2);
      } else {
         exception_check_trace(nr);
         exception3_notinstruction(regs.ir, newpc);
      }
      return;
   }
   m68k_setpc (newpc);
   regs.ir = x_get_word (m68k_getpc ()); // prefetch 1
   if (hardware_bus_error) {
      if (nr == 2 || nr == 3) {
         cpu_halt(CPU_HALT_DOUBLE_FAULT);
         return;
      }
      exception2_fetch(regs.irc, 0, 0);
      return;
   }
   x_do_cycles (2 * CYCLE_UNIT / 2);
   regs.ipl_pin = intlev();
   ipl_fetch();
   regs.irc = x_get_word (m68k_getpc () + 2); // prefetch 2
   if (hardware_bus_error) {
      if (nr == 2 || nr == 3) {
         cpu_halt(CPU_HALT_DOUBLE_FAULT);
         return;
      }
      exception2_fetch(regs.ir, 0, 2);
      return;
   }
#ifdef JIT
   if (currprefs.cachesize) {
      set_special(SPCFLAG_END_COMPILE);
   }
#endif
   exception_check_trace (nr);
}
#endif

static void add_approximate_exception_cycles(int nr)
{
   int cycles;

   if (currprefs.cpu_model == 68000) {
      // 68000 exceptions
      if (nr >= 24 && nr <= 31) {
         /* Interrupts */
         cycles = 44 * CYCLE_UNIT / 2;
      } else if (nr >= 32 && nr <= 47) {
         /* Trap (total is 34, but cpuemu_x.cpp already adds 4) */
         cycles = (34 - 4) * CYCLE_UNIT / 2;
      } else {
         switch (nr)
         {
         case 2: cycles = 50 * CYCLE_UNIT / 2; break;      /* Bus error */
         case 3: cycles = 50 * CYCLE_UNIT / 2; break;      /* Address error */
         case 4: cycles = 34 * CYCLE_UNIT / 2; break;      /* Illegal instruction */
         case 5: cycles = 38 * CYCLE_UNIT / 2; break;      /* Division by zero */
         case 6: cycles = 40 * CYCLE_UNIT / 2; break;      /* CHK */
         case 7: cycles = (34 - 8) * CYCLE_UNIT / 2; break;      /* TRAPV */
         case 8: cycles = 34 * CYCLE_UNIT / 2; break;      /* Privilege violation */
         case 9: cycles = 34 * CYCLE_UNIT / 2; break;      /* Trace */
         case 10: cycles = 34 * CYCLE_UNIT / 2; break;   /* Line-A */
         case 11: cycles = 34 * CYCLE_UNIT / 2; break;   /* Line-F */
         default:
            cycles = 4 * CYCLE_UNIT / 2;
            break;
         }
      }
   } else if (currprefs.cpu_model == 68010) {
      // 68010 exceptions
      if (nr >= 24 && nr <= 31) {
         /* Interrupts */
         cycles = 46 * CYCLE_UNIT / 2;
      } else if (nr >= 32 && nr <= 47) {
         /* Trap */
         cycles = (38 - 4) * CYCLE_UNIT / 2;
      } else {
         switch (nr)
         {
         case 2: cycles = 126 * CYCLE_UNIT / 2; break;   /* Bus error */
         case 3: cycles = 126 * CYCLE_UNIT / 2; break;   /* Address error */
         case 4: cycles = 38 * CYCLE_UNIT / 2; break;      /* Illegal instruction */
         case 5: cycles = 42 * CYCLE_UNIT / 2; break;      /* Division by zero */
         case 6: cycles = 40 * CYCLE_UNIT / 2; break;      /* CHK */
         case 7: cycles = (38 - 8) * CYCLE_UNIT / 2; break;      /* TRAPV */
         case 8: cycles = 38 * CYCLE_UNIT / 2; break;      /* Privilege violation */
         case 9: cycles = 38 * CYCLE_UNIT / 2; break;      /* Trace */
         case 10: cycles = 38 * CYCLE_UNIT / 2; break;   /* Line-A */
         case 11: cycles = 38 * CYCLE_UNIT / 2; break;   /* Line-F */
         case 14: cycles = 50 * CYCLE_UNIT / 2; break;   /* RTE frame error */
         default:
            cycles = 4 * CYCLE_UNIT / 2;
            break;
         }
      }
   } else {
      // 68020 exceptions
      if (nr >= 24 && nr <= 31) {
         /* Interrupts */
         cycles = 26 * CYCLE_UNIT / 2;
      } else if (nr >= 32 && nr <= 47) {
         /* Trap */
         cycles = 20 * CYCLE_UNIT / 2;
      } else {
         switch (nr)
         {
         case 2: cycles = 43 * CYCLE_UNIT / 2; break;      /* Bus error */
         case 3: cycles = 43 * CYCLE_UNIT / 2; break;      /* Address error ??? */
         case 4: cycles = 20 * CYCLE_UNIT / 2; break;      /* Illegal instruction */
         case 5: cycles = 32 * CYCLE_UNIT / 2; break;      /* Division by zero */
         case 6: cycles = 32 * CYCLE_UNIT / 2; break;      /* CHK */
         case 7: cycles = 25 * CYCLE_UNIT / 2; break;      /* TRAPV */
         case 8: cycles = 20 * CYCLE_UNIT / 2; break;      /* Privilege violation */
         case 9: cycles = 25 * CYCLE_UNIT / 2; break;      /* Trace */
         case 10: cycles = 20 * CYCLE_UNIT / 2; break;   /* Line-A */
         case 11: cycles = 20 * CYCLE_UNIT / 2; break;   /* Line-F */
         case 14: cycles = 21 * CYCLE_UNIT / 2; break;   /* RTE frame error */
         default:
            cycles = 4 * CYCLE_UNIT / 2;
            break;
         }
      }
   }
   cycles = adjust_cycles(cycles);
   x_do_cycles(cycles);
}

static void Exception_normal (int nr)
{
   uae_u32 newpc;
   uae_u32 currpc = m68k_getpc();
   uae_u32 nextpc;
   int sv = regs.s;
   int interrupt;
   int vector_nr = nr;
   bool g1 = false;

   interrupt = nr >= 24 && nr < 24 + 8;
   if (interrupt)
      vector_nr = iack_cycle(nr);

   if (currprefs.cpu_model <= 68010) {
      g1 = generates_group1_exception(regs.ir);
   }

   MakeSR ();

   if (!regs.s) {
      regs.usp = m68k_areg (regs, 7);
      if (currprefs.cpu_model >= 68020 && currprefs.cpu_model < 68060) {
         m68k_areg (regs, 7) = regs.m ? regs.msp : regs.isp;
      } else {
         m68k_areg (regs, 7) = regs.isp;
      }
      regs.s = 1;
   }

   if ((m68k_areg(regs, 7) & 1) && currprefs.cpu_model < 68020) {
      if (nr == 2 || nr == 3)
         cpu_halt (CPU_HALT_DOUBLE_FAULT);
      else
         exception3_notinstruction(regs.ir, m68k_areg(regs, 7));
      return;
   }
   if ((nr == 2 || nr == 3) && exception_in_exception < 0) {
      cpu_halt (CPU_HALT_DOUBLE_FAULT);
      return;
   }

   if (!currprefs.cpu_compatible) {
      addrbank *ab = &get_mem_bank(m68k_areg(regs, 7) - 4);
      // Not plain RAM check because some CPU type tests that
      // don't need to return set stack to ROM..
      if (!ab || ab == &dmmy_bank || (ab->flags & ABFLAG_IO)) {
         cpu_halt(CPU_HALT_SSP_IN_NON_EXISTING_ADDRESS);
         return;
      }
   }

   bool used_exception_build_stack_frame = false;

   if (currprefs.cpu_model > 68000) {
      uae_u32 oldpc = regs.instruction_pc;
      nextpc = exception_pc (nr);
      if (nr == 2 || nr == 3) {
         int i;
         if (currprefs.mmu_model && nr == 2) {
            // UAE_030_MMU: a real 68030 data/instruction bus fault from the page-fault
            // engine. mmu030_page_fault() already computed regs.mmu_ssw (with DF set and
            // the real RW/SIZE/FC) and regs.mmu_fault_addr; stack them VERBATIM so that
            // m68k_do_rte_mmu030() re-performs the faulted access on RTE (it is gated on
            // ssw & MMU030_SSW_DF). Frame A (short) if the instruction's LAST write
            // faulted, else frame B (long). Restores the WinUAE 4.4.0 mmu_model branch the
            // fork dropped: without it the nr==2 path below synthesizes an SSW with no DF
            // and a stale fault address, so demand paging never resumes (AMIX won't boot).
            int frameformat = (mmu030_state[1] & MMU030_STATEFLAG1_LASTWRITE) ? 0xa : 0xb;
            Exception_build_stack_frame(regs.instruction_pc, currpc, regs.mmu_ssw, nr, frameformat);
            used_exception_build_stack_frame = true;
         } else if (currprefs.cpu_model >= 68040) {
            if (nr == 2) {

                  // 68040 bus error (not really, some garbage?)
                  for (i = 0 ; i < 18 ; i++) {
                     m68k_areg (regs, 7) -= 2;
                     x_put_word (m68k_areg (regs, 7), 0);
                  }
                  m68k_areg (regs, 7) -= 4;
                  x_put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
                  m68k_areg (regs, 7) -= 2;
                  x_put_word (m68k_areg (regs, 7), 0);
                  m68k_areg (regs, 7) -= 2;
                  x_put_word (m68k_areg (regs, 7), 0);
                  m68k_areg (regs, 7) -= 2;
                  x_put_word (m68k_areg (regs, 7), 0);
                  m68k_areg (regs, 7) -= 2;
                  x_put_word (m68k_areg (regs, 7), 0x0140 | (sv ? 6 : 2)); /* SSW */
                  m68k_areg (regs, 7) -= 4;
                  x_put_long (m68k_areg (regs, 7), last_addr_for_exception_3);
                  m68k_areg (regs, 7) -= 2;
                  x_put_word (m68k_areg (regs, 7), 0x7000 + vector_nr * 4);
                  m68k_areg (regs, 7) -= 4;
                  x_put_long (m68k_areg (regs, 7), regs.instruction_pc);
                  m68k_areg (regs, 7) -= 2;
                  x_put_word (m68k_areg (regs, 7), regs.sr);
                  goto kludge_me_do;

            } else {
               // 68040/060 odd PC address error
               Exception_build_stack_frame(last_fault_for_exception_3, currpc, 0, nr, 0x02);
               used_exception_build_stack_frame = true;
            }
         } else if (currprefs.cpu_model >= 68020) {
            // 68020/030 odd PC address error (partially implemented only)
            // annoyingly this generates frame B, not A.
            uae_u16 ssw = (sv ? 4 : 0) | last_fc_for_exception_3;
            ssw |= MMU030_SSW_RW | MMU030_SSW_SIZE_W;
            regs.mmu_fault_addr = last_fault_for_exception_3;
            Exception_build_stack_frame(last_fault_for_exception_3, currpc, ssw, nr, 0x0b);
            used_exception_build_stack_frame = true;
         } else {
            // 68010 bus/address error (partially implemented only)
            uae_u16 ssw = (sv ? 4 : 0) | last_fc_for_exception_3;
            ssw |= last_di_for_exception_3 > 0 ? 0x0000 : (last_di_for_exception_3 < 0 ? (0x2000 | 0x1000) : 0x2000);
            ssw |= (!last_writeaccess_for_exception_3 && last_di_for_exception_3) ? 0x1000 : 0x000; // DF
            ssw |= (last_op_for_exception_3 & 0x10000) ? 0x0400 : 0x0000; // HB
            ssw |= last_size_for_exception_3 == 0 ? 0x0200 : 0x0000; // BY
            ssw |= last_writeaccess_for_exception_3 ? 0x0000 : 0x0100; // RW
            if (last_op_for_exception_3 & 0x20000)
               ssw &= 0x00ff;
            regs.mmu_fault_addr = last_fault_for_exception_3;
            Exception_build_stack_frame(oldpc, currpc, ssw, nr, 0x08);
            used_exception_build_stack_frame = true;
         }
      } else if (regs.m && interrupt) { /* M + Interrupt */
         m68k_areg (regs, 7) -= 2;
         x_put_word (m68k_areg (regs, 7), vector_nr * 4);
         m68k_areg (regs, 7) -= 4;
         x_put_long (m68k_areg (regs, 7), currpc);
         m68k_areg (regs, 7) -= 2;
         x_put_word (m68k_areg (regs, 7), regs.sr);
         regs.sr |= (1 << 13);
         regs.msp = m68k_areg(regs, 7);
         regs.m = 0;
         m68k_areg(regs, 7) = regs.isp;
         m68k_areg (regs, 7) -= 2;
         x_put_word (m68k_areg (regs, 7), 0x1000 + vector_nr * 4);
      } else {
         Exception_build_stack_frame_common(oldpc, currpc, nr);
         used_exception_build_stack_frame = true;
      }
   } else {
      nextpc = m68k_getpc ();
      if (nr == 2 || nr == 3) {
         // 68000 bus/address error
         uae_u16 mode = (sv ? 4 : 0) | last_fc_for_exception_3;
         mode |= last_writeaccess_for_exception_3 ? 0 : 16;
         mode |= last_notinstruction_for_exception_3 ? 8 : 0;
         exception_in_exception = -1;
         Exception_build_68000_address_error_stack_frame(mode, last_op_for_exception_3, last_fault_for_exception_3, last_addr_for_exception_3);
         goto kludge_me_do;
      }
   }
   if (!used_exception_build_stack_frame) {
      m68k_areg (regs, 7) -= 4;
      x_put_long (m68k_areg (regs, 7), nextpc);
      m68k_areg (regs, 7) -= 2;
      x_put_word (m68k_areg (regs, 7), regs.sr);
   }
   if (currprefs.cpu_model == 68040 && nr == 3 && (last_op_for_exception_3 & 0x10000)) {
      // Weird 68040 bug with RTR and RTE. New SR when exception starts. Stacked SR is different!
      // Just replace it in stack, it is safe enough because we are in address error exception
      // any other exception would halt the CPU.
      x_put_word(m68k_areg(regs, 7), last_sr_for_exception3);
   }
kludge_me_do:
   if ((regs.vbr & 1) && currprefs.cpu_model <= 68010) {
      cpu_halt(CPU_HALT_DOUBLE_FAULT);
      return;
   }
   if (interrupt)
      regs.intmask = nr - 24;
   newpc = x_get_long (regs.vbr + 4 * vector_nr);
   exception_in_exception = 0;
   if (newpc & 1) {
      if (nr == 2 || nr == 3) {
         cpu_halt(CPU_HALT_DOUBLE_FAULT);
         return;
      }
      // 4 idle, write pc low, write sr, write pc high, read vector high, read vector low
      x_do_cycles(adjust_cycles(6 * 4 * CYCLE_UNIT / 2));
      if (currprefs.cpu_model == 68000) {
         regs.t1 = 0;
         MakeSR();
         m68k_setpc(regs.vbr + 4 * vector_nr);
         if (interrupt) {
            regs.ir = nr;
            exception3_read_access(regs.ir | 0x20000 | 0x10000, newpc, sz_word, 2);
         } else {
            exception3_read_access(regs.ir | 0x40000 | 0x20000 | (g1 ? 0x10000 : 0), newpc, sz_word, 2);
         }
      } else if (currprefs.cpu_model == 68010) {
         regs.t1 = 0;
         MakeSR();
         regs.write_buffer = 4 * vector_nr;
         regs.read_buffer = newpc;
         regs.irc = regs.read_buffer;
         exception3_read_access(regs.ir, newpc, sz_word, 2);
      } else {
         exception_check_trace(nr);
         exception3_notinstruction(regs.ir, newpc);
      }
      return;
   }
   add_approximate_exception_cycles(nr);
   m68k_setpc (newpc);
#ifdef JIT
   if (currprefs.cachesize) {
      set_special(SPCFLAG_END_COMPILE);
   }
#endif
   regs.ipl_pin = intlev();
   ipl_fetch();
   fill_prefetch ();
   exception_check_trace (nr);
}

// address = format $2 stack frame address field
static void ExceptionX (int nr, uaecptr oldpc)
{
   uaecptr pc = m68k_getpc();
   regs.exception = nr;
   if (cpu_tracer) {
      cputrace.state = nr;
   }
   if (oldpc != 0xffffffff) {
      regs.instruction_pc = oldpc;
   }

#ifdef CPUEMU_13
   if (currprefs.cpu_cycle_exact && currprefs.cpu_model <= 68010)
      Exception_ce000 (nr);
   else
#endif
   {
      Exception_normal(nr);
   }
   regs.exception = 0;
   if (cpu_tracer) {
      cputrace.state = 0;
   }
}

void REGPARAM2 Exception_cpu_oldpc(int nr, uaecptr oldpc)
{
   bool t0 = currprefs.cpu_model >= 68020 && regs.t0 && !regs.t1;
   ExceptionX(nr, oldpc);
   // Check T0 trace
   // RTE format error ignores T0 trace
   if (nr != 14) {
      if (currprefs.cpu_model >= 68040 && internalexception(nr)) {
         t0 = false;
      }
      if (t0) {
         activate_trace();
      }
   }
}
void REGPARAM2 Exception_cpu(int nr)
{
   Exception_cpu_oldpc(nr, 0xffffffff);
}
void REGPARAM2 Exception(int nr)
{
   ExceptionX(nr, 0xffffffff);
}

extern "C" void bus_error(void)
{
   TRY (prb2) {
      Exception (2);
   } CATCH (prb2) {
      cpu_halt (CPU_HALT_BUS_ERROR_DOUBLE_FAULT);
   } ENDTRY
}

static void do_interrupt (int nr)
{
#ifdef DEBUGGER
   if (debug_dma)
      record_dma_event(DMA_EVENT_CPUIRQ, current_hpos (), vpos);
#endif
//   if (inputrecord_debug & 2) {
//      if (input_record > 0)
//         inprec_recorddebug_cpu(2, 0);
//      else if (input_play > 0)
//         inprec_playdebug_cpu(2, 0);
//   }

   m68k_unset_stop();

   for (;;) {
      Exception (nr + 24);
      if (!currprefs.cpu_compatible || currprefs.cpu_model == 68060)
         break;
      if (m68k_interrupt_delay)
         nr = regs.ipl;
      else
         nr = intlev();
      if (nr <= 0 || regs.intmask >= nr)
         break;
   }

   doint ();
}

void NMI (void)
{
   do_interrupt (7);
}

static void maybe_disable_fpu(void)
{
   if (!currprefs.fpu_model) {
      regs.pcr |= 2;
   }
}

static void m68k_reset_sr(void)
{
   SET_XFLG ((regs.sr >> 4) & 1);
   SET_NFLG ((regs.sr >> 3) & 1);
   SET_ZFLG ((regs.sr >> 2) & 1);
   SET_VFLG ((regs.sr >> 1) & 1);
   SET_CFLG (regs.sr & 1);
   regs.t1 = (regs.sr >> 15) & 1;
   regs.t0 = (regs.sr >> 14) & 1;
   regs.s  = (regs.sr >> 13) & 1;
   regs.m  = (regs.sr >> 12) & 1;
   regs.intmask = (regs.sr >> 8) & 7;
   /* set stack pointer */
   if (regs.s)
      m68k_areg (regs, 7) = regs.isp;
   else
      m68k_areg (regs, 7) = regs.usp;
}

void m68k_reset_newcpu(bool hardreset)
{
   uae_u32 v;

   regs.pissoff = 0;

   /* UAE_030_MMU: a 68030 CPU reset zeroes the E-bits of TC/TT (translation off).
    * The firmware otherwise never resets the real MMU engine, so after AMIX enables
    * it a warm reset (this fn is the boot AND the n040RSTI GPIO reset path) would run
    * the reset-vector fetch (get_long(4) below) through the previous session's stale
    * page tables. mmu030_reset clears mmu030.enabled + TC E-bit (hardreset>=0) and,
    * for a full reset, SRP/CRP/TT/ATC (hardreset>0). Must precede the get_long(4). */
   if (currprefs.mmu_model == 68030)
      mmu030_reset(hardreset ? 1 : 0);

   regs.halted = 0;
//   gui_data.cpu_halted = 0;
//   gui_led (LED_CPU, 0, -1);

   regs.spcflags = 0;
   m68k_reset_delay = 0;
   regs.ipl = regs.ipl_pin = 0;
//   for (int i = 0; i < IRQ_SOURCE_MAX; i++) {
//      uae_interrupts2[i] = 0;
//      uae_interrupts6[i] = 0;
//      uae_interrupt = 0;
//   }

   // Force config changes (CPU speed) into effect on hard reset
   update_68k_cycles();
   
#ifdef SAVESTATE
   if (isrestore ()) {
      m68k_reset_sr();
      m68k_setpc_normal (regs.pc);
      return;
   } else {
      m68k_reset_delay = currprefs.reset_delay;
      set_special(SPCFLAG_CHECK);
   }
#endif
   regs.s = 1;
   v = get_long (4);
   printf("Read PC from address 4 : 0x%08X\n",v);
#ifndef HOST_TEST_HARNESS
   /* AMIX warm-reboot guard: on `reboot`/`uadmin`, AMIX has cleared the overlay so $0-$8 is chip RAM,
    * not the Kickstart ROM, and the emulator does not restore it on a 68k reset -> get_long(4) returns
    * garbage (e.g. 0xFFFFFFFF) instead of the Kickstart reset PC. Letting the 68k run from a garbage PC
    * scribbles the Z3660 PISCSI registers (the "Unhandled register write" flood) and only ends when a
    * Data Abort triggers hard_reboot() anyway. So detect the invalid vector (a valid reset PC lives in
    * the Kickstart ROM, $F00000-$1000000) and do that clean reboot immediately -- no flood, no garbage
    * execution. The cold-boot vector (Kickstart ROM overlaid at 0 on the real bus) is always valid, so
    * this never false-fires on a normal boot. Gated on a3000_amix_mode so it engages ONLY under AMIX --
    * other CPU modes/configs are untouched. (A true in-place warm restart would need the overlay/030-MMU
    * state fully reset so get_long could fetch the ROM vector at $0/$4 without faulting -- not done here.) */
   if (a3000_amix_mode && (v < 0x00F00000 || v >= 0x01000000)) {
      printf("[Core1] Invalid reset vector 0x%08X (AMIX warm reboot, overlay not restored) -> clean reboot\n", v);
      hard_reboot();
   }
#endif
   m68k_areg (regs, 7) = get_long (0);

   m68k_setpc_normal(v);
   regs.m = 0;
   regs.stopped = 0;
   regs.t1 = 0;
   regs.t0 = 0;
   SET_ZFLG (0);
   SET_XFLG (0);
   SET_CFLG (0);
   SET_VFLG (0);
   SET_NFLG (0);
   regs.intmask = 7;
   regs.vbr = regs.sfc = regs.dfc = 0;
   regs.irc = 0xffff;
#ifdef FPUEMU
   fpu_reset ();
#endif
   regs.caar = regs.cacr = 0;
   regs.itt0 = regs.itt1 = regs.dtt0 = regs.dtt1 = 0;
   regs.tcr = regs.mmusr = regs.urp = regs.srp = 0;
   if (currprefs.cpu_model == 68020) {
      regs.cacr |= 8;
      set_cpu_caches (false);
   }

   mmufixup[0].reg = -1;
   if (currprefs.cpu_model >= 68040) {
      set_cpu_caches(false);
   }
//   a3000_fakekick (0);
   /* only (E)nable bit is zeroed when CPU is reset, A3000 SuperKickstart expects this */
   fake_tc_030 &= ~0x80000000;
   fake_tt0_030 &= ~0x80000000;
   fake_tt1_030 &= ~0x80000000;
   if (hardreset || regs.halted) {
      fake_srp_030 = fake_crp_030 = 0;
      fake_tt0_030 = fake_tt1_030 = fake_tc_030 = 0;
   }
   fake_mmusr_030 = 0;

   /* 68060 FPU is not compatible with 68040,
   * 68060 accelerators' boot ROM disables the FPU
   */
   regs.pcr = 0;

   fill_prefetch ();
}

uae_u32 REGPARAM2 op_illg (uae_u32 opcode)
{
   uaecptr pc = m68k_getpc ();
   int inrom = in_rom (pc);
   int inrt = in_rtarea (pc);

   if (opcode == 0x4afc || opcode == 0xfc4a) {
      if (!valid_address(pc, 4) && valid_address(pc - 4, 4)) {
         // PC fell off the end of RAM
         bus_error();
         return 4;
      }
   }
/*
   if (cloanto_rom && (opcode & 0xF100) == 0x7100) {
      m68k_dreg (regs, (opcode >> 9) & 7) = (uae_s8)(opcode & 0xFF);
      m68k_incpc_normal (2);
      fill_prefetch ();
      return 4;
   }

   if (opcode == 0x4E7B && inrom) {
      if (get_long (0x10) == 0) {
         notify_user (NUMSG_KS68020);
         uae_restart(&currprefs, -1, NULL);
         m68k_setstopped();
         return 4;
      }
   }
*/
#ifdef AUTOCONFIG
   if (opcode == 0xFF0D && inrt) {
      /* User-mode STOP replacement */
      m68k_setstopped ();
      return 4;
   }

   if ((opcode & 0xF000) == 0xA000 && inrt) {
      /* Calltrap. */
      m68k_incpc_normal (2);
      m68k_handle_trap(opcode & 0xFFF);
      fill_prefetch ();
      return 4;
   }
#endif

   if ((opcode & 0xF000) == 0xF000) {
      // Missing MMU or FPU cpSAVE/cpRESTORE privilege check
      if (privileged_copro_instruction(opcode)) {
         Exception(8);
      } else {
         Exception(0xB);
      }
      return 4;
   }
   if ((opcode & 0xF000) == 0xA000) {
      Exception (0xA);
      return 4;
   }

   Exception (4);
   return 4;
}

#ifdef CPUEMU_0

static bool mmu_op30_invea(uae_u32 opcode)
{
   int eamode = (opcode >> 3) & 7;
   int rreg = opcode & 7;

   // Dn, An, (An)+, -(An), immediate and PC-relative not allowed
   if (eamode == 0 || eamode == 1 || eamode == 3 || eamode == 4 || (eamode == 7 && rreg > 1))
      return true;
   return false;
}

static bool mmu_op30fake_pmove (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
   int preg = (next >> 10) & 31;
   int rw = (next >> 9) & 1;
   int fd = (next >> 8) & 1;
   int unused = (next & 0xff);
   const TCHAR *reg = NULL;
   uae_u32 otc = fake_tc_030;
   int siz;

   if (mmu_op30_invea(opcode))
      return true;
   // unused low 8 bits must be zeroed
   if (unused)
      return true;
   // read and fd set?
   if (rw && fd)
      return true;

   switch (preg)
   {
   case 0x10: // TC
      reg = _T("TC");
      siz = 4;
      if (rw)
         x_put_long (extra, fake_tc_030);
      else
         fake_tc_030 = x_get_long (extra);
      break;
   case 0x12: // SRP
      reg = _T("SRP");
      siz = 8;
      if (rw) {
         x_put_long (extra, fake_srp_030 >> 32);
         x_put_long (extra + 4, (uae_u32)fake_srp_030);
      } else {
         fake_srp_030 = (uae_u64)x_get_long (extra) << 32;
         fake_srp_030 |= x_get_long (extra + 4);
      }
      break;
   case 0x13: // CRP
      reg = _T("CRP");
      siz = 8;
      if (rw) {
         x_put_long (extra, fake_crp_030 >> 32);
         x_put_long (extra + 4, (uae_u32)fake_crp_030);
      } else {
         fake_crp_030 = (uae_u64)x_get_long (extra) << 32;
         fake_crp_030 |= x_get_long (extra + 4);
      }
      break;
   case 0x18: // MMUSR
      if (fd) {
         // FD must be always zero when MMUSR read or write
         return true;
      }
      reg = _T("MMUSR");
      siz = 2;
      if (rw)
         x_put_word (extra, fake_mmusr_030);
      else
         fake_mmusr_030 = x_get_word (extra);
      break;
   case 0x02: // TT0
      reg = _T("TT0");
      siz = 4;
      if (rw)
         x_put_long (extra, fake_tt0_030);
      else
         fake_tt0_030 = x_get_long (extra);
      break;
   case 0x03: // TT1
      reg = _T("TT1");
      siz = 4;
      if (rw)
         x_put_long (extra, fake_tt1_030);
      else
         fake_tt1_030 = x_get_long (extra);
      break;
   }

   if (!reg)
      return true;

//   if ((currprefs.cs_mbdmac & 1) && currprefs.mbresmem_low.size > 0) {
//      if (otc != fake_tc_030) {
//         a3000_fakekick (fake_tc_030 & 0x80000000);
//      }
//   }
   return false;
}

static bool mmu_op30fake_ptest (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
   int level = (next&0x1C00)>>10;
   int a = (next >> 8) & 1;

   if (mmu_op30_invea(opcode))
      return true;
   if (!level && a)
      return true;

   fake_mmusr_030 = 0;
   return false;
}

static bool mmu_op30fake_pload (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
   int unused = (next & (0x100 | 0x80 | 0x40 | 0x20));

   if (mmu_op30_invea(opcode))
      return true;
   if (unused)
      return true;
   write_log(_T("PLOAD\n"));
   return false;
}

static bool mmu_op30fake_pflush (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
   int flushmode = (next >> 8) & 31;
   int fc = next & 31;
   int mask = (next >> 5) & 3;
   int fc_bits = next & 0x7f;
   TCHAR fname[100];

   switch (flushmode)
   {
   case 0x00:
   case 0x02:
      return mmu_op30fake_pload(pc, opcode, next, extra);
   case 0x18:
      if (mmu_op30_invea(opcode))
         return true;
      _sntprintf (fname, sizeof fname, _T("FC=%x MASK=%x EA=%08x"), fc, mask, 0);
      break;
   case 0x10:
      _sntprintf (fname, sizeof fname, _T("FC=%x MASK=%x"), fc, mask);
      break;
   case 0x04:
      if (fc_bits)
         return true;
      _tcscpy (fname, _T("ALL"));
      break;
   default:
      return true;
   }
   return false;
}

// 68030 (68851) MMU instructions only
bool mmu_op30 (uaecptr pc, uae_u32 opcode, uae_u16 extra, uaecptr extraa)
{
   int type = extra >> 13;
   /* tri-state: 0 ok, 1 = F-line, -1 = MMU config exception only (no F-line).
    * Backport of WinUAE a333766b. mmu_op30_pmove returns the int; the other
    * handlers still return bool (0/1). */
   int fline = 0;

   switch (type)
   {
   case 0:
   case 2:
   case 3:
      // UAE_030_MMU: real PMOVE actually loads TC/SRP/CRP/TT into the engine.
      if (currprefs.mmu_model)
         fline = mmu_op30_pmove (pc, opcode, extra, extraa);
      else
         fline = mmu_op30fake_pmove (pc, opcode, extra, extraa);
      break;
   case 1:
      if (currprefs.mmu_model)
         fline = mmu_op30_pflush (pc, opcode, extra, extraa);
      else
         fline = mmu_op30fake_pflush (pc, opcode, extra, extraa);
      break;
   case 4:
      if (currprefs.mmu_model)
         fline = mmu_op30_ptest (pc, opcode, extra, extraa);
      else
         fline = mmu_op30fake_ptest (pc, opcode, extra, extraa);
      break;
   }
   if (fline > 0) {
      m68k_setpc(pc);
      op_illg(opcode);
   }
   return fline != 0;
}

/* check if an address matches a ttr */
static int fake_mmu_do_match_ttr(uae_u32 ttr, uaecptr addr, bool super)
{
   if (ttr & MMU_TTR_BIT_ENABLED)   {   /* TTR enabled */
      uae_u8 msb, mask;

      msb = ((addr ^ ttr) & MMU_TTR_LOGICAL_BASE) >> 24;
      mask = (ttr & MMU_TTR_LOGICAL_MASK) >> 16;

      if (!(msb & ~mask)) {

         if ((ttr & MMU_TTR_BIT_SFIELD_ENABLED) == 0) {
            if (((ttr & MMU_TTR_BIT_SFIELD_SUPER) == 0) != (super == 0)) {
               return TTR_NO_MATCH;
            }
         }

         return (ttr & MMU_TTR_BIT_WRITE_PROTECT) ? TTR_NO_WRITE : TTR_OK_MATCH;
      }
   }
   return TTR_NO_MATCH;
}

static int fake_mmu_match_ttr(uaecptr addr, bool super, bool data)
{
   int res;

   if (data) {
      res = fake_mmu_do_match_ttr(regs.dtt0, addr, super);
      if (res == TTR_NO_MATCH)
         res = fake_mmu_do_match_ttr(regs.dtt1, addr, super);
   } else {
      res = fake_mmu_do_match_ttr(regs.itt0, addr, super);
      if (res == TTR_NO_MATCH)
         res = fake_mmu_do_match_ttr(regs.itt1, addr, super);
   }
   return res;
}

// 68040+ MMU instructions only
void mmu_op (uae_u32 opcode, uae_u32 extra)
{
   if ((opcode & 0xFE0) == 0x0500) {
      /* PFLUSH */
      regs.mmusr = 0;
      return;
   } else if ((opcode & 0x0FD8) == 0x0548) {
      /* PTEST */
      int regno = opcode & 7;
      uae_u32 addr = m68k_areg(regs, regno);
      bool write = (opcode & 32) == 0;
      bool super = (regs.dfc & 4) != 0;
      bool data = (regs.dfc & 3) != 2;

      regs.mmusr = 0;
      if (fake_mmu_match_ttr(addr, super, data) != TTR_NO_MATCH) {
         regs.mmusr = MMU_MMUSR_T | MMU_MMUSR_R;
      }
      regs.mmusr |= addr & 0xfffff000;
      return;
   }
   m68k_setpc_normal (m68k_getpc () - 2);
   op_illg (opcode);
}

#endif

static void do_trace (void)
{
   // need to store PC because of branch instructions
   regs.trace_pc = m68k_getpc();
   if (regs.t0 && !regs.t1 && currprefs.cpu_model >= 68020) {
      // this is obsolete
      return;
   }
   if (regs.t1) {
      activate_trace();
   }
}
const uae_atomic uae_int_requested=0;
extern int read_irq;
extern SHARED *shared;
#include "../main.h"
int pissoff_int=1024;
//int pissoff_int=-1024000000;
int set_special_var=1;
void z3660_tasks(void);
extern "C" void ipl_main_read(void);
extern "C" { extern volatile int a3000_amix_mode; }   // A3000 SCSI WD33C93 int-countdown pump
extern "C" { extern volatile uae_u32 amix_tick; }     // TEMP: ~guest-instruction counter for INT2 latency
extern "C" void a3000_scsi_hsync(void);
// a3000_scsi.cpp gates AMIX completion delivery on the guest's CPU interrupt mask so a SCSI INT2 is
// taken at most once per command at an instruction boundary OUTSIDE the level-2 ISR — never while
// a3091intr is mid-flight issuing the next autonomous command (which would ack/lose the new completion).
extern "C" int amix_cpu_intmask(void) { return (int)regs.intmask; }
// ===== AMIX sleep-channel trace (TEMP — remove before commit) =====
// Walk the AMIX 030 supervisor page tables (srp_030) to read a kernel SCN1 VA. DT-aware.
extern uae_u64 srp_030;
static uae_u32 amix_kget2(uae_u32 va){
   uae_u32 idx[3]={(va>>30)&3,(va>>17)&0x1FFF,(va>>11)&0x3F};
   uae_u32 tbl=(uae_u32)srp_030&0xFFFFFFF0u; int dt=(int)((srp_030>>32)&3);
   for(int l=0;l<3;l++){ int ds=(dt==3)?8:4; uae_u32 da=tbl+idx[l]*ds;
      uae_u32 d0=get_long(da), aw=(ds==8)?get_long(da+4):d0;
      if((d0&3)==0) return 0xDEADBEEFu;
      if(l==2||(d0&3)==1) return get_long((aw&0xFFFFF800u)|(va&0x7FF));
      tbl=aw&0xFFFFFFF0u; dt=(int)(d0&3); }
   return 0xDEADBEEFu;
}
// expose guest CPU PC / frame so a3000_scsi.cpp can record WHO reads WD_SCSI_STATUS (TEMP)
extern "C" uae_u32 amix_cpu_pc(void){ return (uae_u32)regs.instruction_pc; }
extern "C" uae_u32 amix_cpu_a6(void){ return (uae_u32)m68k_areg(regs,6); }
extern "C" uae_u32 amix_cpu_kread(uae_u32 va){ return amix_kget2(va); }
static uae_u32 amix_slpchan[8], amix_slpcaller[8]; static int amix_slphead=0;
static uae_u32 amix_slpstk[8][6];   // TEMP: kernel call-stack (6 return addrs via a6 chain) per sleep -> trace init's block path
static uae_u32 amix_swapconf_n=0, amix_swapadd_n=0, amix_swap_openres=0xdead, amix_swap_sizeres=0xdead, amix_swap_devsz=0, amix_lookupname_n=0, amix_lookup_res=0xdead;  // TEMP: swap-config flow trace
static uae_u32 amix_ih_n=0, amix_ih_div=0, amix_ih_fdone=0, amix_ih_fhead=0, amix_ih_ftick=0, amix_ih_ldone=0, amix_ih_lhead=0;  // TEMP: ihandle FIFO-divergence (completed buf a0 vs ddtab.HEAD a3)
static uae_u32 amix_bb_n=0, amix_bb_stk[8]={0}, amix_bb_parent=0;  // TEMP: buf_breakup caller chain (who re-reads init text 179456-179468)
static uae_u32 amix_oss_chan[16]={0}, amix_oss_caller[16]={0}, amix_oss_tick[16]={0}, amix_oss_stk[16][4]={{0}}; static int amix_oss_valid[16]={0};  // TEMP: outstanding-sleep table (sleep adds, wakeprocs removes) -> the sleep never-woken = init terminal block
static uae_u32 amix_donebuf[16], amix_donetick[16], amix_donecaller[16]; static int amix_donehead=0;  // TEMP: biodone(bp) buf+tick+caller ring
static uae_u32 amix_selflink=0, amix_biodone_last=0, amix_biodone_same=0, amix_biodone_maxsame=0;  // TEMP: detect Q1 self-link (bp->av_forw==bp) + repeated same-buf biodone (the write loop)
static uae_u32 amix_stuckbuf=0, amix_stucktick=0;                                // TEMP: last buf_breakup biowait target
static uae_u32 amix_pageinbuf=0, amix_pageintick=0;                              // TEMP: last ufs_getapage(0x07082398) biowait buf = the stranded page-in
static uae_u32 amix_a3091_istate[8]={0,0,0,0,0,0,0,0};                           // TEMP: (unused now) istate histogram
static uae_u32 amix_issue_buf[16], amix_issue_unit[16], amix_issue_tick[16]; static int amix_issue_h=0;  // TEMP: bufs startany ISSUES (a3) + unit (a4)
static uae_u32 amix_disp_csr[16], amix_disp_act[16], amix_disp_tick[16]; static int amix_disp_h=0;  // TEMP: a3091intr@0xd142 dispatch (d2=csr, d0=action)
static uae_u32 amix_disp_acthist[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};  // TEMP: count per dispatched action (action 1 = badhardware = the strand)
static uae_u32 amix_badhw_n=0, amix_badhw_tick=0;  // TEMP: a3091 badhardware(0x700cf46) hits = completion dispatched with NO biodone
static int amix_ddcrit=0; static uae_u32 amix_ddstrat_n=0, amix_preempt_n=0, amix_preempt_tick=0;  // TEMP: a3091intr ENTERED during ddstrategy's IPL2 splbio window = the IPL-preemption strand
static uae_u32 amix_dds_buf[16], amix_dds_valid[16], amix_dds_path[16], amix_dds_tick[16]; static int amix_dds_h=0, amix_dds_cur=-1;  // TEMP: ddstrategy(buf) fate: sdvalid + dispatch path (0=pending 1=startio/dispatch 2=append-no-kick 3=sdvalid-REJECT)
static uae_u32 amix_dds_reject_n=0, amix_dds_reject_buf=0;  // TEMP: count of sdvalid rejects (ddstrategy drops buf with NO enqueue + NO biodone)
// PARENT-COMPLETION ACCOUNTING (verdict's experiment): the page-in process biowaits a parent buf woken when a per-parent
// child-count hits 0. If a child completes WITHOUT decrementing its parent (b_iodone not yet wired = the synchronous-DMA
// race), the count never hits 0 -> parent never biodone'd -> biowait forever.
static uae_u32 amix_gio_par[16], amix_gio_cnt[16], amix_gio_child[16]; static int amix_gio_h=0;  // gen_iodone@0x703d940: parent (child->+0x50), count AFTER decrement, child
static uae_u32 amix_pbio_buf[16]; static int amix_pbio_h=0;          // biodone no-callback (parent/standalone) @0x703ce34: buf set B_DONE+wakeprocs
static uae_u32 amix_gio_n=0, amix_gio_orphan_n=0, amix_pbio_n=0, amix_cbio_n=0;  // counts: gen_iodone calls, orphan(child->+0x50==0), parent-biodones, child(callback)-biodones
static uae_u32 amix_bw_buf[8]; static int amix_bw_h=0;               // biowait@0x703cd2c entry: buf a2 (what the page-in process blocks on)
extern "C" { extern volatile uae_u32 amix_scmd_unit[16],amix_scmd_lba[16],amix_scmd_n[16],amix_scmd_w[16]; extern volatile int amix_scmd_head; }
extern "C" void a3000_scsi_dumpstate(void);
extern "C" void a3000_scsi_dumpqueue(void);
extern "C" { extern volatile uae_u32 amix_wcmd_cmd[16],amix_wcmd_dest[16],amix_wcmd_ph[16]; extern volatile int amix_wcmd_head; }
extern "C" { extern volatile uae_u32 amix_compl_tick[16],amix_compl_istate[16],amix_compl_csr[16],amix_compl_unit[16],amix_compl_head[16]; extern volatile int amix_compl_h; }
// ===== STRAND TRACE (TEMP) — per-buf lifecycle to locate where the stranded page-in completion is lost =====
// stage: 1=ddstrategy entered, 2=startany issued, 3=a3091intr HBA-complete, 4=biodone. ddstrategy(1) RESETS (handles buf-addr reuse).
// After the strand the system goes idle (no new I/O), so the stranded buf's entry survives = its final stage shows where it stopped.
#define AMIX_STRK_N 96
static uae_u32 strk_buf[AMIX_STRK_N]={0}, strk_stg[AMIX_STRK_N]={0};
static uae_u32 strk_t1[AMIX_STRK_N]={0},strk_t2[AMIX_STRK_N]={0},strk_t3[AMIX_STRK_N]={0},strk_t4[AMIX_STRK_N]={0};
static uae_u32 strk_ca2=0,strk_ca2_4=0,strk_ca2_1c=0,strk_ca2_20=0;   // a3091intr COMPLETE: resolve a2 = cmd-node vs buf (one sample)
static uae_u32 amix_done_lba[16]={0}, amix_done_fn[16]={0};   // biodone: buf b_blkno(+0x28) + the TRUE completion fn (iodone's caller, 1 frame up)
static uae_u32 amix_d16e_n=0;   // a3091intr COMPLETE-arm ENTRY (0x700d16e, the jmp target) hit count — tests jmp-target hook visibility
static uae_u32 amix_bd_total=0, amix_bd_haveiod=0, amix_bd_shouldcb=0;   // biodone: total, with b_iodone(+48)!=0, with b_iodone&&B_CALL(bit29) [should take callback]
static uae_u32 amix_bd_iodr[12]={0}, amix_bd_flr[12]={0}, amix_bd_bufr[12]={0}; static int amix_bd_h=0;   // ring of biodones that HAVE a b_iodone
static uae_u32 amix_pio_su_buf=0, amix_pio_su_iod=0, amix_pio_su_gp=0, amix_pio_su_fl=0, amix_pio_su_n=0;   // ufs_getapage: buf at pageio_setup return (0x70822d8)
static uae_u32 amix_pio_wt_buf=0, amix_pio_wt_iod=0, amix_pio_wt_gp=0, amix_pio_wt_fl=0, amix_pio_wt_n=0;   // ufs_getapage: buf right before biowait (0x7082390) — stale-at-setup vs set-by-strategy
static uae_u32 amix_app_n=0, amix_app_drained_n=0, amix_app_ot=0, amix_app_otf=0, amix_app_buf=0, amix_app_tick=0;   // ddstrategy APPEND: count appends onto an already-DRAINED (B_DONE) tail = the IPL2-bypass strand
static int strk_idx(uae_u32 b){ if(!b) return -1; unsigned h=(b>>4)%AMIX_STRK_N; for(unsigned i=0;i<AMIX_STRK_N;i++){ unsigned j=(h+i)%AMIX_STRK_N; if(strk_buf[j]==b) return (int)j; if(strk_buf[j]==0){ strk_buf[j]=b; return (int)j; } } return -1; }
static void strk_mk(uae_u32 b,int s,uae_u32 t){ int j=strk_idx(b); if(j<0) return; if(s==1){ strk_stg[j]=1; strk_t1[j]=t; strk_t2[j]=strk_t3[j]=strk_t4[j]=0; } else { if((uae_u32)s>strk_stg[j]) strk_stg[j]=(uae_u32)s; if(s==2)strk_t2[j]=t; else if(s==3)strk_t3[j]=t; else if(s==4)strk_t4[j]=t; } }
static inline void check_uae_int_request(void)
{
   z3660_tasks();
   ipl_main_read();
   // A3000 SCSI WD33C93 interrupt-delay pump: advances the status countdown on a fixed cadence in
   // EVERY CPU run loop (including the MMU-off loop the Kickstart ROM uses to load the kernel), so the
   // SELECT and SRV_REQ INT2s stay spaced for AMIX's step-by-step sd open WITHOUT stalling the boot load.
   if(a3000_amix_mode){ amix_tick++; static int scsi_hctr=0; if(++scsi_hctr>=256){ scsi_hctr=0; a3000_scsi_hsync(); } }
   // ===== AMIX sleep-channel + PC sampler (TEMP) — gated behind debug_emu (DEMU) so it is OFF by
   // default; this per-instruction PC sampling (~40 compares + guest reads) was a real AMIX perf
   // drain. Turn DEMU on to re-arm it for a3091 work; the dumps need a moment to re-accumulate. =====
   if(a3000_amix_mode && shared->debug_emu){
      uae_u32 ipc=(uae_u32)regs.instruction_pc;
      if(ipc==0x0700BED8u){ amix_ddcrit=1; amix_ddstrat_n++; }   // ddstrategy: just did move.w #$2200,sr (IPL2 splbio) -> enter critical
      if(ipc==0x0700BF04u){ amix_ddcrit=0; }                       // ddstrategy: move.w d2,sr (splx) -> leave critical
      if(ipc==0x0700D0E0u && amix_ddcrit){ amix_preempt_n++; amix_preempt_tick=amix_tick; }  // a3091intr ENTERED while ddstrategy holds IPL2 = IPL-preemption violation
      if(ipc==0x0700D142u){   // a3091intr: d0 = atab[istate*9+itab[csr]] = the dispatched ACTION (just set @0xd13e); d2 = csr
         int h=amix_disp_h&15; uae_u32 act=(uae_u32)(m68k_dreg(regs,0)&0xff);
         amix_disp_csr[h]=(uae_u32)(m68k_dreg(regs,2)&0xff); amix_disp_act[h]=act; amix_disp_tick[h]=amix_tick; amix_disp_h++;
         if(act<16) amix_disp_acthist[act]++;
      }
      if(ipc==0x0700D16Eu) amix_d16e_n++;   // a3091intr COMPLETE-arm entry (the jmp target) — does a jmp-target PC-hook fire? (0x700d17e never did)
      if(ipc==0x070822D8u){ uae_u32 b=(uae_u32)m68k_areg(regs,0); amix_pio_su_buf=b; amix_pio_su_iod=amix_kget2(b+0x48u); amix_pio_su_gp=amix_kget2(b+0x50u); amix_pio_su_fl=amix_kget2(b+0x0u); amix_pio_su_n++; }   // ufs_getapage: pageio_setup just returned buf=a0
      if(ipc==0x07082390u){ uae_u32 b=(uae_u32)m68k_areg(regs,2); amix_pio_wt_buf=b; amix_pio_wt_iod=amix_kget2(b+0x48u); amix_pio_wt_gp=amix_kget2(b+0x50u); amix_pio_wt_fl=amix_kget2(b+0x0u); amix_pio_wt_n++; }   // ufs_getapage: about to biowait(a2)
      if(ipc==0x0700CF46u){ amix_badhw_n++; amix_badhw_tick=amix_tick; }   // badhardware: a completion that biodones NOTHING
      if(ipc==0x0700BE9Au){   // ddstrategy: d0 = sdvalid() result (tst.b d0 next), a2 = buf. Records EVERY ddstrategy call's fate.
         int h=amix_dds_h&15; amix_dds_buf[h]=(uae_u32)m68k_areg(regs,2);
         uae_u32 v=(uae_u32)(m68k_dreg(regs,0)&0xff); amix_dds_valid[h]=v; amix_dds_path[h]=v?0:3; amix_dds_tick[h]=amix_tick;
         amix_dds_cur=h; amix_dds_h++; strk_mk((uae_u32)m68k_areg(regs,2),1,amix_tick);   // STRK stage1: ddstrategy entered (resets buf entry)
         if(!v){ amix_dds_reject_n++; amix_dds_reject_buf=(uae_u32)m68k_areg(regs,2); }   // sdvalid REJECT: dropped with no enqueue, no biodone
      }
      if(ipc==0x0700BEFCu && amix_dds_cur>=0){ amix_dds_path[amix_dds_cur]=1; }   // startio: DDTAB was empty -> dispatched
      if(ipc==0x0700BEE4u){   // ddstrategy APPEND (a0=old TAIL, a2=buf): DDTAB non-empty -> appended, relies on in-flight completion to re-kick
         if(amix_dds_cur>=0) amix_dds_path[amix_dds_cur]=2;
         amix_app_n++;
         uae_u32 ot=(uae_u32)m68k_areg(regs,0), otf=amix_kget2(ot);
         if(otf&2u){ amix_app_drained_n++; amix_app_ot=ot; amix_app_otf=otf; amix_app_buf=(uae_u32)m68k_areg(regs,2); amix_app_tick=amix_tick; }   // appended onto an ALREADY-DRAINED (B_DONE) tail = the IPL2-bypass strand
      }
      // --- PARENT-COMPLETION accounting (verdict's experiment) ---
      if(ipc==0x0703D8D6u){   // gen_iodone entry: a0 = child buf; child->+0x50 = parent (0 = orphan = lost decrement)
         amix_gio_n++; if(amix_kget2((uae_u32)m68k_areg(regs,0)+0x50u)==0) amix_gio_orphan_n++;
      }
      if(ipc==0x0703D940u){   // gen_iodone: just did subq.l #1,$54(a2); a2 = parent, a0 = child. Count AFTER decrement.
         int h=amix_gio_h&15; amix_gio_par[h]=(uae_u32)m68k_areg(regs,2); amix_gio_child[h]=(uae_u32)m68k_areg(regs,0);
         amix_gio_cnt[h]=amix_kget2((uae_u32)m68k_areg(regs,2)+0x54u); amix_gio_h++;
      }
      if(ipc==0x0703CE28u){ amix_cbio_n++; }   // biodone: child path (b_iodone callback = gen_iodone) -> decrements a parent
      if(ipc==0x0703CE34u){   // biodone: no-callback path -> set B_DONE + (if sync) wakeprocs. This is a parent/standalone buf.
         amix_pbio_n++; int h=amix_pbio_h&15; amix_pbio_buf[h]=(uae_u32)m68k_areg(regs,2); amix_pbio_h++;
      }
      if(ipc==0x0703CD34u){   // biowait entry: just did movea.l $8(a6),a2 -> a2 = the buf the page-in process blocks on
         int h=amix_bw_h&7; amix_bw_buf[h]=(uae_u32)m68k_areg(regs,2); amix_bw_h++;
      }
      if(ipc==0x070485F4u){   // sleep(chan,pri): just after 'link.w a6,#0', so caller=(a6+4), chan=(a6+8)
         uae_u32 a6=(uae_u32)m68k_areg(regs,6);
         uae_u32 caller=amix_kget2(a6+4);
         if((caller<0x07059000u||caller>=0x0705A000u) && caller!=0x0703D9F0u){   // skip swapper/sched idle (0x07059xxx) AND buf_breakup (0x0703D9F0) so init's blocking sleep is retained
            uae_u32 chan=amix_kget2((uae_u32)m68k_areg(regs,7)+4);   // REAL sleep chan (sleep's 1st arg on the stack, a7+4 before its link executes)
            int sh=amix_slphead&7;
            amix_slpcaller[sh]=caller;
            amix_slpchan[sh]=chan;
            // walk the kernel call stack (a6 frame chain) -> 6 return addrs, to trace init's syscall->sleep path
            { uae_u32 fp=a6; for(int L=0;L<6;L++){ if(fp<0x07000000u||fp>=0x42000000u){ amix_slpstk[sh][L]=0; continue; }
                 amix_slpstk[sh][L]=amix_kget2(fp+4); uae_u32 nf=amix_kget2(fp); if(nf<=fp||nf==0) { for(int M=L+1;M<6;M++) amix_slpstk[sh][M]=0; break; } fp=nf; } }
            amix_slphead++;
            // outstanding-sleep table: slot by chan, else free, else oldest
            { int slot=-1; for(int j=0;j<16;j++) if(amix_oss_valid[j]&&amix_oss_chan[j]==chan){slot=j;break;}
              if(slot<0) for(int j=0;j<16;j++) if(!amix_oss_valid[j]){slot=j;break;}
              if(slot<0){ uae_u32 mt=0xffffffffu; for(int j=0;j<16;j++) if(amix_oss_tick[j]<=mt){mt=amix_oss_tick[j];slot=j;} }
              amix_oss_chan[slot]=chan; amix_oss_caller[slot]=caller; amix_oss_tick[slot]=amix_tick; amix_oss_valid[slot]=1;
              for(int L=0;L<4;L++) amix_oss_stk[slot][L]=amix_slpstk[sh][L]; }
            if(caller==0x07082398u){ amix_pageinbuf=chan; amix_pageintick=amix_tick; }   // ufs_getapage page-in buf = the stranded read
         }
      }
      if(ipc==0x0703D14Eu){   // buf_breakup entry: a6 not yet linked; arg parent=$c(a7)+? walk RETURN chain via a7 then a6 after link is later. Use a7: ret@a7, caller frame.
         amix_bb_n++; amix_bb_parent=amix_kget2((uae_u32)m68k_areg(regs,7)+0xc);   // 2nd arg (the parent buf) at a7+0xc (ret@a7, arg1@a7+4=strategy, arg2@a7+8?, parent@a7+c)
         uae_u32 ra=amix_kget2((uae_u32)m68k_areg(regs,7)); amix_bb_stk[0]=ra;     // immediate caller (gen_strategy)
         uae_u32 fp=(uae_u32)m68k_areg(regs,6);
         for(int L=1;L<8;L++){ if(fp<0x07000000u||fp>=0x42000000u){amix_bb_stk[L]=0;continue;} amix_bb_stk[L]=amix_kget2(fp+4); uae_u32 nf=amix_kget2(fp); if(nf<=fp||nf==0){for(int M=L+1;M<8;M++)amix_bb_stk[M]=0;break;} fp=nf; }
      }
      // --- SWAP-CONFIG flow trace ---
      if(ipc==0x070B3FFAu) amix_swapconf_n++;                          // swapconf entry
      if(ipc==0x070B4028u){ amix_lookupname_n++; amix_lookup_res=(uae_u32)m68k_dreg(regs,0); }   // after lookupname(swap path): d0=result
      if(ipc==0x070B3114u) amix_swapadd_n++;                           // swapadd entry
      if(ipc==0x070B3152u) amix_swap_openres=(uae_u32)m68k_dreg(regs,6);   // after VOP_OPEN (spec_open): d6=open result
      if(ipc==0x070B318Eu){ amix_swap_sizeres=(uae_u32)m68k_dreg(regs,6); amix_swap_devsz=amix_kget2((uae_u32)m68k_areg(regs,6)-0x4cu); }  // after size VOP: d6=result, -0x4c(a6)=devsize
      if(ipc==0x070488B6u){   // wakeprocs(chan,flag): at entry a7+4 = chan (1st arg, before link). Clear outstanding sleeps on this chan.
         uae_u32 wc=amix_kget2((uae_u32)m68k_areg(regs,7)+4);
         for(int j=0;j<16;j++) if(amix_oss_valid[j]&&amix_oss_chan[j]==wc) amix_oss_valid[j]=0;
      }
      if(ipc==0x0700BFE8u){   // ihandle: a0 = cmd_node ($8(a6)); REAL completed buf = cmd_node->4; a3 = ddtab.HEAD (the buf ihandle WILL biodone). Bug if completed!=HEAD.
         uae_u32 cmdnode=(uae_u32)m68k_areg(regs,0), head=(uae_u32)m68k_areg(regs,3);
         uae_u32 donebuf=amix_kget2(cmdnode+4u);   // CORRECTED: cmd_node->4 = the buf the HBA actually completed
         amix_ih_n++; amix_ih_ldone=donebuf; amix_ih_lhead=head;
         if(donebuf!=head){ amix_ih_div++; if(!amix_ih_fdone){ amix_ih_fdone=donebuf; amix_ih_fhead=head; amix_ih_ftick=amix_tick; } }
      }
      if(ipc==0x0700D034u){   // startany: a3 = the sc(=bp) it is about to issue, a4 = its unit (regs reliable)
         int hh=amix_issue_h&15;
         amix_issue_buf[hh]=(uae_u32)m68k_areg(regs,3); amix_issue_unit[hh]=(uae_u32)m68k_areg(regs,4);
         amix_issue_tick[hh]=amix_tick; amix_issue_h++; strk_mk((uae_u32)m68k_areg(regs,3),2,amix_tick);   // STRK stage2: startany issue (a3=sc/buf)
      }
      if(ipc==0x0700D17Eu){   // a3091intr d16e COMPLETE handler: a2 = curunit->head (buf being biodone'd), a3 = curunitp
         int hh=amix_compl_h&15;
         amix_compl_head[hh]=(uae_u32)m68k_areg(regs,2); amix_compl_unit[hh]=(uae_u32)m68k_areg(regs,3);
         amix_compl_istate[hh]=1; amix_compl_csr[hh]=0x16; amix_compl_tick[hh]=amix_tick; amix_compl_h++;
         { uae_u32 a2c=(uae_u32)m68k_areg(regs,2);   // STRK: resolve a2 (cmd-node vs buf) + mark stage3 for BOTH a2 and a2->4 (the real buf gets marked)
           if(!strk_ca2){ strk_ca2=a2c; strk_ca2_4=amix_kget2(a2c+4); strk_ca2_1c=amix_kget2(a2c+0x1c); strk_ca2_20=amix_kget2(a2c+0x20); }
           strk_mk(a2c,3,amix_tick); strk_mk(amix_kget2(a2c+4),3,amix_tick); }
      }
      if(ipc==0x0703CE18u){   // biodone: after 'movea.l $8(a6),a2', a2 = bp (the completing buf); 4(a6) = caller
         uae_u32 bp=(uae_u32)m68k_areg(regs,2); strk_mk(bp,4,amix_tick);   // STRK stage4: biodone
         amix_donebuf[amix_donehead&15]=bp;
         amix_donetick[amix_donehead&15]=amix_tick;
         amix_donecaller[amix_donehead&15]=amix_kget2((uae_u32)m68k_areg(regs,6)+4);
         amix_done_lba[amix_donehead&15]=amix_kget2(bp+0x28u);   // b_blkno
         amix_done_fn[amix_donehead&15]=amix_kget2(amix_kget2((uae_u32)m68k_areg(regs,6))+4u);   // saved-a6 = iodone's frame; +4 = iodone's caller = the REAL completion fn
         amix_donehead++;
         { uae_u32 iod=amix_kget2(bp+0x48u), fl=amix_kget2(bp+0x0u);   // b_iodone(+0x48), flags(+0); biodone callback needs iod!=0 && (fl&bit29=B_CALL)
           amix_bd_total++;
           if(iod){ amix_bd_haveiod++; int h=amix_bd_h%12; amix_bd_iodr[h]=iod; amix_bd_flr[h]=fl; amix_bd_bufr[h]=bp; amix_bd_h++;
              if(fl&0x20000000u) amix_bd_shouldcb++; } }
         { uae_u32 avf=amix_kget2(bp+0xcu); if(bp && avf==bp) amix_selflink++; }   // bp->av_forw(+0xc)==bp = Q1 SELF-LINK
         if(bp==amix_biodone_last){ amix_biodone_same++; if(amix_biodone_same>amix_biodone_maxsame) amix_biodone_maxsame=amix_biodone_same; }
         else { amix_biodone_same=0; amix_biodone_last=bp; }
      }
      if(ipc==0x0703D1DCu || ipc==0x0703D22Cu){   // buf_breakup 'jsr sleep': a2 = the chunk buf it biowaits on
         amix_stuckbuf=(uae_u32)m68k_areg(regs,2);
         amix_stucktick=amix_tick;
      }
      static uae_u32 pcc=0; if(shared->debug_emu && (++pcc & 0x3FFFFF)==0){   // [PC]/[SLP] trace behind the DEMU toggle
         z3660_printf("[PC] %08lX s=%d msk=%d\r\n",(unsigned long)ipc,(int)regs.s,(int)regs.intmask);
         // when idling in swtch (0x070b90xx), dump the last sleep callers/chans + last SCSI commands
         if(ipc>=0x070B9000u && ipc<0x070B9300u){
            for(int k=0;k<8;k++){ int i=(amix_slphead-8+k)&7;
               z3660_printf("[SLP] caller=%08lX chan=%08lX  stk: %08lX %08lX %08lX %08lX %08lX %08lX\r\n",
                  (unsigned long)amix_slpcaller[i],(unsigned long)amix_slpchan[i],
                  (unsigned long)amix_slpstk[i][0],(unsigned long)amix_slpstk[i][1],(unsigned long)amix_slpstk[i][2],
                  (unsigned long)amix_slpstk[i][3],(unsigned long)amix_slpstk[i][4],(unsigned long)amix_slpstk[i][5]); }
            z3660_printf("[SWAP] swapconf=%lu lookupname=%lu(res=%ld) swapadd=%lu openVOPres=%ld sizeVOPres=%ld devsz=%lu(=%lu blks)\r\n",
               (unsigned long)amix_swapconf_n,(unsigned long)amix_lookupname_n,(long)(int32_t)amix_lookup_res,(unsigned long)amix_swapadd_n,
               (long)(int32_t)amix_swap_openres,(long)(int32_t)amix_swap_sizeres,(unsigned long)amix_swap_devsz,(unsigned long)(amix_swap_devsz>>9));
            z3660_printf("[IHANDLE] calls=%lu  FIFO-divergences(completed!=ddtab.HEAD)=%lu  first: completed=%08lX head=%08lX @tick=%lu  last: completed=%08lX head=%08lX\r\n",
               (unsigned long)amix_ih_n,(unsigned long)amix_ih_div,(unsigned long)amix_ih_fdone,(unsigned long)amix_ih_fhead,(unsigned long)amix_ih_ftick,(unsigned long)amix_ih_ldone,(unsigned long)amix_ih_lhead);
            z3660_printf("[OSS] outstanding sleeps (never woken = the terminal block; oldest tick = init's block):\r\n");
            for(int j=0;j<16;j++) if(amix_oss_valid[j])
               z3660_printf("  chan=%08lX caller=%08lX tick=%lu  stk: %08lX %08lX %08lX %08lX\r\n",
                  (unsigned long)amix_oss_chan[j],(unsigned long)amix_oss_caller[j],(unsigned long)amix_oss_tick[j],
                  (unsigned long)amix_oss_stk[j][0],(unsigned long)amix_oss_stk[j][1],(unsigned long)amix_oss_stk[j][2],(unsigned long)amix_oss_stk[j][3]);
            z3660_printf("[BBREAK] buf_breakup calls=%lu parent=%08lX  caller-chain: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n",
               (unsigned long)amix_bb_n,(unsigned long)amix_bb_parent,(unsigned long)amix_bb_stk[0],(unsigned long)amix_bb_stk[1],(unsigned long)amix_bb_stk[2],(unsigned long)amix_bb_stk[3],
               (unsigned long)amix_bb_stk[4],(unsigned long)amix_bb_stk[5],(unsigned long)amix_bb_stk[6],(unsigned long)amix_bb_stk[7]);
            { int donefound=-1; for(int k=0;k<16;k++){ if(amix_donebuf[k]==amix_stuckbuf && amix_stuckbuf){ donefound=(int)amix_donetick[k]; } }
              z3660_printf("[STUCKBUF] buf=%08lX slept@tick=%lu  biodone'd=%s (tick=%d)  tick_now=%lu\r\n",
                 (unsigned long)amix_stuckbuf,(unsigned long)amix_stucktick, donefound>=0?"YES":"NO", donefound,(unsigned long)amix_tick);
              for(int k=0;k<16;k++){ int i=(amix_donehead-16+k)&15;
                 z3660_printf("[DONE] buf=%08lX tick=%lu by=%08lX\r\n",(unsigned long)amix_donebuf[i],(unsigned long)amix_donetick[i],(unsigned long)amix_donecaller[i]); } }
            { int pf=-1; for(int k=0;k<16;k++){ if(amix_donebuf[k]==amix_pageinbuf && amix_pageinbuf){ pf=(int)amix_donetick[k]; } }
              z3660_printf("[PGINBUF] buf=%08lX slept@tick=%lu  biodone'd=%s (tick=%d)\r\n",
                 (unsigned long)amix_pageinbuf,(unsigned long)amix_pageintick, pf>=0?"YES":"NO", pf); }
            for(int k=0;k<16;k++){ int i=(amix_issue_h-16+k)&15;
               z3660_printf("[ISSUE] buf=%08lX unit=%08lX tick=%lu\r\n",(unsigned long)amix_issue_buf[i],(unsigned long)amix_issue_unit[i],(unsigned long)amix_issue_tick[i]); }
            { int iss=0,cmp=0; for(int k=0;k<16;k++){ if(amix_pageinbuf){ if(amix_issue_buf[k]==amix_pageinbuf) iss=1; if(amix_compl_head[k]==amix_pageinbuf) cmp=1; } }
              z3660_printf("[PGINTRK] pageinbuf=%08lX in_issue=%d in_compl=%d\r\n",(unsigned long)amix_pageinbuf,iss,cmp); }
            // [STRK] CORRECTED lifecycle: resolve a3091intr a2 (cmd-node vs buf) + the stranded buf's furthest stage
            z3660_printf("[STRK] complete a2=%08lX ->4=%08lX ->1c=%08lX ->20=%08lX  (if cmd-node: ->4=buf ->1c=lun ->20=tgt)\r\n",
               (unsigned long)strk_ca2,(unsigned long)strk_ca2_4,(unsigned long)strk_ca2_1c,(unsigned long)strk_ca2_20);
            { uae_u32 cand[3]={amix_pageinbuf,amix_stuckbuf,amix_bb_parent}; const char* nm[3]={"pagein","stuckchunk","bbparent"};
              for(int c=0;c<3;c++){ uae_u32 sb=cand[c]; if(!sb) continue; int sj=-1; for(int i=0;i<AMIX_STRK_N;i++) if(strk_buf[i]==sb){sj=i;break;}
                if(sj>=0) z3660_printf("[STRK] %s buf=%08lX stage=%lu t1=%lu t2=%lu t3=%lu t4=%lu  (1=ddstrat 2=issue 3=HBAcompl 4=biodone)\r\n",
                   nm[c],(unsigned long)sb,(unsigned long)strk_stg[sj],(unsigned long)strk_t1[sj],(unsigned long)strk_t2[sj],(unsigned long)strk_t3[sj],(unsigned long)strk_t4[sj]);
                else z3660_printf("[STRK] %s buf=%08lX NOT-IN-TABLE\r\n",nm[c],(unsigned long)sb); } }
            // [STRK-SLP] for each recent sleep chan in buf-range: LIVE B_DONE (buf+0 bit1) + [STRK] stage. DECISIVE:
            //   B_DONE=1 while still slept-on => LOST WAKEUP (or re-fault); B_DONE=0 => lost completion. gencnt>0 => gen-layer child strand.
            { uae_u32 seen[8]={0}; int ns=0;
              for(int s=0;s<8;s++){ uae_u32 sb=amix_slpchan[s]; if(sb<0x40000000u||sb>=0x42000000u) continue;
                int dup=0; for(int q=0;q<ns;q++) if(seen[q]==sb) dup=1; if(dup) continue; if(ns<8) seen[ns++]=sb;
                int sj=-1; for(int i=0;i<AMIX_STRK_N;i++) if(strk_buf[i]==sb){sj=i;break;}
                uae_u32 f0=amix_kget2(sb), gp=amix_kget2(sb+0x50), gc=amix_kget2(sb+0x54);
                z3660_printf("[STRK-SLP] buf=%08lX caller=%08lX B_DONE=%d stage=%ld t1=%lu t4=%lu genpar=%08lX gencnt=%ld\r\n",
                  (unsigned long)sb,(unsigned long)amix_slpcaller[s],(f0&2)?1:0,(sj>=0?(long)strk_stg[sj]:-1L),
                  (unsigned long)(sj>=0?strk_t1[sj]:0),(unsigned long)(sj>=0?strk_t4[sj]:0),(unsigned long)gp,(long)(int32_t)gc); } }
            // [DONE-FN] for each biodone: buf, b_blkno, and the TRUE completion fn (iodone's caller). The fn that repeatedly biodones the wrong buf is the bug site.
            z3660_printf("[D16E] a3091intr COMPLETE-arm-entry(0x700d16e) hits=%lu  ([DISPACT] COMPLETE=139ish; 0 here => jmp-target PC-hooks are blind)\r\n",(unsigned long)amix_d16e_n);
            // [BIODONE] do gen children reach biodone with b_iodone(+0x48)+B_CALL(bit29) set? should_callback>0 but cbio(0x703ce28)=0 => the cbio HOOK is blind (children DO callback, guest fine). should_callback==0 => children never biodone'd with the callback (wrong-buf OR cleared flag = the bug). gen_iodone=0x0703D8CC.
            z3660_printf("[BIODONE] total=%lu have_b_iodone=%lu should_callback(iod&&bit29)=%lu  cbio_hook(0x703ce28)=%lu  gen_iodone=0x0703D8CC\r\n",
               (unsigned long)amix_bd_total,(unsigned long)amix_bd_haveiod,(unsigned long)amix_bd_shouldcb,(unsigned long)amix_cbio_n);
            for(int k=0;k<12;k++){ int i=(amix_bd_h-12+k); while(i<0)i+=12; i%=12; if(amix_bd_bufr[i])
               z3660_printf("[BD] buf=%08lX b_iodone=%08lX flags=%08lX B_CALL=%d\r\n",(unsigned long)amix_bd_bufr[i],(unsigned long)amix_bd_iodr[i],(unsigned long)amix_bd_flr[i],(amix_bd_flr[i]&0x20000000u)?1:0); }
            // [PIOSU]/[PIOWT] does ufs_getapage's buf have b_iodone(gen_iodone=0703D8CC)+genparent AT pageio_setup return (stale) vs only at biowait (set by the strategy)?
            z3660_printf("[PIOSU] pageio_setup-ret buf=%08lX b_iodone=%08lX genparent=%08lX flags=%08lX B_CALL=%d n=%lu\r\n",
               (unsigned long)amix_pio_su_buf,(unsigned long)amix_pio_su_iod,(unsigned long)amix_pio_su_gp,(unsigned long)amix_pio_su_fl,(amix_pio_su_fl&0x20000000u)?1:0,(unsigned long)amix_pio_su_n);
            z3660_printf("[PIOWT] pre-biowait    buf=%08lX b_iodone=%08lX genparent=%08lX flags=%08lX B_CALL=%d n=%lu\r\n",
               (unsigned long)amix_pio_wt_buf,(unsigned long)amix_pio_wt_iod,(unsigned long)amix_pio_wt_gp,(unsigned long)amix_pio_wt_fl,(amix_pio_wt_fl&0x20000000u)?1:0,(unsigned long)amix_pio_wt_n);
            for(int k=0;k<16;k++){ int i=(amix_donehead-16+k)&15; if(!amix_donebuf[i]) continue;
               z3660_printf("[DONE-FN] buf=%08lX lba=%lu fn=%08lX (iodone-ret=%08lX)\r\n",(unsigned long)amix_donebuf[i],(unsigned long)amix_done_lba[i],(unsigned long)amix_done_fn[i],(unsigned long)amix_donecaller[i]); }
            { uae_u32 seen2[8]={0}; int n2=0;
              for(int s=0;s<8;s++){ uae_u32 sb=amix_slpchan[s]; if(sb<0x40000000u||sb>=0x42000000u) continue;
                int dup=0; for(int q=0;q<n2;q++) if(seen2[q]==sb) dup=1; if(dup) continue; if(n2<8) seen2[n2++]=sb;
                uae_u32 lba=amix_kget2(sb+0x28u); int dl=0; for(int k=0;k<16;k++) if(amix_done_lba[k]==lba && lba) dl=1;
                z3660_printf("[STRK-LBA] strandbuf=%08lX lba=%lu thatLBA_biodone'd=%d\r\n",(unsigned long)sb,(unsigned long)lba,dl); } }
            // [APPEND] does any ddstrategy append land on an ALREADY-DRAINED tail? (>0 => the IPL2-bypass strand) + the stranded TAIL's live linkage
            z3660_printf("[APPEND] total=%lu onto-DRAINED-tail=%lu  last: old_tail=%08lX flags=%08lX buf=%08lX @tick=%lu\r\n",
               (unsigned long)amix_app_n,(unsigned long)amix_app_drained_n,(unsigned long)amix_app_ot,(unsigned long)amix_app_otf,(unsigned long)amix_app_buf,(unsigned long)amix_app_tick);
            { uae_u32 tail=amix_kget2(0x070EA8E0u);   // ddtab[tgt6].TAIL (runtime ddtab6=0x070EA8D8, +8=TAIL)
              z3660_printf("[DDLINK] ddtab6 HEAD=%08lX TAIL=%08lX  TAIL->0(flags)=%08lX B_DONE=%d  TAIL->0xc(next)=%08lX\r\n",
                 (unsigned long)amix_kget2(0x070EA8DCu),(unsigned long)tail,(unsigned long)amix_kget2(tail),(amix_kget2(tail)&2)?1:0,(unsigned long)amix_kget2(tail+0xcu)); }
            { int n=0; for(int i=0;i<AMIX_STRK_N&&n<8;i++) if(strk_buf[i]&&strk_stg[i]==3){ z3660_printf("[STRK] s3-only(HBAcompl,NO-biodone) buf=%08lX t1=%lu t3=%lu\r\n",(unsigned long)strk_buf[i],(unsigned long)strk_t1[i],(unsigned long)strk_t3[i]); n++; } }
            { int n=0; for(int i=0;i<AMIX_STRK_N&&n<8;i++) if(strk_buf[i]&&strk_stg[i]==1){ z3660_printf("[STRK] s1-only(ddstrat,NO-compl) buf=%08lX t1=%lu\r\n",(unsigned long)strk_buf[i],(unsigned long)strk_t1[i]); n++; } }
            z3660_printf("[LOOP] selflink(bp->av_forw==bp)=%lu maxsame_biodone=%lu last=%08lX\r\n",
               (unsigned long)amix_selflink,(unsigned long)amix_biodone_maxsame,(unsigned long)amix_biodone_last);
            // Dump raw buf headers (bufs live at 0x40xxxxxx = MMU section 1, so amix_kget2 reads them reliably).
            // Reverse-engineer: b_flags (B_DONE/B_READ/B_BUSY/B_ERROR), b_forw/av_forw (0x40xxxxxx links), b_blkno (=an [SDMA] lba).
            { uae_u32 b=amix_pageinbuf; z3660_printf("[BUFHDR] pagein  %08lX:",(unsigned long)b);
              for(int o=0;o<24;o++) z3660_printf(" %08lX",(unsigned long)(b?amix_kget2(b+o*4):0)); z3660_printf("\r\n"); }
            { uae_u32 b=amix_donebuf[(amix_donehead-1)&15]; z3660_printf("[BUFHDR] lastdone %08lX:",(unsigned long)b);
              for(int o=0;o<24;o++) z3660_printf(" %08lX",(unsigned long)(b?amix_kget2(b+o*4):0)); z3660_printf("\r\n"); }
            for(int k=0;k<16;k++){ int i=(amix_scmd_head-16+k)&15;
               z3660_printf("[SDMA] %c unit=%lu lba=%lu n=%lu\r\n",amix_scmd_w[i]?'W':'R',
                  (unsigned long)amix_scmd_unit[i],(unsigned long)amix_scmd_lba[i],(unsigned long)amix_scmd_n[i]); }
            z3660_printf("[PREEMPT] a3091intr-during-ddstrategy-IPL2 = %lu (last@%lu)  ddstrategy_n=%lu  ddcrit=%d\r\n",
               (unsigned long)amix_preempt_n,(unsigned long)amix_preempt_tick,(unsigned long)amix_ddstrat_n,amix_ddcrit);
            z3660_printf("[DDSREJ] sdvalid_reject_n=%lu reject_buf=%08lX   (ddstrategy drops buf with NO enqueue + NO biodone)\r\n",
               (unsigned long)amix_dds_reject_n,(unsigned long)amix_dds_reject_buf);
            for(int k=0;k<16;k++){ int i=(amix_dds_h-16+k)&15; const char*pn= amix_dds_path[i]==1?"startio":amix_dds_path[i]==2?"APPEND":amix_dds_path[i]==3?"REJECT":"pending";
               z3660_printf("[DDS] buf=%08lX valid=%lu path=%s tick=%lu%s%s\r\n",(unsigned long)amix_dds_buf[i],(unsigned long)amix_dds_valid[i],pn,(unsigned long)amix_dds_tick[i],
                  (amix_dds_buf[i]==amix_pageinbuf&&amix_pageinbuf)?" <==PAGEINBUF":"",(amix_dds_buf[i]==amix_stuckbuf&&amix_stuckbuf)?" <==STUCKBUF":""); }
            // --- PARENT-COMPLETION accounting dump (verdict's experiment) ---
            z3660_printf("[GIOCNT] gen_iodone=%lu orphan(child->+0x50==0)=%lu  child-biodone(callback)=%lu  parent-biodone(wakeprocs)=%lu\r\n",
               (unsigned long)amix_gio_n,(unsigned long)amix_gio_orphan_n,(unsigned long)amix_cbio_n,(unsigned long)amix_pbio_n);
            for(int k=0;k<16;k++){ int i=(amix_gio_h-16+k)&15; if(!amix_gio_par[i]) continue;
               z3660_printf("[GIO] parent=%08lX cnt_after=%ld child=%08lX%s\r\n",(unsigned long)amix_gio_par[i],(long)(int32_t)amix_gio_cnt[i],(unsigned long)amix_gio_child[i],
                  (amix_gio_par[i]==amix_pageinbuf&&amix_pageinbuf)?" <==PAGEINBUF":""); }
            // For each buf the page-in process blocks on (biowait ring), dump its completion state + whether it was ever
            // gen_iodone-parent'd / parent-biodone'd. cnt fields: +0x02(word,pageio cnt) +0x54(long,gen cnt); flags long@+0x00,
            // B_DONE = bit1 of byte@+0x03; b_iodone@+0x48; pageio-parent@+0x4c; gen-parent@+0x50.
            for(int k=0;k<8;k++){ int i=(amix_bw_h-8+k)&7; uae_u32 b=amix_bw_buf[i]; if(!b) continue;
               uae_u32 f0=amix_kget2(b), iod=amix_kget2(b+0x48), pp=amix_kget2(b+0x4c), gp=amix_kget2(b+0x50), gc=amix_kget2(b+0x54), pc2=amix_kget2(b+0x00);
               int bdone=(f0&0x2)?1:0;   // B_DONE = bit1 of the +0x00 long
               int waspar=0,wasbio=0; for(int j=0;j<16;j++) if(amix_gio_par[j]==b) waspar=1; for(int j=0;j<16;j++) if(amix_pbio_buf[j]==b) wasbio=1;
               z3660_printf("[PARENT] buf=%08lX flags=%08lX B_DONE=%d b_iodone=%08lX pageioparent(+4c)=%08lX genparent(+50)=%08lX gencnt(+54)=%ld  was_gioparent=%d was_parentbiodone=%d\r\n",
                  (unsigned long)b,(unsigned long)f0,bdone,(unsigned long)iod,(unsigned long)pp,(unsigned long)gp,(long)(int32_t)gc,waspar,wasbio); (void)pc2; }
            // --- 030-PMMU demand-paging FAULT tracker (re-fault detector) ---
            { extern volatile uae_u32 amix_ftot,amix_fsamemax,amix_fsameva,amix_fring_va[16],amix_fring_pc[16],amix_fring_rw[16],amix_fhva[16],amix_fhcnt[16],amix_fhpc[16]; extern volatile int amix_fring_h;
              z3660_printf("[FAULT] total=%lu  max_consecutive_same_VA=%lu @VA=%08lX\r\n",(unsigned long)amix_ftot,(unsigned long)amix_fsamemax,(unsigned long)amix_fsameva);
              z3660_printf("[FAULT] last-16 fault VAs (the recent fault sequence -> re-fault if same VA repeats):\r\n");
              for(int k=0;k<16;k++){ int i=(amix_fring_h-16+k)&15; if(!amix_fring_va[i]&&!amix_fring_pc[i]) continue;
                 z3660_printf("   va=%08lX pc=%08lX %c\r\n",(unsigned long)amix_fring_va[i],(unsigned long)amix_fring_pc[i],amix_fring_rw[i]?'R':'W'); }
              z3660_printf("[FAULT] top faulting VAs by count:\r\n");
              for(int k=0;k<16;k++){ if(amix_fhcnt[k]>1) z3660_printf("   va=%08lX cnt=%lu pc=%08lX\r\n",(unsigned long)amix_fhva[k],(unsigned long)amix_fhcnt[k],(unsigned long)amix_fhpc[k]); } }
            z3660_printf("[DISPACT] hist: a0(COMPLETE)=%lu a1(BADHW)=%lu a2=%lu a3=%lu a4=%lu a5=%lu a6=%lu a7(DISC)=%lu a8=%lu a9=%lu  badhw_n=%lu@%lu\r\n",
               (unsigned long)amix_disp_acthist[0],(unsigned long)amix_disp_acthist[1],(unsigned long)amix_disp_acthist[2],
               (unsigned long)amix_disp_acthist[3],(unsigned long)amix_disp_acthist[4],(unsigned long)amix_disp_acthist[5],
               (unsigned long)amix_disp_acthist[6],(unsigned long)amix_disp_acthist[7],(unsigned long)amix_disp_acthist[8],
               (unsigned long)amix_disp_acthist[9],(unsigned long)amix_badhw_n,(unsigned long)amix_badhw_tick);
            for(int k=0;k<16;k++){ int i=(amix_disp_h-16+k)&15;
               z3660_printf("[DISP] csr=%02lX act=%lu tick=%lu\r\n",(unsigned long)amix_disp_csr[i],(unsigned long)amix_disp_act[i],(unsigned long)amix_disp_tick[i]); }
            a3000_scsi_dumpstate();   // why isn't the SCSI INT2 firing at the stall?
            a3000_scsi_dumpqueue();   // dump the guest a3091 queue (istate/curunitp/units6 chain) directly
            for(int k=0;k<16;k++){ int i=(amix_wcmd_head-16+k)&15;
               z3660_printf("[WCMD] cmd=%02lX dest=%lu ph=%02lX\r\n",(unsigned long)amix_wcmd_cmd[i],
                  (unsigned long)amix_wcmd_dest[i],(unsigned long)amix_wcmd_ph[i]); }
         }
      }
   }
#if INT_IPL_ON_THIS_CORE == 0
   if(shared->int_available)
   {
      shared->int_available=0;
      read_irq=shared->irq;
      // intlev() OR-s in the emulated A3000 SCSI level-2 (a3000_scsi_irq); gate on
      // the effective level so a freshly-asserted SCSI INT2 latches SPCFLAG_DOINT.
      int eff_irq=intlev();
      if(eff_irq>regs.intmask || eff_irq==7)
         set_special(SPCFLAG_DOINT);
   }
#else
   // intlev() OR-s in the emulated A3000 SCSI level-2 (a3000_scsi_irq); must gate on
   // the effective level, not the bare physical read_irq, or the SCSI INT2 never latches.
   int eff_irq=intlev();
   if(eff_irq>regs.intmask || eff_irq==7)
   {
      set_special(SPCFLAG_DOINT);
      if(pissoff_int!=0)
         regs.pissoff=pissoff_int;
   }
#endif
#if 1
   if(read_reset==0 && read_reset_last==1)
   {
//      printf("Reset!!!\n");
      usleep2(100000L); // this file renames Xilinx sleep.h usleep->usleep2 (see top); usleep2 backed in cpu_emulator.cpp
      do{
         uint32_t read1=*(volatile uint32_t*)(XPAR_PS7_GPIO_0_BASEADDR+XGPIOPS_DATA_RO_OFFSET);
         read_reset=(read1>>(n040RSTI   ))&1;
      }while(read_reset==0);
      uaecptr pc;
      uaecptr ksboot = 0xf80002 - 2;
      addrbank *ab;

      custom_reset_cpu(false, false);
      z3660_quiesce_real_chipset_on_reset();
      m68k_setpc_normal (ksboot);
      ovl=1;
      m68k_reset_newcpu(1);
//      cpu_emulator_reset_core0();
      reset_autoconfig();
//      init_m68k();
//      build_cpufunctbl();
      m68k_setpc_normal (regs.pc);
//      doint();
      fill_prefetch_quick ();
      set_cycles (start_cycles);
      regs.stopped=false;
   }
   read_reset_last=read_reset;
#endif
/*
   if (uae_int_requested) {
      bool irq = false;
      if (uae_int_requested & 0x00ff) {
         INTREQ_f(0x8000 | 0x0008);
         irq = true;
      }
      if (uae_int_requested & 0xff00) {
         INTREQ_f(0x8000 | 0x2000);
         irq = true;
      }
      if (uae_int_requested & 0xff0000) {
            atomic_and(&uae_int_requested, ~0x010000);
      }
      if (irq) {
         doint();
   }
*/
}
#if 0
void safe_interrupt_set(int num, int id, bool i6)
{
   if (!is_mainthread()) {
      set_special_exter(SPCFLAG_UAEINT);
      volatile uae_atomic *p;
      if (i6)
         p = &uae_interrupts6[num];
      else
         p = &uae_interrupts2[num];
      atomic_or(p, 1 << id);
      atomic_or(&uae_interrupt, 1);
   } else {
      int inum = i6 ? 13 : 3;
      uae_u16 v = 1 << inum;
      if (currprefs.cpu_cycle_exact || currprefs.cpu_compatible) {
         INTREQ_INT(inum, 0);
      } else if (!(intreq & v)) {
         INTREQ_0(0x8000 | v);
      }
   }
}
#endif
int cpu_sleep_millis(int ms)
{
//   return sleep_millis_main(ms);
   return(0);
}
#if 0
static bool haltloop_do(int vsynctimeline, frame_time_t rpt_end, int lines)
{
   int ovpos = vpos;
   while (lines-- >= 0) {
      ovpos = vpos;
      while (ovpos == vpos) {
         x_do_cycles(8 * CYCLE_UNIT);
         unset_special(SPCFLAG_UAEINT);
         check_uae_int_request();
#ifdef WITH_PPC
         ppc_interrupt(intlev());
         uae_ppc_execute_check();
#endif
         if (regs.spcflags & SPCFLAG_COPPER)
            do_copper();
         if (regs.spcflags & (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE)) {
            if (regs.spcflags & SPCFLAG_BRK) {
               unset_special(SPCFLAG_BRK);
   #ifdef DEBUGGER
               if (debugging)
                  debug();
   #endif
            }
            return true;
         }
      }

      // sync chipset with real time
      for (;;) {
         check_uae_int_request();
#ifdef WITH_PPC
         ppc_interrupt(intlev());
         uae_ppc_execute_check();
#endif
         if (event_wait)
            break;
         frame_time_t d = read_processor_time() - rpt_end;
         if (d < -2 * vsynctimeline || d >= 0)
            break;
      }
   }
   return false;
}
#endif
static bool haltloop(void)
{
#ifdef WITH_PPC
   if (regs.halted < 0) {
      int rpt_end = 0;
      int ovpos = vpos;

      while (regs.halted) {
         int vsynctimeline = (int)(vsynctimebase / (maxvpos_display + 1));
         int lines;
         frame_time_t rpt_scanline = read_processor_time();
         frame_time_t rpt_end = rpt_scanline + vsynctimeline;

         // See expansion handling.
         // Dialog must be opened from main thread.
         if (regs.halted == -2) {
            regs.halted = -1;
            notify_user (NUMSG_UAEBOOTROM_PPC);
         }

         if (currprefs.ppc_cpu_idle) {

            int maxlines = 100 - (currprefs.ppc_cpu_idle - 1) * 10;
            int i;

            event_wait = false;
            for (i = 0; i < ev_max; i++) {
               if (i == ev_hsync)
                  continue;
               if (i == ev_audio)
                  continue;
               if (!eventtab[i].active)
                  continue;
               if (eventtab[i].evtime - currcycle < maxlines * maxhpos * CYCLE_UNIT)
                  break;
            }
            if (currprefs.ppc_cpu_idle >= 10 || (i == ev_max && vpos > 0 && vpos < maxvpos - maxlines)) {
               cpu_sleep_millis(1);
            }
            check_uae_int_request();
            uae_ppc_execute_check();

            lines = (int)(read_processor_time() - rpt_scanline) / vsynctimeline + 1;

         } else {

            event_wait = true;
            lines = 0;

         }

         if (lines > maxvpos / 2)
            lines = maxvpos / 2;

         if (haltloop_do(vsynctimeline, rpt_end, lines))
            return true;

      }

   } else  {
#endif
      while (regs.halted) {
//         static int prevvpos;
//         if (vpos == 0 && prevvpos) {
//            prevvpos = 0;
//            cpu_sleep_millis(8);
//         }
//         if (vpos)
//            prevvpos = 1;
         x_do_cycles(8 * CYCLE_UNIT);

//         if (regs.spcflags & SPCFLAG_COPPER)
//            do_copper();

         if (regs.spcflags) {
            if ((regs.spcflags & (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE)))
               return true;
         }
      }
#ifdef WITH_PPC
   }
#endif

   return false;
}

#ifdef WITH_PPC
static bool uae_ppc_poll_check_halt(void)
{
   if (regs.halted) {
      if (haltloop())
         return true;
   }
   return false;
}
#endif


// handle interrupt delay (few cycles)
STATIC_INLINE bool time_for_interrupt (void)
{
   return regs.ipl > regs.intmask || regs.ipl == 7;
}

void doint(void)
{
#ifdef WITH_PPC
   if (ppc_state) {
      if (!ppc_interrupt(intlev()))
         return;
   }
#endif
   if (m68k_interrupt_delay) {
      int il = intlev();
      regs.ipl_pin = il;
      if (regs.ipl_pin > regs.intmask || regs.ipl_pin == 7)
         set_special(SPCFLAG_INT);
      return;
   }
   if (currprefs.cpu_compatible && currprefs.cpu_model < 68020)
      set_special (SPCFLAG_INT);
   else
      set_special (SPCFLAG_DOINT);
}

static int do_specialties (int cycles)
{

   if (regs.spcflags & SPCFLAG_MODE_CHANGE)
      return 1;

   if (regs.spcflags & SPCFLAG_CHECK) {
      if (regs.halted) {
         unset_special(SPCFLAG_CHECK);
         if (haltloop())
            return 1;
      }
      if (m68k_reset_delay) {
/*
         int vsynccnt = 60;
         int vsyncstate = -1;
         while (vsynccnt > 0 && !quit_program) {
            x_do_cycles(8 * CYCLE_UNIT);
            if (regs.spcflags & SPCFLAG_COPPER)
               do_copper();
            if (timeframes != vsyncstate) {
               vsyncstate = timeframes;
               vsynccnt--;
            }
         }
*/
      }
      m68k_reset_delay = 0;
      unset_special(SPCFLAG_CHECK);
   }

#ifdef ACTION_REPLAY
#ifdef ACTION_REPLAY_HRTMON
   if ((regs.spcflags & SPCFLAG_ACTION_REPLAY) && hrtmon_flag != ACTION_REPLAY_INACTIVE) {
      int isinhrt = (m68k_getpc () >= hrtmem_start && m68k_getpc () < hrtmem_start + hrtmem_size);
      /* exit from HRTMon? */
      if (hrtmon_flag == ACTION_REPLAY_ACTIVE && !isinhrt)
         hrtmon_hide ();
      /* HRTMon breakpoint? (not via IRQ7) */
      if (hrtmon_flag == ACTION_REPLAY_IDLE && isinhrt)
         hrtmon_breakenter ();
      if (hrtmon_flag == ACTION_REPLAY_ACTIVATE)
         hrtmon_enter ();
   }
#endif
   if ((regs.spcflags & SPCFLAG_ACTION_REPLAY) && action_replay_flag != ACTION_REPLAY_INACTIVE) {
      /*if (action_replay_flag == ACTION_REPLAY_ACTIVE && !is_ar_pc_in_rom ())*/
      /*   write_log (_T("PC:%p\n"), m68k_getpc ());*/

      if (action_replay_flag == ACTION_REPLAY_ACTIVATE || action_replay_flag == ACTION_REPLAY_DORESET)
         action_replay_enter();
      if ((action_replay_flag == ACTION_REPLAY_HIDE || action_replay_flag == ACTION_REPLAY_ACTIVE) && !is_ar_pc_in_rom ()) {
         action_replay_hide();
         unset_special (SPCFLAG_ACTION_REPLAY);
      }
      if (action_replay_flag == ACTION_REPLAY_WAIT_PC) {
         /*write_log (_T("Waiting for PC: %p, current PC= %p\n"), wait_for_pc, m68k_getpc ());*/
         if (m68k_getpc () == wait_for_pc) {
            action_replay_flag = ACTION_REPLAY_ACTIVATE; /* Activate after next instruction. */
         }
      }
   }
#endif

//   if (regs.spcflags & SPCFLAG_COPPER)
//      do_copper();

#ifdef JIT
   if (currprefs.cachesize) {
      unset_special(SPCFLAG_END_COMPILE);
   }
#endif
/*
   while ((regs.spcflags & SPCFLAG_BLTNASTY) && dmaen (DMA_BLITTER) && cycles > 0 && ((currprefs.waiting_blits && currprefs.cpu_model >= 68020) || !currprefs.blitter_cycle_exact)) {
      int c = blitnasty ();
      if (c < 0) {
         break;
      } else if (c > 0) {
         cycles -= c * CYCLE_UNIT * 2;
         if (cycles < CYCLE_UNIT)
            cycles = 0;
      } else {
         c = 4;
      }
      x_do_cycles (c * CYCLE_UNIT);
      if (regs.spcflags & SPCFLAG_COPPER)
         do_copper ();
#ifdef WITH_PPC
      if (ppc_state)  {
         if (uae_ppc_poll_check_halt())
            return true;
         uae_ppc_execute_check();
      }
#endif
   }
*/
   if (regs.spcflags & SPCFLAG_DOTRACE)
      Exception(9);

   if (regs.spcflags & SPCFLAG_TRAP) {
      unset_special (SPCFLAG_TRAP);
      Exception(3);
   }

   while (regs.spcflags & SPCFLAG_STOP) {

      if (regs.s == 0 && currprefs.cpu_model <= 68010) {
         // 68000/68010 undocumented special case:
         // if STOP clears S-bit and T was not set:
         // cause privilege violation exception, PC pointing to following instruction.
         // If T was set before STOP: STOP works as documented.
         m68k_unset_stop();
         Exception(8);
         break;
      }
      m68k_unset_stop();
   isstopped:
      check_uae_int_request();
/*
      {
         if (bsd_int_requested)
            bsdsock_fake_int_handler ();
      }

      if (cpu_tracer > 0) {
         cputrace.stopped = regs.stopped;
         cputrace.intmask = regs.intmask;
         cputrace.sr = regs.sr;
         cputrace.state = 1;
         cputrace.pc = m68k_getpc ();
         cputrace.memoryoffset = 0;
         cputrace.cyclecounter = cputrace.cyclecounter_pre = cputrace.cyclecounter_post = 0;
         cputrace.readcounter = cputrace.writecounter = 0;
      }

      if (m68k_interrupt_delay) {
         unset_special(SPCFLAG_INT);
         if (time_for_interrupt ()) {
            x_do_cycles(4 * cpucycleunit);
            do_interrupt (regs.ipl);
            break;
         }
      } else {
*/
         if (regs.spcflags & (SPCFLAG_INT | SPCFLAG_DOINT)) {
            int intr = intlev ();
            unset_special (SPCFLAG_INT | SPCFLAG_DOINT);
#ifdef WITH_PPC
            bool m68kint = true;
            if (ppc_state) {
               m68kint = ppc_interrupt(intr);
            }
            if (m68kint) {
#endif
               if (intr > 0 && intr > regs.intmask) {
                  do_interrupt(intr);
                  break;
               }
#ifdef WITH_PPC
            }
#endif
         }
//      }

      ipl_fetch();

//      x_do_cycles(4 * cpucycleunit);

//      if (regs.spcflags & SPCFLAG_COPPER) {
//         do_copper();
//      }

      if (regs.spcflags & SPCFLAG_MODE_CHANGE) {
         m68k_resumestopped();
         return 1;
      }

#ifdef WITH_PPC
      if (ppc_state) {
         uae_ppc_execute_check();
         uae_ppc_poll_check_halt();
      }
#endif

   }
/*
   if (regs.spcflags & SPCFLAG_TRACE)
      do_trace();

   if (regs.spcflags & SPCFLAG_UAEINT) {
      check_uae_int_request();
      unset_special(SPCFLAG_UAEINT);
   }

   if (m68k_interrupt_delay) {
      if (time_for_interrupt()) {
         unset_special(SPCFLAG_INT);
         do_interrupt(regs.ipl);
      }
   } else {
*/
      if (regs.spcflags & SPCFLAG_INT) {
         int intr = intlev ();
         unset_special (SPCFLAG_INT | SPCFLAG_DOINT);
         if (intr > 0 && (intr > regs.intmask || intr == 7))
            do_interrupt (intr);
      }
//   }

   if (regs.spcflags & SPCFLAG_DOINT) {
      unset_special (SPCFLAG_DOINT);
      set_special (SPCFLAG_INT);
   }

   if (regs.spcflags & SPCFLAG_BRK) {
      unset_special(SPCFLAG_BRK);
   }

   return 0;
}

#ifndef CPUEMU_11

static void m68k_run_1 (void)
{
}

#else

/* It's really sad to have two almost identical functions for this, but we
do it all for performance... :(
This version emulates 68000's prefetch "cache" */
static void m68k_run_1 (void)
{
   struct regstruct *r = &regs;
   bool exit = false;

   while (!exit) {
      TRY (prb) {
         while (!exit) {
            r->opcode = r->ir;

            r->instruction_pc = m68k_getpc ();
            cpu_cycles = (*cpufunctbl[r->opcode])(r->opcode);
            cpu_cycles = adjust_cycles (cpu_cycles);
            do_cycles(cpu_cycles);
            regs.instruction_cnt++;
            if (r->spcflags) {
               if (do_specialties (cpu_cycles))
                  exit = true;
            }
            regs.ipl = regs.ipl_pin;
            if (!currprefs.cpu_compatible || (currprefs.cpu_cycle_exact && currprefs.cpu_model <= 68010))
               exit = true;
         }
      } CATCH (prb) {
         bus_error();
         if (r->spcflags) {
            if (do_specialties(cpu_cycles))
               exit = true;
         }
         regs.ipl = regs.ipl_pin;
      } ENDTRY
   }
}

#endif /* CPUEMU_11 */

#ifndef CPUEMU_13

static void m68k_run_1_ce (void)
{
}

#else

/* cycle-exact m68k_run () */

static void m68k_run_1_ce (void)
{
   struct regstruct *r = &regs;
   bool first = true;
   bool exit = false;

   while (!exit) {
      TRY (prb) {
         if (first) {
            if (cpu_tracer < 0) {
               memcpy (&r->regs, &cputrace.regs, 16 * sizeof (uae_u32));
               r->ir = cputrace.ir;
               r->irc = cputrace.irc;
               r->sr = cputrace.sr;
               r->usp = cputrace.usp;
               r->isp = cputrace.isp;
               r->intmask = cputrace.intmask;
               r->stopped = cputrace.stopped;
               r->read_buffer = cputrace.read_buffer;
               r->write_buffer = cputrace.write_buffer;
               m68k_setpc (cputrace.pc);
               if (!r->stopped) {
                  if (cputrace.state > 1) {
                     write_log (_T("CPU TRACE: EXCEPTION %d\n"), cputrace.state);
                     Exception (cputrace.state);
                  } else if (cputrace.state == 1) {
                     write_log (_T("CPU TRACE: %04X\n"), cputrace.opcode);
                     (*cpufunctbl[cputrace.opcode])(cputrace.opcode);
                  }
               } else {
                  write_log (_T("CPU TRACE: STOPPED\n"));
               }
               if (r->stopped)
                  set_special (SPCFLAG_STOP);
               set_cpu_tracer (false);
               goto cont;
            }
            set_cpu_tracer (false);
            first = false;
         }

         while (!exit) {
            r->opcode = r->ir;

            if (cpu_tracer) {
               memcpy (&cputrace.regs, &r->regs, 16 * sizeof (uae_u32));
               cputrace.opcode = r->opcode;
               cputrace.ir = r->ir;
               cputrace.irc = r->irc;
               cputrace.sr = r->sr;
               cputrace.usp = r->usp;
               cputrace.isp = r->isp;
               cputrace.intmask = r->intmask;
               cputrace.stopped = r->stopped;
               cputrace.read_buffer = r->read_buffer;
               cputrace.write_buffer = r->write_buffer;
               cputrace.state = 1;
               cputrace.pc = m68k_getpc ();
               cputrace.startcycles = get_cycles ();
               cputrace.memoryoffset = 0;
               cputrace.cyclecounter = cputrace.cyclecounter_pre = cputrace.cyclecounter_post = 0;
               cputrace.readcounter = cputrace.writecounter = 0;
            }

//            if (inputrecord_debug & 4) {
//               if (input_record > 0)
//                  inprec_recorddebug_cpu(1, r->opcode);
//               else if (input_play > 0)
//                  inprec_playdebug_cpu(1, r->opcode);
//            }

#ifdef DEBUGGER
            if (debug_opcode_watch) {
               debug_trainer_match();
            }
#endif

            r->instruction_pc = m68k_getpc ();
#ifdef DEBUGGER
            if (debug_dma) {
               record_dma_event_data(DMA_EVENT_CPUINS, current_hpos(), vpos, r->opcode);
            }
#endif

            (*cpufunctbl[r->opcode])(r->opcode);
            regs.instruction_cnt++;
            if (cpu_tracer) {
               cputrace.state = 0;
            }
cont:
            if (cputrace.needendcycles) {
               cputrace.needendcycles = 0;
               write_log(_T("STARTCYCLES=%08x ENDCYCLES=%08x\n"), cputrace.startcycles, get_cycles());
            }

            if (r->spcflags) {
               if (do_specialties (0))
                  exit = true;
            }

            if (!currprefs.cpu_cycle_exact || currprefs.cpu_model > 68010)
               exit = true;
         }
      } CATCH (prb) {
         bus_error();
         if (r->spcflags) {
            if (do_specialties(0))
               exit = true;
         }
      } ENDTRY
   }
}

#endif

#ifdef WITH_THREADED_CPU
static volatile int cpu_thread_active;
static uae_sem_t cpu_in_sema, cpu_out_sema, cpu_wakeup_sema;

static volatile int cpu_thread_ilvl;
static volatile uae_u32 cpu_thread_indirect_mode;
static volatile uae_u32 cpu_thread_indirect_addr;
static volatile uae_u32 cpu_thread_indirect_val;
static volatile uae_u32 cpu_thread_indirect_size;
static volatile uae_u32 cpu_thread_reset;
static SDL_Thread* cpu_thread;
static SDL_threadID cpu_thread_tid;

static bool m68k_cs_initialized;

static int do_specialties_thread(void)
{
   if (regs.spcflags & SPCFLAG_MODE_CHANGE)
      return 1;

#ifdef JIT
   if (currprefs.cachesize) {
      unset_special(SPCFLAG_END_COMPILE);
   }
#endif

   if (regs.spcflags & SPCFLAG_DOTRACE)
      Exception(9);

   if (regs.spcflags & SPCFLAG_TRAP) {
      unset_special(SPCFLAG_TRAP);
      Exception(3);
   }

   if (regs.spcflags & SPCFLAG_TRACE)
      do_trace();

   for (;;) {

      if (regs.spcflags & (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE)) {
         return 1;
      }

      int ilvl = cpu_thread_ilvl;
      if (ilvl > 0 && (ilvl > regs.intmask || ilvl == 7)) {
         do_interrupt(ilvl);
      }

      if (!(regs.spcflags & SPCFLAG_STOP))
         break;

      uae_sem_wait(&cpu_wakeup_sema);
   }

   return 0;
}

static void init_cpu_thread(void)
{
   if (!currprefs.cpu_thread)
      return;
   if (m68k_cs_initialized)
      return;
   uae_sem_init(&cpu_in_sema, 0, 0);
   uae_sem_init(&cpu_out_sema, 0, 0);
   uae_sem_init(&cpu_wakeup_sema, 0, 0);
   m68k_cs_initialized = true;
}

extern addrbank *thread_mem_banks[MEMORY_BANKS];

uae_u32 process_cpu_indirect_memory_read(uae_u32 addr, int size)
{
   // Do direct access if call is from filesystem etc thread 
   if (cpu_thread_tid != uae_thread_get_id(nullptr)) {
      uae_u32 data = 0;
      addrbank *ab = thread_mem_banks[bankindex(addr)];
      switch (size)
      {
      case 0:
         data = ab->bget(addr) & 0xff;
         break;
      case 1:
         data = ab->wget(addr) & 0xffff;
         break;
      case 2:
         data = ab->lget(addr);
         break;
      }
      return data;
   }

   cpu_thread_indirect_mode = 2;
   cpu_thread_indirect_addr = addr;
   cpu_thread_indirect_size = size;
   uae_sem_post(&cpu_out_sema);
   uae_sem_wait(&cpu_in_sema);
   cpu_thread_indirect_mode = 0xfe;
   return cpu_thread_indirect_val;
}

void process_cpu_indirect_memory_write(uae_u32 addr, uae_u32 data, int size)
{
   if (cpu_thread_tid != uae_thread_get_id(nullptr)) {
      addrbank *ab = thread_mem_banks[bankindex(addr)];
      switch (size)
      {
      case 0:
         ab->bput(addr, data & 0xff);
         break;
      case 1:
         ab->wput(addr, data & 0xffff);
         break;
      case 2:
         ab->lput(addr, data);
         break;
      }
      return;
   }
   cpu_thread_indirect_mode = 1;
   cpu_thread_indirect_addr = addr;
   cpu_thread_indirect_size = size;
   cpu_thread_indirect_val = data;
   uae_sem_post(&cpu_out_sema);
   uae_sem_wait(&cpu_in_sema);
   cpu_thread_indirect_mode = 0xff;
}

static void run_cpu_thread(int (*f)(void*))
{
   int framecnt = -1;
   int vp = 0;
   int intlev_prev = 0;

   cpu_thread_active = 0;
   uae_sem_init(&cpu_in_sema, 0, 0);
   uae_sem_init(&cpu_out_sema, 0, 0);
   uae_sem_init(&cpu_wakeup_sema, 0, 0);

   if (!uae_start_thread(_T("cpu"), f, NULL, &cpu_thread))
      return;
   while (!cpu_thread_active) {
      sleep_millis(1);
   }

   while (!(regs.spcflags & SPCFLAG_MODE_CHANGE)) {
      int maxperloop = 10;

      while (!uae_sem_trywait(&cpu_out_sema)) {
         uae_u32 addr, data, size, mode;

         addr = cpu_thread_indirect_addr;
         data = cpu_thread_indirect_val;
         size = cpu_thread_indirect_size;
         mode = cpu_thread_indirect_mode;

         switch (mode)
         {
         case 1:
         {
            addrbank* ab = thread_mem_banks[bankindex(addr)];
            switch (size)
            {
            case 0:
               ab->bput(addr, data & 0xff);
               break;
            case 1:
               ab->wput(addr, data & 0xffff);
               break;
            case 2:
               ab->lput(addr, data);
               break;
            }
            uae_sem_post(&cpu_in_sema);
            break;
         }
         case 2:
         {
            addrbank* ab = thread_mem_banks[bankindex(addr)];
            switch (size)
            {
            case 0:
               data = ab->bget(addr) & 0xff;
               break;
            case 1:
               data = ab->wget(addr) & 0xffff;
               break;
            case 2:
               data = ab->lget(addr);
               break;
            }
            cpu_thread_indirect_val = data;
            uae_sem_post(&cpu_in_sema);
            break;
         }
         default:
            write_log(_T("cpu_thread_indirect_mode=%08x!\n"), mode);
            break;
         }

         if (maxperloop-- < 0)
            break;
      }

      if (framecnt != timeframes) {
         framecnt = timeframes;
      }

      if (cpu_thread_reset) {
         bool hardreset = cpu_thread_reset & 2;
         bool keyboardreset = cpu_thread_reset & 4;
         custom_reset(hardreset, keyboardreset);
         cpu_thread_reset = 0;
         uae_sem_post(&cpu_in_sema);
      }

      if (regs.spcflags & SPCFLAG_BRK) {
         unset_special(SPCFLAG_BRK);
#ifdef DEBUGGER
         if (debugging) {
            debug();
         }
#endif
      }

      if (vp == vpos) {

         do_cycles((maxhpos / 2) * CYCLE_UNIT);

         if (regs.spcflags & SPCFLAG_COPPER) {
            do_copper();
         }

         check_uae_int_request();
         if (regs.spcflags & (SPCFLAG_INT | SPCFLAG_DOINT)) {
            int intr = intlev();
            unset_special(SPCFLAG_INT | SPCFLAG_DOINT);
            if (intr > 0) {
               cpu_thread_ilvl = intr;
               cycles_do_special();
               uae_sem_post(&cpu_wakeup_sema);
            } else {
               cpu_thread_ilvl = 0;
            }
         }
         continue;
      }

      frame_time_t next = vsyncmintimepre + (vsynctimebase * vpos / (maxvpos + 1));
      frame_time_t c = read_processor_time();
      if (next - c > 0 && next - c < vsyncmaxtime * 2)
         continue;

      vp = vpos;

   }

   while (cpu_thread_active) {
      uae_sem_post(&cpu_in_sema);
      uae_sem_post(&cpu_wakeup_sema);
      sleep_millis(1);
   }

}

#endif

void custom_reset_cpu(bool hardreset, bool keyboardreset)
{
#ifdef WITH_THREADED_CPU
   if (cpu_thread_tid != uae_thread_get_id(nullptr)) {
      custom_reset(hardreset, keyboardreset);
      return;
   }
   cpu_thread_reset = 1 | (hardreset ? 2 : 0) | (keyboardreset ? 4 : 0);
   uae_sem_post(&cpu_wakeup_sema);
   uae_sem_wait(&cpu_in_sema);
#else
//   custom_reset(hardreset, keyboardreset);
   printf("custom_reset\n");
#endif
}

#ifdef JIT  /* Completely different run_2 replacement */

void execute_exception(uae_u32 cycles)
{
   countdown -= cycles;
   Exception_cpu(regs.jit_exception);
   regs.jit_exception = 0;
//   cpu_cycles = adjust_cycles(4 * CYCLE_UNIT / 2);
//   do_cycles(cpu_cycles);
   // after leaving this function, we fall back to execute_normal()
}

void do_nothing (void)
{
//   if (!currprefs.cpu_thread) {
      /* What did you expect this to do? */
//      do_cycles (0);
      /* I bet you didn't expect *that* ;-) */
//   }
}

static uae_u32 get_jit_opcode(void)
{
   uae_u32 opcode;
   opcode = get_diword(0);
   return opcode;
}

void exec_nostats (void)
{
   struct regstruct *r = &regs;

   for (;;)
   {
      r->opcode = get_jit_opcode();

      cpu_cycles = (*cpufunctbl[r->opcode])(r->opcode);

      cpu_cycles = adjust_cycles(cpu_cycles);

//      if (!currprefs.cpu_thread) {
//         do_cycles (cpu_cycles);

#ifdef WITH_PPC
         if (ppc_state)
            ppc_interrupt(intlev());
#endif
//      }

      check_uae_int_request();

      if (end_block(r->opcode) || r->spcflags || uae_int_requested)
         return; /* We will deal with the spcflags in the caller */
   }
}
void execute_normal(void)
{
   struct regstruct *r = &regs;
   int blocklen;
   cpu_history pc_hist[MAXRUN];
   int total_cycles;

   if (check_for_cache_miss ())
      return;

   total_cycles = 0;
   blocklen = 0;
   start_pc_p = r->pc_oldp;
   start_pc = r->pc;
   for (;;) {
      /* Take note: This is the do-it-normal loop */
      r->opcode = get_jit_opcode();

      special_mem = DISTRUST_CONSISTENT_MEM;
      pc_hist[blocklen].location = (uae_u16*)r->pc_p;
      cpu_cycles = (*cpufunctbl[r->opcode])(r->opcode);

      cpu_cycles = adjust_cycles(cpu_cycles);
//      if (!currprefs.cpu_thread) {
//         do_cycles (cpu_cycles);
//      }
      total_cycles += cpu_cycles;

      check_uae_int_request();

      pc_hist[blocklen].specmem = special_mem;
      blocklen++;
      if (end_block (r->opcode) || blocklen >= MAXRUN || r->spcflags || uae_int_requested) {
         compile_block (pc_hist, blocklen, total_cycles);
         return; /* We will deal with the spcflags in the caller */
      }
      /* No need to check regs.spcflags, because if they were set,
         we'd have ended up inside that "if" */
   }
}

typedef void compiled_handler (void);

#ifdef WITH_THREADED_CPU
static int cpu_thread_run_jit(void *v)
{
   cpu_thread_tid = uae_thread_get_id(cpu_thread);
   cpu_thread_active = 1;
#ifdef USE_STRUCTURED_EXCEPTION_HANDLING
   __try
#endif
   {
      for (;;) {
         ((compiled_handler*)(pushall_call_handler))();
         /* Whenever we return from that, we should check spcflags */
         if (regs.spcflags || cpu_thread_ilvl > 0) {
            if (do_specialties_thread()) {
               break;
            }
         }
      }
   }
#ifdef USE_STRUCTURED_EXCEPTION_HANDLING
#ifdef JIT
   __except (EvalException(GetExceptionInformation()))
#else
   __except (DummyException(GetExceptionInformation(), GetExceptionCode()))
#endif
   {
      // EvalException does the good stuff...
   }
#endif
   cpu_thread_active = 0;
   return 0;
}
#endif

static void m68k_run_jit(void)
{
#ifdef WITH_THREADED_CPU
   if (currprefs.cpu_thread) {
      run_cpu_thread(cpu_thread_run_jit);
      return;
   }
#endif

   for (;;) {
      ((compiled_handler*)(pushall_call_handler))();

      /* Whenever we return from that, we should check spcflags */
      check_uae_int_request();
      if (regs.spcflags) {
         if (do_specialties(0)) {
            return;
         }
      }
      // If T0, T1 or M got set: run normal emulation loop
      if (regs.t0 || regs.t1 || regs.m) {
         flush_icache(3);
         struct regstruct* r = &regs;
         bool exit = false;
         while (!exit && (regs.t0 || regs.t1 || regs.m)) {
            r->instruction_pc = m68k_getpc();
            r->opcode = x_get_iword(0);
            (*cpufunctbl[r->opcode])(r->opcode);
            do_cycles(4 * CYCLE_UNIT);
            if (r->spcflags) {
               if (do_specialties(cpu_cycles))
                  exit = true;
            }
         }
         unset_special(SPCFLAG_END_COMPILE);
      }
   }
}
#endif /* JIT */

static void check_halt(void)
{
   if (regs.halted)
      do_specialties (0);
}

void cpu_inreset(void)
{
   regs.s = 1;
   regs.intmask = 7;
   MakeSR();
}

void cpu_halt (int id)
{
   // id < 0: m68k halted, PPC active.
   // id > 0: emulation halted.
   if (!regs.halted) {
      write_log (_T("CPU halted: reason = %d PC=%08x\n"), id, M68K_GETPC);
      if (currprefs.crash_auto_reset) {
         write_log(_T("Forcing hard reset\n"));
//         uae_reset(true, false);
//         quit_program = -quit_program;
         set_special(SPCFLAG_BRK | SPCFLAG_MODE_CHANGE);
         cpureset();
         return;
      }
      regs.halted = id;
//      gui_data.cpu_halted = id;
//      gui_led(LED_CPU, 0, -1);
      if (id >= 0) {
         regs.intmask = 7;
         MakeSR ();
//         audio_deactivate ();
         //if (debugging)
         //   activate_debugger();
      }
   }
   set_special(SPCFLAG_CHECK);
}

#ifdef WITH_THREADED_CPU
static int cpu_thread_run_2(void *v)
{
   bool exit = false;
   struct regstruct *r = &regs;

   cpu_thread_tid = uae_thread_get_id(cpu_thread);

   cpu_thread_active = 1;
   while (!exit) {
      TRY(prb)
      {
         while (!exit) {
            r->instruction_pc = m68k_getpc();

            r->opcode = x_get_iword(0);

            (*cpufunctbl[r->opcode])(r->opcode);

            if (regs.spcflags || cpu_thread_ilvl > 0) {
               if (do_specialties_thread())
                  exit = true;
            }

         }
      } CATCH(prb)
      {
         bus_error();
         if (r->spcflags) {
            if (do_specialties_thread())
               exit = true;
         }
      } ENDTRY
   }
   cpu_thread_active = 0;
   return 0;
}
#endif

/* Same thing, but don't use prefetch to get opcode.  */
static void m68k_run_2_000(void)
{
   struct regstruct *r = &regs;
   bool exit = false;

   while (!exit) {
      TRY(prb) {
         while (!exit) {
            r->instruction_pc = m68k_getpc ();

            r->opcode = get_diword(0);

            cpu_cycles = (*cpufunctbl[r->opcode])(r->opcode);
            cpu_cycles = adjust_cycles (cpu_cycles);
            do_cycles(cpu_cycles);

            check_uae_int_request();

            if (r->spcflags) {
               if (do_specialties (cpu_cycles))
                  exit = true;
            }
         }
      } CATCH(prb) {
         bus_error();
         if (r->spcflags) {
            if (do_specialties(cpu_cycles))
               exit = true;
         }
      } ENDTRY
   }
}

static void m68k_run_2_020(void)
{
#ifdef WITH_THREADED_CPU
   if (currprefs.cpu_thread) {
      run_cpu_thread(cpu_thread_run_2);
      return;
   }
#endif

   struct regstruct *r = &regs;
   bool exit = false;

   while (!exit) {
      TRY(prb) {
         while (!exit) {
            r->instruction_pc = m68k_getpc();
            r->opcode = get_diword(0);
            cpu_cycles = (*cpufunctbl[r->opcode])(r->opcode);
            cpu_cycles = adjust_cycles(cpu_cycles);
            do_cycles(cpu_cycles);

            check_uae_int_request();

            if (r->spcflags) {
               if (do_specialties(cpu_cycles))
                  exit = true;
            }
         }
      } CATCH(prb) {
         bus_error();
         if (r->spcflags) {
            if (do_specialties(cpu_cycles))
               exit = true;
         }
      } ENDTRY
   }
}

// UAE_030_MMU run loop. Fetches opcodes through the MMU (x_prefetch =
// get_iword_mmu030) and dispatches the fault-restartable cpuemu_32 handlers
// (op_smalltbl_32_ff). Adapted from WinUAE 4.4.0 m68k_run_mmu030 for the Z3660
// (non-cycle-exact, non-compatible). On a page fault the cpummu030 engine THROWs;
// we restore flags, build the 030 bus-error frame via Exception() and resume.
// Instruction-START pc, saved each instruction before any handler runs. Several cpuemu_32 write handlers
// (MOVES and regular (An)+/-(An) moves) advance the pc and overwrite regs.instruction_pc BEFORE the
// faulting store; on a demand-page fault the CATCH below must build the 030 bus-error frame with the
// instruction-START pc so the RTE/re-run restarts the whole instruction. Read-side and prefetch faults
// already leave regs.instruction_pc at the start, so restoring it is a no-op there (no regression).
static uaecptr mmu030_insn_start_pc;

// ===== wip-030-mmu-buserror: catch the corruptor of the AMIX user-PC wild-jump =====
// Symptom: under a fork/exec storm a freshly-exec'd USER process (AMIX user base 0x80000000;
// e.g. in.telnetd entry 0x80001520) has its PC silently set to a kernel/low-region address
// (observed CONSTANT 0x080012A0); the next instruction fetch faults and AMIX prints
// "User BUS ERROR at <pc>, PC:<pc> FAULT:6". The corruption is UPSTREAM of the fault -- some
// already-retired instruction (rts/jmp/jsr/rte/movem) wrote the bad value into the user PC.
// Catch it: ring-buffer the last AMIX_RRING retired instructions and, the instant we are about
// to fetch from a wild USER pc, dump the ring + register file ONCE. AMIX-gated, ~5 stores/insn,
// one-shot serial -> negligible timing shift (and the bug repros ~100% under the storm anyway).
extern "C" { extern volatile int amix_mmu_on; }
// RTE-frame ring captured in cpummu030.cpp's m68k_do_rte_mmu030 (see there).
extern "C" {
extern volatile uae_u32 amix_rte_a7[16], amix_rte_pc[16], amix_rte_oc[16], amix_rte_fault[16];
extern volatile uae_u32 amix_rte_ssw[16], amix_rte_frame[16], amix_rte_h;
extern volatile int amix_wild_rte_pending; extern volatile uae_u32 amix_wild_rte_pc;
}
#define AMIX_RRING 32
static uae_u32 amix_rring_pc[AMIX_RRING];   // retired instruction's start PC
static uae_u16 amix_rring_op[AMIX_RRING];   // its opcode word
static uae_u32 amix_rring_npc[AMIX_RRING];  // PC after it retired (= branch target for control transfers)
static uae_u8  amix_rring_s[AMIX_RRING];    // supervisor flag at retire
static uae_u32 amix_rring_h = 0;            // ring head (next slot to write)
static int     amix_wild_latched = 0;       // one-shot dump guard

static void amix_dump_wild(uae_u32 wildpc)
{
   z3660_printf("\r\n[WILD] AMIX user PC went wild: PC=%08lX  (user-mode ifetch below user-base 0x80000000 -> jumped into kernel/low region)\r\n",
                (unsigned long)wildpc);
   z3660_printf("[WILD] D0-7: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n",
      (unsigned long)regs.regs[0],(unsigned long)regs.regs[1],(unsigned long)regs.regs[2],(unsigned long)regs.regs[3],
      (unsigned long)regs.regs[4],(unsigned long)regs.regs[5],(unsigned long)regs.regs[6],(unsigned long)regs.regs[7]);
   z3660_printf("[WILD] A0-7: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n",
      (unsigned long)regs.regs[8],(unsigned long)regs.regs[9],(unsigned long)regs.regs[10],(unsigned long)regs.regs[11],
      (unsigned long)regs.regs[12],(unsigned long)regs.regs[13],(unsigned long)regs.regs[14],(unsigned long)regs.regs[15]);
   z3660_printf("[WILD] usp=%08lX isp=%08lX  s=%d intmask=%d sfc=%lu dfc=%lu vbr=%08lX\r\n",
      (unsigned long)regs.usp,(unsigned long)regs.isp,(int)regs.s,(int)regs.intmask,
      (unsigned long)regs.sfc,(unsigned long)regs.dfc,(unsigned long)regs.vbr);
   z3660_printf("[WILD] last %d retired insns (oldest first):  startpc   op    -> nextpc    [s]\r\n", AMIX_RRING);
   for (int i = 0; i < AMIX_RRING; i++) {
      uae_u32 idx = (amix_rring_h + (uae_u32)i) & (AMIX_RRING - 1);
      if (amix_rring_pc[idx] == 0 && amix_rring_npc[idx] == 0) continue;   // unused slot
      z3660_printf("[WILD]  %08lX  %04X  -> %08lX  [%d]%s\r\n",
         (unsigned long)amix_rring_pc[idx], (unsigned)amix_rring_op[idx], (unsigned long)amix_rring_npc[idx],
         (int)amix_rring_s[idx], (amix_rring_npc[idx] == wildpc) ? "   <== CORRUPTOR" : "");
   }
   z3660_printf("[WILD] last 16 RTE-frame resumes (oldest first):  a7        pc        frame oc        ssw   fault\r\n");
   for (int i = 0; i < 16; i++) {
      uae_u32 idx = (amix_rte_h + (uae_u32)i) & 15;
      if (amix_rte_a7[idx] == 0 && amix_rte_pc[idx] == 0) continue;
      z3660_printf("[WILD]  %08lX  %08lX  %04lX  %08lX  %04lX  %08lX%s\r\n",
         (unsigned long)amix_rte_a7[idx], (unsigned long)amix_rte_pc[idx], (unsigned long)amix_rte_frame[idx],
         (unsigned long)amix_rte_oc[idx], (unsigned long)amix_rte_ssw[idx], (unsigned long)amix_rte_fault[idx],
         (amix_rte_pc[idx] == wildpc) ? "   <== popped the wild PC" : "");
   }
   z3660_printf("[WILD] (one-shot latched; resets on reboot)\r\n\r\n");
}

// ===== perf investigation (wip-emu-030-perf): instruction-rate benchmark + tunable poll cadence =====
#include "xtime_l.h"   // ARM global timer (XTime / COUNTS_PER_SECOND); same header a3000_scsi.cpp uses
static int     z3660_service_cadence = 1; // instructions between check_uae_int_request() polls; synced from shared->service_cadence
static uae_u64 z3660_perf_count      = 0; // instructions retired in m68k_run_mmu030 (benchmark accumulator)
static void z3660_perf_tick(void)         // called ~every 1M instructions from the run loop
{
   int c = (int)shared->service_cadence; if(c < 1) c = 1; z3660_service_cadence = c;   // pick up the runtime knob (SERV)
   static uae_u64 lastcnt = 0; static XTime last = 0;
   XTime now; XTime_GetTime(&now);
   XTime el = now - last;
   if(el >= (XTime)COUNTS_PER_SECOND){           // ~1 Hz window
      if(shared->perf_report && last != 0){
         uae_u64 di = z3660_perf_count - lastcnt;
         uint32_t kips = (uint32_t)(di * (uae_u64)COUNTS_PER_SECOND / (uae_u64)el / 1000u);
         z3660_printf("[PERF] ~%lu kIPS (uncalibrated, use as relative) cadence=%d\r\n",(unsigned long)kips, z3660_service_cadence);
      }
      lastcnt = z3660_perf_count; last = now;    // keep the window fresh even when reporting is off
   }
}

static void m68k_run_mmu030(void)
{
   struct flag_struct f;
   int halt = 0;

   mmu030_opcode_stageb = -1;
   mmu030_fake_prefetch = -1;
   while (!halt) {
      TRY(prb) {
         for (;;) {
            int cnt;
insretry:
            regs.instruction_pc = m68k_getpc();
            mmu030_insn_start_pc = regs.instruction_pc;   // snapshot before any handler can mis-advance it
            // wip-030-mmu-buserror: about to fetch from a wild USER pc (user mode + PC below the AMIX
            // user base 0x80000000 = jumped into the kernel/low region). Dump the corruptor once,
            // BEFORE the fetch faults, so the retired-insn ring still holds the instruction that did it.
            if (amix_mmu_on && !regs.s && regs.instruction_pc < 0x80000000u && !amix_wild_latched) {
               amix_wild_latched = 1;
               amix_dump_wild((uae_u32)regs.instruction_pc);
            }
            // RTE-source detector (any target mode): an RTE popped a wild PC -> dump at the source.
            if (amix_wild_rte_pending && !amix_wild_latched) {
               amix_wild_latched = 1;
               amix_dump_wild(amix_wild_rte_pc);
            }
            amix_wild_rte_pending = 0;
            f = regs.ccrflags;

            mmu030_state[0] = mmu030_state[1] = mmu030_state[2] = 0;
            mmu030_opcode = -1;
            if (mmu030_opcode_stageb < 0) {
               regs.opcode = x_prefetch(0);
            } else {
               regs.opcode = mmu030_opcode_stageb;
               mmu030_opcode_stageb = -1;
            }
            mmu030_opcode = regs.opcode;
            mmu030_idx_done = 0;

            cnt = 50;
            for (;;) {
               regs.opcode = regs.irc = mmu030_opcode;
               mmu030_idx = 0;
               mmu030_retry = false;

               count_instr(regs.opcode);
               do_cycles(cpu_cycles);
               cpu_cycles = (*cpufunctbl[regs.opcode])(regs.opcode);

               cnt--; // don't loop forever if things go wrong
               if (!mmu030_retry)
                  break;
               if (cnt < 0) {
                  cpu_halt(CPU_HALT_CPU_STUCK);
                  break;
               }
               if (mmu030_retry && mmu030_opcode == -1)
                  goto insretry;
               // FIX (wip-030-mmu-buserror): a continuation resume — m68k_do_rte_mmu030 replaying a
               // faulted instruction from its 030 bus-error frame (frame $A/$B), or a sub-access
               // continue — re-runs the instruction at its OWN pc, which m68k_setpci(pc) has just set.
               // Re-snapshot the instruction-start pc here so that if the RESUMED instruction faults
               // AGAIN (e.g. MOVEM.L <regs>,-(SP) crossing into the next not-yet-resident user-stack
               // page) the CATCH below builds the new bus-error frame from the resumed instruction's
               // pc — NOT the stale outer-loop pc (the kernel's RTE epilogue). With the stale pc, the
               // rebuilt frame carried a kernel pc; the next RTE then resumed the user process there in
               // user mode -> wild PC -> "User BUS ERROR" (cron/in.telnetd etc.). For the common
               // same-pc sub-access continue this is a no-op (m68k_getpc() == the current snapshot).
               regs.instruction_pc = mmu030_insn_start_pc = m68k_getpc();
            }

            // wip-030-mmu-buserror: record this just-retired instruction (start pc, opcode, and the
            // PC it left behind = branch target for control transfers) so the wild-PC detector above
            // can show what corrupted the user PC. AMIX-gated; mmu030_opcode still holds the opcode here.
            if (amix_mmu_on) {
               uae_u32 h = amix_rring_h & (AMIX_RRING - 1);
               amix_rring_pc[h]  = (uae_u32)mmu030_insn_start_pc;
               amix_rring_op[h]  = (uae_u16)mmu030_opcode;
               amix_rring_npc[h] = (uae_u32)m68k_getpc();
               amix_rring_s[h]   = (uae_u8)regs.s;
               amix_rring_h++;
            }

            mmu030_opcode = -1;
            cpu_cycles = adjust_cycles(cpu_cycles);
            // perf investigation: instruction-rate benchmark + tunable IPL/cross-core poll cadence.
            // do_specialties() stays per-instruction (STOP/trace/mode-change correctness, and its
            // STOP loop polls internally); only check_uae_int_request (which DETECTS interrupts) is
            // throttled to every z3660_service_cadence instructions -> max ~N-instruction int latency.
            z3660_perf_count++;
            if((z3660_perf_count & 0xFFFFFu) == 0) z3660_perf_tick();
            { static int serv_ctr = 0;
              if(++serv_ctr >= z3660_service_cadence){ serv_ctr = 0; check_uae_int_request(); } }
            if (regs.spcflags) {
               if (do_specialties(cpu_cycles))
                  return;
            }
         }
      } CATCH(prb) {
         bool lastwrite_norestart = false;
         if (mmu030_opcode == -1) {
            // fault during opcode prefetch
            mmufixup[0].reg = -1;
            mmufixup[1].reg = -1;
         } else if (mmu030_state[1] & MMU030_STATEFLAG1_LASTWRITE) {
            // Frame-$A (last-write) fault. Distinguish by whether the instruction's handler already
            // ADVANCED the PC past itself before the faulting store (2026-06-15 gated fix, audit-refined):
            //  * Handler ADVANCED the PC (regs.instruction_pc != insn-start): every RMW + plain-store
            //    handler does `regs.instruction_pc = m68k_getpci()` before the put. The single buffered
            //    store is replayed by m68k_do_rte_mmu030 and execution resumes at the NEXT instruction --
            //    do NOT restart. Re-executing would re-READ a just-replayed value and DOUBLE a
            //    read-modify-write: this is the addq #1,abs (ttymon counter 0->2 -> getty SIGBUS) AND
            //    the addq #1,(a0)+ / bset #n,(a0)+ etc. case (audit "rmw-with-an"/"misaligned", HIGH).
            //    Any (An)+/-(An) the handler applied STAYS applied (correct; no rollback) -- the store
            //    replays to the saved fault address.
            //  * Handler did NOT advance the PC (== insn-start): the fork's MOVES (An)/(An)+ move
            //    handlers leave PC at the start. These RESTART the whole instruction, so roll the
            //    auto-modified An back to its pre-increment value first -- else the re-run writes the
            //    source longword to dest+size, DUPLICATING it (init icode copyout -> execve EFAULT ->
            //    hang at 0x80800010). Guard the 2nd rollback against a same-register dual-autoinc move
            //    (move (a0)+,(a0)+): mmufixup[0] holds the TRUE pre-instruction value (audit
            //    "an-moves-regular", MEDIUM). The MOVES opcode (0x0Exx) is forced to restart belt-and-
            //    suspenders in case a MOVES variant advances the PC.
            if (regs.instruction_pc != mmu030_insn_start_pc
                  && (mmu030_opcode & 0xFF00) != 0x0E00 /* never no-restart a MOVES */) {
               lastwrite_norestart = true;   // PC already past the insn: replay-only, do not re-execute
            } else {
               if (mmufixup[0].reg >= 0)
                  m68k_areg(regs, mmufixup[0].reg & 7) = mmufixup[0].value;
               if (mmufixup[1].reg >= 0
                     && (mmufixup[0].reg < 0 || (mmufixup[1].reg & 7) != (mmufixup[0].reg & 7)))
                  m68k_areg(regs, mmufixup[1].reg & 7) = mmufixup[1].value;
            }
            mmufixup[0].reg = -1;
            mmufixup[1].reg = -1;
         } else {
            regs.ccrflags = f;
            cpu_restore_fixup();
         }
         // Frame-$B / prefetch / read faults RESTART the whole instruction -> rebuild the bus-error
         // frame from the instruction-START pc. Same for a MOVES / (An)+ LASTWRITE write (case 1 above:
         // rolled-back An + restart). ONLY a no-(An) same-address RMW LASTWRITE (case 2) keeps the
         // handler-advanced PC so RTE resumes at the NEXT instruction (rewinding re-executed the RMW
         // -> the addq 0->2 double that crashed ttymon/getty).
         if (!lastwrite_norestart)
            regs.instruction_pc = mmu030_insn_start_pc;
         m68k_setpci(regs.instruction_pc);
         TRY(prb2) {
            Exception(prb);
         } CATCH(prb2) {
            // Fault while building the bus-error frame == double fault.
            halt = CPU_HALT_BUS_ERROR_DOUBLE_FAULT;
         } ENDTRY
      } ENDTRY
   }
   cpu_halt(halt);
}

static int in_m68k_go = 0;

static bool cpu_hardreset, cpu_keyboardreset;

bool is_hardreset(void)
{
   return cpu_hardreset;
}
bool is_keyboardreset(void)
{
   return  cpu_keyboardreset;
}

#ifdef USE_JIT_FPU
static uae_u8 fp_buffer[9 * 16];
#endif

void m68k_go (int may_quit)
{
   int hardboot = 1;

#ifdef WITH_THREADED_CPU
   init_cpu_thread();
#endif
   if (in_m68k_go || !may_quit) {
      write_log (_T("Bug! m68k_go is not reentrant.\n"));
      abort ();
   }

#ifdef USE_JIT_FPU
#ifdef CPU_AARCH64
   save_host_fp_regs(fp_buffer);
#elif defined (CPU_arm)
   // This caused crashes in RockChip 32-bit platforms unless it was inlined like this
   __asm__ volatile ("vstmia %[fp_buffer]!, {d7-d15}"::[fp_buffer] "r" (fp_buffer));
#endif
#endif

//   reset_frame_rate_hack ();
   update_68k_cycles ();
   start_cycles = 0;

   set_cpu_tracer (false);

   cpu_prefs_changed_flag = 0;
   in_m68k_go++;
   for (;;) {
      int restored = 0;
      void (*run_func)(void);

      cputrace.state = -1;

//      if (currprefs.inprecfile[0] && input_play) {
//         inprec_open (currprefs.inprecfile, NULL);
//         changed_prefs.inprecfile[0] = currprefs.inprecfile[0] = 0;
//         quit_program = UAE_RESET;
//      }
//      if (input_play || input_record)
//         inprec_startup ();

#if 0
      if (quit_program > 0) {
         cpu_keyboardreset = quit_program == UAE_RESET_KEYBOARD;
         cpu_hardreset = ((quit_program == UAE_RESET_HARD ? 1 : 0) || hardboot) != 0;
         hardboot |= quit_program == UAE_RESET_HARD ? 1 : 0;

         if (quit_program == UAE_QUIT)
            break;

         hsync_counter = 0;
         vsync_counter = 0;
         quit_program = 0;

#ifdef SAVESTATE
         if (savestate_state == STATE_DORESTORE)
            savestate_state = STATE_RESTORE;
         if (savestate_state == STATE_RESTORE)
            restore_state (savestate_fname);
         else if (savestate_state == STATE_REWIND)
            savestate_rewind ();
#endif
         prefs_changed_cpu();
         build_cpufunctbl();
         set_x_funcs();
         set_cycles (start_cycles);
         custom_reset (cpu_hardreset != 0, cpu_keyboardreset);
         m68k_reset (cpu_hardreset != 0);
         if (cpu_hardreset) {
            memory_clear ();
            write_log (_T("hardreset, memory cleared\n"));
         }
#ifdef SAVESTATE
         /* We may have been restoring state, but we're done now.  */
         if (isrestore()) {
            restored = savestate_restore_finish ();
#ifdef DEBUGGER
            memory_map_dump ();
#endif
            hardboot = 1;
         }
#endif
         if (currprefs.produce_sound == 0)
            eventtab[ev_audio].active = false;
         m68k_setpc_normal (regs.pc);
         check_prefs_changed_audio ();

         if (!restored || hsync_counter == 0)
            savestate_check ();
         if (input_record == INPREC_RECORD_START)
            input_record = INPREC_RECORD_NORMAL;
         statusline_clear();
      } else {
         if (input_record == INPREC_RECORD_START) {
            input_record = INPREC_RECORD_NORMAL;
            savestate_init ();
            hsync_counter = 0;
            vsync_counter = 0;
            savestate_check ();
         }
      }

      if (changed_prefs.inprecfile[0] && input_record)
         inprec_prepare_record (savestate_fname[0] ? savestate_fname : NULL);
#ifdef DEBUGGER
      if (changed_prefs.trainerfile[0])
         debug_init_trainer(changed_prefs.trainerfile);
#endif
#endif
      set_cpu_tracer (false);

#ifdef DEBUGGER
      if (debugging)
         debug ();
#endif
      if (regs.spcflags & SPCFLAG_MODE_CHANGE) {
         if (cpu_prefs_changed_flag & 1) {
            uaecptr pc = m68k_getpc();
            prefs_changed_cpu();
//            custom_cpuchange();
            build_cpufunctbl();
            m68k_setpc_normal(pc);
            fill_prefetch();
            update_68k_cycles();
         }
         if (cpu_prefs_changed_flag & 2) {
//            fixup_cpu(&changed_prefs);
            currprefs.m68k_speed = changed_prefs.m68k_speed;
            currprefs.m68k_speed_throttle = changed_prefs.m68k_speed_throttle;
            update_68k_cycles();
//            target_cpu_speed();
         }
         cpu_prefs_changed_flag = 0;
      }

      set_x_funcs();
//      if (hardboot) {
//         custom_prepare();
//         mman_set_barriers(false);
//         protect_roms(true);
//      }
//      if ((cpu_keyboardreset || hardboot) && !restored) {
//         warpmode_reset();
//      }
      cpu_hardreset = false;
      cpu_keyboardreset = false;
      event_wait = true;
      unset_special(SPCFLAG_MODE_CHANGE);
#if 0
      if (!restored && hardboot) {
         uaerandomizeseed();
         uae_u32 s = uaerandgetseed();
         uaesetrandseed(s);
         write_log("rndseed = %08x (%u)\n", s, s);
         // add random delay before CPU starts
         int t = uaerand() & 0x7fff;
         while (t > 255) {
            x_do_cycles(255 * CYCLE_UNIT);
            t -= 255;
         }
         x_do_cycles(t * CYCLE_UNIT);
      }
#endif
      hardboot = 0;

#ifdef SAVESTATE
      if (restored) {
         restored = 0;
         savestate_restore_final();
      }
#endif

      if (!regs.halted) {
         // check that PC points to something that looks like memory.
         uaecptr pc = m68k_getpc();
         addrbank *ab = &get_mem_bank(pc);
         if (ab == NULL || ab == &dmmy_bank || (!currprefs.cpu_compatible && !valid_address(pc, 2)) || (pc & 1)) {
            cpu_halt(CPU_HALT_INVALID_START_ADDRESS);
         }
      }
      if (regs.halted) {
         cpu_halt (regs.halted);
         if (regs.halted < 0) {
            haltloop();
            continue;
         }
      }

      run_func =
         currprefs.mmu_model == 68030 ? m68k_run_mmu030 :   // UAE_030_MMU
         currprefs.cpu_cycle_exact && currprefs.cpu_model <= 68010 ? m68k_run_1_ce :
         currprefs.cpu_compatible && currprefs.cpu_model <= 68010 ? m68k_run_1 :
#ifdef JIT
         currprefs.cpu_model >= 68020 && currprefs.cachesize ? m68k_run_jit :
#endif
         currprefs.cpu_model < 68020 ? m68k_run_2_000 : m68k_run_2_020;
      run_func();
   }
//   protect_roms (false);
//   mman_set_barriers(true);

   // Prepare for a restart: reset pc
   regs.pc = 0;
#define nullptr ((uae_u8 *)0)
   regs.pc_p = nullptr;
   regs.pc_oldp = nullptr;

#ifdef USE_JIT_FPU
#ifdef CPU_AARCH64
   restore_host_fp_regs(fp_buffer);
#elif defined (CPU_arm)
   // This caused crashes in RockChip platforms unless it was inlined like this
   __asm__ volatile ("vldmia %[fp_buffer]!, {d7-d15}" ::[fp_buffer] "r"(fp_buffer));
#endif
#endif

   in_m68k_go--;
}
#ifdef SAVESTATE

/* CPU save/restore code */

#define CPUTYPE_EC 1
#define CPUMODE_HALT 1

uae_u8 *restore_cpu (uae_u8 *src)
{
   int flags, model;
   uae_u32 l;

   currprefs.cpu_model = changed_prefs.cpu_model = model = restore_u32 ();
   flags = restore_u32 ();
   changed_prefs.address_space_24 = false;
   if (flags & CPUTYPE_EC)
      changed_prefs.address_space_24 = true;
   currprefs.address_space_24 = changed_prefs.address_space_24;
   currprefs.cpu_compatible = changed_prefs.cpu_compatible;
   currprefs.cpu_cycle_exact = changed_prefs.cpu_cycle_exact;
   currprefs.cpu_memory_cycle_exact = changed_prefs.cpu_memory_cycle_exact;
   currprefs.blitter_cycle_exact = changed_prefs.blitter_cycle_exact;
   currprefs.cpu_frequency = changed_prefs.cpu_frequency = 0;
   currprefs.cpu_clock_multiplier = changed_prefs.cpu_clock_multiplier = 0;
   for (int i = 0; i < 15; i++)
      regs.regs[i] = restore_u32 ();
   regs.pc = restore_u32 ();
   regs.irc = restore_u16 ();
   regs.ir = restore_u16 ();
   regs.usp = restore_u32 ();
   regs.isp = restore_u32 ();
   regs.sr = restore_u16 ();
   l = restore_u32 ();
   if (l & CPUMODE_HALT) {
      regs.stopped = 1;
   } else {
      regs.stopped = 0;
   }
   if (model >= 68010) {
      regs.dfc = restore_u32 ();
      regs.sfc = restore_u32 ();
      regs.vbr = restore_u32 ();
   }
   if (model >= 68020) {
      regs.caar = restore_u32 ();
      regs.cacr = restore_u32 ();
      regs.msp = restore_u32 ();
   }
   if (model >= 68030) {
      fake_crp_030 = restore_u64 ();
      fake_srp_030 = restore_u64 ();
      fake_tt0_030 = restore_u32 ();
      fake_tt1_030 = restore_u32 ();
      fake_tc_030 = restore_u32 ();
      fake_mmusr_030 = restore_u16 ();
   }
   if (model >= 68040) {
      regs.itt0 = restore_u32 ();
      regs.itt1 = restore_u32 ();
      regs.dtt0 = restore_u32 ();
      regs.dtt1 = restore_u32 ();
      regs.tcr = restore_u32 ();
      regs.urp = restore_u32 ();
      regs.srp = restore_u32 ();
   }
   if (flags & 0x80000000) {
      int khz = restore_u32 ();
      restore_u32 ();
      if (khz < 0)
         currprefs.m68k_speed = changed_prefs.m68k_speed = -1;
      else if (khz > 0 && khz < 800000)
         currprefs.m68k_speed = changed_prefs.m68k_speed = 0;
   }
   set_cpu_caches (true);
   if (flags & 0x10000000) {
      regs.chipset_latch_rw = restore_u32 ();
   }

   if (flags & 0x2000000 && currprefs.cpu_model <= 68010) {
      restore_u32();
      regs.read_buffer = restore_u16();
      regs.write_buffer = restore_u16();
   }

   m68k_reset_sr();

   write_log (_T("CPU: %d%s%03d, PC=%08X\n"),
      model / 1000, flags & 1 ? _T("EC") : _T(""), model % 1000, regs.pc);

   return src;
}
#endif
void fill_prefetch_quick (void)
{
   if (currprefs.cpu_model >= 68020) {
      fill_prefetch ();
      return;
   }
   // old statefile compatibility, this needs to done,
   // even in 68000 cycle-exact mode
   regs.ir = get_word (m68k_getpc ());
   regs.irc = get_word (m68k_getpc () + 2);
}
#if 0
void restore_cpu_finish (void)
{
   if (!currprefs.fpu_model)
      fpu_reset();
   init_m68k ();
   m68k_setpc_normal (regs.pc);
   doint ();
   fill_prefetch_quick ();
   set_cycles (start_cycles);
   events_schedule ();
   if (regs.stopped)
      set_special (SPCFLAG_STOP);
}

uae_u8 *save_cpu_trace(size_t *len, uae_u8 *dstptr)
{
   uae_u8 *dstbak, *dst;

   if (cputrace.state <= 0)
      return NULL;

   if (dstptr)
      dstbak = dst = dstptr;
   else
      dstbak = dst = xmalloc (uae_u8, 10000);

   save_u32 (2 | 4 | 16 | 32 | 64);
   save_u16 (cputrace.opcode);
   for (int i = 0; i < 16; i++)
      save_u32 (cputrace.regs[i]);
   save_u32 (cputrace.pc);
   save_u16 (cputrace.irc);
   save_u16 (cputrace.ir);
   save_u32 (cputrace.usp);
   save_u32 (cputrace.isp);
   save_u16 (cputrace.sr);
   save_u16 (cputrace.intmask);
   save_u16 ((cputrace.stopped ? 1 : 0) | (regs.stopped ? 2 : 0));
   save_u16 (cputrace.state);
   save_u32 (cputrace.cyclecounter);
   save_u32 (cputrace.cyclecounter_pre);
   save_u32 (cputrace.cyclecounter_post);
   save_u32 (cputrace.readcounter);
   save_u32 (cputrace.writecounter);
   save_u32 (cputrace.memoryoffset);
   write_log (_T("CPUT SAVE: PC=%08x C=%08X %08x %08x %08x %d %d %d\n"),
      cputrace.pc, cputrace.startcycles,
      cputrace.cyclecounter, cputrace.cyclecounter_pre, cputrace.cyclecounter_post,
      cputrace.readcounter, cputrace.writecounter, cputrace.memoryoffset);
   for (int i = 0; i < cputrace.memoryoffset; i++) {
      save_u32 (cputrace.ctm[i].addr);
      save_u32 (cputrace.ctm[i].data);
      save_u32 (cputrace.ctm[i].mode);
      write_log (_T("CPUT%d: %08x %08x %08x\n"), i, cputrace.ctm[i].addr, cputrace.ctm[i].data, cputrace.ctm[i].mode);
   }
   save_u32 ((uae_u32)cputrace.startcycles);

   save_u16(cputrace.read_buffer);
   save_u16(cputrace.writecounter);

   *len = dst - dstbak;
   cputrace.needendcycles = 1;
   return dstbak;
}

uae_u8 *restore_cpu_trace(uae_u8 *src)
{
   cpu_tracer = 0;
   cputrace.state = 0;
   uae_u32 v = restore_u32 ();
   if (!(v & 2))
      return src;
   cputrace.opcode = restore_u16 ();
   for (int i = 0; i < 16; i++)
      cputrace.regs[i] = restore_u32 ();
   cputrace.pc = restore_u32 ();
   cputrace.irc = restore_u16 ();
   cputrace.ir = restore_u16 ();
   cputrace.usp = restore_u32 ();
   cputrace.isp = restore_u32 ();
   cputrace.sr = restore_u16 ();
   cputrace.intmask = restore_u16 ();
   cputrace.stopped = restore_u16 ();
   cputrace.state = restore_u16 ();
   cputrace.cyclecounter = restore_u32 ();
   cputrace.cyclecounter_pre = restore_u32 ();
   cputrace.cyclecounter_post = restore_u32 ();
   cputrace.readcounter = restore_u32 ();
   cputrace.writecounter = restore_u32 ();
   cputrace.memoryoffset = restore_u32 ();
   for (int i = 0; i < cputrace.memoryoffset; i++) {
      cputrace.ctm[i].addr = restore_u32 ();
      cputrace.ctm[i].data = restore_u32 ();
      cputrace.ctm[i].mode = restore_u32 ();
   }
   cputrace.startcycles = restore_u32();

   if (v & 4) {
      if (currprefs.cpu_model == 68020) {
         if (v & 64) {
            cputrace.read_buffer = restore_u16();
            cputrace.write_buffer = restore_u16();
         }
      }
   }

   cputrace.needendcycles = 1;
   if (v && cputrace.state) {
      if (currprefs.cpu_model > 68000) {
         if (v & 4)
            cpu_tracer = -1;
         // old format?
         if ((v & (4 | 8)) != (4 | 8) && (v & (32 | 16 | 8 | 4)) != (32 | 16 | 4))
            cpu_tracer = 0;
      } else {
         cpu_tracer = -1;
      }
   }

   return src;
}

uae_u8 *restore_cpu_extra(uae_u8 *src)
{
   restore_u32 ();
   uae_u32 flags = restore_u32 ();

   currprefs.cpu_cycle_exact = changed_prefs.cpu_cycle_exact = (flags & 1) ? true : false;
   currprefs.cpu_memory_cycle_exact = changed_prefs.cpu_memory_cycle_exact = currprefs.cpu_cycle_exact;
   if ((flags & 32) && !(flags & 1))
      currprefs.cpu_memory_cycle_exact = changed_prefs.cpu_memory_cycle_exact = true;
   currprefs.blitter_cycle_exact = changed_prefs.blitter_cycle_exact = currprefs.cpu_cycle_exact;
   currprefs.cpu_compatible = changed_prefs.cpu_compatible = (flags & 2) ? true : false;
   currprefs.cpu_frequency = changed_prefs.cpu_frequency = restore_u32 ();
   currprefs.cpu_clock_multiplier = changed_prefs.cpu_clock_multiplier = restore_u32 ();
   //currprefs.cachesize = changed_prefs.cachesize = (flags & 8) ? 8192 : 0;

   currprefs.m68k_speed = changed_prefs.m68k_speed = 0;
   if (flags & 4)
      currprefs.m68k_speed = changed_prefs.m68k_speed = -1;
   if (flags & 16)
      currprefs.m68k_speed = changed_prefs.m68k_speed = (flags >> 24) * CYCLE_UNIT;

   return src;
}

uae_u8 *save_cpu_extra(size_t *len, uae_u8 *dstptr)
{
   uae_u8 *dstbak, *dst;
   uae_u32 flags;

   if (dstptr)
      dstbak = dst = dstptr;
   else
      dstbak = dst = xmalloc (uae_u8, 1000);
   save_u32 (0); // version
   flags = 0;
   flags |= currprefs.cpu_cycle_exact ? 1 : 0;
   flags |= currprefs.cpu_compatible ? 2 : 0;
   flags |= currprefs.m68k_speed < 0 ? 4 : 0;
   flags |= currprefs.cachesize > 0 ? 8 : 0;
   flags |= currprefs.m68k_speed > 0 ? 16 : 0;
   flags |= currprefs.cpu_memory_cycle_exact ? 32 : 0;
   if (currprefs.m68k_speed > 0)
      flags |= (currprefs.m68k_speed / CYCLE_UNIT) << 24;
   save_u32 (flags);
   save_u32 (currprefs.cpu_frequency);
   save_u32 (currprefs.cpu_clock_multiplier);
   *len = dst - dstbak;
   return dstbak;
}

uae_u8 *save_cpu(size_t *len, uae_u8 *dstptr)
{
   uae_u8 *dstbak, *dst;
   int model, khz;

   if (dstptr)
      dstbak = dst = dstptr;
   else
      dstbak = dst = xmalloc (uae_u8, 1000 + 30000);
   model = currprefs.cpu_model;
   save_u32 (model);               /* MODEL */
   save_u32(0x80000000 | 0x40000000 | 0x20000000 | 0x10000000 | 0x8000000 | 0x4000000 | 0x2000000 | (currprefs.address_space_24 ? 1 : 0)); /* FLAGS */
   for (int i = 0;i < 15; i++)
      save_u32 (regs.regs[i]);      /* D0-D7 A0-A6 */
   save_u32 (m68k_getpc ());         /* PC */
   save_u16 (regs.irc);            /* prefetch */
   save_u16 (regs.ir);               /* instruction prefetch */
   MakeSR ();
   save_u32 (!regs.s ? regs.regs[15] : regs.usp);   /* USP */
   save_u32 (regs.s ? regs.regs[15] : regs.isp);   /* ISP */
   save_u16 (regs.sr);                        /* SR/CCR */
   save_u32 (regs.stopped ? CPUMODE_HALT : 0); /* flags */
   if (model >= 68010) {
      save_u32 (regs.dfc);         /* DFC */
      save_u32 (regs.sfc);         /* SFC */
      save_u32 (regs.vbr);         /* VBR */
   }
   if (model >= 68020) {
      save_u32 (regs.caar);         /* CAAR */
      save_u32 (regs.cacr);         /* CACR */
      save_u32 (regs.msp);         /* MSP */
   }
   if (model >= 68030) {
      save_u64 (fake_crp_030);      /* CRP */
      save_u64 (fake_srp_030);      /* SRP */
      save_u32 (fake_tt0_030);      /* TT0/AC0 */
      save_u32 (fake_tt1_030);      /* TT1/AC1 */
      save_u32 (fake_tc_030);      /* TCR */
      save_u16 (fake_mmusr_030);      /* MMUSR/ACUSR */
   }
   if (model >= 68040) {
      save_u32 (regs.itt0);      /* ITT0 */
      save_u32 (regs.itt1);      /* ITT1 */
      save_u32 (regs.dtt0);      /* DTT0 */
      save_u32 (regs.dtt1);   /* DTT1 */
      save_u32 (regs.tcr);    /* TCR */
      save_u32 (regs.urp);      /* URP */
      save_u32 (regs.srp);      /* SRP */
   }
   khz = -1;
   if (currprefs.m68k_speed == 0) {
      khz = currprefs.ntscmode ? 715909 : 709379;
      if (currprefs.cpu_model >= 68020)
         khz *= 2;
   }
   save_u32 (khz); // clock rate in KHz: -1 = fastest possible
   save_u32 (0); // spare
   save_u32 (regs.chipset_latch_rw);
   if (currprefs.cpu_model <= 68010) {
      save_u32(0);
      save_u16(regs.read_buffer);
      save_u16(regs.write_buffer);
   }
   *len = dst - dstbak;
   return dstbak;
}

#endif /* SAVESTATE */

static void exception3f(uae_u32 opcode, uaecptr addr, bool writeaccess, bool instructionaccess, bool notinstruction, uaecptr pc, int size, int fc, uae_u16 secondarysr)
{
   if (currprefs.cpu_model >= 68040)
      addr &= ~1;
   if (currprefs.cpu_model >= 68020) {
      if (pc == 0xffffffff)
         last_addr_for_exception_3 = regs.instruction_pc;
      else
         last_addr_for_exception_3 = pc;
   } else if (pc == 0xffffffff) {
      last_addr_for_exception_3 = m68k_getpc();
   } else {
      last_addr_for_exception_3 = pc;
   }
   last_fault_for_exception_3 = addr;
   last_op_for_exception_3 = opcode;
   last_writeaccess_for_exception_3 = writeaccess;
   last_fc_for_exception_3 = fc >= 0 ? fc : (instructionaccess ? 2 : 1);
   last_notinstruction_for_exception_3 = notinstruction;
   last_size_for_exception_3 = size;
   last_sr_for_exception3 = secondarysr;
   Exception (3);
}

static void exception3_notinstruction(uae_u32 opcode, uaecptr addr)
{
   last_di_for_exception_3 = 1;
   exception3f (opcode, addr, true, false, true, 0xffffffff, 1, -1, 0);
}

// 68010 special prefetch handling
void exception3_read_prefetch_only(uae_u32 opcode, uae_u32 addr)
{
   if (currprefs.cpu_model == 68010) {
      uae_u16 prev = regs.read_buffer;
      x_get_word(addr & ~1);
      regs.irc = regs.read_buffer;
   } else {
      x_do_cycles(4 * CYCLE_UNIT / 2);
   }
   last_di_for_exception_3 = 0;
   exception3f(opcode, addr, false, true, false, m68k_getpc(), sz_word, -1, 0);
}

// Some hardware accepts address error aborted reads or writes as normal reads/writes.
void exception3_read_prefetch(uae_u32 opcode, uaecptr addr)
{
   x_do_cycles(4 * CYCLE_UNIT / 2);
   last_di_for_exception_3 = 0;
   if (currprefs.cpu_model == 68000) {
      m68k_incpci(2);
   }
   exception3f(opcode, addr, false, true, false, m68k_getpc(), sz_word, -1, 0);
}
void exception3_read_prefetch_68040bug(uae_u32 opcode, uaecptr addr, uae_u16 secondarysr)
{
   x_do_cycles(4 * CYCLE_UNIT / 2);
   last_di_for_exception_3 = 0;
   exception3f(opcode | 0x10000, addr, false, true, false, m68k_getpc(), sz_word, -1, secondarysr);
}

void exception3_read_access(uae_u32 opcode, uaecptr addr, int size, int fc)
{
   x_do_cycles(4 * CYCLE_UNIT / 2);
   exception3_read(opcode, addr, size, fc);
}
void exception3_read_access2(uae_u32 opcode, uaecptr addr, int size, int fc)
{
   // (An), -(An) and (An)+ and 68010: read happens twice!
   x_do_cycles(8 * CYCLE_UNIT / 2);
   exception3_read(opcode, addr, size, fc);
}
void exception3_write_access(uae_u32 opcode, uaecptr addr, int size, uae_u32 val, int fc)
{
   x_do_cycles(4 * CYCLE_UNIT / 2);
   exception3_write(opcode, addr, size, val, fc);
}

void exception3_read(uae_u32 opcode, uaecptr addr, int size, int fc)
{
   bool ni = false;
   bool ia = false;
   if (currprefs.cpu_model == 68000 && currprefs.cpu_compatible) {
      if (generates_group1_exception(regs.ir) && !(opcode & 0x20000)) {
         ni = true;
         fc = -1;
      }
      if (opcode & 0x10000)
         ni = true;
      if (opcode & 0x40000)
         ia = true;
      opcode = regs.ir;
   }
   last_di_for_exception_3 = 1;
   exception3f(opcode, addr, false, ia, ni, 0xffffffff, size & 15, fc, 0);
}
void exception3_write(uae_u32 opcode, uaecptr addr, int size, uae_u32 val, int fc)
{
   bool ni = false;
   bool ia = false;
   if (currprefs.cpu_model == 68000 && currprefs.cpu_compatible) {
      if (generates_group1_exception(regs.ir) && !(opcode & 0x20000)) {
         ni = true;
         fc = -1;
      }
      if (opcode & 0x10000)
         ni = true;
      if (opcode & 0x40000)
         ia = true;
      opcode = regs.ir;
   }
   last_di_for_exception_3 = 1;
   regs.write_buffer = val;
   exception3f(opcode, addr, true, ia, ni, 0xffffffff, size & 15, fc, 0);
}

void exception2_setup(uae_u32 opcode, uaecptr addr, bool read, int size, uae_u32 fc)
{
   last_addr_for_exception_3 = m68k_getpc();
   last_fault_for_exception_3 = addr;
   last_writeaccess_for_exception_3 = read == 0;
   last_op_for_exception_3 = opcode;
   last_fc_for_exception_3 = fc;
   last_notinstruction_for_exception_3 = exception_in_exception != 0;
   last_size_for_exception_3 = size & 15;
   last_di_for_exception_3 = 1;
   hardware_bus_error = 0;

   if (currprefs.cpu_model == 68000 && currprefs.cpu_compatible) {
      if (generates_group1_exception(regs.ir) && !(opcode & 0x20000)) {
         last_notinstruction_for_exception_3 = true;
         fc = -1;
      }
      if (opcode & 0x10000)
         last_notinstruction_for_exception_3 = true;
      if (!(opcode & 0x20000))
         last_op_for_exception_3 = regs.ir;
   }
}

// Common hardware bus error entry point. Both for MMU and non-MMU emulation.
void hardware_exception2(uaecptr addr, uae_u32 v, bool read, bool ins, int size)
{
   if (currprefs.cpu_compatible && HARDWARE_BUS_ERROR_EMULATION) {
      hardware_bus_error = 1;
   } else {
      int fc = (regs.s ? 4 : 0) | (ins ? 2 : 1);
      // Non-MMU
      exception2_setup(regs.opcode, addr, read, size, fc);
      THROW(2);
   }
}

void exception2_read(uae_u32 opcode, uaecptr addr, int size, int fc)
{
   exception2_setup(opcode, addr, true, size & 15, fc);
   Exception(2);
}

void exception2_write(uae_u32 opcode, uaecptr addr, int size, uae_u32 val, int fc)
{
   exception2_setup(opcode, addr, false, size & 15, fc);
   if (size & 0x100) {
      regs.write_buffer = val;
   } else {
      if (size == sz_byte) {
         regs.write_buffer &= 0xff00;
         regs.write_buffer |= val & 0xff;
      } else {
         regs.write_buffer = val;
      }
   }
   Exception(2);
}

static void exception2_fetch_common(uae_u32 opcode, int offset)
{
   last_fault_for_exception_3 = m68k_getpc() + offset;
   // this is not yet fully correct
   last_addr_for_exception_3 = last_fault_for_exception_3;
   last_writeaccess_for_exception_3 = 0;
   last_op_for_exception_3 = opcode;
   last_fc_for_exception_3 = 2;
   last_notinstruction_for_exception_3 = exception_in_exception != 0;
   last_size_for_exception_3 = sz_word;
   last_di_for_exception_3 = 0;
   hardware_bus_error = 0;

   if (currprefs.cpu_model == 68000 && currprefs.cpu_compatible) {
      if (generates_group1_exception(regs.ir) && !(opcode & 0x20000)) {
         last_fc_for_exception_3 |= 8;  // set N/I
      }
      if (opcode & 0x10000)
         last_fc_for_exception_3 |= 8;
   }
}

void exception2_fetch_opcode(uae_u32 opcode, int offset, int pcoffset)
{
   exception2_fetch_common(opcode, offset);
   last_addr_for_exception_3 += pcoffset;
   if (currprefs.cpu_model == 68010) {
      last_di_for_exception_3 = -1;
   }
   Exception(2);
}

void exception2_fetch(uae_u32 opcode, int offset, int pcoffset)
{
   exception2_fetch_common(opcode, offset);
   last_addr_for_exception_3 += pcoffset;
   Exception(2);
}

int reset_loop_counter=0;
void hard_reboot(void)
{
#define PS_RST_CTRL_REG         (XPS_SYS_CTRL_BASEADDR + 0x244)
#define PS_RST_MASK         0x3   /**< PS software reset (Core 1 reset)*/
   Xil_Out32(PS_RST_CTRL_REG, PS_RST_MASK);
   XScuWdt_Config *config=XScuWdt_LookupConfig(XPAR_SCUWDT_0_DEVICE_ID);
   XScuWdt instance;
   XScuWdt_CfgInitialize(&instance,config,config->BaseAddr);
   XScuWdt_LoadWdt(&instance,0xFF);
   XScuWdt_Start(&instance);
   XScuWdt_SetWdMode(&instance);
   while(1);
}

bool cpureset (void)
{
   /* RESET hasn't increased PC yet, 1 word offset */
   uaecptr pc;
   uaecptr ksboot = 0xf80002 - 2;
   uae_u16 ins;
   addrbank *ab;
   bool extreset = false;

   maybe_disable_fpu();
   m68k_reset_delay = currprefs.reset_delay;
   set_special(SPCFLAG_CHECK);
#if 0
   send_internalevent(INTERNALEVENT_CPURESET);
   warpmode_reset();
#ifndef AMIBERRY
   if (cpuboard_forced_hardreset()) {
      custom_reset_cpu(false, false);
      m68k_reset();
      return true;
   }
#endif
#endif
   if ((currprefs.cpu_compatible || currprefs.cpu_memory_cycle_exact) && currprefs.cpu_model <= 68020) {
      custom_reset_cpu(false, false);
      return false;
   }
   pc = m68k_getpc () + 2;

    ab = &get_mem_bank (pc);
   if (ab->check (pc, 2)) {
//      write_log (_T("CPU reset PC=%x (%s)..\n"), pc - 2, ab->name);
      write_log (_T("CPU reset PC=%x\n"), pc - 2);

      ins = get_word (pc);
      (void)ins; /* reset/jmp PC-hack removed below: do a full clean reset instead */
      custom_reset_cpu(false, false);
      z3660_quiesce_real_chipset_on_reset();
      m68k_setpc_normal (ksboot);
      cpu_emulator_reset_core0();
      /* The old reset/jmp PC-hack left the 68k mid-vector (0xF80002) with stale
       * SR/SSP/MMU, so AMIX's warm (uadmin) reboot never actually restarted
       * Kickstart -- only the EXTER storm (now fixed) had masked it.  Do the same
       * full CPU reset the cold-boot and n040RSTI paths use: overlay ROM at 0
       * (ovl=1, which AMIX had cleared) so get_long(4) returns the real reset
       * vector, then m68k_reset_newcpu(1) sets PC=0xF800D2, SSP, SR(intmask=7),
       * and resets MMU/caches. */
      ovl = 1;
      m68k_reset_newcpu(1);
      reset_autoconfig();
      /* Pre-compensate for the caller's trailing m68k_incpc(2): the RESET opcode
       * handler (op_4e70_*) does `cpureset(); m68k_incpc(2);`, unconditionally
       * advancing PC by 2 after we return.  Leave PC at (reset PC - 2) so the +2
       * restores Kickstart's entry 0xF800D2.  (The n040RSTI path has no trailing
       * +2, hence it doesn't need this.) */
      m68k_setpc_normal (m68k_getpc () - 2);
      fill_prefetch_quick ();
      set_cycles (start_cycles);
      regs.stopped = false;
      return false;
   }

   // the best we can do, jump directly to ROM entrypoint
   // (which is probably what program wanted anyway)
//   write_log (_T("CPU Reset PC=%x (%s), invalid memory -> %x.\n"), pc, ab->name, ksboot + 2);

   write_log (_T("CPU Reset PC=%x, invalid memory -> %x.\n"), pc, ksboot + 2);
   custom_reset_cpu(false, false);
   z3660_quiesce_real_chipset_on_reset();
   m68k_setpc_normal (ksboot);
   cpu_emulator_reset_core0();
   /* full clean reset, same as the main branch (see comments there) */
   ovl = 1;
   m68k_reset_newcpu(1);
   reset_autoconfig();
   m68k_setpc_normal (m68k_getpc () - 2);
   fill_prefetch_quick ();
   set_cycles (start_cycles);
   regs.stopped = false;
   return false;
}


void m68k_setstopped (void)
{
   /* A traced STOP instruction drops through immediately without
   actually stopping.  */
   if ((regs.spcflags & SPCFLAG_DOTRACE) == 0) {
      m68k_set_stop();
   } else {
      m68k_resumestopped ();
   }
}

void m68k_resumestopped (void)
{
   if (!regs.stopped)
      return;
   if (currprefs.cpu_cycle_exact && currprefs.cpu_model == 68000) {
      x_do_cycles (6 * CYCLE_UNIT / 2);
   }
   fill_prefetch ();
   m68k_unset_stop();
}

uae_u32 mem_access_delay_word_read (uaecptr addr)
{
   uae_u32 v;
/*   switch (ce_banktype[addr >> 16])
   {
   case CE_MEMBANK_CHIP16:
   case CE_MEMBANK_CHIP32:
      v = wait_cpu_cycle_read (addr, 1);
      break;
   case CE_MEMBANK_FAST16:
   case CE_MEMBANK_FAST32:
      v = get_word (addr);
      x_do_cycles_post (4 * CYCLE_UNIT / 2, v);
      break;
   default:
      v = get_word (addr);
      break;
   }
   regs.db = v;
   regs.read_buffer = v;*/
   return get_word (addr);
}
uae_u32 mem_access_delay_wordi_read (uaecptr addr)
{
   uae_u32 v;
/*   switch (ce_banktype[addr >> 16])
   {
   case CE_MEMBANK_CHIP16:
   case CE_MEMBANK_CHIP32:
      v = wait_cpu_cycle_read (addr, 2);
      break;
   case CE_MEMBANK_FAST16:
   case CE_MEMBANK_FAST32:
      v = get_wordi (addr);
      x_do_cycles_post (4 * CYCLE_UNIT / 2, v);
      break;
   default:
      v = get_wordi (addr);
      break;
   }
   regs.db = v;
   regs.read_buffer = v;*/
   return get_wordi (addr);
}

uae_u32 mem_access_delay_byte_read (uaecptr addr)
{
   uae_u32  v;
/*   switch (ce_banktype[addr >> 16])
   {
   case CE_MEMBANK_CHIP16:
   case CE_MEMBANK_CHIP32:
      v = wait_cpu_cycle_read (addr, 0);
      break;
   case CE_MEMBANK_FAST16:
   case CE_MEMBANK_FAST32:
      v = get_byte (addr);
      x_do_cycles_post (4 * CYCLE_UNIT / 2, v);
      break;
   default:
      v = get_byte (addr);
      break;
   }
   regs.db = (v << 8) | v;
   regs.read_buffer = v;*/
   return get_byte (addr);
}
void mem_access_delay_byte_write (uaecptr addr, uae_u32 v)
{
/*   regs.db = (v << 8)  | v;
   regs.write_buffer = v;
   switch (ce_banktype[addr >> 16])
   {
   case CE_MEMBANK_CHIP16:
   case CE_MEMBANK_CHIP32:
      wait_cpu_cycle_write (addr, 0, v);
      return;
   case CE_MEMBANK_FAST16:
   case CE_MEMBANK_FAST32:
      put_byte (addr, v);
      x_do_cycles_post (4 * CYCLE_UNIT / 2, v);
      return;
   }*/
   put_byte (addr, v);
}
void mem_access_delay_word_write (uaecptr addr, uae_u32 v)
{
/*   regs.db = v;
   regs.write_buffer = v;
   switch (ce_banktype[addr >> 16])
   {
   case CE_MEMBANK_CHIP16:
   case CE_MEMBANK_CHIP32:
      wait_cpu_cycle_write (addr, 1, v);
      return;
   case CE_MEMBANK_FAST16:
   case CE_MEMBANK_FAST32:
      put_word (addr, v);
      x_do_cycles_post (4 * CYCLE_UNIT / 2, v);
      return;
   }*/
   put_word (addr, v);
}

void check_t0_trace(void)
{
   if (regs.t0 && !regs.t1 && currprefs.cpu_model >= 68020) {
      unset_special (SPCFLAG_TRACE);
      set_special (SPCFLAG_DOTRACE);
   }
}

void fill_prefetch (void)
{
   if (currprefs.cachesize)
      return;
   if (!currprefs.cpu_compatible)
      return;
   uaecptr pc = m68k_getpc ();
   regs.ir = x_get_word (pc);
   regs.irc = x_get_word (pc + 2);
   regs.read_buffer = regs.irc;
}
