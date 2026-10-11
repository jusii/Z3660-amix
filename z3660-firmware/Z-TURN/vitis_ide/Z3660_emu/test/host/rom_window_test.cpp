/* Host guard for the ROM-window admission rules (../../../Z3660/src/rom_window.h).
 *
 * WHY THIS EXISTS. BLIZZARD F0 (2026-08-24) booted the real 68060 with
 * `ext_kickstart 2` pointing at a 384-byte image and the serial log said, in order,
 * "Extended Kickstart file size: 0x00000180", "File read OK", and then MAPROMEXT
 * stayed ENABLED. load_romext() checked nothing at all: not the size against the
 * window, not f_read()'s byte count, not the image's ROM id -- it printed "File read
 * OK" and returned success unconditionally, so main.c:1723 enabled the decode.
 *
 * That is not a cosmetic fail-open. Once MAPROMEXT is on, the FPGA answers
 * $00F00000..$00F7FFFF out of ARM DDR (z3660.v:281) and Kickstart's RomTagScanner
 * sweeps the whole window for $4AFC resident matchwords, calling whatever init
 * vector it finds. A 384-byte file therefore published ~512 KB of uninitialised DDR
 * to the guest's instruction fetch. In F0 the image happened to be a valid Z3660
 * boot ROM and it happened to execute correctly -- which is precisely why a fail-open
 * of this shape survives: it looks like it works until the day the tail is not zero.
 *
 * The oversize direction is worse and was equally unguarded: both loaders read
 * f_size() bytes into a fixed 512 KB window with no bound of their own, so a file
 * larger than the window overruns into whatever the ARM map holds above it.
 *
 * WHAT THIS CAN AND CANNOT CATCH. It cannot boot a card, and it cannot prove what
 * Kickstart does with a given image -- core0's cpu_emulator.c is not host-compilable
 * (Xilinx BSP + FatFs). What it can pin is the PREDICATE both loaders now consult
 * before enabling a decode, including the exact sizes and the id of every extended
 * ROM this board ships. Same pattern as bootmode_test.cpp and amix_ram_test.cpp,
 * which reach core0's self-contained headers by relative path.
 */
#include <cstdio>
#include <cstdint>

extern "C" {
#include "../../../Z3660/src/rom_window.h"
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } \
} while (0)

