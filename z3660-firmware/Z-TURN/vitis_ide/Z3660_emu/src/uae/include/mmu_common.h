#ifndef UAE_MMU_COMMON_H
#define UAE_MMU_COMMON_H

#include "uae/types.h"
#include "uae/likely.h"

#define MMUDEBUG 0
#define MMUINSDEBUG 0
#define MMUDEBUGMISC 0

#ifdef __cplusplus
struct m68k_exception {
	int prb;
	m68k_exception (int exc) : prb (exc) {}
	operator int() { return prb; }
};
#define SAVE_EXCEPTION
#define RESTORE_EXCEPTION
#define TRY(var) try
#define CATCH(var) catch(m68k_exception var)
#define THROW(n) throw m68k_exception(n)
#define THROW_AGAIN(var) throw
#define ENDTRY
#define STOPTRY
#else
/* we are in plain C, just use a stack of long jumps */
#include <setjmp.h>
extern jmp_buf __exbuf;
extern int     __exvalue;
#define TRY(DUMMY)       __exvalue=setjmp(__exbuf);       \
                  if (__exvalue==0) { __pushtry(&__exbuf);
#define CATCH(x)  __poptry(); } else {m68k_exception x=__exvalue; 
#define ENDTRY    __poptry();}
#define STOPTRY   __poptry()
#define THROW(x) if (__is_catched()) {longjmp(__exbuf,x);}
#define THROW_AGAIN(var) if (__is_catched()) longjmp(*__poptry(),__exvalue)
#define SAVE_EXCEPTION
#define RESTORE_EXCEPTION
jmp_buf* __poptry(void);
void __pushtry(jmp_buf *j);
int __is_catched(void);

typedef  int m68k_exception;

#endif

/* special status word (access error stack frame) */
/* 68060 */
#define MMU_FSLW_MA		0x08000000
#define MMU_FSLW_LK		0x02000000
#define MMU_FSLW_R		0x01000000
#define MMU_FSLW_W		0x00800000
#define MMU_FSLW_SIZE_L	0x00000000 /* Note: wrong in mc68060 manual! */
#define MMU_FSLW_SIZE_B	0x00200000
#define MMU_FSLW_SIZE_W	0x00400000
#define MMU_FSLW_SIZE_D	0x00600000
#define MMU_FSLW_TT		0x00180000
#define MMU_FSLW_TT_N	0x00000000 /* Normal access */
#define MMU_FSLW_TT_16  0x00080000 /* MOVE16 */
#define MMU_FSLW_TM		0x00070000 /* = function code */
#define MMU_FSLW_IO		0x00008000
#define MMU_FSLW_PBE	0x00004000
#define MMU_FSLW_SBE	0x00002000
#define MMU_FSLW_PTA	0x00001000
#define MMU_FSLW_PTB	0x00000800
#define MMU_FSLW_IL		0x00000400
#define MMU_FSLW_PF		0x00000200
#define MMU_FSLW_SP		0x00000100
#define MMU_FSLW_WP		0x00000080
#define MMU_FSLW_TWE	0x00000040
#define MMU_FSLW_RE		0x00000020
#define MMU_FSLW_WE		0x00000010
#define MMU_FSLW_TTR	0x00000008
#define MMU_FSLW_BPE	0x00000004
#define MMU_FSLW_SEE	0x00000001
/* 68040 */
#define MMU_SSW_TM		0x0007
#define MMU_SSW_TT		0x0018
#define MMU_SSW_TT1		0x0010
#define MMU_SSW_TT0		0x0008
#define MMU_SSW_SIZE	0x0060
#define MMU_SSW_SIZE_B	0x0020
#define MMU_SSW_SIZE_W	0x0040
#define MMU_SSW_SIZE_L	0x0000
#define MMU_SSW_SIZE_CL	0x0060
#define MMU_SSW_RW		0x0100
#define MMU_SSW_LK		0x0200
#define MMU_SSW_ATC		0x0400
#define MMU_SSW_MA		0x0800
#define MMU_SSW_CM	0x1000
#define MMU_SSW_CT	0x2000
#define MMU_SSW_CU	0x4000
#define MMU_SSW_CP	0x8000
/* 68030 */
#define MMU030_SSW_FC       0x8000
#define MMU030_SSW_FB       0x4000
#define MMU030_SSW_RC       0x2000
#define MMU030_SSW_RB       0x1000
#define MMU030_SSW_DF       0x0100
#define MMU030_SSW_RM       0x0080
#define MMU030_SSW_RW       0x0040
#define MMU030_SSW_SIZE_MASK    0x0030
#define MMU030_SSW_SIZE_B       0x0010
#define MMU030_SSW_SIZE_W       0x0020
#define MMU030_SSW_SIZE_L       0x0000
#define MMU030_SSW_FC_MASK      0x0007


#define ALWAYS_INLINE __inline

// take care of 2 kinds of alignment, bus size and page
static ALWAYS_INLINE bool is_unaligned_page(uaecptr addr, int size)
{
    return unlikely((addr & (size - 1)) && (addr ^ (addr + size - 1)) & regs.mmu_page_size);
}
static ALWAYS_INLINE bool is_unaligned_bus(uaecptr addr, int size)
{
    return (addr & (size - 1));
}

// AMIX: hide the boot RAM at $08000000 from the kernel's MMU-time accesses (phys_* is
// ONLY the MMU-translated physical path; the boot runs MMU-off via get_*). Once the AMIX
// MMU is up the kernel has relocated to $07000000 and must NOT see RAM at $08000000, or it
// over-uses the address space and panics vatosde. Reads return ~0, writes are dropped, so
// the kernel's RAM probe sees "no RAM" there. (Page tables live at $07xxxxxx, untouched.)
extern "C" volatile int amix_mmu_on;
// EXPERIMENT (2026-06-20): the CURRENT AMIX kernel brings up its MMU while STILL at $08000000
// (serial: "MMU enabled ... PC=08000fe6", NOT the 07000fe6 this hide assumed) -- i.e. it runs in
// PLACE at $08000000 (its MAINSTORE) and does NOT relocate to $07000000. With only $08000000 in
// the memlist (the DDR drct CPU-RAM), AMIX sees a single 16MB window there, so hiding $08000000
// just yanks the running kernel's own code -> halt/reboot-loop. Disable the hide so the kernel
// runs on DDR @ $08000000. amix_mmu_on stays live (a3000_scsi.cpp completion timing needs it).
#define AMIX_HIDE08(a) (0)
#define AMIX_HIDE08_OLD(a) (amix_mmu_on && ((a) & 0xFF000000u) == 0x08000000u)
static ALWAYS_INLINE void phys_put_long(uaecptr addr, uae_u32 l)
{
    if (AMIX_HIDE08(addr)) return;
    put_long(addr, l);
}
static ALWAYS_INLINE void phys_put_word(uaecptr addr, uae_u32 w)
{
    if (AMIX_HIDE08(addr)) return;
    put_word(addr, w);
}
static ALWAYS_INLINE void phys_put_byte(uaecptr addr, uae_u32 b)
{
    if (AMIX_HIDE08(addr)) return;
    put_byte(addr, b);
}
static ALWAYS_INLINE uae_u32 phys_get_long(uaecptr addr)
{
    if (AMIX_HIDE08(addr)) return 0xFFFFFFFFu;
    return get_long(addr);
}
static ALWAYS_INLINE uae_u32 phys_get_word(uaecptr addr)
{
    if (AMIX_HIDE08(addr)) return 0xFFFFu;
    return get_word(addr);
}
static ALWAYS_INLINE uae_u32 phys_get_byte(uaecptr addr)
{
    if (AMIX_HIDE08(addr)) return 0xFFu;
    return get_byte(addr);
}

extern uae_u32(*x_phys_get_iword)(uaecptr);
extern uae_u32(*x_phys_get_ilong)(uaecptr);
extern uae_u32(*x_phys_get_byte)(uaecptr);
extern uae_u32(*x_phys_get_word)(uaecptr);
extern uae_u32(*x_phys_get_long)(uaecptr);
extern void(*x_phys_put_byte)(uaecptr, uae_u32);
extern void(*x_phys_put_word)(uaecptr, uae_u32);
extern void(*x_phys_put_long)(uaecptr, uae_u32);

#endif /* UAE_MMU_COMMON_H */
