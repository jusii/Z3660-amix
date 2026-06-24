# Z3660 — Amix (Amiga UNIX) support

This is the Amix-oriented fork of [shanshe/Z3660](https://github.com/shanshe/Z3660). On top of
the upstream firmware it adds a **real 68030 PMMU emulator** and an **emulated A3000 mainboard
SCSI controller**, which together let a Z3660 + Z-turn boot **Amiga UNIX (AMIX) 2.1** — no
physical 68030 or A3000 SCSI hardware required.

Everything here is **optional**. If you don't run AMIX, none of it changes how your Z3660 boots
AmigaOS — the new options default to off and the new boot mode is just one more entry in the
existing list. Upstream Z3660 behaviour is unchanged.

> **Status in one line:** the AMIX 2.1 kernel boots under this firmware (you get the SVR4
> banner, RAM sizing and copyright on screen); reaching a *stable login shell* is still being
> worked on, and the remaining blocker is **guest-side** (inside AMIX's own SCSI driver), not in
> the emulator. See [Current status](#current-status--known-limitation) below.

---

## What this fork adds

| Addition | Summary |
|---|---|
| **68030 PMMU emulation** | New `bootmode UAE_030_MMU` — a cycle-accurate 68030 interpreter **with a working PMMU** (demand paging, `mmu.library` sees it). AMIX needs a real MMU; the stock 68040-JIT modes don't provide the 030 PMMU semantics it expects. |
| **Emulated A3000 SCSI** | An emulated Commodore A3000 mainboard SCSI (SuperDMAC + WD33C93). This is AMIX's **kernel bootstrap** path — AMIX boots its kernel through the A3000 SCSI driver, then hands off to the native Z3660 drivers. |
| **Native AMIX drivers** | `amix-z3660scsi` / `amix-z3660net` run inside AMIX against the Z3660 PISCSI + ethernet register protocol this repo owns. |
| **AMIX warm-reboot fix** | A clean Zynq reboot on an AMIX `reboot`, instead of the register-flood / data-abort the naïve path produced. |
| **Emulator perf + debug knobs** | `service_cadence` (CPU throughput vs. interrupt latency) and per-category debug gating (`DEMU` / `DSCSI`) to silence the emulator's debug floods. |

---

## Boot modes

`bootmode` (in `z3660cfg.txt` or a preset) selects how the CPU is provided. All eight values:

| `bootmode` | What it is |
|---|---|
| `MOBOCPU` | Motherboard 68060 (Z3660 out of the path) |
| `CPU` | The Z3660's own real 68060 |
| `MUSASHI` | Musashi 68k interpreter |
| `UAE_030` | UAE 68030 interpreter (no JIT, **no MMU**) |
| `UAEJIT_030` | UAE 68030 JIT (no MMU) |
| `UAE_040` | UAE 68040 interpreter |
| `UAEJIT_040` | UAE 68040 JIT — the fast everyday AmigaOS emulator (~120 MIPS) |
| **`UAE_030_MMU`** | **UAE 68030 interpreter with a real PMMU — the AMIX mode.** "Emulation mode 4". |

`UAE_030_MMU` is the only mode that provides the 68030 PMMU; it is interpreted (no JIT), so it is
slower than `UAEJIT_040` — that is the price of the MMU semantics AMIX requires.

The 030 MMU/CPU core is imported from WinUAE 4.4.0 (= Amiberry v5.6.0, the same lineage as the
existing Z3660 emulator). On real hardware `mmu.library` correctly detects the emulated 68030 PMMU.

---

## Running AMIX

### You must supply (not redistributable)

- **An A3000-variant Kickstart ROM.** AMIX boots through the A3000 mainboard SCSI driver, so it
  needs an A3000 Kickstart — a genuine **KS 3.1 r40.68 (A3000)** is known to work. **A4000 ROMs
  cannot boot AMIX** (no A3000 SCSI driver — you get "insert disk"). Lower A3000 KS (e.g. 2.04)
  also reaches the same point.
- **An AMIX hard-disk image** (`Amix.hdf`, an RDB/RDSK disk). AMIX itself is copyrighted
  Commodore software and is not included here.

Neither the Kickstart nor the AMIX disk image ships in this repo or in the release bundle.

### Config

A minimal AMIX `z3660cfg.txt` looks like:

```
bootmode UAE_030_MMU          # 68030 + PMMU
amix_mode YES                 # AMIX memory contract + A3000-SCSI bootstrap (default ON for this bootmode)
service_cadence 4             # AMIX-safe perf; see below

# Map an A3000-variant Kickstart (you supply the .rom file):
kickstart5 kicks/A3kKS31.rom
kickstart 5

# Put the AMIX disk image on a SCSI unit (you supply the .hdf):
hdf0 hdf/Amix.hdf
scsi0 0
```

`amix_mode YES` is the default whenever `bootmode UAE_030_MMU` is selected, so it can be omitted;
it is shown for clarity. The same options work in a `presets/presetN.txt` quick-select file.

---

## New configuration options

These two options are specific to this fork (defined in
[`config_file.c`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)).

### `amix_mode YES|NO`

Applies the **AMIX memory contract** (a single ≤16 MB window — no separate `$08000000` CPU-RAM
board) and enables the **A3000-SCSI bootstrap**.

- **Default:** `YES` when `bootmode` is `UAE_030_MMU`, otherwise `NO`. Existing non-AMIX configs
  are therefore unaffected.
- **Side effect:** enabling it forces `cpu_ram = NO` (the 128 MB CPU-RAM board is incompatible
  with the AMIX address map).
- It is a *separate* switch from `bootmode` on purpose: you can run plain 030+PMMU AmigaOS
  (`bootmode UAE_030_MMU`, `amix_mode NO`) without the AMIX memory layout.

### `service_cadence N`  (integer, `N ≥ 1`)

How many emulated instructions run between emulator interrupt-service polls. Lower = lower
interrupt latency; higher = more CPU throughput.

- **Default:** `1` (poll every instruction). Values `< 1` are clamped to `1`.
- **Throughput:** raising it speeds the emulated CPU up to roughly **+1.3×** near a knee around 8;
  disk I/O throughput is essentially flat across the range.
- **⚠️ AMIX boot-safety:** the AMIX A3000-SCSI bootstrap is sensitive to INT2 latency.
  Cadence **2 and 4 boot; cadence 8 hangs the bootstrap.** **Use `service_cadence 4`** as the
  boot-time sweet spot (it captures most of the gain). You can raise it to `8` *after* boot via
  the serial **SERV** runtime menu if you want maximum throughput once AMIX is up.
- **⚠️** A very high cadence combined with an unthrottled network flood (e.g. a fast telnet
  transfer) has been observed to panic AMIX — keep boot-time cadence ≤ 4.

### Runtime debug toggles (not config-file options)

The emulator prints verbose debug traces that visibly slow interactive use. These are gated at
runtime from the serial **C** (console) menu — they are *not* `z3660cfg.txt` options:

- **`DEMU`** — gates the emulator floods (`[PC]`, `[RTE-B-IF]`, fixup traces). Turning these
  **off makes interactive AMIX feel substantially faster.**
- **`DSCSI`** — gates the `[PISCSI]` SCSI register-trace flood.

Both default to off (quiet) in normal operation.

---

## How the A3000 SCSI bootstrap works

The emulated controller
([`a3000_scsi.cpp`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/a3000_scsi.cpp)) is
ported from WinUAE's `a2091.cpp` (Commodore SuperDMAC + WD33C93 core, GPLv2 — the same provenance
as the imported 030 MMU code), with two deliberate divergences:

1. **Interrupt model.** The Amiga INT2 line is recomputed as a pure level function of
   `(SDMAC.CNTR & INTEN) && WD.ASR_INT` after every register access (WinUAE's hsync-paced status
   queue has no analogue here).
2. **The SCSI target is the Z3660 backend.** Instead of WinUAE's `scsi.cpp`, the emulated
   controller synthesises `INQUIRY` / `READ CAPACITY` / `MODE SENSE` / `TUR` / `REQUEST SENSE`
   and moves `READ`/`WRITE` block data over the existing **PISCSI cross-core channel** to core 0's
   FatFS. The SCSI target id is the `devs[]` unit index — `Amix.hdf` is target 6.

It runs on core 1 (the 68k emulator); its MMIO lives at the `$00DD0000` page, gated by
`amix_mode`. Full design notes:
[`AMIX_SCSI_design.md`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/AMIX_SCSI_design.md).

---

## Verifying it booted (serial console)

On the JTAG/serial console (`/dev/ttyUSB0`) a correct AMIX boot shows:

```
Emulation mode 4
68030 MMU enabled
[Core1] Starting UAE_030_MMU emulator        (… _AMIX when amix_mode is on)
```

If you instead see `Emulation mode 1` or `[SD Init] FAIL`, the firmware fell back to the
68040-JIT path — re-check that `BOOT.BIN` deployed and that `bootmode UAE_030_MMU` is active.

When you issue `reboot` inside AMIX the emulator detects the warm-reset vector (the overlay isn't
restored on a 68k reset) and performs a **clean Zynq reboot** rather than flooding the bus; the
board comes back in ~2.5 minutes.

---

## Current status & known limitation

The AMIX 2.1 kernel **boots** under this firmware: dynamic linker runs, `/sbin/init` runs, the
SVR4 banner, memory sizing and copyright appear on HDMI. What is **not yet reliable** is reaching
a steady login shell — and the remaining blocker is **inside AMIX's own software**, not the
emulator:

- The AMIX `a3091` SCSI driver has a `ddtab.HEAD` / lost-completion race: under certain
  demand-paging completion orderings it biodone()s the wrong buffer, stranding a page-in. The
  emulator delivers the completion correctly; the guest driver mis-routes it.
- A separate memory-coherency line of investigation (`lpsched`) is documented under
  [`docs/investigations/`](investigations/).

Neither is a Z3660 hardware or emulator-correctness bug in the usual sense — they are guest-side
AMIX issues this fork is a vehicle for debugging. If you want to help, the disassembly tooling and
analysis are in the investigations folder and `KNOWN_ISSUES.md`.

---

## Benchmarking

`tools/amix-bench/` ships a portable Dhrystone 2.1 + disk-I/O benchmark with precompiled AMIX
binaries, used to compare real vs. emulated 030 throughput and to tune `service_cadence`. See
[`tools/amix-bench/README.md`](../tools/amix-bench/README.md).

---

## See also

- [`README.md`](../README.md) — project overview and quick start
- [`KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) — hardware/firmware limitations
- [`docker/README.md`](../docker/README.md) — building the firmware from source
- [`AMIX_SCSI_design.md`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/AMIX_SCSI_design.md),
  [`AMIX_ethernet_driver_plan.md`](../z3660-drivers/amix/AMIX_ethernet_driver_plan.md) — design notes
