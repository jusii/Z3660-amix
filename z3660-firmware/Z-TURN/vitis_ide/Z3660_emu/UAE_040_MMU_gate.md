# 68040-MMU boot mode — GO (metal-proven), now runnable (MMU-off)

> **Status: GO — verified on real hardware 2026-08-09.** The 12.156 MiB oversized
> `BOOT.BIN` (the 68040-MMU table linked in) loaded and booted on metal
> ("[SD Init] Done (Z3660.BIN)", not FAILSAFE), so the old ~12 MiB "cap" is a
> dead TFTP-era rule of thumb, not a boot-load wall (real ceiling is >12.156 MiB,
> exact unknown). `UAE_040_MMU` is now **runnable**: the run loop, the
> `set_x_funcs` 040 accessor arm and the `main.cpp` dispatch are wired, the six
> compile bridges are faithful, and a host smoke executes the 68040 interpreter.
> Validated only in the **MMU-off / transparent** path (no 68040 AMIX kernel
> exists yet); real 040 table-walk/TTR translation is the 3.3 harness and the
> faithful 68040 bus-error stack frame is the still-parked 3.4 task.

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
| **Current runnable image (P3.2 wiring)** | **12 745 956** | **12.156 MiB, zlib crc32 `0x92E1473F`** (+56 B: run loop + dispatch) |

040-table linked contribution (from `Z3660_emu/build/mapfile.txt`): `cpuemu_31.o`
268 364 + `cpustbl_mmu040.o` 22 488 = 290 852 B. (For reference the 030 table is
larger: `cpuemu_32.o` 468 848 + `cpustbl_mmu030.o` 22 116 = 490 964 B — the 68040
handlers generate more compactly.)

Current runnable image on disk for flashing:
`z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/BOOT.BIN`
(12 745 956 B, zlib crc32 `0x92E1473F` — matches the board's SD `E`→CRC).

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
  `uae_emulator(0, 68040, 1)` (JIT off, `mmu_model = 68040`). Append-only
  `UAE_040_MMU` in both `BOOTMODE` enums (`config_file.h`/`.c`, `main.h`
  `static_assert` 9); no existing index moved.
- Host smoke `test_mmu040_interp_exec` (test/host, **69/69**): with the MMU off
  (`regs.mmu_enabled == 0`, transparent accessors) `build_cpufunctbl` selects
  mode 4 / `op_smalltbl_31_ff` and the interpreter executes `moveq`/`add.l` plus a
  `move.l Dn,(An)` store through `put_long_mmu040`.

NOT done (out of this step's scope):
- **Real 040 MMU translation** (TC-enabled table walk / TTR / descriptor format)
  is untested — needs the 3.3 TTR/descriptor vector harness, and ultimately a
  68040 OS that enables the MMU. There is no 68040 AMIX kernel yet, so only the
  MMU-off path is exercisable.
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
3. **Boot a plain AmigaOS** (Kickstart + an AmigaOS/Workbench disk) — **not AMIX**
   (no 68040 AMIX kernel). AmigaOS does not enable the 040 MMU, so the `*_mmu040`
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
