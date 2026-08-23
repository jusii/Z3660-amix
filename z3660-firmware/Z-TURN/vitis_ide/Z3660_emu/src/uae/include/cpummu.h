/*
 * cpummu.h - MMU emulation
 *
 * Copyright (c) 2001-2004 Milan Jurik of ARAnyM dev team (see AUTHORS)
 * 
 * Inspired by UAE MMU patch
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * ARAnyM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ARAnyM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ARAnyM; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#ifndef UAE_CPUMMU_H
#define UAE_CPUMMU_H

#include "uae/types.h"

#define MMU_ICACHE 0
#define MMU_IPAGECACHE 1
#define MMU_DPAGECACHE 1

/* CYBERSTORM C1. Upstream already carries the hit/miss branch scaffolding for the two
 * page caches -- correctly placed on every hit and every miss arm below -- and only
 * switches it off. The profiling build switches it on; the default and lean builds get
 * the literal 0 they have always had, so their generated code is unchanged.
 *
 * It stays OUT of the default build on purpose. An increment looks free, but this is a
 * read-modify-write on every guest memory access, and C3b's metal calibration priced the
 * per-instruction 64-bit accumulator -- a strictly smaller amount of the same work -- at
 * 2.0 % of guest throughput. Adding it back to the shipping loop would undo the thing
 * C3b just finished proving. */
#include "z3660_prof.h"
/* CYBERSTORM C2 rung 1: the decoded-op cache publishes its one extension-word window
 * here, so the single out-of-line copy of uae_mmu040_get_iword() below can answer from
 * it. Nothing else in this header depends on it. */
#include "z3660_dopc.h"
/* CYBERSTORM C2 rung 2: the data accessors' fast path. Read that header first -- it is
 * where the design, the measurement that motivates it, and the coherence argument live. */
#include "z3660_dfast.h"
#if Z3660_EMU_PROF
#define CACHE_HIT_COUNT 1
#else
#define CACHE_HIT_COUNT 0
#endif

#include "mmu_common.h"

#ifndef FULLMMU
#define FULLMMU
#endif

#define DUNUSED(x)
#define D
#if DEBUG
#define bug write_log
#else
#define bug
#endif

static __inline void flush_internals (void) { }

extern int mmu060_state;

extern int mmu040_movem;
extern uaecptr mmu040_movem_ea;
extern uae_u32 mmu040_move16[4];

extern bool mmu_pagesize_8k;
extern int mmu_pageshift, mmu_pageshift1m;
extern uae_u16 mmu_opcode;
extern bool mmu_restart;
extern bool mmu_ttr_enabled, mmu_ttr_enabled_ins, mmu_ttr_enabled_data;
extern bool rmw_cycle;
extern uae_u8 mmu_cache_state;
extern uae_u8 cache_default_ins, cache_default_data;

extern void mmu_dump_tables(void);

#define MMU_TTR_LOGICAL_BASE				0xff000000
#define MMU_TTR_LOGICAL_MASK				0x00ff0000
#define MMU_TTR_BIT_ENABLED					(1 << 15)
#define MMU_TTR_BIT_SFIELD_ENABLED			(1 << 14)
#define MMU_TTR_BIT_SFIELD_SUPER			(1 << 13)
#define MMU_TTR_SFIELD_SHIFT				13
#define MMU_TTR_UX_MASK						((1 << 9) | (1 << 8))
#define MMU_TTR_UX_SHIFT					8
#define MMU_TTR_CACHE_MASK					((1 << 6) | (1 << 5))
#define MMU_TTR_CACHE_SHIFT					5
#define MMU_TTR_CACHE_DISABLE				(1 << 6)
#define MMU_TTR_CACHE_MODE					(1 << 5)
#define MMU_TTR_BIT_WRITE_PROTECT			(1 << 2)

#define MMU_UDT_MASK	3
#define MMU_PDT_MASK	3

#define MMU_DES_WP			4
#define MMU_DES_USED		8

/* page descriptors only */
#define MMU_DES_MODIFIED	16
#define MMU_DES_SUPER		(1 << 7)
#define MMU_DES_GLOBAL		(1 << 10)

#define MMU_ROOT_PTR_ADDR_MASK			0xfffffe00
#define MMU_PTR_PAGE_ADDR_MASK_8		0xffffff80
#define MMU_PTR_PAGE_ADDR_MASK_4		0xffffff00

#define MMU_PAGE_INDIRECT_MASK			0xfffffffc
#define MMU_PAGE_ADDR_MASK_8			0xffffe000
#define MMU_PAGE_ADDR_MASK_4			0xfffff000
#define MMU_PAGE_UR_MASK_8				((1 << 12) | (1 << 11))
#define MMU_PAGE_UR_MASK_4				(1 << 11)
#define MMU_PAGE_UR_SHIFT				11

#define MMU_MMUSR_ADDR_MASK				0xfffff000
#define MMU_MMUSR_B						(1 << 11)
#define MMU_MMUSR_G						(1 << 10)
#define MMU_MMUSR_U1					(1 << 9)
#define MMU_MMUSR_U0					(1 << 8)
#define MMU_MMUSR_Ux					(MMU_MMUSR_U1 | MMU_MMUSR_U0)
#define MMU_MMUSR_S						(1 << 7)
#define MMU_MMUSR_CM					((1 << 6) | ( 1 << 5))
#define MMU_MMUSR_CM_DISABLE			(1 << 6)
#define MMU_MMUSR_CM_MODE				(1 << 5)
#define MMU_MMUSR_M						(1 << 4)
#define MMU_MMUSR_W						(1 << 2)
#define MMU_MMUSR_T						(1 << 1)
#define MMU_MMUSR_R						(1 << 0)

