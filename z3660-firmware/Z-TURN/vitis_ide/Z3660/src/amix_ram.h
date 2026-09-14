/*
 * amix_ram.h -- the AMIX main-RAM window contract (size, bounds, bank arithmetic).
 *
 * FORK-LOCAL (Amix interop). Upstream shanshe/Z3660 has no size-parameterised RAM
 * presentation anywhere; this file is purely additive, so an upstream merge cannot
 * conflict on it. See docs/amix-ram-presentation.md for the full investigation.
 *
 * WHAT THIS IS. Under amix_mode the firmware does not publish a memory-list entry --
 * it maps a range of guest addresses so that it *behaves* like RAM, and something in
 * the guest (the A3000-variant Kickstart's sizer, or the extended-Kickstart module)
 * discovers it and puts it in the AmigaOS memory list. The AMIX loader then
 * AllocMem(MEMF_FAST)s the kernel into that region and the kernel's config() builds
 * MAINSTORE/VSIZOFMEM from bootinfo.memory[] (a verbatim copy of the AmigaOS list).
 * So "how much RAM does AMIX get" is decided by exactly one thing on our side: how
 * many 64 KB banks starting at guest $08000000 point at drct_bank.
 *
 * The AMIX kernel maps physical main RAM into ONE section (SCN1). Every region the
 * AmigaOS memory list holds must lie inside that single window or the kernel adds
 * stray pages to its pool, touches one outside SCN1 and panics vatosde(). That is
 * why the map must present exactly ONE contiguous window with nothing probeable
 * adjacent to it -- a property this header's arithmetic preserves at every size.
 *
 * SHARED BY BOTH CORES AND THE HOST HARNESS. Self-contained (no BSP, no FatFs, no
 * UAE types) so it can be included from:
 *   - core0  Z3660/src/config_file.c        -- parse, validate, refuse, publish
 *   - core1  Z3660_emu/src/uae/uae_emulator.cpp -- build the bank map
 *   - host   Z3660_emu/test/host/amix_ram_test.cpp -- guard the arithmetic
 * Same pattern as boot_mode_is_emulated() in config_file.h, which the host
 * bootmode_test already reaches by relative path.
 */

#ifndef SRC_AMIX_RAM_H_
#define SRC_AMIX_RAM_H_

/* ---------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------ */

/* The guest address the AMIX window starts at. Host DDR is 1:1 here (drct_bank),
 * so guest $08xxxxxx is host 0x08xxxxxx byte for byte. */
#define AMIX_RAM_GUEST_BASE     0x08000000u

/* mem_banks[] is 64 KB-granular: bankindex(addr) = addr >> 16. */
#define AMIX_RAM_BANK_SHIFT     16
#define AMIX_RAM_BASE_BANK      (AMIX_RAM_GUEST_BASE >> AMIX_RAM_BANK_SHIFT)  /* 0x0800 */
#define AMIX_RAM_BANKS_PER_MB   16u                                           /* 1 MiB / 64 KB */

/* ---------------------------------------------------------------------------
 * The hard upper bound: guest $10000000, where the combo board window lives
 * ------------------------------------------------------------------------ */

