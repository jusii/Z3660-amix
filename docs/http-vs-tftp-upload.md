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

> **UPDATE 2026-08-20 — the deploy-kvm.sh warning below is retired.** An earlier revision of this
> document warned that `amix-kerntools/tools/hw-ops/deploy-kvm.sh` was broken on beta 21 because it
> pressed `P` and then TFTPed. That script has since been rewritten to the HTTP-upload → boot-menu
> `COPY` → `CRC` route proven further down, and it works on beta-21 firmware. No action needed.

> **UPDATE 2026-08-20 — download is now native.** Finding 2 below ("HTTP cannot write the boot
> volume") still holds for *writes*, and deliberately so. But the server now **reads** both volumes:
> `GET /download?path=<vol>:/<file>` streams any file off `0:` or `1:`, with a real
> `Content-Length`. See *Practical guidance*.

## Numbers

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
2. **HTTP cannot write the boot volume.** `PUT … ?path=0:/` is answered with a 404. *(As measured,
   that was an accident of mounting: `0:` was syntactically accepted by the destination validator
   and then failed to open, because the server mounted only `1:`. Since 2026-08-20 the server does
   mount `0:` — for reading — so the refusal is now written into the validator explicitly, and the
   404 is a decision rather than a side effect. Writing the boot volume stays a boot-menu-only
   operation: `E` → `COPY` → `CRC`, with `FAILSAFE.bin` behind it.)*

## Caveat, stated plainly

The two measurements differ in three variables at once — protocol, filesystem (exFAT vs FAT32),
and ARM load (emulating a 68030 vs idle). They could not be separated, because the one experiment
that would isolate the filesystem (HTTP → volume 0) is exactly the case the server refuses. Note
the direction of the handicap: **HTTP posted the faster time while also carrying the emulation
load**, so the protocol gap is if anything understated here.

## The replacement flash route — proven on hardware 2026-08-02

HTTP cannot write the boot volume (`PUT …?path=0:/` → 404, now by explicit policy — see finding 2),
and the web UI's **"Update Firmware" button has no event handler at all** — only the upload button
does. So firmware is flashed in two steps now:

1. **Upload over HTTP** (from the boot menu, after `C` → `P`):
   `curl -T fw.bin "http://<board>/upload/fw.bin?path=1:/hdf"` — 27 s for a 12.4 MB firmware.
2. **Boot menu → `E` → cross-volume copy, then verify:**
   `COPY 1:/hdf/fw.bin 0:/Z3660.bin` → `CRC 0:/Z3660.bin` → compare against the host's
   `zlib.crc32` → `EXIT` → `R`.

Verified end to end: `COPY 1:/hdf/bench.tmp 0:/flashtest.bin` reported
`COPY OK: 12428476 bytes (read == write, verified)` and `CRC 0:/flashtest.bin` returned
**`CDEF0090`** — byte-identical to the file uploaded over HTTP. Volume `0:` has 149 MB free of
196 MB, so a staging copy fits comfortably beside the three firmware images.

Since 2026-08-20 there is a third, optional step available: **`GET /download?path=0:/Z3660.bin`
pulls the flashed image back off the card**, so the on-card `CRC` can be checked against a hash of
the bytes the board actually serves, not only against the host's copy of what was sent. Useful for
capturing the currently-flashed firmware before replacing it, too — that rollback image no longer
has to be staged by hand.

Better than the old flow on every axis that survives scrutiny: faster transfer, a direct write to
the data volume, a real status code, two independent CRC gates, and one power cut instead of two.
It is *not* faster in the way the first draft claimed — the guest stops either way.

## Practical guidance

### The boot menu is single-key, and several of those keys are destructive

Everything in this section is driven by **one keystroke at a time** at the pre-boot menu. There
is no line editing, no confirmation, and no way to take a key back. `D` downloads and flashes
the **CPLD**, `O` overwrites `z3660scsi.rom`, `U` overwrites `Z3660.bin`, `F` overwrites
`FAILSAFE.bin`, `P` starts the HTTP server.

**Never send a word to this menu.** A runtime console command typed here is executed letter by
letter: `DOPC` is `D`, `O`, `P`, `C`. Console words belong to the running emulator's `C`
console, which is a different prompt reached after boot — see `AMIX.md`, *"Never send a console
word to the boot menu"*. Two adjacent traps worth knowing before driving this menu from a
script: the boot-time memory-test prompts **drain buffered `C` keys**, so power-on `C`-spam can
miss the menu (`carrier-bringup-checklist.md`), and a bare ESC on the serial console spins core0
forever (`core0-loop-blocking-audit.md` B3).

### `D` never worked: the version-file parse was truncated (fixed 2026-08-25)

Boot-menu `D` refused every CPLD download with `File z3660.jed NOT written (bad checksum)`.
BLIZZARD F4-M2 caught it on metal:

```
Checksum-32 2796438751 (A6AE44DF) 10923588 (00A6AE44)
```

The **computed** value is right — recomputing the shipped `update_system_files/z3660.jed` with
the firmware's own algorithm (sum of big-endian 32-bit words over `filesize/4` words) gives
exactly `A6AE44DF`, which is what `jed_version.txt` declares. The **expected** value was wrong:
`0x00A6AE44` is the first *six* hex digits of `A6AE44DF`, so the parser had read a truncated
string.

Cause, in `lwip.c`'s `genericRecvVersionCallback()`: it copied `p->len` — the first pbuf of a
chain it then acknowledged as `p->tot_len` — and never NUL-terminated the buffer. A version file
that arrived split was parsed short. `jed_version.txt` is the longest of the three version files
(67 bytes against 57 and 45) and the only one that crossed the split, which is why `D` alone was
broken while `U`/`F`/`T` and `O` worked.

Fixed by copying the whole chain with `pbuf_copy_partial()`, bounded and terminated. The same
pass made the field parse fail closed — `version=`, `length=` and `checksum32=` and their `\r`
terminators were each dereferenced unchecked, so a server answering with anything else wrote
through a NULL — bounded the version-string copy, and freed the pbuf on the reject path, which
leaked.

**This bug was load-bearing protection.** `D` reflashes the CPLD, the one operation on this card
that needs a jumper board and a hands-on recovery if it goes wrong
(`z3660-cpld-flash-needs-jumper`). It has been refusing to run since the update system shipped.
Now that it can run, treat it as the hazard it is: create `FAILSAFE.bin` (menu `J`) first, and
do not press `D` unless a CPLD update is actually intended.

### Getting files onto the card — `PUT /upload`, volume `1:` only

- **Disk images (`.hdf`, `.iso`) → HTTP.** They live on `1:`, which is the only volume HTTP can
  write: `curl -T image.hdf "http://<board>/upload/image.hdf?path=1:/hdf"` from the boot menu's
  `P` mode. The guest still has to stop — see the correction at the top — but the transfer is
  faster, lands directly on `1:`, and reports a status code.
  The endpoint creates the destination directory if it is missing.
- **`?path=0:/…` is refused, on purpose.** Uploads are `1:`-only. The boot volume carries
  `Z3660.bin` and `FAILSAFE.bin`, and the only supported way to replace them is the boot menu's
  `E` → `COPY` → `CRC` gate, which verifies both sides and leaves a failsafe image in place. An
  HTTP write path to `0:` would bypass all of that, so there isn't one.
- **Firmware (`Z3660.bin`, `FAILSAFE.bin`) → HTTP upload to `1:` then boot-menu `COPY` to `0:`**
  (see above). This is the route `amix-kerntools/tools/hw-ops/deploy-kvm.sh` now drives, and it
  works on beta 21. (Re-enabling `USE_TFTP` in `mobotest.c` remains a one-line uncomment if TFTP
  is ever wanted back — the server is still compiled in — but nothing needs it.)
- The upload has **no integrity check of its own** — the server reports `UPLOAD OK` on a completed
  write, nothing more. Verify large images afterwards with the boot menu's `CRC <file>` against a
  host-side `zlib.crc32`, exactly as the firmware flash procedure does.

### Getting files off the card — `GET /download`, both volumes (since 2026-08-20)

```
curl -o Z3660.bin  "http://<board>/download?path=0:/Z3660.bin"
curl -o amix.hdf   "http://<board>/download?path=1:/hdf/amix.hdf"
```

- **Both volumes are readable**, `0:` and `1:`. `0:` is mounted read-only by the server; the
  asymmetry with upload is the deliberate flash gate described above, not an oversight.
- **Same `?path=` convention as upload**, percent-decoding included, so
  `?path=1%3A%2Fhdf%2Fa.hdf` works too. The volume prefix is required; anything other than `0:`
  or `1:`, and any path containing `..`, is a 404.
- **A `Content-Length` is always sent**, so a truncated transfer is detectable rather than silent —
  `curl` reports `transfer closed with N bytes remaining to read` and exits non-zero. Worth
  scripting a size check around, since the upload direction still has no integrity signal of
  its own.
- **Whole files, any size.** The response is streamed from the `tcp_sent` callback, so it is not
  bounded by `TCP_SND_BUF` (8 KB). Before this change, any HTTP response past roughly the send
  buffer was silently cut off at that point — which is why the web UI's own assets had to stay
  tiny.
- Reads are the natural verification companion to a `CRC` on the card: download the file back and
  hash it host-side if you want an end-to-end check of a round trip.
- There is still **no delete or rename over HTTP**. A scratch upload can only be shrunk by
  overwriting it with a small file; removing it needs the boot menu's `DEL`.

## Cleanup

Done — all four scratch files (`1:/hdf/bench.tmp`, `bench4m.tmp`, `bench300.tmp`,
`0:/flashtest.bin`) were deleted at the end of the session. Free space back to
`0:` 149 MB / 196 MB and `1:` 222,756 MB / 243,806 MB.
