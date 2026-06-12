# Z3660 Docker build environment

One container with every toolchain needed to rebuild the Z3660 firmware
(Zynq-7020 `BOOT.BIN`) and the Amiga-side drivers (`.device`, `.library`,
`.card`, `0_Z3660.adf`) from a single host.

## What's inside

| Toolchain | Path in image | Notes |
|---|---|---|
| AMD/Xilinx Vitis 2023.2 + Update 2 | `/opt/Xilinx/Vitis/2023.2` | Only with `--target full` |
| `m68k-amigaos-gcc` + `vasmm68k_mot` ([AmigaPorts fork][gcc-fork] of Bebbo's amiga-gcc) | `/opt/amiga/bin` | Cross-compiles drivers |
| NDK 3.9 ASM includes | `/opt/vbcc/NDK_3.9/Include/include_i` (symlink to `/opt/amiga/m68k-amigaos/ndk-include`) | Consumed by `vasm` for assembler sources |
| amitools (`vamos`, `xdftool`) | `/usr/local/bin` | Used by `make adf` |

[gcc-fork]: https://github.com/AmigaPorts/m68k-amigaos-gcc

Two image targets, picked at build time:

- **`drivers-only`** — ~3 GB, all of the above except Vitis. Fully redistributable,
  no AMD account needed. Builds everything under `z3660-drivers/`.
- **`full`** — ~25–30 GB, drivers-only + Vitis 2023.2.2. Needs you to stage the
  Xilinx installer locally (see below). Builds the firmware as well.

## Prerequisites

- **Docker** with BuildKit (Docker 23+ has BuildKit on by default).
- **~100 GB free disk** to build the `full` image (~60 GB transient during the
  Vitis install, prunes to ~25–30 GB). The `drivers-only` image is ~3 GB.
- A free **AMD account** to download the Xilinx installer (full image only).

## Build

### Drivers-only (no AMD account needed)

```sh
./docker/build.sh --drivers-only
```

Takes ~15 minutes (most of it the amiga-gcc compile).

### Full (drivers + firmware)

1. Sign in at <https://www.amd.com/en/support/downloads/aft-fpgas-adaptive-socs.html>, download:
   - **`AMD Unified Installer for FPGAs & Adaptive SoCs 2023.2`** —
     `FPGAs_AdaptiveSoCs_Unified_2023.2_*.tar.gz`, ~100 GB.
   - **`Update 2023.2.2`** —
     `Vivado_Vitis_Update_2023.2.2_*.tar` (or `.tar.gz`), ~30 GB.

2. Extract them (`build.sh` accepts either tarballs or pre-extracted directories;
   pre-extracted is much faster):

   ```sh
   cd ~/Downloads/Vivado
   tar xzf FPGAs_AdaptiveSoCs_Unified_2023.2_*.tar.gz
   tar xf  Vivado_Vitis_Update_2023.2.2_*.tar
   ```

3. Point `build.sh` at them:

   ```sh
   XILINX_DIR=~/Downloads/Vivado ./docker/build.sh
   ```

   The default `XILINX_DIR` is `docker/xilinx/` (gitignored), but you'll
   probably keep these multi-GB archives outside the repo.

3. Build:

   ```sh
   ./docker/build.sh
   ```

   Takes ~45–60 minutes on a modern desktop. `docker/xilinx/` is gitignored so
   you can leave the installer files there indefinitely.

## Use

Bind-mounts the repo at `/work` so outputs land back in your source tree,
owned by your host user.

### Build the firmware (produces `BOOT.BIN`)

```sh
./docker/run.sh make
```

### Build the drivers + ADF

```sh
./docker/run.sh make -C z3660-drivers all adf
```

### Verify rebuilt artifacts

```sh
./docker/run.sh ./docker/verify.sh
```

### Interactive shell

```sh
./docker/run.sh
```

## Image size lever

To trim the `full` image further, edit
[install_config.txt](install_config.txt) — the `Modules=` line currently
keeps only `Zynq-7000:1`. Add other device families if you need them.

## What `make all` actually builds in the container

The driver Makefile aggregates many targets. Inside the container, the
following sub-targets compile cleanly:

| Target | Status |
|---|---|
| `make rtg` (`Z3660.card`) | works |
| `make eth` (`Z3660Net.device`) | works |
| `make scsi` (`z3660_scsi.device` + `z3660_scsi.rom`) | works |
| `make usb` (`z3660_usb.device`) | works |
| `make kickrom` (`kick060.rom` stub) | works (~444 bytes; full ROM needs you to supply a Kickstart 3.1 base ROM separately) |
| `make ahi` | **upstream: missing AHI SDK** (`proto/ahi_sub.h` not on the host) |
| `make mhi` | **upstream: Makefile bug** (uses host `cc` instead of `m68k-amigaos-gcc` for `axmp3.c`) |
| `make soft3d` (Wazp3D) | **upstream: missing Warp3D SDK** (`Warp3D/Warp3D.h` not on the host) |
| `make mpg` | **upstream: `mpg/` directory not in the tree** |
| `make ZTop` | **upstream: file is `Ztop.c` but Makefile says `ZTop.c`** (case-mismatch — works on macOS HFS+, fails on Linux). Also requires SAS/C — see "Not covered" below. |
| `make autoconfig` | **upstream: `z3660_autoconfig/` directory not in the tree**. Also requires SAS/C. |

These upstream gaps are independent of the Docker setup — they'd reproduce on
any Linux host running the existing Makefile. They're listed here so you
don't conclude the container is broken when a target fails. `git checkout
--` any binaries that `make clean` removed before `make` errored out.

## Firmware (`make` at the repo root) — upstream issue

The container ships with all the right tools (`vivado v2023.2.2`,
`arm-none-eabi-gcc` from Vitis, `mkbootimage`) and a `system.bif` invocation
that produces a valid `BOOT.BIN`. However, the firmware sources in
[z3660-firmware/Z-TURN/vitis_ide/Z3660/src/usb/asm/ch9.h](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/usb/asm/ch9.h)
use the ARMCC/IAR shorthand `__packed` (without `#define`):

```c
struct __packed usb_class_hid_descriptor {  ...  };
struct __packed usb_class_report_descriptor { ... };
```

GCC doesn't recognise that keyword, so the firmware fails to compile under
`arm-none-eabi-gcc` (which is what Vitis ships on Linux). The rest of the
codebase uses the portable form `__attribute__((packed))`, so this is
clearly two strays. Workarounds, listed in order of how invasive they are:

1. Two-line patch to `ch9.h` replacing `__packed` with `__attribute__((packed))`.
2. Adding `-D__packed=__attribute__((packed))` to the Z3660 sub-Makefile's
   `CFLAGS`.
3. Switching to the AMD-provided ARM Compiler (armclang) in Vitis — that
   compiler does predefine `__packed`. Heavier change.

Option 1 is the cleanest and probably what an upstream PR should look like.
Until that lands, the firmware build inside the container errors out at the
`usb` compilation step.

## Troubleshooting

- **`docker buildx: command not found`** — install BuildKit
  (`sudo apt install docker-buildx` on Ubuntu 22.04+, or use Docker Desktop).
- **`no Xilinx_Unified*_SFD.tar.gz found in xilinx context`** — the SFD
  installer isn't where `build.sh` looked. Check `XILINX_DIR` and that the
  filename starts with `Xilinx_Unified` and ends `_SFD.tar.gz`.
- **`xsetup --batch Install` errors out on the config file** — Xilinx may have
  changed the supported `Modules=` list on a sub-version of 2023.2. Generate a
  fresh config from inside the install_config.txt comments.
- **`make all` fails at `ZTop` or `z3660_autoconfig`** — these two sources are
  SAS/C-only. SAS/C 6.58 is not currently bundled in the container (see
  "Not covered" below). Workaround: skip those two targets with
  `make -k all || true` then `make adf`, and rely on the checked-in
  `ZTop`/`z3660_autoconfig` binaries.
- **Outputs owned by root on the host** — your host UID/GID weren't matched at
  build time. Rebuild: `./docker/build.sh` runs `id -u` / `id -g` itself.

## Not covered

- **SAS/C 6.58 inside the container** — needed only to rebuild `ZTop` and
  `z3660_autoconfig`. SAS/C is no longer hosted on Aminet and there's no
  reliable redistribution URL, so it's deferred. Pre-built binaries are
  checked into the repo, so `make adf` still produces a complete `0_Z3660.adf`
  using the existing artifacts.
- **CPLD bitstream** (`z3660.jed` for XC95144XL) — needs ISE 14.7, separate image.
- **Vivado GUI** for editing block designs — image is headless. Add X11
  forwarding to `run.sh` if needed.
- **SD card assembly** with user-provided kickstart ROMs, HDF images, timings,
  config overlays — out of scope for v1. Use `z3660-zturn_SD_content/` directly.

## License

Same as the parent Z3660 project (GPL). All files in this directory are
contributed under the same license.
