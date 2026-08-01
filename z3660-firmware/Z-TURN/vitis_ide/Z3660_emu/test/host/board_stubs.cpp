/* Board-seam stubs for the Layer-1 host MMU harness.
 *
 * Replaces the Z3660 board environment (Xilinx BSP, FPGA/Zorro bridge, chipset,
 * IPL, board printf/task-pump) with a deterministic flat-RAM machine on x86-64.
 *
 * Memory model (decision #6): one malloc'd buffer indexed by uae_u32, accessed
 * big-endian (68k order). Every memory bank points here via lget/wget (NOT
 * baseaddr_direct), so memory_get and memory_put never dereference a guest
 * address as a host pointer: no uaecptr-to-void* round-trip that would diverge
 * under 64-bit pointers. */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "harness.h"
#include "main.h"      /* SHARED struct (harness-safe; newcpu.cpp pulls it via ../main.h) */

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>

/* ---- Emulator config globals (normally in uae_emulator.cpp, excluded) ---- */
struct uae_prefs currprefs, changed_prefs;

/* Cross-core SHARED struct (defined in main.cc on target, excluded here) + the
 * emulator-debug gate accessor (main.cc). Inert in the harness: debug_emu / amix_mode
 * zero, z3660_dbg_emu()==0, so the [PC]/[RTE-B-IF]/fixup traces stay off. */
SHARED _harness_shared;
SHARED *shared = &_harness_shared;
extern "C" int z3660_dbg_emu(void){ return 0; }

/* =====================================================================
 *  Flat-RAM machine
 * ===================================================================== */
static uae_u8 *g_ram = NULL;

uae_u8 *hram_base(void) { return g_ram; }

void hram_poke32(uae_u32 a, uae_u32 v){ a&=HRAM_MASK; g_ram[a]=v>>24; g_ram[a+1]=v>>16; g_ram[a+2]=v>>8; g_ram[a+3]=v; }
void hram_poke16(uae_u32 a, uae_u16 v){ a&=HRAM_MASK; g_ram[a]=v>>8; g_ram[a+1]=(uae_u8)v; }
void hram_poke8 (uae_u32 a, uae_u8  v){ a&=HRAM_MASK; g_ram[a]=v; }
uae_u32 hram_peek32(uae_u32 a){ a&=HRAM_MASK; return ((uae_u32)g_ram[a]<<24)|((uae_u32)g_ram[a+1]<<16)|((uae_u32)g_ram[a+2]<<8)|g_ram[a+3]; }
uae_u16 hram_peek16(uae_u32 a){ a&=HRAM_MASK; return (uae_u16)(((uae_u16)g_ram[a]<<8)|g_ram[a+1]); }
uae_u8  hram_peek8 (uae_u32 a){ a&=HRAM_MASK; return g_ram[a]; }

/* ---- Bank accessors: big-endian into the flat buffer ---- */
static uae_u32 REGPARAM2 hram_lget(uaecptr a){ return hram_peek32(a); }
static uae_u32 REGPARAM2 hram_wget(uaecptr a){ return hram_peek16(a); }
static uae_u32 REGPARAM2 hram_bget(uaecptr a){ return hram_peek8(a); }
static void REGPARAM2 hram_lput(uaecptr a, uae_u32 v){ hram_poke32(a, v); }
static void REGPARAM2 hram_wput(uaecptr a, uae_u32 v){ hram_poke16(a, (uae_u16)v); }
static void REGPARAM2 hram_bput(uaecptr a, uae_u32 v){ hram_poke8 (a, (uae_u8)v); }

/* baseaddr and baseaddr_direct_* left NULL (trailing fields zero-init) so
 * memory_get and memory_put take the lget/wget path, not the direct-pointer one. */
addrbank hram_bank = {
	hram_lget, hram_wget, hram_bget,
	hram_lput, hram_wput, hram_bput,
	NULL, NULL, NULL, _T("hram"), _T("Harness RAM"),
	hram_lget, hram_wget,
	ABFLAG_RAM, 0, 0
};

void harness_mem_reset(void){ if (g_ram) memset(g_ram, 0, HRAM_SIZE); }

void harness_mem_init(void)
{
	if (!g_ram) {
		g_ram = (uae_u8 *)calloc(1, HRAM_SIZE);
		if (!g_ram) { fprintf(stderr, "[harness] OOM allocating %u bytes RAM\n", HRAM_SIZE); abort(); }
	}
	/* Point every 64KB bank slot at the flat RAM (whole 32-bit space resolves). */
	for (unsigned i = 0; i < MEMORY_BANKS; i++)
		mem_banks[i] = &hram_bank;
}

/* =====================================================================
 *  do_get_mem_* / do_put_mem_*  (normally in uae_emulator.cpp, excluded)
 *  Route through memory_get and memory_put exactly as the real board does: the
 *  arg is a guest address disguised as a pointer (get_real_address returns
 *  (u8*)addr in this fork).
 * ===================================================================== */
