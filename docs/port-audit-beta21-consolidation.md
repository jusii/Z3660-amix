# Port audit — consolidating the fork onto the beta-21 line

**Status:** complete. Every fix classified below as MISSING has been ported; the result builds.
**Date:** 2026-08-19
**Target branch:** `amix-main-2` @ `203f108` → `cc54084` (this audit's ports)
**Reference branch:** `piscsi-crawl` @ `f148270` — **frozen**, reference only
**Ruling:** beta 21 is the single going-forward line. `piscsi-crawl` (the beta-20 line) is not developed further.

This is the companion to [port-ledger-beta21.md](port-ledger-beta21.md). That document planned the
resync; this one audits what actually arrived, ports the gap, and proves the result builds.

---

## 1. Why the audit had to be content-level

`amix-main-2` was not branched from the beta-20 line — it is a **re-port** of the same work onto
upstream's recreated beta-21 history (`ebe5381` "emu: port the main.cc delta onto beta 21's
main.cpp" and siblings). The two lines therefore share **no common ancestor**:

```
git merge-base amix-main-2 piscsi-crawl    ->  (no output)
git merge-base amix-main   piscsi-crawl    ->  5b24720
```

Consequences that shape the method:

- **Commit identity means nothing across the boundary.** No `git cherry`, no patch-id matching, and
  no changelog claim can establish that a fix arrived. A re-port can silently drop a hunk while the
  changelog still says the commit was carried.
- **The only ground truth is the content of the touched files.** Every classification below is
  backed by comparing the added lines of the original commit against the target branch's copy of
  the same file, resolving beta-21 renames (`main.cc` → `main.cpp`, `compemu_support.cpp` →
  `compemu_support_arm.cpp`).
- `piscsi-crawl` = `amix-main` + 12 commits, so "is fix X on `amix-main-2`?" is decided against the
  `amix-main-2` tree directly, never against branch topology.

A note on the mechanics, because it nearly produced a completely wrong audit: the first pass ran
`comm` over `sort`ed line sets under a UTF-8 locale, whose collation disagrees with `comm`'s
byte-order expectation. Every file came back "0/N present" with a quiet `comm: input is not in
sorted order` on stderr. Under `LC_ALL=C` the same comparison reported the true figures. **A
missing-hunk result from this kind of tooling is worthless unless the sort order is pinned.**

---

## 2. The post-resync gap (newer than the resync ledger `5b24720`)

Expected to be absent, and all of it was. These are the twelve commits on `piscsi-crawl` newer than
the ledger entry, i.e. work done after the beta-21 re-port had already been cut.

| Commit | Subject | Verdict | Evidence on `amix-main-2` @ `203f108` |
|---|---|---|---|
| `7d54d5c` | scsi: PSTAT per-span latency counters | **MISSING** | no `PSTAT` token in `scsi.c` or `debug_console.c` |
| `f1d94cf` | scsi: uninstall CLMT when `f_lseek(CREATE_LINKMAP)` fails | **MISSING** | `scsi.c` had 1 `CREATE_LINKMAP` hit vs 3 on the reference |
| `320f7ad` | scsi: grow fast-seek link map to 256 items | **MISSING** | no `CLMT` sizing symbols in `scsi.h` |
| `316593a` | scsi: never divide by an unmapped unit's zero block size | **MISSING** | no `block_size == 0` guard in `scsi.c` |
| `7fb42f0` | scsi: stop ignoring `f_lseek`/`f_write` failures on the request path | **MISSING** | 16 `f_lseek` sites vs 27 on the reference |
| `89b1bad` | core0: C0STAT per-task duration accounting | **MISSING** | `core0_stats.c` / `core0_stats.h` absent from the tree entirely |
| `7751813` | docs: map the piscsi service path | **MISSING** | `docs/piscsi-service-path.md` absent |
| `3602247` | docs: mark the piscsi fix list against what landed | **MISSING** | same file absent |
| `9288b87` | docs: 08-17 metal verdict, C0STAT, core0 blocking audit | **MISSING** | `docs/core0-loop-blocking-audit.md` absent |
| `e1093bc` `86b42cc` `f148270` | CHANGELOG companions | **MISSING** | changelog is written at review time, not carried by this audit |

---

## 3. Spot-check of the older consolidated hunks

These predate the resync ledger and were expected to have been carried by the re-port. The point of
checking was to catch a silent drop. **One was found.**

| Area | Commit | Verdict | Evidence |
|---|---|---|---|
| 030 MMU | `d7dcc43` frame-$A restart gated by PC-advance | PRESENT | `newcpu.cpp` 36/36, `newcpu_common.cpp` 7/7, `newcpu_mmu_glue.cpp` 16/16 added lines |
| 030 MMU | `0b42cb8` format-$B frame storage | PRESENT | `newcpu_common.cpp` 31/31 |
| 030 MMU | `3069e22` prefetch-fault sentinel | PRESENT | `newcpu_common.cpp` 15/15; `cpummu030.cpp` is **byte-identical** to the reference (blob `9c30c4e`), and the one unmatched line was superseded on *both* lines by `7ff5774`/`acdfe15` |
| 030 MMU | `7ff5774` multi-fault continuation | PRESENT | `cpummu030.cpp` 24/24, `newcpu.cpp` 30/30 |
| 030 MMU | `acdfe15` in-RTE retry mis-attribution | PRESENT | `cpummu030.cpp` 15/15, `newcpu.cpp` 1/1, host harness 89/89 |
| 030 MMU | `1b22201` PMOVE F-line WinUAE backport | PRESENT | `cpummu030.cpp` 8/8, `cpummu030.h` 1/1, `newcpu.cpp` 6/6 |
| piscsi CD | `86c6fe8` 2048-byte read-only CD units | PRESENT (config key superseded) | `scsi.c` 27/27, `scsi.h` 2/2, enums 3/3, `cd_target[7]` present. The `cdrom_units` **key** is N/A: beta 21 adopted upstream's `isoN` keys (`71f9240`, bounds-fixed in `8dad55a`) |
| piscsi CD | `e3592a7` a3000_scsi pdt=0x05 read-only | PRESENT | `a3000_scsi.cpp` 45/45, host test 170/170, stub 17/17 |
| DMA | `d1da9f8` direct-DMA of DDR CPU-RAM | PRESENT | `scsi.c` 16/16 |
| DMA | `e37bb43` SD-controller DMA coherence | PRESENT | `scsi.c` 22/22; emu side 7/7 in `main.cpp` (the beta-21 successor to `main.cc`) |
| DMA | `0a4c064` core1 L1 invalidate after piscsi READ DMA | PRESENT | 29/29 in `main.cpp` |
| warm reset | `4f409f3` full CPU reset in `cpureset()` | PRESENT | `newcpu.cpp` 21/21 |
| warm reset | `d6c45ef` quiesce real chipset on CORE1 | PRESENT (code) | `newcpu.cpp` 31/31 — the functional CORE1 half landed |
| warm reset | `1ff37ad` quiesce chipset from core0 | N/A — superseded | `d6c45ef` deliberately removed the core0 attempt (core0 deadlocks on `read_reg_s00` while core1 owns the bus) |
| warm reset | `53b6869` re-run `piscsi_init()` on guest reset | **MISSING** | see §4 |
| warm reset | `a1cb13a` clear stale `FPGA_INT6` on guest reset | **MISSING** | see §4 |
| RDB | `c04bc95` 512-byte RDB scan | PRESENT | 39/47 added lines; the 8 unmatched are **comment prose reworded by `c90aef2`**, code identical |
| RDB | `c90aef2` gate the 512-byte scan on `amix_mode` | PRESENT | `scsi.c` 20/20 |
| JIT | `8be8949` `-Os` JIT miscompile fix | PRESENT | 12/12 in `compemu_support_arm.cpp` (renamed) |
| config | `amix_mode` + `service_cadence` keys | PRESENT | token counts equal across lines: 39/39 and 26/26 |

---

## 4. The one real regression: the core0 half of the warm-reset chain

`amix-main-2` carried the **CORE1** half of the warm-reset work (`d6c45ef`'s
`z3660_quiesce_real_chipset_on_reset()` in `newcpu.cpp`, 31/31 lines) but **lost the entire core0
block** from `emulator_reset_thread()` in `Z3660/src/cpu_emulator.c`. The re-port's diff against
`amix-main` shows 31 lines deleted there and nothing equivalent added back:

- `piscsi_init()` on guest reset (`53b6869`) — without it a soft reboot (notably fsck's post-repair
  reboot) restarts Kickstart with no SCSI boot device, hanging at `0x00F81212`.
- `amiga_interrupt_clear(0xFFFFFFFF)` (`a1cb13a`) — without it a board interrupt still pending
  across the reset leaves `FPGA_INT6` asserted into a Kickstart that has no Z3660 INT6 handler yet,
  so level 6 re-fires forever.

Confirmed absent by search, not just by diff: on `203f108` nothing under `Z3660/src` called
`piscsi_init()` outside cold boot (`main.c:1737`, plus a commented-out `main.c:1961`), and no reset
path cleared the interrupt mask. Both are restored here.

This is exactly the failure mode §1 warns about: the changelog recorded the warm-reset chain as
carried, and two thirds of it was — the missing third was invisible at commit level.

---

## 5. What was ported

Cherry-picked onto `amix-main-2`, oldest first, each as its own commit:

| New commit | Ported from | Conflict handling |
|---|---|---|
| `1f3521f` | `53b6869` | tracked `BOOT.BIN` deleted (untracked on this line per `90cf432`) |
| `dc38a02` | `a1cb13a` | same |
| `9b56883` | `7d54d5c` | clean |
| `d182e41` | `f1d94cf` | clean |
| `aea93bf` | `320f7ad` | clean |
| `c0510a7` | `316593a` | clean |
| `dc81392` | `7fb42f0` | clean |
| `b4c2789` | `89b1bad` | include-order conflict only (beta 21's `floppy/floppy.h` vs `core0_stats.h`; both kept) |
| `7d0353b` `578e53f` `146e7d6` | `7751813` `3602247` `9288b87` | clean (docs) |
| `cc54084` | *new* | beta-21 adaptation, see below |

**Verification that the port is faithful:** after porting, `scsi.c` and `scsi.h` differ from the
frozen reference by *only* the pre-existing beta-21 API deltas — the `int debug` parameter on
`process_hunk`/`reloc_hunk(s)`, `FIL fd[16]` vs `fd[8]`, and three whitespace/comment touches. That
is the identical delta measured **before** any porting, so no fix hunk was lost or altered in
transit.

### The one deliberate deviation

`cc54084` is not a cherry-pick. Beta 21 adds two arms to `emulator_thread()`'s dispatch chain that
did not exist when C0STAT was written — `write_floppy` / `read_floppy`, reaching
`handle_pifloppy_reg_write()` / `handle_pifloppy_read()`. Those handlers do FatFs I/O against ADF
images on the *same SD card* the piscsi path uses, so they are precisely the class of callee C0STAT
exists to name. Left alone their time is not lost but is **unattributed** — it lands in the
enclosing `EMU` span, reintroducing for floppies the exact ambiguity that motivated splitting the
priority chain into arms. Two ids were added in loop order immediately after the SCSI arms,
preserving the enum's parent-then-children invariant the worst-call scan depends on; `C0T_NUM` goes
17 → 19, well under the 32 the top-N bitmap allows.

---

## 6. Build

`./docker/run.sh make bootbin` (see [../docker/README.md](../docker/README.md); use `bootbin`, not a
bare `make`, whose `version` step is macOS-only).

| Build | Exit | `BOOT.BIN` size | CRC32 (zlib) |
|---|---|---:|---|
| `203f108` pre-port baseline | 0 | 12 664 036 | `C894CF73` |
| `cc54084` ported | 0 | 12 680 420 | `E64FA66D` |

The ported build is **reproducible** — built twice from clean, byte-identical both times. The whole
port costs **16 384 bytes** (exactly 16 KiB) of `BOOT.BIN`.

### Two traps worth recording

**A stale object survived a branch switch and faked a source defect.** The first build failed at
link with `undefined reference to floatx80_rem(floatx80, floatx80, unsigned long long*, unsigned
long long*, float_status*)`. That signature wants `flag` = `uint64_t`, but beta 21 changed
`softfloat.h` to `typedef uint8_t flag` (and commented out `mamesf.h`'s competing definition), so
nothing on this line should have referenced it. The cause was `Z3660_emu/build/musashi/m68kcpu.o`
dated **Aug 17 20:00** — left over from a beta-20-line build — while every other object was from
the current run. `build/` is gitignored and is *not* cleaned by a branch switch, and the Makefile
does not track header dependencies, so a header-only change like the `flag` typedef will not
trigger a rebuild. **Wipe `Z3660/build` and `Z3660_emu/build` when moving between the two lines.**
The failure was confirmed pre-existing (it reproduced identically at `203f108`) and then vanished
on a clean build of both commits — it was never a source defect and never related to this port.

**The size gate is already exceeded, and not because of this work.** `CLAUDE.md` records "keep
`BOOT.BIN` < ~12 MB or the SD write silently fails". Both builds above are ~12.08–12.09 MiB, i.e.
the **pre-port baseline was already over**. This port adds 16 KiB to an overage inherited from beta
21; it did not cause it. If the gate still binds on the current flash route (beta 21 removed TFTP —
see [http-vs-tftp-upload.md](http-vs-tftp-upload.md)), the cheapest levers are `CORE0_STATS 0` in
`core0_stats.h` and `PISCSI_STATS 0` in `scsi.c`, both of which compile their instrumentation out
entirely. **This wants a deliberate decision before anything is flashed.**

---

## 7. Result

Every fix that was unique to the beta-20 line is now on `amix-main-2`, verified by content rather
than by changelog. The consolidation found one genuine regression (§4) that commit-level or
changelog-level checking would have missed. `piscsi-crawl` is frozen; beta 21 is the line.