// 68040 and 68060
#define MMU_TCR_E						0x8000
#define MMU_TCR_P						0x4000
// 68060 only
#define MMU_TCR_NAD						0x2000
#define MMU_TCR_NAI						0x1000
#define MMU_TCR_FOTC					0x0800
#define MMU_TCR_FITC					0x0400
#define MMU_TCR_DCO1					0x0200
#define MMU_TCR_DCO0					0x0100
#define MMU_TCR_DUO1					0x0080
#define MMU_TCR_DUO0					0x0040
#define MMU_TCR_DWO						0x0020
#define MMU_TCR_DCI1					0x0010
#define MMU_TCR_DCI0					0x0008
#define MMU_TCR_DUI1					0x0004
#define MMU_TCR_DUI0					0x0002

#define TTR_I0	4
#define TTR_I1	5
#define TTR_D0	6
#define TTR_D1	7

#define TTR_NO_MATCH	0
#define TTR_NO_WRITE	1
#define TTR_OK_MATCH	2

struct mmu_atc_line {
	uaecptr tag; // tag is 16 or 17 bits S+logical
	uae_u32 valid;
	uae_u32 status;
	uaecptr phys; // phys base address
};

/*
 * 68040 ATC is a 4 way 16 slot associative address translation cache
 * the 68040 has a DATA and an INSTRUCTION ATC.
 * an ATC lookup may result in : a hit, a miss and a modified state.
 * the 68060 can disable ATC allocation
 * we must take care of 8k and 4k page size, index position is relative to page size
 */

#define ATC_WAYS 4
#define ATC_SLOTS 16
#define ATC_TYPE 2

extern uae_u32 mmu_is_super;
extern uae_u32 mmu_tagmask, mmu_pagemask, mmu_pagemaski;
extern struct mmu_atc_line mmu_atc_array[ATC_TYPE][ATC_SLOTS][ATC_WAYS];

extern void mmu_tt_modified(void);
extern int mmu_match_ttr_ins(uaecptr addr, bool super);
extern int mmu_match_ttr(uaecptr addr, bool super, bool data);
extern void mmu_bus_error_ttr_write_fault(uaecptr addr, bool super, bool data, uae_u32 val, int size);
extern int mmu_match_ttr_write(uaecptr addr, bool super, bool data, uae_u32 val, int size);
extern int mmu_match_ttr_maybe_write(uaecptr addr, bool super, bool data, int size, bool write);
extern uaecptr mmu_translate(uaecptr addr, uae_u32 val, bool super, bool data, bool write, int size);
extern void mmu_hardware_bus_error(uaecptr addr, uae_u32 v, bool read, bool ins, int size);
extern bool mmu_is_super_access(bool read);

extern uae_u32 REGPARAM3 mmu060_get_rmw_bitfield (uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width) REGPARAM;
extern void REGPARAM3 mmu060_put_rmw_bitfield (uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width) REGPARAM;

extern uae_u16 REGPARAM3 mmu_get_word_unaligned(uaecptr addr, bool data) REGPARAM;
extern uae_u32 REGPARAM3 mmu_get_long_unaligned(uaecptr addr, bool data) REGPARAM;

extern uae_u32 REGPARAM3 mmu_get_ilong_unaligned(uaecptr addr) REGPARAM;

extern void REGPARAM3 mmu_put_word_unaligned(uaecptr addr, uae_u16 val, bool data) REGPARAM;
extern void REGPARAM3 mmu_put_long_unaligned(uaecptr addr, uae_u32 val, bool data) REGPARAM;

extern void mmu_make_transparent_region(uaecptr baseaddr, uae_u32 size, int datamode);

#define FC_DATA		(regs.s ? 5 : 1)
#define FC_INST		(regs.s ? 6 : 2)

extern void mmu_bus_error(uaecptr addr, uae_u32 val, int fc, bool write, int size, uae_u32 status, bool nonmmu);

extern uae_u32 REGPARAM3 sfc_get_long(uaecptr addr) REGPARAM;
extern uae_u16 REGPARAM3 sfc_get_word(uaecptr addr) REGPARAM;
extern uae_u8 REGPARAM3 sfc_get_byte(uaecptr addr) REGPARAM;
extern void REGPARAM3 dfc_put_long(uaecptr addr, uae_u32 val) REGPARAM;
extern void REGPARAM3 dfc_put_word(uaecptr addr, uae_u16 val) REGPARAM;
extern void REGPARAM3 dfc_put_byte(uaecptr addr, uae_u8 val) REGPARAM;

#define sfc040_get_long sfc_get_long
#define sfc040_get_word sfc_get_word
#define sfc040_get_byte sfc_get_byte
#define dfc040_put_long dfc_put_long
#define dfc040_put_word dfc_put_word
#define dfc040_put_byte dfc_put_byte

#define sfc060_get_long sfc_get_long
#define sfc060_get_word sfc_get_word
#define sfc060_get_byte sfc_get_byte
#define dfc060_put_long dfc_put_long
#define dfc060_put_word dfc_put_word
#define dfc060_put_byte dfc_put_byte

extern void uae_mmu_put_lrmw (uaecptr addr, uae_u32 v, int size, int type);
extern uae_u32 uae_mmu_get_lrmw (uaecptr addr, int size, int type);

