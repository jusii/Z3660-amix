/* Public API of the Layer-1 host MMU harness substrate (board_stubs.cpp). */
#ifndef Z3660_HOST_HARNESS_H
#define Z3660_HOST_HARNESS_H

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flat-RAM machine. Allocates a HRAM_SIZE buffer and points every memory bank at
 * it via the lget/wget path (no baseaddr_direct => no guest-addr-as-host-pointer
 * deref; decision #6). All accesses are big-endian (68k order) regardless of host
 * endianness. Call once before using the CPU core. */
#define HRAM_SIZE  0x10000000u   /* 256 MB: covers low RAM + local RAM @0x08000000 */
#define HRAM_MASK  (HRAM_SIZE - 1u)

void harness_mem_init(void);
void harness_mem_reset(void);              /* zero the buffer */

/* Restore the cross-core SHARED knobs to the firmware's power-on state
 * (service_cadence 1, perf_stats ON, emu_probes ON). harness_mem_init() calls it;
 * a test that flips a knob calls it again to leave the fixture clean. */
void harness_shared_defaults(void);

/* Direct big-endian buffer poke/peek for test setup/inspection (bypasses banks). */
void     hram_poke32(uae_u32 addr, uae_u32 v);
void     hram_poke16(uae_u32 addr, uae_u16 v);
void     hram_poke8 (uae_u32 addr, uae_u8  v);
uae_u32  hram_peek32(uae_u32 addr);
uae_u16  hram_peek16(uae_u32 addr);
uae_u8   hram_peek8 (uae_u32 addr);
uae_u8  *hram_base(void);                  /* raw buffer pointer */

/* =====================================================================
 *  THE IDENTITY WINDOW -- a bank where the BOARD's memory model is literally true.
 *
 * The flat-RAM machine above deliberately does NOT set `baseaddr_direct_*`, because on
 * the board a direct bank means "the guest's physical address IS the host address" and a
 * 64-bit host cannot honour that for a malloc'd buffer (decision #6). That was fine
 * while nothing tested the direct arm.
 *
 * C2 rung 2's accessor fast path serves its whole hit case out of exactly that arm, so
 * leaving every bank non-direct would have meant the fast path DECLINING in every test
 * -- a corpus that proves the fallback is correct and says nothing about the thing being
 * added, which is the failure mode a state-equivalence test is blindest to.
 *
 * So one window gets the board's model for real: HDIRECT_SIZE bytes mmap'd at the fixed
 * host address HDIRECT_BASE, and a bank over guest HDIRECT_BASE whose baseaddr_direct_r
 * and _w are set. Inside it `(uae_u8 *)guest_addr` is a valid host pointer and the
 * fork's identity assumption holds exactly as it does on the ARM.
 *
 * MAP_FIXED_NOREPLACE, never MAP_FIXED: if something already owns that address the
 * mapping FAILS and harness_direct_ok() returns 0, so the tests that need it fail loudly
 * instead of silently dropping their coverage.
 * ===================================================================== */
#define HDIRECT_BASE  0x40000000u   /* guest AND host: the identity is the point */
#define HDIRECT_SIZE  0x00100000u   /* 1 MB -- 256 4K pages, far more than any test uses */

int  harness_direct_ok(void);       /* 1 if the window was mapped; 0 = tests must fail   */
void harness_direct_reset(void);    /* zero the window                                   */
/* Map the window's guest bank slots to the DIRECT bank, or to a non-direct one over the
 * same storage. The second is how a test proves the fast path declines for a device
 * rather than fast-pathing a side effect away. */
void harness_direct_map(int direct);
/* Big-endian poke/peek straight into the window, bypassing the banks. The analogue of
 * hram_poke32 / hram_peek32 above. */
void     hdirect_poke32(uae_u32 addr, uae_u32 v);
uae_u32  hdirect_peek32(uae_u32 addr);
/* How many times the non-direct bank's accessors were called -- the evidence that a
 * declined access really did reach the bank's function pointers. */
extern unsigned long hdirect_io_hits;

/* Times the run loop's interrupt poll (check_uae_int_request -> ipl_main_read) has run.
 * The yield-granularity measurement for the C2 block-idiom fast path. */
extern unsigned long harness_ipl_polls;

#ifdef __cplusplus
}
#endif
#endif