/* Guest $10000000 is the first bank the AMIX window must NOT reach. Two
 * INDEPENDENT facts put the wall at exactly this address, and they agree:
 *
 *  1. THE DEVICE WINDOW. With autoconfig_rtg NO (the shipped default and what
 *     every AMIX preset uses), reset_autoconfig() hard-maps the RTG/combo board
 *     at guest $10000000 -- Z3660_emu/src/cpu_emulator.cpp:224, then
 *     init_rtg_bank(0x100) -> uae_emulator.cpp:1019-1023 lays rtg_regs_bank +
 *     rtg_bank over banks 0x1000..0x1800. That window is the board base the
 *     amix-z3660net driver binds to (Z3660_FIXED 0x10000000, amix-z3660net/
 *     src/z3660eth.h:16), with its rings at board+$07ED0000/$07EE0000. And
 *     reset_autoconfig() runs AFTER the RANGE_MAPs in uae_emulator(), so an
 *     oversized window would not even lose loudly -- RTG would silently punch a
 *     hole through the middle of what AMIX had been told was RAM.
 *
 *  2. THE ARM PAGE TABLE. drct_bank is 1:1, so guest $1xxxxxxx would be host
 *     0x1xxxxxxx -- and core 1 marks host 0x10000000-0x17FFFFFF RESERVED
 *     (Z3660_emu/src/main.cpp:918-919), which is the Xilinx BSP's FAULT
 *     descriptor. Any 68k access there takes a data abort on the emulator core.
 *
 * Both give the same number, so the bound is not a guess: 128 MiB.
 *
 * TRIPWIRE: if reset_autoconfig()'s 0x10000000 or core1's RESERVED range ever
 * moves, AMIX_RAM_BOARD_BANK must move with it. amix_ram_test.cpp pins it. */
#define AMIX_RAM_BOARD_BANK     0x1000u

/* ---------------------------------------------------------------------------
 * Sizes, in MiB
 * ------------------------------------------------------------------------ */

/* The historical size. Not a kernel limit and never was -- it is one RANGE_MAP
 * literal this fork chose. Kept as the default so an unset amix_ram reproduces
 * the shipped behaviour byte for byte on both the 030 and the 040 line. */
#define AMIX_RAM_MB_DEFAULT     16

/* Below 1 MiB the bank range would be empty or inverted. Refused, not clamped. */
#define AMIX_RAM_MB_MIN         1

/* (0x1000 - 0x0800) banks / 16 banks-per-MiB = 128. Derived, not asserted --
 * amix_ram_test.cpp checks the derivation against the literal. */
#define AMIX_RAM_MB_MAX \
   (int)(((AMIX_RAM_BOARD_BANK - AMIX_RAM_BASE_BANK) / AMIX_RAM_BANKS_PER_MB))

/* ADVISORY, not a bound. The AMIX kernel stops being able to DESCRIBE memory
 * long before the address map runs out: past a computed ~128 MiB of 2 KB pages,
 * rmalloc() returns NULL unchecked and page_init() bit-sets from physical 0 --
 * no panic, no console output at all (grimoire-amix/docs/how-it-works/
 * ram-ceiling.md; mechanism + six-run ablation in amix-kerntools/docs/
 * kernel-52550-hang.md). That bound has NEVER been bisected -- nothing between
 * 32 MiB (works) and 512 MiB (fatal) has been booted -- and it MOVES DOWN as the
 * kernel grows, because it is a function of how much kernel virtual space the
 * running kernel has already spent. Hence a 2x margin for anything shipped.
 *
 * The 4 KB-page Model-B (68040) kernel needs half as many page[] entries per MiB
 * as the 2 KB-page (68030) kernel, so its own ceiling is roughly twice as far
 * out -- which is why the 040 line can be laddered above this figure while the
 * 030 line should not be. Warned about, never enforced: the whole point of the
 * knob is to let the ladder measure where the real line is. */
#define AMIX_RAM_MB_SHIP_MAX    64

/* a3000mem's host backing store is 0x09000000..0x09FFFFFF (A3000MEM_HOST,
 * uae_emulator.cpp:903) -- which is the second 16 MiB of a widened window. So a
 * window larger than this overlaps it and a3000mem must be dropped whole: its
 * RANGE_MAP, its startup memset, AND its get_real_address() aliasing hook. */
#define AMIX_RAM_A3000MEM_MB    16

/* ---------------------------------------------------------------------------
 * Validation
 * ------------------------------------------------------------------------ */

