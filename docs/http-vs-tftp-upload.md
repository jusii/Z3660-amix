# HTTP vs TFTP upload on the Z3660 (beta 21) — measured 2026-08-02

**Verdict: for disk images, HTTP wins on both counts — ~17 % faster *and* no reboot.
For firmware it is not an option at all: the web server cannot write the boot volume.**

## Numbers

Same 12,428,476-byte payload (a BOOT.BIN, used purely as a representative image-sized file),
same host, same LAN, same board.

| Transport | Volume | Emulator | Time | Throughput |
|---|---|---|---|---|
| **HTTP `PUT`** | `1:` exFAT | **running** | 27.37 / 27.33 / 27.05 s (mean **27.25 s**) | **~456 KB/s** |
| TFTP `WRQ` | `0:` FAT32 | stopped (boot menu) | 32.05 s | ~388 KB/s |

Smaller payload, to show per-file overhead: 4,194,304 B over HTTP took 10.03 s = 418 KB/s, so the
effective rate improves with file size — the fixed cost per transfer is small but not zero.

HTTP variance across three runs was ±0.3 %, so the gap is not noise.

## The two structural findings, which matter more than the 17 %

1. **TFTP is only reachable from the boot menu.** A TFTP `GET` attempted while the emulator was
   running timed out (`curl` exit 28, 0 bytes). The server exists only in `P` mode. So every TFTP
   transfer costs a full stop/restart cycle of the Amiga — minutes of wall clock and the loss of
   whatever was running — before the transfer itself begins. **HTTP uploads happen with the guest
   running and cost nothing but the transfer.** For iterating on disk images that difference
   dwarfs the throughput gap.
2. **HTTP cannot write the boot volume.** `PUT … ?path=0:/` is answered with a 404: the web server
   mounts only `1:` (`[httpd] FATFS mounted on 1:` at startup), so `0:` is syntactically accepted
   by the destination validator and then fails to open. **Firmware flashing therefore still
   requires TFTP** (`0:/Z3660.bin`), and that is unlikely to change without mounting `0:` in the
   httpd path.

## Caveat, stated plainly

The two measurements differ in three variables at once — protocol, filesystem (exFAT vs FAT32),
and ARM load (emulating a 68030 vs idle). They could not be separated, because the one experiment
that would isolate the filesystem (HTTP → volume 0) is exactly the case the server refuses. Note
the direction of the handicap: **HTTP posted the faster time while also carrying the emulation
load**, so the protocol gap is if anything understated here.

## Practical guidance

- **Disk images (`.hdf`, `.iso`) → HTTP.** They live on `1:`, which is the only volume HTTP can
  write, and no reboot is needed: `curl -T image.hdf "http://<board>/upload/image.hdf?path=1:/hdf"`.
  The endpoint creates the destination directory if it is missing.
- **Firmware (`Z3660.bin`, `FAILSAFE.bin`) → TFTP**, from the boot menu, as before.
- The upload has **no integrity check of its own** — the server reports `UPLOAD OK` on a completed
  write, nothing more. Verify large images afterwards with the boot menu's `CRC <file>` against a
  host-side `zlib.crc32`, exactly as the firmware flash procedure does.
- There is **no delete over HTTP**. A scratch upload can only be shrunk by overwriting it with a
  small file; removing it needs the boot menu's `DEL`.

## Cleanup owed

`1:/hdf/bench.tmp` and `1:/hdf/bench4m.tmp` were left behind by this benchmark, each overwritten
down to ~50 bytes. Delete both at the next boot-menu visit (`E` → `DEL 1:/hdf/bench.tmp`).
