# Z3660 Known Issues / Limitations

<!-- fork:amix begin — self-contained fork section, safe to drop on an upstream merge -->
## Amiga UNIX (AMIX) — fork notes

This fork boots Amiga UNIX (AMIX) 2.1; full setup is in **[docs/AMIX.md](docs/AMIX.md)**. The two
things most likely to bite:

* **The AMIX disk must be on the SCSI id it was installed on** (`scsiN` in the config). AMIX bakes
  its root device to a fixed *(controller, target)* — the usual A3000 install is **id 6**
  (root mounts as `/dev/dsk/c6d0s1`). On any other id the kernel loads (you get the SVR4 banner) but
  root-mount fails and you never reach login.
* **Use an A3000-variant Kickstart** (e.g. KS 3.1 r40.68 A3000). A4000 ROMs have no A3000 SCSI driver
  and can't boot AMIX (you get "insert disk").

AMIX 2.1 boots reliably to a stable multiuser login shell. One rare demand-paging edge case under
extreme sustained load is tracked but does not block normal use — details in
[docs/AMIX.md](docs/AMIX.md#current-status).
<!-- fork:amix end -->

##Clock configuration
 * upto v1.02 CPLD and FPGA/ARM versions<br>
The frequency of 060 is limited to 50 and 100 MHz.
<br>When using EMU (MUSASHI, UAE or UAEJIT) then the 060 frequency affects to the emulation chip speed access. So the maximum chip speed is reached when clocking the system at 100 MHz (060 receives the same clock).
<br>

* v1.03 beta 4 and later FPGA/ARM versions<br>
The frequency of the 060 can be selected between 50 and 100 MHz, in 5 Mhz steps. 90 MHz and 95 MHz are a bit unstable on my systems (A4000 and AA3000). All other frequencies seems to work fine (again, on my systems).

* v1.03 beta 18 and later FPGA/ARM versions
The 060 frequency and the CPLD frequency are noe decoupled, so when using an EMU, the 060 will get 50 MHz or less.

<!-- fork:amix begin — self-contained fork section, safe to drop on an upstream merge -->
### Fork note (2026-08-24 / 2026-09-01, measured): with a REAL 060 (`bootmode CPU`), the working `cpufreq` is **per card**

The "50 to 100 MHz in 5 MHz steps" range above is a **user-facing statement that does not hold
uniformly when the 68060 is the real CPU**. Two physical Z3660 cards have now been measured, each
as a single-variable sweep (only `cpufreq` changed between boots), and **they do not agree**:

| Card | `cpufreq` | Result |
|------|-----------|--------|
| **A** — the fork's original bench card (A4000) | **100** | Boots KS 3.2.3 from reset through to the full Intuition desktop with a live mouse |
| **A** | 50 | **Nothing at all** — black HDMI output, zero serial output past the ARM banner, held 13 minutes |
| **B** — a second Z3660 (A4000D, socketed MC68LC060 rev 4, no FPU) | **50** | Boots AMIX 2.1 to a **multiuser login**, twice from cold power-on |
| **B** | 70 | Green diagnostic screen (chip RAM error) |
| **B** | 80 | Colour-screen stall at `arm_frequency` 667 and 867; blue diagnostic screen (custom chip error) at 1100 |

(Card B was not separately booted at 100; its tuned 70/80/100 rows are card A's, and the board does
not survive 70.)

The earlier conclusion here — *"treat 100 MHz as the only supported real-CPU frequency"* — is
therefore **withdrawn**. What is measured is narrower and more useful: **the working frequency is a
property of the individual board, not of this fork.** 50 and 100 MHz have each been observed to
work, on *different* cards — and 50, which boots card B twice, is the value that produced nothing
at all on card A. **Determine it per card**, and start from 50 and 100 rather than from the middle
of the range.

The fault is not the CPU and not the bus. On card B **DiagROM boots fine at 80 MHz on the real
060**, so both reach 80; AmigaOS and AMIX fail identically there, and what they do that DiagROM
never does is **Kickstart's early chipset/RAM init**. That is where it dies — which is why the
symptom is an Amiga diagnostic colour screen (**green = chip RAM error, blue = custom chip error**)
rather than a hang in a driver.

Clock timings are part of the per-card story: `1:/timings/default_timings.txt` decides what a given
`cpufreq` actually programs, and a tuned row is **not portable between cards**. Card A's 70/80/100
rows were applied to card B verbatim and confirmed on the serial — 80 MHz still failed. Only the
index-0 (50 MHz) row transferred. See [`docs/clock-timings.md`](docs/clock-timings.md).

This corroborates [`z3660-firmware/CPLD/README.md`](z3660-firmware/CPLD/README.md), which says of
the CPLD images that *"all firmware here is working only at 100 MHz. I have tried other
frequencies, but only some 'magic' frequencies work"* — i.e. the CPLD README was right that the
range above is optimistic for the real-CPU path, and right that which frequencies work is a
per-board property.

Note also that the *code* does not implement the range this section states: `configure_clk()`
clamps to `[CPUFREQ_MIN, CPUFREQ_MAX]` = `[50, 120]` (`config_file.h:333-334`) and then replaces
the requested value with a table entry (`config_clk.c:459-470`, `clk = cd[ind].clk`). So the
configured number is **not** necessarily the number applied — read the `Clock index %d` line
back from the serial log rather than trusting the config.

This says nothing about emulated boot modes, where the 060 is a bus-parked passenger and the
whole 50–100 range remains as documented above.
<!-- fork:amix end -->

##SCSI SD emulation
 * v1.01 CPLD and FPGA/ARM versions<br>
There is a bug in hunk relocation code that makes that one hdf grows upto fill the SD. Please don't use this version of firmware with SCSI SD emulation.
 <br><br>
 * v1.02 CPLD and FPGA/ARM versions<br>
 1) Copying data between partitions of different hdf files, will result in an unformatted partition and all data will be lost (on the written partition).
