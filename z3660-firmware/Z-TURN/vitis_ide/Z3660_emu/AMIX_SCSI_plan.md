# Plan: A3000 onboard SCSI (WD33C93 + SuperDMAC) emulation for booting AMIX

> **Status: DESIGN COMPLETE, implementation not started. Branch `amix-boot`
> (off `uae-030-mmu`).** Prereq — the `UAE_030_MMU` 68030 PMMU — works on real
> hardware (the target board). This adds the one remaining AMIX boot blocker:
> a SCSI controller AMIX's kernel can actually drive.

## Why
AMIX 2.1 (Amiga Unix, SVR4) only has SCSI drivers for the **A3000 onboard SCSI
(WD33C93 + Commodore "SuperDMAC"/sdmac)** and the A2091/A590 (also WD33C93). The
Z3660's SCSI is a custom **PISCSI** Zorro II board (`$E90000`) with a proprietary
register protocol only the Z3660 AmigaOS `scsi.device` understands. So today AMIX's
bootstrap loads via PISCSI but its kernel finds **no root device** → panic/hang.

## Ground truth: the working Amiberry config (`~/Amiberry/Configurations/AmigaUnix.uae`)
The reference that boots AMIX. We replicate it:
- `chipset_compatible=A3000`, `ramsey=13`, `fatgary=0`, KS `v2.04 r37.175 (A3000)` (= our `a3k204.rom`).
- `cpu_model=68030 fpu_model=68882 mmu_model=68030 cpu_compatible=false cachesize=0` → exactly `UAE_030_MMU`.
- **SCSI:** `scsi_a3000=true`; disk `uaehf0=hdf,...,Amix.hdf,32,1,2,512,0,,scsi6_a3000`
  (Amix.hdf on the **A3000 onboard controller, target ID 6**, geom 32 sec/1 head/2 reserved, 512-byte blocks).
  A tape image lives on `scsi4_a3000`. **No floppy needed** (user confirmed AMIX now boots straight from the SCSI hdf).
- **RAM:** `chipmem_size=4` (2MB chip) + `a3000mem_size=8` (8MB A3000 mobo fast) + `z3mem=0 fastmem=0`.
  → AMIX = 2MB chip + **8MB** fast, **no Zorro RAM**. AMIX crashes >16MB. The Z3660's 256MB Z3 RAM and
  128MB CPU RAM must BOTH be hidden from AMIX (see "RAM" task). (`cpu_ram YES` was only needed for the
  *PISCSI* scsiboot; with native A3000 SCSI it should not be exposed to AMIX as fast RAM.)
- Network `a2065` — separate effort (`z3660-drivers/amix/AMIX_ethernet_driver_plan.md`).

## Source (the import) — same provenance as the 030 MMU
Port from **WinUAE commit `c7b24b37`** (== Amiberry v5.6.0, the tree the MMU engine came from):
`https://raw.githubusercontent.com/tonioni/WinUAE/c7b24b37/a2091.cpp` (4715 lines, GPLv2 —
consistent with the already-imported GPL MMU/cpuemu code). It is the canonical
`A590/A2091/A3000/CDTV (DMAC/SuperDMAC + WD33C93)` emulation. The A3000 path is `COMMODORE_SDMAC`.
Saved reference copy: `/tmp/winuae_a2091.cpp`.

**DECISION: import/port, don't hand-write.** SCSI phase/IRQ timing is finicky enough that AMIX boot
hangs are likely from scratch code; this exact code is known to boot AMIX. Port the WD33C93 register
file + SASR/SCMD indirect access + phase/command state machine (`wd_*`, ~400-600 lines) and the
SuperDMAC register bank (`COMMODORE_SDMAC` decode + `dmac_dma`/`do_dma`). DO NOT port WinUAE's
autoconfig/board-presence, its `scsi.cpp` disk layer, or its ROM bootstrap.

## Architecture (reuse the Z3660 disk backend)
```
guest (AMIX) MMIO @ $00DD0000  ->  a3000_scsi_bank (new addrbank)
   -> a3000_scsi.cpp : WD33C93 + SuperDMAC state machine (ported)
       -> decodes standard SCSI CDB (READ6/10, WRITE6/10) + synthesizes
          INQUIRY/READ_CAPACITY/TUR/MODE_SENSE/REQUEST_SENSE/START_STOP
       -> data phase via SuperDMAC (ACR address, WTC count)
            -> a3000_scsi_block_io(unit,lba,blocks,buf,write)   [NEW seam in scsi.c]
                 -> devs[unit] + read_disk()/write_disk()   (scsi.c:936-971)  -> Amix.hdf
       -> raises level-2 IRQ into the emulator's intlev()/read_irq   [NEW]
```

## Hook points (concrete, verified file:line)
- **Bank:** add `a3000_scsi_bank` in `Z3660_emu/src/uae/uae_emulator.cpp` modeled on the existing
  `z2scsi_bank` (~line 814) + handlers modeled on `z2scsi_read/write_8/16/32` (~149-263) and
  `z2scsi_check` (~684). Change `uae_emulator.cpp:956` `RANGE_MAP(0x00DD,0x00DE,slow_bank)` →
  `RANGE_MAP(0x00DD,0x00DE,a3000_scsi_bank)`. Leave `0x00DC` (RTC) and `0x00DE-0x00E0` (mobo_bank) alone.
  Dispatch above this (`cpu_emulator.cpp:1024-1145` lget/wget/bget) needs NO change.
- **New files:** `Z3660_emu/src/uae/a3000_scsi.{h,cpp}` (ported state machine + register decode +
  `a3000_scsi_init()`). Add to the Z3660_emu Vitis/Makefile build (same glob as `uae_emulator.cpp`).
