# Where an Amiga RDB may live

**2026-08-25.** BLIZZARD F4-M2 booted a real 68060 with an AMIX root on the piscsi path and
got `[PISCSI] No RDB found on disk, making up some CHS values.` — no partitions, no boot.
Evidence: `Amix/tmp/2026-08-25-blizzard-f4m2/` (`SCORESHEET.md` §4, `80-RDB-LBA0-WORKAROUND.txt`,
serial spans `70-AMIX-BOOT-SPAN.txt` and `82-BOOT2-SPAN.txt`) and, for the measurements below,
`Amix/tmp/2026-08-25-blizzard-blockerB/`.

Written pre-registration-style, like `real-cpu-autoconfig-and-ext-rom.md`: the verdicts first,
then what each rests on, then what is predicted of the change, so a later metal session can
score this page against itself.

---

## 0. Verdict, up front

1. **An AMIX root was unbootable on the real-CPU path, and no single line was wrong.** Three
   shipped lines, each defensible alone, formed a chain that closed. §1.

2. **The gate on the scan was protecting something real** — hybrid disks — and this repo ships
   one. That is why the fix is not "delete the gate". §2.

3. **The replacement key is the disk's own sector 0**, not a config flag. Where an RDB may live
   is a property of the disk. §4.

4. **No AmigaOS disk changes behaviour.** Measured, not argued: both policies replayed against
   all 41 disk images in the workspace. 26 rows differ and **every one of them is an AMIX
   root**; the other 15 — nine AmigaOS RDB disks, two hybrids, three blanks and an ADF — are
   identical. §5.

5. **The scan is now testable.** It moved into a self-contained header the host harness drives
   directly, with the hybrid protection pinned as a regression. 32 checks, and the suite it
   joins is 3587 green. §7.

6. **What is deliberately NOT changed:** the two other links in the chain, `amix_mode`'s
   remaining duties, and the raw-SD scan. §9.

---

## 1. The chain

```
config_file.c:1322   if(config.amix_mode) config.cpu_ram=0;
main.c:1450          if(config.scsiboot && config.cpu_ram) ... else "CPURAM disabled -> SCSI boot disabled"
scsi.c:621           if(config.amix_mode) { per-512-byte-sector RDB scan }
```

Read together: the real-CPU path **requires** `cpu_ram YES` for the SCSI boot ROM, `amix_mode YES`
**forces** `cpu_ram 0`, and the only scan that can find an AMIX RDB was gated on `amix_mode`.

> On `bootmode CPU`, the only RDB scan that can find an AMIX root's RDB was unreachable.
> `amix_mode` and `scsiboot` were mutually exclusive on the real-060 path.

**Single-variable proof, on metal.** Boot 1 (`amix_mode NO`, RDSK at sector 2): `No RDB found`
→ no partitions → Kickstart insert-disk. The work copy's 512-byte RDSK block was then duplicated
to sector 0 (verified all-zero before the write; `rdb_PartitionList` holds absolute block
numbers, so nothing else moves). Boot 2, nothing else altered:

```
[PISCSI] Partition 0: UNIX_Root (9)
[PISCSI] Partition 1: UNIX_Swap (9)
[PISCSI] Partition 2: UNIX_Boot (9)
```

## 2. What the gate was for

Not a mystery — it has its own commit. `c90aef2` (2026-06-26), *"scsi: gate the 512-byte-per-sector
RDB scan on amix_mode"*:

> But a plain-AmigaOS HDF can be a hybrid disk (sector-0 MBR plus an Amiga RDB at a sub-64K
> sector); the per-sector scan finds that RDB and applies geometry the image was never used with
> on stock, so Kickstart can't read DH0.

Upstream scans in 64 KB strides and tests only the first four bytes of each read, so it samples
byte offsets 0, 64K, 128K … — in practice it can only ever find an RDB at sector 0. A hybrid
image's sub-64K RDB is therefore invisible to stock, the image has only ever been used with the
synthetic-CHS fallback, and handing it real RDB geometry breaks it.