extern void REGPARAM3 mmu_flush_atc(uaecptr addr, bool super, bool global) REGPARAM;
extern void REGPARAM3 mmu_flush_atc_all(bool global) REGPARAM;
extern void REGPARAM3 mmu_op_real(uae_u32 opcode, uae_u16 extra) REGPARAM;

extern void REGPARAM3 mmu_reset(void) REGPARAM;
extern void REGPARAM3 mmu_set_funcs(void) REGPARAM;
extern uae_u16 REGPARAM3 mmu_set_tc(uae_u16 tc) REGPARAM;
extern void REGPARAM3 mmu_set_super(bool super) REGPARAM;
extern void REGPARAM3 mmu_flush_cache(void) REGPARAM;

static ALWAYS_INLINE uaecptr mmu_get_real_address(uaecptr addr, struct mmu_atc_line *cl)
{
	return cl->phys | (addr & mmu_pagemask);
}

extern void mmu_get_move16(uaecptr addr, uae_u32 *v, bool data, int size);
extern void mmu_put_move16(uaecptr addr, uae_u32 *val, bool data, int size);

#if MMU_IPAGECACHE
extern uae_u32 atc_last_ins_laddr, atc_last_ins_paddr;
extern uae_u8 atc_last_ins_cache;
#endif

#if MMU_DPAGECACHE
#define MMUFASTCACHE_ENTRIES 256
struct mmufastcache
{
	uae_u32 log;
	uae_u32 phys;
	uae_u8 cache_state;
};
extern struct mmufastcache atc_data_cache_read[MMUFASTCACHE_ENTRIES];
extern struct mmufastcache atc_data_cache_write[MMUFASTCACHE_ENTRIES];
#endif

#if CACHE_HIT_COUNT && !Z3660_EMU_PROF
extern int mmu_ins_hit, mmu_ins_miss;
extern int mmu_data_read_hit, mmu_data_read_miss;
extern int mmu_data_write_hit, mmu_data_write_miss;
#endif
#if Z3660_EMU_PROF
/* Land upstream's six hit/miss increments in the profiler's 64-bit counters instead of
 * upstream's ints. Not a stylistic preference: an int counter overflows after 2.1e9
 * accesses, which a hot interpreter reaches in a few minutes -- shorter than the boot
 * profile this instrument exists to take, and it would wrap silently. Done by macro so
 * the twelve branch sites below stay verbatim upstream code. */
#define mmu_ins_hit         z3660_prof_cnt[Z3660_PROF_C_IPAGE_HIT]
#define mmu_ins_miss        z3660_prof_cnt[Z3660_PROF_C_IPAGE_MISS]
#define mmu_data_read_hit   z3660_prof_cnt[Z3660_PROF_C_DPAGE_RHIT]
#define mmu_data_read_miss  z3660_prof_cnt[Z3660_PROF_C_DPAGE_RMISS]
#define mmu_data_write_hit  z3660_prof_cnt[Z3660_PROF_C_DPAGE_WHIT]
#define mmu_data_write_miss z3660_prof_cnt[Z3660_PROF_C_DPAGE_WMISS]
#endif

static ALWAYS_INLINE uae_u32 mmu_get_ilong(uaecptr addr, int size)
{
	mmu_cache_state = cache_default_ins;
	if ((!mmu_ttr_enabled_ins || mmu_match_ttr_ins(addr,regs.s!=0) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_IPAGECACHE
		if (((addr & mmu_pagemaski) | regs.s) == atc_last_ins_laddr) {
#if CACHE_HIT_COUNT
			mmu_ins_hit++;
#endif
			addr = atc_last_ins_paddr | (addr & mmu_pagemask);
			mmu_cache_state = atc_last_ins_cache;
		} else {
#if CACHE_HIT_COUNT
			mmu_ins_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, false, false, size);
#if MMU_IPAGECACHE
		}
#endif
	}
	return x_phys_get_ilong(addr);
}

static ALWAYS_INLINE uae_u16 mmu_get_iword(uaecptr addr, int size)
{
	mmu_cache_state = cache_default_ins;
	if ((!mmu_ttr_enabled_ins || mmu_match_ttr_ins(addr,regs.s!=0) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_IPAGECACHE
		if (((addr & mmu_pagemaski) | regs.s) == atc_last_ins_laddr) {
#if CACHE_HIT_COUNT
			mmu_ins_hit++;
#endif
			addr = atc_last_ins_paddr | (addr & mmu_pagemask);
			mmu_cache_state = atc_last_ins_cache;
		} else {
#if CACHE_HIT_COUNT
			mmu_ins_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, false, false, size);
#if MMU_IPAGECACHE
		}
#endif
	}
	return x_phys_get_iword(addr);
}


static ALWAYS_INLINE uae_u32 mmu_get_long(uaecptr addr, bool data, int size)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr(addr,regs.s!=0,data) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, data, false, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	return x_phys_get_long(addr);
}

static ALWAYS_INLINE uae_u16 mmu_get_word(uaecptr addr, bool data, int size)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr(addr,regs.s!=0,data) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, data, false, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	return x_phys_get_word(addr);
}

static ALWAYS_INLINE uae_u8 mmu_get_byte(uaecptr addr, bool data, int size)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr(addr,regs.s!=0,data) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, data, false, size);
#if MMU_DPAGECACHE
		}
#endif
}
	return x_phys_get_byte(addr);
}

static ALWAYS_INLINE void mmu_put_long(uaecptr addr, uae_u32 val, bool data, int size)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,regs.s!=0,data,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, regs.s!=0, data, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	x_phys_put_long(addr, val);
}

