/*
* UAE - The Un*x Amiga Emulator
*
* MC68000 emulation
*
* Copyright 1995 Bernd Schmidt
*/

#ifndef UAE_NEWCPU_H
#define UAE_NEWCPU_H

#include "uae/types.h"
#include "readcpu.h"
#include "memory.h"
#include "machdep/m68k.h"
#include "sysconfig.h"
//#include "events.h"
//#include "thread.h"

extern const int areg_byteinc[];
extern const int imm8_table[];

extern int movem_index1[256];
extern int movem_index2[256];
extern int movem_next[256];

extern int hardware_bus_error;

typedef uae_u32 REGPARAM3 cpuop_func(uae_u32) REGPARAM;
typedef void REGPARAM3 cpuop_func_ce(uae_u32) REGPARAM;

struct cputbl {
	cpuop_func* handler_ff;
	uae_u16 opcode;
	uae_s8 length;
	uae_s8 disp020[2];
	uae_s8 branch;
};

#ifdef JIT
#define MIN_JIT_CACHE 128
#define MAX_JIT_CACHE 32768
typedef uae_u32 REGPARAM3 compop_func(uae_u32) REGPARAM;

#define COMP_OPCODE_ISJUMP      0x00000001
#define COMP_OPCODE_LONG_OPCODE 0x00000002
#define COMP_OPCODE_CMOV        0x00000004
#define COMP_OPCODE_ISADDX      0x00000008
#define COMP_OPCODE_ISCJUMP     0x00000010
#define COMP_OPCODE_USES_FPU    0x00000020
#define COMP_BAD                0x40000000

struct comptbl {
	compop_func* handler;
   uae_u32 specific;
	uae_u32 opcode;
};
#endif

extern uae_u32 REGPARAM3 op_illg(uae_u32) REGPARAM;

typedef uae_u8 flagtype;

#ifdef FPUEMU

typedef double fptype;
#endif

struct mmufixup
{
	int reg;
	uae_u32 value;
};
extern struct mmufixup mmufixup[2];   /* idx 1 used by cpuemu_32 / mmu030 fixup (WinUAE 4.4.0) */

typedef struct
{
	fptype fp;
} fpdata;

#ifdef JIT
#include "jit/comptbl.h"
#include "jit/compemu.h"
#endif

/* 020/030 prefetch pipeline depth — needed by the WinUAE 4.4.0 MMU engine
 * (cpummu030.cpp) imported for UAE_030_MMU. */
#ifndef CPU_PIPELINE_MAX
#define CPU_PIPELINE_MAX 4
#endif

struct regstruct
{
	uae_u32 regs[16];
	struct flag_struct ccrflags;

	uae_u32 pc;
	uae_u8* pc_p;
	uae_u8* pc_oldp;
	uae_u16 opcode;
	uae_u32 instruction_pc;
	uae_u32 trace_pc;

	uae_u16 irc, ir;
	volatile uae_atomic spcflags;
	uae_u32 chipset_latch_rw;
	uae_u16 db, write_buffer, read_buffer;
	int instruction_cnt;

	uaecptr usp, isp, msp;
	uae_u16 sr;
	flagtype t1;
	flagtype t0;
	flagtype s;
	flagtype m;
	flagtype x;
	flagtype stopped;
	int halted;
	int exception;
	int intmask;
	int ipl, ipl_pin;

	uae_u32 vbr, sfc, dfc;

#ifdef FPUEMU
	fpdata fp[8];
#ifdef JIT
	fpdata fp_result;
#endif
	uae_u32 fpcr, fpsr, fpiar;
	uae_u32 fpu_state;
	uae_u32 fpu_exp_state;
	uae_u16 fp_opword;
	uaecptr fp_ea;
	bool fp_ea_set;
	uae_u32 fp_exp_pend, fp_unimp_pend;
	bool fpu_exp_pre;
	bool fp_unimp_ins;
	bool fp_exception;
	bool fp_branch;
#endif
	uae_u32 cacr, caar;
	uae_u32 itt0, itt1, dtt0, dtt1;
	uae_u32 tcr, mmusr, urp, srp;
	uae_u32 mmu_fault_addr;