int main(void)
{
   printf("=== Z3660 core0 ROM-window admission guard ===\n");

   /* 1. The window is the FPGA's decode, not a number picked here. MAPROM_RANGE and
    *    MAPROMEXT_RANGE both compare A060[31:19], i.e. 2^19 = 512 KB apiece, based at
    *    $00F80000 and $00F00000 (z3660.v:281-282). If any of the three ever drifts from
    *    the RTL the loaders would bound against the wrong extent. */
   printf("[test] window geometry matches the FPGA decode\n");
   CHECK(ROM_WINDOW_SIZE == (1u << 19), "the window is 2^19 = 512 KB\n");
   CHECK(EXT_ROM_WINDOW_BASE == 0x00F00000u, "extended ROM is based at $00F00000\n");
   CHECK(KICKSTART_WINDOW_BASE == 0x00F80000u, "kickstart is based at $00F80000\n");
   CHECK(EXT_ROM_WINDOW_BASE + ROM_WINDOW_SIZE == KICKSTART_WINDOW_BASE,
         "the two windows abut with no gap and no overlap\n");

   /* 2. The F0 case itself, as a regression. A 384-byte file is a legal extended ROM
    *    by size, and the point of the fix is NOT to reject it -- it is that the tail
    *    behind it gets zeroed and that the verdict is reached by checking, not by
    *    printing "File read OK" regardless. A complete read of a valid image passes. */
   printf("[test] the F0 image (384 bytes, id $1111) is admitted when the read is complete\n");
   CHECK(ext_rom_verdict(384, 384, 0x1111) == ROM_LOAD_OK,
         "a complete 384-byte $1111 image is admitted\n");

   /* 3. ...and the same image is REFUSED the moment the read did not complete. This is
    *    the branch load_romext() did not have: f_read()'s byte count was discarded. */
   printf("[test] a short read is refused rather than mapped\n");
   CHECK(ext_rom_verdict(384, 383, 0x1111) == ROM_LOAD_SHORT_READ,
         "one byte short is a short read\n");
   CHECK(ext_rom_verdict(384, 0, 0x1111) == ROM_LOAD_SHORT_READ,
         "a read that delivered nothing is a short read\n");
   CHECK(rom_window_verdict(512u * 1024u, 128u) == ROM_LOAD_SHORT_READ,
         "a truncated full-size kickstart is a short read\n");

   /* 4. The overrun direction. Exactly-the-window is legal; one byte more is not, and
    *    must not be silently truncated -- both loaders size their read from the file. */
   printf("[test] the window bound is exact and refuses overrun\n");
   CHECK(rom_window_verdict(ROM_WINDOW_SIZE, ROM_WINDOW_SIZE) == ROM_LOAD_OK,
         "a 512 KB image exactly fills the window\n");
   CHECK(rom_window_verdict(ROM_WINDOW_SIZE + 1u, ROM_WINDOW_SIZE + 1u) == ROM_LOAD_TOO_BIG,
         "512 KB + 1 byte is refused\n");
   CHECK(rom_window_verdict(1024u * 1024u, 1024u * 1024u) == ROM_LOAD_TOO_BIG,
         "a 1 MB image is refused\n");
   CHECK(ext_rom_verdict(ROM_WINDOW_SIZE + 1u, ROM_WINDOW_SIZE + 1u, 0x1111) == ROM_LOAD_TOO_BIG,
         "oversize beats a good id -- the bound is checked first\n");

   /* 5. Empty files. f_size() == 0 with FA_OPEN_EXISTING is a real outcome (a truncated
    *    upload), and zero bytes into an enabled decode is pure DDR. */
   printf("[test] an empty file is refused\n");
   CHECK(rom_window_verdict(0, 0) == ROM_LOAD_EMPTY, "a zero-length file is empty\n");
   CHECK(ext_rom_verdict(0, 0, 0x1111) == ROM_LOAD_EMPTY, "empty beats a good id\n");

   /* 6. The extended-ROM id. $1111 is the Amiga extended-ROM convention and also what
    *    kick31_060.asm:56 emits; all four shipped kick060*.rom images begin `1111 007c`.
    *    A file named into ext_kickstart that is not one -- an ADF, an .hdf, a hunk
    *    executable, a whole 512 KB Kickstart -- must be refused, not mapped. */
   printf("[test] the extended-ROM id is enforced\n");
   CHECK(ext_rom_verdict(384, 384, 0x1111) == ROM_LOAD_OK, "$1111 is the id\n");
   CHECK(ext_rom_verdict(384, 384, 0x0000) == ROM_LOAD_BAD_MAGIC, "all-zero is refused\n");
   CHECK(ext_rom_verdict(384, 384, 0xFFFF) == ROM_LOAD_BAD_MAGIC, "all-ones is refused\n");
   CHECK(ext_rom_verdict(384, 384, 0x000003F3) == ROM_LOAD_BAD_MAGIC,
         "HUNK_HEADER ($3F3) is refused -- a hunk executable is not an extended ROM\n");
   CHECK(ext_rom_verdict(384, 384, 0x1114) == ROM_LOAD_BAD_MAGIC,
         "$1114 is the 512 KB Kickstart id, not an extended-ROM id\n");
   CHECK(ext_rom_verdict(384, 384, 0xAB001111) == ROM_LOAD_OK,
         "only the low 16 bits are the id\n");

   /* 7. A truncated file reports truncation, not a bad id -- the operator has to be
    *    told which of the two it is, and a short read leaves the id bytes unread. */
   printf("[test] completeness is diagnosed before the id\n");
   CHECK(ext_rom_verdict(384, 1, 0x0000) == ROM_LOAD_SHORT_READ,
         "a short read is reported as a short read, not as a bad id\n");

   /* 8. Every verdict has text. The serial banner is the only thing the operator sees
    *    when a ROM is refused, and "not mapped" with no reason is a support ticket. */
   printf("[test] every verdict carries a message\n");
   {
      static const ROM_LOAD_VERDICT all[] = { ROM_LOAD_OK, ROM_LOAD_EMPTY, ROM_LOAD_TOO_BIG,
                                              ROM_LOAD_SHORT_READ, ROM_LOAD_BAD_MAGIC };
      for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
         const char *t = rom_load_verdict_name(all[i]);
         CHECK(t != 0 && t[0] != '\0', "verdict %d has non-empty text\n", (int)all[i]);
      }
      CHECK(rom_load_verdict_name((ROM_LOAD_VERDICT)12345) != 0,
            "an unknown verdict still returns text, not NULL\n");
   }

   /* 9. The four ROMs this repository actually ships, by their real sizes
    *    (z3660-drivers/kick31_060/). All four must be admitted -- a guard that
    *    rejected the shipped images would be worse than no guard. */
   printf("[test] all four shipped kick060*.rom sizes are admitted\n");
   {
      static const unsigned int shipped[] = { 172, 232, 384, 444 };
      for (unsigned i = 0; i < sizeof(shipped) / sizeof(shipped[0]); i++)
         CHECK(ext_rom_verdict(shipped[i], shipped[i], 0x1111) == ROM_LOAD_OK,
               "the %u-byte shipped ROM is admitted\n", shipped[i]);
   }

   printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
