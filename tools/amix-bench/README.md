# 68030 UNIX CPU + I/O benchmark

A tiny, portable benchmark to compare a **real 68030 UNIX machine** against the
**Z3660-emulated 68030** (the Zynq accelerator's UAE-based 68030+MMU core, running
AmigaOS or Amiga Unix "AMIX"). Run it on any 68030 UNIX, send back the results, and
we can line your numbers up against the emulator.

## Run it

```sh
sh runbench.sh
```

Requirements: `/bin/sh`, `dd`; `compress` optional. A C compiler (`cc`/`gcc`) is needed
**only** if the bundled `dhry`/`hz` binaries don't run on your machine (see below).
It reuses-or-builds Dhrystone, runs the tests, and writes **`bench-results.<hostname>.txt`**
— send that file back.

## What it measures

| Test | What | Metric |
|------|------|--------|
| **Dhrystone 2.1** | integer CPU (Weicker's classic) | Dhrystones/sec; **DMIPS = rate / 1757** (VAX-11/780 = 1 MIPS) |
| **disk write / read** (`dd`) | sequential disk/SCSI throughput | seconds for `IOMB` MB (machine/disk dependent) |
| **mixed** (`dd \| compress`) | combined CPU + write I/O | seconds |

## Tunables (environment)

```sh
RUNS=50000      # Dhrystone iterations. The RATE is what's comparable, so any value
                # giving a >=2 s run is fine; bump it on fast machines.
IOMB=2          # disk I/O test size, MB
BENCHDIR=.      # directory for the I/O test file -- must be on a REAL disk
BUILD=1         # force compiling dhry.c from source even if a precompiled binary runs
```

## Notes

- **Dhrystone source** (`dhry.c`) is the canonical netlib 2.1 (Reinhold Weicker),
  combined into one file. The only change from pristine: the redundant K&R
  `extern int times()` declaration is removed — it conflicts with `<sys/times.h>`'s
  prototype on an ANSI `cc` (e.g. AMIX's SVR4 `cc`). Built with
  `-DHZ=<detected>` (via `getconf CLK_TCK`, else the `hz` helper's `sysconf`, else 100).
  `dhry-RATIONALE.txt` / `dhry-README_C.txt` are the original netlib docs.
- Some vintage `gcc` reject `-O2`; the runner uses `-O`.
- The shell-builtin `time` does **not** reliably print `real/user/sys` for a bare
  `time cmd | other` on SVR4 sh, so the runner wraps timed commands as
  `{ time ...; } 2>&1` — keep that if you adapt it.

## Precompiled binaries (`dhry`, `hz`)

The package ships with **`dhry` and `hz` prebuilt on AMIX** (m68k ELF, dynamically linked
against `/usr/lib/libc.so.1`; HZ=60). The runner **reuses an existing `./dhry`/`./hz` if present**
(no rebuild) — so on AMIX there is no compile step at all:

```sh
sh runbench.sh               # reuses the bundled dhry/hz here; builds them elsewhere
echo 50000 | ./dhry          # or run Dhrystone directly
./hz                         # prints this machine's clock-tick rate (HZ)
```

**To build from source instead, delete the binaries** (`rm dhry hz`) and run again — the
runner compiles `dhry.c` / `hz.c` and that fresh build is reused on later runs. (`BUILD=1`
does the same without deleting.) On a non-AMIX m68k UNIX the bundled binaries won't execute —
`rm dhry hz` there and the runner builds from source (with that machine's correct HZ). The
bundled `dhry` assumes **HZ=60**, so on a different-HZ machine, rebuild.

## Reporting back

Include in / alongside `bench-results.<host>.txt`:
- machine model, **68030 type + clock (MHz)**, FPU (68881/68882?), RAM
- OS + version (e.g. NetBSD/mac68k 9.x, SunOS 4.x, A/UX 3.x, AMIX 2.1)
- compiler + version

The emulator-side reference (AMIX 2.1 on Z3660, single data point) is recorded in the
project; your real-hardware numbers are exactly what we need to calibrate it.