static ALWAYS_INLINE void mmu_put_word(uaecptr addr, uae_u16 val, bool data, int size)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,regs.s!=0,data,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, regs.s!=0, data, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	x_phys_put_word(addr, val);
}

static ALWAYS_INLINE void mmu_put_byte(uaecptr addr, uae_u8 val, bool data, int size)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,regs.s!=0,data,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, regs.s!=0, data, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	x_phys_put_byte(addr, val);
}

static ALWAYS_INLINE uae_u32 mmu_get_user_long(uaecptr addr, bool super, bool write, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_maybe_write(addr,super,true,size,write) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, super, true, write, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	return x_phys_get_long(addr);
}

static ALWAYS_INLINE uae_u16 mmu_get_user_word(uaecptr addr, bool super, bool write, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_maybe_write(addr,super,true,size,write) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, super, true, write, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	return x_phys_get_word(addr);
}

static ALWAYS_INLINE uae_u8 mmu_get_user_byte(uaecptr addr, bool super, bool write, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_maybe_write(addr,super,true,size,write) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, super, true, write, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	return x_phys_get_byte(addr);
}

static ALWAYS_INLINE void mmu_put_user_long(uaecptr addr, uae_u32 val, bool super, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,super,true,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, super, true, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	x_phys_put_long(addr, val);
}

static ALWAYS_INLINE void mmu_put_user_word(uaecptr addr, uae_u16 val, bool super, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,super,true,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, super, true, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	x_phys_put_word(addr, val);
}

static ALWAYS_INLINE void mmu_put_user_byte(uaecptr addr, uae_u8 val, bool super, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,super,true,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, super, true, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	x_phys_put_byte(addr, val);
}


static ALWAYS_INLINE void HWput_l(uaecptr addr, uae_u32 l)
{
	put_long (addr, l);
}
static ALWAYS_INLINE void HWput_w(uaecptr addr, uae_u32 w)
{
	put_word (addr, w);
}
static ALWAYS_INLINE void HWput_b(uaecptr addr, uae_u32 b)
{
	put_byte (addr, b);
}
static ALWAYS_INLINE uae_u32 HWget_l(uaecptr addr)
{
	return get_long (addr);
}
static ALWAYS_INLINE uae_u32 HWget_w(uaecptr addr)
{
	return get_word (addr);
}
static ALWAYS_INLINE uae_u32 HWget_b(uaecptr addr)
{
	return get_byte (addr);
}

#if MMU_ICACHE
#define MMU_ICACHE_SZ 4096
struct mmu_icache
{
	uae_u16 data;
	uae_u32 addr;
};

extern struct mmu_icache mmu_icache_data[MMU_ICACHE_SZ];

static ALWAYS_INLINE uae_u16 uae_mmu040_getc_iword(uaecptr addr)
{
	int icidx = (addr & (MMU_ICACHE_SZ - 1)) | regs.s;
	if (addr != mmu_icache_data[icidx].addr || !(regs.cacr & 0x8000)) {
		mmu_icache_data[icidx].data = mmu_get_iword(addr, sz_word);
		mmu_icache_data[icidx].addr = addr;
		return mmu_icache_data[icidx].data;
	} else {
		return mmu_icache_data[icidx].data;
	}
}
#endif

/* ==========================================================================
 * CYBERSTORM C2 rung 2 -- THE DATA-ACCESSOR FAST PATH, RESOLVED IN ONE PLACE.
 *
 * z3660_dfast.h carries the design and the measurement. What is here is the resolver
 * these two functions are: given a guest address that the caller has ALREADY proved
 * naturally aligned, either produce the host address it lands on and the cache state the
 * access installs -- or say nothing, and let the caller run the original accessor
 * verbatim.
 *
 * `super` is passed as `regs.s` UNNORMALISED, exactly as the existing page-cache index
 * arithmetic below uses it (`... | regs.s`), so the tag this computes is bit-for-bit the
 * tag `mmu_get_long()` computes and DPAGE_RHIT/WHIT go on counting the same events. The
 * verdict table is indexed with it normalised, because a table index must not be a bit
 * pattern: `regs.s` is 0 or 1 everywhere in this tree, and the harness pins that.
 *
 * NOTHING IS WRITTEN BEFORE THE ANSWER IS CERTAIN. `mmu_cache_state` is handed back
 * rather than stored, so an arm that declines has left no trace and the original path
 * starts from the state it always did. (The original's first statement is
 * `mmu_cache_state = cache_default_data` anyway, so a stray store would be harmless --
 * but "harmless because something else overwrites it" is not a property worth relying
 * on.)
 * ========================================================================== */
