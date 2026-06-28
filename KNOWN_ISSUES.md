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
 