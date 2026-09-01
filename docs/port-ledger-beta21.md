# Port ledger — resyncing the Amix fork onto upstream firmware v1.03 beta 21

**Status:** analysis only. Nothing in this document has been ported yet.
**Date:** 2026-07-25
**Fork branch:** `amix-main` @ `51038f3`
**Upstream target:** `upstream/main` @ `1dddf79` ("Update adf", 2026-07-22) — firmware **v1.03 beta 21**
**Old fork base:** local branch `main` @ `159b3b5` ("Copy all update system files from beta to alfa folder", 2026-05-13)

> **Historical, and deliberately so.** This is the planning artefact written *before* the resync,
> so every branch and commit named here is as it stood on 2026-07-25. The resync it planned was
> carried out and produced **`amix-main-2`**, which is this fork's branch of record; `amix-main` is
> the superseded pre-beta-21 line. Read this for *why* the replay took the shape it did, not as a
> description of the current tree.

This is the planning artefact for the beta-21 resync. It answers three questions:

1. Which commits are actually *ours*?
2. For each of them: has upstream already absorbed it, must we hand-port it, or is it dead?
3. What changed underneath us in the interfaces the Amix guest drivers depend on?

The re-port itself is a separate exercise. Read this first.

### Where the upstream base stands (added 2026-09-01)

The replay this document planned was carried out; `amix-main-2` is its result. Its merge-base with
`upstream/main` is **`1dddf79`** — the beta-21 commit named above — and it is now **11 commits
behind** that branch:

| Commits | What |
|---|---|
| `068f4b4`, `9ede2c8`, merged `f0c09ae` | **this fork's PR #21** (`fix/clmt-linkmap`) — uninstall the fast-seek link map when creating it fails, and widen it to 256 items |
| `91ce038`, merged `fcdb9d2` | **this fork's PR #20** (`fix/dma-cache-hierarchy`) — whole-hierarchy cache maintenance at the direct-DMA sites, i.e. the §4g regression |
| `efbfc1b`, `c823aeb`, merged `cb1d720`, `d440e30` | codewiz's RTG fixes (zero-pitch blit template, COMPLEMENT lines) |
| `1a1336b` | ZTop.c compiles with gcc |
| `25587f5` | an upstream merge commit |

Merging `upstream/main` into `amix-main-2` is a separate exercise and is **not** done. Two
consequences worth carrying into it: the interface verifications in §6 and §7 — that beta 21 drifted
neither the ethernet register/frame contract nor the piscsi mailbox — were taken at `1dddf79` and are
now eleven commits old; and upstream's copy of §4g is its own fix, so that hunk will merge rather
than conflict.

---

## 1. Deriving the true fork commit set

Upstream **rewrote history** between our fork point and beta 21. The consequence:

```
git merge-base amix-main upstream/main   ->  (no output; no common ancestor)
git merge-base main      upstream/main   ->  (no output; no common ancestor)
```

There is no merge base at all, so the obvious range is badly wrong:

| Range | Count | Meaning |
|---|---:|---|
| `upstream/main..amix-main` | **244** | Everything in our branch with no counterpart object in beta 21 — i.e. our 90 commits **plus the entire pre-rewrite upstream history (154)**. Useless. |
| `main..amix-main` | **90** | Our work, and only our work. |
| `amix-main..main` | **0** | Confirms `main` is a strict ancestor of `amix-main`. |

The local `main` branch is a preserved copy of the **pre-rewrite upstream tip** we forked from. Because `amix-main..main` is empty, `main` is exactly the base of our work, and `main..amix-main` is exactly our patch series. Supporting counts: `main` = 154 commits, `upstream/main` = 160, `amix-main` = 244 = 154 + 90.

**Therefore the fork set is `main..amix-main` = 90 commits, `26746db` … `51038f3` (2026-05-30 … 2026-07-12).**

Because there is no merge base, the resync cannot be a `git merge` or `git rebase`. It has to be a **replay onto a fresh branch cut from `upstream/main`**, guided by this ledger.

### What upstream did in the same window

`git diff main upstream/main` — 457 files, +285 213 / −334 792. Almost all of that volume is BSP/Vivado tree churn (deleted `design_1_wrapper/export/**` headers, regenerated IP). Filtered to the two source trees we care about:

**Added:** floppy emulation (`Z3660/src/floppy/*`, `z3660-drivers/floppy/*`), a web UI + `lwip/httpd.c`, wazp3d NEON/SIMD headers, `xc3_usleep.h`, **`Z3660/src/sd_fileops.{c,h}`** (see §4c), `uae/include/uae/likely.h`, `uae/jit/compemu_arm.cpp`.

**Deleted:** **`Z3660_emu/src/main.cc`**, **`Z3660_emu/src/set_cpu_freq.cc`**, `uae/jit/compemu.cpp`, the standalone USB test files (`usb_mouse_test_improved.c`, `usb_test_standalone.*`, `usb_hid_diagnostic.c`, `z3660_usb_handler.*`), the pl_mpeg player files.

**Renamed:** the whole JIT set gained an `_arm` suffix — `compemu_support.cpp` → `compemu_support_arm.cpp`, `compemu.h` → `compemu_arm.h`, `compemu_fpp.cpp` → `compemu_fpp_arm.cpp`, `compstbl.cpp` → `compstbl_arm.cpp`, `comptbl.h` → `comptbl_arm.h`.

**Critically low churn where our work lives.** The UAE 68k core barely moved:

| File | upstream delta (base → beta 21) |
|---|---|
| `uae/cpummu030.cpp` | **unchanged** |
| `uae/cpuemu_32.cpp` | **unchanged** |
| `uae/newcpu_mmu_glue.cpp` | **unchanged** |
| `uae/include/mmu_common.h`, `include/memory.h` | **unchanged** |
| `uae/newcpu.cpp` | +6 / −5 |
| `uae/newcpu_common.cpp` | +6 / −4 |
| `uae/include/newcpu.h` | +2 / −2 |
| `uae/uae_emulator.cpp` | +71 / −18 |
| `Z3660_emu/src/cpu_emulator.cpp` | +111 / −19 |

So the 030 PMMU engine — the single largest and riskiest part of the fork — re-ports onto a nearly untouched substrate. The friction is concentrated in `main.cc`→`main.cpp`, the shared cross-core struct, the config enum, and the Makefile.

### Upstream has been pulling from this fork

Several of our commits appear in beta 21 **byte-identical, including our own comment text** — `sd_fileops.{c,h}` (identical blob SHAs), the `usleep2` shim (our comment verbatim), `PISCSI_CMD_PDT` (our comment verbatim), the `cd_target[7]` struct field (our comment verbatim, *including its reference to our `cdrom_units` config key which beta 21 no longer implements*), and the CD-ROM branch in `piscsi_map_drive()`. That is the mechanism behind most ABSORBED rows below, and it is why the disposition has to be checked per hunk rather than per commit — upstream absorbed some hunks and not others, and in two places (§4g, §5) transcribed a change incorrectly.

---

## 2. Classification summary

| Disposition | Rows | Meaning |
|---|---:|---|
| **ABSORBED** | 2 | Beta 21 already contains the whole commit. Do not re-apply. |
| **RE-PORT** | 64 | Must be hand-carried onto beta 21. |
| **DROP** | 18 | Obsolete, TEMP, superseded, or a rebuilt artefact. |
| **SPLIT** | 4 | Part re-ports, part drops — the halves are listed in the row. |

88 ledger rows cover all 90 commits (two TEMP/revert pairs are squashed into one row each).

The whole-commit ABSORBED count is low, but it understates how much upstream took: absorption happened at **hunk** granularity, not commit granularity. `86c6fe8` (CD-ROM units) is absorbed apart from its config front end; `1ea7b8a` is absorbed for exactly one file (`likely.h`); `e37bb43`'s comments are absorbed while its code is not (§4g); `9902fdd`'s loud-halt hunk is absorbed in one preprocessor arm and mis-transcribed in the other (§4a). Read §4 before treating any RE-PORT row as a blind re-apply.

