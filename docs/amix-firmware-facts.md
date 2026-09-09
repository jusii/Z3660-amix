# AMIX firmware facts — things that are true of this firmware and cost time to learn

Established on metal 2026-09-08/09 while preparing a v023 card for hand-over, plus the source
reading that explains each one. Everything here is **firmware-side**: it is about what this fork
presents to the guest and what its boot path does, not about the AMIX kernel.

Each item states the evidence and the source reference, so you can re-verify rather than trust
this page. Where something is a single observation rather than a measured rate, it says so.

---

## 1. `aen0` can never come up under this firmware — `zen0` is the only ethernet

AMIX's stock `aen` driver binds to a Commodore A2065. Its autoconfig probe hunts for
manufacturer/product `0x02020070` (`aenautoconfig`, `.text+0x15DE6` — established by the
kernel-side driver work, not by this repo).

This firmware cannot satisfy that probe, for two independent reasons:

* **Every emulated board it presents carries `MANUF_ID 0x144B`** — `cpu_emulator.cpp:263`,
  and the id is assigned unconditionally in both FPGA chains (see `CHANGELOG.md`, board-identity
  entry). `0x144B` (5195) is the project's own assigned manufacturer id, recorded in `README.md`.
* **A2065 emulation is compiled out.** `uae/include/sysconfig.h:79` reads
  `/* #define A2065 */ /* A2065 Ethernet card */` — commented, alongside `A2091`, `NCR`, `SANA2`
  and the rest of the optional UAE devices.

