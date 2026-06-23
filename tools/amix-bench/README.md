# 68030 UNIX CPU + I/O benchmark

A tiny, portable benchmark to compare a **real 68030 UNIX machine** against the
**Z3660-emulated 68030** (the Zynq accelerator's UAE-based 68030+MMU core, running
AmigaOS or Amiga Unix "AMIX"). Run it on any 68030 UNIX, send back the results, and
we can line your numbers up against the emulator.

## Run it

```sh
sh runbench.sh
```

Requirements: `/bin/sh`, a C compiler (`cc` or `gcc`), `dd`; `compress` optional.
It builds Dhrystone, runs the tests, and writes **`bench-results.<hostname>.txt`** —
send that file back.

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
```

## Notes

- **Dhrystone source** (`dhry.c`) is the canonical netlib 2.1 (Reinhold Weicker),
  combined into one file. The only change from pristine: the redundant K&R
  `extern int times()` declaration is removed — it conflicts with `<sys/times.h>`'s
  prototype on an ANSI `cc` (e.g. AMIX's SVR4 `cc`). Built with
  `-DHZ=$(getconf CLK_TCK)` for correct timing (falls back to `HZ=100`).
  `dhry-RATIONALE.txt` / `dhry-README_C.txt` are the original netlib docs.
- Some vintage `gcc` reject `-O2`; the runner uses `-O`.
- The shell-builtin `time` does **not** reliably print `real/user/sys` for a bare
  `time cmd | other` on SVR4 sh, so the runner wraps timed commands as
  `{ time ...; } 2>&1` — keep that if you adapt it.

## Precompiled binaries (AMIX / SVR4-m68k)

`dhry.amix` and `hz.amix` are the AMIX-built executables (m68k ELF, dynamically linked
against `/usr/lib/libc.so.1`; **HZ=60 baked into `dhry.amix`**). On AMIX — or any
SVR4-m68k system with that libc and a 60 Hz clock — you can skip compiling entirely:

```sh
echo 50000 | ./dhry.amix     # run Dhrystone directly
./hz.amix                    # prints the clock-tick rate (HZ) this binary assumes
sh runbench.sh               # auto-falls back to these if no cc/gcc is present
```

`runbench.sh` prefers building from source (so it picks up the machine's *correct* HZ);
it uses `dhry.amix`/`hz.amix` only when no compiler is found. **On a machine with a
different HZ (e.g. 100) or a different m68k UNIX flavour, recompile** — otherwise
`dhry.amix`'s baked-in HZ=60 skews its reported rate.

## Reporting back

Include in / alongside `bench-results.<host>.txt`:
- machine model, **68030 type + clock (MHz)**, FPU (68881/68882?), RAM
- OS + version (e.g. NetBSD/mac68k 9.x, SunOS 4.x, A/UX 3.x, AMIX 2.1)
- compiler + version

The emulator-side reference (AMIX 2.1 on Z3660, single data point) is recorded in the
project; your real-hardware numbers are exactly what we need to calibrate it.