Conventions used below:

- **RE-PORT (clean)** — the file is untouched or barely touched upstream; expect a near-literal apply.
- **RE-PORT (rebase)** — the target file moved, was renamed, or changed around our hunk; the change is still needed but the insertion point must be re-found.
- **RE-PORT (additive)** — introduces files/dirs that do not exist upstream at all; no conflict possible.
- **DROP (superseded)** — a later commit of ours supersedes it; we port the *final file state*, not the intermediate step.

---

## 3. Full commit classification

### 3.1 Host MMU test harness and gencpu tooling

Beta 21 has **no `Z3660_emu/test/` directory and no `UAE_030_MMU_plan.md` / `AMIX_SCSI_*.md`** — verified by tree listing. Everything in this group is conflict-free additive.

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `26746db` | Layer-1 host MMU harness skeleton | RE-PORT (additive) | `Z3660_emu/test/host/**` — absent upstream |
| `f3d5ce0` | harness: 68030 interpreter + first exec tests | RE-PORT (additive) | ditto; also touches `uae/newcpu.cpp` (+6/−5 upstream) |
| `1863858` | gencpu toolchain + non-MMU baseline | RE-PORT (additive) | `Z3660_emu/test/gencpu/**` — absent upstream |
| `2f349a9` | harness: adversarial-instruction baseline | RE-PORT (additive) | `test/host/harness_main.cpp` |
| `2c049e7` | first MMU030 translation test (TTR) | RE-PORT (additive) | `test/host/harness_main.cpp` |
| `24651ec` | MMU030 page-table walk 36/36 | RE-PORT (additive) | `test/host/harness_main.cpp` |
| `349b026` | MMU030 tests 3–5 (fault/restart, LRMW, PFLUSH) 46/46 | RE-PORT (additive) | `test/host/{board_stubs.cpp,harness_main.cpp,stubs/sleep.h}` |
| `5247b0a` | stub A3000-SCSI seams so harness links | RE-PORT (additive) | `test/host/board_stubs.cpp` |
| `26072ee` | stub `shared` + `z3660_dbg_emu` for harness | RE-PORT (additive) | `test/host/board_stubs.cpp` — must track the beta-21 `SHARED` layout, see §5 |
| `38cca35` | UAE_030_MMU plan: grilling decisions | DROP | Historical planning doc; superseded by `5887703` and by this ledger |
| `90da035` | plan: gencpu Phase-0 baseline result | DROP | ditto |
| `770e5be` | plan: progress log | DROP | ditto |
| `5887703` | plan: mark UAE_030_MMU implemented | RE-PORT (additive) | Keep the final `UAE_030_MMU_plan.md` state only — one file, not the four-commit history |
| `9557aed` | plan A3000 WD33C93+SuperDMAC emulation | RE-PORT (additive) | `Z3660_emu/AMIX_SCSI_plan.md` — keep final state |

### 3.2 The 68030 PMMU engine

The heart of the fork. Substrate is essentially unchanged upstream (see §1), so these are mostly clean applies. Port the **final file state** of `cpummu030.cpp`, `cpummu.cpp`, `cpuemu_32.cpp`, `cpustbl_mmu030.cpp`, `newcpu_mmu_glue.cpp` (all upstream-unchanged files) and hand-merge `newcpu.cpp` / `newcpu_common.cpp` / `include/newcpu.h`.

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `1ea7b8a` | import WinUAE 4.4.0 MMU headers + regstruct fields | RE-PORT (rebase) — **partly ABSORBED** | `uae/include/{cpummu.h,cpummu030.h,debug.h,mmu_common.h,newcpu.h}` re-port. **`uae/include/uae/likely.h` is ABSORBED — blob `635ed392…` identical in `amix-main` and `upstream/main`.** Do not re-add it. |
| `a015607` | MMU engine compiles + links | RE-PORT (clean) | `uae/cpummu.cpp`, `uae/cpummu030.cpp` (both absent/unchanged upstream), `include/sysconfig.h`, `newcpu_mmu_glue.cpp` |
| `9cbf8f5` | wire `mmu_op30` dispatch + `set_x_funcs` MMU arm | RE-PORT (rebase) | `uae/newcpu.cpp` |
| `e49192a` | generate + integrate `cpuemu_32` + cputbls column | RE-PORT (clean) | `uae/cpuemu_32.cpp`, `uae/cpustbl_mmu030.cpp` — unchanged upstream |
| `60d36cd` | `m68k_run_mmu030` run loop 49/49 | RE-PORT (rebase) | `uae/newcpu.cpp` |
| `924b670` | **fix critical demand-paging bus-error frame** | RE-PORT (rebase) | `uae/newcpu.cpp`, `newcpu_common.cpp`, `newcpu_mmu_glue.cpp`, `include/{cpummu030.h,newcpu.h}` |
| `ab75b44` | roll back `MOVES (An)+` on write page fault | RE-PORT (rebase) | `uae/newcpu.cpp` |
| `f81ddf1` | restart faulting `(An)` writes from instruction-start PC | RE-PORT (clean) | `uae/cpuemu_32.cpp` (unchanged upstream), `newcpu.cpp` |
| `f214552` | stack fault address in stage-B field | RE-PORT (clean) | `uae/newcpu_common.cpp` |
| `3069e22` | restore prefetch-fault sentinel in frame $B | RE-PORT (clean) | `uae/cpummu030.cpp` (unchanged upstream), `newcpu_common.cpp` |
| `0b42cb8` | full WinUAE format-$B frame storage | RE-PORT (clean) | `uae/newcpu_common.cpp` |
| `d7dcc43` | **gate frame-$A restart by PC-advance (RMW double-apply)** | RE-PORT (rebase) | `uae/newcpu.cpp`, `newcpu_common.cpp`, `newcpu_mmu_glue.cpp` — the "boots to login" fix |
| `1b22201` | backport WinUAE `a333766b` PMOVE config exception | RE-PORT (clean) | `uae/cpummu030.cpp`, `include/cpummu030.h`, `newcpu.cpp` |
| `a8ca043` | diag: catch AMIX user-PC wild jump | DROP | Diagnostic scaffolding for the BUS ERROR hunt; the hunt closed with `7ff5774` + `acdfe15` |
| `7ff5774` | **fix multi-fault continuation corrupting bus-error frame PC** | RE-PORT (clean) | `uae/cpummu030.cpp`, `newcpu.cpp` |
| `acdfe15` | **fix in-RTE retry-access mis-attributed bus-error frame** | RE-PORT (clean) | `uae/cpummu030.cpp`, `newcpu.cpp`, `test/host/harness_main.cpp`, `CHANGES.md` |
| `1138b3f` | gate iack_cycle + cpureset changes on `mmu_model==68030` | RE-PORT (rebase) | `uae/newcpu.cpp` |
| `71d89ac` | clean reboot on AMIX warm reset (skip PISCSI flood) | RE-PORT (rebase) | `uae/newcpu.cpp` |
| `4f409f3` | `cpureset()` does a full CPU reset (PC=0xF800D2) | RE-PORT (rebase) | `uae/newcpu.cpp` |
| `d6c45ef` | warm-reset: quiesce real chipset on CORE1 | RE-PORT (rebase) | `Z3660/src/cpu_emulator.c`, `uae/newcpu.cpp` |

### 3.3 Boot-mode plumbing, amix_mode, debug console, perf

