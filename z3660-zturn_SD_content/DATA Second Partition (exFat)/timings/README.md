# TIMING FILES
## Beta 18 and beta 19 versions:
The <b>timings.txt</b> is a text file with the name of the timings file that the ARM of the ZTurn board will use to generate all clocks needed for the Z3660.<br>

For now, we have only two sets of timings: A4000 and AA3000 (the systems that I own).<br>
The idea is that those files can be adapted to different systems, and if a user discovers that some different phases work better than others, the user can share those files and more users can benefit from it.
<br>
Starting version 1.03 BETA 18, the clock system has been changed so when an EMU is used, then the 060 is clocked to 50 MHz or less. For this reason the CPLD now needs to be programmed with the jed BETA 18 version.
<br>
## Beta 20 version:
The <b>timings.txt</b> is a text file with the name of the timings file that the ARM of the ZTurn board will use to generate all clocks needed for the Z3660.<br>

In this beta 20 version, we go back to CPLD beta 12 timings. We have a default timings file that work for A4000 and AA3000 (the systems that I own).<br>
The user can adapt the timings to different systems using the Ztop ARM app.<br>

<!-- fork:amix begin — self-contained fork section, safe to drop on an upstream merge -->

---

## Fork notes (Amix fork)

Full treatment, with source references: **[`docs/clock-timings.md`](../../../docs/clock-timings.md)**.

### The `default_timings.txt` in this directory is a template, not a backup

It was written back by a 1.03 BETA 20 firmware under the name `AA3000_timings.txt` — its own first
two lines say so — and it is a snapshot of one machine's file, close to the firmware's compiled
defaults but not identical to today's (at index 14 it is still on the beta-20 values). **If your
card's own `default_timings.txt` has been adapted — which is exactly what the paragraph above
invites you to do — copying this one over it reverts the adaptation, silently.** The tuning lives
in one 5 KB file on the SD and nowhere else, so keep a copy off the card and re-check it after any
SD refresh or firmware update.

The bench card this fork is developed on is one such case. Its file has the same **phases** as the
firmware's own defaults at every index, and differs only in how the dividers realise the clock
tree — which makes the customisation genuinely easy to miss:

| index | field | firmware default | this fork's bench card |
|-------|-------|-----------------:|-----------------------:|
| 0 (50 MHz) | `bclk` / `cpuclk` / `clk90` `.divider` | 64 | **32** — 25.00 MHz instead of 12.50 |
| 6 (80 MHz) | `global.multiplier` / `.divider` | 28 / 5 (VCO 1120) | **4 / 1** (VCO 800) |
| | `pclk.divider` / `clken.divider` | 14 / 28 | **10 / 20** |
| | `bclk` / `cpuclk` / `clk90` `.divider` | 56 | **32** — 25.00 MHz instead of 20.00 |

PCLK, AXICLK and CLKEN come out identical either way at index 6; the only net effect is the
68060 bus-side clocks. The exact blocks to restore are in `docs/clock-timings.md` §4.

### `SCT` at the debug console rewrites this file

`write_timings()` opens the file named by `timings.txt` with `FA_CREATE_ALWAYS` and rewrites all
15 index blocks from the firmware's in-RAM table — no prompt, no backup, comments and formatting
gone. Three things reach it:

* **`SCT`** (`SAVE_CURRENT_TIMINGS`) typed at the running serial debug console;
* the **ZTop timings-editor OK button**;
* the mobotest **`G`** key, which loads the compiled defaults first and so is *always* destructive
  to an adapted file.

`SCT` is the easy accident: if the firmware failed to read the timings file at boot — which it
does **silently** — the in-RAM table is the compile-time fallback, and `SCT` writes that over your
tuned file. Treat `SCT` as a write to the SD card, because that is what it is.

### `emu_extra_phase` applies only in the emulated boot modes

`configure_clk()` adds `emu_extra_phase` to `cpuclk.phase` and `clk90.phase` **only when it is
called for an emulator** (`emu != 0`). With a real 68060 the clocks are programmed with `emu = 0`,
so the value is **inert** — tuning it will not change anything a real CPU sees, and a real-CPU
measurement says nothing about whether it is right for MUSASHI / UAE / UAEJIT.

### AXI is derived from `pclk.divider`, and is not a key in this file

There is no `axi` line in the format. The parser computes `axi.divider = pclk.divider / 2` as it
reads each block, so **`AXICLK = 2 x PCLK`** in real-CPU mode — an 80 MHz PCLK gives the 160.00 MHz
the board prints. (In an emulated mode `pclk.divider` is doubled at programming time and
`axi.divider` is not, so there `AXICLK = 4 x PCLK`.)

<!-- fork:amix end -->