uae_u32  do_get_mem_long(uae_u32 *a){ return memory_get_long((uaecptr)(uintptr_t)a); }
uint16_t do_get_mem_word(uint16_t *a){ return (uint16_t)memory_get_word((uaecptr)(uintptr_t)a); }
uint8_t  do_get_mem_byte(uint8_t  *a){ return (uint8_t) memory_get_byte((uaecptr)(uintptr_t)a); }
void do_put_mem_long(uae_u32 *a, uae_u32 v){ memory_put_long((uaecptr)(uintptr_t)a, v); }
void do_put_mem_word(uint16_t *a, uint16_t v){ memory_put_word((uaecptr)(uintptr_t)a, v); }
void do_put_mem_byte(uint8_t  *a, uint8_t  v){ memory_put_byte((uaecptr)(uintptr_t)a, v); }

/* =====================================================================
 *  Board bridge (normally in cpu_emulator.cpp, excluded) -> flat buffer
 * ===================================================================== */
extern "C" unsigned int ps_read_8 (unsigned int a){ return hram_peek8(a); }
extern "C" unsigned int ps_read_16(unsigned int a){ return hram_peek16(a); }
extern "C" unsigned int ps_read_32(unsigned int a){ return hram_peek32(a); }
extern "C" void ps_write_8 (unsigned int a, unsigned int v){ hram_poke8 (a, (uae_u8)v); }
extern "C" void ps_write_16(unsigned int a, unsigned int v){ hram_poke16(a, (uae_u16)v); }
extern "C" void ps_write_32(unsigned int a, unsigned int v){ hram_poke32(a, v); }
extern "C" unsigned int test_read_8 (unsigned int a){ return hram_peek8(a); }
extern "C" unsigned int test_read_16(unsigned int a){ return hram_peek16(a); }
extern "C" unsigned int test_read_32(unsigned int a){ return hram_peek32(a); }
extern "C" void test_write_8 (unsigned int a, unsigned int v){ hram_poke8 (a, (uae_u8)v); }
extern "C" void test_write_16(unsigned int a, unsigned int v){ hram_poke16(a, (uae_u16)v); }
extern "C" void test_write_32(unsigned int a, unsigned int v){ hram_poke32(a, v); }

/* =====================================================================
 *  Misc board seams (chipset / IPL / reset / debug) — inert on host
 * ===================================================================== */
extern "C" int  intlev(void){ return -1; }            /* no pending interrupt */
extern "C" void ipl_main_read(void){ }
extern "C" void cpu_emulator_reset_core0(void){ }
extern "C" void z3660_printf(const TCHAR *fmt, ...){ va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void z3660_tasks(void){ }
void do_cycles_ce(int){ }
bool is_cycle_ce(uaecptr){ return false; }
extern "C" void reset_autoconfig(void){ }

/* Host backing for newcpu.cpp's renamed usleep (firmware defines this in
 * cpu_emulator.cpp, which the harness excludes). */
extern "C" void usleep2(unsigned long useconds){ usleep(useconds); }

/* Board signal variables (defined in cpu_emulator.cpp on target) */
int ovl = 0;
int read_irq = 0;
volatile int read_reset = 1;

/* Default/dummy bank referenced by memory.cpp. Inert reads/writes. */
static uae_u32 REGPARAM2 dmmy_lget(uaecptr){ return 0; }
static uae_u32 REGPARAM2 dmmy_wget(uaecptr){ return 0; }
static uae_u32 REGPARAM2 dmmy_bget(uaecptr){ return 0; }
static void REGPARAM2 dmmy_lput(uaecptr, uae_u32){ }
static void REGPARAM2 dmmy_wput(uaecptr, uae_u32){ }
static void REGPARAM2 dmmy_bput(uaecptr, uae_u32){ }
addrbank dmmy_bank = {
	dmmy_lget, dmmy_wget, dmmy_bget,
	dmmy_lput, dmmy_wput, dmmy_bput,
	NULL, NULL, NULL, _T("dmmy"), _T("Dummy"),
	dmmy_lget, dmmy_wget,
	ABFLAG_NONE, 0, 0
};

/* =====================================================================
 *  A3000-SCSI emulation seams — referenced by the portable MMU core
 *  (newcpu.cpp INT2 completion pump + cpummu030.cpp AMIX gate) but
 *  defined in a3000_scsi.cpp / main.cc, which this harness excludes.
 *  Stubbed inert so the MMU harness links on the A3000-SCSI base.
 * ===================================================================== */
extern "C" {
volatile int a3000_amix_mode = 0;          /* AMIX (A3000 SCSI) mode — off for MMU tests */
volatile int amix_mmu_on = 0;
volatile uae_u32 amix_compl_tick[16], amix_compl_istate[16], amix_compl_csr[16],
                 amix_compl_unit[16], amix_compl_head[16];
volatile int amix_compl_h = 0;
volatile uae_u32 amix_scmd_unit[16], amix_scmd_lba[16], amix_scmd_n[16], amix_scmd_w[16];
volatile int amix_scmd_head = 0;
volatile uae_u32 amix_wcmd_cmd[16], amix_wcmd_dest[16], amix_wcmd_ph[16];
volatile int amix_wcmd_head = 0;
void a3000_scsi_hsync(void){ }
void a3000_scsi_dumpstate(void){ }
void a3000_scsi_dumpqueue(void){ }
}
/* plain C++ linkage (defined in a3000_scsi.cpp / main.cc on target) */
volatile uae_u32 amix_tick = 0;
volatile int a3000_scsi_irq = 0;
int ipl_read = 0;