Highest-friction group after `main.cc`. See §5 for the shared-struct hazard and §4f for the enum hazard.

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `9902fdd` | UAE_030_MMU bootmode threaded through enum + dispatch | RE-PORT (rebase) | `Z3660/src/config_file.{c,h}`, `Z3660/src/ARM_ztop/list_button.{c,h}`, `z3660-drivers/ZTop/Ztop.c`, `Z3660_emu/src/{main.cpp,main.h}`, `uae/uae_emulator.{cpp,h}`. Beta 21 leaves `enum BOOTMODE` **unchanged** (7 modes) — `UAE_030_MMU` still appends cleanly at index 7 and the `BOOTMODE_NUM == 8` static_assert stays valid. `uae_emulator.h` unchanged upstream, so the defaulted-arg signature `uae_emulator(int,int,int=0,int=0)` applies as-is. |
| `9fa72ac` | thread the mode through control-core emulator-mode checks | RE-PORT (rebase) | `Z3660/src/{debug_console.c,main.c,rtg/rtg.c}` — `rtg.c` is heavily churned upstream (+107/−221) |
| `50ffe30` | decouple AMIX memory contract from UAE_030_MMU boot mode | RE-PORT (rebase) | `Z3660/src/{config_file.c,config_file.h,main.h}`, `Z3660_emu/src/{main.cc→main.cpp,main.h}`, `uae/uae_emulator.{cpp,h}` — **shared-struct collision, see §5** |
| `51d7d5c` | tri-state verbosity (DBGLVL serial command) | RE-PORT (rebase) | `Z3660/src/{debug_console.c,debug_console.h,main.h,scsi/scsi.c}`, `Z3660_emu/src/main.h`. Note `debug_console.h` is **byte-identical** across base/ours/beta 21 (`2f1aaabe…`) — our net change there is zero; all menu additions live in `debug_console.c`. |
| `8c02f87` | per-category debug gating (SCSI + EMU toggle) | RE-PORT (rebase) | as above plus `Z3660_emu/src/main.cc→main.cpp`, `uae/cpummu030.cpp`, `newcpu.cpp`. **Beta 21 added its own `DENET`/`DEBUG_ENET` cases to the same `debug_console.c` switch** — our `DEMU`/`DEBUG_EMU` must be merged alongside, not over. |
| `f7bbdb0` | 030-MMU poll cadence (SERV) + instruction-rate readout (PERF) | RE-PORT (rebase) | `Z3660/src/{debug_console.c,main.h}`, `Z3660_emu/src/main.h`, `uae/newcpu.cpp`, `test/host/stubs/xtime_l.h` — shared-struct collision, §5 |
| `e0e17d5` | `service_cadence` config option (z3660cfg + per-preset) | RE-PORT (rebase) | `Z3660/src/{config_file.c,config_file.h,debug_console.c}` — CONFITEM enum hazard, §4f |
| `22ce6f2` | AMIX on Zynq-local DDR (`$08000000` CPU-RAM); drop AMIX_HIDE08 | RE-PORT (clean) | `uae/include/mmu_common.h` (unchanged upstream), `uae/uae_emulator.cpp` (+71/−18 upstream — re-find insertion point) |

### 3.4 Emulated A3000 WD33C93 + SuperDMAC SCSI

`uae/a3000_scsi.{cpp,h}` **do not exist in beta 21** (verified by tree listing) — pure additive. We port the final file state, so the WIP/diagnostic checkpoints are superseded.

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `47aeaf8` | boot AMIX 2.1 kernel + init on the A3000 SCSI | RE-PORT (additive + rebase) | Introduces `uae/a3000_scsi.{cpp,h}`. Also carries the **Makefile `-Os` + `--gc-sections`** change — see §4e. |
| `f8e3a2b` | WIP intmask-gated completion fix (+diagnostics) | DROP (superseded) | Intermediate state of `a3000_scsi.cpp`; final content lands via `e91dedc`/`c03449c` |
| `bbbea46` | WIP desync/clobber detector + paced completion | DROP (superseded) | ditto |
| `665cb06` | diag: dispatch-action histogram + IPL-preemption detector | DROP (superseded) | `uae/newcpu.cpp` diagnostics, removed by later work |
| `c03449c` | model SuperDMAC end-of-process (DMA-done) interrupt | RE-PORT (additive) | `uae/a3000_scsi.cpp` |
| `e91dedc` | make completion delivery faithful to WinUAE `a2091.cpp` | RE-PORT (additive) | `uae/a3000_scsi.cpp` |
| `12246d3` | AMIX autoboots from SCSI id 6 alone (drop id-6→drive-0 alias) | RE-PORT (additive) | `uae/a3000_scsi.cpp`, `CHANGELOG`/`CHANGES.md` |
| `e3592a7` | present a read-only CD-ROM (pdt=0x05) to the Amix kernel | RE-PORT (additive) | `uae/a3000_scsi.cpp`, `test/host/{Makefile,scsi_cd_test.cpp,stubs/xil_cache.h}` |
| `b3f23fa` | raise A3000-KS auto-click cap for AMIX warm reboot | RE-PORT (rebase) | `Z3660_emu/src/cpu_emulator.cpp` (+111/−19 upstream) |

### 3.5 piscsi / `Z3660/src/scsi/scsi.c`

The most tangled group: upstream absorbed some of these hunks, weakened one (§4g), and left the rest.

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `86c6fe8` | piscsi: read-only 2048-byte CD-ROM units | **ABSORBED (mostly)** | Beta 21 contains, byte-identical with our comments: `piscsi_map_drive(..., uint8_t is_cd)` + all three call sites; the `is_cd ? (FA_READ\|FA_OPEN_EXISTING) : …` read-only open; the whole `if (is_cd) { block_size=2048; pdt=0x05; h=16; s=63; c=… }` branch; `PISCSI_CMD_PDT = 0xA0` in `z3660_scsi_enums.h`; `case PISCSI_CMD_PDT` in `handle_piscsi_read()`; and `int cd_target[7]` in `config_file.h`. **`z3660_scsi_enums.h` is byte-identical between `amix-main` and `upstream/main`.** Only the `cdrom_units` config parsing is not absorbed — see §4b. |
| `c04bc95` | scan 512-byte blocks for RDB (fix fsck-every-boot loop) | RE-PORT (clean) | `scsi.c` `piscsi_parse_rdb()` — **not absorbed**; beta 21 still has the 64K-stride scan only |
| `c90aef2` | gate the 512-byte RDB scan on `amix_mode` | RE-PORT (clean) | folds into the same `piscsi_parse_rdb()` hunk; port as one |
| `d1da9f8` | DMA AMIX DDR CPU-RAM buffers directly instead of bouncing | RE-PORT (clean) | The `(config.cpu_ram \|\| config.amix_mode)` gate in both the READ and WRITE handlers — beta 21 still has bare `config.cpu_ram` |
| `e37bb43` | make direct-DMA transfers coherent with the SD controller DMA | **SPLIT — see §4g** | Comment blocks ABSORBED verbatim; **the cache calls were weakened to L1-only upstream and must be RE-PORTed.** Also touches `main.cc`→`main.cpp`. |
| `09520e0` + `21ef23b` | TEMPORARY piscsi transfer crumbs, and its revert | DROP | Add/revert pair, net zero |
| `af29745` + `8b16b87` | TEMPORARY A/B discriminator for cdfs-mount fault, and its revert | DROP | Add/revert pair, net zero |
| `bcc8227` | stop logging the per-command piscsi PDT register poll | RE-PORT (rebase) | `main.cc`+`main.cpp` → beta 21 `main.cpp` only, §4a |
| `53b6869` | re-run `piscsi_init()` on guest reset (post-fsck reboot hang) | RE-PORT (clean) | `Z3660/src/cpu_emulator.c` `emulator_reset_thread()`. Upstream's change to this file is in `emulator_thread()` (floppy mailbox branches) — **non-overlapping**. |
| `a1cb13a` | clear stale Amiga board interrupt (FPGA_INT6) on guest reset | RE-PORT (clean) | same function, same commit region |
| `1ff37ad` | quiesce real A4000 chipset interrupts before 68k restart | RE-PORT (clean) | `Z3660/src/cpu_emulator.c` |
| `0a4c064` | **invalidate core1 L1 after piscsi READ DMA (AMP coherence)** | RE-PORT (rebase) | `main.cc` → beta 21 `main.cpp` — **hot item, §4a** |

