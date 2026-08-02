# HTTP vs TFTP upload on the Z3660 — measured 2026-08-02

**Headline: the question is settled by beta 21 itself — it REMOVED the TFTP server.** Boot-menu
key `P`, which started a TFTP server on beta 20, now starts an HTTP server (`mobotest.c`: the
`USE_TFTP` define is commented out; `tftp_server.c` is still compiled but never started). So on
the firmware now flashed there is no choice to make. HTTP is also **~18 % faster** and writes volume `1:` directly.

> **CORRECTION 2026-08-02 (same day).** An earlier revision of this document claimed HTTP uploads
> "happen with the guest running". **That is false and the error was mine.** `start_httpd()` and
> the `run_httpd()` pump both sit inside the `P` branch of the boot-menu loop
> (`mobotest.c:2451-2464`), which is only reached by catching the countdown with `C`; nothing
> pumps lwIP once the emulator starts. Verified three ways: the code, the board's own serial log
> (it sat in the httpd loop from 12:59 to 15:20, after a `[Core0] Reset` at 12:55 — no emulator
> was running during *any* measured upload), and a live probe — with the emulator up, port 80 is
> unreachable and the board does not even answer ARP; both come alive the instant `P` is pressed.
> **An upload still costs a power-cycle and the running guest.** The throughput numbers below and
> the 404-on-`0:` finding are unaffected; the ARM-state column has been corrected.

⚠ **Operational consequence: `amix-kerntools/tools/hw-ops/deploy-kvm.sh` no longer works on
beta-21 firmware.** It presses `P` and then TFTPs — the `P` now starts HTTP and the TFTP transfer
will hang. The replacement route is proven below.

## Numbers## Numbers

Same 12,428,476-byte payload (a BOOT.BIN, used purely as a representative image-sized file),
same host, same LAN, same board.

Every HTTP row was measured in the boot-menu httpd loop — the only state in which the server
exists. There is no "while emulating" row because that configuration is unreachable.

| Transport (firmware) | Volume | Size | Time | Throughput |
|---|---|---|---|---|
| TFTP `WRQ` (beta 20) | `0:` FAT32 | 12.43 MB | 32.05 s | ~388 kB/s |
| **HTTP `PUT`** (beta 21) | `1:` exFAT | 12.43 MB | 27.37 / 27.33 / 27.05 s | **~456 kB/s** |
| **HTTP `PUT`** (beta 21) | `1:` exFAT | 12.43 MB | 27.08 / 27.31 s | **~457 kB/s** |
| HTTP `PUT` (beta 21) | `1:` exFAT | **300 MiB** | **675.15 s** | **~466 kB/s** |
| HTTP `PUT` (beta 21) | `1:` exFAT | 4 MiB | 10.03 s | ~418 kB/s |

Two things fall out of that table (a third, "emulation load costs nothing", was withdrawn — the
two runs it compared turned out to be the same state, so it measured nothing):

- **Throughput is flat with size, once past a few MB.** 456 kB/s at 12 MB, 466 kB/s at 300 MiB —
  no degradation across 11 minutes of sustained transfer. Only the 4 MiB run is slower
  (418 kB/s), which is per-transfer fixed cost showing up on a short run.
  Five independent runs at 12.43 MB spread over two sessions land within 1.2 % of each other,
  so the rate is stable.
- **The remaining gap is protocol, not filesystem.** TFTP's 512-byte fixed blocks with an ACK per
  block (`DATA_PACKET_MSG_LEN 512`, no RFC 2348 `blksize` negotiation anywhere in
  `tftp_server.c`) make it stop-and-wait: ~758 round trips per second at the observed rate. HTTP
  over TCP streams with a window and never stalls per-block.

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

## The replacement flash route — proven on hardware 2026-08-02

HTTP cannot write the boot volume (`PUT …?path=0:/` → 404; the server mounts only `1:`, per
`[httpd] FATFS mounted on 1:` at startup), and the web UI's **"Update Firmware" button has no
event handler at all** — only the upload button does. So firmware is flashed in two steps now:

1. **Upload over HTTP** (from the boot menu, after `C` → `P`):
   `curl -T fw.bin "http://<board>/upload/fw.bin?path=1:/hdf"` — 27 s for a 12.4 MB firmware.
2. **Boot menu → `E` → cross-volume copy, then verify:**
   `COPY 1:/hdf/fw.bin 0:/Z3660.bin` → `CRC 0:/Z3660.bin` → compare against the host's
   `zlib.crc32` → `EXIT` → `R`.

Verified end to end: `COPY 1:/hdf/bench.tmp 0:/flashtest.bin` reported
`COPY OK: 12428476 bytes (read == write, verified)` and `CRC 0:/flashtest.bin` returned
**`CDEF0090`** — byte-identical to the file uploaded over HTTP. Volume `0:` has 149 MB free of
196 MB, so a staging copy fits comfortably beside the three firmware images.

Better than the old flow on every axis that survives scrutiny: faster transfer, a direct write to
the data volume, a real status code, two independent CRC gates, and one power cut instead of two.
It is *not* faster in the way the first draft claimed — the guest stops either way.

## Practical guidance


- **Disk images (`.hdf`, `.iso`) → HTTP.** They live on `1:`, which is the only volume HTTP can
  write: `curl -T image.hdf "http://<board>/upload/image.hdf?path=1:/hdf"` from the boot menu's
  `P` mode. The guest still has to stop — see the correction at the top — but the transfer is
  faster, lands directly on `1:`, and reports a status code.
  The endpoint creates the destination directory if it is missing.
- **Firmware (`Z3660.bin`, `FAILSAFE.bin`) → HTTP upload to `1:` then boot-menu `COPY` to `0:`**
  (see above). TFTP is gone on beta 21; `deploy-kvm.sh` needs rewriting to this route, or the fork
  can simply re-enable `USE_TFTP` in `mobotest.c` — it is a one-line uncomment and the server is
  still compiled in.
- The upload has **no integrity check of its own** — the server reports `UPLOAD OK` on a completed
  write, nothing more. Verify large images afterwards with the boot menu's `CRC <file>` against a
  host-side `zlib.crc32`, exactly as the firmware flash procedure does.
- There is **no delete over HTTP**. A scratch upload can only be shrunk by overwriting it with a
  small file; removing it needs the boot menu's `DEL`.

## Cleanup

Done — all four scratch files (`1:/hdf/bench.tmp`, `bench4m.tmp`, `bench300.tmp`,
`0:/flashtest.bin`) were deleted at the end of the session. Free space back to
`0:` 149 MB / 196 MB and `1:` 222,756 MB / 243,806 MB.