Z3660_DFAST_INLINE int z3660_dfast_read(uaecptr addr, uaecptr *pa, uae_u8 *cs)
{
	uae_u16 v = z3660_dfv_r[regs.s ? 1 : 0][addr >> 24];
	unsigned verdict = Z3660_DFAST_VERDICT(v);
	uaecptr phys;

	if (likely(verdict == Z3660_DFAST_PAGE)) {
		/* No TTR matched and the MMU is on. THE INDEX AND TAG ARITHMETIC BELOW IS COPIED
		 * FROM mmu_get_long() UNCHANGED -- that is what keeps the two caches the same
		 * cache, and what makes the map's "the dpagecache hit rates must not move"
		 * pre-registration checkable rather than aspirational. */
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log != idx1)
			return 0;                 /* a miss: mmu_translate must run, and it must run
			                           * from the original path, with its faults, its ATC
			                           * refill and its own bucket. */
		phys = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
		*cs  = atc_data_cache_read[idx2].cache_state;
	} else if (verdict == Z3660_DFAST_XPAR) {
		/* A data TTR matched: the 68040 uses the logical address as the physical one and
		 * the register's CM field supplies the cache state. Both are in the table. */
		phys = addr;
		*cs  = Z3660_DFAST_CSTATE(v);
	} else {
		return 0;
	}

	/* The bank map is consulted LIVE, exactly as memory_get_long() consults it, so a
	 * device bank declines here and the original path issues the access with its side
	 * effects intact. Caching this decision would buy a load and owe map_banks() a hook;
	 * it is not worth a coherence obligation. */
	if (unlikely(!addrbank_is_direct_r(&get_mem_bank(phys))))
		return 0;

	/* EVERY COUNTER IS BELOW EVERY DECLINE, and that ordering is a bug this corpus
	 * caught rather than a precaution. With the page-cache hit counted where the
	 * original counts it -- at the probe -- a decline at the bank test above charged
	 * DPAGE_RHIT once here and once again when the original path re-probed the same
	 * entry, so the map's "the dpagecache hit rates must not move" pre-registration
	 * failed on exactly the device case it exists to protect. Counting after the last
	 * decline gives one increment per access on both arms: the fast path counts it when
	 * it serves, and the original counts it when the fast path does not. */
	if (verdict == Z3660_DFAST_PAGE) {
#if CACHE_HIT_COUNT
		mmu_data_read_hit++;   /* the PAGE arm only: a TTR match never probes the cache,
		                        * so counting one here would invent a hit the original
		                        * path does not have. */
#endif
	} else {
		Z3660_PROF_CNT(Z3660_PROF_C_DFAST_XPAR);
	}
	*pa = phys;
	Z3660_PROF_CNT(Z3660_PROF_C_DFAST_HIT);
	Z3660_DFAST_SERVED();
	return 1;
}

Z3660_DFAST_INLINE int z3660_dfast_write(uaecptr addr, uaecptr *pa, uae_u8 *cs)
{
	uae_u16 v = z3660_dfv_w[regs.s ? 1 : 0][addr >> 24];
	unsigned verdict = Z3660_DFAST_VERDICT(v);
	uaecptr phys;

	if (likely(verdict == Z3660_DFAST_PAGE)) {
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log != idx1)
			return 0;
		phys = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
		/* NOT a transcription error: the original reads the cache state out of the READ
		 * array here too (cpummu.h, mmu_put_long). Copied rather than corrected --
		 * "state equivalence" means equivalence to what the machine does, and a
		 * divergence introduced under the banner of a speed change is the worst kind. */
		*cs  = atc_data_cache_read[idx2].cache_state;
	} else if (verdict == Z3660_DFAST_XPAR) {
		phys = addr;
		*cs  = Z3660_DFAST_CSTATE(v);
	} else {
		return 0;
	}

	if (unlikely(!addrbank_is_direct_w(&get_mem_bank(phys))))
		return 0;

	/* Below every decline -- see z3660_dfast_read() for the double-count this ordering
	 * closes. */
	if (verdict == Z3660_DFAST_PAGE) {
#if CACHE_HIT_COUNT
		mmu_data_write_hit++;
#endif
	} else {
		Z3660_PROF_CNT(Z3660_PROF_C_DFAST_XPAR);
	}
	*pa = phys;
	Z3660_PROF_CNT(Z3660_PROF_C_DFAST_HIT);
	Z3660_DFAST_SERVED();
	return 1;
}

/* ==========================================================================
 * CYBERSTORM C1 -- where the interpreter's stage buckets are actually charged.
 *
 * This is the choke layer, and picking it was the whole design decision. The named
 * accessors above it (get_long_mmu040 and friends) are STATIC_INLINE = always_inline and
 * vanish into ~3000 call sites in cpuemu_31; the mmu_get_ / mmu_put_ family below it is
 * the same. The uae_mmu040_ functions in between are ALWAYS_INLINE, which here is bare
 * `__inline` (mmu_common.h:109) -- a hint, not a command -- and gcc emits ONE out-of-line
 * copy per translation unit. Verified against the shipped objects: cpuemu_31.o carries
 * exactly one uae_mmu040_get_iword with 920 call relocations into it.
 *
 * So a probe here is ~10 code sites in the whole firmware rather than 3000. (This used to
 * be justified by the "~12 MB SD/TFTP write threshold", which is retired -- but the choice
 * was never really about the size gate: 3000 inlined probe sites would also have wrecked
 * the icache behaviour of the very loop being measured, which is a worse failure than a
 * large image because it changes the answer rather than the deploy.)
 *
 * The Z3660_PROF_RET idiom exists so the NON-profiling expansion is textually the
 * original `return expr;` -- default and lean must be byte-identical to the build before
 * this file was touched, and the cheapest way to be certain is to hand gcc the same text.
 * ========================================================================== */
