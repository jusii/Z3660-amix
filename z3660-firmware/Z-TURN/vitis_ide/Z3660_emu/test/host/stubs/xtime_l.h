/* Host-harness stub for the Xilinx ARM global-timer header (xtime_l.h).
 *
 * The real header lives in the Zynq BSP and backs the [PERF] instruction-rate
 * readout in newcpu.cpp's m68k_run_mmu030.  On the x86-64 MMU harness there is
 * no global timer and no perf reporting, so XTime_GetTime() returns 0 -- the
 * readout's "elapsed >= COUNTS_PER_SECOND" gate is then never true and nothing
 * is printed.  (-Istubs shadows this ahead of the absent BSP header.) */
#ifndef XTIME_L_H_HOSTSTUB
#define XTIME_L_H_HOSTSTUB
typedef unsigned long long XTime;
#define COUNTS_PER_SECOND 1000000ULL
static inline void XTime_GetTime(XTime *t) { if (t) *t = 0; }
#endif
