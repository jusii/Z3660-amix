# 68040-MMU boot mode — GO (metal-proven), now runnable (MMU-off)

> **Status: GO — verified on real hardware 2026-08-09.** The 12.156 MiB oversized
> `BOOT.BIN` (the 68040-MMU table linked in) loaded and booted on metal
> ("[SD Init] Done (Z3660.BIN)", not FAILSAFE), so the old ~12 MiB "cap" is a
> dead TFTP-era rule of thumb, not a boot-load wall (real ceiling is >12.156 MiB,
> exact unknown). `UAE_040_MMU` is now **runnable**: the run loop, the
> `set_x_funcs` 040 accessor arm and the `main.cpp` dispatch are wired, the six
> compile bridges are faithful, and a host smoke executes the 68040 interpreter.
> **MMU-ON address translation is validated on the host harness (126/126):**
> the real 040 table walk (4K/8K), TTR matching, protection and U/M-bit updates
> all check against the 68040 spec (§6). **The 68040 access-error (format `$7`)
> frame is implemented and harness-proven, contents included** — the old parked
> 3.4 task, and the first thing a demand-paging 040 kernel needs (§8). The AMIX
> contract now travels onto the 040 core with the mode (§8). Remaining unmodelled
> 040 semantics are listed in §7. A metal MMU-off AmigaOS smoke is still queued,
> and no AMIX-040 boot has been attempted on metal yet.

This tracks a 68040-MMU interpreter as a sibling to the existing `UAE_030_MMU`
mode (the real 68030 PMMU interpreter — "Emulation mode 4", see
`UAE_030_MMU_plan.md` and `docs/AMIX.md`).

## The ~12 MiB line — settled

`BOOT.BIN` deployment was historically kept under ~12 MiB (12 × 1024 × 1024 =
12 582 912 B). That figure originated as a **TFTP-write rule of thumb** (above it
the TFTP path failed silently and fell back to 040-JIT) and was never a proven
boot-loader limit. A 12 745 900 B (12.156 MiB) image — 159 KiB over the line —
was flashed and **booted on real hardware 2026-08-09**: boot log
"[SD Init] Done (Z3660.BIN)", BETA 21, Emulation mode 4, `UAE_030_MMU_AMIX`,
emulator started. So the line is retired as a boot constraint; the practical
ceiling is higher (exact value untested). The board keeps a `FAILSAFE.bin`, and
the production image (`59FAE8CA`) was restored after the test.

## 1. How the CPU/MMU opcode tables are generated and built in

The emulator core (`src/uae/`) is Amiberry v5.6.0's UAE CPU core. The instruction
handlers are not hand-written; they are emitted by WinUAE/Amiberry's `gencpu`
generator, one C++ file per (CPU model, execution style) combination.

**Generator toolchain** — `test/gencpu/`: `build_gencpu.sh <winuae-src-dir> [out]`
builds a host `gencpu` from an upstream `gencpu.cpp` against **this tree's**
`cpudefs.cpp`/`readcpu.cpp`/headers, so generated tables match this tree's opcode
set; `baseline_diff.sh` is the non-MMU correctness gate.

**Generator used for the MMU tables:** WinUAE **4.4.0** `gencpu` (tag `4400`,
commit `c7b24b37`). Its `generate_cpu()` assigns the MMU file ids: **id 31 =
68040 MMU**, id 32 = 68030 MMU, id 33 = 68060 MMU. So the 68040 table is
`cpuemu_31.cpp` with dispatch table `op_smalltbl_31_ff` — the id belongs to the
generator (the 68030 table is `cpuemu_32.cpp`, *not* the `cpuemu_31` the original
plan guessed). A newer (6.1.0) `gencpu` diverges from this tree's headers (2-arg
`x_get_disp_ea_020`, `real_opcode` intermediates); 4.4.0 is the matching lineage.

