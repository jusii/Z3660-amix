# Plan: Enable real 68030 MMU in the UAE (WinUAE) emulator core

> **Status: design / not started.** This is the prerequisite for running AMIX
> (Amiga Unix) and any other MMU-dependent OS on the Z3660. The companion
> Ethernet-driver plan that depends on this living at
> [../../../../z3660-drivers/amix/AMIX_ethernet_driver_plan.md](../../../../z3660-drivers/amix/AMIX_ethernet_driver_plan.md).
> Musashi's PMMU path was explicitly rejected; this plan covers the UAE path only.

## Resolved decisions (grilling pass, 2026-05-30)

These supersede any conflicting wording further down; the older sections are kept
for context but read these first.

1. **The MMU was faked from the start, not regressed.** Confirmed by reading the
   code: `mmu_op30()` ([src/uae/newcpu.cpp:2416](src/uae/newcpu.cpp#L2416)) is
   verbatim WinUAE with **every arm forced to the `*fake_*` variant**. Upstream,
   this same switch branches on `currprefs.mmu_model` (set → real
   `mmu_op30_pmove/pflush/ptest` in `cpummu030.cpp`; unset → the fake ones). So
   re-enabling = restoring the real arms behind `if (currprefs.mmu_model)`. The
   seam (dispatcher, `movec` MMU-reg decode in `newcpu_common.cpp`, the real TTR
   matcher `fake_mmu_do_match_ttr` at [newcpu.cpp:2444](src/uae/newcpu.cpp#L2444),
   the bus-error stack-frame builders, and `regs.mmu_fault_addr` plumbing) is
   **provably intact** — this is re-attaching an amputated limb to a live socket,
   not reconstructing. The stub `cpummu.h` is the genuine ARAnyM/WinUAE header
   hand-truncated to only the TTR `#define`s the fake matcher needs.

2. **Two register stores coexist; Phase 1's "extend regstruct" wording is wrong.**
   - 030 side: file-static fakes `fake_tc_030/srp_030/crp_030/mmusr_030/tt0_030/
     tt1_030` ([newcpu.cpp:151](src/uae/newcpu.cpp#L151)), written only by
     `mmu_op30fake_pmove`.
   - 040 side: real-named `regs.itt0/dtt0/tcr/mmusr/urp/srp`
     ([newcpu.h:137](src/uae/include/newcpu.h#L137)), written by `movec`, consumed
     only by the stack-frame builders.
   The 030 PMMU state **and the ATC live in `cpummu030.cpp`'s own file-static
   globals upstream** (`tc_030`, `srp_030`, `crp_030`, `mmu030.atc[]`), **not** in
   `regstruct`. Phase 1 is therefore "import `cpummu030.cpp` *with* its own state
   and route the PMOVE arm to write *that*," not "grow `regstruct`." **Keep the
   `fake_*_030` globals** — the non-MMU `UAE_030`/`UAEJIT_030` modes still use the
   fake path.

3. **Spike is repositioned as a post-Phase-1 risk probe, not a cheap path to boot.**
   The spike needs the `*_mmu030` accessors, which only exist after Phase 1 is
   imported. So: do Phase 1 first, then probe. **Kill criterion** — single-step
   each of {`CAS (a0),(a1)` RMW, a misaligned long crossing a page boundary, a
   `MOVEM` faulting mid-list} across a fault; if *any one* diverges from the
   Amiberry oracle on restart → commit to the faithful `gencpu` route, no debate.
   Expectation: at least the `MOVEM`/RMW cases fail (un-regenerated `cpuemu_13`
   opcodes were not emitted to be restartable), so budget for faithful anyway and
   treat a passing spike as a bonus that de-scopes Phase 0. **This reverses the
   iteration ladder: Phase 1 → spike → (only if it fails) Phase 0 `gencpu`.**

4. **MMU source: import from Amiberry, but verify by direct diff — and the real
   gate is symbol-level, not the version label.** "Matches WinUAE 5.6.0" is a
   proxy; the gate that protects compilation is *"every external symbol
   `cpummu030.cpp`/`cpummu.cpp` references resolves against **this tree's**
   `regstruct`/`memory.cpp`/`newcpu.cpp` signatures."* Steps: (a) pick the Amiberry
   commit, (b) `diff` its `cpummu030.cpp`/`.h` against WinUAE 5.6.0 source to
   confirm functional identity — record SHA + diff, do **not** trust the tag name,
   (c) run the symbol-level API diff as the acceptance gate, logging every
   unresolved symbol as an explicit port decision.

5. **Host harness is the oracle-of-record; the per-instruction Amiberry
   differential is demoted to optional.** The differential-vs-Amiberry test is
   fragile (needs identical file *and* identical wiring; on disagreement you can't
   tell which). Promote the Layer-1 host harness, validated end-to-end by **AMIX
   actually booting** under Amiberry/FS-UAE — the unambiguous signal.

6. **`-m32` is not mandatory.** `uaecptr` is `uae_u32` on every host; the
   translation logic is pointer-width-independent. Width only bites at NATMEM
   (host `baseaddr + addr`), which the harness stubs away. Fix the *cause*: a
   **pointer-width-agnostic flat-buffer memory model** (index by `uae_u32`, zero
   `uaecptr`↔`void*` round-trips, NATMEM fully stubbed). Build **native 64-bit by
   default** (ASan/valgrind/gdb are first-class; they're degraded-to-broken under
   `-m32`). Keep an **optional `-m32` parity smoke-test**; escalate to mandatory
   only if it surfaces an unavoidable truncating round-trip (which would itself be
   a latent target bug worth auditing).

7. **On-target C++ exception-unwinding gate before Phase 1.** The fault-restart
   model is real C++ `throw`/`catch` (`mmu_common.h` TRY/CATCH). The build is
   bare-metal `arm-none-eabi` + `-O3 -flto` with `-specs=Xilinx.spec`; unwinding
   depends on `.ARM.exidx`/`.ARM.extab` being emitted **and placed by the Xilinx
   linker script** (good: `--gc-sections` is commented out, so exidx isn't
   stripped). The 18 existing TRY/CATCH sites prove it *compiles*, not that it
   *unwinds at runtime*. **The host harness runs under Linux g++ where unwinding
   always works, so it structurally cannot validate this** — strike
   exception-unwinding from the "done locally" coverage claim; it is hardware-only.
   **Add Phase 0.5:** cross-build a trivial `TRY{THROW(7);}CATCH(e){assert(e.prb==7);}`
   under `-O3 -flto`, flash to the Z-Turn, confirm it catches on real hardware.
   - Works → proceed.
   - Doesn't unwind → redefine TRY/CATCH/THROW to `setjmp`/`longjmp` **now**, before
     MMU code is written against the exception shape.
   - Works but slow (throw on Cortex-A9 with table search is not cheap, and faults
     are hot during demand paging) → measure vs the paging budget; pre-empt to
     `setjmp`/`longjmp` if needed.

8. **Enum threading — corrections and hardening (see Phase 7 fixes inline below).**
   The plan's own touch-list had a desync error: control-core `BOOTMODE` lives in
   **`config_file.h:11`, not `main.h`**. And the second dispatch block in
   `main.cpp` ends with a **bare `else` meaning `UAEJIT_040`** — appending the new
   mode there silently boots JIT-040. Strategy: keep append-at-end (index 0–6 are
   stable across all replicas), correct the path, **de-fang the bare-else in both
   dispatch blocks** (explicit arm + explicit `else → HALT`), add a per-file
   tripwire (`static_assert(BOOTMODE_NUM == N)` / cross-ref comment). Full shared
   header stays out of scope (3 build systems).

9. **`gencpu` diff-gate needs a baseline first.** The Phase 0 gate is "regenerated
   non-MMU `cpuemu_*` matches checked-in modulo **known local edits**" — but those
   edits aren't enumerated. Before the gate is meaningful, diff the **checked-in**
   `cpuemu_{0,4,11,13,40,44}`/`cpustbl.cpp` against pristine **Amiberry-v5.6.0**
   `gencpu` output to produce the known-edit set, so the gate becomes "diff ==
   known-edit set," not "diff == empty."

10. **Provenance corrected: core is Amiberry `v5.6.0` (`88f30af9`), not WinUAE
    5.6.0** (which never existed). Pin all imported sources (MMU engine, `gencpu`)
    to that tag. See the rewritten "Provenance & source strategy" section. This
    also fixes decision #4: there is no WinUAE 5.6.0 to diff against, so the
    Amiberry import is the source of record and the only gate is the symbol-level
    API diff against this tree.

11. **AMIX oracle is real and the config is pinned** (see "AMIX oracle recipe"):
    AMIX 2.1 demonstrably boots on Amiberry + FS-UAE (issue #1376). The verified
    config doubles as the spec for the Z3660 `UAE_030_MMU` mode — notably
    **`cpu_compatible = false`** ("More Compatible" OFF; ON panics AMIX), JIT off,
    A3000-class only.

### Progress log (host harness)
- **Layer-1 host harness is built and green** ([test/host/](test/host/)): the
  portable UAE core subset (`newcpu`, all six `cpuemu_*`, `cpustbl`, `cpudefs`,
  `readcpu`, `memory`, `events`, `fpp`) compiles+links on x86-64 g++ with board
  seams stubbed to a big-endian flat-RAM buffer (decision #6: `lget`/`wget` path,
  no `baseaddr_direct`, no `-m32`). The 68030 direct interpreter (mode 0, JIT off)
  executes real instructions; 10/10 foundation assertions pass (moveq, add.l,
  move.l #imm32, memory store/load, byte/word/long big-endian round-trips). Two
  arch-safe core fixes were required to build off-ARM: `regs.pissoff` moved out of
  `#ifdef JIT` (newcpu.h); ARM `vmrs/vmsr` asm in `fesetround` guarded with a host
  no-op (fpp_native.cpp). A `HOST_TEST_HARNESS`-guarded `harness_set_x_funcs()`
  wrapper exposes the static `set_x_funcs()`. **This is the substrate for the MMU
  translation tests, pending the engine import.**

## Context & goal

The Z3660 UAE core today presents a 68030/68040 to the Amiga but the **MMU is
faked**: `mmu_model` is hardcoded to `0`
([src/uae/uae_emulator.cpp:912](src/uae/uae_emulator.cpp#L912)) and the
`mmu_op30fake_*` routines in [src/uae/newcpu.cpp](src/uae/newcpu.cpp) only store
TC/SRP/CRP and match TTRs — no page-table translation. AMIX is SVR4 UNIX with
demand paging; it programs the 030 PMMU root pointers and requires real
logical→physical translation, so it cannot boot.

**Goal:** add a working WinUAE-native 68030 PMMU as a new interpreted boot mode
(`UAE_030_MMU`), sufficient to boot AMIX 2.1p2 to multi-user. 68040-MMU is a
stretch goal that falls out of the same machinery.

## Provenance & source strategy

> **CORRECTED 2026-05-30 (was wrong in the original draft).** The core is
> **Amiberry v5.6.0** (git tag `v5.6.0`, sha `88f30af909da67c51723ef517ea5e2840c7beebf`,
> 2023-03-24), **not WinUAE 5.6.0 — which does not exist.** Verified by git
> archaeology: WinUAE's `include/options.h` version triple jumps **5.3.1 → 6.0.0
> directly** (at WinUAE commit `eee1bc96`, "Custom chipset complete rewrite",
> 2025-01-04); there is no 5.4/5.5/5.6 anywhere in WinUAE history. The
> `UAEMAJOR 5 / UAEMINOR 6 / UAESUBREV 0` in
> [src/uae/include/options.h](src/uae/include/options.h) is **Amiberry's own
> version number** (Amiberry's `options.h` defines UAEMAJOR/MINOR/SUBREV as the
> Amiberry release; tag `v5.6.0` is the unique commit where it reads exactly 5/6/0).
> The ARM JIT (`src/uae/jit/`) is the Amiberry/ARAnyM backend — consistent with an
> Amiberry base.

**Pin the source to Amiberry `v5.6.0` (`88f30af9`).** This *dissolves* the old
decision-#4 tension (there is no WinUAE 5.6.0 to diff against): the tree's lineage
*is* Amiberry, so import the MMU engine straight from Amiberry `v5.6.0` and the
differential-oracle premise holds by construction. The MMU code's ultimate origin
is **WinUAE 2.6.0's** "full 68030/040/060 MMU" feature (Toni Wilen, ~2013, the
release that first booted AMIX), inherited through the Amiberry line.

**File-name caveat (verify on clone):** at Amiberry `v5.6.0`, `src/include/cpummu030.h`
is already a *small* declarations header (same shape as this tree's stub), with the
68030-MMU implementation carried in a `.cpp` whose exact name must be confirmed by
cloning (WinUAE used `cpummu30.cpp`/`cpummu030.cpp` at repo root historically;
Amiberry puts sources under `src/`). The real acceptance gate remains the
**symbol-level API diff against this tree** (decision #4), not the file name.

## What already exists (the integration seam)

- **Memory access is indirected through function pointers** set in
  [set_x_funcs()](src/uae/newcpu.cpp#L618): `x_get_long`/`x_get_word`/
  `x_get_byte`/`x_put_*`/`x_get_iword`. The 030 interpreter (`cpuemu_13`) reaches
  memory only through these. This is exactly where WinUAE inserts the
  `*_mmu030` accessors — **the hook point is intact; only the MMU arm was
  stripped** (the 68020+ branch currently chooses jit-vs-direct only).
- **Fault/restart scaffolding is present**: `mmu_common.h` defines
  `TRY/CATCH/ENDTRY` as C++ `try / catch(m68k_exception)`, already used in ~8
  sites in `newcpu.cpp`. MMU bus faults restart instructions via this.
- **Partial MMU register state** already in the `regstruct`
  ([src/uae/include/newcpu.h:137](src/uae/include/newcpu.h#L137)):
  `itt0/itt1/dtt0/dtt1, tcr, mmusr, urp, srp, mmu_fault_addr`.
- **Page tables land in fast local RAM.** AMIX's tables sit in OS RAM = the
  card's Zynq DDR (`0x08000000+`, direct pointer) or Z3 fast RAM, not the slow
  Zorro bus. Table walks are cheap and the ATC caches them — MMU overhead is
  bounded.
- **`cpudefs.cpp` + `readcpu.cpp` are present** — the inputs `gencpu` needs.

## What is missing (the work)

1. **The MMU engine** — `cpummu030.cpp` + full `cpummu030.h`, `cpummu.cpp` +
   full `cpummu.h` (today only stub headers exist). Provides `mmu030_translate`,
   the ATC, and `get_long_mmu030`/`put_long_mmu030`/`get_iword_mmu030`.
2. **The MMU instruction table** — WinUAE 68030-MMU runs on a separately
   *generated* `cpuemu_31.cpp` (and `cpuemu_32.cpp` for 040), where each
   memory-touching opcode is emitted to fault-and-restart safely. **Both are
   absent**, and so is **`gencpu.c`** that generates them. The dispatch matrix
   [`cputbls[5][4]`](src/uae/newcpu.cpp#L849) has **no MMU column** — its four
   modes are `{direct, jit, more-compatible, cycle-exact}`.

## Effort tiers — decide with a spike

**Spike first (days):** point the `x_*` pointers at the `*_mmu030` accessors on
top of the *existing* `cpuemu_13`, set `mmu_model=68030`, and single-step AMIX's
early MMU setup. If a fault mid-`MOVE (a0),(a1)` restarts cleanly with the right
SSW/stack frame, the cheap route is viable and `gencpu` may be avoidable. If
restarts corrupt state (likely for read-modify-write and some EA modes), commit
to the faithful route below.

**Faithful route (the plan of record):** regenerate the MMU instruction tables
with `gencpu`. This is what WinUAE actually ships and the only route that is
correct for all opcodes.

## Work breakdown (faithful route)

### Phase 0 — Host `gencpu` toolchain + correctness baseline
- Import `gencpu.c` (and its small helpers) from WinUAE 5.6.0. Build it as a
  **host tool** (x86 Linux), fed by the present `cpudefs.cpp`/`readcpu.cpp`.
- **Correctness gate:** first regenerate the *non-MMU* `cpuemu_0/4/11/13/40/44`
  and `cpustbl.cpp` and diff against the checked-in files. They must match (modulo
  known local edits) before any generated MMU file can be trusted. This validates
  the gencpu version + flags against what's already in the tree.
- Then generate `cpuemu_31.cpp` (68030 MMU) — and `cpuemu_32.cpp` (68040 MMU) for
  the stretch goal — plus the matching `op_smalltbl_*` entries.

### Phase 1 — Import the MMU engine
- Add `cpummu030.cpp`/`.cpp` and replace the stub `cpummu030.h`/`cpummu.h` with
  the full WinUAE versions; replace `mmu_common.h` only if the fuller version is
  needed (keep the existing TRY/CATCH macro shape that compiles here).
- **Correction (see Resolved decision #2):** the 030 PMMU state *and the ATC* live
  in `cpummu030.cpp`'s own file-static globals upstream (`tc_030`, `srp_030`,
  `crp_030`, `mmu030.atc[]`), **not** in `regstruct`. So this is mostly "import
  `cpummu030.cpp` with its state intact," not "grow `regstruct`." Only add a
  `regstruct` field if a specific symbol the imported file references resolves
  there (the symbol-level API diff, decision #4, tells you which). Route the real
  `mmu_op30_pmove` to write the `cpummu030.cpp` 030 state; **leave the
  `fake_*_030` globals in place** for the non-MMU `UAE_030`/`UAEJIT_030` modes.
- Add all new `.cpp` to the build — note the [Makefile](Makefile) auto-globs
  `src/**/*.cpp`, so files just need to land under `src/uae/`; only update the
  explicit filter-out list if a file must be excluded.

### Phase 2 — Wire the dispatch table
- Extend `cputbls` to a `[5][5]` (add an MMU mode column) and populate the 68030
  row with `op_smalltbl_<mmu030>` (and 68040 with the 040 variant); add those
  table externs to `cpustbl.cpp`.
- In [build_cpufunctbl()](src/uae/newcpu.cpp#L863), select the MMU column when
  `currprefs.mmu_model` is set.

### Phase 3 — Restore the MMU accessor arm + config
- In [set_x_funcs()](src/uae/newcpu.cpp#L618), add the `if (currprefs.mmu_model)`
  arm that points `x_get_long`→`get_long_mmu030`, `x_get_iword`→`get_iword_mmu030`,
  `x_put_*`→`put_*_mmu030`, etc. (verbatim from WinUAE 5.6.0).
- Replace the `mmu_op30fake_*` stubs with the real `mmu_op30` from
  `cpummu030.cpp` (PMOVE actually loads TC/SRP/CRP into MMU state; PTEST/PFLUSH/
  PLOAD operate on the ATC).
- Add a `UAE_030_MMU` value to the `BOOTMODE` enum (main.h) and a dispatch arm in
  [main.cpp:707](src/main.cpp#L707); have `uae_emulator()` set
  `mmu_model = 68030`, `cpu_compatible` per WinUAE MMU requirements,
  `cachesize = 0` (**JIT disabled — mandatory**, the JIT inlines direct pointers
  and cannot restart on faults).
- **There are TWO dispatch blocks** in [main.cpp](src/main.cpp#L704) (`#ifdef
  MUSASHI_EMULATOR` and the `#else`). The first ends `else → HALT`; the **second
  ends with a bare `else` that means `uae_emulator(1,68040)` = JIT-040** (line
  ~746, comment `// if(shared->cfg_emu==UAEJIT_040)`). Appending `UAE_030_MMU`
  without touching this block makes selecting it **silently boot JIT-040** — wrong
  CPU *and* JIT-on. Fix: add the explicit `UAE_030_MMU` arm to **both** blocks and
  convert the second block's bare `else` into an explicit `else → HALT` mirroring
  the first. Also check `get_bootmode_type()`'s unknown-name fallback
  ([config_file.c:391](../Z3660/src/config_file.c#L391)) returns `UAEJIT_040`.

### Phase 4 — Fault/restart correctness
- Confirm a `TRY/CATCH` frame wraps the instruction dispatch in the relevant
  `m68k_run_*` loop so `cpummu030` page-fault exceptions unwind and restart the
  faulting instruction; build the correct 68030 bus-error stack frame and SSW.
- Exercise read-modify-write (`CAS`, `TAS`) and all EA modes across page
  boundaries.

### Phase 5 — Physical-access + ATC wiring
- Ensure `cpummu030`'s post-translation `phys_get_*`/`phys_put_*` bottom out at
  the existing physical dispatch (local RAM direct pointer, or `ps_read_*`/
  `ps_write_*` to the real Zorro bus). The translation layer must sit *above* the
  current address-range dispatch, not replace it.
- Verify the ATC is populated/invalidated by PFLUSH and on TC/SRP/CRP changes.

### Phase 6 — Bring-up
- Boot the AMIX kernel; instrument the first `PMOVE` to TC (MMU enable) and the
  first page faults; drive to single-user, then multi-user/login.

### Phase 7 — Expose `UAE_030_MMU` through the config & UI stack
The boot mode is selected via a `bootmode` enum that is **replicated, index-aligned,
across at least five independent places**. The new mode must be threaded through
all of them or selection/labels desync. **Append the new value at the end (before
the `*_NUM`/`NUM_BOOTMODES` sentinel)** so existing stored indices, configs, and
presets keep their meaning — do not insert it mid-enum.

Touch points (keep order identical everywhere):
1. **Emulator core enum** — `BOOTMODE` in
   [src/main.h](src/main.h) (drives the `uae_emulator()` dispatch in
   [src/main.cpp:707](src/main.cpp#L707), already handled in Phase 3).
2. **Control-core enum** — the parallel `BOOTMODE` in the Z3660 control firmware
   **`../Z3660/src/config_file.h:11`** (NOT `main.h` — corrected from the original
   draft; verified by grep). The two cores must agree.
3. **Config-file parser** — `bootmode_names[BOOTMODE_NUM]` in
   `../Z3660/src/config_file.c` (used by `get_bootmode_type()` → `shared->cfg_emu`),
   plus the default-config writer text and the `# Select boot mode …` comment in
   the same file.
4. **Config file + presets** — the `bootmode` line and its comment in
   `z3660-zturn_SD_content/DATA Second Partition (exFat)/z3660cfg.txt` and the
   `presets/preset*.txt` files (document the new `UAE_030_MMU` value).
5. **ZTop (Amiga side)** — the `bootmode` enum and `bootmode_names[NUM_BOOTMODES][25]`
   label array in `z3660-drivers/ZTop/Ztop.c` (e.g. add `"030 UAE MMU emu "`),
   feeding `GID_BOOT_LIST_BOOTMODE` and the `REG_ZZ_BOOTMODE` /
   `REG_ZZ_APPLY_BOOTMODE` write path.
6. **ZTop (ARM/on-board UI)** — the `b_list_emu` selectable list in
   `../Z3660/src/ARM_ztop/button.c` (writes `REG_ZZ_BOOTMODE`,
   [button.c:142](../Z3660/src/ARM_ztop/button.c#L142)) and wherever that list's
   item strings are defined.
7. **Register glue** — the `REG_ZZ_BOOTMODE` / `REG_ZZ_APPLY_BOOTMODE` handlers in
   `../Z3660/src/rtg/rtg.c` that turn the selected index into `cfg_emu` and apply.
8. **Status reporting** — ensure JIT/emulation-used readouts
   (`REG_ZZ_JIT_ENABLE`, `REG_ZZ_EMULATION_USED`, ZTop's `GID_INFO_JIT`) report
   **JIT = off** for this mode.

## Local development & debug strategy (Linux: Amiberry + FS-UAE)

The Z3660 emu is CPU-only (no chipset — `custom.cpp` is gutted) and the build is
cross-compile-only (`arm-none-eabi-`, Xilinx BSP). So you **cannot run the
firmware binary as a local Amiga** — there's nothing under it to boot. Instead,
~80–90% of the MMU correctness work is done locally on the dev Linux box across
three layers; only final integration needs the board. **None of this needs
WinUAE** (Windows-only); Amiberry and FS-UAE cover it on Linux.

### Layer 1 — Host MMU unit harness (no Amiga, no board) — primary loop
- New dir `test/host/` with a host `g++` Makefile that compiles the **portable
  core subset** for x86: `newcpu.cpp`, `cpuemu_13.cpp` (+ generated
  `cpuemu_31.cpp`), `cpummu030.cpp`, `cpummu.cpp`, `memory.cpp`, `readcpu.cpp`,
  `cpudefs.cpp`, `cpustbl.cpp`, plus stubs.
- **Stub the board seams**: `ps_read_*`/`ps_write_*` → a flat `malloc`'d RAM
  buffer; the local-RAM ranges → the same buffer; `xil_cache`/`xil_io` → no-ops;
  IPL read → 0. This gives a deterministic "flat memory" machine.
- **Tests** (gdb/valgrind-debuggable):
  1. TTR transparent translation passes through.
  2. Build a minimal 3-level 030 page table in the buffer, `PMOVE` SRP/CRP + TC
     enable, assert a logical addr → expected physical; honor CI/WP bits.
  3. Unmapped page → bus fault (`m68k_exception`) caught by `TRY/CATCH`, correct
     SSW/stack frame, instruction restarts after a simulated fault handler maps
     the page.
  4. RMW (`CAS`/`TAS`), misaligned, and cross-page accesses.
  5. `PFLUSH`/`PTEST`/`PLOAD` against the ATC.
- This is the main dev loop for Phases 1–5 — fast, deterministic, no hardware.

### Layer 2 — Amiberry as oracle + differential test (x86-64 Linux)
- Build Amiberry from source on the dev box; configure 68030 + MMU and **boot
  AMIX** to confirm the AMIX image + Kickstart are good and that the
  WinUAE-lineage MMU boots AMIX at all.
- **Differential oracle**: run the same canned translation/fault scenarios
  through Amiberry's `mmu030` and the Layer-1 harness. Both derive from the same
  `cpummu030.cpp`, so any divergence is a **Z3660 porting/wiring bug**, not an
  algorithm bug — a precise signal.
- Amiberry is also the **source** for `cpummu030.cpp/.h`, `cpummu.cpp/.h`, and
  `gencpu.c` (version-pinned per "Provenance").

### Layer 3 — FS-UAE as local AMIX runtime / second reference
- Run AMIX under FS-UAE (full chipset + 030 MMU) as the runnable "local AMIX":
  validates the install, hosts later AMIX-side work (the Ethernet driver,
  `zorro_probe`), and is an independent behavioral cross-check vs Amiberry.

### AMIX oracle recipe (verified 2026-05-30)
**Known-good:** AMIX 2.1 boots to a login prompt on **both Amiberry and FS-UAE**
on x86-64 Linux — first-hand confirmed in Amiberry issue
[#1376](https://github.com/BlitterStudio/amiberry/issues/1376) (users codewiz +
maintainer midwan, 2024). This validates the Layer-2/Layer-3 oracle assumption;
the MMU lineage traces to WinUAE 2.6.0 ("Amix … fully working").

**Fast path (skip the multi-hour install):** the "Noth" pre-installed image
`basicamixX11R4.tbz2` (https://ftp2.grandis.nu/turran/FTP/Misc/AMIX/Noth/) boots
directly; login `guest` (no password), root password `wasp`.

**Config (the load-bearing part — also the spec for the Z3660 `UAE_030_MMU` mode):**
- Machine: **A3000** (NOT A4000 — the 040/060 MMU is a different, AMIX-incompatible MMU). 68030 only.
- CPU: **68030, real MMU ON, FPU 68882 ON**.
- **JIT: OFF** (mandatory — cannot restart on faults).
- **"More Compatible": OFF** — `cpu_compatible` ON causes AMIX kernel panics.
  *This pins Phase 3's "`cpu_compatible` per WinUAE MMU requirements" → set it
  **false** (the direct interpreter, mode 0 — the same path the host harness
  already runs).*
- Chipset: ECS, Chipset-Extra = A3000. RAM: 2 MB chip + 16 MB motherboard/fast
  (AMIX recognizes 4–16 MB fast; 16 = max safe).
- ROM: a **genuine A3000 Kickstart** with the on-board SCSI boot ROM
  (KS 2.04 rev 37.175 or KS 3.1 rev 40.x), NOT the UAE built-in ROM.
- Disk: HDF on the A3000 internal SCSI controller at **SCSI ID 6** (install tape, if used, at ID 4).

### gencpu (Phase 0) is Linux-native
- Build `gencpu.c` with host `g++`, regenerate `cpuemu_*`, and run the
  non-MMU regen-vs-checked-in **diff gate** — all on the PC.

### Linux-specific caveats
- **Build the host harness 32-bit (`-m32`)** to match the 32-bit ARM target's
  pointer width — the UAE core's `uaecptr`/`baseaddr`/NATMEM assumptions can
  diverge under 64-bit pointers and produce false mismatches vs the target.
- Endianness is consistent (x86-64 and ARM both LE; 68k big-endian handled by the
  same byteswaps), so no extra work there.
- The host harness tests **translation logic**, not bus timing / cache coherency /
  real-IPL — those remain hardware-only (below).

### Iteration ladder (recommended order — revised per grilling)
0. **On-target unwind gate (Phase 0.5, hardware):** flash the trivial
   `TRY{THROW(7);}CATCH` test under `-O3 -flto`; confirm it catches on the Z-Turn.
   If it doesn't unwind, switch TRY/CATCH/THROW to `setjmp`/`longjmp` before any
   MMU code. (Decision #7. This is hardware-only — the host harness can't test it.)
1. **gencpu baseline first:** diff checked-in non-MMU `cpuemu_*` against pristine
   WinUAE-5.6.0 gencpu output → enumerate the known-edit set (Decision #9). *Then*
   the diff gate is meaningful.
2. Import `cpummu030.cpp` (Phase 1) + symbol-level API diff gate (Decision #4).
3. **Spike (post-Phase-1 risk probe):** the 3 adversarial restart cases vs the
   Amiberry oracle (Decision #3). Pass → Phase 0 `gencpu` regeneration may be
   skippable. Fail (expected) → commit to faithful `gencpu` route.
4. Layer-1 unit harness green (tests 1–5), native 64-bit, flat-buffer memory.
5. Layer-2 Amiberry differential parity on canned scenarios (optional cross-check).
6. AMIX boots in Amiberry **and** FS-UAE (image/OS sanity — the oracle-of-record).
7. Cross-build firmware in the `docker/` env, deploy `BOOT.BIN` to SD, boot AMIX
   on Z-Turn + Z3660 + real A3000/A4000 (the only step that needs hardware).

### Still hardware-only (unchanged)
Real-bus `ps_read`/`ps_write` to chip RAM + chipset, real IPL injection,
CPU↔motherboard timing/cache-inhibit coherency, on-board ZTop selection, real
performance numbers, and the definitive "AMIX boots on the Z3660" milestone.

## Risks / open questions

1. **Spike outcome** decides Phase 0 scope — whether `gencpu` regeneration is
   required or `cpuemu_13` + accessors suffice. Resolve early.
2. **gencpu version drift** — the non-MMU regeneration diff (Phase 0 gate) is the
   guard; do not skip it.
3. **Instruction restart correctness** — the classic hard part of MMU emulation
   (RMW, misaligned cross-page, FPU memory operands). Highest correctness risk.
4. **030 caches** — AMIX may enable instruction/data caches via CACR; verify the
   core's cache emulation interacts correctly with MMU + real-bus accesses
   (`cpu_data_cache` is marked "not used" in options.h today).
5. **Performance** — interpreted 030+MMU on the Cortex-A9 is tens of MIPS at best,
   well below the 120-MIPS JIT 040. AMIX shipped on a 25 MHz 030 (~4–5 MIPS), so
   it should be usable, but set expectations: no JIT in MMU mode.
6. **Real-bus timing under translation** — table walks to local RAM are fast, but
   confirm no regression in IPL/interrupt latency
   ([main.cpp ipl_main_read](src/main.cpp#L345)) while the MMU path is active.
7. **Enum desync across the config/UI stack** — the `bootmode` enum is duplicated
   in 5+ files (Phase 7) and must stay index-aligned. Appending avoids reindexing
   existing configs/presets; a mismatch silently selects the wrong mode. A single
   shared header would be the clean fix but is out of scope here.

## Verification (end to end)

1. **Codegen gate**: regenerated non-MMU `cpuemu_*` diff-match the checked-in
   files.
2. **Unit-ish**: a bare-metal/AmigaOS test that sets up a known page table, does
   `PMOVE` to enable, and verifies a translated access lands at the expected
   physical address and that an unmapped access bus-faults and restarts.
3. **AMIX boot**: kernel enables MMU and survives early paging; reaches
   single-user.
4. **Full**: AMIX to multi-user/login; run a paging-heavy workload (compile) to
   shake out restart bugs.
5. **Selection (Phase 7)**: `UAE_030_MMU` is choosable and persists via all three
   paths — `z3660cfg.txt`, the ZTop Amiga GUI list, and the on-board ARM ZTop
   button — and each reports JIT = off; existing modes/presets still resolve to
   the same behavior (no index shift).
6. Only then layer the Ethernet driver (companion plan).
