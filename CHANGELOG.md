# Changelog

## 2026-07-12

- **piscsi READ: invalidate core1's L1 after the DMA (AMP coherence).** The last T2.P3
  metal bug, and the one that made direct-DMA CD reads unusable. Core0 owns the SD
  transfer and (since e37bb43) invalidates L1+L2 for the target range *before* it — but
  **core1**, the 68k emulator, is parked in `while(shared->write_scsi==1){NOP;}` for the
  whole transfer, and a parked Cortex-A9 is still live: it speculatively refills its own
  L1D over that same range while the DMA is in flight. Nothing ever invalidated core1's
  L1 *after* the transfer, so the guest read a mix of fresh DRAM and stale cache lines.
  `write_scsi_register()` now stashes the READ descriptor as the guest programs it
  (ADDR2 = byte length, ADDR3 = DMA target) and, after the completion spin, calls
  `Xil_L1DCacheInvalidateRange(addr,len)` for READ/READ64/READBYTES only — the WRITE path
  never reads its buffer back, and its trigger-time flush is unchanged. The BSP
  clean+invalidates only the unaligned edge lines, so neighbours of odd driver buffers
  survive. Covers disk **and** CD reads, so it plausibly also closes the rare disk-read
  corruption seen earlier on this platform (the cdfs one-sector cap had been masking it
  by shortening the speculation window).
  Metal-diagnosed on the real A4000+Z3660: with the forced bounce gone, a *quiescent*
  `mount -F cdfs` read the CD byte-correctly, but under combined CD+disk load the guest
  kernel's kmem daemon panicked on a wild freelist pointer (a stale line landing on a
  kmem pool page), and a second run silently corrupted the root filesystem — two failures
  out of two. With the fix: zero panics, zero corruption, and the full R4 read suite
  byte-identical to the ISO (all 7 files `cmp`-verified host-side, including a UTF-8
  name, a three-level directory and a symlink deref), clean umount + remount.

- **De-instrument the piscsi transfer path for production.** Revert the two TEMPORARY
  diagnostic commits that cornered the (now fixed) cdfs-mount fault: the A/B discriminator
  (af29745 — it force-bounced every non-root unit, masking the direct-DMA read path
  entirely) and the per-transfer crumb instrumentation (09520e0). The SD-controller
  coherence fix those crumbs uncovered (e37bb43) is retained verbatim: the WRITE-DIRECT
  revert conflict was resolved by keeping `Xil_DCacheFlushRange` and dropping only the
  crumb call. Net `scsi.c` state = d1da9f8 + bcc8227 + e37bb43. BOOT.BIN 12,412,164 B
  (< 12 MiB gate).

## 2026-07-11

- **firmware boot menu: SD-card file manager (key 'E').** New self-contained
  module `sd_fileops.c/.h` adds an interactive `SD>` command loop over the serial
  console, reachable from `show_options()`/`main_thread()` in mobotest.c. Mounts
  both SD volumes (0: FAT boot, 1: exFAT data) and offers DIR, COPY, REN, DEL,
  MKDIR, CRC, FREE, HELP and EXIT, with explicit `vol:/path` arguments. COPY
  generalizes the fixed BOOT.bin/FAILSAFE.bin/Z3660.bin copy shortcuts: streamed
  through a 4 MB→64 KB fallback buffer (so multi-GB hdf images work), progress
  every ~10%, read-total vs write-total verify, overwrite confirm and ESC-abort.
  CRC is a zlib-compatible CRC-32. Upstream-clean (mobotest.c + the new pair
  only); +8 KB core0 text, no BOOT.BIN size change. Verified on the board
  2026-07-11: CRC matches host zlib byte-exact (incl. the freshly TFTP'd
  Z3660.bin), 943 MB image copy in 101 s with verify green. Also on branch
  `pr-sd-fileops` (same two commits on upstream 5e216af) for an eventual
  upstream PR.

## 2026-07-10

- **scsi: DMA AMIX DDR CPU-RAM buffers directly instead of bouncing.** After the DDR move
  (22ce6f2 put AMIX RAM at $08000000) the firmware's direct/bounce boundary was gated on
  `cpu_ram` — force-disabled for AMIX — while both drivers key their bounce protocol on
  `BOUNCE_THRESH = 0x08000000`. The two sides disagreed about who copies. Gate on
  `cpu_ram || amix_mode` so a buffer inside the drct_bank window
  `[0x08000000,0x10000000)` is DMAed in place, honouring the driver contract (d1da9f8).

- **scsi: make direct-DMA piscsi transfers coherent with the SD controller DMA.** The SD
  controller DMAs to DRAM *below* the PL310 L2, while the guest's freshest bytes live in
  the cache hierarchy. Reads now `Xil_DCacheInvalidateRange` before the transfer, so a
  stale dirty L2 line cannot be written back on top of freshly DMAed bytes (observed on
  real HW: a UFS superblock read returning old guest memory). Writes now
  `Xil_DCacheFlushRange` before the transfer — the write path had **no** cache maintenance
  at all and so deterministically wrote stale DRAM to disk, which physically corrupted the
  SD image's UFS superblock (e37bb43).

- **emu: stop logging the per-command piscsi PDT register poll** — serial noise on every
  command; cosmetic only (bcc8227).

## 2026-07-07

- **piscsi: read-only 2048-byte CD-ROM units.** Expose a SCSI target as a
  read-only CD-ROM backed by a raw ISO/UDF image, alongside the existing RDB
  hardfiles. `struct piscsi_dev_` gains a `pdt` (peripheral device type) field;
  a new read-only mailbox register `PISCSI_CMD_PDT` (0xA0) returns the current
  drive's `pdt` and is the single source of truth for device type. A CD-flagged
  slot in `piscsi_map_drive()` bypasses the RDB scan (a bare optical image has no
  RDB), opens its backing image read-only, reports a 2048-byte block size
  (`BLOCKS = file_size / 2048`), sets `pdt = 0x05`, and uses benign non-zero CHS
  geometry; the normal disk path is unchanged. New config key
  `cdrom_units <id[,id...]>` flags SCSI target IDs as CD-ROM units, threaded
  through `piscsi_init()` into `piscsi_map_drive()`. Removability (start/stop,
  prevent/allow, media-change, unit attention) is deferred — the medium is
  assumed always present in this phase.
- **a3000_scsi (emulated A3000 WD33C93): present a read-only CD-ROM to the stock
  Amix kernel.** A backend unit flagged as a CD-ROM (via `PISCSI_CMD_PDT`, `pdt =
  0x05`) is shown as a read-only optical drive — INQUIRY device-type 0x05 + RMB +
  `"AMIX CD-ROM"`, READ CAPACITY at 2048-byte blocks, MODE SENSE(6)/(10)
  write-protect bit with the rigid-disk geometry pages dropped, WRITE(6)/(10)
  rejected with CHECK CONDITION / DATA PROTECT, and PREVENT/ALLOW MEDIUM REMOVAL as
  a GOOD no-op. The disk path (`pdt = 0x00`) is unchanged. New host unit test
  `test/host/scsi_cd_test.cpp` asserts the exact CDB response bytes (34/34) — the
  byte-level reference the native z3660scsi driver (Path A) must match. READ TOC
  and removability/media-change are deferred to a later increment.