static ALWAYS_INLINE uae_u16 uae_mmu040_get_iword(uaecptr addr)
{
	/* CYBERSTORM C2 rung 1 -- the decoded-op cache's extension-word window.
	 *
	 * This is the ONLY place a handler's extension-word fetch can be intercepted. The
	 * generated handlers call get_iword_mmu040(N) directly (cpuemu_31 does so about 920
	 * times) and nothing in the run loop can hand them a value, so a cache that means to
	 * attack FETCHEX -- 11.31 % of the interpreter, against FETCHOP's 17.34 % -- has to
	 * answer here or not at all.
	 *
	 * It is safe because it is keyed on the ADDRESS and not on the pc: the window holds
	 * the word at one specific guest address, armed only by a validated cache hit whose
	 * generation stamp is current, and a fetch of any other address falls straight
	 * through. A handler that has already moved the pc therefore computes a different
	 * address and gets a real fetch; so does the next instruction's opcode, and so does
	 * every fetch after any invalidation, because invalidating disarms the window. The
	 * disarmed sentinel is odd and every instruction-word address is even.
	 *
	 * It sits ABOVE the bracket on purpose: a served word costs no fetch, so it must not
	 * be charged to FETCHEX, or the profile would report the cache working as the fetch
	 * still being expensive. DOPC_EXT counts what was served instead. */
	if (addr == (uaecptr)z3660_dopc_ext_pc) {
		Z3660_PROF_CNT(Z3660_PROF_C_DOPC_EXT);
		return (uae_u16)z3660_dopc_ext_val;
	}
	Z3660_PROF_ENTER_IFETCH();
	Z3660_PROF_CNT(Z3660_PROF_C_FETCH);
#if MMU_ICACHE
	Z3660_PROF_RET(uae_mmu040_getc_iword(addr));
#else
	Z3660_PROF_RET(mmu_get_iword(addr, sz_word));
#endif
}
static ALWAYS_INLINE uae_u32 uae_mmu040_get_ilong(uaecptr addr)
{
	Z3660_PROF_ENTER_IFETCH();
	Z3660_PROF_CNT(Z3660_PROF_C_FETCH);
#if MMU_ICACHE
	uae_u32 result = uae_mmu040_getc_iword(addr);
	result <<= 16;
	result |= uae_mmu040_getc_iword(addr + 2);
	Z3660_PROF_RET(result);
#else
	if (unlikely(is_unaligned_bus(addr, 4))) {  /* ISSUE-52: see uae_mmu040_get_long */
		Z3660_PROF_CNT(Z3660_PROF_C_MISALIGN_R);
		/* C2 rung 2: MISALIGN_R has always been incremented here as well as at the two
		 * DATA sites, so the map's "20.71 % of reads are misaligned" divided
		 * (data + instruction) misalignments by data reads alone. This site is the
		 * instruction-stream half, and it misaligns BY CONSTRUCTION -- guest
		 * instructions are word-aligned, so a 32-bit fetch at pc+2 sits at an address
		 * = 2 (mod 4) half the time. MISALIGN_R is left exactly as it was, so every
		 * existing capture and identity still reads; MISALIGN_I is the subtrahend that
		 * makes MISALIGN_R - MISALIGN_I the genuine misaligned DATA reads. */
		Z3660_PROF_CNT(Z3660_PROF_C_MISALIGN_I);
		Z3660_PROF_RET(mmu_get_ilong_unaligned(addr));
	}
	Z3660_PROF_RET(mmu_get_ilong(addr, sz_long));
#endif
}
static ALWAYS_INLINE uae_u16 uae_mmu040_get_ibyte(uaecptr addr)
{
#if MMU_ICACHE
	uae_u16 result = uae_mmu040_getc_iword(addr & ~1);
	return (addr & 1) ? result & 0xFF : result >> 8;
#else
	return mmu_get_byte(addr, false, sz_byte);
#endif
}
static ALWAYS_INLINE uae_u32 uae_mmu040_get_long(uaecptr addr)
{
	/* ISSUE-52: split on BUS misalignment, not just page-crossing. A misaligned longword
	 * within one page (the format-0 frame PC at a7+2 = 0x...FBA) used to take the single
	 * mmu_get_long -> x_phys_get_long path, handing a misaligned PHYSICAL address to the
	 * board's memory layer, which cannot do a misaligned transfer and drops bytes on metal
	 * (0x80800000 -> 0x80000000, bit 23). The unaligned splitter issues only aligned word/
	 * byte sub-accesses, which every backing does correctly. See is_unaligned_bus. */
	Z3660_PROF_ENTER(Z3660_PROF_B_READ);
	Z3660_PROF_CNT(Z3660_PROF_C_READ);
	if (unlikely(is_unaligned_bus(addr, 4))) {
		Z3660_PROF_CNT(Z3660_PROF_C_MISALIGN_R);
		Z3660_PROF_RET(mmu_get_long_unaligned(addr, true));
	}
	/* C2 rung 2. Below the misalignment test on purpose: everything past it is naturally
	 * aligned, which is what makes "never fast-path across a page boundary" structural. */
	{
		uaecptr pa; uae_u8 cs;
		if (likely(z3660_dfast_read(addr, &pa, &cs))) {
			mmu_cache_state = cs;
			Z3660_PROF_RET(direct_get_long(pa));
		}
	}
	Z3660_PROF_RET(mmu_get_long(addr, true, sz_long));
}
static ALWAYS_INLINE uae_u16 uae_mmu040_get_word(uaecptr addr)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_READ);
	Z3660_PROF_CNT(Z3660_PROF_C_READ);
	if (unlikely(is_unaligned_bus(addr, 2))) {  /* ISSUE-52: see uae_mmu040_get_long */
		Z3660_PROF_CNT(Z3660_PROF_C_MISALIGN_R);
		Z3660_PROF_RET(mmu_get_word_unaligned(addr, true));
	}
	{
		uaecptr pa; uae_u8 cs;
		if (likely(z3660_dfast_read(addr, &pa, &cs))) {
			mmu_cache_state = cs;
			Z3660_PROF_RET((uae_u16)direct_get_word(pa));
		}
	}
	Z3660_PROF_RET(mmu_get_word(addr, true, sz_word));
}
static ALWAYS_INLINE uae_u8 uae_mmu040_get_byte(uaecptr addr)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_READ);
	Z3660_PROF_CNT(Z3660_PROF_C_READ);
	/* A byte is aligned by definition, so there is no misalignment test above this one
	 * and nothing else has to be true for the page-boundary argument to hold. */
	{
		uaecptr pa; uae_u8 cs;
		if (likely(z3660_dfast_read(addr, &pa, &cs))) {
			mmu_cache_state = cs;
			Z3660_PROF_RET((uae_u8)direct_get_byte(pa));
		}
	}
	Z3660_PROF_RET(mmu_get_byte(addr, true, sz_byte));
}