### 3.6 SD-card file manager

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `f6be42b` | Add SD-card file manager for the boot serial console | **DROP (absorbed)** | `sd_fileops.c` blob `18f9e7ba…` and `sd_fileops.h` blob `61b25150…` are **identical in `amix-main` and `upstream/main`**. See §4c. |
| `01b8b18` | Add SD file manager entry (key 'E') to the boot menu | **DROP (absorbed)** | `mobotest.c` `else if(keybd_data=='e' \|\| keybd_data=='E') { sd_fileops_menu(); }` is byte-identical in both trees |
| `5ef7c19` | Changelog: SD-card file manager | DROP | Changelog for absorbed work |

### 3.7 Build, docker, toolchain

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `8cd8a07` | fix pre-existing `usleep` build break | **ABSORBED** | Beta 21 `Z3660_emu/src/cpu_emulator.cpp:1930-1937` contains our `usleep2()` shim **with our comment verbatim**; `uae/newcpu.cpp:2554` calls `usleep2(100000L)`. Only our trailing inline comment on the call site is missing (cosmetic). |
| `c2a5c1d` | fix pre-existing USB-driver build breaks | **SPLIT** | `usb/asm/ch9.h` — **RE-PORT**: beta 21's `ch9.h` is identical to the old base, so our fix was *not* absorbed and is still needed. `usb/usb_mouse_test_improved.c` — **DROP**: upstream deleted the file. |
| `7794e40` | docker: make BOOT.BIN assembly work (no tool install) | RE-PORT (clean) | `vitis_ide/Makefile`, `Z3660_system/Alfa/system_cli.bif` |
| `a015123` | fix `main.cc`/`main.cpp` → `build/main.o` collision | **DROP (obsolete)** | The twin is gone: beta 21 deleted `main.cc`. See §4d. (Its second half — mirroring the UAE_030_MMU dispatch into `main.cc` — is subsumed by §4a.) |
| `d8a2981` | filter `set_cpu_freq.cpp` twin to avoid `.o` collision | **DROP (obsolete)** | Beta 21 deleted `set_cpu_freq.cc`; only `set_cpu_freq.cpp` remains (blob `163f98da…`, identical to ours). See §4d. |
| `8be8949` | fix `-Os` JIT miscompile so UAEJIT modes boot | RE-PORT (rebase) | Target file **renamed**: `uae/jit/compemu_support.cpp` → **`uae/jit/compemu_support_arm.cpp`**. The re-port site is beta 21 `compemu_support_arm.cpp:2177-2178`, still the commented-out `// flush_cpu_icache((void *)popallspace, (void *)target);`. Couples to §4e. |
| `038f9d5` | WIP boot instrumentation snapshot (auto-preserved) | **SPLIT** | The `docker/**` tree (`Dockerfile`, `build.sh`, `run.sh`, `verify.sh`, `install_config.txt`, `README.md`) and `.gitignore` — **RE-PORT (additive)**, upstream has no `docker/`. The instrumentation edits to `a3000_scsi.cpp`/`cpummu030.cpp`/`newcpu.cpp` and the committed `BOOT.BIN`/`.elf`/`.bin` artefacts — **DROP**. |
| `1095c09` | untrack rebuilt firmware build artifacts | RE-PORT (clean) | `.gitignore` intent. Note beta 21 **re-adds** `Z3660_emu/build/mapfile.txt` to the tree — decide whether to re-ignore it on our branch. |
| `9615f89` | gitignore agent config + investigations + bench archive | RE-PORT (clean) | `.gitignore` |

### 3.8 Docs, changelogs, tools, artefacts

| Commit | Subject | Disposition | Target / evidence |
|---|---|---|---|
| `d1518ba` | document AMIX support; correct docker build notes | RE-PORT (rewrite) | `README.md`, `docker/README.md`, `docs/AMIX.md`. Keep inside the `fork:amix` README boundary. |
| `a5b3143` | refresh AMIX docs (stable login shell; SCSI-id) | RE-PORT (rewrite) | `README.md`, `KNOWN_ISSUES.md`, `docs/AMIX.md` — port the final doc state, not the diff |
| `703c0dc` | link the companion amix-kerntools repo | RE-PORT (rewrite) | `README.md`, `docs/AMIX.md` |
| `6fe117f` | CHANGES: 030 PMMU multi-fault BUS ERROR fix | DROP | `CHANGES.md` is upstream's changelog; beta 21 rewrote it. Fold our entries into `CHANGELOG.md` instead. |
| `15119fd` | start CHANGELOG with the CD-ROM units entry | DROP | Describes work now absorbed upstream (§3.5) |
| `12cfe6a` | changelog the a3000_scsi CD-ROM emulation | RE-PORT (rewrite) | `CHANGELOG.md` — the `a3000_scsi` half is still ours |
| `51038f3` | changelog T2.P3 firmware | RE-PORT (rewrite) | `CHANGELOG.md` — rewrite for the post-resync state |
| `fbe6f22` | tools/amix-bench: Dhrystone + I/O benchmark | RE-PORT (additive) | `tools/amix-bench/**` — absent upstream |
| `aac3621` | amix-bench: bundle precompiled AMIX binaries | DROP (superseded) | Superseded by `b0b70d8`'s naming scheme |
| `b0b70d8` | amix-bench: ship binaries as `dhry`/`hz` | RE-PORT (additive) | Final state of `tools/amix-bench/**` |
| `22cf98c` | tools: add amix-stress | RE-PORT (additive) | `tools/amix-stress/**` — absent upstream. **Drop the committed `__pycache__/*.pyc`.** |
| `d40cf11` | ship AMIX firmware BOOT.BIN + document AMIX config | **SPLIT** | `z3660cfg.txt` documentation — **RE-PORT (rewrite)**, and it must be re-keyed from `cdrom_units` to `isoN` (§4b). The committed `BOOT.BIN` — **DROP**. |
| `c7c7556` | refresh the AMIX BOOT.BIN | DROP | Rebuilt binary artefact |

---

## 4. Hot items

### 4a. `main.cc` is gone — port to `main.cpp`

**Confirmed.** `Z3660_emu/src/main.cc` is deleted in beta 21. Our fork carried both: `main.cc` (blob `d96fa89f…`, the file actually built, 829 lines) and a stale `main.cpp` (blob `4ddd26a8…`). Beta 21 has only `main.cpp` (blob `b0f0ae74…`, 959 lines), grown +202/−7 from the old base.

Our entire `main.cc` delta versus the old base is **six hunks**. Each must move into beta 21's `main.cpp`. Line numbers below are in beta 21's `main.cpp`.

| # | Our change | Beta-21 insertion point |
|---|---|---|
| 1 | `extern "C" int z3660_dbg_emu(void)` accessor — lets `newcpu.cpp` / `cpummu030.cpp` read the DEMU toggle without pulling in `SHARED` | Immediately after `init_shared()` closes at **line 80**, before `int read_irq=0;` at line 81. (In our tree it sits after `init_shared()` and before `XGpioPs GpioPs;`; beta 21 moved `XGpioPs GpioPs;` up to line 58, so anchor on the end of `init_shared()`.) |
| 2 | `volatile int a3000_scsi_irq=0;` + `intlev()` OR-ing it in — the software-injected level-2 (Amiga INT2/PORTS) for the emulated WD33C93+SuperDMAC. **Not** the physical FPGA INT6 path. | Replace beta 21 **lines 81-85**:<br>`int read_irq=0;` / `extern "C" int intlev(void) { return(read_irq); }`<br>with the declaration plus the `if(a3000_scsi_irq>level) level=a3000_scsi_irq;` form. |
| 3 | `PISCSI_CMD_PDT = 0xA0` in `enum piscsi_cmds` | Beta 21 **line 157-158**: insert between `PISCSI_CMD_USED_DMA = 0x9C,` and `PISCSI_DBG_MSG = 0x100,`. Matches the value already in `z3660_scsi_enums.h` (absorbed). |
| 4 | In `write_scsi_register()`: the `static uint32_t piscsi_len_stash / piscsi_addr_stash` pair, the `case PISCSI_CMD_ADDR2:` capture and the new `case PISCSI_CMD_ADDR3:` capture, **and** the post-spin `Xil_L1DCacheInvalidateRange(piscsi_addr_stash, piscsi_len_stash)` for the three READ triggers. This is `0a4c064`, the AMP cache-coherence fix. | Declarations at the top of `write_scsi_register()` (beta 21 **line 237-239**). The `ADDR2` capture replaces beta 21 **line 258-259** (`case PISCSI_CMD_ADDR2: break;`); `case PISCSI_CMD_ADDR3:` is **new** — beta 21 defines `PISCSI_CMD_ADDR3 = 0x28` at line 137 but has **no case for it** in the write switch. The invalidate goes after `while(shared->write_scsi==1){NOP;}` at beta 21 **line 288**, before the closing brace at 289. |
| 5 | In `read_scsi_register()`: `case PISCSI_CMD_PDT: break;` (silences the per-command log) plus the load-bearing comment on the `PISCSI_CMD_USED_DMA` whole-L1 flush | Beta 21 `read_scsi_register()` starts at **line 291**; the `USED_DMA` case is at **308**, `READBYTES`+`Xil_L1DCacheInvalidate()` at **331-332**. Add the `PDT` case alongside, ahead of the `default:` that prints `"SCSI read command %02x"`. |
| 6 | `UAE_030_MMU` dispatch: `else if(shared->cfg_emu==UAE_030_MMU) { uae_emulator(0,68030,1,shared->amix_mode); }` in **both** `#ifdef MUSASHI_EMULATOR` arms, plus the loud halt on an unknown mode | Beta 21 arm 1 (live): insert between **line 912** (`UAEJIT_040` block close) and **913** (`else if(...==MUSASHI)`). Arm 2 (dead — see below): insert into the **935/936** region. |