So there is no `autoconfig_*` value, and no config key of any kind, that makes `aen0` work here:
**a manufacturer id is not settable.** The ethernet path is `zen0` / `z3660eth` — product
`0x144B0001`, AMIX cdevsw major **51** — which is the interface the native driver
([`amix-z3660net`](https://github.com/jusii/amix-z3660net)) binds to.

> **The corollary that costs the hours.** Other firmware builds *do* present an A2065 façade, so
> "`aen0` works on that box" is a true statement that **does not transfer across a firmware
> boundary**. A dev SD networking on `aen0` tells you nothing about whether it will here. Check
> which image is running (§3) before concluding anything about which interface should work.

Bringing `zen0` up by hand, for a system whose rc scripts don't yet do it:

```sh
mknod /dev/zen0 c 51 0
/usr/sbin/slink                            # "link: Device busy" if already linked — harmless
/usr/sbin/slink addaen /dev/zen0 zen0
/usr/sbin/ifconfig zen0 <addr> netmask 255.255.255.0 up -trailers
/usr/sbin/route add default <gw> 1
```

Persisting it as `/etc/rc2.d/S99zen` wants a retry loop around the `addaen` — the STREAMS stack is
not ready the instant `rc2` runs. Measured bring-up on a v023 card: **t+72 s** under
`bootmode CPU` (real 68LC060), **t+156 s** under `UAE_040_MMU`, both 0 % packet loss.

---

## 2. ★ `amix_mode YES` + `bootmode CPU` silently disables SCSI boot

**Rule: a real-060 AMIX preset must set `amix_mode NO`.** It reads backwards and it is not a typo.

The chain crosses two files, which is why neither site looks alarming on its own:

```
config_file.c:1322   if(config.amix_mode) config.cpu_ram=0;   // AMIX forces a single contiguous window
main.c:1450          if(config.scsiboot && config.cpu_ram) { ...FPGA_AUTOCONFIG_BOOT_EN... }
                     else { ... DiscreteClear(REG0,FPGA_AUTOCONFIG_BOOT_EN); scsiboot_rom_loaded=0; }
main.c:1459          printf("CPURAM disabled -> SCSI boot disabled")
```

So the preset boots, the Kickstart loads, and the machine then cannot find its disk — with a
**single serial line** as the only warning. If SCSI boot ever fails to find a disk, grep the serial
for `CPURAM disabled` before suspecting the disk, the RDB or the kernel.

An **emulated** bootmode preset legitimately keeps `amix_mode YES`: on that path the CPU RAM and
autoconfig are emulated in `cpu_emulator()`, so the forcing does no harm. That is why a working
pair of AMIX presets looks self-contradictory in a summary table — the real-060 one says
`amix_mode NO` and the emulated one says `YES`, and both are correct.

`amix_ram` is inert whenever `amix_mode` is `NO`; the firmware says so at boot
(`[CFG] amix_ram N ignored: amix_mode is NO`). Leave the line alone rather than tidying it.

See also [`AMIX.md`](AMIX.md#amix_mode-yesno).

---

## 3. Which firmware image ran is a serial line, not a CRC manifest

The FSBL loads `0:/Z3660.bin` when present, falling back to `0:/FAILSAFE.bin`, then to `BOOT.BIN`
itself. It **prints which one it took** — `zynq_fsbl/main.c:455-477`:

```
[SD Init] Done (Z3660.BIN)        <- the fork's image ran
[SD Init] Done (FAILSAFE.BIN)
[SD Init] Done (BOOT.BIN)
[SD Init] FAIL (Z3660.BIN)        <- printed for each step it tried and could not load
```

**A CRC manifest of the three files does not tell you which one ran.** That distinction cost a
whole wrong hypothesis in the 2026-09-09 campaign: a firmware-mismatch theory survived until the
`[SD Init] Done (Z3660.BIN)` lines in the log killed it, and it would otherwise have led to an
unnecessary restore. All three images also report the same version string, so **identify by CRC on
disk and by `[SD Init]` at runtime** — never by the banner.

A practical consequence: because the loader falls through, **deleting `Z3660.bin` only reverts to
stock if `FAILSAFE.bin` and `BOOT.BIN` are actually stock**. See §4 for how easily they stop being.

---

## 4. The boot menu is a single-keystroke consumer, and six of its keys copy firmware images

[`AMIX.md`](AMIX.md#never-send-a-console-word-to-the-boot-menu) covers the general rule. The part
worth stating separately is what the copy keys do (`mobotest.c`, the `b/n/m/j/k/l` handler):

| key | action |
|---|---|
| `M` | copy `Z3660.bin` → `BOOT.BIN` |
| `B` | copy `BOOT.BIN` → `FAILSAFE.bin` |
| `N` | copy `BOOT.BIN` → `Z3660.bin` |
| `J` | copy `Z3660.bin` → `FAILSAFE.bin` |
| `K` | copy `FAILSAFE.bin` → `BOOT.BIN` |
| `L` | copy `FAILSAFE.bin` → `Z3660.bin` |

`D` reflashes the CPLD (destructive; recovery needs a jumper board), and `U`/`F`/`T`/`O` fetch
upstream over the network.

**What happened on 2026-09-09.** A line-oriented send — `scmd` where `skey` was required — put a
sequence of `DMS <hexaddr>` lines into the menu. Each line executed letter by letter: four `M` and
three `B` ran, so `BOOT.BIN` and then `FAILSAFE.bin` were both overwritten with the fork's image
(serial log: 08:36:02, 08:36:08, 08:37:47, 08:42:36, 08:42:40, 08:44:19, 08:44:23). Five CPLD
cycles also ran, all reprogramming the card's own `1:/z3660.jed` with a matching checksum, and the
upstream fetches all failed for want of a network path — so the outcome was a lost fallback rather
than a bricked card, by luck as much as design.

Two things generalise:

* **`D` is not what copies the images.** Its handler downloads `z3660.jed` and runs `main_xc3sprog`;
  it copies nothing. After any menu mishap, CRC all three volume-`0:` images before assuming only
  the CPLD was touched.
* **A guard that takes the caller's word for the console mode will eventually be overridden.** The
  send declared a debug mode while the board sat at the boot menu. The pattern that holds is
  `sd_open()`'s: derive the mode from the *observed* banner, never from a declaration.

---

## 5. `autoconfig_rtg NO` hard-maps the RTG/ethernet board outside the guest's autoconfig walk

With `autoconfig_rtg NO`, `reset_autoconfig()` (`cpu_emulator.cpp:209`) plants the RTG/ethernet
combo board at a fixed `0x10000000` and sets `configured_z3 |= 2` — i.e. **already configured**, so
it never enters the guest's autoconfig walk at all. The logged address depends on **both** keys:

| `autoconfig_ram` | `autoconfig_rtg` | logged `[Core1] Autoconfig RTG to` |
|---|---|---|
| YES | YES | `0x5000` |
| NO | YES | `0x4000` — measured 2026-09-09 15:05 |
| — | NO | `0x1000` — measured 2026-09-09, four boots |

(The `0x5000` row is the expected value with 256 MB of Z3 RAM configured first at `0x40000000`;
my own logs cover the other two rows only.)

**Under `bootmode CPU` no `Autoconfig RTG` line is printed at all** — verified on two real-060
boots that ran `autoconfig_rtg YES` and logged nothing. `reset_autoconfig()` is core1/emulator
machinery; the real-CPU path uses `main.c`'s FPGA gate instead, which is different machinery with
different logging. So the absence of the line on a real-060 boot says nothing about the setting.

For the RTL side of the same story — `z3660.v:393-396` setting `configured_z3[1]` — and for the
board-identity work, see [`real-cpu-autoconfig-and-ext-rom.md`](real-cpu-autoconfig-and-ext-rom.md).

---

## 6. An intermittent pre-handoff stall on the real-060 path

**Signature:** the firmware loads the Kickstart, maps the hdf, enumerates the RDB — and then stops
without ever printing `060 starting now`. Zero further log growth, grey screen. **A power cycle
cures it**, every time so far.

**Observed ~1 in 5 real-060 boots** (1 stall in 5 on 2026-09-09 with a fixed preset). Five samples
is an order of magnitude, not a rate.

**Do not confuse it with the older `Reset active` failure**, which looks completely different:
there `060 starting now` *does* print and is followed by a repeating `[Core0] Reset active (DOWN)...`
every ~3 s. That one is a preset problem (a 2.04 A3000 Kickstart on the real 060), not this.

**Where it hangs, by elimination.** The last line printed is `scsi/scsi.c:447`, inside
`piscsi_init()`'s loop over the SD's four MBR entries — silent for the remaining entries because
their type byte is 0. Everything after it is memory work with no output: the `mount_sd_root` block
is skipped when that key is `NO` (and nothing in the loop maps a drive when `mount_sd_0x76` is
`NO`), then the return, `pifloppy_init()`'s memsets, and its ADF loop, which has no body when
`z3660cfg.txt` carries no `adf` lines. That leaves **exactly one operation in the window that can
block**: `f_open(&in, DEFAULT_ROOT "z3660_scsi.rom")` at `floppy/floppy.c:206`, and the `f_lseek`
behind it. Every path out of that open prints something — `Hunk ID:` on success via
`process_hunks`, `[PIFLOPPY] Could not open …` on failure, `… shorter than 0x2600` on a short file.
**Silence means that open did not return**, which is also why no `[PIFLOPPY]` line appears at all
on a stalled boot.

It fits the behaviour: it is SD I/O, and the first access after a burst of them (the boot-ROM load,
the hdf maps, and the MBR read that produced the very line you stall on). A power cycle resets both
the card and the Zynq SD controller. And the path has no config-dependent branch anywhere in that
window, so the intermittency is timing rather than logic.

**The call it hangs in does no useful work in the shipped image.** `floppy.c:196`'s comment records
why: `0x2600` is that call site's own constant, **not** `PISCSI_DRIVER_OFFSET` (`0x1000`,
`scsi/scsi.h:74`), and in the shipped 16 KB `z3660_scsi.rom` that offset holds zeros — so the parse
has never yielded a relocation. You can watch it happen in any healthy boot log:

```
Hunk ID: 00000000 (UNKNOWN HUNK TYPE)
[!!!HUNK_RELOC] Unknown hunk type 00000000! Can't process!
Done processing hunks.
```

It was left as found deliberately, because the correct offset is an upstream question rather than a
guess to make locally. But it does mean **the one operation that can hang the boot is performing SD
I/O for no functional benefit, and is removable upstream** if it ever becomes more than a nuisance.

**Still unknown:** *which layer* hangs — FatFs, the Zynq SD driver, or the card. Separating them
needs instrumentation inside `f_open`. Also unknown whether any timeout exists: the observed
silence outlasted the whole window (six minutes) before power was cut, which is not proof it would
never have returned.

**The one-line diagnostic that would settle it:** a single `printf` immediately before the `f_open`
at `floppy.c:206` turns "silence somewhere in a window" into "confirmed hanging in the SD open".
Deliberately not applied here — it is a firmware change, and the campaign it came from ended with a
card that had passed its acceptance run.
