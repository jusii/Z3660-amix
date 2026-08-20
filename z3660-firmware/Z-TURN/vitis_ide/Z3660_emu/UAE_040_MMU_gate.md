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
> Three metal attempts on 2026-08-20. Two stopped short of the emulator — core0 did not
> recognise `UAE_040_MMU` as an emulated mode and never started core1 (§8.3), then the
> core1 breadcrumb called `z3660_printf` before `init_shared()` and wedged core1 on every
> mode (§8.4). The third brought the 040 core fully up (banner, `Emulation mode 4`,
> `CPU=68040 FPU=68040`, 030 canary green) and AmigaOS wedged on its first `STOP`, whose
> 040-MMU-table handler never advanced the pc (§8.5). The fourth reached the AMIX kernel,
> which panicked because `MOVEC`->TC never enabled the MMU (§8.6); the fifth got one VM-init
> stage further, because `PFLUSH` was a no-op and `PTEST` fabricated (§8.8). All fixed. The
> recurring shape: a 68040 register or instruction whose side-effect function exists in
> `cpummu.cpp` but has no caller.

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
| AMIX-040 fault-path image (§8.1 + §8.2) | 12 762 340 | 12.171 MiB, zlib crc32 `0x2CFA9216` — format-$7 frame + the 040 `amix_mmu_on` latch. **Never emulated anything on metal: core0 did not start core1 (§8.3)** |
| core1-launch fix image (§8.3) | 12 762 340 | 12.171 MiB, zlib crc32 `0xBBD5C14F` — `boot_mode_is_emulated()` + the core1 breadcrumbs. **Core1 wedged on every mode: the breadcrumb called `z3660_printf` pre-`init_shared()` (§8.4)** |
| NULL-safe `z3660_printf` image (§8.4) | 12 762 340 | 12.171 MiB, zlib crc32 `0x806C76AE` — **first image to run the 040 core; AmigaOS wedged on its first `STOP` (§8.5)** |
| `STOP` pc-advance image (§8.5) | 12 762 340 | 12.171 MiB, zlib crc32 `0x6FDBDA40` — **ran the whole 040 chain; kernel panicked because the 040 MMU was never enabled (§8.6)** |
| `MOVEC`->TC MMU-enable image (§8.6/§8.7) | 12 762 356 | 12.171 MiB, zlib crc32 `0xD910C41A` — **MMU enable + preload speedup both proven; kernel then died on a no-op `PFLUSH` (§8.8)** |
| `PFLUSH`/`PTEST` dispatch image (§8.8) | 12 762 364 | 12.171 MiB, zlib crc32 `0x095356E9` — **panicked byte-identically to D910C41A: real bugs, not this panic's cause (§8.9)** |
| exception-trace instrumentation image (§8.10) | 12 762 364 | 12.171 MiB, zlib crc32 `0xF4782D71` — **one-line trace; prediction falsified (§8.11)** |
| extended-trace image (§8.12) | 12 762 372 | 12.171 MiB, zlib crc32 `0xF87F6FA7` — **named the fault: exec's trap dispatcher on a dead ExecBase (§8.13)** |
| **ROM-entry probe image (§8.14)** | **12 762 372** | **12.171 MiB, zlib crc32 `0x8C51370A`** — diagnostic build, no behaviour change |

040-table linked contribution (from `Z3660_emu/build/mapfile.txt`): `cpuemu_31.o`
268 364 + `cpustbl_mmu040.o` 22 488 = 290 852 B. (For reference the 030 table is
larger: `cpuemu_32.o` 468 848 + `cpustbl_mmu030.o` 22 116 = 490 964 B — the 68040
handlers generate more compactly.)

Current image on disk for flashing (rebuild target, gitignored):
`z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/BOOT.BIN`
(12 762 372 B, zlib crc32 `0x8C51370A` — compare against the board's SD `E`→CRC).

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

#### 3. First metal attempt (2026-08-20): core0 never started core1

Firmware `2CFA9216` was flashed and `UAE_040_MMU` selected for the first time on real
hardware. The card never emulated anything: **not one line of core1 output**, and the
Amiga reset line cycled every ~3 s forever.

The cause was not in the emulator, and nothing in §8.1/§8.2 was reached. **Core1 was
never started.** `Z3660/src/main.c` classified the boot mode with a copy-pasted list of
the *emulator* modes:

```c
else if(   config.boot_mode==MUSASHI  || config.boot_mode==UAE_030
        || config.boot_mode==UAEJIT_030 || config.boot_mode==UAE_040
        || config.boot_mode==UAEJIT_040 || config.boot_mode==UAE_030_MMU)
   emu=1;
else
   emu=0;
```

`UAE_040_MMU` is absent, so `emu` was 0 — the *real 68060* setting. `emu==1` gates the
only site that starts core1 (entry address → `0xFFFFFFF0`, `sev`, the `shared_data`
handshake, `main.c` ~line 1780), so core0 skipped it entirely, fell through to its
"exclusive for real CPU" path, and did `CPLD_RESET_ARM(1); NBR_ARM(1)` — relinquishing
the bus to a 68060 that is not fitted on this board. With no CPU driving it, the Amiga
watchdog-resets forever.

The serial log proves which path ran, with no inference required:

| message | this boot | whole serial history |
|---|---:|---:|
| `Starting CPU emulator on Core1` (emulator path) | **0** | 648 |
| `060 starting now...` (real-CPU path) | **1** | **1** |

That single lifetime occurrence of `060 starting now...` is this boot. Core1's own first
line (`MMUL2Table = ...`, printed before any emulator work) is absent too, which is what
distinguishes "never launched" from "launched and crashed".

**The predicate was replicated eight times** — `main.c` ×1, `debug_console.c` ×6,
`rtg/rtg.c` ×1 — and all eight omitted `UAE_040_MMU`. They are now one shared
`boot_mode_is_emulated()` in `Z3660/src/config_file.h`, written as a **closed list of the
two real-CPU modes** (`MOBOCPU`, `CPU`) so that a mode appended to `BOOTMODE` defaults to
*emulated*. That is the safe direction: core1's own dispatch halts loudly with
`No emulator selected!!!` if it has not been taught a mode, whereas the old default handed
the bus to absent silicon.

**Host guard.** `test/host/bootmode_test.cpp` (new, 26 checks) compiles
`Z3660/src/config_file.h` — self-contained, no BSP — and asserts that every index in
`[0, BOOTMODE_NUM)` except `MOBOCPU`/`CPU` classifies as emulated, plus the literal enum
indices the replicas depend on. Verified non-vacuous: rebuilt against the pre-fix
predicate it fails on index 8 with
`boot mode index 8 is not classified as emulated`. The rest of core0 (`main.c`,
`debug_console.c`, `rtg.c`) is **not** host-compilable — Xilinx BSP, FatFs, FPGA register
access — so the *symptom* cannot be reproduced host-side; only the invariant behind it.

**Breadcrumbs added** (`Z3660_emu/src/main.cpp`), both permanent and one-shot:
`[Core1] alive` as the first statement of core1's `main()` (before any risky init;
`shared` is still NULL there, so it prints nothing else), and
`[Core1] dispatch: cfg_emu=N amix_mode=N` when core0 hands core1 a job. The *absence* of
the first line is now an unambiguous "core0 never started core1"; the second pins
`cfg_emu` against the `BOOTMODE` replicas and shows the AMIX flag core1 actually received.

**Still open after this fix:** the Amiga-side `z3660-drivers/ZTop/Ztop.c` keeps its own
8-entry `BOOTMODE` enum and a `bootmode_names[NUM_BOOTMODES][25]` label table that stop at
`UAE_030_MMU`. Opening ZTop while the card runs `UAE_040_MMU` (index 8) reads
`bootmode_names[8]` out of bounds, and ZTop cannot select the mode. Driver-side, not
touched here.

#### 4. Second metal attempt (2026-08-20): the breadcrumb itself killed core1

`BBD5C14F` proved the §8.3 fix: core0 printed `Starting CPU emulator on Core1` and
`060 starting now...` was gone. But core1 then died on **every** boot mode — the golden
030 path included, which had been multiuser-green on `455A7D12` hours earlier — with core0
parked on `Waiting ack from core1...` forever and not one byte from core1, not even the
new `[Core1] alive` breadcrumb.

**The breadcrumb was the bug.** `z3660_printf()` takes a UART semaphore that lives in the
SHARED struct:

```c
extern "C" void z3660_printf(const TCHAR *format, ...)
{
   ...
   while(shared->uart_semaphore!=0);   // <-- dereferences `shared`
   shared->uart_semaphore=1;
```

`shared` is assigned in `init_shared()`, called at `main.cpp:886`. The breadcrumb sat at
`main.cpp:852`, **34 lines earlier**, so the very first statement core1 executed was
`while(((SHARED*)0)->uart_semaphore != 0);` — a spin on DDR `0x0000001C`
(`uart_semaphore` is at SHARED offset `$1C`). Nothing faults: core1 has not yet poked
`MMUTable[0]=0` (that is line 882, *after* the breadcrumb), so address 0 is still ordinary
mapped DDR. The word there was non-zero and never changed, so core1 spun there forever,
emitting nothing and never acking.

Mode-independent by construction — it is the first statement of `main()`, long before any
mode dispatch — which is precisely why the proven 030 path broke too. That total silence
plus a hung ack is the signature of this class, and it is worth recognising on sight.

I had noted in the original comment that `shared` was NULL at that point and that the line
"must not dereference it" — then called a helper that dereferences it internally. Checking
the arguments and not the callee is the whole mistake.

