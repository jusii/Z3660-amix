# 68040-MMU boot mode — size-feasibility gate

> **Status: gate MEASURED — verdict NO-GO under the current `-Os` + `--gc-sections`
> regime. A `UAE_040_MMU` boot mode built as a *second, sibling* full 68040-MMU
> opcode table linked into `BOOT.BIN` does not fit under the ~12 MiB SD/TFTP write
> cap. Adding it is now a design decision (share the table / load on demand /
> drop the mode / free headroom elsewhere), not a mechanical port.**

This is the opening analysis for a 68040-MMU interpreter as a sibling to the
existing `UAE_030_MMU` mode (the real 68030 PMMU interpreter — "Emulation mode 4",
see `UAE_030_MMU_plan.md` and `docs/AMIX.md`). Before generating a ~40 000-line
opcode table, we answer one question: **does it fit in `BOOT.BIN`?**

## 1. How the CPU/MMU opcode tables are generated and built in

The emulator core (`src/uae/`) is Amiberry v5.6.0's UAE CPU core. The instruction
handlers are not hand-written; they are emitted by WinUAE/Amiberry's `gencpu`
generator, one C++ file per (CPU model, execution style) combination.

**Generator toolchain** — `test/gencpu/`:

- `build_gencpu.sh <amiberry-or-winuae-src-dir> [out]` builds a host `gencpu`
  from the upstream checkout's `gencpu.cpp` against **this tree's** `cpudefs.cpp`,
  `readcpu.cpp` and headers, so generated tables match this tree's opcode set.
  The only external symbol is Amiberry's `ua()`, stubbed by `charset_stub.cpp`.
- `baseline_diff.sh <amiberry-v5.6.0/src>` is the correctness gate: this tree's
  checked-in non-MMU `cpuemu_{4,11,13,44}` are byte-identical to Amiberry v5.6.0's
  pre-generated files; `cpuemu_0` (14), `cpuemu_40` (104) and `cpustbl` (1712,
  cosmetic) are the only known local edits. `test/gencpu/README.md` has the detail.
- **`gencpu` version caveat:** Amiberry v5.6.0's *repo* `gencpu.cpp` aborts on the
  68060 `HALT`/`PULSE`/`LPSTOP` opcodes and omits the `(uae_s16)` `cpustbl` cast
  this tree carries. The MMU tables were produced by a **WinUAE 4.4.0-lineage
  `gencpu`** whose output pairs cleanly with the pristine `cpuemu_13`.

**File-number caveat (important):** the generator, not the plan, assigns the file
id. The 68030-MMU table landed as **`cpuemu_32.cpp` (id 32)** — *not* the
`cpuemu_31` the original plan assumed. The 68040-MMU table's id is likewise
whatever `gencpu` emits for the 040-MMU combination; do not assume `cpuemu_31`.

**Build-in path** (no Makefile edit needed to add a table):

1. Any `*.cpp` under `src/` is auto-globbed by the emulator `Makefile`
   (`find $(SRCDIR) -type f -name '*.cpp'`), compiled `-Os -ffunction-sections
   -fdata-sections`, and linked with `--gc-sections`.
2. The 030-MMU handlers live in `cpuemu_32.cpp`; their dispatch table
   `op_smalltbl_32_ff[]` lives in the standalone `cpustbl_mmu030.cpp` (keeps the
   stock `cpustbl.cpp`/`cputbl.h` untouched).
3. `newcpu.cpp` references `op_smalltbl_32_ff` from the dispatch matrix
   `cputbls[5][5]` — column 4 (the MMU column), row 3 (68030). `build_cpufunctbl()`
   selects column 4 when `currprefs.mmu_model == 68030`. The 68040 row already has
   a reserved-but-`NULL` MMU cell (`cputbls[4][4]`) awaiting an 040 table.

Because a table is reachable through `cputbls[][]`, **`--gc-sections` cannot drop
it** — every handler it names is retained. That retention is exactly the size cost
this gate measures.

## 2. The size gate — measured numbers

Cap: **`BOOT.BIN` must stay < ~12 MiB** (12 × 1024 × 1024 = 12 582 912 B). Above
it the SD/TFTP write silently fails and the board falls back to 040-JIT.
(`CLAUDE.md`, `CHANGELOG.md`, `docs/port-ledger-beta21.md`.)

Baseline: a clean container build (`./docker/run.sh make build` → `make bootbin`)
at the current tree.

| quantity | bytes | note |
|---|---:|---|
| Cap (12 MiB) | 12 582 912 | SD/TFTP write threshold |
| Baseline `BOOT.BIN` | 12 428 476 | 11.853 MiB — freshly built, matches the shipped figure |
| **Headroom under cap** | **154 436** | **150.8 KiB** |