	/* MMU engine state required by the imported WinUAE 4.4.0 cpummu030.cpp /
	 * cpummu.cpp (UAE_030_MMU). Field types/order verbatim from WinUAE 4.4.0
	 * newcpu.h so the engine and its (gencpu-generated) cpuemu_31 agree. */
	uae_u32 mmu_fslw;            /* 060 fault status longword */
	uae_u32 mmu_effective_addr;  /* EA at fault */
	uae_u16 mmu_ssw;             /* 030/040 special status word (bus-error frame) */
	uae_u32 wb2_address;
	uae_u32 wb3_data;
	uae_u8  wb3_status, wb2_status;
	int     mmu_enabled;         /* per-regs MMU-active fast-path flag */
	int     mmu_page_size;       /* cached from TC */
	uae_u16 prefetch020[CPU_PIPELINE_MAX];
	uae_u8  prefetch020_valid[CPU_PIPELINE_MAX];
	int     pipeline_pos;
	int     pipeline_r8[2];
	int     pipeline_stop;
	uae_u8  fc030;               /* function code for 030 bus cycle */

	uae_u32 pcr;
	uae_u32 address_space_mask;

	uae_u8* natmem_offset;

	/* Cycle countdown ("countdown" macro == regs.pissoff in events.h). Used by
	 * non-JIT paths (do_extra_cycles, events.cpp) too, so it must exist even when
	 * JIT is off — e.g. the x86-64 host MMU harness. Previously JIT-only, which
	 * compiled only because the ARM target always builds with JIT enabled. */
	int pissoff;

#ifdef JIT
	/* store scratch regs also in this struct to avoid load of mem pointer */
	uae_u32 scratchregs[VREGS - S1];
	fpu_register scratchfregs[VFREGS - 8];
	uae_u32 jit_exception;

	/* pointer to real arrays/structs for easier access in JIT */
	uae_u32* raw_cputbl_count;
	uintptr mem_banks;
	uintptr cache_tags;

#endif
};

extern struct regstruct regs;

#define MAX_CPUTRACESIZE 128
struct cputracememory
{
	uae_u32 addr;
	uae_u32 data;
	int mode;
};

struct cputracestruct
{
	uae_u32 regs[16];
	uae_u32 usp, isp, pc;
	uae_u16 ir, irc, sr, opcode;
	int intmask, stopped, state;

	uae_u32 msp, vbr;
	uae_u32 cacr, caar;
	uae_u16 read_buffer, write_buffer;

	uae_u32 startcycles;
	int needendcycles;
	int memoryoffset;
	int cyclecounter, cyclecounter_pre, cyclecounter_post;
	int readcounter, writecounter;
	struct cputracememory ctm[MAX_CPUTRACESIZE];
};

#define REGS_DEFINED
#include "machdep/m68k.h"
#include "events.h"

STATIC_INLINE uae_u32 munge24(uae_u32 x)
{
	return x & regs.address_space_mask;
}

extern int cpu_cycles;
extern int m68k_pc_indirect;
extern bool m68k_interrupt_delay;

extern void safe_interrupt_set(int, int, bool);

STATIC_INLINE void set_special_exter(uae_u32 x)
{
	__atomic_or_fetch(&regs.spcflags, x, __ATOMIC_SEQ_CST);
}
STATIC_INLINE void set_special(uae_u32 x)
{
	__atomic_or_fetch(&regs.spcflags, x, __ATOMIC_SEQ_CST);
	//cycles_do_special();
}

STATIC_INLINE void unset_special(uae_u32 x)
{
	__atomic_and_fetch(&regs.spcflags, ~x, __ATOMIC_SEQ_CST);
}

#define m68k_dreg(r,num) ((r).regs[(num)])
#define m68k_areg(r,num) (((r).regs + 8)[(num)])

extern uae_u32(*x_prefetch)(int);   /* WinUAE 4.4.0 MMU engine (cpummu030.cpp) */
extern uae_u32(*x_get_byte)(uaecptr addr);
extern uae_u32(*x_get_word)(uaecptr addr);
extern uae_u32(*x_get_long)(uaecptr addr);
extern void(*x_put_byte)(uaecptr addr, uae_u32 v);
extern void(*x_put_word)(uaecptr addr, uae_u32 v);
extern void(*x_put_long)(uaecptr addr, uae_u32 v);
extern uae_u32(*x_next_iword)(void);
extern uae_u32(*x_next_ilong)(void);
extern uae_u32(*x_get_iword)(int);