**Fix.** `z3660_printf()` is now NULL-safe: it takes the semaphore when `shared` exists and
falls back to a bare `vprintf` before that — which is exactly what core1's long-proven
pre-init prints (`printf("MMUL2Table = ...")` at line 892, and the silicon-version line)
have always done. The helper is therefore safe to call from anywhere, so the breadcrumb
keeps its earliest-possible placement and the trap cannot bite the next caller either.

*Considered and rejected:* statically initialising `SHARED *shared = (SHARED *)0xFFFF0000`
to remove the NULL window for every user. It is tempting and more general, but it changes
the timing of core1's first OCM access and turns the existing deliberate NULL check in
`z3660_dbg_emu()` into dead code — two speculative changes riding along in an image whose
whole job is to prove one fix. The local guard is the minimal provably-correct change.

**The other suspect was refuted by measurement, not argument.** A C++ static initializer
introduced by the night's emulator commits would produce the same total silence (a crash
before `main()`). It did not happen: no object touched that night carries a global
constructor, and `.init_array` is unchanged at four pre-existing entries.

```
$ arm-none-eabi-nm build/{main,uae/uae_emulator,uae/cpummu,uae/newcpu,uae/newcpu_common,uae/a3000_scsi}.o | grep -c _GLOBAL__sub_I_
0            # every one of them
$ arm-none-eabi-readelf -S Z3660_emu.elf | grep init_array
  [60] .init_array  INIT_ARRAY  3062ea50 63ea50 000010 04  WA  0  0  4     # 0x10 = 4 entries
```

That is a one-command check worth repeating whenever core1 goes silent.

**Not host-checkable, and this is not a gap that can be closed.** `Z3660_emu/src/main.cpp`
is board code — Xilinx cache/GPIO/PLL calls, FPGA registers, the SHARED struct at a fixed
physical address — and the host harness compiles none of it. Worse, `board_stubs.cpp`
*fakes the exact seam that broke*: it defines inert stand-ins for the board globals and has
no `shared` pointer and no UART semaphore at all, so a host build cannot even represent
"helper dereferences a not-yet-initialised shared pointer during core1 bring-up". The
defence here is the structural one — the helper no longer has an unsafe window — plus the
pre-init contract now written at both the definition and the call site.

#### 5. Third metal attempt (2026-08-20): STOP never advanced the pc

`806C76AE` brought the 040 core up for the first time — `[Core1] alive`, dispatch
`cfg_emu=8 amix_mode=1`, the `UAE_040_MMU_AMIX` banner, `Emulation mode 4`,
`CPU=68040, FPU=68040`, the SCSI boot ROM autoconfigured, 030 canary green, no reset
loops. AmigaOS then wedged in the Kickstart strap: uniform grey screen, no insert-disk
animation, the guest's SCSI conversation stopping after the boot ROM's initial
enumeration (57 `SCSIW` / 21 `SCSIR`, then nothing), and the guest pc pinned at
`0x00F81420`, `s=1 msk=0`, identical across every sample.

That pc is exec.library's idle loop. **The pc was pinned ON the `STOP`, not past it** —
`DM` of the live guest ROM:

```
0x00F81410 13205026 AE521066 EE081801 24010700 0020724E 8020E660 ...
                                                ^^^^^^^^ ^^^^^^^^
   $00F81420 = bytes 4E 72 20 00 = STOP #$2000
   $00F81424 = bytes 60 E6       = BRA.S back to $00F8140C (the loop top)
```

A correctly executed `STOP` leaves the pc at `$00F81424` (the `BRA`). It was at
`$00F81420`.

**Cause.** `cpuemu_31.cpp` is the only table in this tree generated by the newer WinUAE
`gencpu`, whose 68040 `STOP` is **re-entrant**: it guards the operand fetch with
`if (!regs.stopped)` and deliberately never advances the pc, because upstream's 040 run
loop re-dispatches the same `STOP` while stopped and advances the pc when the stop ends.
This tree is the older *advance-and-stop* lineage — `m68k_run_mmu040()`,
`do_specialties()` and `m68k_resumestopped()` (`fill_prefetch(); m68k_unset_stop();`)
never touch the pc — so the two halves of the contract did not meet. A survey of every
`STOP` handler in the tree:

| table | ends `STOP` with |
|---|---|
| `cpuemu_0`, `cpuemu_4`, `cpuemu_40`, `cpuemu_44` | `m68k_incpc(4)` |
| `cpuemu_11`, `cpuemu_13`, `cpuemu_32` (68030 MMU) | `m68k_incpci(4)` |
| **`cpuemu_31` (68040 MMU)** | **nothing** |

Interrupts were being taken the whole time — this was never an interrupt-delivery
failure, and the piscsi axis was a coincidence. Every `rte` simply returned to the `STOP`
and stopped again, so the instruction *after* it never ran. exec's idle loop is
`move #$2700,sr / check TaskReady / bset AttnResched / stop #$2000 / bra <top>`: never
reaching the `bra` means the scheduler never re-checks `TaskReady`, so no task is ever
made runnable again. Grey screen, silent SCSI, pinned pc — all one cause. Any 040-MMU
boot of any AmigaOS would have wedged at its first `STOP`; nothing about AMIX or piscsi
was required.

**Fix.** The bridge block at the top of `cpuemu_31.cpp` — the established place for
generator-lineage adaptations — now maps `m68k_setstopped(level)` to a helper that stops
*and* steps past `STOP`'s opcode + operand word, in that order (matching the metal-proven
030 table, whose `DOTRACE` `fill_prefetch` runs at the un-advanced pc). `m68k_setstopped`
appears exactly once in the generated body, so the pc advance cannot leak into any other
instruction, and the generated code itself stays byte-identical to the generator output.

**Host guard.** `test_stop_advances_pc_all_tables` executes a real `STOP #$2000` through
each dispatch table the firmware can select — 68030 and 68040, MMU and non-MMU — and
asserts the pc moved to instruction+4, that `regs.stopped` is set, and that the operand
loaded `intmask` 0. Written as a loop over the tables so a table added later is covered by
construction. Verified non-vacuous: rebuilt against the pre-fix bridge it fails with
`68040 MMU table (cpuemu_31): STOP left the pc ON the instruction (00002000)`.
Harness 126 → **142**.

**Bounded sweep for the same class.** Both MMU tables were diffed handler-by-handler for
"the 030 table updates the pc, the 040 table does not", across all 1836 opcodes they share.
Exactly two hits: `op_4e72` (`STOP`, fixed above) and `op_4848` (`BKPT`), where the 030
table does `m68k_incpci(2); op_illg(opcode);` and the 040 table only `op_illg(opcode)`.
`BKPT` is left alone deliberately: it is unreachable on any AmigaOS or AMIX path, the two
lineages genuinely disagree about whether an illegal-instruction frame should stack the
faulting or the following pc, and changing an exception's stacked pc on a hunch is not
worth a metal cycle. Recorded here so it is a known difference rather than a surprise.

#### 6. Fourth metal attempt (2026-08-20): the 040 MMU was never actually enabled

`6FDBDA40` ran the whole 040 chain for the first time — piscsi strap → boot1 → boot2-040 →
~31 MB miniroot preload → bootstrap warm reset → kernel start →
`[A3000SCSI] WD33C93+SuperDMAC emulation initialised` → VM init → kernel panic:

```
PANIC: page_sub / 4.0 ?   Backtrace: 80F485B: / kstack 0x80F47F8!
TRAP proc = 0 (pid 524295,) psw = 2400 / pc = 40080000
DOUBLE PANIC: KERNEL FAULT psw=0x2400, pc=0x40080000, fmt=0x0, vector=0xB (Line-F Emulator)
```

**`MOVEC` to TC only stored the register.** `m68k_move2c()` (`newcpu_common.cpp`) had:

```c
/* 68040 only */
case 3:
        regs.tcr = *regp & 0xc000;
        break;
```

and `mmu_set_tc()` — which is where `regs.mmu_enabled`, the page size and its masks/shifts,
the ATC flush and the default cache modes are all set — **had no caller anywhere in the
tree**. Every `*_mmu040` accessor gates translation on `regs.mmu_enabled`:

```c
if ((!mmu_ttr_enabled || mmu_match_ttr(...) == TTR_NO_MATCH) && regs.mmu_enabled) { addr = mmu_translate(...); }
```

so the guest's MMU never engaged. The kernel built its tables, wrote TC with E=1, believed
it was running in virtual space — and the emulator kept every access transparent. Kernel VAs
at `0x40000000+` are not RAM on this board; they fall in `RANGE_MAP(0x4000,0x8000,slow_bank)`,
the Z3 expansion window. The cases 4–7 (`ITT0`/`ITT1`/`DTT0`/`DTT1`) had the same shape: the
registers were stored but `mmu_tt_modified()` was never called, so the transparent windows
were dead too.

**The 68030 never comes through here.** It enables its PMMU with `PMOVE` to TC, handled in
`cpummu030.cpp` — which is also where the `68030 MMU enabled. Page size = 2048` line is
printed. That is why `UAE_030_MMU` was completely unaffected and this stayed invisible.

One cause accounts for every observation of the night:

| observation | explanation |
|---|---|
| `PANIC: page_sub` in VM init | first walk of the kernel's own structures through kernel VAs, which are untranslated |
| `pc = 40080000` | a kernel VA executed as a physical address — garbage instruction stream |
| `vector=0xB` (Line-F) | garbage decodes to an F-line opcode readily |
| `fmt=0x0` | correct for a *plain* F-line — see below; it is evidence of garbage, not of an FPU trap |
| no MMU-enable line on serial | the only place that prints one was never called |
| `amix_mmu_on` never latched | it hangs off `mmu_set_tc()`, so the SCSI pacing regime never engaged |
| `[CLICK] e48` runaway | the clicker's disarm signals both depend on the kernel getting somewhere |
| no serial console output | the kernel died in VM init, before console init |