- **Backend seam:** add `int a3000_scsi_block_io(uint8_t unit, uint32_t lba, uint32_t blocks,
  uint8_t *buf, int write)` in `Z3660/src/scsi/scsi.c` (decl in `scsi.h`): set `devs[unit].lba=lba`,
  call `read_disk/write_disk(&devs[unit],buf,blocks)`, return GOOD/CHECK_CONDITION. Amix.hdf is
  mounted by `piscsi_map_drive` (scsi.c:626-715) at the `config.scsi_num[]` index. Map target ID 6 →
  that devs[] unit. DMA buffer ptr = ACR translated to CPU-RAM map space (as `handle_piscsi_reg_write`
  does, scsi.c:1016-1069); `Xil_L1DCacheFlush()/Xil_L2CacheFlush()` after DATA-IN.
- **Cross-check oracle:** the existing custom driver `z3660-drivers/scsi/z3660_scsi.c:473-746` already
  interprets these CDB opcodes — match the synthesized INQUIRY/READ_CAPACITY/MODE_SENSE responses to it.

## IRQ wiring — HIGHEST RISK, do first
AMIX's A3000 WD33C93 driver is **interrupt-driven on Amiga INT2 (level 2)**. The firmware's only
software IRQ (`amiga_interrupt_set` → `FPGA_INT6`, interrupt.c:18-37) is the **wrong level (6) and
wrong path (physical FPGA line)** — do NOT reuse it. Instead inject a software level-2 into the
emulator core's IPL: add a `volatile a3000_scsi_irq` flag in the a3000_scsi state and make
`intlev()` / the `check_uae_int_request` path (`newcpu.cpp:2610-2629`, `main.cpp:82-85`) compute
`read_irq = max(physical_pin_IPL, a3000_scsi_irq ? 2 : 0)`. Set on WD33C93/SDMAC INTR (gated by the
SDMAC CNTR INTEN bit); clear on the guest's status-read / DMAC CINT. The INTR must be asserted
**before** AMIX reads status — match a2091.cpp's exact set/clear events (selection complete, phase→
status, DMA done, command-complete message) or AMIX hangs. Level 2 → 68k autovector $68 (the AMIX
side vectors; the emu only sets IPL).

## Register map — extract EXACT offsets from a2091.cpp before coding
Risk #2: every offset in the design report was a placeholder. The real values are in
`/tmp/winuae_a2091.cpp` (`COMMODORE_SDMAC` read/write decode + the `WD_*` register `#define`s near
the top: `WD_AUXILIARY_STATUS`, `WD_DATA`, `WD_COMMAND`, `WD_SCSI_STATUS`, `WD_COMMAND_PHASE`,
`WD_DESTINATION_ID`, `WD_CONTROL`, `WD_TRANSFER_COUNT(_LSB/_MSB)=0x12...`). Read the SuperDMAC ISTR/
CNTR/WTC/ACR/ST_DMA/SP_DMA/CINT/FLUSH offsets out of the `COMMODORE_SDMAC` switch. Confirm the
WD33C93 SASR (index latch, write-only) / SCMD (data, auto-increments index) byte offsets in the
`$00DD` page. Verify ACR is full 32-bit.

## Ordered steps
0. **(GATE done)** a2091.cpp sourced (above). Read out real SDMAC/WD33C93 offsets + INTR set/clear events.
1. Backend seam `a3000_scsi_block_io()` in scsi.c/scsi.h; pin the Amix.hdf devs[] unit ↔ target-ID-6 mapping; cache flush.
2. New `a3000_scsi.{h,cpp}`; state struct (wdregs+sasr index+phase; SDMAC ISTR/CNTR/WTC/ACR + irq flag); add to build.
3. Port WD33C93 + SuperDMAC state machine; strip autoconfig/ROM/disk-layer; redirect data movement to `a3000_scsi_block_io`; synthesize non-data CDBs.
4. `a3000scsi_read/write_8/16/32` + `_check` + `a3000_scsi_bank` in uae_emulator.cpp (model on z2scsi_*).
5. Re-map `0x00DD` → `a3000_scsi_bank`; call `a3000_scsi_init()` at emulator setup.
6. IRQ: level-2 injection into `intlev()/read_irq` (see above).
7. RAM: present AMIX an 8MB A3000-style fast bank, hide the 256MB Z3 + 128MB CPU RAM (preset/firmware — TBD).
8. Build for ARM (docker), no regression to UAE_030_MMU.
9. **Milestone 0:** boot AmigaOS, register round-trip self-test, confirm `$00DD` interception scoped (RTC/mobo OK).
10. **Milestone 1:** boot AMIX; trace WD33C93 RESET/SELECT/TUR/INQUIRY/READ_CAPACITY/READ(10); Amix.hdf superblock → guest RAM; level-2 IRQ fires; AMIX finds root.
11. Full multiuser boot: WRITE path, REQUEST_SENSE/error recovery, disconnect/reconnect; verify on hardware.

## Top risks
1. **IRQ wiring** (level-2 injection + assert-before-status timing). Prototype first.
2. **Exact $00DD register offsets** — extract from a2091.cpp, don't guess.
3. **DMA address/width + cache coherency** — ACR→CPU-RAM map translation + Xil cache flush (32-bit ACR).
4. **Phase/disconnect semantics** — keep WinUAE's phase sequencing intact while redirecting data to the synchronous Z3660 backend.
5. **Target-ID 6 ↔ devs[] unit + geometry** — INQUIRY/READ_CAPACITY must match what AMIX's root config expects.
6. **8MB-RAM presentation** — orthogonal but required for AMIX not to crash.
