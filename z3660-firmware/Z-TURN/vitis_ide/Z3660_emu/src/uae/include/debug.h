#ifndef UAE_DEBUG_H
#define UAE_DEBUG_H

/* Minimal debug.h for the Z3660 build. The full WinUAE debugger is absent here
 * (custom.cpp is gutted, no console debugger). The imported MMU engine
 * (cpummu030.cpp / cpummu.cpp) only needs console_out_f / activate_debugger /
 * mmu_dump_tables — provided as inert inlines so no debugger TU is required.
 * write_log is the real logger (sysconfig.h maps it to z3660_printf). */

#include "sysconfig.h"
#include "sysdeps.h"
#include <cstdarg>

static inline void console_out_f(const TCHAR *fmt, ...) { (void)fmt; }
static inline void console_out(const TCHAR *s) { (void)s; }
static inline void activate_debugger(void) { }

/* Branch-trace debugger hooks referenced by the generated cpuemu_32 (68030 MMU).
 * debugmem_trace is always false here, so the push is never taken; the pops just
 * return the pc unchanged. (uaecptr is uae_u32.) */
extern bool debugmem_trace;
static inline void branch_stack_push(uae_u32 oldpc, uae_u32 nextpc) { (void)oldpc; (void)nextpc; }
static inline uae_u32 branch_stack_pop_rte(uae_u32 oldpc) { return oldpc; }
static inline uae_u32 branch_stack_pop_rts(uae_u32 oldpc) { return oldpc; }

#endif /* UAE_DEBUG_H */