**Defect found in beta 21 `main.cpp` while locating hunk 6.** Lines 936-944 read:

```cpp
         else// if(shared->cfg_emu==UAEJIT_040)
         {
            uae_emulator(1,68040);
         }
         else // decision #8: was a bare 'else' meaning UAEJIT_040 -- an unknown
         {    // mode silently booted JIT-040. Halt loudly instead.
            z3660_printf("[Core1] No emulator selected!!!\nHALT!!!\n");
            while(1);
         }
```

That is an `else` following an `else` — **not valid C++**. The trailing comment is *our* text from `9902fdd`: upstream transcribed our decision-#8 change but kept their original bare `else` instead of converting it to `else if(shared->cfg_emu==UAEJIT_040)`. It compiles today only because `main.cpp:23-24` define **both** `MUSASHI_EMULATOR` and `UAE_EMULATOR`, so this whole `#else` arm is preprocessed out. It is a latent build break for any configuration that turns `MUSASHI_EMULATOR` off. Our re-port should fix line 936 to `else if(shared->cfg_emu==UAEJIT_040)` while inserting hunk 6.

Also note upstream already absorbed the *first* arm's loud halt (beta 21 lines 917-921) — that half of `9902fdd` is done.

### 4b. `isoX` versus `cdrom_units` — adopt upstream

**Adopt upstream's `isoN` wholesale; drop our `cdrom_units` key and its parser. Keep nothing of ours here except the config documentation.**

Evidence. Beta 21 `config_file.h` contains, verbatim, **our own line including our comment**:

```c
   int cd_target[7];   // per-SCSI-target CD-ROM flag: cd_target[id]!=0 => target id is a read-only 2048-byte CD-ROM (pdt 0x05). Parsed from "cdrom_units".
```

So the *struct field* and everything downstream of it (`piscsi_map_drive(..., config.cd_target[i])`, the `is_cd` branch, `pdt=0x05`, the read-only `f_open`) are absorbed unchanged. Only the **parsing front end** differs:

| | Ours | Beta 21 |
|---|---|---|
| Config key | `cdrom_units <list>` — one key naming which SCSI target ids are CDs | `iso0` … `iso19` — one key per image slot, parallel to `hdf0`…`hdf19` |
| CONFITEM | `CONFITEM_CDROM_UNITS` appended at the enum tail | `CONFITEM_ISO0`…`CONFITEM_ISO19` **inserted** between `CONFITEM_HDF19` and `CONFITEM_SCSI0` |
| Storage | sets `config.cd_target[id]` directly | `sprintf(config.hdf[index],"%s%s", DEFAULT_ROOT, cur_cmd); config.cd_target[index]=1;` — an ISO is just an `hdf[]` slot flagged as CD |
| Default init | — | `for(int i=0;i<7;i++) config.cd_target[i]=0;` in `load_default_config()` |

Upstream's model is strictly better for us: the image path and the CD flag travel together in one array, and target assignment stays with the existing `scsiN` keys. A user writes `iso0 iso/amix.iso` then `scsi2 0`.

**Plan:**
1. Take beta 21's `CONFITEM_ISO0..19` block, its `config_item_names[]` entries, its parser case, and its `load_default_config()` init — unmodified.
2. **Drop** our `CONFITEM_CDROM_UNITS` enum member, its `"cdrom_units"` name-table entry, and its parser case.
3. Keep our `int cd_target[7]` — already there; **fix the now-stale comment**, which still says "Parsed from `cdrom_units`" in beta 21 even though nothing parses that key.
4. Rewrite our `z3660cfg.txt` AMIX documentation (`d40cf11`) from `cdrom_units` to `isoN` + `scsiN`.
5. Our `a3000_scsi.cpp` CD emulation (`e3592a7`) reads `config.cd_target[]`, so it is unaffected.

### 4c. SD-fileops — absorbed byte-identical

**Verified DROP.** Blob-hash comparison between `amix-main` and `upstream/main`:

| File | ours | upstream | |
|---|---|---|---|
| `Z3660/src/sd_fileops.c` | `18f9e7ba061741de46c576ebc6c3e4cc3a73a244` | `18f9e7ba061741de46c576ebc6c3e4cc3a73a244` | identical (15 951 bytes both) |
| `Z3660/src/sd_fileops.h` | `61b2515087a0d8ec450fd426e35c668e27c3c658` | `61b2515087a0d8ec450fd426e35c668e27c3c658` | identical |

The boot-menu wiring is absorbed too — beta 21 `mobotest.c` contains the same `else if(keybd_data=='e' || keybd_data=='E') { sd_fileops_menu(); }` block, and the same `#include "sd_fileops.h"`, in the same position (only the surrounding line number differs: ours 3003, beta 21 3014). Beta 21 also declares `void sd_fileops_menu(void);` at `sd_fileops.h:13` exactly as we do.

`f6be42b`, `01b8b18` and `5ef7c19` are therefore all **DROP**. Nothing to carry.

### 4d. The `.cc`/`.cpp` twin filter must go away entirely

**Confirmed, with a correction to the framing:** the filter does not "invert" — it must be **deleted**.

Old base and our fork carried byte-identical twins:

| | base / `amix-main` | beta 21 |
|---|---|---|
| `main.cc` | present (built) | **deleted** |
| `main.cpp` | present (filtered out) | present (blob `b0f0ae74…`) — the only copy |
| `set_cpu_freq.cc` | present (built), blob `163f98da…` | **deleted** |
| `set_cpu_freq.cpp` | present (filtered out), blob `163f98da…` | present, blob `163f98da…` — identical to ours |

Both files are globbed by `CXX_SRCS := $(shell find $(SRCDIR) ... \( -name "*.cc" -o -name "*.cpp" ... \))` and both map to the same `build/<base>.o`, hence our filter. Our `Z3660_emu/Makefile` currently reads:

```make
CXX_SRCS := $(filter-out \
	$(SRCDIR)/uae/fpp_native.cpp \
	$(SRCDIR)/main.cpp \
	$(SRCDIR)/set_cpu_freq.cpp \
	$(SRCDIR)/uae/jit/codegen_arm.cpp \
	...
```

Under beta 21 there is no `.cc` twin for either file, so those two `filter-out` lines would exclude the **only** copy of `main.cpp` and `set_cpu_freq.cpp` — dropping `main()` and `set_cpu_freq()` from the link.

