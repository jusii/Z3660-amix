# Changelog

## 2026-08-06 — new carrier revision: bring-up findings and the 060 rule

- **`docs/carrier-bringup-checklist.md` (`849a4bd`).** Two findings from the new carrier
  revision's bring-up, A/B-proven against the old carrier with the same Zynq module, SD card,
  firmware (`59FAE8CA`) and A4000: **the 68060 must be physically fitted even in pure-emulation
  boot modes** — an empty socket leaves the interrupt//BERR/arbitration lines floating, which
  the boot-time memory test never drives, so the test passes while Kickstart dies 3–13 s in
  (the auto-clicker acknowledges the alert → RESET at ROM `f80de8`, endless reboot loop); and
  **`enable_test`/`test_range` as the carrier-fault discriminator** — its failure pattern
  isolated a CPLD solder bridge (32-bit chip-RAM cycles all garbage while 8-bit CIA cycles
  passed) and a stuck-high D23 (the A4000 Fast-RAM SIMM unseated during the board swaps), plus
  the operational gotchas (region prompts drain buffered `C` keys before the countdown;
  re-disable the test for unattended boots). The new carrier was validated on metal 2026-08-06:
  full stress battery — CD/DMA canaries, disk, CPU, network, warm reboot — PASS.

## 2026-08-02/03 — the web UI, the httpd defects, and the metal validation

- **`httpd` sends a well-formed response header now (`2f4e58a`).** Beta 21's 200-response ended
  every header line with a bare `\r` instead of `\r\n`, so the whole block arrived as one run-on
  status line with no blank line before the body: `HTTP/1.1 200 OKContent-Type: text/htmlConnection:
  close`. The file was transmitted intact — a raw socket read got every byte — but a conforming
  client discarded the body, which read as an empty page. `http_send_404()` two functions later
  already spelled `\r\n`, which is what identified it as a typo. `.json` was also added to
  `mime_type()`; the shipped UI fetches `web/locales/*.json` and they were going out as
  `application/octet-stream`.
- **The web UI's own files are now shipped (`2f4e58a`, `7b9b874`).** `httpd` serves from `1:/web`,
  but beta 21 ships those files only under `Z3660/src/web/` — nothing installs them, so an updated
  board answers every request with `f_open failed … 5` and a 404. Vendored into
  `z3660-zturn_SD_content/…/web/` so they travel with the card image. The markup's fallback text is
  also translated: the loader hardcodes `lang='en'` and fetches only `/locales/en.json`, but every
  literal in the HTML was Spanish — so whenever that fetch failed (i.e. always, given the header
  defect) the page silently rendered in a language the code never selected.
- **Both fixes flashed and verified on hardware 2026-08-03** (firmware crc32 `59FAE8CA`, flashed
  through the rewritten `deploy-kvm.sh`): a strict client now receives the three header lines
  separately and the full 7,410-byte body — it received **zero** before — and `locales/en.json`
  arrives as `application/json` and parses. Post-flash gates all green: `BETA 21`,
  `Emulation mode 4`, `68030 MMU enabled`, A3000 SCSI initialised, Amix to multiuser on the LAN,
  and `validate-metal.sh --cycles 5` PASS on the CD stack.
- **`docs/http-vs-tftp-upload.md` (new)** — measurements behind the workspace's move off TFTP,
  including the correction that HTTP uploads do *not* run while the guest does.
- **Defect catalogue in `docs/AMIX.md` now lists seven beta-21 defects**, none reported upstream:
  the L1-only cache maintenance, the `else`-after-`else`, the `machdep/m68k.h` include guard, the
  `isoN` slot/target trap **and its out-of-bounds write**, the malformed HTTP header, the
  uninstalled web UI, and the UI's fake file rows (`core.bin`/`config.ini`/`logo.png`) with dead
  action buttons.

## 2026-08-01 — beta-21 resync complete (branch `amix-main-2`)

Rebased the Amix fork onto upstream firmware v1.03 beta 21 (`upstream/main` `1dddf79`) on a
fresh branch — upstream rewrote history, so there is no merge base and the resync is a
guided replay, not a merge. Plan of record: `docs/port-ledger-beta21.md`.

**Steps 1–4 (`90cf432..06645d6`).** Build plumbing, `-Os` + `--gc-sections`, the
`SHARED`/`BOOTMODE`/`CONFITEM` ABI merges, and the 68030 PMMU engine + host MMU harness.

