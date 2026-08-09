# 68040-MMU boot mode — size gate, now built and measured

> **Status: the 68040-MMU opcode table is GENERATED and LINKED IN, and a real
> oversized `BOOT.BIN` has been built and measured. It is 12 745 900 B
> (12.156 MiB) — about 159 KiB over the conventional ~12 MiB line. That line is
> an unproven TFTP-era rule of thumb (see below); this image exists so a
> hardware flash-and-boot test can decide whether the line is real. Only the
> table + linkage + the append-only `UAE_040_MMU` enum slot are wired — the
> run loop and the `set_x_funcs` 040 accessor arm are deliberately NOT wired
> yet (selecting `UAE_040_MMU` is not functional).**

This tracks a 68040-MMU interpreter as a sibling to the existing `UAE_030_MMU`
mode (the real 68030 PMMU interpreter — "Emulation mode 4", see
`UAE_030_MMU_plan.md` and `docs/AMIX.md`).

## The ~12 MiB line

`BOOT.BIN` deployment has historically been kept under ~12 MiB (12 × 1024 × 1024
= 12 582 912 B). The figure originated as a **TFTP-write rule of thumb**: above
it the TFTP path was observed to fail silently and fall back to 040-JIT. It was
never established as a hard boot-loader limit — HTTP upload moves far larger
images without trouble, and the loader has simply never been asked to read and
start a `BOOT.BIN` above ~12 MiB. So the line is treated here as **empirical and
unproven**, and this oversized image is the test of it. The board keeps a
`FAILSAFE.bin`, so an image that fails to boot is recoverable.

## 1. How the CPU/MMU opcode tables are generated and built in

The emulator core (`src/uae/`) is Amiberry v5.6.0's UAE CPU core. The instruction
handlers are not hand-written; they are emitted by WinUAE/Amiberry's `gencpu`
generator, one C++ file per (CPU model, execution style) combination.

**Generator toolchain** — `test/gencpu/`:

- `build_gencpu.sh <winuae-src-dir> [out]` builds a host `gencpu` from an upstream
  checkout's `gencpu.cpp` against **this tree's** `cpudefs.cpp`, `readcpu.cpp` and
  headers, so generated tables match this tree's opcode set. The only external
  symbol is Amiberry's `ua()`, stubbed by `charset_stub.cpp`.
- `baseline_diff.sh` is the non-MMU correctness gate (see `test/gencpu/README.md`).

**Generator used for the MMU tables:** WinUAE **4.4.0** `gencpu` (tag `4400`,
commit `c7b24b37`). Its `generate_cpu()` assigns the MMU file ids: **id 31 =
68040 MMU**, id 32 = 68030 MMU, id 33 = 68060 MMU. So the 68040 table is
`cpuemu_31.cpp` with dispatch table `op_smalltbl_31_ff` — do not assume the id;
the generator picks it. (The 68030 table is `cpuemu_32.cpp`, *not* the `cpuemu_31`
the original plan guessed — a caution that the id belongs to the generator.)

**A newer `gencpu` does not match this tree.** The WinUAE 6.1.0 `gencpu` builds
and generates, but its 68040 output diverges from this tree's Amiberry-5.6.0
headers (a 2-arg `x_get_disp_ea_020`, `real_opcode` intermediates, `_ff` vs
`CPUFUNC` form). The 4.4.0 `gencpu` matches — same lineage that produced the
checked-in `cpuemu_32`.

**Standalone dispatch table.** As with `cpustbl_mmu030.cpp`, the generator's
`cpustbl.cpp` `CPUEMU_31` block is lifted into a standalone `cpustbl_mmu040.cpp`
and converted from the generator's 6-field `struct cputbl` (`{func, NULL, …}`) to
this tree's 5-field one (`{func, opcode, len, {d,d}, br}`), with per-handler
externs added, so the stock `cpustbl.cpp`/`cputbl.h` stay untouched.

**Compile bridges.** The 68040 table is the first MMU table this tree has built,
and a few WinUAE-040 codegen symbols differ from this tree's (Amiberry 5.6.0) API.
`cpuemu_31.cpp` carries a small local `#define` block mapping them onto existing
tree symbols so it compiles and links (all inert until the 040 run loop exists):

| generated call | mapped to | note |
|---|---|---|
| `x_get_disp_ea_020(base, idx)` | `x_get_disp_ea_020(base)` | idx is vestigial (always 0) |
| `m68k_setstopped(level)` | `m68k_setstopped()` | tree's is arg-less |
| `do_cycles_stop(c)` | `((void)(c))` | no cycle accounting (matches the 030 STOP path) |
| `MakeFromSR_STOP()` | `MakeFromSR()` | |
| `checkint()` | `doint()` | |
| `op_unimpl(op)` | `op_illg(op)` | 040 unimplemented-instruction trap → illegal (stub) |