**Required beta-21 Makefile state:** remove the `$(SRCDIR)/main.cpp` and `$(SRCDIR)/set_cpu_freq.cpp` lines from the `filter-out`, and remove the explanatory comment above it. That is exactly upstream's filter list, which keeps only `fpp_native.cpp`, `codegen_arm.cpp`, `compemu_midfunc_arm.cpp`, `compemu_midfunc_arm2.cpp`. Do **not** add an inverted `*.cc` filter — no `.cc` files remain in the tree.

### 4e. The two Makefile build fixes, re-verified

| Our fix | Verdict |
|---|---|
| `a015123` — filter `main.cpp` twin | **OBSOLETE.** Twin deleted upstream. §4d. |
| `d8a2981` — filter `set_cpu_freq.cpp` twin | **OBSOLETE.** Twin deleted upstream. §4d. |

Both are DROP. **But a third, unlabelled Makefile change in `47aeaf8` is the one that actually matters and is still needed.** Diffing our `Z3660_emu/Makefile` against beta 21's:

| Setting | ours (`amix-main`) | beta 21 |
|---|---|---|
| Optimisation | `-Os` | `-O3` |
| Linker GC | `-Wl,--gc-sections` **active** | `#-Wl,--gc-sections` **commented out** |

This is the pair that keeps `BOOT.BIN` under the ~12 MB SD/TFTP write threshold. Beta 21 adds a lot of new code (floppy emulation, a web UI + httpd, wazp3d NEON/SIMD, `compemu_arm.cpp` at 51 496 lines), so the size pressure is higher, not lower. **RE-PORT, and treat the resulting `BOOT.BIN` size as a gate to measure after the first successful build.**

`-Os` is what exposed the JIT miscompile fixed by `8be8949`, so the two travel together: re-port `8be8949` into the renamed `uae/jit/compemu_support_arm.cpp` (site: beta 21 line 2177-2178, the still-commented `// flush_cpu_icache((void *)popallspace, (void *)target);`). Its two halves are the `#pragma GCC optimize("O3")` at the top of the translation unit and the restored `flush_cpu_icache(popallspace, get_target())`. Beta 21 has neither. The I-cache-flush half is arguably correct independent of `-Os` and should go in regardless.

Also still needed (not absorbed): `c2a5c1d`'s `usb/asm/ch9.h` half and `7794e40`'s `vitis_ide/Makefile` + `system_cli.bif`. Absorbed and to be dropped: `8cd8a07` (the `usleep2` shim, verbatim in beta 21).

### 4f. `process_hunk`, `FIL fd[16]`, `config_item_names[]`

**`process_hunk` — signature changed upstream; we do not touch the file, but our `scsi.c` port must land on the new call form.**

Beta 21 `scsi/hunk-reloc.c` added a trailing `int debug` parameter throughout:

```c
int  process_hunk (uint32_t index, HUNK_INFO *info, FIL *f, HUNK_RELOC *hreloc, int debug);
void process_hunks(FIL *in, HUNK_INFO *h_info, HUNK_RELOC *r, uint32_t offset, int debug);
void reloc_hunks  (HUNK_RELOC *r, uint8_t *buf, HUNK_INFO *h_info, int debug);
```

Call sites in `scsi.c` gained a `,0`: `process_hunks(&in, &piscsi_hinfo, piscsi_hreloc, PISCSI_DRIVER_OFFSET, 0)` and `reloc_hunks(..., &piscsi_hinfo, 0)`. **No commit in our fork set touches `hunk-reloc.{c,h}`** (verified: `git log main..amix-main -- .../hunk-reloc.*` is empty), and none of our changes depend on the old arity. **Impact: none, beyond taking beta 21's call sites as-is when re-porting our `scsi.c` hunks.**

**`FIL fd[16]` — grew upstream, harmless to us.** Beta 21 changed `FIL fd[8];` to `FIL fd[16];` in `scsi.c`. Our fork still has `FIL fd[8]` at `scsi.c:679`, and our only use is `FIL *tmp_fd=&fd[index];` at line 692 — unchanged semantics. `NUM_SCSI_UNITS_MAX` is **18 in both** trees (`z3660_scsi_enums.h:3`), so beta 21 is closing a pre-existing gap between the declared maximum and the file-handle array. **Take beta 21's `fd[16]`. No conflict, no assumption of ours broken.**

**`config_item_names[]` — ordering changed materially; our append-only discipline saves us.**

Beta 21 **inserted** 48 entries into the middle of both `enum CONFITEM` and the parallel `config_item_names[]`:

- 20 × `iso0`…`iso19`, inserted between `hdf19` and `scsi0`
- 20 × `adf_file0`…`adf_file19` and 8 × `adf0`…`adf7`, inserted between `scsi6` and `autoconfig_ram`

Every `CONFITEM_*` numeric value from `CONFITEM_SCSI0` onward has therefore shifted. Our three additions — `CONFITEM_AMIX_MODE`, `CONFITEM_SERVICE_CADENCE`, `CONFITEM_CDROM_UNITS` — were appended at the tail immediately before `CONFITEM_NUM`, and everything in the codebase refers to them symbolically, so **no numeric collision occurs**. The merge is mechanical:

- append `CONFITEM_AMIX_MODE` and `CONFITEM_SERVICE_CADENCE` after beta 21's last member (`CONFITEM_ARM_FREQUENCY`), before `CONFITEM_NUM`;
- append the matching `"amix_mode"` / `"service_cadence"` strings at the **end** of beta 21's `config_item_names[]`;
- **drop** `CONFITEM_CDROM_UNITS` and `"cdrom_units"` entirely (§4b).

The one hard invariant: `config_item_names[]` is a designated-index-free parallel array sized `[CONFITEM_NUM]`, so the string order must match the enum order exactly. Verify the count after merging.

Two related enums to check while in the area, both **safe**:

- `enum BOOTMODE` (`Z3660/src/config_file.h` and `Z3660_emu/src/main.h`) is **unchanged in beta 21** — still 7 modes, `BOOTMODE_NUM` at index 7. `UAE_030_MMU` still appends at 7 and our `static_assert(BOOTMODE_NUM == 8, ...)` remains correct.
- `enum zz_reg_offsets` (`rtg/zzregs.h`): beta 21 repurposed `REG_ZZ_USB_PORTSC1 0x1AC` → `REG_ZZ_RESERVED_1AC`, `REG_ZZ_USB_READ0 0x1B8` → `REG_ZZ_USB_RESERVED0`, `REG_ZZ_USB_PARAM0..7 0x2A0-0x2BC` → `REG_ZZ_ADF_SEL_0..7`, and added `REG_ZZ_ADF_SEL_TXT 0x2F8` / `REG_ZZ_APPLY_ADF 0x2FC` / `REG_ZZ_SEL_ADF_TXT 0x800`. **None of these are ethernet or piscsi registers** — see §6.

### 4g. Regression found: beta 21 weakened our direct-DMA cache maintenance

This is the most consequential finding in the resync and it is **not** a simple ABSORBED/RE-PORT call.

Beta 21 contains **both** of our direct-DMA coherency comment blocks from `e37bb43`, transcribed verbatim — including the sentences "Invalidate the range from **L1+L2** BEFORE the transfer" and "Clean the range through **L1+L2** to DRAM first". But the calls underneath them were changed to the **L1-only** variants:

| Site | Our `amix-main` | Beta 21 | |
|---|---|---|---|
| READ handler, `scsi.c` | `Xil_DCacheInvalidateRange((INTPTR)map, piscsi_u32_read[1])` (line 1117) | `Xil_L1DCacheInvalidateRange(...)` (line 1051) | **weakened** |
| WRITE handler, `scsi.c` | `Xil_DCacheFlushRange((INTPTR)map, piscsi_u32_write[1])` (line 1233) | `Xil_L1DCacheFlushRange(...)` (line 1159) | **weakened** |