extern uae_u32(*x_cp_get_byte)(uaecptr addr);
extern uae_u32(*x_cp_get_word)(uaecptr addr);
extern uae_u32(*x_cp_get_long)(uaecptr addr);
extern void(*x_cp_put_byte)(uaecptr addr, uae_u32 v);
extern void(*x_cp_put_word)(uaecptr addr, uae_u32 v);
extern void(*x_cp_put_long)(uaecptr addr, uae_u32 v);
extern uae_u32(*x_cp_next_iword)(void);
extern uae_u32(*x_cp_next_ilong)(void);

#define x_cp_get_disp_ea_020(base) x_get_disp_ea_020(base)

/* direct (regs.pc_p) access */

STATIC_INLINE void m68k_setpc(uaecptr newpc)
{
	regs.pc_p = regs.pc_oldp = get_real_address(newpc);
	regs.instruction_pc = regs.pc = newpc;
}
STATIC_INLINE void m68k_setpc_j(uaecptr newpc)
{
	regs.pc_p = regs.pc_oldp = get_real_address(newpc);
	regs.pc = newpc;
}
STATIC_INLINE uaecptr m68k_getpc(void)
{
	return (uaecptr)(regs.pc + ((uae_u8*)regs.pc_p - (uae_u8*)regs.pc_oldp));
}
#define M68K_GETPC m68k_getpc()
STATIC_INLINE void m68k_incpc(int o)
{
	regs.pc_p += o;
}

STATIC_INLINE uae_u32 get_dibyte(int o)
{
	return do_get_mem_byte((uae_u8*)((regs).pc_p + (o)+1));
}
STATIC_INLINE uae_u32 get_diword(int o)
{
	return do_get_mem_word((uae_u16*)((regs).pc_p + (o)));
}
STATIC_INLINE uae_u32 get_dilong(int o)
{
	return do_get_mem_long((uae_u32*)((regs).pc_p + (o)));
}
STATIC_INLINE uae_u32 next_diword(void)
{
	uae_u32 r = do_get_mem_word((uae_u16*)((regs).pc_p));
	m68k_incpc(2);
	return r;
}
STATIC_INLINE uae_u32 next_dilong(void)
{
	uae_u32 r = do_get_mem_long((uae_u32*)((regs).pc_p));
	m68k_incpc(4);
	return r;
}

STATIC_INLINE void m68k_do_bsr(uaecptr oldpc, uae_s32 offset)
{
	m68k_areg(regs, 7) -= 4;
	put_long(m68k_areg(regs, 7), oldpc);
	m68k_incpc(offset);
}
STATIC_INLINE void m68k_do_rts(void)
{
	uae_u32 newpc = get_long(m68k_areg(regs, 7));
	m68k_setpc(newpc);
	m68k_areg(regs, 7) += 4;
}

/* indirect (regs.pc) access */

STATIC_INLINE void m68k_setpci(uaecptr newpc)
{
	regs.instruction_pc = regs.pc = newpc;
}
STATIC_INLINE void m68k_setpci_j(uaecptr newpc)
{
	regs.pc = newpc;
}
STATIC_INLINE uaecptr m68k_getpci(void)
{
	return regs.pc;
}
STATIC_INLINE void m68k_incpci(int o)
{
	regs.pc += o;
}

STATIC_INLINE uae_u32 get_iiword(int o)
{
	return get_wordi(m68k_getpci() + (o));
}
STATIC_INLINE uae_u32 get_iilong(int o)
{
	return get_longi(m68k_getpci() + (o));
}

STATIC_INLINE uae_u32 next_iiword(void)
{
	uae_u32 r = get_iiword(0);
	m68k_incpci(2);
	return r;
}
STATIC_INLINE uae_u32 next_iilong(void)
{
	uae_u32 r = get_iilong(0);
	m68k_incpci(4);
	return r;
}