static ALWAYS_INLINE void uae_mmu040_put_word(uaecptr addr, uae_u16 val)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_WRITE);
	Z3660_PROF_CNT(Z3660_PROF_C_WRITE);
	if (unlikely(is_unaligned_bus(addr, 2))) {  /* ISSUE-52: see uae_mmu040_get_long */
		Z3660_PROF_CNT(Z3660_PROF_C_MISALIGN_W);
		Z3660_PROF_RETV(mmu_put_word_unaligned(addr, val, true));
	}
	{
		uaecptr pa; uae_u8 cs;
		if (likely(z3660_dfast_write(addr, &pa, &cs))) {
			mmu_cache_state = cs;
			Z3660_PROF_RETV(direct_put_word(pa, val));
		}
	}
	Z3660_PROF_RETV(mmu_put_word(addr, val, true, sz_word));
}
static ALWAYS_INLINE void uae_mmu040_put_byte(uaecptr addr, uae_u8 val)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_WRITE);
	Z3660_PROF_CNT(Z3660_PROF_C_WRITE);
	{
		uaecptr pa; uae_u8 cs;
		if (likely(z3660_dfast_write(addr, &pa, &cs))) {
			mmu_cache_state = cs;
			Z3660_PROF_RETV(direct_put_byte(pa, val));
		}
	}
	Z3660_PROF_RETV(mmu_put_byte(addr, val, true, sz_byte));
}
static ALWAYS_INLINE void uae_mmu040_put_long(uaecptr addr, uae_u32 val)
{
	Z3660_PROF_ENTER(Z3660_PROF_B_WRITE);
	Z3660_PROF_CNT(Z3660_PROF_C_WRITE);
	if (unlikely(is_unaligned_bus(addr, 4))) {  /* ISSUE-52: see uae_mmu040_get_long */
		Z3660_PROF_CNT(Z3660_PROF_C_MISALIGN_W);
		Z3660_PROF_RETV(mmu_put_long_unaligned(addr, val, true));
	}
	{
		uaecptr pa; uae_u8 cs;
		if (likely(z3660_dfast_write(addr, &pa, &cs))) {
			mmu_cache_state = cs;
			Z3660_PROF_RETV(direct_put_long(pa, val));
		}
	}
	Z3660_PROF_RETV(mmu_put_long(addr, val, true, sz_long));
}


static ALWAYS_INLINE uae_u32 uae_mmu060_get_ilong(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		return mmu_get_ilong_unaligned(addr);
	return mmu_get_ilong(addr, sz_long);
}
static ALWAYS_INLINE uae_u16 uae_mmu060_get_iword(uaecptr addr)
{
	return mmu_get_iword(addr, sz_word);
}
static ALWAYS_INLINE uae_u16 uae_mmu060_get_ibyte(uaecptr addr)
{
	return mmu_get_byte(addr, false, sz_byte);
}
static ALWAYS_INLINE uae_u32 uae_mmu060_get_long(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		return mmu_get_long_unaligned(addr, true);
	return mmu_get_long(addr, true, sz_long);
}
static ALWAYS_INLINE uae_u16 uae_mmu060_get_word(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 2)))
		return mmu_get_word_unaligned(addr, true);
	return mmu_get_word(addr, true, sz_word);
}
static ALWAYS_INLINE uae_u8 uae_mmu060_get_byte(uaecptr addr)
{
	return mmu_get_byte(addr, true, sz_byte);
}
static ALWAYS_INLINE void uae_mmu_get_move16(uaecptr addr, uae_u32 *val)
{
	// move16 is always aligned
	mmu_get_move16(addr, val, true, 16);
}

static ALWAYS_INLINE void uae_mmu060_put_long(uaecptr addr, uae_u32 val)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		mmu_put_long_unaligned(addr, val, true);
	else
		mmu_put_long(addr, val, true, sz_long);
}
static ALWAYS_INLINE void uae_mmu060_put_word(uaecptr addr, uae_u16 val)
{
	if (unlikely(is_unaligned_page(addr, 2)))
		mmu_put_word_unaligned(addr, val, true);
	else
		mmu_put_word(addr, val, true, sz_word);
}
static ALWAYS_INLINE void uae_mmu060_put_byte(uaecptr addr, uae_u8 val)
{
	mmu_put_byte(addr, val, true, sz_byte);
}
static ALWAYS_INLINE void uae_mmu_put_move16(uaecptr addr, uae_u32 *val)
{
	// move16 is always aligned
	mmu_put_move16(addr, val, true, 16);
}

