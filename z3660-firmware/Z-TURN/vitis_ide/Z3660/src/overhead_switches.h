// SPDX-License-Identifier: MIT
/* CYBERSTORM C3 -- run-loop overhead switches: the single copy of the resolution rule.
 *
 * WHAT THESE ARE
 *
 * Three YES/NO config keys that switch off measurement and tracing the firmware
 * otherwise does on every emulated instruction (core1) or every cooperative-loop
 * iteration (core0):
 *
 *   perf_stats    core1: the per-instruction instruction-rate accumulator + [PERF] tick
 *   emu_probes    core1: the ISSUE-52-era run-loop probes ([ROMENT] [RTE] [URTE]
 *                 [USTEP] [IDCMP] on the 040 loop; wild-PC detector + retired-
 *                 instruction ring on the 030 loop)
 *   core0_stats   core0: C0STAT per-task span stamping (NOT just its printing --
 *                 `PSTAT` has always gated only the printing)
 *
 * They are DIAGNOSTIC-COST switches, never functional ones. Off, the guest sees the
 * same emulated machine; it just gets more of the ARM's time. That is precisely what
 * makes them safe to sweep on metal under a live AMIX.
 *
 * WHY THE RULE LIVES IN A HEADER
 *
 * Same reason as amix_ram.h: the contract that matters is "a config file that does not
 * mention these keys behaves exactly as this firmware did before they existed", and a
 * contract worth stating is worth testing. config_file.c needs the Xilinx BSP and cannot
 * be compiled on the host, so the rule is kept here, self-contained, and the host guard
 * (Z3660_emu/test/host/bootmode_test.cpp) compiles this header directly and asserts the
 * contract. If the default ever drifts, that test fails rather than a metal run
 * silently losing its instruments.
 *
 * SENTINEL: -1 means "no key in the file". Both config paths (no-preset and preset)
 * initialise to -1 and resolve once, after the whole file is parsed.
 */
#ifndef SRC_OVERHEAD_SWITCHES_H_
#define SRC_OVERHEAD_SWITCHES_H_

/* Unset resolves to ON, i.e. the behaviour the firmware has always had.
 * This is load-bearing: absent keys must not change a single instruction. */
#define Z3660_OVH_UNSET     (-1)
#define Z3660_OVH_DEFAULT   1

/* requested: -1 (unset) or the parser's YES/NO (1/0). Anything else is coerced to a
 * clean 0/1 rather than propagated -- these values are written straight into the
 * cross-core mailbox, which core1 tests for truth. */
static inline int z3660_ovh_resolve(int requested)
{
   if(requested == Z3660_OVH_UNSET) return Z3660_OVH_DEFAULT;
   return requested ? 1 : 0;
}

/* "This resolved value is the historical behaviour" -- used by the config printer to
 * stay silent unless something is actually switched off. */
static inline int z3660_ovh_is_default(int resolved)
{
   return resolved == Z3660_OVH_DEFAULT;
}

#endif /* SRC_OVERHEAD_SWITCHES_H_ */
