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

/* Times the run loop's interrupt poll (check_uae_int_request -> ipl_main_read) has run.
 * The yield-granularity measurement for the C2 block-idiom fast path. */
extern unsigned long harness_ipl_polls;

#ifdef __cplusplus
}
#endif
#endif