**This repository ships two such images.** `hdf/Amix_small.hdf` and `hdf/Amix_small_su.hdf` carry
the VHD `conectix` cookie at sector 0 and a checksum-valid Amiga RDB at **sector 10**. The
hazard is not hypothetical and it is not remote — it is in the tree.

So the gate's intent has to survive. It just cannot be keyed on a flag that also disables CPU RAM.

## 3. The census

Every disk image in the workspace, first 16 sectors (`11-sector-census.txt`, produced by
`rdbscan.py`):

| class | n | sector 0 | RDSK at | stock finds |
|---|---|---|---|---|
| AMIX roots | 26 | **all zero** | sector 2 | nothing → synthetic CHS |
| AmigaOS disks | 9 | `RDSK` | sector 0 | sector 0 |
| hybrid (VHD) | 2 | `conectix` | sector 10 | nothing → synthetic CHS |
| blank / no RDB | 3 | zero or other | — | nothing |
| ADF | 1 | — | — | nothing |

Two things fall out, and both are load-bearing:

- **Every AMIX root has an empty sector 0.** 26 of 26. Nothing claims the front of the disk.
- **Every disk that must not change has something at sector 0** — either the RDB itself, or a
  foreign claimant. There is no disk in the corpus with an empty sector 0 that needs the
  fallback.

The RDB checksum does **not** discriminate: all of them balance, hybrids included
(`rdbsum.py`). Sector 0 is the discriminator, exactly as `c90aef2` said.

## 4. The key

**The sub-LBA0 scan runs only when sector 0 is entirely zero.**

A non-empty sector 0 means something already claims the front of the disk — an MBR, a container
cookie, a boot block — and upstream has always given such a disk the fallback. An empty sector 0
means nothing claims it, and an RDB in blocks 1..15 is then the only partition authority the disk
has. That is precisely how AMIX lays out a root, and precisely what a hybrid is not.

Emptiness rather than "is it an MBR" is the conservative direction: it changes behaviour only for
disks whose front is *blank*, and leaves every disk that carries anything at all exactly where it
was. A new container format nobody has seen yet is protected for free.

A phase-2 candidate must additionally pass the standard RDB checksum, so a stray `RDSK` in the
front of a data disk cannot be promoted into a partition table. Phase 1 does **not** checksum:
adding a test there could reject a disk that boots today.

## 5. Behaviour compatibility, measured

