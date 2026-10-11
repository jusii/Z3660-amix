/*
 * rom_window.h -- what may be mapped into the two 512 KB ROM windows, and why.
 *
 * FORK-LOCAL (Amix interop). Purely additive: upstream shanshe/Z3660 has no
 * validation of either ROM image anywhere, so an upstream merge cannot conflict
 * on this file. See docs/real-cpu-autoconfig-and-ext-rom.md for the investigation
 * that produced it (BLIZZARD F0, 2026-08-24).
 *
 * WHAT THIS IS. The FPGA decodes two fixed 512 KB windows out of the Amiga's
 * address space and answers them from ARM DDR:
 *
 *   MAPROM_RANGE     A060[31:19] == 0000000011111  ->  $00F80000..$00FFFFFF
 *   MAPROMEXT_RANGE  A060[31:19] == 0000000011110  ->  $00F00000..$00F7FFFF
 *
 * (z3660.v:281-282, gated by maprom_enable / mapromext_enable, which the FPGA
 * latches from REG0 bits 3 and 7.) core0 fills those windows from files named in
 * z3660cfg.txt and then tells the FPGA to enable the decode.
 *
 * WHY IT HAS TO FAIL CLOSED. Neither window is a buffer the guest merely reads on
 * request -- once the decode is on, Kickstart *scans* them. Whatever is left in the
 * window is executed as ROM: the 68k reset vector comes out of $00F80000, and the
 * RomTagScanner sweeps the extended window for $4AFC resident matchwords and calls
 * whatever init vector it finds. So a load that fails halfway, or a file smaller
 * than the window, does not leave a "partly loaded ROM" -- it leaves uninitialised
 * DDR wired to the CPU's instruction fetch. The two rules that follow are therefore
 * not hygiene:
 *
 *   1. A read that did not fully succeed must leave the decode DISABLED.
 *   2. A file smaller than the window must have the remainder ZEROED, so the
 *      scanner cannot find a stale matchword behind the image.
 *
 * and a file LARGER than the window must be refused rather than truncated-by-luck:
 * core0 reads f_size() bytes into the window with no bound of its own, so an
 * oversized file overruns into whatever the ARM map holds above it.
 *
 * THE EXTENDED-ROM MAGIC. Every extended-ROM image this board accepts begins with
 * the 16-bit ROM id $1111 -- the Amiga extended-ROM convention ($1111 4EF9 and
 * $1114 4EF9 on CDTV/CD32/A570 ROMs) and also what the Z3660's own boot ROMs use
 * (z3660-drivers/kick31_060/kick31_060.asm:56, `dc.w $1111 ;magic DiagROM ID`; all
 * four shipped kick060*.rom images start `1111 007c`). Checking it is what turns
 * "wrong file named in the config" from a silent map of garbage into a refusal.
 *
 * The main Kickstart window is deliberately NOT magic-gated here: the config's
 * kickstart slots legitimately hold whole-ROM images of several vintages plus
 * DiagROM, and upstream only warns on an unusual size. It gets the bound and the
 * completeness check, which is the half that can overrun.
 *
 * SELF-CONTAINED (no BSP, no FatFs, no UAE types) so the host harness can reach it:
 *   - core0  Z3660/src/cpu_emulator.c                    -- load_rom(), load_romext()
 *   - host   Z3660_emu/test/host/rom_window_test.cpp     -- guard the predicate
 * Same pattern as boot_mode_is_emulated() in config_file.h and the amix_ram.h
 * arithmetic, both of which the host harness already reaches by relative path.
 */

#ifndef SRC_ROM_WINDOW_H_
#define SRC_ROM_WINDOW_H_

/* Both windows are 512 KB; see the decode comment above. */
#define ROM_WINDOW_SIZE      (512u * 1024u)

#define KICKSTART_WINDOW_BASE 0x00F80000u
#define EXT_ROM_WINDOW_BASE   0x00F00000u

/* 16-bit ROM id at offset 0 of every extended-ROM image we accept. */
#define EXT_ROM_MAGIC        0x1111u

typedef enum
{
   ROM_LOAD_OK = 0,
   ROM_LOAD_EMPTY,       /* zero-length file                                    */
   ROM_LOAD_TOO_BIG,     /* larger than the window it would be mapped into      */
   ROM_LOAD_SHORT_READ,  /* f_read() returned fewer bytes than the file holds   */
   ROM_LOAD_BAD_MAGIC    /* extended ROM without the $1111 id                   */
} ROM_LOAD_VERDICT;

static inline const char *rom_load_verdict_name(ROM_LOAD_VERDICT v)
{
   switch(v)
   {
   case ROM_LOAD_OK:          return "ok";
   case ROM_LOAD_EMPTY:       return "file is empty";
   case ROM_LOAD_TOO_BIG:     return "file is larger than the 512 KB ROM window";
   case ROM_LOAD_SHORT_READ:  return "short read";
   case ROM_LOAD_BAD_MAGIC:   return "not an extended ROM (missing $1111 id)";
   }
   return "unknown";
}

/*
 * Size/completeness verdict shared by both windows. bytes_read is what f_read()
 * actually delivered; pass size for both when the caller has not read yet and only
 * wants the bound checked.
 */
static inline ROM_LOAD_VERDICT rom_window_verdict(unsigned int size, unsigned int bytes_read)
{
   if(size == 0u)
      return ROM_LOAD_EMPTY;
   if(size > ROM_WINDOW_SIZE)
      return ROM_LOAD_TOO_BIG;
   if(bytes_read != size)
      return ROM_LOAD_SHORT_READ;
   return ROM_LOAD_OK;
}

/*
 * Extended-ROM verdict: the shared checks plus the $1111 id. first_word is the
 * first 16 bits of the image in Amiga (big-endian) order, i.e. buf[0]<<8|buf[1].
 * Only consulted once the size/completeness checks have passed, so a truncated
 * file is reported as truncated rather than as a magic failure.
 */
static inline ROM_LOAD_VERDICT ext_rom_verdict(unsigned int size, unsigned int bytes_read,
                                               unsigned int first_word)
{
   ROM_LOAD_VERDICT v = rom_window_verdict(size, bytes_read);
   if(v != ROM_LOAD_OK)
      return v;
   if((first_word & 0xFFFFu) != EXT_ROM_MAGIC)
      return ROM_LOAD_BAD_MAGIC;
   return ROM_LOAD_OK;
}

#endif /* SRC_ROM_WINDOW_H_ */