**Steps 5–10 (`ebe5381..2fea74c`).** `main.cc` is gone upstream, so its six-hunk delta moved
into `main.cpp` (the `z3660_dbg_emu` accessor, the `a3000_scsi_irq` INT2 injection,
`PISCSI_CMD_PDT`, the AMP read-coherence stash+invalidate from `0a4c064`, the PDT read case,
the `UAE_030_MMU` dispatch). The emulated A3000 WD33C93+SuperDMAC lands whole with the AMIX
memory contract and the CD-ROM CDB test; `scsi.c` regains the 512-byte RDB scan and the
`amix_mode` direct-DMA gate; `config_file` regains `amix_mode`/`service_cadence`; the console
regains DEMU/PERF/SERV alongside beta 21's DENET.

- **§4g — the flash-safety gate (`ddb7caf`).** Beta 21 had weakened our direct-DMA cache
  maintenance to **L1-only** on both the piscsi READ and WRITE paths while keeping our
  "L1+L2" comments. The full-hierarchy `Xil_DCacheInvalidateRange`/`Xil_DCacheFlushRange`
  are restored. On real hardware the weakened form can send stale DRAM to disk and let an L2
  writeback land on freshly DMA-ed bytes; amiberry models no cache hierarchy, so the bench
  cannot test this — **the first flash is the test**.
- **`isoN` adopted (`71f9240`), `cdrom_units` dropped** (grep-absent tree-wide). An ISO is an
  `hdf[]` slot flagged as a CD, so **the slot number must equal the SCSI target**: `iso4` +
  `scsi4 4`. Documented in `z3660cfg.txt`, `config_file.h` and `docs/AMIX.md`.
- **`iso7`..`iso19` are now refused (`8dad55a`).** Upstream's parser computes `index` 0..19
  but `cd_target[]` is 7 deep, so those keys wrote past it into `adf_num[]` and the head of
  `hdf[0]` — silent config corruption in shipping upstream code. Guarded, with a console
  line naming the 0–6 limit. The shipped example moves to the correct `iso4`/`scsi4 4` form
  (upstream's `iso6` + `scsi4 6` pairing produced no working CD).

**Gates at final state:** docker build green with the full engine linked; `BOOT.BIN`
12,428,476 B = 11.853 MiB, **154,436 B under the 12 MiB threshold** (+16 KiB vs the
metal-proven golden `fee33b02`); host MMU harness **64/64**; CD-CDB test **34/34**; the two
`SHARED` copies byte-identical at 51 fields; `CONFITEM`/`config_item_names[]` **129/129**
(`CONFITEM_ISO0..19` inserted at 25–44); `COMMANDS`/`command_names[]` 83/83.

**Four beta-21 defects** are recorded in `docs/AMIX.md` as upstream-report candidates: the
L1-only cache maintenance, an `else`-after-`else` in `main.cpp`'s dead preprocessor arm, the
lost two-phase include guard in `machdep/m68k.h` (blocks any x86 build — fixed here in the
x86 arm), and the `isoN` out-of-bounds write.

## 2026-07-25

- **beta-21 resync port ledger (analysis only).** `docs/port-ledger-beta21.md` (`939936b`): the
  full commit-by-commit plan for rebuilding this fork on upstream firmware v1.03 beta 21.
  Derives the true fork set (90 commits, `main..amix-main`) despite upstream's history rewrite
  leaving no merge base — the resync must be a replay, not a merge — and classifies every commit
  ABSORBED / RE-PORT / DROP with file-level evidence (2/64/18 + 4 split). Contract verdicts:
  ethernet and piscsi-mailbox deltas both **none** (`z3660_scsi_enums.h` byte-identical), so the
  Amix drivers and the amiberry emulation need no changes. Notable hazards recorded: beta 21
  transcribed our direct-DMA coherency comments but weakened the calls to L1-only (§4g — real
  stale-L2 corruption risk on any non-bounce piscsi transfer; re-port + upstream-report
  candidate); a silent cross-core `SHARED`-struct ABI collision at `0xFFFF009C` (§5); an
  invalid-C++ `else`-after-`else` in beta 21 `main.cpp` hidden by preprocessing; BOOT.BIN size
  pressure (`-O3`, gc-sections off) sequenced to be re-proven before the MMU engine in P2.
  `docs/AMIX.md` gains a pointer.

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