On Zynq-7000 (Cortex-A9 + PL310) these are distinct APIs — `.../standalone_v9_0/src/arm/cortexa9/xil_cache.c` defines `Xil_DCacheInvalidateRange` / `Xil_DCacheFlushRange` (whole hierarchy, L1 **and** L2) separately from `Xil_L1DCacheInvalidateRange` / `Xil_L1DCacheFlushRange` (L1 only) and `Xil_L2Cache*Range`. Beta 21 itself demonstrates it knows the distinction — `scsi.c:1558-1559` deliberately calls `Xil_L1DCacheFlushRange` **and** `Xil_L2CacheFlushRange` back-to-back for the boot-ROM range.

Why the weakened form is wrong, restated from the original bug:

- **Write path.** `Xil_L1DCacheFlushRange` pushes the guest's dirty lines from L1 into the PL310 L2 — **not to DRAM**. `XSdPs_WritePolled` performs no cache maintenance and reads DRAM below the L2. So the DMA can write **stale DRAM contents** to disk.
- **Read path.** `Xil_L1DCacheInvalidateRange` leaves stale **dirty L2** lines alive over the DMA target. `XSdPs_ReadPolled` only invalidates after the transfer, so an L2 writeback during the multi-millisecond transfer can land **on top of** freshly DMA-ed bytes — the exact failure originally observed as a UFS superblock read returning old guest memory.

**Disposition: RE-PORT the two call sites** (restore `Xil_DCacheInvalidateRange` / `Xil_DCacheFlushRange`); **DROP the comment blocks**, which are already present and correct. Everything else in `e37bb43` — the `main.cc` side — moves via §4a hunk 4.

Worth noting for the maintainer relationship: this is a defect in beta 21 that affects any direct-DMA (non-bounce) piscsi transfer, not only Amix. It is a candidate to report upstream.

**Reported and merged (2026-08-19).** It went upstream as PR #20 from `fix/dma-cache-hierarchy` — commit `91ce038`, merged as `fcdb9d2`. Upstream's tree now carries the whole-hierarchy calls; this branch has not yet taken that merge back (see *Where the upstream base stands*, above).

---

## 5. Cross-core `SHARED` struct — silent ABI collision

Not on the original hot list; found while checking `main.h`. **This is the highest-risk merge in the whole resync** because it fails silently rather than at build time.

The `SHARED` struct is the core0↔core1 mailbox. It is **duplicated** in `Z3660/src/main.h` (control core) and `Z3660_emu/src/main.h` (emulator core) and the two copies must stay byte-identical — the fields are annotated with absolute addresses. Both we and upstream appended to the same tail, at **the same offsets**:

| Offset | Ours (`amix-main`) | Beta 21 |
|---|---|---|
| `0xFFFF009C` | `debug_emu` | `write_floppy` |
| `0xFFFF00A0` | `amix_mode` | `write_floppy_addr` |
| `0xFFFF00A4` | `service_cadence` | `write_floppy_data` |
| `0xFFFF00A8` | `perf_report` | `write_floppy_type` |
| `0xFFFF00AC` | — | `read_floppy` |
| `0xFFFF00B0` | — | `read_floppy_addr` |
| `0xFFFF00B4` | — | `read_floppy_data` |
| `0xFFFF00B8` | — | `read_floppy_type` |

A merge that lands our four fields anywhere other than **after** all eight of upstream's will mis-alias the mailbox — core0 writing `write_floppy` into the slot core1 reads as `debug_emu`, and so on. There is no compiler diagnostic for this; the symptom would be floppy commands corrupting the debug/perf state and vice versa.

**Required:** append our four fields **after** `read_floppy_type`, i.e. at `0xFFFF00BC` / `0xFFFF00C0` / `0xFFFF00C4` / `0xFFFF00C8`, and make the identical edit in **both** `Z3660/src/main.h` and `Z3660_emu/src/main.h`, updating the address comments. Then update `Z3660_emu/test/host/board_stubs.cpp` (`26072ee`), which stubs `shared`, to match the new layout.

Related, same file: beta 21 `Z3660/src/main.h` also gained `void xc3_usleep(unsigned int usec);` and `void set_cpu_freq_for_usleep(int cpufreq);` declarations, and `cpu_emulator.c` gained floppy dispatch branches in `emulator_thread()`. Our `cpu_emulator.c` changes (`53b6869`, `a1cb13a`, `1ff37ad`) are all in `emulator_reset_thread()` — **non-overlapping**, clean apply.

---

## 6. Ethernet / lwip contract delta — **none**

The `z3660eth` guest driver binds to manufacturer `0x144B` / product `0x01`, and depends on a 32-bit big-endian register file at `board+0x104…0x1A8` plus fixed DDR frame windows. Reading the base→beta-21 delta against that contract:

| Contract surface | Beta-21 delta | Verdict |
|---|---|---|
| `rtg/zzregs.h` — `REG_ZZ_CONFIG 0x104`, `REG_ZZ_ETH_TX 0x190`, `REG_ZZ_ETH_RX 0x194`, `REG_ZZ_ETH_MAC_HI 0x198`, `REG_ZZ_ETH_MAC_LO 0x19C`, `REG_ZZ_FW_VERSION 0x1A0`, `REG_ZZ_ETH_RX_ADDRESS 0x1A4`, `REG_ZZ_INT_STATUS 0x1A8` | **all unchanged**. The only edits in this header are USB→ADF repurposing at `0x1AC`, `0x1B8`, `0x2A0-0x2BC`, plus new `0x2F8`/`0x2FC`/`0x800`. | **no delta** |
| `rtg/rtg.c` — the `REG_ZZ_CONFIG` / `ETH_TX` / `ETH_RX` / `ETH_MAC_HI` / `ETH_MAC_LO` / `ETH_RX_ADDRESS` / `INT_STATUS` handler bodies | Extracted and diffed both trees: **byte-identical**. Only line numbers moved (e.g. `REG_ZZ_CONFIG` 1149→1078) from unrelated USB-code deletion. Filtering the whole `rtg.c` diff to changed lines mentioning eth/interrupt/config yields **two lines, both ADF-related**. | **no delta** |
| `memorymap.h` — `RX_BACKLOG_ADDRESS 0x07ED0000`, `TX_FRAME_ADDRESS 0x07EE0000`, `RX_FRAME_ADDRESS 0x07EF0000`, `RX_FRAME_PAD 4` | file **unchanged** | **no delta** |
| `ethernet.h` — the producer/consumer API (`ethernet_send_frame`, `ethernet_receive_frame`, `ethernet_get_backlog`, `ethernet_current_receive_ptr`, `ethernet_update_mac_address`) | file **unchanged** | **no delta** |
| `ethernet.c` — RX producer, backlog fill, `frame_serial` stamping, TX result codes | +7/−8, and **all of it cosmetic**: one `#include "sleep.h"` removal and six `printf` lines commented out or de-unicoded. `FRAME_MAX_BACKLOG` is still **64**; the slot header is still big-endian `[len16][serial16]` at offset 0 with payload at +4; `frame_serial` handling and the catch-up serial fake-up are untouched. | **no delta** |
| `interrupt_enabled_ethernet` gating of ring fill | unchanged — the backlog is still filled independently of the interrupt-enable flag (`rtg.c:319`/`455` gate only the *interrupt raise*, not the fill) | **no delta** — the driver's single most fragile assumption still holds |
| `lwip.c` | +13/−7, entirely host-side: `usleep`→`xc3_usleep`, the 250 ms timer load value now derived from the runtime `_cpufreq` instead of the compile-time `XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ`, and the acquired IP printed to HDMI. Nothing guest-visible. | **no delta** |
| `lwip.h`, `lwip/tftp_server.c` | unchanged / 1-line cosmetic | **no delta** |

**Conclusion: beta 21 introduces zero changes to the emulated-ethernet register or frame-transfer contract.** The `z3660eth` driver needs no change for this resync, and no firmware-side ethernet work is implied. Note also that **our fork never touched `ethernet.c` or `lwip.c`** (`git log main..amix-main -- .../ethernet.c .../lwip/*` is empty), so there is nothing of ours to re-port here either.