/* common access */

STATIC_INLINE void m68k_incpc_normal(int o)
{
	if (m68k_pc_indirect > 0)
		m68k_incpci(o);
	else
		m68k_incpc(o);
}

STATIC_INLINE void m68k_setpc_normal(uaecptr pc)
{
	if (m68k_pc_indirect > 0) {
		regs.pc_p = regs.pc_oldp = 0;
		m68k_setpci(pc);
	} else {
		m68k_setpc(pc);
	}
}

extern void check_t0_trace(void);

/* 030/040 MMU + cache memory accessors, imported from WinUAE 4.4.0 newcpu.h for
 * UAE_030_MMU. The 030 function pointers and the dcache030/icache functions are
 * referenced by the inline wrappers in cpummu030.h and cpummu.h; defined in
 * cpummu030.cpp and newcpu.cpp. (Stripped from this tree's faked-MMU build.) */
extern uae_u32(*read_data_030_bget)(uaecptr);
extern uae_u32(*read_data_030_wget)(uaecptr);
extern uae_u32(*read_data_030_lget)(uaecptr);
extern void(*write_data_030_bput)(uaecptr,uae_u32);
extern void(*write_data_030_wput)(uaecptr,uae_u32);
extern void(*write_data_030_lput)(uaecptr,uae_u32);

extern uae_u32(*read_data_030_fc_bget)(uaecptr, uae_u32);
extern uae_u32(*read_data_030_fc_wget)(uaecptr, uae_u32);
extern uae_u32(*read_data_030_fc_lget)(uaecptr, uae_u32);
extern void(*write_data_030_fc_bput)(uaecptr, uae_u32, uae_u32);
extern void(*write_data_030_fc_wput)(uaecptr, uae_u32, uae_u32);
extern void(*write_data_030_fc_lput)(uaecptr, uae_u32, uae_u32);

extern void write_dcache030_bput(uaecptr, uae_u32, uae_u32);
extern void write_dcache030_wput(uaecptr, uae_u32, uae_u32);
extern void write_dcache030_lput(uaecptr, uae_u32, uae_u32);
extern void write_dcache030_retry(uaecptr addr, uae_u32 v, uae_u32 fc, int size, int flags);
extern uae_u32 read_dcache030_bget(uaecptr, uae_u32);
extern uae_u32 read_dcache030_wget(uaecptr, uae_u32);
extern uae_u32 read_dcache030_lget(uaecptr, uae_u32);
extern uae_u32 read_dcache030_retry(uaecptr addr, uae_u32 fc, int size, int flags);

extern void write_dcache030_mmu_bput(uaecptr, uae_u32);
extern void write_dcache030_mmu_wput(uaecptr, uae_u32);
extern void write_dcache030_mmu_lput(uaecptr, uae_u32);
extern uae_u32 read_dcache030_mmu_bget(uaecptr);
extern uae_u32 read_dcache030_mmu_wget(uaecptr);
extern uae_u32 read_dcache030_mmu_lget(uaecptr);
extern void write_dcache030_lrmw_mmu(uaecptr, uae_u32, uae_u32);
extern void write_dcache030_lrmw_mmu_fcx(uaecptr, uae_u32, uae_u32, int);
extern uae_u32 read_dcache030_lrmw_mmu(uaecptr, uae_u32);
extern uae_u32 read_dcache030_lrmw_mmu_fcx(uaecptr, uae_u32, int);

extern uae_u32 get_word_icache030(uaecptr addr);
extern uae_u32 get_long_icache030(uaecptr addr);

uae_u32 fill_icache040(uae_u32 addr);
extern void put_long_cache_040(uaecptr, uae_u32);
extern void put_word_cache_040(uaecptr, uae_u32);
extern void put_byte_cache_040(uaecptr, uae_u32);
extern uae_u32 get_ilong_cache_040(int);
extern uae_u32 get_iword_cache_040(int);
extern uae_u32 get_long_cache_040(uaecptr);
extern uae_u32 get_word_cache_040(uaecptr);
extern uae_u32 get_byte_cache_040(uaecptr);
extern uae_u32 next_iword_cache040(void);
extern uae_u32 next_ilong_cache040(void);
extern uae_u32 get_word_icache040(uaecptr addr);
extern uae_u32 get_long_icache040(uaecptr addr);