`differential.py` replays both policies — shipped (`amix_mode NO`, upstream's 64K stride) and new
— against all 41 images and compares **the outcome the caller receives**: found-or-not plus the
geometry read out of the accepted block. Full table: `10-old-vs-new-differential.txt`.

```
15 identical, 26 differ
```

- **26 differ.** Every one is an AMIX root: `NONE` → found at sector 2. That is the fix.
- **15 identical.** All nine AmigaOS RDB disks (same geometry, still decided at block 0), both
  `conectix` hybrids (still `NONE` — the protection holds), all three blanks, and the ADF.

**Zero AmigaOS disks change.** The order is preserved too: an RDB at sector 0 is still found on
the first read, before any new code can run, and the harness asserts exactly that (§7 case 1).

## 6. The three phases

`scsi/rdb_scan.h`, driven through two callbacks so the policy carries no transport:

1. **Sector 0.** Upstream's first iteration read 64K here and tested the same four bytes. Every
   AmigaOS disk is decided here, on the first read, accepted on magic alone.
2. **Sectors 1..15**, the RDB spec's own location limit — only if sector 0 is empty, and only for
   a candidate whose checksum balances.
3. **Upstream's remaining 64K sample points** (64K, 128K …), in their old order. Nothing
   spec-conformant can live out there, but a disk found there today must still be found there.

`scsi.c`'s `piscsi_parse_rdb()` has no scan loop of its own on the image-file path: there is one
implementation of this policy and the harness exercises that one. `_Static_assert`s pin the
header's constants to `PISCSI_MAX_BLOCK_SIZE`, `RDB_BLOCK_LIMIT` and `RDB_IDENTIFIER`.

## 7. Harness coverage

`Z3660_emu/test/host/rdb_scan_test.cpp`, `make -C .../test/host run-rdb-scan`. Same pattern as
`bootmode_test.cpp` / `rom_window_test.cpp` / `amix_ram_test.cpp` — core0's self-contained header
by relative path — because `scsi.c` itself is not host-compilable (FatFs + Xilinx BSP). A
synthetic disk supplies the two callbacks and counts the reads.

**32 checks, 0 failures**, and the full suite it joins is **3587 passed, 0 failed**
(`20-host-harness-full.txt`: MMU 3334, CD CDB 34, boot-mode 75, AMIX RAM 81, ROM window 31,
RDB 32).

What it pins: the phase order; that a sector-0 RDB short-circuits after exactly one read and
never runs the 64K scan; the empty-sector-0 key (including that a single non-zero byte anywhere
in sector 0 suppresses phase 2); the checksum qualification; both real hybrid layouts as
regressions (MBR-at-0-with-RDB-at-2, and `conectix`-at-0-with-RDB-at-10); the 1..15 bounds at
both ends; and that a sector-0 RDB with a broken checksum is still accepted.

What it cannot catch, and what therefore still rides on metal: a FatFS or SD transport fault, the
cache maintenance around each read, and what the caller does with the geometry afterwards. Those
live on the far side of the callbacks.

## 8. Pre-registered, for the next metal session

**Read this first: the F4-M2 work root no longer tests the fix.** `f4m2-work-root.hdf` now has
RDSK at **both** sector 0 and sector 2 — the session's workaround is baked into the image. On
that file as it stands the new firmware takes **phase 1** and prints nothing new, which proves
only that phase 1 still works. To exercise the fix, zero sector 0 first (restoring the pristine
AMIX layout), or start from a fresh copy.

Then, with `bootmode CPU`, `scsiboot YES`, `cpu_ram YES`, `amix_mode NO`:

| # | prediction | how it is scored |
|---|---|---|
| 1 | On an AMIX root with sector 0 zeroed, the serial carries exactly one new line: `[PISCSI] RDB at sector 2 (sector 0 empty).` | grep the boot span |
| 2 | Immediately after it, the three partitions appear — `UNIX_Root (9)`, `UNIX_Swap (9)`, `UNIX_Boot (9)` | same as F4-M2 boot 2 |
| 3 | `No RDB found on disk, making up some CHS values.` does **not** appear for that unit | absence in the span |
| 4 | boot1/boot2 load and the kernel runs, with **no** edit to the disk | the F4-M2 boot-2 outcome, workaround removed |
| 5 | On the unmodified work root (RDSK at both 0 and 2) the new line is **absent** and the partitions still appear | phase 1, unchanged |
| 6 | An AmigaOS disk mapped alongside prints no new line at all | absence |

**Falsifiers.** If prediction 1's line appears but the partition names are wrong or the geometry
differs from the AmigaOS-side reading of the same disk, phase 2 found the RDB but the caller is
mis-reading it — that is not this change. If prediction 3 fails, sector 0 was not actually zero;
re-run the census on the file before blaming the firmware.

`amix_mode NO` is correct here and is not a workaround: apart from this scan, `amix_mode`'s
remaining duties are the emulator-side AMIX memory contract (`shared->amix_mode`, `amix_ram`),
and on `bootmode CPU` the emulator is not executing the 68k at all. The direct-DMA gate for the
`$08000000` window is `(config.cpu_ram || config.amix_mode)` — with `cpu_ram YES` it is already
open.

## 9. Not changed here

- **`config_file.c:1322` and `main.c:1450` stand.** Each is right for what it does; with the scan
  no longer keyed on the flag, the chain no longer closes. Touching them would move variables
  this change does not need.
- **`amix_mode` keeps its other duties.** It still opens the direct-DMA path and still drives the
  AMIX memory contract. Only its role as a disk-layout oracle is gone.
- **The raw-SD path is untouched.** It reads one 512-byte block per index for every disk already,
  so it has always scanned blocks 0..15 — i.e. it has the hybrid exposure phase 2's key removes
  on the image path. Changing it means changing shipped behaviour for SD-partition boots with no
  measurement behind it; it is recorded here as a known asymmetry, not fixed blind.