Cost of **one** MMU opcode table, taken from the linked contribution of the
existing 030-MMU table in the same image (the best proxy — the 040-MMU table is
generated the same way and, like the 030 one, reaches memory through out-of-line
`*_mmu*_state` accessors rather than inlined direct pointers, so its handlers are
the same *compact* shape, not the ~1.4 MB shape of the inlined `cpuemu_13`):

| object | linked contribution | role |
|---|---:|---|
| `cpuemu_32.o` | 468 848 | 030-MMU opcode handlers |
| `cpustbl_mmu030.o` | 22 116 | 030-MMU dispatch table |
| **one MMU table** | **490 964** | **479.5 KiB** |

Projection with a sibling 040-MMU table linked in:

```
projected BOOT.BIN = 12 428 476 + 490 964 = 12 919 440 B (12.321 MiB)
overshoot vs cap   = 12 919 440 − 12 582 912 = 336 528 B (328.6 KiB OVER)
table / headroom   = 490 964 / 154 436 = 3.18x   (must be <= 1.00 to fit)
```

Sensitivity — even an unrealistically small 040 table still overshoots:

| assumed 040 handler size | + dispatch tbl | projected `BOOT.BIN` | over cap by |
|---|---:|---:|---:|
| 324 884 (68060 non-MMU, absolute floor) | 22 116 | 12 775 476 | 192 564 B |
| 365 352 (68040 non-MMU) | 22 116 | 12 815 944 | 233 032 B |
| 468 848 (030-MMU proxy, expected) | 22 116 | 12 919 440 | 336 528 B |

**Reproduce the per-object contribution** (GNU ld map; sums every retained input
section attributed to the object):

```sh
cd Z3660_emu/build
awk '$NF ~ /\/cpuemu_32.o$/     && $(NF-1) ~ /^0x/ {s+=strtonum($(NF-1))} END{print s}' mapfile.txt
awk '$NF ~ /\/cpustbl_mmu030.o$/ && $(NF-1) ~ /^0x/ {s+=strtonum($(NF-1))} END{print s}' mapfile.txt
```

## 3. Verdict

**NO-GO.** With the current `-Os` + `--gc-sections` regime and the current
baseline, a second full 68040-MMU opcode table overshoots the 12 MiB cap by
~330 KiB (best estimate) and by ~190 KiB even in the most optimistic case. There
is no headroom-only path: the table is 3.18× the available 150.8 KiB. Generating
and linking it as-is would push `BOOT.BIN` over the write cap and silently break
deployment (fallback to 040-JIT).

The 68030-MMU mode (`UAE_030_MMU`) is unaffected and AMIX (a 68030 OS) boots on
it today; nothing here regresses the shipping path.

## 4. Options for enabling a 040-MMU mode (design decision)

Each is a real design change beyond a mechanical table port; pick before spending
generation effort.

1. **Share one MMU table across 030 and 040.** The 030 and 040 user-mode
   instruction sets overlap heavily; the generated handlers differ mainly in the
   memory-accessor calls (`*_mmu030_state` vs the 040 `*_mmu` accessors) and a few
   model-specific MMU/cache ops. Routing the accessor through the existing per-mode
   `x_*` function pointers (already the indirection point in `set_x_funcs()`) could
   let one table serve both models — no second 470 KB. Highest engineering effort;
   also the cleanest long-term.
2. **Load the 040 table on demand (overlay).** Keep the ~470 KB table off
   `BOOT.BIN`; ship it as a separate SD blob loaded into DDR only when 040-MMU is
   selected. `BOOT.BIN` stays under cap and DDR has room to spare. Cost: a loader
   plus position-consistent code/relocatable function-pointer table on bare metal
   — non-trivial.
3. **Drop the mode.** AMIX targets the 68030 and boots under `UAE_030_MMU`;
   `UAEJIT_040` already covers non-MMU 040 workloads. If no near-term OS needs a
   faithful 040 PMMU on this board, dropping the faithful 040-MMU interpreter is
   the cheapest outcome and costs nothing today.
4. **Free ~470 KB elsewhere first.** The largest retained objects are `cpuemu_13`
   (1.44 MB, needed by `UAE_030`) and `cpuemu_11` (1.40 MB, 68020 cycle-exact).
   Reclaiming space would mean auditing which boot modes/tables are actually
   reachable and dropping an unused one — trades one mode for another, needs a
   usage audit, and the tree is already `-Os`. No clean 470 KB win in sight.

## 5. What was deliberately NOT done

Per the gate outcome, no tables were generated, no `BOOTMODE` enum slot was added,
and no build files were changed — those belong to the "fits" branch, which this
gate closed as NO-GO. When a direction above is chosen, the reserved 68040 MMU
cell is `cputbls[4][4]` in `newcpu.cpp` and the append point for a `UAE_040_MMU`
enum value is after `UAE_030_MMU` in both `Z3660/src/config_file.h` (with the
matching name in `config_file.c bootmode_names[]`) and `Z3660_emu/src/main.h`
(bump its `static_assert(BOOTMODE_NUM == …)`).
