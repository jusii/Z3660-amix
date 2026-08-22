/* Host guard for the AMIX main-RAM window contract (../../../Z3660/src/amix_ram.h).
 *
 * WHY THIS EXISTS. Until now the AMIX window was one compile-time literal --
 * RANGE_MAP(0x0800, 0x0900, drct_bank) -- so it could not be wrong at runtime. It is
 * now sized from config (`amix_ram <MiB>`), which puts three things one typo away from
 * a very bad afternoon:
 *
 *   1. AN OVERSIZED WINDOW IS SILENT, NOT LOUD. reset_autoconfig() maps the RTG/ethernet
 *      combo board at guest $10000000 AFTER uae_emulator() lays down the RAM banks, so a
 *      window that ran past $10000000 would not lose an argument with the board -- the
 *      board would quietly punch a hole through the middle of memory AMIX had already
 *      been told was RAM. Downstream that is a driver that can no longer find its
 *      registers (amix-z3660net binds to Z3660_FIXED 0x10000000) plus a kernel writing
 *      into device space. The window must stop at or below bank 0x1000, always.
 *
 *   2. THE DEFAULT MUST STAY BYTE-IDENTICAL. Every 16 MiB metal boot on both the 030 and
 *      the 040 line is evidence gathered against the old fixed map. If an absent
 *      amix_ram key ever resolved to anything but 16, that whole corpus would silently
 *      stop describing the machine we are running.
 *
 *   3. A REFUSED VALUE MUST NOT BE TRUNCATED. Silently clamping `amix_ram 512` to 128
 *      would boot a machine the operator believes is something else -- and the failure
 *      mode at the top of this range is the guest producing NO console output at all, so
 *      a wrong-but-plausible size is the most expensive possible lie.
 *
 * WHAT THIS CAN AND CANNOT CATCH. It cannot boot a card, and it cannot answer the one
 * question only metal can (does the guest's RAM sizer scale with the mapped extent at
 * all, or does it read a Ramsey register and report 16 MiB no matter what we map --
 * docs/amix-ram-presentation.md §5.1). What it can do is pin the arithmetic and the
 * refusal policy that core0 and core1 BOTH run, since amix_ram.h is the single copy.
 */
#include <cstdio>
#include <cstdint>

extern "C" {
#include "../../../Z3660/src/amix_ram.h"
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } \
} while (0)