extern uae_u32 sfc_nommu_get_byte(uaecptr);
extern uae_u32 sfc_nommu_get_word(uaecptr);
extern uae_u32 sfc_nommu_get_long(uaecptr);
extern void dfc_nommu_put_byte(uaecptr, uae_u32);
extern void dfc_nommu_put_word(uaecptr, uae_u32);
extern void dfc_nommu_put_long(uaecptr, uae_u32);

extern void (*x_do_cycles)(int);
extern void (*x_do_cycles_pre)(int);
extern void (*x_do_cycles_post)(int, uae_u32);

extern uae_u32 REGPARAM3 x_get_disp_ea_020(uae_u32 base) REGPARAM;

extern void m68k_setstopped(void);
extern void m68k_resumestopped(void);
extern void m68k_cancel_idle(void);

extern uae_u32 REGPARAM3 get_disp_ea_020(uae_u32 base) REGPARAM;
extern uae_u32 REGPARAM3 get_bitfield(uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width) REGPARAM;
extern void REGPARAM3 put_bitfield(uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width) REGPARAM;
/* MMU-named aliases used by the generated cpuemu_32 (68030 MMU). Pure bit ops on
 * already-loaded bdata[] (no memory access), so they just forward. */
extern uae_u32 REGPARAM3 x_get_bitfield(uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width) REGPARAM;
extern void REGPARAM3 x_put_bitfield(uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width) REGPARAM;

extern int get_cpu_model(void);

extern void set_cpu_caches(bool flush);
extern void flush_cpu_caches_040(uae_u16 opcode);
extern void REGPARAM3 MakeSR(void) REGPARAM;
extern void REGPARAM3 MakeFromSR(void) REGPARAM;
extern void REGPARAM3 MakeFromSR_T0(void) REGPARAM;
extern void REGPARAM3 Exception(int) REGPARAM;
extern void REGPARAM3 Exception_cpu(int) REGPARAM;
extern void REGPARAM3 Exception_cpu_oldpc(int, uaecptr) REGPARAM;
extern void NMI (void);
extern void IRQ_forced(int, int);
extern void prepare_interrupt (uae_u32);
extern void doint (void);
extern int m68k_move2c(int, uae_u32*);
extern int m68k_movec2(int, uae_u32*);
extern int m68k_divl(uae_u32, uae_u32, uae_u16, uaecptr);
extern int m68k_mull(uae_u32, uae_u32, uae_u16);
/* The WinUAE 4.4.0 gencpu (cpuemu_32) emits 3-arg m68k_divl; this tree's real one
 * takes the restart pc explicitly. Forward to it with the instruction pc. */
static inline int m68k_divl(uae_u32 opcode, uae_u32 dst, uae_u16 extra)
{ return m68k_divl(opcode, dst, extra, regs.instruction_pc); }
/* Inert on the Z3660: the SR-write handlers' following MakeFromSR_T0() ->
 * MakeFromSR_x() -> doint_imm() unconditionally sets SPCFLAG_INT in the no-JIT
 * path (cachesize==0), forcing do_specialties() to re-check intlev() against the
 * NEW intmask on the next pass. (The regs.ipl_pin fold-in in MakeFromSR_x is NOT
 * the guarantee — ipl_pin is not refreshed per-instruction in m68k_run_mmu030.) */
