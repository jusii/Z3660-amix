# Z3660 — Amix (Amiga UNIX) support

This is the Amix-oriented fork of [shanshe/Z3660](https://github.com/shanshe/Z3660). On top of
the upstream firmware it adds a **real 68030 PMMU emulator** and an **emulated A3000 mainboard
SCSI controller**, which together let a Z3660 + Z-turn boot **Amiga UNIX (AMIX) 2.1** — no
physical 68030 or A3000 SCSI hardware required.

Everything here is **optional**. If you don't run AMIX, none of it changes how your Z3660 boots
AmigaOS — the new options default to off and the new boot mode is just one more entry in the
existing list. Upstream Z3660 behaviour is unchanged.

> **Status in one line:** AMIX 2.1 **boots reliably to a stable multiuser login shell** under this
> firmware — SVR4 banner, multi-user, root login on the HDMI console and over telnet. It survives
> repeated reboots and sustained fork/exec load (a 27-reboot soak ran clean). One rare demand-paging
> edge case under extreme load is tracked but does not block normal use. See
> [Current status](#current-status) below.

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

### Getting started (the quick path)

1. **Firmware.** Grab a pre-built `BOOT.BIN` from this repo's Releases (no build needed) or build it
   yourself (see [`docker/README.md`](../docker/README.md)), and deploy it to the SD card.
2. **Supply the bits that can't ship here** — an A3000-variant Kickstart ROM and your AMIX `.hdf`
   (details just below).
3. **Put the `.hdf` on the SCSI id AMIX was installed on** — id 6 for the usual A3000 install. This
   matters: see the ⚠️ note under [Config](#config).
4. **Configure** `bootmode UAE_030_MMU`, point a `kickstartN` at the A3000 ROM, and map the disk with
   `hdfN` + `scsiN` (full example under [Config](#config)).
5. **Boot.** On the serial console you should see `Emulation mode 4` / `68030 MMU enabled`; AMIX then
   banners and reaches a `login:` prompt (log in as `root`).

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

# Put the AMIX disk image on the SCSI ID it was INSTALLED on (you supply the .hdf):
hdf0 hdf/Amix.hdf
scsi6 0                       # Amix.hdf on SCSI target 6 -- see the SCSI-ID note below
```

`amix_mode YES` is the default whenever `bootmode UAE_030_MMU` is selected, so it can be omitted;
it is shown for clarity. The same options work in a `presets/presetN.txt` quick-select file.

> **⚠️ The SCSI ID must match the ID AMIX was installed on.** AMIX bakes its root device into the
> kernel as a fixed *(controller, target)* pair — for the usual A3000 install that is **controller 0,
> target 6** (root then mounts as `/dev/dsk/c6d0s1`). So put `Amix.hdf` on **SCSI id 6** (`scsi6`).
> On any other id the kernel still loads and you get the SVR4 banner, but the root mount fails
> (`s5mountroot` → `VOP_OPEN EIO`) and you never reach login. If *your* image was installed on a
> different id, use that id instead. The firmware presents the disk on exactly the id you configure —
> there is no hidden id-0 aliasing.

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
   FatFS. The SCSI target id is the `devs[]` unit index set by `scsiN` in the config, and each id maps
   to its own backend disk — the disk answers on exactly the id you configure, with no id aliasing.
   AMIX's root lives on the id it was installed on (target 6 in the usual A3000 install).

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

AMIX then prints the SVR4 banner and memory sizing, runs its rc scripts, and reaches a `login:`
prompt on the HDMI console (log in as `root`). Once it is multi-user you can also reach it over the
network (`telnet` to the AMIX guest). If it banners but never reaches `login:`, the most common cause
is the disk being on the wrong SCSI id — see the SCSI-id note above.

When you issue `reboot` inside AMIX the emulator detects the warm-reset vector (the overlay isn't
restored on a 68k reset) and performs a **clean Zynq reboot** rather than flooding the bus; the
board comes back in ~2.5 minutes.

---

## Current status

AMIX 2.1 **boots reliably to a stable multiuser login shell** under this firmware: the dynamic
linker and `/sbin/init` run, the SVR4 banner and memory sizing appear, the system reaches
multi-user, and you get a root login on both the HDMI console and over telnet. It has survived
repeated warm/cold reboots (a 27-reboot soak with concurrent fork/exec load, all clean) and stays
up under sustained load.

The post-banner stalls that earlier blocked login are **resolved in practice.** The boot-time
failures came down to emulator-side 68030 bus-error-frame corruption on the demand-paging path —
fixed this cycle (see `CHANGES.md`: the in-RTE retry-access frame fix and the multi-fault
continuation fix). AMIX now boots and runs through that path cleanly, so the earlier `a3091` /
completion-ordering stall no longer occurs in normal operation.

**Known remaining issue (does not block normal use):** under *extreme* sustained demand-paging load
a rare 68030 bus-error-frame SR-flip can still trip the emulator's user-PC sanity guard. It did not
recur across a 27-reboot, multi-hour soak; normal boot and interactive use are unaffected. It is
tracked for a future fix.

The deeper guest-side analyses (the `a3091` driver disassembly, the `lpsched` memory-coherency
investigation) are retained under [`docs/investigations/`](investigations/) for reference and for
anyone who wants to push emulation fidelity further.

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
