# Phase 0 — gencpu toolchain + non-MMU known-edit baseline

This is the host-side `gencpu` toolchain and the **correctness baseline** for the
faithful-route MMU work (UAE_030_MMU_plan.md, Phase 0 / decision #9). Goal: prove
we can regenerate the CPU instruction tables and know exactly which checked-in
files are hand-edited, *before* trusting a generated `cpuemu_31.cpp` (the 68030
MMU opcode table).

## Provenance (confirmed 2026-05-30)

This tree's CPU core **is Amiberry v5.6.0** (`88f30af9`). Proven by direct diff of
the checked-in `cpuemu_*.cpp` against Amiberry v5.6.0's *pre-generated* files
(Amiberry ships them checked-in):

| file        | changed lines vs Amiberry v5.6.0 | nature |
|-------------|----------------------------------|--------|
| `cpuemu_4`  | 0  | **byte-identical** |
| `cpuemu_11` | 0  | **byte-identical** |
| `cpuemu_13` | 0  | **byte-identical** (the 68030 interpreter the MMU work builds on) |
| `cpuemu_44` | 0  | **byte-identical** |
| `cpuemu_0`  | 14 | Z3660 hand-edits: debug `printf`s + one `uae_u32`/`uae_s32 src` signedness change (EORSR.W path) |
| `cpuemu_40` | 104| Z3660 hand-edits: `uae_u16`/`uae_s16 src` signedness + a few removed `src = regs.regs[...]` lines in 68040 handlers |
| `cpustbl`   | 1712| **cosmetic only** — an explicit `(uae_s16)` cast on Bcc/DBcc table entries; a gencpu-version formatting difference, semantically identical |

**Takeaway:** the non-MMU tables are pristine Amiberry v5.6.0 except a small,
enumerated hand-edit set. The Phase-0 diff gate is therefore *"regenerated /
checked-in non-MMU `cpuemu_*` == Amiberry v5.6.0 pre-generated, modulo the table
above."* The reference is Amiberry v5.6.0's own pre-generated files — running
`gencpu` is not even required to validate the non-MMU side.

## gencpu version caveat (important for `cpuemu_31`)

Amiberry v5.6.0's *repository* `src/gencpu.cpp` is NOT the generator that produced
the checked-in tables: it **aborts in `term()` on the 68060 `HALT`/`PULSE`/`LPSTOP`
opcodes** (no `case i_HALT`), which the checked-in tables treat as illegal (no
`op_4ac8` handler anywhere). And its `cpustbl` output lacks the `(uae_s16)` cast
our tree has. So the real generator was a slightly different (WinUAE-lineage)
`gencpu`. **For the faithful route, source `cpuemu_31.cpp` (and the matching
`gencpu`) from the WinUAE version whose `cpuemu_13` matches this tree's** — i.e.
the WinUAE that Amiberry v5.6.0's pre-generated tables came from — so the MMU table
pairs cleanly with the (pristine) `cpuemu_13`. See the MMU-engine API-diff work.

## Files here

- `build_gencpu.sh` — builds the host `gencpu` from a given Amiberry/WinUAE source
  checkout against *this tree's* headers. Works (`gencpu.cpp` compiles clean
  against our headers with `-D_vsnprintf=vsnprintf`); the only external symbol is
  `ua()`, stubbed by `charset_stub.cpp`.
- `charset_stub.cpp` — identity host stub of Amiberry's `ua()` (TCHAR→UTF-8).
- `baseline_diff.sh` — re-runs the known-edit diff of this tree against a
  pinned Amiberry v5.6.0 checkout.

Neither script is wired into the firmware build (the firmware Makefile globs
`src/**`, not `test/**`).
