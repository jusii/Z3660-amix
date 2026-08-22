/* Host guard for core0's boot-mode classification (../../../Z3660/src/config_file.h).
 *
 * WHY THIS EXISTS. The first AMIX-040 metal boot (2026-08-20, firmware 2CFA9216) died
 * with core1 never producing a single line of output. The cause was not in the emulator
 * at all: core0 classified UAE_040_MMU as a REAL-CPU mode, because its "is this an
 * emulator mode?" predicate was an enumeration of the emulator modes, copy-pasted into
 * eight places, and appending UAE_040_MMU to BOOTMODE left all eight answering "no".
 * main.c's copy gates the only site that starts core1 (entry address -> 0xFFFFFFF0, sev,
 * shared_data handshake), so core0 skipped core1 entirely, fell through to its real-CPU
 * path, and relinquished the bus to a 68060 that is not fitted -- the Amiga then
 * watchdog-reset every ~3 s forever. Log signature: "060 starting now..." (1 occurrence
 * in the entire serial history) instead of "Starting CPU emulator on Core1" (648).
 *
 * WHAT THIS CAN AND CANNOT CATCH. core0's main.c / debug_console.c / rtg.c are not
 * host-compilable (Xilinx BSP, FatFs, FPGA register access), so the SYMPTOM -- core1
 * never started -- cannot be reproduced here, and no host test will ever boot a card.
 * The INVARIANT behind it can be, because config_file.h is self-contained: every boot
 * mode that is not one of the two real-CPU modes must classify as emulated, for every
 * index in the enum including ones appended after this test was written.
 */
#include <cstdio>
#include <cstdint>   /* config_file.h uses uint8_t/uint32_t and relies on its includers for them */

extern "C" {
#include "../../../Z3660/src/config_file.h"
#include "../../../Z3660/src/overhead_switches.h"
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } \
} while (0)

