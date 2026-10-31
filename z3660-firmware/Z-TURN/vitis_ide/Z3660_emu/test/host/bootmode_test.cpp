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
#include "../../../Z3660/src/prof_config.h"
}
/* Profiler. This is the ONLY place in the tree where core0's prof_config.h and
 * core1's z3660_prof.h are compiled side by side. They MUST agree -- core0 writes the
 * command word, core1 decodes it -- and they cannot share a header, because one is core0
 * C and the other lives inside the UAE C++ include tree. Two hand-maintained copies that
 * only ever meet at runtime, on metal, silently, are exactly the shape of the beta-21
 * SHARED mis-merge (docs/piscsi-service-path.md 5). So they meet here instead. */
#include "../../src/uae/z3660_prof.h"

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

   /* 4. Overhead-switch config keys: the CONFITEM enum is APPEND-ONLY.
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
   CHECK(CONFITEM_CORE0_STATS     + 1 == CONFITEM_PROF_HZ,
         "core0_stats is followed by prof_hz\n");
   CHECK(CONFITEM_PROF_HZ         + 1 == CONFITEM_PROF_BUCKETS,
         "prof_hz is followed by prof_buckets\n");
   CHECK(CONFITEM_PROF_BUCKETS    + 1 == CONFITEM_ETH_RERAISE,     "prof_buckets -> eth_reraise\n");
   CHECK(CONFITEM_ETH_RERAISE     + 1 == CONFITEM_ETH_RXKICK,      "eth_reraise -> eth_rxkick\n");
   CHECK(CONFITEM_ETH_RXKICK      + 1 == CONFITEM_ETH_TAILDROP,    "eth_rxkick -> eth_taildrop\n");
   CHECK(CONFITEM_ETH_TAILDROP    + 1 == CONFITEM_ETH_TX_POLL,     "eth_taildrop -> eth_tx_poll\n");
   CHECK(CONFITEM_ETH_TX_POLL     + 1 == CONFITEM_ETH_ADV1000,     "eth_tx_poll -> eth_adv1000\n");
   /* A bound, not an equality: a key appended after the chain is legal and must not fail
    * this test (fec86e0 did exactly that). The chain above still catches any insert into
    * or reorder of this fork's keys; extend it when you append. */
   CHECK(CONFITEM_ETH_ADV1000     <  CONFITEM_NUM,
         "eth_adv1000 is inside the enum -- append new keys after it, never before\n");

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

   /* 6. Profiler: the profiler config keys.
    *
    *    Same negative guarantee as the switches above, landing on the opposite value and
    *    for the same reason. The C3 switches default ON because ON is what this firmware
    *    has always done; these default OFF because a profiler has never existed. "An
    *    absent key changes nothing" is one rule, not two. */
   printf("[test] profiler config: an absent key leaves the board not profiling\n");
   CHECK(z3660_prof_cfg_hz(Z3660_PROF_CFG_UNSET) == 0,
         "unset prof_hz -> 0 = off: an absent key does not start a profiler\n");
   CHECK(z3660_prof_cfg_buckets(Z3660_PROF_CFG_UNSET) == 0,
         "unset prof_buckets -> off\n");
   CHECK(z3660_prof_cfg_is_default(z3660_prof_cfg_hz(Z3660_PROF_CFG_UNSET),
                                   z3660_prof_cfg_buckets(Z3660_PROF_CFG_UNSET)),
         "and the resolved pair IS the default, so the config printer stays silent\n");
   CHECK(Z3660_PROF_CFG_UNSET == -1, "the unset sentinel is -1, as both other headers use\n");
   CHECK(z3660_prof_cfg_hz(0) == 0, "an explicit prof_hz 0 is off, not 'unset'\n");
   CHECK(z3660_prof_cfg_hz(1000) == 1000, "a plain rate passes through\n");
   /* Out-of-range CLAMPS rather than refusing -- deliberately unlike amix_ram, because
    * the worst case here is profiling at a different rate (which the dump header records)
    * rather than a board that does not work. */
   CHECK(z3660_prof_cfg_hz(999999) == Z3660_PROF_CFG_HZ_MAX,
         "an absurd rate clamps to the ceiling instead of refusing to profile\n");
   CHECK(z3660_prof_cfg_hz(-5) == 0, "a negative rate is off, not a clamp to the floor\n");
   CHECK(z3660_prof_cfg_buckets(1) == 1 && z3660_prof_cfg_buckets(7) == 1,
         "a junk truthy prof_buckets is coerced to exactly 1 (it goes into the mailbox)\n");

   printf("[test] profiler: core0's mirrored constants agree with core1's originals\n");
   CHECK((int)Z3660_PROF_CFG_HZ_MIN == (int)Z3660_PROF_HZ_MIN, "HZ_MIN agrees\n");
   CHECK((int)Z3660_PROF_CFG_HZ_MAX == (int)Z3660_PROF_HZ_MAX, "HZ_MAX agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_NONE        == Z3660_PROF_CTL_NONE,        "CTL_NONE agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_ARM         == Z3660_PROF_CTL_ARM,         "CTL_ARM agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_STOP        == Z3660_PROF_CTL_STOP,        "CTL_STOP agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_ZERO        == Z3660_PROF_CTL_ZERO,        "CTL_ZERO agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_DUMPSTATS   == Z3660_PROF_CTL_DUMPSTATS,   "CTL_DUMPSTATS agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_DUMPRING    == Z3660_PROF_CTL_DUMPRING,    "CTL_DUMPRING agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_BUCKETS_ON  == Z3660_PROF_CTL_BUCKETS_ON,  "CTL_BUCKETS_ON agrees\n");
   CHECK(Z3660_PROF_CFG_CTL_BUCKETS_OFF == Z3660_PROF_CTL_BUCKETS_OFF, "CTL_BUCKETS_OFF agrees\n");
   /* The packing is the part that would fail QUIETLY rather than loudly: a command whose
    * argument landed in the wrong half would still decode as SOME valid command. */
   CHECK(Z3660_PROF_CFG_CTL_MAKE(Z3660_PROF_CFG_CTL_DUMPRING, 4096)
         == Z3660_PROF_CTL_MAKE(Z3660_PROF_CTL_DUMPRING, 4096), "the packing agrees\n");
   CHECK(Z3660_PROF_CTL_CMD(Z3660_PROF_CFG_CTL_MAKE(Z3660_PROF_CFG_CTL_ARM, 1))
         == Z3660_PROF_CTL_ARM, "core1 decodes the command core0 packed\n");
   CHECK(Z3660_PROF_CTL_ARG(Z3660_PROF_CFG_CTL_MAKE(Z3660_PROF_CFG_CTL_ARM, 1)) == 1,
         "...and its argument, which is how a config-armed boot profile gets its buckets\n");
   CHECK((unsigned)Z3660_PROF_CFG_RING_ENTRIES == (unsigned)Z3660_PROF_RING_ENTRIES,
         "RING_ENTRIES agrees -- core0 prints the wrap-free window when arming\n");

   /* Profiler groundwork: the `PROF` ladder, and the rule that a rate off the ladder means
    * STOP rather than step-up.
    *
    * Every rung ARMS, and arming ZEROES the ring, the buckets and the counters. The C1
    * metal session armed a boot profile at 200 Hz from z3660cfg.txt and then had no
    * keystroke that meant "stop": the first PROF press mapped 200 into the `< 1000` arm
    * and destroyed a multi-minute capture that could only be retaken by cold-booting.
    * The rule lives in prof_config.h precisely so it can be asserted here -- the console
    * code that runs it needs the Xilinx BSP and cannot be compiled on the host. */
   printf("[test] profiler: the PROF rate ladder, and what it does with a rate it did not set\n");
   CHECK(z3660_prof_cfg_next_rate(0)    == 250,  "off -> 250\n");
   CHECK(z3660_prof_cfg_next_rate(250)  == 1000, "250 -> 1000\n");
   CHECK(z3660_prof_cfg_next_rate(1000) == 4000, "1000 -> 4000\n");
   CHECK(z3660_prof_cfg_next_rate(4000) == 0,    "4000 -> off (the ladder wraps to a stop)\n");
   CHECK(z3660_prof_cfg_is_ladder_rate(0) && z3660_prof_cfg_is_ladder_rate(250) &&
         z3660_prof_cfg_is_ladder_rate(1000) && z3660_prof_cfg_is_ladder_rate(4000),
         "the four ladder rates are recognised as its own\n");
   CHECK(!z3660_prof_cfg_is_ladder_rate(200),
         "200 Hz -- the C1 boot-profile rate -- is NOT one of them\n");
   CHECK(z3660_prof_cfg_next_rate(200) == 0,
         "...so PROF at 200 Hz STOPS (lossless) instead of arming over the capture\n");
   CHECK(z3660_prof_cfg_next_rate(2000) == 0 && z3660_prof_cfg_next_rate(20000) == 0,
         "the same for any other explicitly-chosen rate, above or below a rung\n");
   CHECK(z3660_prof_cfg_next_rate(999) == 0,
         "a rate just under a rung is still off-ladder -- proximity is not membership\n");

   printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