enum AMIX_RAM_STATUS {
   AMIX_RAM_UNSET = 0,      /* no amix_ram key -- the historical default applies */
   AMIX_RAM_OK,             /* in bounds and at or under the shipping advisory */
   AMIX_RAM_EXPERIMENTAL,   /* in bounds, above the shipping advisory -- warn */
   AMIX_RAM_REFUSED_LOW,    /* < AMIX_RAM_MB_MIN */
   AMIX_RAM_REFUSED_HIGH    /* > AMIX_RAM_MB_MAX -- would collide with the board window */
};

/* Classify a requested size. -1 means "key absent". */
static inline int amix_ram_classify(int mb)
{
   if (mb == -1)                return AMIX_RAM_UNSET;
   if (mb < AMIX_RAM_MB_MIN)    return AMIX_RAM_REFUSED_LOW;
   if (mb > AMIX_RAM_MB_MAX)    return AMIX_RAM_REFUSED_HIGH;
   if (mb > AMIX_RAM_MB_SHIP_MAX) return AMIX_RAM_EXPERIMENTAL;
   return AMIX_RAM_OK;
}

static inline int amix_ram_is_refused(int status)
{
   return status == AMIX_RAM_REFUSED_LOW || status == AMIX_RAM_REFUSED_HIGH;
}

/* The size actually used. A REFUSED value falls back to the default -- it is
 * never truncated to the bound, because a silently-truncated 512 would boot a
 * machine at 128 that the operator believes is at 512, and the failure mode at
 * the top of this range is total silence, which is exactly the signal that must
 * not be muddied. The caller is expected to have shouted first (see
 * amix_ram_status_text). Falling back rather than halting keeps the card
 * bootable: the operator gets a known-good 16 MiB machine plus a serial banner
 * naming the refusal, rather than a board that looks bricked. */
static inline int amix_ram_resolve(int mb)
{
   int st = amix_ram_classify(mb);
   if (st == AMIX_RAM_UNSET || amix_ram_is_refused(st))
      return AMIX_RAM_MB_DEFAULT;
   return mb;
}

/* One-line reason, for whichever core is doing the shouting. */
static inline const char *amix_ram_status_text(int status)
{
   switch (status) {
   case AMIX_RAM_UNSET:
      return "not set -- using the default";
   case AMIX_RAM_OK:
      return "ok";
   case AMIX_RAM_EXPERIMENTAL:
      return "ABOVE the shipping advisory: past the kernel's describable-RAM "
             "ceiling the boot dies SILENTLY (no banner, no console output)";
   case AMIX_RAM_REFUSED_LOW:
      return "REFUSED: below the minimum -- the window would be empty";
   case AMIX_RAM_REFUSED_HIGH:
      return "REFUSED: the window would reach guest $10000000 and collide with "
             "the RTG/ethernet combo board window";
   default:
      return "REFUSED: unknown status";
   }
}

/* ---------------------------------------------------------------------------
 * Bank arithmetic -- what uae_emulator() actually maps
 * ------------------------------------------------------------------------ */

/* First bank NOT in the window. Always <= AMIX_RAM_BOARD_BANK for any value
 * amix_ram_resolve() returns; amix_ram_test.cpp proves that over the whole
 * int range, which is the invariant the refusal exists to hold. */
static inline unsigned int amix_ram_end_bank(int mb)
{
   return (unsigned int)AMIX_RAM_BASE_BANK + (unsigned int)mb * AMIX_RAM_BANKS_PER_MB;
}

/* Window size in bytes. 128 MiB = 0x08000000 fits a uint32 with room to spare. */
static inline unsigned int amix_ram_bytes(int mb)
{
   return (unsigned int)mb * 1024u * 1024u;
}

/* Does this window swallow a3000mem's host backing at 0x09000000? If so the
 * a3000mem bank, its startup memset and its get_real_address() alias must all
 * go -- leaving the alias behind would silently redirect guest $07xxxxxx reads
 * and writes into LIVE guest RAM at $09000000. */
static inline int amix_ram_hides_a3000mem(int mb)
{
   return mb > AMIX_RAM_A3000MEM_MB;
}

#endif /* SRC_AMIX_RAM_H_ */