Fixed by calling `mmu_set_tc()` from case 3 and `mmu_tt_modified()` from cases 4–7, both
guarded on `mmu_model` 68040/68060 so the non-MMU 040 modes are untouched. Host guard
`test_mmu040_movec_tc_enables_mmu` executes a real `movec d0,tc` through the 040 dispatch
table and asserts translation actually turns on, the page size is derived, and a VA then
walks the guest's tables to the right frame; plus a `movec d0,dtt0` arm for the TTR path.
Proven non-vacuous: against the pre-fix code it fails with
`MOVEC to TC turned translation ON: got 0x00000000 want 0x00000001`.

##### The F-line frame-format question, answered

The suspicion was that the card core pushes `fmt $0` where a 68040 should push `fmt $2`.
It does not — and the `$0` is a *clue*, not a bug. `Exception_build_stack_frame_common()`:

```c
} else if (nr == 11 && regs.fp_unimp_ins) {
        ... Exception_build_stack_frame(..., nr, 0x2);      /* FPU-unimplemented F-line */
} else {
        Exception_build_stack_frame(oldpc, currpc, 0, nr, 0x0);   /* plain F-line */
}
```

Format `$2` is pushed only when the F-line came from the FPU unimplemented-instruction path
(`regs.fp_unimp_ins`), which is correct 68040 behaviour; a plain F-line gets format `$0`,
also correct. The panic reporting `fmt=0x0` therefore says the CPU hit a **plain** F-line —
i.e. it was executing something that merely decoded as F-line — rather than a real
FP instruction bound for FPSP. That is corroboration of the wild-pc reading, not a
frame-format defect.

##### Ranked hypotheses for `page_sub` (formed before the fix, kept for the record)

1. **The 040 MMU was never enabled** — confirmed above; explains all seven observations with
   one cause, and is fixed. Everything below is what to look at *if page_sub survives it*.
2. **Deferred write-back fabrication** (the bench ISSUE-10 class). Only reachable once
   translation genuinely runs; §8.1's format-`$7` frame now supplies real WB3S/WB3A/WB3D, and
   none of it has executed on hardware yet.
3. **Page size** — if the 040 kernel chose 8 K pages, `mmu_pagesize_8k` is exercised for the
   first time. Now trivially observable: the new TC line spells the page size out.
4. **URP/SRP without an ATC flush** — `movec` to `0x806`/`0x807` stores the root pointer but
   flushes nothing. AMIX should `PFLUSH` on context switch; if it relies on the write alone
   it would see stale translations. Examined and deliberately left alone (a behaviour change
   that cannot be justified without evidence) — first thing to check next.
5. **The 030-shaped assumptions in the AMIX memory contract** — the single ≤16 MB window at
   `$08000000` and the `$07000000` `a3000mem` window were sized for the 030 kernel's SCN1.
   A 040 kernel with a different `MAINSTORE`/`VSIZOFMEM` would fault differently.

#### 7. Observability and iteration-cost work landed alongside

**AMIX console → serial, under 040.** No gating bug was found, and the report should say so
plainly: `a3000_amix_mode` *was* engaged (it is set by `a3000_scsi_init()`, the same function
that printed the `[A3000SCSI] …initialised` banner seen on the night), and the kernel writes
SERDAT as `movew d0,a0@(0x30)` with `a0 = $00DFF000` (`amix-040-060-port` `src/serdbg.s`,
`btrace.s`, `mainmarks.s`) — a word write, exactly what the hook caught. The silence was a
*consequence* of the kernel dying in VM init before console init. Hardened anyway, cheaply:
the mirror now also covers byte writes to `$00DFF031` and long writes to `$00DFF030`, so
access width can never be the reason for silence, and the first mirrored character prints
`[SERMIRROR] AMIX console -> serial` once — which is the line that will *prove* the mirror is
live rather than leaving it to be inferred from absence. On the 030 path only the word arm
ever fires, so nothing there changes.

**MMU-enable print.** `mmu_set_tc()` now emits one line per TC write, shaped like the 030's:
`68040 MMU enabled. TC=8000 Page size = 4096 amix_mmu_on=1 PC=…`. Page size is spelled out
(the 68040 has no 2 K page, so this is the quickest way to see 4 K vs 8 K) and the latch state
is echoed so the SCSI pacing regime is visible at the moment it engages.