int main(void)
{
   printf("=== Z3660 AMIX RAM window contract guard ===\n");

   /* 1. Geometry constants match the addresses they claim to describe. These are the
    *    numbers the map is built from; if one drifts, everything below is fiction. */
   printf("[test] geometry constants\n");
   CHECK(AMIX_RAM_GUEST_BASE == 0x08000000u, "window base is guest $08000000\n");
   CHECK(AMIX_RAM_BASE_BANK  == 0x0800u,     "base bank is $0800 (= $08000000 >> 16)\n");
   CHECK(AMIX_RAM_BANKS_PER_MB == 16u,       "1 MiB = 16 banks of 64 KB\n");
   CHECK((AMIX_RAM_BOARD_BANK << AMIX_RAM_BANK_SHIFT) == 0x10000000u,
         "board bank $1000 is guest $10000000 -- reset_autoconfig()'s RTG base and\n"
         "        amix-z3660net's Z3660_FIXED\n");

   /* 2. The maximum is DERIVED from the board window, not typed in twice. */
   printf("[test] the 128 MiB maximum is derived from the board window\n");
   CHECK(AMIX_RAM_MB_MAX == 128, "max is 128 MiB\n");
   CHECK(amix_ram_end_bank(AMIX_RAM_MB_MAX) == AMIX_RAM_BOARD_BANK,
         "a max-size window ends exactly where the board window begins\n");
   CHECK(amix_ram_bytes(AMIX_RAM_MB_MAX) == 0x08000000u,
         "128 MiB == 0x08000000 bytes (fits drct_bank's declared allocated_size)\n");

   /* 3. THE INVARIANT THE REFUSAL EXISTS TO HOLD. Sweep the whole plausible int range,
    *    not just the ladder: whatever comes out of amix_ram_resolve(), the resulting
    *    bank range must be non-empty and must never reach the board window. This is the
    *    check that makes the other tests trustworthy -- everything else is a spot
    *    sample, this is a proof over the input domain. */
   printf("[test] no input can produce a window that reaches the board window\n");
   {
      int bad = 0, empty = 0;
      for (long v = -100000; v <= 100000; v++) {
         int mb = amix_ram_resolve((int)v);
         unsigned int end = amix_ram_end_bank(mb);
         if (end > AMIX_RAM_BOARD_BANK) { bad++; if (bad == 1) printf("  FAIL: amix_ram %ld -> %d MiB -> end bank $%04X > $%04X\n", v, mb, end, AMIX_RAM_BOARD_BANK); }
         if (end <= AMIX_RAM_BASE_BANK) { empty++; if (empty == 1) printf("  FAIL: amix_ram %ld -> %d MiB -> empty/inverted bank range\n", v, mb); }
      }
      CHECK(bad == 0,   "%d inputs produced a window overlapping the board window\n", bad);
      CHECK(empty == 0, "%d inputs produced an empty or inverted window\n", empty);
      /* INT_MIN / INT_MAX, which the sweep cannot reach. */
      CHECK(amix_ram_end_bank(amix_ram_resolve(2147483647)) <= AMIX_RAM_BOARD_BANK, "INT_MAX bounded\n");
      CHECK(amix_ram_end_bank(amix_ram_resolve(-2147483647 - 1)) > AMIX_RAM_BASE_BANK, "INT_MIN bounded\n");
   }

   /* 4. The default. -1 is "no amix_ram line anywhere"; it must reproduce the map that
    *    every 16 MiB metal boot to date was gathered on. */
   printf("[test] an absent key reproduces the historical 16 MiB map exactly\n");
   CHECK(AMIX_RAM_MB_DEFAULT == 16, "default is 16 MiB\n");
   CHECK(amix_ram_classify(-1) == AMIX_RAM_UNSET, "-1 classifies as unset\n");
   CHECK(amix_ram_resolve(-1) == 16, "unset resolves to 16 MiB\n");
   CHECK(amix_ram_end_bank(amix_ram_resolve(-1)) == 0x0900u,
         "unset yields RANGE_MAP(0x0800, 0x0900, drct_bank) -- the pre-knob literal\n");
   CHECK(amix_ram_resolve(16) == 16, "an explicit 16 is also 16\n");
   CHECK(amix_ram_end_bank(16) == 0x0900u, "explicit 16 yields the same bank range\n");
   CHECK(!amix_ram_hides_a3000mem(amix_ram_resolve(-1)),
         "at the default, a3000mem stays mapped and its alias stays armed\n");

   /* 5. Refusal, not truncation. */
   printf("[test] out-of-range values are refused and fall back, never truncated\n");
   CHECK(amix_ram_classify(0)   == AMIX_RAM_REFUSED_LOW,  "0 is refused\n");
   CHECK(amix_ram_classify(-2)  == AMIX_RAM_REFUSED_LOW,  "-2 is refused (only -1 means unset)\n");
   CHECK(amix_ram_classify(129) == AMIX_RAM_REFUSED_HIGH, "129 is refused\n");
   CHECK(amix_ram_classify(512) == AMIX_RAM_REFUSED_HIGH, "512 is refused\n");
   CHECK(amix_ram_resolve(512) == AMIX_RAM_MB_DEFAULT,
         "a refused value falls back to the DEFAULT (16), not to the bound (128)\n");
   CHECK(amix_ram_resolve(512) != AMIX_RAM_MB_MAX,
         "512 must not silently become 128 -- that is the lie this test exists to stop\n");
   CHECK(amix_ram_resolve(0) == AMIX_RAM_MB_DEFAULT, "0 falls back to 16\n");
   CHECK(amix_ram_is_refused(amix_ram_classify(129)), "is_refused agrees with classify (high)\n");
   CHECK(amix_ram_is_refused(amix_ram_classify(0)),   "is_refused agrees with classify (low)\n");
   CHECK(!amix_ram_is_refused(amix_ram_classify(-1)), "unset is not a refusal\n");

   /* 6. The boundary itself: 128 is IN, 129 is OUT. The ladder's top rung depends on
    *    128 being accepted, so an off-by-one here is not academic. */
   printf("[test] the 128/129 boundary\n");
   CHECK(!amix_ram_is_refused(amix_ram_classify(128)), "128 is accepted\n");
   CHECK(amix_ram_resolve(128) == 128, "128 resolves to itself\n");
   CHECK(amix_ram_end_bank(128) == 0x1000u, "128 ends at bank $1000, touching but not entering\n");
   CHECK(amix_ram_is_refused(amix_ram_classify(129)), "129 is refused\n");
   CHECK(!amix_ram_is_refused(amix_ram_classify(1)), "the minimum, 1, is accepted\n");
   CHECK(amix_ram_end_bank(1) == 0x0810u, "1 MiB = 16 banks\n");

   /* 7. The advisory band. Above the shipping cap the value is still USED -- the ladder
    *    needs 96 and 128 -- it is only flagged. A refusal here would block the very
    *    measurement the knob was added for. */
   printf("[test] the shipping advisory warns but does not refuse\n");
   CHECK(AMIX_RAM_MB_SHIP_MAX == 64, "shipping advisory is 64 MiB\n");
   CHECK(amix_ram_classify(64) == AMIX_RAM_OK, "64 is inside the advisory\n");
   CHECK(amix_ram_classify(65) == AMIX_RAM_EXPERIMENTAL, "65 is flagged experimental\n");
   CHECK(amix_ram_classify(128) == AMIX_RAM_EXPERIMENTAL, "128 is flagged experimental\n");
   CHECK(amix_ram_resolve(96) == 96, "96 is used as given, not clamped to 64\n");
   CHECK(amix_ram_resolve(128) == 128, "128 is used as given\n");

   /* 8. The a3000mem overlap predicate -- the pair of changes that must move together.
    *    a3000mem's host backing is 0x09000000..0x09FFFFFF, i.e. the SECOND 16 MiB of the
    *    window. Past 16 MiB the bank, its startup memset and its get_real_address()
    *    alias must ALL go; leaving the alias armed would redirect guest $07xxxxxx into
    *    live guest RAM, silently, on the same pages the direct-DMA path writes to disk. */
   printf("[test] a3000mem overlap is detected at exactly the right size\n");
   CHECK(AMIX_RAM_A3000MEM_MB == 16, "a3000mem backing is 16 MiB\n");
   CHECK(!amix_ram_hides_a3000mem(16), "at 16 MiB the window stops below host $09000000\n");
   CHECK(amix_ram_hides_a3000mem(17),  "at 17 MiB the window has entered host $09000000\n");
   CHECK(amix_ram_hides_a3000mem(32),  "32 MiB overlaps a3000mem\n");
   CHECK(amix_ram_hides_a3000mem(128), "128 MiB overlaps a3000mem\n");
   /* Stated as the address fact it really is, so a change to A3000MEM_HOST is caught. */
   CHECK(AMIX_RAM_GUEST_BASE + amix_ram_bytes(AMIX_RAM_A3000MEM_MB) == 0x09000000u,
         "a window of exactly AMIX_RAM_A3000MEM_MB ends where a3000mem's backing begins\n");

   /* 9. The metal ladder. These are the rungs the RAM ladder run will actually flash;
    *    the bank ranges are what uae_emulator() will lay down for each, and the byte
    *    counts are what the guest should see IF its sizer scales with the mapped extent.
    *    Recorded here so the expected serial numbers have a checked-in source. */
   printf("[test] the metal ladder rungs\n");
   {
      static const struct { int mb; unsigned int end_bank; unsigned int bytes; } ladder[] = {
         {  16, 0x0900u,  16u * 1024u * 1024u },
         {  32, 0x0A00u,  32u * 1024u * 1024u },
         {  64, 0x0C00u,  64u * 1024u * 1024u },
         {  96, 0x0E00u,  96u * 1024u * 1024u },
         { 128, 0x1000u, 128u * 1024u * 1024u },
      };
      for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
         int mb = ladder[i].mb;
         CHECK(!amix_ram_is_refused(amix_ram_classify(mb)), "rung %d MiB is accepted\n", mb);
         CHECK(amix_ram_resolve(mb) == mb, "rung %d MiB resolves to itself\n", mb);
         CHECK(amix_ram_end_bank(mb) == ladder[i].end_bank,
               "rung %d MiB ends at bank $%04X\n", mb, ladder[i].end_bank);
         CHECK(amix_ram_bytes(mb) == ladder[i].bytes, "rung %d MiB byte count\n", mb);
         CHECK(amix_ram_end_bank(mb) <= AMIX_RAM_BOARD_BANK,
               "rung %d MiB clears the board window\n", mb);
      }
      /* Only the first rung keeps a3000mem; every rung above it drops the bank AND the
       * alias. Spelled out per-rung because "the ladder" is what someone will read. */
      CHECK(!amix_ram_hides_a3000mem(16), "rung 16 keeps a3000mem\n");
      CHECK(amix_ram_hides_a3000mem(32) && amix_ram_hides_a3000mem(64) &&
            amix_ram_hides_a3000mem(96) && amix_ram_hides_a3000mem(128),
            "rungs 32/64/96/128 all drop a3000mem\n");
   }

   /* 10. Every status has text, and refusals say so in words -- the serial banner is the
    *     only thing the operator sees when a value is rejected at 3 a.m. */
   printf("[test] every status carries a message\n");
   {
      static const int all[] = { AMIX_RAM_UNSET, AMIX_RAM_OK, AMIX_RAM_EXPERIMENTAL,
                                 AMIX_RAM_REFUSED_LOW, AMIX_RAM_REFUSED_HIGH };
      for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
         const char *t = amix_ram_status_text(all[i]);
         CHECK(t != 0 && t[0] != '\0', "status %d has non-empty text\n", all[i]);
      }
      CHECK(amix_ram_status_text(AMIX_RAM_REFUSED_HIGH)[0] == 'R', "the high refusal reads REFUSED\n");
      CHECK(amix_ram_status_text(AMIX_RAM_REFUSED_LOW)[0]  == 'R', "the low refusal reads REFUSED\n");
      CHECK(amix_ram_status_text(12345) != 0, "an unknown status still returns text, not NULL\n");
   }

   printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