**Build-in path** (no Makefile edit): any `*.cpp` under `src/` is auto-globbed,
compiled `-Os -ffunction-sections -fdata-sections`, linked with `--gc-sections`.
`newcpu.cpp` puts `op_smalltbl_31_ff` in the dispatch matrix `cputbls[4][4]` (the
68040 row, MMU column) and `build_cpufunctbl()` selects that column when
`currprefs.mmu_model == 68040`. Because the table is reachable through `cputbls`,
`--gc-sections` retains all of it — its full size lands in `BOOT.BIN`.

## 2. Measured numbers

Clean container build (`./docker/run.sh make build` → `make bootbin`), both cores
green, 040 table linked in.

| quantity | bytes | note |
|---|---:|---|
| Conventional line (12 MiB) | 12 582 912 | TFTP-era rule of thumb, under test |
| Baseline `BOOT.BIN` (no 040 table) | 12 428 476 | 11.853 MiB |
| **Oversized `BOOT.BIN` (040 table linked)** | **12 745 900** | **12.156 MiB** |
| Delta added by the 040 table | 317 424 | 310.0 KiB |
| Over the 12 MiB line by | 162 988 | 159.2 KiB |

040-table linked contribution (from `Z3660_emu/build/mapfile.txt`):

| object | linked contribution | role |
|---|---:|---|
| `cpuemu_31.o` | 268 364 | 68040-MMU opcode handlers |
| `cpustbl_mmu040.o` | 22 488 | 68040-MMU dispatch table |
| **040 table total** | **290 852** | 284.0 KiB |

For reference the 030 table is larger (`cpuemu_32.o` 468 848 + `cpustbl_mmu030.o`
22 116 = 490 964 B): the 68040 handlers generate more compactly than the 68030-MMU
ones. An earlier estimate that used the 030 table as a proxy over-predicted the
image at ~12.32 MiB; the built number is 12.156 MiB.

**Oversized image for the metal test:**

- path: `z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/BOOT.BIN`
- size: 12 745 900 B
- zlib crc32 (matches the board's SD `E`→CRC): `0x95e2020c`

Reproduce a per-object contribution:

```sh
cd Z3660_emu/build
awk '$NF ~ /\/cpuemu_31.o$/      && $(NF-1) ~ /^0x/ {s+=strtonum($(NF-1))} END{print s}' mapfile.txt
awk '$NF ~ /\/cpustbl_mmu040.o$/ && $(NF-1) ~ /^0x/ {s+=strtonum($(NF-1))} END{print s}' mapfile.txt
```

## 3. What the metal test decides

The image is 159 KiB over the conventional line. The flash-and-boot test answers:
does the boot loader read and start a `BOOT.BIN` of this size, with the firmware
coming up normally (in its existing default mode — `UAE_040_MMU` itself is not yet
runnable)?

- **Boots** → the ~12 MiB figure was a TFTP-transfer artifact, not a boot limit;
  the 040-MMU lane can proceed to wire the run loop (below) within a larger budget.
- **Does not boot / silent SD-write fail** → the line is real; enabling 040-MMU
  then requires one of the size strategies in §5. `FAILSAFE.bin` recovers the board.

The 68030-MMU mode (`UAE_030_MMU`) is unchanged and AMIX (a 68030 OS) boots on it
as before; nothing here regresses the shipping path.

## 4. What is wired, and what is not

Wired (this change):
- `cpuemu_31.cpp` + `cpustbl_mmu040.cpp` generated, compile-clean, linked in.
- `op_smalltbl_31_ff` in `cputbls[4][4]`; `build_cpufunctbl()` selects it for
  `mmu_model == 68040` (so the table is retained past `--gc-sections`).
- Append-only `UAE_040_MMU` value in both `BOOTMODE` enums —
  `Z3660/src/config_file.h` (+ `config_file.c bootmode_names[]`) and
  `Z3660_emu/src/main.h` (`static_assert` bumped to 9). No existing index moved.

NOT wired (next step — do not assume `UAE_040_MMU` runs):
- the `m68k_run_mmu*` run loop for 68040 and the `m68k_go` selection arm;
- the `set_x_funcs()` 040 arm (point `x_*` at the `*_mmu040` accessors) and the
  `uae_emulator()` config that sets `mmu_model = 68040`, JIT off;
- the `main.cpp` dispatch arm (selecting `UAE_040_MMU` currently falls through to
  the existing default);
- the compile bridges in §1 are stubs, correct only for a non-executing table;
- the Amiga-side ZTop and on-board ARM ZTop UI lists still show the old modes
  (their `NUM_BOOT_MODE_LABELS` is separate; unchanged here).

## 5. Size strategies if the line proves real

1. **Share one MMU table across 030 and 040** via the `set_x_funcs()` accessor
   indirection — no second table. Cleanest long-term, most work.
2. **Load the 040 table on demand** as an SD overlay into DDR — keeps `BOOT.BIN`
   small; needs a bare-metal loader + relocatable dispatch table.
3. **Drop the mode** — AMIX boots under `UAE_030_MMU`; `UAEJIT_040` covers non-MMU
   040 workloads. Cheapest.
4. **Free headroom elsewhere** — the biggest retained objects (`cpuemu_13` 1.44 MB,
   `cpuemu_11` 1.40 MB) are both in use; no clean win without a boot-mode audit.