<br> Copying data between partitions on the same hdf file, doesn't seem to be affected by this issue.
<br>
 2) The only working filesystem is FFS (DOSx identifiers). Others are not working due a bug in hunk relocation code.
<br><br>
 * v1.03 beta 4 FPGA/ARM versions<br>
 The issues in v1.02 has been fixed. You can copy between partitions and use any filesystems.
 But still please use with caution, make always a backup of your files...
<br><br>
 * v1.03 beta 14 FPGA/ARM versions<br>
 After beta 14, you can make a third partition in the SD with type 0x76 (like you do with pistorm/emu68). It will be automounted by the SCSI SD emulation. (beta 12 and beta 13 had a bug that made some hdfs to grow up, and it was fixed in beta 14).
 
##DMA and Zorro III bus bastmer
 * v1.02 CPLD and FPGA/ARM versions<br>
The CPLD firmware doesn't implement DMA accesses, so you can't use busmaster Zorro III boards (like the A4091).
Also A4000T and A3000(T) will not work with any SCSI attached unit.
<br>A4000T can boot, but not use the internal SCSI, if you use the A4000D kickstart.
<br><br>
 * v1.03 beta 2 CPLD and FPGA/ARM versions<br>
The CPLD firmware implement DMA accesses, but EMU has not been implemented yet. So you can use DMA boards only with a 060 CPU.

##Mpeg player and USB only usable with EMU
 * v1.03 Beta 20<br>
 Starting this beta 20 version, we have a mpeg library in the ARM (pl_mpeg.h) and a PIP system to decode and play mpeg 1 files in RTG modes. (See pl_mpeg.h for file limitations). The driver needs to be adapted to apply cache coherency between 060 and ARM system.<br>
 The same applies to USB: the driver needs to apply cache coherency.<br>
 