int main(void)
{
   printf("=== Z3660 core0 boot-mode classification guard ===\n");

   /* 1. The real-CPU set is exactly {MOBOCPU, CPU}. Everything else -- every index in the
    *    enum, including modes appended after today -- must be emulated. This is the check
    *    that the 2026-08-20 bug would have failed: boot_mode_is_emulated(UAE_040_MMU)
    *    was effectively false. */
   printf("[test] every non-real-CPU BOOTMODE index classifies as emulated\n");
   CHECK(!boot_mode_is_emulated(MOBOCPU), "MOBOCPU is a real-CPU mode\n");
   CHECK(!boot_mode_is_emulated(CPU),     "CPU is a real-CPU mode\n");
   for (int m = 0; m < BOOTMODE_NUM; m++) {
      if (m == MOBOCPU || m == CPU)
         continue;
      if (!boot_mode_is_emulated(m)) {
         g_fail++;
         printf("  FAIL: boot mode index %d is not classified as emulated -- core0 would\n"
                "        skip starting core1 and hand the bus to absent silicon\n", m);
      } else {
         g_pass++;
      }
   }

   /* 2. Named spot-checks, so the test also reads as the documentation of the split. */
   printf("[test] named modes land in the right bucket\n");
   CHECK(boot_mode_is_emulated(MUSASHI),     "MUSASHI emulated\n");
   CHECK(boot_mode_is_emulated(UAE_030),     "UAE_030 emulated\n");
   CHECK(boot_mode_is_emulated(UAEJIT_030),  "UAEJIT_030 emulated\n");
   CHECK(boot_mode_is_emulated(UAE_040),     "UAE_040 emulated\n");
   CHECK(boot_mode_is_emulated(UAEJIT_040),  "UAEJIT_040 emulated\n");
   CHECK(boot_mode_is_emulated(UAE_030_MMU), "UAE_030_MMU emulated\n");
   CHECK(boot_mode_is_emulated(UAE_040_MMU), "UAE_040_MMU emulated (the 2026-08-20 bug)\n");

   /* 3. Index alignment with the replicas. The BOOTMODE enum is duplicated in
    *    Z3660_emu/src/main.h (which carries its own static_assert on the count),
    *    z3660-drivers/ZTop/Ztop.c and Z3660/src/ARM_ztop, and core1 compares
    *    shared->cfg_emu against ITS copy -- so a reorder here silently dispatches the
    *    wrong emulator. Pin the literal indices. */
   printf("[test] BOOTMODE indices match the replicated enums\n");
   CHECK(MOBOCPU     == 0, "MOBOCPU == 0\n");
   CHECK(CPU         == 1, "CPU == 1\n");
   CHECK(MUSASHI     == 2, "MUSASHI == 2\n");
   CHECK(UAE_030     == 3, "UAE_030 == 3\n");
   CHECK(UAEJIT_030  == 4, "UAEJIT_030 == 4\n");
   CHECK(UAE_040     == 5, "UAE_040 == 5\n");
   CHECK(UAEJIT_040  == 6, "UAEJIT_040 == 6\n");
   CHECK(UAE_030_MMU == 7, "UAE_030_MMU == 7\n");
   CHECK(UAE_040_MMU == 8, "UAE_040_MMU == 8\n");
   CHECK(BOOTMODE_NUM == 9, "BOOTMODE_NUM == 9 -- if this fires, a mode was appended: put it\n"
                            "        in the right bucket above and update every replica\n");

   /* 4. CYBERSTORM C3 config keys: the CONFITEM enum is APPEND-ONLY.
    *    config_item_names[] is indexed by this enum, so an entry inserted in the middle
    *    shifts every name after it and the parser silently writes the WRONG config field
    *    for the rest of the file -- no error, no diagnostic, just a config that means
    *    something else. (config_file.c carries a compile-time tripwire for a MISSING
    *    name; this one pins the ORDER, which no compiler can see.)
    *    The upstream-merge hazard is live here: beta 21 inserted 48 entries mid-enum. */
   printf("[test] CONFITEM is append-only: this fork's keys sit at the tail\n");
   CHECK(CONFITEM_AMIX_MODE       + 1 == CONFITEM_SERVICE_CADENCE, "amix_mode -> service_cadence\n");
   CHECK(CONFITEM_SERVICE_CADENCE + 1 == CONFITEM_AMIX_RAM,        "service_cadence -> amix_ram\n");
   CHECK(CONFITEM_AMIX_RAM        + 1 == CONFITEM_PERF_STATS,      "amix_ram -> perf_stats\n");
   CHECK(CONFITEM_PERF_STATS      + 1 == CONFITEM_EMU_PROBES,      "perf_stats -> emu_probes\n");
   CHECK(CONFITEM_EMU_PROBES      + 1 == CONFITEM_CORE0_STATS,     "emu_probes -> core0_stats\n");
   CHECK(CONFITEM_CORE0_STATS     + 1 == CONFITEM_NUM,
         "core0_stats is the LAST item -- append new keys after it, never before\n");

   /* 5. The overhead switches resolve the way the whole feature promises they do.
    *
    *    The load-bearing guarantee for a metal calibration session is the NEGATIVE one:
    *    a config file that mentions none of these keys must produce the firmware that
    *    existed before they did. That is one line of code (overhead_switches.h) and it
    *    is the line that must never quietly flip. */
   printf("[test] overhead switches: an absent key reproduces the historical behaviour\n");
   CHECK(z3660_ovh_resolve(Z3660_OVH_UNSET) == 1,
         "unset -> ON: absent perf_stats/emu_probes/core0_stats keys change nothing\n");
   CHECK(z3660_ovh_is_default(z3660_ovh_resolve(Z3660_OVH_UNSET)),
         "and the resolved default IS the default, so the config printer stays silent\n");
   CHECK(z3660_ovh_resolve(YES) == 1, "YES -> ON\n");
   CHECK(z3660_ovh_resolve(NO)  == 0, "NO  -> OFF\n");
   CHECK(z3660_ovh_resolve(0)   == 0, "an explicit off is reachable -- the switch is a real switch\n");
   CHECK(!z3660_ovh_is_default(z3660_ovh_resolve(NO)), "NO is not the default\n");
   /* Anything that is neither the sentinel nor 0/1 is coerced, not propagated: these
    * values go straight into the cross-core mailbox, which core1 tests for truth. */
   CHECK(z3660_ovh_resolve(7)   == 1, "a junk truthy value is coerced to exactly 1\n");
   /* The sentinel and the default are literals that config_file.c writes and compares
    * against by name; pin them, because a config path that initialises to some other
    * "unset" value would resolve every absent key to OFF. */
   CHECK(Z3660_OVH_UNSET   == -1, "the unset sentinel is -1 (what both config paths write)\n");
   CHECK(Z3660_OVH_DEFAULT ==  1, "the default is ON\n");

   printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