static inline void MakeFromSR_intmask(uae_u16 oldsr, uae_u16 newsr) { (void)oldsr; (void)newsr; }
extern void init_m68k(void);
extern void m68k_go(int);
extern int getMulu68kCycles(uae_u16 src);
extern int getMuls68kCycles(uae_u16 src);
extern int getDivu68kCycles(uae_u32 dividend, uae_u16 divisor);
extern int getDivs68kCycles(uae_s32 dividend, uae_s16 divisor);
extern void divbyzero_special(bool issigned, uae_s32 dst);
extern void setdivuflags(uae_u32 dividend, uae_u16 divisor);
extern void setdivsflags(uae_s32 dividend, uae_s16 divisor);
extern void setchkundefinedflags(uae_s32 src, uae_s32 dst, int size);
extern void setchk2undefinedflags(uae_s32 lower, uae_s32 upper, uae_s32 val, int size);
extern void protect_roms(bool);
extern void unprotect_maprom(void);
extern bool is_hardreset(void);
extern bool is_keyboardreset(void);
extern void Exception_build_stack_frame_common(uae_u32 oldpc, uae_u32 currpc, int nr);
extern void Exception_build_stack_frame(uae_u32 oldpc, uae_u32 currpc, uae_u32 ssw, int nr, int format);
extern void Exception_build_68000_address_error_stack_frame(uae_u16 mode, uae_u16 opcode, uaecptr fault_addr, uaecptr pc);
extern uae_u32 exception_pc(int nr);
extern void cpu_restore_fixup(void);
extern bool privileged_copro_instruction(uae_u16 opcode);
extern bool generates_group1_exception(uae_u16 opcode);

void ccr_68000_long_move_ae_LZN(uae_s32 src);
void ccr_68000_long_move_ae_LN(uae_s32 src);
void ccr_68000_long_move_ae_HNZ(uae_s32 src);
void ccr_68000_long_move_ae_normal(uae_s32 src);
void ccr_68000_word_move_ae_normal(uae_s16 src);
void dreg_68000_long_replace_low(int reg, uae_u16 v);
void areg_68000_long_replace_low(int reg, uae_u16 v);

extern void mmu_op(uae_u32, uae_u32);
extern bool mmu_op30(uaecptr, uae_u32, uae_u16, uaecptr);

extern void fpuop_arithmetic(uae_u32, uae_u16);
extern void fpuop_dbcc(uae_u32, uae_u16);
extern void fpuop_scc(uae_u32, uae_u16);
extern void fpuop_trapcc(uae_u32, uaecptr, uae_u16);
extern void fpuop_bcc(uae_u32, uaecptr, uae_u32);
extern void fpuop_save(uae_u32);
extern void fpuop_restore(uae_u32);
extern void fpu_reset(void);

extern void exception3_read(uae_u32 opcode, uaecptr addr, int size, int fc);
extern void exception3_write(uae_u32 opcode, uaecptr addr, int size, uae_u32 val, int fc);
extern void exception3_read_access(uae_u32 opcode, uaecptr addr, int size, int fc);
extern void exception3_read_access2(uae_u32 opcode, uaecptr addr, int size, int fc);
extern void exception3_write_access(uae_u32 opcode, uaecptr addr, int size, uae_u32 val, int fc);
extern void exception3_read_prefetch(uae_u32 opcode, uaecptr addr);
extern void exception3_read_prefetch_68040bug(uae_u32 opcode, uaecptr addr, uae_u16 secondarysr);
extern void exception3_read_prefetch_only(uae_u32 opcode, uaecptr addr);
extern void hardware_exception2(uaecptr addr, uae_u32 v, bool read, bool ins, int size);
extern void exception2_setup(uae_u32 opcode, uaecptr addr, bool read, int size, uae_u32 fc);
extern void exception2_read(uae_u32 opcode, uaecptr addr, int size, int fc);
extern void exception2_write(uae_u32 opcode, uaecptr addr, int size, uae_u32 val, int fc);
extern void exception2_fetch_opcode(uae_u32 opcode, int offset, int pcoffset);
extern void exception2_fetch(uae_u32 opcode, int offset, int pcoffset);
extern void m68k_reset(void);
extern bool cpureset(void);
extern void cpu_halt(int id);
extern void cpu_inreset(void);
extern int cpu_sleep_millis(int ms);
extern void cpu_change(int newmodel);
extern void cpu_fallback(int mode);

extern void fill_prefetch(void);
extern void fill_prefetch_020_ntx(void);   /* WinUAE 4.4.0 MMU engine */
extern void fill_prefetch_030_ntx(void);
extern void fill_prefetch_030_ntx_continue(void);
extern void fill_prefetch_020(void);
extern void fill_prefetch_030(void);