One forward-looking note, unchanged by beta 21 but worth recording: `FRAME_MAX_BACKLOG` is 64 while the ring between `RX_BACKLOG_ADDRESS` and `TX_FRAME_ADDRESS` holds only 32 slots of 2048 bytes. The guest driver defensively maps the full 64-slot range and bounds-checks `REG_ZZ_ETH_RX_ADDRESS` against it, so the mismatch is currently harmless. If a future upstream release *fixes* `FRAME_MAX_BACKLOG` to 32 the driver stays correct; if it *raises* the ring size or moves `RX_BACKLOG_ADDRESS`, the driver's bounds check silently kills RX. Worth a re-check at each upstream resync.

---

## 7. Mailbox-protocol pin

### 7.1 Firmware side: our fork and beta 21 agree exactly

`Z3660/src/scsi/z3660_scsi_enums.h` is **byte-identical between `amix-main` and `upstream/main`**. Diffing the old base against beta 21 shows upstream's only change to that header is the addition of **our own** register, with our comment:

```c
   // Read-only device-type register. SINGLE SOURCE OF TRUTH for the peripheral
   // device type: 0x00 = direct-access disk, 0x05 = read-only CD-ROM.
   PISCSI_CMD_PDT          = 0xA0,
```

So the piscsi mailbox contract has **not drifted at all** across this resync. `NUM_SCSI_UNITS_MAX` is 18 in both. The per-unit register arrays are 8 entries each in both: `PISCSI_CMD_BLOCKSIZE0..7` at `0x200`–`0x21C` and `PISCSI_CMD_BLOCKS0..7` at `0x220`–`0x23C`.

### 7.2 amiberry's Z3660 native-device emulation

The bench emulator (amiberry, workspace fork of BlitterStudio/amiberry) models the Z3660 board so an Amix guest can run without hardware. Its emulation lives on a dedicated branch in `src/z3660_scsi.cpp` (+ `src/include/z3660_scsi.h`, `expansion.cpp`, `rommgr.h`), and its header states the register map was transcribed from **this repository's** `Z3660/src/scsi/scsi.c` + `z3660_scsi_enums.h`.

**Version pin: there is no version pin.** The emulation carries no `PISCSI_DRIVER_VERSION`, no version register read, no magic/ident value, and no comment citing a firmware version, beta number, or date. The only version artefact anywhere in that tree is the boot ROM it serves at `board+0x6000` (an untracked test-data file whose strings read `Z3660 SCSI v1.4` / `Z3660SCSI 1.4 (16 Oct 2024)`) — and the emulation derives no behaviour from it. So the practical pin is *the reference sources as of the branch's commit date (2026-07-11)*, i.e. **this fork's `amix-main` at the T2.P3 firmware state**, which §7.1 has just shown is register-identical to beta 21.

**Drift versus beta 21: none in the modelled subset.** Offset-by-offset the emulation matches beta 21's enums — `0x00` WRITE, `0x04` READ, `0x08` DRVNUM, `0x0C` DRVTYPE, `0x10` BLOCKS, `0x14/0x18/0x1C` CYLS/HEADS/SECS, `0x20-0x2C` READ_ADDR1-4, `0x30` DEBUGME, `0x60` WRITE64, `0x64` READ64, `0x88` READBYTES, `0x8C` WRITEBYTES, `0x90` DRVNUMX, `0x9C` USED_DMA, **`0xA0` PDT**, `0x200+u*4` BLOCKSIZE0, `0x220+u*4` BLOCKS0, `0x240-0x24C` WRITE_ADDR1-4, `0x290` EXPBASE. It models `PDT_DISK 0x00` / `PDT_CDROM 0x05` and CD units at 2048-byte blocks, read-only — matching the absorbed firmware behaviour in §3.5.

**Gaps to record (they are scope choices, not drift):**

1. **Boot/driver-relocation half unimplemented.** `PISCSI_CMD_DRIVER (0x50)`, `GETPART`, `GETFS`, `COPYFS`, `SETFSH` fall into a `default: break;` and are deliberately ignored; AutoConfig `DIAGVALID` is not set, so the guest never runs the firmware boot protocol. Beta 21's changes in exactly this area — the `process_hunks`/`reloc_hunks` `int debug` parameter (§4f) — are therefore **invisible to amiberry**. No emulator work is implied.
2. **Unit count.** amiberry models `Z3660_NUM_UNITS = 8`; firmware declares `NUM_SCSI_UNITS_MAX = 18`. This is **not** a conflict: the per-unit register arrays are only 8 deep in the firmware enums too (`0x200`–`0x23C`), ending exactly where `PISCSI_WRITE_ADDR1 = 0x240` begins. Beta 21 raising `FIL fd[8]` → `fd[16]` (§4f) is a host-side file-handle array, not a mailbox-visible change, so it does **not** widen those arrays and does **not** desynchronise amiberry.
3. **Bounce contract.** amiberry implements the `BOUNCE_THRESH = 0x08000000` rule (below → via `board+0x80000`, `used_dma` = guest addr; at/above → direct), matching the firmware contract documented in our `scsi.c` comments. Bounced transfers over 64 KB are clamped rather than split — an emulator-side limitation to keep in mind, unrelated to beta 21.
4. **Ethernet not modelled** ("phase 2"). Consistent with §6: nothing to sync.
5. **Register-array read side effect.** Reading `BLOCKSIZE0+u*4` or `BLOCKS0+u*4` sets the emulator's current drive — worth re-confirming against firmware behaviour if that path is ever exercised, but unchanged by beta 21 either way.

**Net: the resync requires no amiberry change.** Re-verify after the re-port lands, since §4g restores cache semantics that amiberry does not model at all (it has no cache hierarchy), and §4a moves the AMP invalidate into `main.cpp` — both are real-hardware-only concerns.

---

## 8. Re-port sequencing and residual risk

Suggested order, cheapest-verification-first, cutting a fresh branch from `upstream/main`:

1. **Build plumbing** — `docker/**`, `vitis_ide/Makefile`, `system_cli.bif`, `ch9.h`, `.gitignore`. Goal: beta 21 builds unmodified in our container.
2. **Makefile `-Os` + `--gc-sections`** (§4e) and the JIT fix into `compemu_support_arm.cpp`. Goal: measure `BOOT.BIN` size against the ~12 MB threshold **before** adding the MMU engine.
3. **`SHARED` struct + `BOOTMODE` + `CONFITEM`** (§5, §4f) — the three enum/ABI merges, in both `main.h` copies. Pure plumbing, no behaviour.
4. **MMU engine** (§3.2) — the unchanged-upstream files land whole; hand-merge `newcpu.cpp` / `newcpu_common.cpp` / `include/newcpu.h`. Validate with the host harness (§3.1) before touching hardware.
5. **`main.cc` → `main.cpp`** (§4a), all six hunks plus the double-`else` fix.
6. **A3000 SCSI emulation** (§3.4) — additive files.
7. **`scsi.c`** (§3.5) — the RDB scan, the `amix_mode` direct-DMA gate, and the §4g cache-call restoration.
8. **`amix_mode` / debug / perf console** (§3.3) — merge alongside beta 21's `DENET`.
9. **Adopt `isoN`** (§4b), drop `cdrom_units`, rewrite the config docs.
10. **Docs, tools, changelog** (§3.8).

**Residual risks, in order of severity:**

- **`SHARED` offset mis-merge** (§5) — silent, no build error. Verify by diffing the two `main.h` copies against each other after the merge.
- **`BOOT.BIN` size** — beta 21 is materially larger before we add anything. If `-Os` + `--gc-sections` no longer clears the threshold, that becomes a blocking design question, not a porting detail. Measure at step 2.
- **`config_item_names[]` / `CONFITEM` desync** (§4f) — a parallel array with no compile-time check. Count entries after merging.
- **`uae_emulator.cpp` (+71/−18) and `cpu_emulator.cpp` (+111/−19)** are the two files where our hunks land in genuinely moved code. Everything else in the engine is near-untouched.
- **Upstream absorbed our work with modifications** (§4g, and the broken `else` in §4a). Do not assume "our comment is present" implies "our change is present" — check the code under every absorbed comment.
