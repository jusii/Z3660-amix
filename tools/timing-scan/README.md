# timing-scan: measure the Z3660 bus timing window

A bench instrument that finds, per card, the range of clock phases inside which the 68060 and the
Amiga motherboard talk without errors, and reports the centre of that range. It has three parts:

| Part | File | Runs on |
|---|---|---|
| Console primitives | `z3660-firmware/.../Z3660/src/zt_scan.c` (build flag `ZT_SCAN`) | the Z3660's ARM |
| Bus test program | `ztest.s` → `ztest.rom` | the 68060 (or the emulated 68040) |
| Scan loop | `zscan.py` | the host, over the serial console |

## Firmware

Build core0 with `-DZT_SCAN=1`. Without the flag `zt_scan.c` is empty and the image is the plain build.
The hooks in `debug_console.c` are line-neutral, and only the date string and the GNU build-id differ.
The PL bitstream is untouched, so a window measured with the scanner image holds for the plain one.

One-line console commands (each reply is one line starting `ZT `):

| Command | Does |
|---|---|
| `TPH <pclk> <clken> <bclk> <cpuclk> <clk90>` | stage the phases of the current row in RAM; the first stage snapshots the boot row |
| `TROW <mhz>` | stage another row (`config.cpufreq`) |
| `TAPPLY PARK` | ARM takes the bus (PS_MIO_15 grant), reprograms the clock wizard, releases. Needs the program parked. |
| `TAPPLY RESET` | CPLD reset (an Amiga reset), reprogram, FPGA reset pulse (re-latches MAPROM), release |
| `TAPPLY LIVE` | reprogram with nothing held (the emulated modes) |
| `TMB` | the mailbox decoded: heartbeat, the live `where` marker, per path err/ops/berr/first-address/xor/presence |
| `WM <addr> <v>...` | write the parameter block ($0FFF0200–$0FFF02FC only), 68k longwords in hex |
| `TSTAT` | reset and bus-request pins, wizard registers, the row in file units, apply counters, ARM MHz |
| `TREST` | restore the boot row and apply it |

Every apply checks the row before writing: VCO 600–1600 MHz, PFD ≥ 10 MHz, every divider ≤ 64 (above 64
the wizard cuts the phase short). It also bounds the lock wait, which the stock `configure_clk()` does not.
`SCT` is refused while a staged row differs from the boot row, so a scan value never reaches the SD.

## Test program

`./build.sh` assembles `ztest.rom` with vasm (docker image `amigadev/crosstools:m68k-amigaos`) and fixes its
Kickstart-style checksum. Put it on the card as a kickstart file and select it in a preset:
- `kickstart` = its slot, `ext_kickstart 0`;
- `cpu_ram YES`, `autoconfig_ram YES`, `autoconfig_rtg YES`;
- no SCSI unit.

The program runs from the reset vector of either socket ROM set ($F800D2 Kickstart, $F800D6 DiagROM), and
from the image's own vectors under the emulator.

| Path | What | Emulated 040 |
|---|---|---|
| 1 chip | chip RAM, rotating 4 KB chunks, four patterns plus byte/word lanes | bus |
| 2 mobo | Ramsey RAM, 1 MB blocks found by tag sizing | bus |
| 3 card | card RAM through the CPU-RAM decode, plus MOVE16 bursts | not exercised |
| 4 z3 | card RAM through its Zorro III window (the program configures the board) | not exercised |
| 5 rom | the socket ROM through the overlay at $000000, 4 KB chunk sums | not exercised |
| 6 cia | stopped-timer latches of both 8520s, CIA-A DDRB | bus |
| 7 acfg | the motherboard autoconfig chain behind the card's boards, hashed | not exercised |
| 8 self | the image through the MAPROM decode, 4 KB chunk sums | not exercised |

The mailbox is at $0FFF0000, the parameter block at $0FFF0200, and the mirror at $0FFF0800. The primary
record is live during a pass, so the mirror holds the last consistent publish.

`ztest_sim.py` runs the image on a Musashi 68040 (`pip install machine68k`) against a model A4000 and
checks 12 scenarios: sizing, epoch, PARK, restart, and injected faults landing on the right path and bits.

## Scan loop

```
zscan.py --dry-run --card ddev --box a4000d --row 80 --mode cpu --out DIR      # simulated card
zscan.py --shim '<bench shim>' --card ddev --box a4000d --row 80 --mode cpu --arm-mhz 1000 --out DIR
```

1. **Baseline at the card's own values.** The program sizes the paths and the present mask is frozen, so a
   timing failure is never read as "absent". The baseline must run error-free.
2. **Warm-up** at the baseline values.
3. **Pass A:** CPUCLK with CLK90 locked 90° behind it.
4. **Pass B:** BCLK around the pass-A centre.

Each pass runs 10° steps outward until two consecutive steps fail, then 3° steps across each edge.

A silent step (no heartbeat) gets one `TAPPLY RESET`, then one power cycle, then counts as failed. A
power-on that never runs the program at the baseline values is logged as a restart failure, apart from the
timing results.

Options: `--passes cpuclk,bclk,clk90off,pclk`. `clk90off` steps the CLK90 offset either side of 90°;
`pclk` steps the 060/FPGA pair, for rows where the card-RAM paths fail first.

Outputs:
- **raw log:** one `ZT1` line per step. It records CPU type and stepping, ARM MHz, dividers, the requested and
  rounded phases, BCLK − CPUCLK in degrees and ns, err/ops/berr per path, and the verdict.
- **summary CSV:** the community tracker's columns plus dividers, bus MHz and the window per pass and path.
- **timings block:** the window written as comment lines.

The bench shim is site-specific (serial transport, power socket, power log) and is not part of this tree.
`zscan.py` needs two verbs from it:
- `send <line>...` prints one `ZT ...` reply per line;
- `cycle` power-cycles the box and waits for `060 starting now`.