**Auto-click disarm.** The clicker had one disarm, `amix_kernel_loaded`, set from an
`lba > 1700000` read heuristic that cannot trip on an 85 MB disk — hence `[CLICK] e48
spin=9.4M` clicking into the running kernel and across the panic screen. Two more were added:
`amix_mmu_on`, which is the exact signal (once the AMIX kernel's MMU is up the loader is long
gone and every later prompt is AMIX's own, which must be left for a real click) and is now
disk-size independent *and* latches on both cores; and `amix_click_done`, a hard session-wide
budget of four full episodes that never resets, so neither signal being available can let the
clicker escalate. 030-safe: on the golden path the loader prompt is dismissed long before the
kernel enables its MMU, the per-episode cap of 64 is untouched, and AMIX's bootstrap warm
reset needs two episodes against a budget of four.

**Preload cost — verdict: no fix tonight, and why.** `docs/piscsi-service-path.md` already
models it: a guest block request costs one cross-core handshake *per emulated register
access*, ten of them, only one carrying data. The strap reads 512 B at a time, so it pays
that fixed overhead four times more often than the AMIX driver's 2 KB steady state. The
firmware cannot batch — the guest chooses both the granularity and the register sequence —
and changing the mailbox protocol is excluded and would break the proven 030 path and both
driver repos. One real firmware-side cost is visible in the doc (three of the ten round trips
do a *whole-L1D* `Xil_L1DCacheFlush()`, replaceable with range-limited maintenance) but cache
maintenance is precisely the class that produced the cdfs page-crossing wild write, it sits on
the proven 030 path, and it needs its own bench proof — a dedicated dispatch, not a rider.

What was taken instead is free: `m68k_run_mmu040()` now honours `z3660_service_cadence` exactly
as the 030 loop does. At cadence 1 (today's env) it is byte-for-byte the old behaviour; it
simply makes the existing `SERV` knob reach the 040 path. **No throughput gain is claimed** —
PSTAT's own split (gap 9678–9692 µs of a ~10 ms request) is not decomposed by the current
instrumentation into guest-side time versus handshake latency, and 9.7 ms is far more than
nine handshakes at core0's measured ~1 µs loop iteration, so something else dominates and
guessing at it is how metal cycles get burned. The cheap next step, if the preload cost
matters more than the boot: add a cross-core handshake counter and latency span to PSTAT and
let one boot decide it.

#### 8. Fifth metal attempt (2026-08-20): PFLUSH was a no-op and PTEST was fabricated

`D910C41A` proved the MMU-enable fix — serial showed
`68040 MMU enabled. TC=8000 Page size = 4096 amix_mmu_on=1 PC=080d7550`, the first 4 KiB-page
040 MMU enable on the card, latch engaged — and the `service_cadence` wiring collapsed the
preload from ~10.5 min to seconds. The kernel then got one VM-init stage further and panicked
again: `page_sub` → `page_free`, psw `2400` → `2708`, wild pc `0x40080000` → `0xF80B4C`, and an
Address Error / fmt `$2` double fault instead of Line-F / fmt `$0` (both frame formats correct
for their vectors — the exception plumbing is fine).

**Same disease as `MOVEC`→TC, one layer along.** `cpuemu_31`'s six F5xx handlers
(`PFLUSHN`/`PFLUSH`/`PFLUSHAN`/`PFLUSHA`/`PTESTW`/`PTESTR`) all call `mmu_op(opcode, 0)` —
and that call bound to the wrong function. `cpummu.h` does not declare `mmu_op`, so the only
visible declaration is `newcpu.h`'s, and this fork's `newcpu.cpp` copy of `mmu_op()` was
missing upstream's first three lines:

```c
void mmu_op (uae_u32 opcode, uae_u32 extra)
{
        if (currprefs.mmu_model) {        /* <-- DROPPED BY THIS FORK */
                mmu_op_real (opcode, extra);
                return;
        }
        ... the "fake" path, for configs with NO MMU model ...
```

So every 68040 MMU instruction fell into the fake path:

| instruction | what actually happened |
|---|---|
| `PFLUSH` / `PFLUSHA` / `…N` | **complete no-op** — `regs.mmusr = 0; return;`. The real ATC (`mmu_atc_array`) and the `MMU_DPAGECACHE` shortcut arrays kept serving translations the guest had just invalidated. |
| `PTEST` | **fabricated** — consulted `fake_mmu_match_ttr()` only, never walked the guest's tables, and returned an MMUSR built from the address (`addr & 0xfffff000`), with no resident bit and no real frame. |

Meanwhile `mmu_op_real()` — the genuine implementation, with `mmu_flush_atc_all(glob)`,
`mmu_flush_atc(addr, super, glob)`, `flush_internals()` and a real `mmu_fill_atc()`-backed
PTEST — sat in `cpummu.cpp` with **no caller anywhere in the tree**, exactly as `mmu_set_tc()`
had.

**Diffed against the bench-proven core, as the sharper test.** `../amiberry`'s
`cpuemu_31.cpp` PFLUSH handler is byte-identical to ours — it also calls `mmu_op(opcode, 0)`.
The divergence is entirely in `newcpu.cpp`: Amiberry's `mmu_op()` has the dispatch guard
(`src/newcpu.cpp:4232-4236`), ours had lost it. That settles where the bug lives without
having to reason about it.

Both halves are on AMIX's VM-init critical path, which is why the panic moved but did not
leave the `page_*` family: the kernel edits page tables and `PFLUSH`es (stale translations
survive → it reads and writes the wrong physical frames), and its 040 fault classifier
(`amix-040-060-port` `src/getfault040.s`, called from the `wb040.s` path) `PTEST`s to decide
how to resolve a fault (fabricated MMUSR → wrong resolution).

**Fix:** restore upstream's three lines. Model-guarded, so the fake path still runs verbatim
for every `mmu_model == 0` configuration — `UAE_040`, `UAEJIT_040` and every 68000–68030
non-MMU table are untouched — and the 68030 MMU path never enters this function at all
(`cpuemu_32` calls `mmu_op30()`), so `UAE_030_MMU` is unaffected.

**Host guard** `test_mmu040_pflush_ptest` is shaped like the bug: map a VA, translate it to
populate the ATC, repoint the page descriptor *behind the ATC's back* and prove the stale
entry still wins (which is what makes the ATC real rather than a pass-through), then execute
a genuine `PFLUSHA` through the 040 dispatch table and prove the new mapping takes effect;
finally execute `PTESTR (A0)` and require a resident bit and the frame the walk actually
found. Proven non-vacuous — against the pre-fix `mmu_op()` it produces exactly three
failures:

```
FAIL: PFLUSHA made the updated descriptor take effect: got 0x0a000000 want 0x0b000000
FAIL: PTEST reports the page RESIDENT (a real table walk)
FAIL: PTEST reports the frame the walk found: got 0x00401000 want 0x0b000000
```

Harness 152 → **160**.

##### Hypothesis 4 (URP/SRP without an ATC flush) — closed, correct as-is

The suspicion was that `MOVEC` to URP/SRP needs an implicit flush in an emulator whose ATC
caches translations, even though silicon does not flush. It does not, and the reasoning is
worth keeping because it is the kind of thing that gets "fixed" wrongly later.

A 68040 ATC entry is tagged by **address + supervisor bit** (`mmu_tagmask`) — there is no
ASID and no root-pointer tag. So a URP/SRP write cannot invalidate anything by itself **on
real hardware either**, which is precisely why the architecture makes the OS responsible for
`PFLUSH` after a root-pointer change. This core's ATC model has the same shape, so adding an
implicit flush would *diverge* from hardware and would mask a kernel that omits a required
`PFLUSH`.

The contract, now written at the code site: **`PFLUSH` is the sole ATC coherence point in
this core, exactly as on silicon; URP/SRP writes deliberately do not flush.** That contract
was vacuous while `PFLUSH` was a no-op — which is exactly why hypothesis 4 looked suspicious
from the outside. With `PFLUSH` real it holds, and the URP/SRP site is right as it stands.

##### Remaining ranked hypotheses, if `page_free` survives this

1. **Write-back fabrication** (the bench ISSUE-10 class) — the format-`$7` path of §8.1 is now
   genuinely executing for the first time, with real WB3S/WB3A/WB3D. Top of the list.
2. **The AMIX memory contract** — the single ≤16 MB window at `$08000000` and the `$07000000`
   `a3000mem` window were sized for the 030 kernel's SCN1; a 040 kernel with a different
   `MAINSTORE`/`VSIZOFMEM` would fault differently.
3. **4 KiB first-run paths** — now known to be 4096, so `mmu_pagesize_8k` is *not* implicated;
   what is newly exercised is every 4 K masking path in the walk, which §6 covered on the host
   but which had never run on metal until `D910C41A`.
4. **Cache model** (§7.2) — the 040 copyback/CINV/CPUSH semantics this core does not model at
   all. Unchanged in rank; would show as data-visibility oddities rather than a clean panic.

#### 9. Sixth metal attempt: `page_free` is BOOT-STACK EXHAUSTION (static diagnosis)

`095356E9` enabled the MMU cleanly again and panicked **byte-identically** to
`D910C41A` — same addresses, two images, fully deterministic. So `PFLUSH`/`PTEST` were
real bugs but not this panic's cause. This section is the static read-out of the kernel
binary; no boot was needed for any of it.

**Provenance.** The kernel is the `unix-040` brel in slice 3 of
`amix-installng/work/metal-040-source/metal-040-source-20260819.hdf`. Slice layout is
`boot1 @+0x000 (1 KiB)`, `IBLK @+0x400`, `boot2 @+0x600 (8 KiB)`, `kernel IBLK @+0x2600`,
**kernel ELF @+0x2800** (`0x7f454c46`, big-endian, `EM_68K`, `e_type=0xff00`, one
anonymous `VMA 0` blob of `0x10b930`). It is stripped, but the port repo's
`build/unix-040-z3660` (2026-08-19, 16109 symbols) is the same link with symbols.

**Load base proven, not assumed.** `movec %d0,%tc` sits at `.text+0xd7550` in the
symbolised build, and the serial reported the MMU enable at `PC=080d7550`. So the blob
loads at exactly **`0x08000000`** and kernel VA − `0x08000000` = blob offset. The
enclosing symbol is **`pstart`** (`.text+0xd71a8`, size `0x2d4`).

**What `pstart` programs, in order** (`.text+0xd751a`..`0xd7550`):

```
movec %d0,%srp / %urp      root pointers
movel #0x003FC000,%d0 -> %itt0      instr, 0x00000000-0x3FFFFFFF, S-field=both
clrl  %d0             -> %itt1      disabled
movel #0x003FC060,%d0 -> %dtt0      data,  0x00000000-0x3FFFFFFF, both, noncacheable
movel #0x807FA060,%d0 -> %dtt1      data,  0x80000000-0xFFFFFFFF, SUPERVISOR only, noncacheable
cpusha bc ; pflusha
movel #0x00008000,%d0 -> %tc        E=1, 4 KiB pages
```

Every one of those seven writes was inert before §8.6/§8.8.

**The panic is stack exhaustion.** `.data` begins at VA `0x080F45BC` (`.text` is
`0xf45bc`), and its first two symbols are `bugvbr` at `+0x0` and **`pstackt` at `+0x4`**,
with **`pstack` at `+0x1f44`** — an 8000-byte boot stack growing down from
`0x080F6500` to a limit of `0x080F45C0`. The kernel's own printer is
`kstack 0x%x!` (the `!` is literal in the format string). Both panics:

| | reported `kstack` | bytes used of 8000 | left above `pstackt` |
|---|---|---:|---:|
| `page_sub` (D910C41A) | `0x080F47F8` | 7432 | 568 |
| `page_free` (095356E9) | `0x080F4874` | 7308 | 692 |

Both sit ~92% down an 8 KiB stack. `page_sub` and `page_free` are neighbours in the
page allocator; they are **where the runaway happened to be standing**, not the bug.
The `Backtrace:` value (`0x080F48E0`) is likewise inside `pstack`, and in the first
panic it was **odd** (`0x080F485B`) — a corrupted frame, which is why the follow-on
double fault was an Address Error.

**Mechanism.** A deterministic way to eat a boot stack in VM init is nested unresolved
faults: a 68040 access-error frame is 60 bytes by itself, so ~7300 bytes is on the order
of 40–50 nested faults. Deterministic to the byte across two images fits a systematic
bad translation far better than the probabilistic write-back class.

**What the TTRs rule in and out.** `ITT0`/`DTT0` cover `0x00000000-0x3FFFFFFF`
transparently, so the kernel image at `0x08000000` **and the Kickstart ROM at
`0x00F80000` need no page tables at all** — the double-fault `pc = 0xF80B4C` is
therefore a legitimately executable address here, not evidence of a wild jump through
unmapped memory. `DTT1` covers `0x80000000+` for supervisor data. What is left to the
real table walk is **`0x40000000-0x7FFFFFFF`** — and the first panic's wild
`pc = 0x40080000` sits exactly there. So the fault chain lives in the page-table-walked
kernel window, not in the transparent ones.

**TTR decoding verified against these exact values, and it is correct.**
`mmu_do_match_ttr()` was checked line by line against the three live TTRs: base/mask
(`msb = ((addr ^ ttr) & 0xff000000) >> 24`, `mask = (ttr & 0x00ff0000) >> 16`,
match iff `!(msb & ~mask)`) gives `0x00000000-0x3FFFFFFF` for `0x003FC000` and
`0x80000000-0xFFFFFFFF` for `0x807FA060`; the S-field logic (bit 14 set = match both,
else bit 13 selects supervisor/user) gives "both" for `ITT0`/`DTT0` and
"supervisor only" for `DTT1`, which is what the encodings say. **The TTR class is
eliminated.**

**What static analysis cannot decide:** *which* fault repeats. That is a runtime fact,
and guessing it is how the earlier metal cycles were spent.

#### 10. Bounded exception trace (shipped instrumentation, not a fix)

`amix_xtrace()` in `Exception_normal()` prints the first 64 non-interrupt exceptions
once the AMIX 040 MMU is up:

```
[XT01] vec=2  ipc=080D8A12 pc=080D8A12 fa=40080000 ssw=0405 sr=2708 a7=080F6480
```

Self-arming (`amix_mmu_on` + `mmu_model == 68040`), self-closing after 64 lines, skips
interrupt vectors 24–31 because ISRs fire throughout and are not the fault chain, and
costs one comparison per exception thereafter. `a7` is included deliberately: a
repeating unresolved fault appears as the same `vec`/`fa` with `a7` stepping down toward
`0x080F45C0`, which converts "the stack was exhausted" into "*this* address faulted N
times and never resolved". 68030 and non-AMIX 040 boots are untouched by the
`mmu_model` gate.

One boot of this image answers, in order: is it one address or many; is it a read or a
write (`ssw` RW bit); does the kernel's handler run at all between faults (`ipc` moving);
and how many faults fit before `pstack` runs out.

#### 11. The trace came back with one line, and it falsified the prediction

`F4782D71` produced the entire trace in a single entry, ~3 s after MMU-on:

```
[XT01] vec=3  ipc=00F80B4C pc=00F80B4C fa=00000000 ssw=0000 sr=2700 a7=080F48C0
```

No `vec=2` chain, ever. The pre-registered prediction (a repeating access fault with
`a7` marching down) is **dead**. What it establishes instead:

- The **first non-interrupt exception after MMU-on is the Address Error itself**, at the
  exact pc both panic screens reported.
- `a7 = 0x080F48C0` at that first entry: `pstack` was **already ~92% consumed**
  (7232 of 8000 bytes) *before any traced exception*. So the consumer is either ordinary
  execution or the one thing the tracer deliberately skipped — **interrupt vectors 24–31**.
- The §8.9 stack-exhaustion reading survives; only the cause of the consumption is open.

**Why vec 3 at an even pc — the code path, not a guess.** `Exception_normal()` fetches
the handler with `newpc = x_get_long(regs.vbr + 4 * vector_nr)` and then:

```c
if (newpc & 1) {
        ...
        } else {
                exception_check_trace(nr);
                exception3_notinstruction(regs.ir, newpc);   /* raise vector 3 */
        }
```

So **an odd entry in the vector table raises exception 3** while the pc is still wherever
it was. That makes `vec=3` at an even `pc` entirely consistent — and it means the
interesting number is the *odd value that was fetched*, which the first tracer did not
print. If the exception whose vector was odd happened to be an interrupt, the first
tracer skipped it and only its consequence appeared. That single filter can account for
the whole one-line trace.

**Level 7 is level-triggered here.** `check_uae_int_request()` gates on
`if (eff_irq > regs.intmask || eff_irq == 7)`. On real silicon IPL7 is *edge*-sensitive —
taken on the transition into 7 — but this test re-fires on every poll while the lines
read 7, regardless of `regs.intmask`. A stuck or mis-read IPL7 would therefore be taken
over and over, each one stacking a frame, consuming the boot stack **with nothing visible
to the old tracer**. `sr=2700` at the fault (intmask 7) is consistent with a level-7
interrupt having been taken. This code is shared with the metal-proven 030 path, so it is
**not** being changed on suspicion — the trace has to say whether interrupts are actually
storming, and at which level.

#### 12. Extended trace (second instrumentation build) — what each field decides

| field | question it settles |
|---|---|
| `[XI..]` lines + `[XI-SUM]` per-level counts | Is anything storming, and at which level? Silence here means ordinary execution ate the stack. |
| `minSP` per level, and global | Did interrupts drive the stack down, or was it already low when they arrived? |
| `a7` on the **MMU-enable** line | Was `pstack` already ~7 KB deep at MMU-on? Splits "consumed after MMU-on" from "arrived deep", which nothing so far can distinguish. |
| `vbr` on every line | Is the kernel's vector table where it should be, or still ROM/chip-RAM era? |
| `vec@` (the fetched handler) on interrupts | Directly exposes an odd vector entry — the thing that raises vec 3. |
| `fault=` / `vec3@` on vec 2/3 | The odd address itself (`last_fault_for_exception_3`), and the vector-3 entry. |

Interrupts are counted rather than printed after the first 12, with a periodic summary
capped at 8 lines, so a storm is unmissable but cannot flood the serial. The
`mmu_model == 68040` gate keeps 030 and non-AMIX 040 boots untouched.

Three outcomes and what each means, written down before the boot so the next reading is
not free to drift:

1. **`[XI-SUM]` shows a large count at one level with `minSP` marching down** → interrupt
   storm; the source and the level-7 edge-trigger question above become the fix target,
   with evidence.
2. **`[XI-SUM]` shows few or no interrupts, and the MMU-enable line already shows
   `a7 ≈ 0x080F48C0`** → the stack arrived deep; the consumer is upstream of MMU-on and
   this is a kernel/port-lane question about a 7 KB call chain against an 8000-byte
   `pstack`, not an emulator bug.
3. **Few interrupts, `a7` high at MMU-on, and the stack falls between** → ordinary but
   pathologically deep execution after MMU-on; the `[XT..]` sequence plus `vbr` should
   then show what the kernel is doing.

#### 13. The fault named: AmigaOS exec's trap dispatcher, reading a dead ExecBase

`F87F6FA7` settled the open questions and moved the case out of the emulator.

```
68040 MMU enabled. TC=8000 Page size = 4096 amix_mmu_on=1 PC=080d7550 a7=080f49e8 vbr=08001378
[XI-SUM] at first non-interrupt exception total=0 minSP=080F48C0
[XT01] vec=3  ipc=00F80B4C pc=00F80B4C fa=00000000 ssw=0000 sr=2700 a7=080F48C0 vbr=08001378
[XT01]   fault=FFFFFFFE addr=00F80B4C vec3@=080DA660
```

**Outcome 2 of the three pre-registered readings.** `total=0`: no interrupts at all, so the
storm hypothesis is dead and the level-7 edge-trigger question in §8.11 is moot — that code
was never entered, and not changing it on suspicion was the right call. `a7 = 0x080F49E8`
**at MMU-on**: `pstack` was already 6936 of 8000 bytes deep before the MMU was even enabled,
and only 296 further bytes were consumed before the fault. **The deep stack is ambient state,
not the failure.** §8.9's exhaustion reading was the cascade — the vec-3 handler plus panic
machinery landing on an already-full stack — not the primary event.

##### What is at 0x00F80B4C (KS 2.04 r37.175, ROM base 0x00F80000, file offset 0x0B4C)

```
f80ae2:  subil #0x00F80ABA,%sp@   ; recover the vector index from the bsr.s return address
f80ae8:  lsrw  %sp@(2)
f80aec:  btst  #5,%sp@(4)         ; supervisor?
f80af2:  beqs  f80b38
f80af4:  braw  f83a04
f80af8..f80b36:  ~32 x `bsr.s f80ae2`     <- the per-vector stub table
f80b38:  moveml %a0-%a1,%sp@-
f80b3c:  moveal 4,%a0             ; ExecBase   <- ABSOLUTE ADDRESS 4
f80b40:  moveal %a0@(276),%a0     ; ExecBase->ThisTask   (offset 0x114)
f80b44:  movel  %a0@(50),%sp@(4)  ; Task->tc_TrapCode    (offset 0x32) overwrites saved a1
f80b4a:  moveal %sp@+,%a0
f80b4c:  rts                      ; <- jumps to tc_TrapCode
```

This is **AmigaOS exec's CPU-exception dispatcher**: a table of `bsr.s` stubs whose return
address encodes the vector number, and a tail that forwards to the current task's
`tc_TrapCode`. The instruction that took `-2` is the **`rts` at 0xF80B4C**, and the `-2` came
from `ExecBase->ThisTask->tc_TrapCode` — a pointer chain rooted at **absolute address 4**.

Under AMIX that chain is dead. The kernel has taken the machine over and owns low memory
(the emulator's own warm-reboot guard exists because "AMIX has cleared the overlay so
`$0-$8` is chip RAM"), so `4` no longer holds a valid ExecBase and the walk lands on
whatever is there now. `0xFFFFFFFE` is simply what came back.

##### How execution got there — and the elimination that decides attribution

**Not by an exception.** The kernel's VBR is its own table: `M68Kvec` at `.text+0x1378`,
which is exactly the observed `vbr = 0x08001378`. Its relocations are **entirely kernel
symbols** — `_start` at vector 1, `nullvect` throughout, `fpsp_vec11` at vector 11 — with
**not one ROM address in the table**. The trace confirms it independently: zero interrupts,
and the vec 3 *is* the first non-interrupt exception. So no CPU exception could have
vectored into ROM, and none did. Control was transferred there **through a pointer** —
a call or jump — which is candidate world (b), with world (c) describing what then happened
*inside* the ROM routine.

(Correction to §8.9: the MMU-enable code is **not** inside `pstart`. `pstart` is
`.text+0xd71a8` size `0x2d4`, ending at `0xd747c`; the TC write at `0xd7550` lies in the
local region after it, nearest label `Lkroot` (`.text+0xd74be`), before `sysseginit`
(`.text+0xd7628`). The load-base derivation is unaffected.)

##### Verdict: the emulator is exonerated on this evidence — port-lane case

Everything the firmware is responsible for behaved correctly at the moment of failure:

- the MMU is on with the kernel's own TC/TTRs, and `PFLUSH`/`PTEST` are real (§8.6, §8.8);
- exactly **one** exception was delivered, correctly, with the right frame format for
  vector 3, to the kernel's own sane handler (`vec3@ = 0x080DA660`), which panicked as
  designed;
- no interrupt storm, no stale ATC, no fabricated MMUSR;
- the deep boot stack predates MMU-on and is the kernel's own budget.

The failing act is a control transfer into an AmigaOS ROM service whose backing structures
AMIX has already destroyed. That is a kernel/handover question, and per the standing rule it
belongs to the port lane rather than to this repo.

**The single fact that finishes it** is the *source* of that transfer — which kernel pointer
still held an AmigaOS-era value ~296 stack-bytes after the TC write. That is a runtime fact
the kernel binary cannot supply, so §8.14 ships a probe for it and nothing else.

**Contract option, proposed not implemented.** The 030-shaped AMIX memory contract (one
≤16 MB window at `$08000000`, the vestigial `a3000mem` at `$07000000`) remains a *possible
trigger* if the 040 kernel's `MAINSTORE`/`VSIZOFMEM` expectations differ and it bailed into
an AmigaOS error path. Changing that contract firmware-side is a real option but it is also
the single most load-bearing thing the proven 030 boot depends on, so it should not move
without a decision and a 030 regression plan. The `[ROMENT]` probe distinguishes the two
worlds cheaply: an error/bail path shows a source pc inside the kernel's probe or panic
code, a stale pointer shows one somewhere unrelated.

#### 14. ROM-entry probe (third instrumentation build)

One `if` per instruction in `m68k_run_mmu040()`, AMIX-040 only, silent after four hits:

```
[ROMENT1] pc=00F80B38 from=080Dxxxx a7=080F48C8 sp@=080Dxxxx sp@4=xxxxxxxx execbase@4=xxxxxxxx
```

`from` is the last non-ROM pc — **the kernel instruction that transferred control** — and
`sp@`/`sp@4` are the top of the stack at entry, which for a `jsr`/`bsr` is the pushed return
address, naming the call site. `execbase@4` shows what AMIX has left at absolute address 4,
which is what the ROM routine is about to walk. Four hits is enough to see whether this is a
one-off or a pattern, and the probe cannot flood.

That single line names the caller and closes the attribution question in one 3-minute boot.

## 9. THE PORT-LANE CASE: `page_free` on the 68040 kernel (handover)

**Self-contained.** Everything needed to pick this up is in this section; nothing else in
this document is required reading. Written for `amix-040-060-port`.

**Bottom line: the ROM/ExecBase chain everyone (including me) chased is the DOUBLE PANIC —
the kernel's own panic path failing on the way out. The primary bug is whatever makes
`page_free` panic, and it is not visible to any emulator-side exception trace because it is
a software panic, not a CPU exception.** The kernel said so on the screen all along:
`PANIC: page_free` first, `DOUBLE PANIC` second.

### 9.1 The full mechanism chain

Every address below is from the shipped `unix-040` brel (load base `0x08000000`, proven by
`movec %d0,%tc` at `.text+0xd7550` matching the serial's `PC=080d7550`), symbolised against
`build/unix-040-z3660`, and from the card's own `A3k204r37.rom` (KS 2.04 r37.175, base
`0x00F80000`).

1. **Primary failure — unknown.** Something in early VM init panics with `page_free`. This is
   a *software* panic: `panic()` is called, no CPU exception is taken. That is precisely why
   the firmware exception trace showed **zero** exceptions before the crash
   (`[XI-SUM] total=0`, and the vec-3 entry was the *first* non-interrupt exception).
2. **`panic()` → `xpanic()` → `sync()`.** `panic` is `.text+0x3eb58`; `xpanic` is
   `.text+0x3e668` (size `0x56`) and carries a relocation **`0x3e6a2 R_68K_32 sync`**. So the
   panic path flushes filesystems on its way out — standard SVR4.
3. **`sync()` walks the vfs switch through a NULL pointer.** `sync` is `.text+0x5d21a`:

   ```
   5d222:  moveq  #1,%d2                  ; i = 1   <- starts at ONE
   5d224:  cmpl   nfstype,%d2
   5d22a:  bgew   5d260                   ; done
   5d22e:  lea    vfssw,%a2
   5d236:  asll   #4,%d0                  ; i * 16  (sizeof struct vfssw)
   5d238:  moveal %a2@(8,%d0:l),%a0       ; a0 = vfssw[i].vsw_vfsops   (+8)
   5d24a:  moveal %a0@(16),%a0            ; a0 = vfsops->vfs_sync      (+16)
   5d24e:  jsr    %a0@                    ; <<< the transfer, 2 bytes (0x4E90)
   ```

   `vfssw` is `.data+0x913c`, `0xc0` bytes = 12 entries x 16. Its relocations show the
   initialisation policy exactly:

   | entry | `+0` `vsw_name` | `+4` `vsw_init` | `+8` `vsw_vfsops` |
   |---|---|---|---|
   | 0 | `LC%0` | — | **`vfs_strayops` (static)** |
   | 1 | `LC%1` | `s5init` | *(none — runtime)* |
   | 2 | `LC%2` | `ufsinit` | *(none — runtime)* |
   | 3.. | `LC%n` | `fifoinit`, `rf_init`, `xnaminit`, `bfinit`, `prinit`, `specinit`, `nameinit`, … | *(none — runtime)* |

   `vsw_vfsops` is populated at runtime by each filesystem's `vsw_init()`, i.e. by
   `vfsinit()`. **The only statically populated entry is index 0 — and `sync()`'s loop starts
   at index 1.** Panicking before `vfsinit()` has run therefore guarantees
   `vsw_vfsops == NULL`.
4. **NULL + 16 lands on the low-memory vector table.** `moveal %a0@(16),%a0` with `a0 = 0`
   reads absolute address **`0x10`** — which is **CPU exception vector 4 (Illegal
   Instruction)** in the vector table AmigaOS left in low memory.
5. **The fetched value is an AmigaOS ROM stub, and it self-identifies as vector 4.** The
   captured value is `0x00F80A62`. In the ROM, exec's per-vector stub table is a run of
   `bsr.s` entries whose common tail recovers the vector index from the pushed return
   address: `subil #0x00F80A5C,%sp@ ; lsrw %sp@(2)`. For the stub at `0xF80A62` the pushed
   return address is `0xF80A64`, giving `(0xF80A64 - 0xF80A5C) >> 1 = 4`.

   > **Two fully independent derivations agree on vector 4** — the offset from NULL
   > (`16 / 4 = 4`) and the ROM dispatcher's own arithmetic. That is the proof that the
   > pointer came from low-memory vector 4 via a NULL `vsw_vfsops`, and not from a saved
   > vector the kernel deliberately kept.

6. **exec's dispatcher walks AMIX's pseudo-ExecBase and finds a sentinel.** Control reaches
   the tail at `0xF80B38`:

   ```
   f80b38:  moveml %a0-%a1,%sp@-
   f80b3c:  moveal 4,%a0             ; ExecBase  <- absolute address 4  (= 0x080007F0)
   f80b40:  moveal %a0@(276),%a0     ; ExecBase->ThisTask   (0x114)
   f80b44:  movel  %a0@(50),%sp@(4)  ; Task->tc_TrapCode    (0x32)  -> overwrites saved a1
   f80b4a:  moveal %sp@+,%a0
   f80b4c:  rts                      ; jumps to tc_TrapCode == 0xFFFFFFFE
   ```

   `execbase@4 = 0x080007F0` is a **kernel** address: AMIX installs its own pseudo-ExecBase.
   The walk through it yields `0xFFFFFFFE`, evidently a "no trap code" sentinel.
7. **`rts` to `-2` → odd → Address Error (vector 3)** → the kernel's own, correct vec-3
   handler (`vec3@ = 0x080DA660`) → **`DOUBLE PANIC ... vector=0x3`**.

Captured in one line by the firmware probe:

```
[ROMENT1] pc=00F80A62 from=0805D24E a7=080F48C8 sp@=0805D250 sp@4=00000000 execbase@4=080007F0
```

`from = 0x0805D24E` is `sync+0x34`, `sp@ = from+2` confirms the 2-byte `jsr (a0)`.

### 9.2 Why the bench boots green and the card does not

**The cascade is not environment-dependent.** Steps 2–7 are pure kernel code plus the KS
ROM, with no dependence on the strap, the SCSI path, the emulator or the memory map. If
anything panicked this early under Amiberry, the identical chain would fire there — the
bench uses the same KS 2.04, and AmigaOS leaves the same vector 4 in low memory.

**The bench boots green because `page_free` never panics there.** The divergence is entirely
in **step 1**, the primary failure. So:

> **The ROM, ExecBase, `tc_TrapCode` and the address error are a dead end for
> root-causing. Do not spend time on them.** They are a faithful report that the kernel
> panicked before `vfsinit()`.

That relocates the question to: *what differs about the card that makes the 040 kernel's VM
init reach a `page_free` panic?* The standing candidate is the memory geometry — see §9.4.

### 9.3 What the firmware has been cleared of

Stated so the port lane does not re-litigate it. At the moment of failure, on image
`8C51370A`, all of the following were verified correct, several of them fixed during this
investigation and each covered by a host-harness guard:

| firmware behaviour | status |
|---|---|
| 68040 MMU enable (`MOVEC`->TC) | fixed and verified; `TC=8000`, 4 KiB pages, latch on |
| Transparent translation registers (ITT0/ITT1/DTT0/DTT1) | fixed (`mmu_tt_modified`); decoding verified against the kernel's exact values |
| `PFLUSH` / `PFLUSHA` / `PTEST` | fixed (were a no-op and a fake); harness-guarded |
| 68040 access-error frame (format `$7`) | implemented with real SSW/FA/WB slots |
| `STOP` pc advance | fixed |
| Exception delivery at the failure | exactly one, correct frame format for vector 3, into the kernel's own handler |
| Interrupts | **zero** between MMU-on and the fault — no storm, no stale ATC, no fabricated MMUSR |
| Boot stack | `a7 = 0x080F49E8` at MMU-on: `pstack` was already 6936/8000 deep *before* the MMU came up. Ambient, not caused by anything the emulator did. |

### 9.4 Candidate kernel-side fixes, and the one firmware option

**A. Make the panic path survivable (defensive, small, independently valuable).**
`sync()` dereferences `vsw_vfsops` without a NULL check, so *any* panic before `vfsinit()`
turns into a double panic that destroys the diagnostic. A null guard in `sync()`'s loop — or
having `xpanic()` skip `sync()` when the vfs switch is not yet initialised — would let the
original `page_free` panic print cleanly with its backtrace intact. **This alone would have
made the whole ROM investigation unnecessary, and it will pay for itself the next time
anything panics early.** It does not fix the boot.

**B. Find the primary `page_free` panic (the actual bug).** This is kernel-side and needs the
kernel's own view, which no emulator trace can supply. The cheapest instrument is the panic
site itself: `page_free` is at relocation `0xaf9ea` and `page_abort` at `0xaf8d6` (per the
port repo's own `docs/contracts/CB-PAGE-LIFECYCLE-CLOSURE.md` census), with `panic` at
`.text+0x3eb58` and `xpanic` at `.text+0x3e668`. **What to capture: the panic format string
and its arguments, plus the caller of `page_free`.** With fix A in place the existing panic
printer supplies both.

**C. Memory geometry — the standing hypothesis for step 1.** The AMIX memory contract this
firmware presents was shaped for the **68030** kernel: exactly one <=16 MB Fast-RAM window at
`$08000000`, plus a vestigial `a3000mem` window at `$07000000`. The 040 kernel's
`MAINSTORE`/`VSIZOFMEM` expectations may differ, and a page-allocator panic during VM init is
consistent with a geometry the kernel did not expect. The port lane can check this without
the card: compare the 040 kernel's memory probe and `MAINSTORE`/`VSIZOFMEM` derivation against
what the bench (Amiberry) presents versus the contract in §8.

> **Firmware-side contract change: PROPOSED, NOT IMPLEMENTED.** Widening or reshaping that
> window is a real option and lives in this repo (`uae_emulator.cpp`, the `amix_mode` memory
> map). It is deliberately not done, because that single window is the most load-bearing
> thing the metal-proven 030 boot depends on — the 030 kernel panics `vatosde()` if it sees a
> region outside SCN1, and two adjacent banks coalescing is equally fatal. It must not move
> without an explicit decision and a 030 regression plan. If the port lane's analysis shows
> the 040 kernel needs a different geometry, say what it needs and it can be built and
> A/B'd against the 030 golden.

### 9.6 Candidate C (memory geometry): EXONERATED, with a latent landmine found

Checked statically, three ways. **C is not the cause** — but the pass turned up a real
hazard that the port lane should fix anyway, and it narrowed `page_free` to three named
assertions.

#### The three memory maps

| | chip | main Fast RAM (in the AmigaOS memlist) | other |
|---|---|---|---|
| **Bench** (`i10t-bench-040.uae`) | `chipmem_size=4` | **`a3000mem_size=16` → 16 MB at `0x07000000`** | `fastmem_size=0`, `z3mem_size=0`, `mbresmem_size=0` |
| **Card** (`uae_emulator.cpp`, `amix_mode`) | real A4000 chip | **16 MB at `0x08000000`** (`drct_bank`, added by ext-kickstart autoconfig) | vestigial 16 MB at `0x07000000` (`a3000mem_bank`) **not in the memlist**; `dmmy_bank` above `0x09000000` |

The bench base is confirmed from Amiberry's own code, not inferred:
`src/memory.cpp:2806` sets `a3000lmem_bank.start = 0x08000000 - a3000lmem_bank.reserved_size`,
so 16 MB of `a3000mem` lands at exactly `0x07000000`–`0x07FFFFFF`.

**So the kernel's main RAM is at a different base in the two environments — `0x07000000`
on the bench, `0x08000000` on the card.** That is the one substantive difference, and it is
exactly the kind of thing that could drive a page-database bug. It does not, and here is why.

#### How the 040 kernel actually derives MAINSTORE/VSIZOFMEM

`config()` is `.text+0x18f5c`. It seeds `MAINSTORE = end` (the linker symbol) and
`VSIZOFMEM = 0`, then runs a fixpoint over `bootinfo.memory[]` (16 records of 32 bytes,
`start` at `+0x14`, `end` at `+0x18`), repeating while anything changed:

```
19178:  movel %a0@(0x14,%d0:l),%d1     ; rec.start
1917c:  movel %a0@(0x18,%d0:l),%d2     ; rec.end
19180:  cmpl  MAINSTORE,%d1 / bcc skip ; skip if rec.start >= MAINSTORE
1918a:  cmpl  MAINSTORE,%d2 / bcs skip ; skip if rec.end   <  MAINSTORE
19196:  movel %d1,MAINSTORE            ; LOWER MAINSTORE through a record containing it
1919c:  MAINSTORE + VSIZOFMEM -> %d0   ; the current top
191a8:  cmpl %d1,%d0 / bcs skip        ; skip if top <  rec.start
191ae:  cmpl %d2,%d0 / bcc skip        ; skip if top >= rec.end
191b6:  subl MAINSTORE,%d2 / movel %d2,VSIZOFMEM  ; EXTEND through a record containing the top
```

This is exactly the algorithm `amix-kernel-analysis/vm-map/RAM-BEYOND-16MB-ANALYSIS.md`
describes. Walk it for both environments, with the single memlist record each presents:

- **Card**, record `[0x08000000, 0x09000000)`, `end ≈ 0x0810B930`: the record contains
  `MAINSTORE`, so `MAINSTORE` is lowered to `0x08000000`; the top `0x08000000` lies inside
  the record, so `VSIZOFMEM = 0x09000000 - 0x08000000 = 16 MB`. **Correct.**
- **Bench**, record `[0x07000000, 0x08000000)`, `end ≈ 0x0710B930`: same two rules give
  `MAINSTORE = 0x07000000`, `VSIZOFMEM = 16 MB`. **Correct.**

Chip RAM never lowers `MAINSTORE` (its `end` is below it, so the second test skips it) and
never extends the top. `maxclick = btopr(MAINSTORE) + physmem` therefore covers
`[0, 0x09000000)` on the card and `[0, 0x08000000)` on the bench — in both cases **every page
of real RAM is inside the page-frame database**. There is no geometry-driven out-of-range
page pointer, and the different base is absorbed correctly by the derivation.

**Candidate C is exonerated as the cause of the panic.**

#### The landmine: the fallback path is `0x07000000`-shaped and silently wrong at `0x08000000`

Immediately after the fixpoint, `config()` has a fallback that runs **only if `VSIZOFMEM` is
still zero** (i.e. `bootinfo.memory[]` yielded nothing usable):

```
191d2:  tstl  VSIZOFMEM / bne  skip     ; only when nothing was derived
191dc:  movel #0x07000000,%d5           ; hardcoded
191e2:  cmpil #end,%d5 / bcc skip       ; only if 0x07000000 < end
191ec:  movel #end,%d5
191f2:  andil #0xF7C00000,%d5           ; round down -- mask CLEARS bit 27 (0x08000000)
191f8:  movel %d5,MAINSTORE
191fe:  movel #0x08000000,%d5
19204:  subl  MAINSTORE,%d5
1920a:  movel %d5,VSIZOFMEM             ; VSIZOFMEM = 0x08000000 - MAINSTORE
```

This is the `QUICK_KLUDGE`, and it encodes "the kernel lives in the A3000 motherboard-RAM
window `[0x07000000, 0x08000000)`". The mask `0xF7C00000` keeps bits 31–28 and 26–22 and
**clears bit 27** — the `0x08000000` bit. Consequently:

| kernel load base | `end` | `MAINSTORE = end & 0xF7C00000` | `VSIZOFMEM = 0x08000000 - MAINSTORE` |
|---|---|---|---|
| `0x07000000` (bench) | `0x0710B930` | `0x07000000` ✔ | 16 MB ✔ |
| **`0x08000000` (card)** | `0x0810B930` | **`0x00000000`** �’ | **`0x08000000` = 128 MB** ✗ |

On the card the fallback would declare main memory to be 128 MB starting at zero, sizing the
page database for `[0, 0x08000000)` — which **excludes every byte of the card's actual RAM at
`0x08000000`+**, and would make exactly the `pages`/`epages` assertion below fail on the
first free. It is not what is happening today (the fixpoint succeeds, so the fallback is
skipped), but it is a live trap: **any boot where `bootinfo.memory[]` arrives empty or
unusable on this card fails in precisely the observed way.** Worth fixing in the port lane
independently — the mask and the `0x08000000` terminus both need to come from the memlist
rather than from a 1991 assumption.

#### What `page_free` actually asserts (narrowing candidate B)

`page_free` is `.text+0xaf9ea`. Its prologue is three assertions, all calling **`assfail`**
(SVR4's assertion handler) — so the screen's `PANIC: page_free` is an *assertion failure*,
not a hand-written panic:

| site | test | line | meaning |
|---|---|---|---|
| `0xafa0a` | `pp >= pages && pp < epages` | 622 | the page-struct pointer is inside the page-frame database |
| `0xafa2c` | `btst #5, pp@(0)` clear | 623 | a `p_flags` bit must be clear (free/locked class) |
| `0xafa4e` | `pp@(56) == 0` | 624 | a field at `+56` must be zero |

`page_abort` (`.text+0xaf8d6`) opens with the identical `pages`/`epages` check. **Which of
the three fired is not determinable from the panic text**, and that is the remaining gap.

### 9.7 What candidate B needs that statics cannot supply

With C exonerated, the case rests entirely on the port lane, and the missing facts are all
runtime:

1. **Which assertion fired** — line 622, 623 or 624 (`assfail` prints file and line, so the
   existing printer already carries it).
2. **The offending `pp` value**, and whether it is outside `[pages, epages)` or merely in a
   bad state.
3. **The caller of `page_free`** — the kernel's `Backtrace:` already attempts this.

All three are in the panic output that **candidate A unblocks**: today `assfail` → `panic` →
`xpanic` → `sync()` dereferences a NULL `vsw_vfsops` and double-faults before the message is
usable. A NULL guard in `sync()`'s loop turns this single boot into all three answers at
once. That remains the recommended first move, and nothing further is needed from this repo
to get it.

**No firmware change is proposed.** The contract variant floated in §9.4C is withdrawn as a
fix for this panic — the geometry is correct on both sides — and should not be built. If the
port lane later wants the card's RAM at `0x07000000` to match the bench bit-for-bit (which
would also sidestep the landmine above), that is a separate, deliberate decision with a 030
regression plan, not a fix for `page_free`.

### 9.5 Corrections to the record

Kept because the wrong turns are informative:

- **§8.9 called the stack exhaustion the mechanism.** It is not; it is ambient. `pstack` was
  already 6936 of 8000 bytes deep *at MMU-on*, and only 296 further bytes were used before
  the fault. `page_sub`/`page_free` are neighbours in the page allocator and were simply
  where the panic fired.
- **§8.13 said control was transferred "through a pointer", implying a wild one.** Correct
  but incomplete: the pointer is a *specific, explainable* NULL-plus-offset — `vsw_vfsops`
  is NULL and `+16` lands on low-memory vector 4. Not garbage, and not a saved vector the
  kernel keeps on purpose.
- **The pre-registered "repeating access fault with `a7` marching down" prediction was
  falsified**, which is what made the software-panic reading findable at all.

### 9.8 RESOLVED: `page_free` was ISSUE-48 (unzeroed page database) — kernel-side, confirmed

**The §9 case is closed and the diagnosis held.** The port lane's fix build confirmed
**ISSUE-48 — the page database was unzeroed DRAM** — on metal, with live counters:
`calls=1 npages=3688 dirty=3620 **held=150**`. The `page_free` panic is **gone**
(`syncg calls=0`), and the kernel now runs for **minutes** rather than seconds.

That vindicates the §9.6/§9.7 conclusion on both counts: the geometry (candidate C) was
correctly derived and was never the problem, and the failure was one of the three `assfail`
assertions in `page_free` — a page-struct pointer that was garbage because the database it
indexes had never been zeroed. It also retroactively explains why the fault was byte-for-byte
deterministic: unzeroed DRAM after a cold ARM boot is deterministic.

Candidate A (the `sync()` NULL guard) remains worth landing on its own merits — it is what
turned the double panic into a readable one — but it is no longer on the critical path.

## 10. Post-ISSUE-48: three fronts

With the kernel running for minutes, three new failures surfaced. Two of them share a single
firmware root cause; the third is firmware-side but not yet fixed.

### 10.1 Fronts 2 and 3: one root cause — a guest reset did not clear the MMU state

**Third instance of the pattern that produced the `MOVEC`->TC and `mmu_op` bugs: the register
is stored, the side-effect function is never called.**

`m68k_reset_newcpu()` zeroes `regs.itt0..dtt1`, `tcr`, `mmusr`, `srp`, `urp` — but on the
68040 path `regs.mmu_enabled`, the page geometry (`mmu_pagesize_8k` and the masks/shifts) and
the `mmu_ttr_enabled`/`_ins`/`_data` flags live **outside `regs`** and are only recomputed by
`mmu_set_tc()` / `mmu_tt_modified()`. The ATC was not flushed either. So a guest warm reset
carried the previous session's MMU state into the Kickstart ROM that ran next:

- `mmu_ttr_enabled_ins` stays true while `itt0` is now zero → `mmu_match_ttr_ins()` finds no
  match where ITT0 would have covered the address;
- `regs.mmu_enabled` stays true → the accessor walks the tables anyway;
- `srp`/`urp` are now zero → the walk reads descriptors from address 0.

**Front 2 is exactly this signature.** `[XT01] vec=2 ipc=0800ABEA fa=0800ABEA ssw=0546` is a
supervisor **instruction** fetch (TM=6) taking an ATC fault on kernel text that ITT0 covers,
and it is address-specific because the stale ATC still held valid entries for the handlers
that *did* fetch fine in the same window (`080DA860`, `08001068`). **Front 3** — cycle 1 runs
2:20, cycles 2 and 3 wedge in early KS before the strap — is the same stale state breaking the
post-reset ROM.

Both front-2 sub-questions answered directly:

- **(a) Does the mmu040 ifetch path consult ITT0?** Yes, correctly. `get_iword_mmu040` gates
  on `(!mmu_ttr_enabled_ins || mmu_match_ttr_ins(addr, regs.s != 0) == TTR_NO_MATCH) &&
  regs.mmu_enabled`. The fault cannot come from the ifetch path ignoring TTRs.
- **(b) Does the kernel clear its TTRs post-`sysseginit`?** No. The **only** TTR-clear site in
  the whole image is `Lhs_mmu040` (`.text+0xda012`) — `moveq #0` into TC and all four TTRs,
  then `pflusha` — which is the **haltsys** path, i.e. shutdown. ITT0 stays armed across normal
  operation, so an ATC fault on ITT0-covered kernel text had to come from stale emulator state.

**Fixed**, `mmu_model`-gated so 030 is untouched (the 030 PMMU keeps its state in `mmu030.*`,
which `mmu030_reset()` already handles). Host guard `test_mmu040_reset_clears_mmu_state` arms a
TTR and the MMU, calls `m68k_reset_newcpu(true)` and requires `regs.mmu_enabled` clear, the TTR
registers clear and all three `mmu_ttr_enabled*` flags recomputed; proven non-vacuous (three
failures against the pre-fix reset). Harness 160 → **167**.

**`amix_mmu_on` reset — the deferred flag, now implemented, with the argument.** It is latched
when the kernel brings its MMU up and was never cleared, so every cycle after the first ran the
ROM's kernel load under AMIX-*runtime* pacing (paced completions, per-access pump suppressed)
instead of the immediate-completion regime the code comments call "the proven kernel-load
behavior". The safety argument is that clearing it on reset makes every cycle **start in the
state cycle 1 starts in** — the metal-proven one — rather than introducing a new regime; the
kernel re-latches it as soon as it enables its MMU again, and the auto-clicker correctly
re-arms for the new KS prompt (still bounded by the session budget added earlier). It is
nonetheless a behaviour change on the proven 030 warm-reset path, and the cheap 030 regression
should run before it is trusted.

### 10.2 Front 1: core1 wedged on the real Amiga bus (mechanism named, NOT fixed)

The clean cycle wedged with core1 executing no guest instructions — no `[PC]` samples under
DEMU — while core0's `C0STAT` kept ticking at ~1.0 M it/s. Core1 has **eleven** unbounded
busy-waits, so the question was which. Two live read-only probes settled it:

1. **The cross-core mailbox class is eliminated.** `DM 0xFFFF0000` len `0xC0` on the wedged box:
   `shared_data`, `write_rtg(+04)`, `uart_semaphore(+1C)`, `reset_emulator(+24)`,
   `write_scsi(+38)`, `read_scsi(+48)`, `read_rtg(+60)`, `write_floppy(+9C)`,
   `read_floppy(+AC)` — **all zero**. (`cfg_emu(+34) = 8` and `debug_emu(+BC) = 1` confirm the
   dump is aligned and live.) Core1 is not waiting on core0.
2. **The reset-line spin is eliminated.** `check_uae_int_request()` has
   `do { ... } while(read_reset==0)`; `read_reset` (map: `0x30610288`) reads **1** — not
   asserted.

What remains is the **real-bus accessor**. `arm_read_amiga_byte()` and its siblings contain
**three unbounded waits per access** — `while(read_reg(REG4)&(1<<4))` (pipe empty),
`wait_write_ack()`, `wait_read_ack()` — between `BUS_REQUEST()` and `BUS_RELINQUISH()`. A
cycle that never completes spins core1 forever with no instruction advance, no `[PC]` samples,
core0 unaffected, and the display still painting (the chipset runs from chip RAM independently).

**Confirmed by measurement.** `ps_tally()` records every physical access, so two samples of
`ps_count`/`ps_lastaddr` (map: `0x3071FC68`/`0x3071FC7C`) nine seconds apart:

```
0x3071FC60  00000000 00000000 001D6BC0 00002F81 00069D7C 00030579 013FB97D 01498BF3
0x3071FC60  00000000 00000000 001D6BC0 00002F81 00069D7C 00030579 013FB97D 01498BF3
                              ^lastaddr                                     ^ps_count
```

**Byte-identical: `ps_count` frozen at `0x01498BF3`, zero physical accesses in nine seconds**,
with the last attempted access at **`ps_lastaddr = 0x001D6BC0` — chip RAM**. Core1 is wedged
inside a chip-RAM bus cycle that never completed.

The correlation worth chasing: the last observable guest action before the wedge was a
**display mode change** (repaint to a striped field at ~23:41–42). A freshly programmed
high-bandwidth display, or a blitter running with BLTPRI, starves the CPU of bus slots — and
these waits have no bound.

**Deliberately not fixed.** Bounding those waits is a change to the hottest path in the
firmware, shared with the metal-proven 030 boot, where a spuriously-firing timeout would break
everything. The zero-risk next step is a **watchdog that diagnoses without changing behaviour**:
after N million iterations in each wait, print once — which wait, the address, `REG4`/`REG5`
status, `nbg_arm` — and then keep spinning. That names the stall (bus never granted vs pipe
never empty vs ack never returned) in one boot without altering a single timing decision.

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