**Standalone dispatch table.** As with `cpustbl_mmu030.cpp`, the generator's
`cpustbl.cpp` `CPUEMU_31` block is lifted into a standalone `cpustbl_mmu040.cpp`
and converted from the generator's 6-field `struct cputbl` (`{func, NULL, …}`) to
this tree's 5-field one (`{func, opcode, len, {d,d}, br}`), with per-handler
externs added, so the stock `cpustbl.cpp`/`cputbl.h` stay untouched.

**Compile bridges (now faithful).** The 68040 table is the first MMU table this
tree has built, and a few WinUAE-040 codegen helpers are spelled differently in
this tree's (Amiberry 5.6.0) API. `cpuemu_31.cpp` carries a small local `#define`
block mapping them to the **faithful** 68040 behavior on this non-cycle-exact,
chipset-less interpreter:

| generated call | mapped to | faithfulness note |
|---|---|---|
| `x_get_disp_ea_020(base, idx)` | `x_get_disp_ea_020(base)` | the 2nd arg is vestigial (always 0) in this lineage — exact |
| `m68k_setstopped(level)` | `m68k_setstopped()` | tree's arg-less version does the full stop (sets `stopped` + `SPCFLAG_STOP`, DOTRACE-aware) — exact |
| `do_cycles_stop(c)` | `((void)(c))` | non-cycle-exact + no chipset: STOP is serviced by `do_specialties`' poll loop, nothing to pump — correct here (a CE/chipset build would need real cycle pumping) |
| `MakeFromSR_STOP()` | `MakeFromSR()` | full SR rebuild; the same call the proven 030 STOP path uses (WinUAE's `_STOP` variant only defers the trace/int recheck, immaterial to this non-CE loop) |
| `checkint()` | `doint()` | `doint()` raises `SPCFLAG_INT` for a pending interrupt — exactly what wakes a STOP; = `checkint`'s purpose |
| `op_unimpl(op)` | `Exception(61)` | 68060 "unimplemented integer" vector (WinUAE uses `ExceptionL(61,pc)`; this tree has no `ExceptionL`, so `Exception(61)` with the current pc). **Unreachable on the 68040 path** — `m68k_mull`/`m68k_divl` always succeed for `cpu_model>=68040` (only the 68060 traps these), so this is correct-but-dead here |

**Build-in path** (no Makefile edit): any `*.cpp` under `src/` is auto-globbed,
compiled `-Os -ffunction-sections -fdata-sections`, linked with `--gc-sections`.
`newcpu.cpp` puts `op_smalltbl_31_ff` in the dispatch matrix `cputbls[4][4]` and
`build_cpufunctbl()` selects that column when `currprefs.mmu_model == 68040`.

## 2. Sizes (measured)

Clean container build (`./docker/run.sh make build` → `make bootbin`), both cores
green, 040 table linked in.

| quantity | bytes | note |
|---|---:|---|
| Old "cap" line (12 MiB) | 12 582 912 | retired — not a boot limit (see above) |
| Baseline `BOOT.BIN` (no 040 table) | 12 428 476 | 11.853 MiB |
| Metal-proven image (table linked, pre-run-loop) | 12 745 900 | 12.156 MiB, zlib crc32 `0x95E2020C` — the image booted on metal |
| Runnable image (P3.2 wiring) | 12 745 956 | 12.156 MiB, zlib crc32 `0x92E1473F` (+56 B: run loop + dispatch) |
| AMIX-contract image (§8 wiring) | 12 762 340 | 12.171 MiB, zlib crc32 `0x76FBA34C` — same byte size as the flashed `455A7D12` production image |
| **AMIX-040 fault-path image (§8.1 + §8.2)** | **12 762 340** | **12.171 MiB, zlib crc32 `0x2CFA9216`** — format-$7 frame + the 040 `amix_mmu_on` latch |

040-table linked contribution (from `Z3660_emu/build/mapfile.txt`): `cpuemu_31.o`
268 364 + `cpustbl_mmu040.o` 22 488 = 290 852 B. (For reference the 030 table is
larger: `cpuemu_32.o` 468 848 + `cpustbl_mmu030.o` 22 116 = 490 964 B — the 68040
handlers generate more compactly.)

Current image on disk for flashing (rebuild target, gitignored):
`z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/BOOT.BIN`
(12 762 340 B, zlib crc32 `0x2CFA9216` — compare against the board's SD `E`→CRC).

## 3. What is wired (P3.2), and what is not

Wired and validatable now (MMU-off / transparent):
- `cpuemu_31.cpp` (68040-MMU handlers) + `cpustbl_mmu040.cpp` (`op_smalltbl_31_ff`),
  compile-clean, linked, retained via `cputbls[4][4]`.
- **Run loop** `m68k_run_mmu040()` — modelled on WinUAE 4.4.0's: full instruction
  restart (`mmu_restart` + flag snapshot + `m68k_setpci`), opcode fetch via
  `x_prefetch = get_iword_mmu040`; selected in `m68k_go`'s `run_func` when
  `mmu_model == 68040`. Far simpler than `m68k_run_mmu030` (the 040 does not use
  the 030's sub-access replay).
- **`set_x_funcs()` 040 arm** — points `x_get/put_*`, `x_prefetch`, `x_next_*` at
  the `*_mmu040` accessors (distinct from the 030 pointer bank; the 68040 has its
  own TTR + table-walk/descriptor format).
- **Config/dispatch** — `main.cpp` (both blocks) maps `UAE_040_MMU` →
  `uae_emulator(0, 68040, 1, shared->amix_mode)` (JIT off, `mmu_model = 68040`;
  the AMIX contract now travels with it — see §8). Append-only `UAE_040_MMU` in
  both `BOOTMODE` enums (`config_file.h`/`.c`, `main.h` `static_assert` 9); no
  existing index moved.
- Host smoke `test_mmu040_interp_exec` (test/host, **69/69**): with the MMU off
  (`regs.mmu_enabled == 0`, transparent accessors) `build_cpufunctbl` selects
  mode 4 / `op_smalltbl_31_ff` and the interpreter executes `moveq`/`add.l` plus a
  `move.l Dn,(An)` store through `put_long_mmu040`.

NOT done (out of this step's scope):
- **Real 040 MMU translation** (TC-enabled table walk / TTR / descriptor format)
  is untested — needs the 3.3 TTR/descriptor vector harness, and ultimately a
  68040 OS that enables the MMU. (Superseded in part: §6 validated translation on
  the host harness, and a native 68040 AMIX kernel now exists — see §8.)
- **Faithful 68040 bus-error stack frame** (format $7) in `Exception()` on a real
  040 page fault — the still-parked 3.4 task. The run loop provides the correct
  restart/dispatch structure around it; MMU-off never faults, so this is not hit.
- The Amiga-side ZTop and on-board ARM ZTop UI lists still show the old modes
  (their `NUM_BOOT_MODE_LABELS` is separate) — a later UI-threading item.

## 4. Metal MMU-off smoke recipe (orchestrator-driven)

Proves the 68040 interpreter runs real code on hardware, MMU disabled (no AMIX
needed). FAILSAFE-protected; restore production afterwards.

1. **Flash** the current runnable image:
   `z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/BOOT.BIN`
   (12 745 956 B, crc32 `0x92E1473F`) via the usual deploy — serial `C` → `P`
   (TFTP) → `curl -T BOOT.BIN tftp://a4000.alanara.fi:69/Z3660.bin` → `R`.
2. **Select the mode**: set `bootmode UAE_040_MMU` in `z3660cfg.txt` on the SD
   DATA partition (or via the config path you use). CPU is 68040, JIT off.
3. **Boot a plain AmigaOS** (Kickstart + an AmigaOS/Workbench disk) — **not AMIX**.
   Leave `amix_mode` unset, which is this mode's default (§8): the AMIX memory/SCSI
   contract stays off and AmigaOS does not enable the 040 MMU, so the `*_mmu040`
   accessors run transparent; the point is only that the interpreter executes.
4. **Expected serial**: `[SD Init] Done (Z3660.BIN)`, `Emulation mode 4` with
   CPU 68040 (not `Emulation mode 1` / JIT fallback, not `[SD Init] FAIL`), and
   AmigaOS reaching Workbench/CLI.
   - Boots to AmigaOS → the 68040 MMU-off interpreter runs on metal: GO for 3.3.
   - Hangs / no execution → capture the serial; the fault is in the 040 run
     loop / accessors, not the size.

## 5. Table-size strategies (optional, future)

The size is no longer a boot blocker, but a single shared table would still be
tidier if the image grows a lot later: (1) share one MMU table across 030/040 via
the `set_x_funcs` accessor indirection; (2) load the 040 table on demand as an SD
overlay; (3) drop the mode (AMIX runs on `UAE_030_MMU`; `UAEJIT_040` covers
non-MMU 040); (4) reclaim space by auditing which boot-mode tables are reachable.

## 6. 68040 MMU translation — validated on the host harness (P3.3)

`test/host/harness_main.cpp` drives the real 040 table walk in `cpummu.cpp`
(`mmu_translate` / `mmu_fill_atc`) with hand-built root→pointer→page descriptor
tables in the flat buffer, plus `mmu_match_ttr` for transparent windows. Total
harness **126/126** (64 pre-existing 030 + 5 P3.2 040 interp + 21 P3.3 040
translation + 6 §8 040 FPU-trap + 24 §8.1 040 access-error frame + 6 §8.2 AMIX
040 latch). Coverage, in 68040 terms, of the cases the 030 harness covered:

| behavior | test | asserted |
|---|---|---|
| TTR transparent match / no-match / S-field | `test_mmu040_ttr` | `mmu_match_ttr` = OK / NO_MATCH for in/out-of-window, super+user |
| TTR write-protect | `test_mmu040_ttr` | WP window → `TTR_NO_WRITE` |
| TTR end-to-end bypass | `test_mmu040_ttr` | in-window `get_long_mmu040` returns physical value, no table, no fault |
| 3-level walk, 4K (single-translation smoke) | `test_mmu040_translate_4k` | VA 0x00408678 → PA 0x0A000678 |
| 3-level walk, 8K | `test_mmu040_translate_8k` | VA 0x00408678 → PA 0x0A002678 (13-bit offset) |
| ATC hit | `test_mmu040_translate_4k` | 2nd translate of the page base → frame base, no fault |
| U (used) bit update | `test_mmu040_translate_4k` | page descriptor U set after read |
| miss / invalid page | `test_mmu040_protection` | non-resident descriptor → bus fault (THROW) |
| write-protect (page) | `test_mmu040_protection` | read OK, write → fault |
| supervisor-only page | `test_mmu040_protection` | super read OK, user read → fault |
| M (modified) bit update | `test_mmu040_modified_bit` | U+M set in descriptor after write |

The harness bottoms descriptor and physical accesses out at the same flat buffer
the 030 vectors use (`desc_get/put_long` → `x_phys_*`), so these exercise the
genuine engine, not stubs. Reproduce: `make -C test/host && ./test/host/build/mmu_harness`.

## 7. 68040 MMU behaviors NOT modelled / not yet validated (gap list)

Surfaced now so they are a written list, not a surprise when a real 68040 kernel
(or a 68040 AMIX port) lands. In rough order of how likely a booting 040 OS is to
hit them:

1. ~~**Faithful 68040 bus-error stack frame (format $7)**~~ — **DONE 2026-08-20,
   see §8.1.** `Exception_normal()` now routes an `mmu_model == 68040` access
   error to a real format-$7 frame and `Exception_build_stack_frame()` fills it
   from the fault record (SSW, FA, EA, and the WB2/WB3 write-back slots) instead
   of zeros. Harness-covered end to end by `test_mmu040_access_error_frame`, which
   reads the frame the guest OS would read.
2. **Cache model / copyback + cache-inhibit coherency** — the walk decodes the
   descriptor/TTR CM bits into `mmu_cache_state`, but this bare-metal emulator has
   **no data/instruction cache model** (the Z3660 uses the real bus / a direct
   pointer). CINV/CPUSH/`MOVE16` and cache-inhibited-DMA coherency are therefore
   not emulated; an 040 OS relying on copyback semantics or cache-push for DMA
   would not see faithful behavior. (On the 030 path this class of bug is what the
   real hardware AMP-cache fix addressed; the 040 path has no equivalent model.)
3. **MMU-management instructions PFLUSH/PTEST/PLPA** — `mmu_op_real()` implements
   them and the `cpuemu_31` handlers dispatch them, but the harness does **not**
   exercise them (it tests translation, not the ops). PTEST-sets-MMUSR and the
   PFLUSH variants (by-ASID/global/single-page) are unverified here.
4. **Indirect page descriptors** — the walk handles one level of indirection and
   flags double-indirect as illegal, but no vector exercises the indirect path.
5. **Misaligned / page-crossing accesses** — `mmu_get_word_unaligned` etc. handle
   an access that straddles a page boundary (second-page fault); the vectors use
   aligned longwords only, so the cross-page fault path is unverified for 040.
6. **ATC set-associativity / replacement** — modelled (`mmu_atc_array[data]
   [index][way]`, `ATC_WAYS`) and one hit is checked, but eviction/replacement
   and the G (global) bit's flush exemption are not stress-tested.
7. **68040 vs 68060 divergence** — shared code paths are `cpu_model`-guarded and
   only 68040 is tested; 68060 (`cpuemu_33`, not built) is out of scope.

None of 1–7 block the queued metal MMU-off AmigaOS smoke (that path enables no
translation). They become live the moment a 68040 OS turns the MMU on; #1 and #2
are the ones most likely to bite first.

## 8. AMIX on `UAE_040_MMU` — what is wired, and what still blocks it

A native 68040 AMIX boot chain now exists (boot1 → boot2-040 → `unix-040`, UFS
miniroot), so this mode's original "amix_mode off: no 68040 AMIX kernel yet"
assumption is retired. Two emulator-side changes carry the AMIX contract onto the
040 core.

**1. `main.cpp` passes `shared->amix_mode` into the `UAE_040_MMU` branch** (both
the `MUSASHI_EMULATOR` and the plain dispatch block), exactly as `UAE_030_MMU`
does. `uae_emulator()` then applies the whole existing AMIX contract unchanged
and model-independently: the single ≤16 MB Fast-RAM window at `$08000000`, the
`a3000mem` window at `$07000000`, the emulated A3000 SCSI at `$00DD0000`, the
`a3000_scsi_init()` reset, and the `_AMIX` suffix in the startup banner
(`UAE_040_MMU_AMIX`).

The **non-MMU 040 modes deliberately do not take it**, mirroring the 030 side
exactly: `UAE_040` has no MMU and AMIX is demand-paged; `UAEJIT_040` is JIT,
which is mutually exclusive with the MMU (the JIT inlines direct pointers and
cannot restart on a fault). `UAE_030`/`UAEJIT_030` are excluded for the same two
reasons, so there is no case in which AMIX would want either.

**2. `fpu_no_unimplemented` is set for the 68040 iff `amix_mode`**
(`uae_emulator.cpp`). The pref name reads backwards: *set* means "this FPU does
not implement the whole opcode set inside the emulator", so `fpp.cpp` raises the
genuine silicon traps instead of computing the result itself
(`fault_if_unimplemented_680x0`, `fault_if_no_packed_support`). The AMIX 040
kernel links Motorola's FPSP and only executes it if the unimplemented-instruction
trap (vector 11, stack frame format `$2`) and the unimplemented-data-type trap
actually arrive; left clear, the emulator quietly computes `FSIN`/`FINTRZ`/
packed-decimal and FPSP never runs. 68881/68882 keep their existing `true`; a
plain AmigaOS 040 (`amix_mode` off) keeps its existing `false`. Host vector
`test_fpu040_unimplemented_trap` pins both directions.

**Operator requirement.** `amix_mode` is NOT implied by `bootmode UAE_040_MMU`.
`config_file.c` resolves the unset default (`-1`) as *AMIX iff `UAE_030_MMU`*, so
an `UAE_040_MMU` config with no explicit line still runs `amix_mode` off — which
is exactly what §4's plain-AmigaOS smoke recipe wants. To boot AMIX on this mode
the config/preset must carry **`amix_mode YES`** explicitly. Left that way
deliberately: widening the default would silently change §4's recipe and force
`cpu_ram=0` for every existing 040-MMU config.

### The two AMIX-040 blockers — FIXED 2026-08-20

Both were emulator-side, found while wiring the section above, and both are now
closed. Neither was reachable from an MMU-off AmigaOS run; both went live the
moment the 040 AMIX kernel enabled its MMU or touched the disk.

#### 1. Access-error frame — the 030 branch was hijacking the 040 fault

`Exception_normal()` (`newcpu.cpp`) tested `if (currprefs.mmu_model && nr == 2)`,
which is **truthy for `mmu_model == 68040`**, and built a 68030 frame `$A`/`$B`
out of `mmu030_state[]` and the 030-shaped `regs.mmu_ssw` — state the 040 table
walk never maintains. An 040 access error therefore never reached the
`cpu_model >= 68040` arm below it. The test is now `currprefs.mmu_model == 68030`,
and the 040 arm builds the genuine **format `$7`** frame:

```
Exception_build_stack_frame(regs.mmu_effective_addr, currpc, regs.mmu_ssw, nr, 0x07);
```

only when `mmu_model == 68040`. The MMU-off 040 modes (`UAE_040`, `UAEJIT_040`)
keep their old synthesized frame byte for byte — they have no 040 fault record to
stack. (`mmu_model == 68060` now also lands on that synthesized arm instead of the
030 one; `cpuemu_33` is not built and no `BOOTMODE` selects it, so this is inert,
but a future 060 lane needs its own arm.)

**The contents were the real defect.** The `case 0x7:` builder in
`newcpu_common.cpp` already had the right field *order*, but stacked **zeros** for
every write-back slot. That matters because the 68040 does not re-run a faulted
write on `rte`: the store is handed to the OS in WB3 and the kernel completes it.
Zeroed slots = the store silently dropped. The builder now stacks the real fault
record `mmu_bus_error()` had already computed:

| offset | field | value | real or synthesized |
|---|---|---|---|
| `$08` | EA | `regs.mmu_effective_addr` | **real** (now initialised to the fault address at the top of `mmu_bus_error`'s 040 arm; the MOVE16/MOVEM arms refine it) |
| `$0C` | SSW | `regs.mmu_ssw` | **real** — TM=FC, SIZE, RW, ATC, plus CM/LK/TT0/TT1 for MOVEM / locked RMW / MOVES / MOVE16 |
| `$0E` | WB3S | `regs.wb3_status` | **real** — `0x80 \| (ssw & 0x7f)` on a write, `0` on a read |
| `$10` | WB2S | `regs.wb2_status` | **real** — non-zero only for a MOVE16 line write |
| `$12` | WB1S | `0` | **real value, no source**: this core models no store buffer, so only the faulting access can be pending. `0` = "slot not valid" is the truthful report and the kernel's replay loop skips it |
| `$14` | FA | `regs.mmu_fault_addr` | **real** |
| `$18` | WB3A | `regs.mmu_fault_addr` | **real** — the pending store's address is the faulting address |
| `$1C` | WB3D | `regs.wb3_data` | **real** — the value the instruction was storing. This is the field ISSUE-10 measured being consumed verbatim |
| `$20` | WB2A | `regs.wb2_address` | **real** for MOVE16; otherwise carries whatever it last held (the 040 arm only writes it for a MOVE16 line) |
| `$24` | WB2D | `0` | **structural**: the 68040 has no scalar WB2 data — a MOVE16 line's data travels in PD0–PD3 |
| `$28` | WB1A | `0` | **real value, no source** (see WB1S) |
| `$2C`–`$38` | PD0–PD3 | `mmu040_move16[0..3]` | **real** for a MOVE16 fault; otherwise a stale line, and inert because the kernel only reads them when WB2S marks a cache-line write-back |

The field order was cross-checked against the AMIX 68040 kernel's own
empirically-dumped offsets in the port repo's `src/wb040.s`
(`WB3S@+78 WB3A@+88 WB3D@+92 / WB2S@+80 WB2A@+96 WB2D@+100 / WB1S@+82 WB1A@+104
WB1D@+108`, relative to its trap frame = these + 64) — they agree exactly. So does
the encoding: that file documents `WBxS` as "valid = bit 7 (0x80); FC = bits 2-0;
SIZE = bits 6-5", which is precisely `0x80 | (ssw & 0x7f)` with this tree's
`MMU_SSW_TM`/`MMU_SSW_SIZE_*`. Its metal-captured example `WB3S=0x0081` (user-data
long write) is what this builder now produces.

**Why WB3 is safe to present here.** The generated 040 handlers already implement
the silicon's continuation semantics: immediately before the final store they do
`m68k_incpci(n); regs.instruction_pc = m68k_getpci(); mmu_restart = false;`
(881 sites in `cpuemu_31.cpp`), and `m68k_run_mmu040()`'s CATCH only rewinds the
PC and flags `if (mmu_restart)`. So a **write** fault stacks the *next*
instruction's PC and the kernel's write-back completes the store exactly once —
no double-apply on a non-idempotent RMW. A **read** fault leaves `mmu_restart`
true, stacks the *faulting* PC, and reports WB3S invalid, so the restart redoes
the access and nothing is replayed. `mmufixup` is disarmed by the handler before
the continuation store, so `cpu_restore_fixup()` correctly leaves an `(An)+`
increment standing on the write path and undoes it on the read path. Host vectors
pin both directions.

#### 2. `amix_mmu_on` under the 040 core, and the image/timing split

The latch was set in exactly one place — `cpummu030.cpp`, on the 030 TC write,
gated on `regs.mmu_page_size == 2048` — a page size the 68040 does not have. It
now also latches in `cpummu.cpp`'s `mmu_set_tc()`:

```c
if (a3000_amix_mode && currprefs.mmu_model == 68040 && regs.mmu_enabled && !amix_mmu_on)
```

**Gate choice: amix_mode + the TC E-bit, not a page size.** The 030 site's
`== 2048` test is really asking "is this the AMIX kernel's MMU rather than some
AmigaOS MMU tool?", using the one page size AMIX-030 uses. On the 040 the mode
already answers that: `UAE_040_MMU` + `amix_mode` exists only to run AMIX (the
plain-AmigaOS smoke recipe of §4 runs with `amix_mode` off), so the E-bit alone is
unambiguous. Testing for 4 K instead would silently drop the entire pacing regime
if the 040 port ever moved to 8 K pages — and that would present as an
intermittent SCSI timing fault, not as a configuration change. The page size is
logged on the serial (`68040 MMU enabled under AMIX. Page size = N`) so a surprise
is still visible.

**The separation.** `amix_mmu_on` now means only *when* ("the AMIX kernel is up"),
and is set by both cores. Everything it gates is **timing**, and applies to both:
`set_status()` pacing (`eff = (wd_delay_mode || amix_mmu_on) ? delay : 0`), the
`wd_check_interrupt()` re-arm to `irq = 2`, the per-access pump suppression
(`if (!amix_mmu_on) wd_check_interrupt(1, 0)`), and the aux-streak force. The
paced regime is needed under 040 as well: the 040 kernel embeds the same stock
byte-patched `sd` driver whose step-by-step open required pacing on 030.

A second flag, **`amix_kimage_030`**, now means *which guest* ("the running kernel
is the 68030 AMIX image whose `.data` addresses are hard-coded in
`a3000_scsi.cpp`"). It gates every hard-coded `0x07xxxxxx` read — `0x0700D016`
istate, `0x0700D176` curunitp, `0x070EA740` ddtab, plus `0x0700CF8C`/`0x0700D014`/
`0x0700D020` in `a3000_scsi_dumpqueue()`. Those are byte offsets into one linked
kernel image; against the 040 image they read whatever happens to live there.

It is derived from the CPU model **once per session** in `a3000_scsi_init()`
(`amix_kimage_030 = (currprefs.mmu_model == 68030)`), deliberately *not* from the
latch. Two consequences: the 030 path keeps its behaviour exactly, including
before the latch fires (the flag is already 1 while the Kickstart ROM loads the
kernel, so `a3000_scsi_dumpqueue()` still works pre-latch as it always did); and
no future change to the latch can re-arm the pokes by accident. The two reader
helpers (`amix_cpu_istate()`, `amix_cpu_curunitp_req()`) also refuse internally
(`if (!amix_kimage_030) return 0;`), so a new call site cannot reintroduce the
hazard by forgetting the gate, and `a3000_scsi_dumpqueue()` prints a refusal
instead of a page of garbage.

**Rule for anyone adding to `a3000_scsi.cpp`:** a new hard-coded `0x07xxxxxx`
guest-image read must test `amix_kimage_030`, never `amix_mmu_on`.

### Audit: the `amix_mode` / `a3000_amix_mode` gates under an 040 core

Checked for 030-specific assumptions. All of these are CPU-model-independent and
work unchanged:

| gate | where | verdict under 68040 |
|---|---|---|
| SERDAT `$00DFF030` console mirror to jtag | `cpu_emulator.cpp` `ps_write_16` | OK — address-keyed, on the physical-bus accessor the `slow`/`mobo`/`chpr` banks use. The 040 accessors (`x_phys_*` → `phys_put_word` → `put_word`) go through the same `addrbank` dispatch, so MMIO is reached identically. (Pre-existing on both cores: only *word* writes are mirrored — a long write to SERDAT would not be.) |
| INTREQR `$00DFF01F` PORTS injection | `cpu_emulator.cpp` `ps_read_8` | OK — keyed on `a3000_scsi_irq`, no CPU dependence. |
| CIA-A PRA `$00BFE001` boot-prompt auto-click | `cpu_emulator.cpp` `ps_read_8` | Mechanism OK (spin detection counts `ps_count` gaps — CPU-agnostic). *Watch:* its stop condition `amix_kernel_loaded` is set by an **LBA heuristic** (`!write && lba > 1700000`, `a3000_scsi.cpp`). A 040 miniroot with a different on-disk layout may not trip it, leaving the auto-click armed into AMIX's own pre-root prompt. |
| A3000 SCSI window `$00DD0000` + `a3000_scsi_init()` | `uae_emulator.cpp` | OK — mapped from `amix_mode` in the memory-map setup; no model dependence. |
| single ≤16 MB window / `a3000mem` at `$07000000` | `uae_emulator.cpp`, `memory.h get_real_address` | OK — `a3000_amix_mode`-gated host translation, model-independent. |
| WD33C93 interrupt pump (`a3000_scsi_hsync` every 256 polls) | `newcpu.cpp check_uae_int_request` | OK — `m68k_run_mmu040()` calls `check_uae_int_request()` every instruction, same as the 030 loop. |
| AMIX warm-reboot guard (invalid reset vector → `hard_reboot`) | `newcpu.cpp m68k_reset_newcpu` | OK — `a3000_amix_mode`-gated, model-independent. |
| `AMIX_HIDE08` | `mmu_common.h` | Inert (compiled to `0`) on both cores. |
| wild-PC / retired-instruction diagnostics | `newcpu.cpp m68k_run_mmu030` | Inert under 040 — they live in the 030 run loop only, and are diagnostics. |