/* Cycle-exact memory-access-with-delay helpers (WinUAE 4.4.0 newcpu.h). The
 * imported MMU engine references these in its CE paths; the Z3660 runs the MMU
 * in direct (non-CE) mode so they are not on the hot path, but must resolve.
 * Defined as pass-throughs in newcpu_mmu_glue.cpp. */
void mem_access_delay_long_write_ce020 (uaecptr addr, uae_u32 v);
void mem_access_delay_word_write_ce020 (uaecptr addr, uae_u32 v);
void mem_access_delay_byte_write_ce020 (uaecptr addr, uae_u32 v);
uae_u32 mem_access_delay_byte_read_ce020 (uaecptr addr);
uae_u32 mem_access_delay_word_read_ce020 (uaecptr addr);
uae_u32 mem_access_delay_long_read_ce020 (uaecptr addr);
uae_u32 mem_access_delay_longi_read_ce020 (uaecptr addr);
uae_u32 mem_access_delay_wordi_read_ce020 (uaecptr addr);
void mem_access_delay_long_write_c040 (uaecptr addr, uae_u32 v);
void mem_access_delay_word_write_c040 (uaecptr addr, uae_u32 v);
void mem_access_delay_byte_write_c040 (uaecptr addr, uae_u32 v);
uae_u32 mem_access_delay_byte_read_c040 (uaecptr addr);
uae_u32 mem_access_delay_word_read_c040 (uaecptr addr);
uae_u32 mem_access_delay_long_read_c040 (uaecptr addr);
uae_u32 mem_access_delay_longi_read_c040 (uaecptr addr);

#define CPU_OP_NAME(a) op ## a

/* 68040 */
extern const struct cputbl op_smalltbl_1[];
extern const struct cputbl op_smalltbl_41[];
/* 68030 */
extern const struct cputbl op_smalltbl_2[];
extern const struct cputbl op_smalltbl_42[];
/* 68020 */
extern const struct cputbl op_smalltbl_3[];
extern const struct cputbl op_smalltbl_43[];
/* 68010 */
extern const struct cputbl op_smalltbl_4[];
extern const struct cputbl op_smalltbl_44[];
extern const struct cputbl op_smalltbl_11[]; // prefetch
extern const struct cputbl op_smalltbl_13[]; // CE
/* 68000 */
extern const struct cputbl op_smalltbl_5[];
extern const struct cputbl op_smalltbl_45[];
extern const struct cputbl op_smalltbl_12[]; // prefetch
extern const struct cputbl op_smalltbl_14[]; // CE

extern cpuop_func* cpufunctbl[65536] ASM_SYM_FOR_FUNC("cpufunctbl");

#ifdef JIT
extern void flush_icache(int);
extern void flush_icache_hard(int);
extern void compemu_reset(void);
#else
#define flush_icache(int) do {} while (0)
#define flush_icache_hard(int) do {} while (0)
#endif
bool check_prefs_changed_comp(bool);

extern bool is_cpu_tracer(void);
extern bool set_cpu_tracer(bool force);
extern bool can_cpu_tracer(void);

#define CPU_HALT_PPC_ONLY -1
#define CPU_HALT_BUS_ERROR_DOUBLE_FAULT 1
#define CPU_HALT_DOUBLE_FAULT 2
#define CPU_HALT_OPCODE_FETCH_FROM_NON_EXISTING_ADDRESS 3
#define CPU_HALT_ACCELERATOR_CPU_FALLBACK 4
#define CPU_HALT_ALL_CPUS_STOPPED 5
#define CPU_HALT_FAKE_DMA 6
#define CPU_HALT_AUTOCONFIG_CONFLICT 7
#define CPU_HALT_PCI_CONFLICT 8
#define CPU_HALT_CPU_STUCK 9
#define CPU_HALT_SSP_IN_NON_EXISTING_ADDRESS 10
#define CPU_HALT_INVALID_START_ADDRESS 11
#define CPU_HALT_68060_HALT 12
#define CPU_HALT_BKPT 13

uae_u32 process_cpu_indirect_memory_read(uae_u32 addr, int size);
void process_cpu_indirect_memory_write(uae_u32 addr, uae_u32 data, int size);

#endif /* UAE_NEWCPU_H */