// normal 040
STATIC_INLINE void put_byte_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu040_put_byte (addr, v);
}
STATIC_INLINE void put_word_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu040_put_word (addr, v);
}
STATIC_INLINE void put_long_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu040_put_long (addr, v);
}
STATIC_INLINE uae_u32 get_byte_mmu040 (uaecptr addr)
{
	return uae_mmu040_get_byte (addr);
}
STATIC_INLINE uae_u32 get_word_mmu040 (uaecptr addr)
{
	return uae_mmu040_get_word (addr);
}
STATIC_INLINE uae_u32 get_long_mmu040 (uaecptr addr)
{
	return uae_mmu040_get_long (addr);
}
// normal 060
STATIC_INLINE void put_byte_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu060_put_byte (addr, v);
}
STATIC_INLINE void put_word_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu060_put_word (addr, v);
}
STATIC_INLINE void put_long_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu060_put_long (addr, v);
}
STATIC_INLINE uae_u32 get_byte_mmu060 (uaecptr addr)
{
	return uae_mmu060_get_byte (addr);
}
STATIC_INLINE uae_u32 get_word_mmu060 (uaecptr addr)
{
	return uae_mmu060_get_word (addr);
}
STATIC_INLINE uae_u32 get_long_mmu060 (uaecptr addr)
{
	return uae_mmu060_get_long (addr);
}

STATIC_INLINE void get_move16_mmu (uaecptr addr, uae_u32 *v)
{
	uae_mmu_get_move16 (addr, v);
}
STATIC_INLINE void put_move16_mmu (uaecptr addr, uae_u32 *v)
{
	uae_mmu_put_move16 (addr, v);
}

// locked rmw 060
STATIC_INLINE void put_lrmw_byte_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_byte, 1);
}
STATIC_INLINE void put_lrmw_word_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_word, 1);
}
STATIC_INLINE void put_lrmw_long_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_long, 1);
}
STATIC_INLINE uae_u32 get_lrmw_byte_mmu060 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_byte, 1);
}
STATIC_INLINE uae_u32 get_lrmw_word_mmu060 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_word, 1);
}
STATIC_INLINE uae_u32 get_lrmw_long_mmu060 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_long, 1);
}
// normal rmw 060
STATIC_INLINE void put_rmw_byte_mmu060 (uaecptr addr, uae_u32 v)
{
	rmw_cycle = true;
	uae_mmu060_put_byte (addr, v);
	rmw_cycle = false;
}
STATIC_INLINE void put_rmw_word_mmu060 (uaecptr addr, uae_u32 v)
{
	rmw_cycle = true;
	uae_mmu060_put_word (addr, v);
	rmw_cycle = false;
}
STATIC_INLINE void put_rmw_long_mmu060 (uaecptr addr, uae_u32 v)
{
	rmw_cycle = true;
	uae_mmu060_put_long (addr, v);
	rmw_cycle = false;
}
STATIC_INLINE uae_u32 get_rmw_byte_mmu060 (uaecptr addr)
{
	rmw_cycle = true;
	uae_u32 v = uae_mmu060_get_byte (addr);
	rmw_cycle = false;
	return v;
}
STATIC_INLINE uae_u32 get_rmw_word_mmu060 (uaecptr addr)
{
	rmw_cycle = true;
	uae_u32 v = uae_mmu060_get_word (addr);
	rmw_cycle = false;
	return v;
}
STATIC_INLINE uae_u32 get_rmw_long_mmu060 (uaecptr addr)
{
	rmw_cycle = true;
	uae_u32 v = uae_mmu060_get_long (addr);
	rmw_cycle = false;
	return v;
}
// locked rmw 040
STATIC_INLINE void put_lrmw_byte_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_byte, 0);
}
STATIC_INLINE void put_lrmw_word_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_word, 0);
}
STATIC_INLINE void put_lrmw_long_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_long, 0);
}
STATIC_INLINE uae_u32 get_lrmw_byte_mmu040 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_byte, 0);
}
STATIC_INLINE uae_u32 get_lrmw_word_mmu040 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_word, 0);
}
STATIC_INLINE uae_u32 get_lrmw_long_mmu040 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_long, 0);
}

STATIC_INLINE uae_u32 get_ibyte_mmu040 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu040_get_iword (pc);
}
STATIC_INLINE uae_u32 get_iword_mmu040 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu040_get_iword (pc);
}
STATIC_INLINE uae_u32 get_ilong_mmu040 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu040_get_ilong (pc);
}
STATIC_INLINE uae_u32 next_iword_mmu040 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (2);
	return uae_mmu040_get_iword (pc);
}
STATIC_INLINE uae_u32 next_ilong_mmu040 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (4);
	return uae_mmu040_get_ilong (pc);
}

STATIC_INLINE uae_u32 get_ibyte_mmu060 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu060_get_iword (pc);
}
STATIC_INLINE uae_u32 get_iword_mmu060 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu060_get_iword (pc);
}
STATIC_INLINE uae_u32 get_ilong_mmu060 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu060_get_ilong (pc);
}
STATIC_INLINE uae_u32 next_iword_mmu060 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (2);
	return uae_mmu060_get_iword (pc);
}
STATIC_INLINE uae_u32 next_ilong_mmu060 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (4);
	return uae_mmu060_get_ilong (pc);
}

extern void flush_mmu040 (uaecptr, int);
extern void m68k_do_rts_mmu040 (void);
extern void m68k_do_rte_mmu040 (uaecptr a7);
extern void m68k_do_bsr_mmu040 (uaecptr oldpc, uae_s32 offset);

extern void flush_mmu060 (uaecptr, int);
extern void m68k_do_rts_mmu060 (void);
extern void m68k_do_rte_mmu060 (uaecptr a7);
extern void m68k_do_bsr_mmu060 (uaecptr oldpc, uae_s32 offset);

#endif /* UAE_CPUMMU_H */
