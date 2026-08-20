# Changelog

## 2026-08-20 — UAE_040_MMU: core0 never started core1 (9e1c776, adf8ea1)

First AMIX-040 metal boot (firmware `2CFA9216`) produced no core1 output and the
Amiga reset line cycled every ~3 s forever. Core1 did not crash — it was never
started, and the emulator was not involved.

- `Z3660/src/main.c` classified the boot mode with a copy-pasted list of the
  *emulator* modes that omitted `UAE_040_MMU`, so `emu` was 0 (the real-68060
  setting). `emu==1` gates the only site that starts core1, so core0 skipped it,
  took its real-CPU path and relinquished the bus to a 68060 that is not fitted.
  Log proof: `060 starting now...` — 1 occurrence in the entire serial history,
  this boot — instead of `Starting CPU emulator on Core1` (648 historically).
- The predicate was replicated **8×** (`main.c`, `debug_console.c` ×6, `rtg.c`),
  all omitting the mode. Now one `boot_mode_is_emulated()` in `config_file.h`,
  written as a **closed list of the real-CPU modes** so an appended mode defaults
  to emulated — which fails safe (core1 halts loudly) instead of handing the bus
  to absent silicon.
- New host guard `test/host/bootmode_test.cpp` (26 checks) asserts the invariant
  over every `BOOTMODE` index; proven non-vacuous against the pre-fix predicate.
  The rest of core0 is not host-compilable, so the symptom itself stays
  metal-only — stated explicitly rather than papered over.
- Permanent core1 breadcrumbs: `[Core1] alive` (first statement of `main()`) and
  `[Core1] dispatch: cfg_emu=N amix_mode=N`.
- **Nothing in the 68040 access-error frame or the `amix_mmu_on` latch was
  reached**, so neither is implicated and neither is yet metal-tested.
- Known issue, deferred: AmigaOS-side `z3660-drivers/ZTop/Ztop.c` carries the
  same class of bug — its private `BOOTMODE` enum and label table stop at
  `UAE_030_MMU`, so ZTop opened while the card runs mode 8 reads its label
  array out of bounds and cannot select the mode. Not on the AMIX boot path
  (no AmigaOS/ZTop in the chain); needs a two-line driver-side fix later.
- `BOOT.BIN` 12,762,340 B, zlib crc32 `BBD5C14F`. Build warning-free.

## 2026-08-20 — UAE_040_MMU: the 68040 fault path (10587eb, d25b073)

The two blockers the 040-readiness audit found, closed.

- **Access-error frame.** `Exception_normal()` tested `currprefs.mmu_model && nr == 2`
  — truthy for `mmu_model == 68040` — so every 68040 access error was handed the
  68030 PMMU's frame `$A`/`$B` builder and never reached the `cpu_model >= 68040`
  arm. Guarded to `== 68030`; the 040 arm now builds a real **format `$7`** frame.
  The contents were the actual defect: `case 0x7:` had the right field order but
  stacked **zeros** for every write-back slot, and the 68040 does not re-run a
  faulted write on `rte` — the store travels to the OS in WB3 for the kernel to
  complete, so zeroed slots dropped it. Now stacked from the real fault record:
  SSW, FA, EA, WB3S/WB3A/WB3D, WB2S/WB2A, PD0–PD3. `mmu_bus_error()` also now
  initialises `regs.mmu_effective_addr` (only MOVE16/MOVEM ever assigned it, so EA
  and `wb2_address` carried stale values). Field order and encodings cross-checked
  read-only against the port repo's `wb040.s` consumer offsets — exact agreement,
  including its metal-captured `WB3S=0x0081` for a user-data long write.
- **`amix_mmu_on` under 040.** Latched only in `cpummu030.cpp` on a 2 KB page size
  the 68040 does not have, so the emulated A3000 SCSI stayed in ROM-load timing for
  the whole boot. `mmu_set_tc()` now latches on `amix_mode` + the TC E-bit.
  `amix_mmu_on` = *when* (timing, both cores); new `amix_kimage_030` = *which guest*,
  gating every hard-coded `0x07xxxxxx` 68030-image read, derived from the CPU model
  in `a3000_scsi_init()` so the 040 path can never arm them.
- Host harness **126/126** (was 96) — `test_mmu040_access_error_frame` (24 checks,
  write + read fault: WB3D carries the real store value, write faults continue,
  read faults restart) and `test_amix_mmu040_latch` (6). `scsi_cd_test` 34/34.
- **Consequence worth naming: the emulator's 040 fault path is now behaviourally
  equivalent to silicon for `wb040.s`'s purposes** — deferred-write-back work
  (ISSUE-10/-22 class) should reproduce under `UAE_040_MMU`, not only on metal.
- Flagged, deliberately untouched: `a3000_scsi_init()` does not reset
  `amix_mmu_on`, so after an AMIX warm reboot the next Kickstart ROM load runs
  with runtime pacing instead of the proven immediate-completion timing — fixing
  it changes proven 030 behaviour, so it needs its own decision + metal 030
  regression. And `mmu_model == 68060` now lands on the synthesized MMU-off
  frame (inert today; a future 060 lane needs its own arm).
- `BOOT.BIN` 12,762,340 B, zlib crc32 `2CFA9216`. Build warning-free. Metal
  still untried for AMIX-040 — bench/harness-proven only.

## 2026-08-20 — UAE_040_MMU carries the AMIX contract (f7b2167, 970b33a)

`bootmode UAE_040_MMU` can now run the native AMIX 040 boot chain. Two defects
closed and the mode's gate doc brought up to date.

- **`main.cpp`**: the `UAE_040_MMU` branch passes `shared->amix_mode` into
  `uae_emulator()` (both the `MUSASHI_EMULATOR` and the plain dispatch block),
  exactly as `UAE_030_MMU` does. The whole existing AMIX contract then applies
  model-independently: single ≤16 MB window at `$08000000`, `a3000mem` at
  `$07000000`, emulated A3000 SCSI at `$00DD0000`, `a3000_scsi_init()`, and the
  `UAE_040_MMU_AMIX` banner. `UAE_040`/`UAEJIT_040` deliberately keep it off —
  no MMU / JIT-excludes-MMU, the same rule the 030 modes follow.
- **`uae_emulator.cpp`**: `fpu_no_unimplemented` is set for the 68040 iff
  `amix_mode`. The pref reads backwards — set means "the emulator does not
  implement the whole opcode set", so `fpp.cpp` raises the real 040 traps. The
  AMIX 040 kernel links Motorola FPSP and only executes it if the
  unimplemented-instruction trap (vector 11, frame format `$2`) and the
  unimplemented-data-type trap arrive. 68881/68882 and non-AMIX 040 unchanged.
- Host harness **96/96** (was 90/90): new `test_fpu040_unimplemented_trap` pins
  both directions — pref set → `FSIN` traps to vector 11 with a format `$2` /
  `$02C` frame; pref clear → same instruction executes in-emulator.
- **Operator note: `amix_mode` is NOT implied by `bootmode UAE_040_MMU`** — the
  config default resolves YES only for UAE_030_MMU. An AMIX-040 preset must
  carry `amix_mode YES` explicitly; the serial banner confirming it took is
  `UAE_040_MMU_AMIX`.
- `BOOT.BIN` 12,762,340 B, zlib crc32 `76FBA34C` (warning-free build). NOT
  flashed — see the audit below.
- **Audit (documented in `UAE_040_MMU_gate.md` §8, deliberately not fixed
  here):** two emulator-side blockers still gate an AMIX-040 boot —
  `Exception_normal()`'s `if (currprefs.mmu_model && nr == 2)` branch is truthy
  for `mmu_model == 68040` and builds a 68030 frame `$A`/`$B` for a 68040 fault
  (shadowing the `cpu_model >= 68040` arm — the first demand-paging fault gets
  a wrong-format frame), and `amix_mmu_on` — the A3000-SCSI "AMIX runtime
  timing" latch — can only be set by the 030 TC write (2 KB page gate), so the
  controller would stay in Kickstart-load timing for an entire 040 boot. The
  latch also arms hard-coded 030-kernel-image address pokes that must never
  fire against a 040 image.

## 2026-08-20 — httpd work FLASHED + METAL-VALIDATED (455A7D12 live)

`BOOT.BIN` crc32 `455A7D12` (from `7952863`) flashed to `0:/Z3660.bin` through
both deploy-kvm.sh CRC gates (staged + on-card both `455A7D12`, `COPY OK:
12762340 bytes (read == write, verified)`; `0:/FAILSAFE.bin` `7E03DA03`
untouched) and validated on the A4000+Z3660:

- New-build proof at `P`: `[httpd] FATFS mounted on 0: (read only)` — a string
  that exists only in this image.
- **`GET /download?path=0:/Z3660.bin`: 12,762,340 bytes byte-exact in 7.0 s —
  1.81 MB/s**, ~4× the measured upload rate (TCP streaming with a real window;
  upload remains ~457 kB/s).
- 1 MB round-trip on `1:` byte-identical; 80 KB asset through the static
  `1:/web` path byte-identical (the old engine truncated at 8 KB).
- `PUT ?path=0:/` with a small body: clean `404`, graceful close (curl rc=0).
  With a 1 MB body the 404 status still arrives but curl reports rc=56 — the
  server closes without draining the remaining request body, so TCP resets the
  in-flight tail. Inherent to answer-then-close; the status code is still a
  usable gate, and deliberately NOT "fixed" (draining a huge misdirected PUT
  before answering would stall the refusal for minutes).
- Traversal (`1:/../0:/…`) and unknown volume → 404.
- **40×-404 hammer then a full download on connection #41** — the CLOSE_WAIT
  pcb-strand class (server death after ~32 client-closed connections) is gone.
- Guest regression: production 030 UFS box boots to multiuser (run-level 2,
  `z3660amix`, `zen0` up at `.39`, 214 s from `R` including the normal AMIX
  bootstrap warm-reset double-cycle); boot log clean (`Emulation mode 4`,
  `68030 MMU enabled`, no SD FAIL, no panic). Clean `shutdown -i0`, box parked
  OFF.

## 2026-08-20 — httpd: single connection-teardown path (state + pcb lifetime) (7952863)

Follow-up to the streaming/download change: every way a connection could end
had its own cleanup and most were wrong. Consolidated into `http_conn_close()`,
the only place a connection ends and the only place the per-connection state is
freed. It detaches all five callbacks, then closes, then frees.

The rule the old code broke, read out of the BSP's lwIP 2.1.3: `tcp_close()`
may free the pcb before returning — `tcp_close_shutdown()` takes an RST path
that calls `tcp_free()` outright when the receive window is still short (the
request not yet `tcp_recved()`) — and an `ERR_MEM` is returned as `ERR_OK` with
a deferred close from `tcp_tmr`. After `tcp_close()` the pcb must not be used
again, and there is nothing to retry.

- **`http_send_404()` closed inline and freed nothing** — every 404 leaked a
  connection state, and because it closed before the request was `tcp_recved()`
  it hit the RST-and-free path, so the caller then used a freed pcb and the
  client got a reset instead of the 404 body. It now queues the body and sets
  `close_pending`; the close happens from `http_sent()`/`http_poll()` after the
  request is finished, which reaches the graceful FIN path.
- **`http_sent()`/`http_poll()` called `tcp_arg(pcb, NULL)` after
  `tcp_close()`** — the same use-after-close on every served response.
- **Peer FIN freed the state but never closed**, parking the pcb in CLOSE_WAIT
  permanently: a `tcp_pcb` leak (`MEMP_NUM_TCP_PCB` = 32) per client-closed
  connection, so the server eventually stops accepting.
- **`http_send_json_system()` early-returned** on snprintf/tcp_write failure
  without `close_pending` — state and pcb both stranded.
- **`http_accept()` closed the pcb and returned `ERR_MEM`** — lwIP aborts the
  pcb itself on a non-OK accept return, so that was a close plus an abort of
  the same pcb. Refusal is left to lwIP.
- `http_recv()` drops a connection handed a NULL state, establishing the
  invariant instead of each handler re-checking it.
- Host-harness negative control: the identical 14-scenario lifetime suite
  against the pre-fix tree fails 12 (leaks, use-after-free, CLOSE_WAIT
  strands); fixed tree passes all, ASan+UBSan clean. Send-engine and
  path-policy suites re-run unchanged, all green.
- Visible behavior change: a 404 is now a real HTTP response with a graceful
  FIN instead of a connection reset.
- Build: `BOOT.BIN` 12,762,340 bytes, crc32 `455A7D12`. Not flashed yet.

## 2026-08-20 — httpd: whole-file streaming + native GET /download (both volumes) (311694d..366a53a)

The boot-menu HTTP server could not send a response much larger than
TCP_SND_BUF (8192). `http_send_file()` looped `f_read(1024)` → `tcp_write()` →
`tcp_output()` and broke out on the first `tcp_write()` error; since nothing
pumps lwIP inside a callback, no ACK could arrive and no send buffer could
free up mid-loop, so every response was silently truncated at the send-buffer
boundary. Replaced with a `tcp_sent`-driven continuation (`http_send_pump()`),
with the open `FIL`, its read offset and the not-yet-accepted bytes carried in
the per-connection state; a `tcp_poll` net covers a pass that queues nothing.
Proven in a host harness on the extracted functions: the doc's 12,428,476-byte
benchmark payload delivered byte-exact through 1517 forced `ERR_MEM` refusals
(the old loop delivered 8,192 bytes of it).

- **New `GET /download?path=<vol>:/<file>`** — reads any file from `0:` or
  `1:`, same `?path=` convention (and percent-decoding) as `PUT /upload`.
  `0:` is mounted read-only in the httpd init (`f_clk_mount`, the sd_fileops
  pattern). Paths must name `0:`/`1:` and must not contain `..`; else 404.
- **`Content-Length` on all file responses**, so a short transfer is
  detectable host-side instead of silent. Downloads also get
  `Content-Disposition`. `Connection: close` unchanged.
- **`1:/web` assets share the engine** — they had the same truncation bound.
- **Uploads stay `1:`-only, now by explicit policy.** The `0:` refusal
  previously held only because that volume was unmounted; mounting it for
  reads would have made the boot volume HTTP-writable. Flashing stays on the
  boot-menu `E` → `COPY` → `CRC` gate with FAILSAFE behind it. `/api/dirs`
  (the shipped UI's upload destination selector) drops its `0:` candidate for
  the same reason — no web-UI JS change needed.
- Bounded `extract_filename()` (unbounded target length → caller-buffer
  overrun); closed files leaked by aborted transfers (`tcp_err`, recv-NULL);
  removed per-chunk console logging from the send path (`printf2` also paints
  HDMI — per-ACK logging would dominate a multi-MB transfer).
- Docs: `http-vs-tftp-upload.md` practical guidance rewritten by direction;
  the stale "deploy-kvm.sh broken on beta 21" warning retired (the script was
  rewritten to the HTTP → COPY → dual-CRC route and works).
- Build: `BOOT.BIN` 12,762,340 bytes, crc32 `C05D5F5C`. Not flashed yet;
  metal validation pending (queued: >8 KB web asset, `GET ?path=0:/Z3660.bin`
  vs on-card `CRC`, `PUT` to `0:` still 404).

## 2026-08-19 — Beta-21 consolidation FLASHED + METAL-VALIDATED (CEEC4984 live)

Same-day follow-up to the consolidation entry: `BOOT.BIN` crc32 `CEEC4984`
(from `1e47b77`; clean containerized build with the FSBL rebuilt — it reproduces
byte-identical to the committed `fsbl.elf`) was flashed to the card's
`0:/Z3660.bin` through both deploy CRC gates and validated on the A4000+Z3660:

- Cold boot verified: banner `1.03 (BETA 21)`, boot-menu `P` = HTTP server,
  file-version report `Z3660.bin v1.03 BETA 21`.
- Guest validation: the production golden (030 kernel r3 `sum -r` 55482) boots
  to multiuser with **working ethernet** (.39 answers) — piscsi, 030 MMU, eth,
  C0STAT/PSTAT all live. The B1 boot2-040 test image also booted to login
  (its zen0 stayed down — an image-lineage delta vs the live golden, not a
  firmware issue: the same firmware brought the live golden's network up).
- **Warm-reset restoration proven on metal**: guest `shutdown -i6` →
  `custom_reset` → Core0 reset → full reboot to login (the pre-restoration
  post-fsck hang class).
- The legacy "keep BOOT.BIN under ~12 MB" SD-write warning is retired: the
  12,762,340-byte image staged (TFTP), copied (`read == write, verified`) and
  CRC-matched on-card.
- cdfs/CD checks were NOT exercised (current card cfg has no `isoN` unit);
  DMA canary sums unchanged across the attempts. CD validation queued for a
  session with the golden cfg.
- Rollback set left on 1:: `Z3660-b21-CEEC4984.bin`, `Z3660-bak-BF1805DB.bin`
  (the instrumented crawl build this flash replaced), `Z3660-prod-59FAE8CA.bin`;
  `0:/FAILSAFE.bin` (`7E03DA03`) untouched.

## 2026-08-19 — Beta-21 consolidation: amix-main-2 is the single firmware line (203f108..cc54084)

`amix-main-2` is now the going-forward firmware line; `piscsi-crawl` is frozen
as reference. Content-level audit of both lines (they share **no** git ancestor
— the beta-21 resync was a re-port, so commit identity proves nothing;
`docs/port-audit-beta21-consolidation.md` holds the full table) ported the six
post-resync fixes: CLMT uninstall on `f_lseek(CREATE_LINKMAP)` failure
(d182e41), 256-item fast-seek link map (aea93bf), zero-block-size divide guard
(c0510a7), request-path `f_lseek`/`f_write` error handling (dc81392), PSTAT
per-span latency counters (9b56883), C0STAT per-task accounting (b4c2789,
extended to beta 21's floppy mailbox arms in cc54084), plus their service-path
and blocking-audit docs.

- **The audit caught a re-port regression the changelogs had hidden:** the
  core0 half of the warm-reset chain — `piscsi_init()` re-run on guest reset
  and the stale-`FPGA_INT6` clear — had been dropped while the CORE1 half
  survived, so a soft reboot (notably fsck's post-repair reboot) would hang at
  `0x00F81212`. Restored as 1f3521f + dc38a02.
- Everything else spot-checked PRESENT (030-MMU fix set, CD-ROM units,
  direct-DMA + SD-DMA + AMP core-1 coherence, RDB 512-byte scan, JIT -Os fix);
  two legitimate N/A (superseded warm-reset variant; `cdrom_units` key replaced
  by upstream's `isoN`).
- Containerized build verified twice from clean, byte-identical: `BOOT.BIN`
  12 680 420 bytes, CRC32 `E64FA66D`. Note: this exceeds the legacy ~12 MB
  SD-write warning (already exceeded pre-port) — flash procedure CRC-gates
  every hop and creates FAILSAFE.bin first.
- Trap for future line switches: `build/` survives branch switches and the
  Makefile has no header deps — wipe `Z3660/build` and `Z3660_emu/build` when
  moving between lines, or stale objects produce phantom link errors.

## 2026-08-12 — Investigation: how `amix_mode` presents RAM to AMIX, and what more than 16 MB would cost (07cecf8, docs-only)

`docs/amix-ram-presentation.md`, doc-only — no code or config touched. The 16 MB
is a **compile-time emulator memory-map decision**, not a config knob and not a
kernel limit: `amix_mode` is a boolean the whole way through (config key →
`SHARED` `0xFFFF00C0` → `uae_emulator()`'s fourth argument), and the size is the
literal in `RANGE_MAP(0x0800, 0x0900, drct_bank)` (`uae_emulator.cpp:1141`)
ended by the `dmmy_bank` line after it. Under `amix_mode` the firmware never
publishes a memory-list entry at all — it makes a range behave like RAM and
everything above it behave like nothing, and the guest discovers the extent.

- **Two independent 128 MB ceilings, and they agree**: the RTG board at guest
  `$10000000` (where `z3660eth`'s board window lives) and core 1's `RESERVED`
  (fault) page-table entry for host `0x10000000`–`0x17FFFFFF` — both inherited
  from upstream unchanged. Host DDR is free `0x08000000`–`0x18000000`, so DDR is
  not the constraint. The change surface is two `RANGE_MAP` lines plus retiring
  the vestigial `a3000mem` window whose host backing sits at `0x09000000`; the
  sharpest hazard is `get_real_address()` (`memory.h:801-810`), which aliases
  guest `$07xxxxxx` → host `$09xxxxxx` and must be dropped with the bank. Both
  page tables already cover 128 MB, the piscsi direct-DMA gate already spans
  `[0x08000000, 0x10000000)`, and `z3660eth` never DMAs into guest RAM.
- **`amix_mode` forces `cpu_ram` off but not `autoconfig_ram`**, while the
  shipped defaults and all four shipped presets set `autoconfig_ram YES` — a
  second, 256 MB pool-linked Zorro III region. Source-level observation, not a
  reproduced failure; reading the metal card's active preset resolves it.
- Risk register puts the safe cap at **64 MB** (2× margin under the ~128 MB
  describable-RAM ceiling, never bisected, and it moves as the kernel grows);
  the staged plan opens with one cheap flash whose single serial line decides
  whether the approach works at all.

## 2026-08-09 — P3 040-MMU lane, build-ahead (3.1–3.3 done; 3.4 parked)

Scoped build-ahead of a 68040-MMU emulation mode, ahead of the external 68040 AMIX
kernel that will eventually consume it (no 040 kernel exists to boot yet — every step
here is validated only as far as is possible without one). Four commits, unpushed
(fork, local-only):

- **`704b549` → `6ec08f6` — the 12 MiB `BOOT.BIN` cap is a ghost, and the 68040-MMU
  table is generated + linked.** The size gate first read NO-GO (a sibling opcode table
  overshoots the conventional ~12 MiB line), but that line proved to be a dead TFTP-era
  rule of thumb: the real cpuemu_31 (id 31 = 68040 MMU) + cpustbl_mmu040 tables were
  generated via the WinUAE-4.4.0 gencpu flow, linked in (+291 KB), and the resulting
  **12.156 MiB image booted on real metal** (cap test 2026-08-09: `[SD Init] Done
  (Z3660.BIN)`, not FAILSAFE; production `59FAE8CA` restored after). The ~12 MiB gate in
  CLAUDE.md/port-ledger is advisory, not a boot-load wall.
- **`6848894` — `UAE_040_MMU` is runnable.** `m68k_run_mmu040()` run loop, the
  `set_x_funcs()` 040 accessor arm (mmu_get/put, distinct from the 030 bank), `main.cpp`
  dispatch, and the six link-only compile bridges made faithful (the non-obvious ones —
  `do_cycles_stop`, `MakeFromSR_STOP`, `op_unimpl` — reasoned in the gate doc). Append-only
  bootmode enum (index 8, both copies). Host smoke 69/69 (runs moveq/add/move.l through the
  id-31 table, MMU off). Metal MMU-off AmigaOS boot smoke QUEUED in next-metal-session
  (crc `92E1473F`).
- **`5ebae16` — 68040 MMU address translation validated on the host, 90/90.** The 040
  analog of the 030 MMU vectors, driving the real `cpummu.cpp` engine (which turned out
  already complete — no core file needed touching): root→pointer→page table walks (4K/8K),
  TTR transparent windows, protection (invalid/write-protect/supervisor), U+M bit updates,
  ATC hit; a single-translation smoke asserts VA 0x00408678 → PA 0x0A000678 against the
  hand-computed frame.

`UAE_040_MMU_gate.md` carries the measured GO verdict (§ gate), the coverage table (§6),
and — most usefully — a written **gap list (§7)** of 68040 MMU behaviors not yet modelled,
ordered by when a booting 040 OS would hit them: #1 the format-$7 bus-error stack frame
(the parked 3.4; its frame *builder* is host-assertable ahead of a kernel, its *integration*
is not), #2 no cache model (CINV/CPUSH/MOVE16, cache-inhibit coherency), then PFLUSH/PTEST,
indirect descriptors, page-crossing faults, ATC eviction. **3.4 stays parked pending an
external 68040 kernel** (user decision); the lane is otherwise as far advanced as build-ahead
allows.

## 2026-08-06 — new carrier revision: bring-up findings and the 060 rule

- **`docs/carrier-bringup-checklist.md` (`849a4bd`).** Two findings from the new carrier
  revision's bring-up, A/B-proven against the old carrier with the same Zynq module, SD card,
  firmware (`59FAE8CA`) and A4000: **the 68060 must be physically fitted even in pure-emulation
  boot modes** — an empty socket leaves the interrupt//BERR/arbitration lines floating, which
  the boot-time memory test never drives, so the test passes while Kickstart dies 3–13 s in
  (the auto-clicker acknowledges the alert → RESET at ROM `f80de8`, endless reboot loop); and
  **`enable_test`/`test_range` as the carrier-fault discriminator** — its failure pattern
  isolated a CPLD solder bridge (32-bit chip-RAM cycles all garbage while 8-bit CIA cycles
  passed) and a stuck-high D23 (the A4000 Fast-RAM SIMM unseated during the board swaps), plus
  the operational gotchas (region prompts drain buffered `C` keys before the countdown;
  re-disable the test for unattended boots). The new carrier was validated on metal 2026-08-06:
  full stress battery — CD/DMA canaries, disk, CPU, network, warm reboot — PASS.

## 2026-08-02/03 — the web UI, the httpd defects, and the metal validation

- **`httpd` sends a well-formed response header now (`2f4e58a`).** Beta 21's 200-response ended
  every header line with a bare `\r` instead of `\r\n`, so the whole block arrived as one run-on
  status line with no blank line before the body: `HTTP/1.1 200 OKContent-Type: text/htmlConnection:
  close`. The file was transmitted intact — a raw socket read got every byte — but a conforming
  client discarded the body, which read as an empty page. `http_send_404()` two functions later
  already spelled `\r\n`, which is what identified it as a typo. `.json` was also added to
  `mime_type()`; the shipped UI fetches `web/locales/*.json` and they were going out as
  `application/octet-stream`.
- **The web UI's own files are now shipped (`2f4e58a`, `7b9b874`).** `httpd` serves from `1:/web`,
  but beta 21 ships those files only under `Z3660/src/web/` — nothing installs them, so an updated
  board answers every request with `f_open failed … 5` and a 404. Vendored into
  `z3660-zturn_SD_content/…/web/` so they travel with the card image. The markup's fallback text is
  also translated: the loader hardcodes `lang='en'` and fetches only `/locales/en.json`, but every
  literal in the HTML was Spanish — so whenever that fetch failed (i.e. always, given the header
  defect) the page silently rendered in a language the code never selected.
- **Both fixes flashed and verified on hardware 2026-08-03** (firmware crc32 `59FAE8CA`, flashed
  through the rewritten `deploy-kvm.sh`): a strict client now receives the three header lines
  separately and the full 7,410-byte body — it received **zero** before — and `locales/en.json`
  arrives as `application/json` and parses. Post-flash gates all green: `BETA 21`,
  `Emulation mode 4`, `68030 MMU enabled`, A3000 SCSI initialised, Amix to multiuser on the LAN,
  and `validate-metal.sh --cycles 5` PASS on the CD stack.
- **`docs/http-vs-tftp-upload.md` (new)** — measurements behind the workspace's move off TFTP,
  including the correction that HTTP uploads do *not* run while the guest does.
- **Defect catalogue in `docs/AMIX.md` now lists seven beta-21 defects**, none reported upstream:
  the L1-only cache maintenance, the `else`-after-`else`, the `machdep/m68k.h` include guard, the
  `isoN` slot/target trap **and its out-of-bounds write**, the malformed HTTP header, the
  uninstalled web UI, and the UI's fake file rows (`core.bin`/`config.ini`/`logo.png`) with dead
  action buttons.

## 2026-08-01 — beta-21 resync complete (branch `amix-main-2`)

Rebased the Amix fork onto upstream firmware v1.03 beta 21 (`upstream/main` `1dddf79`) on a
fresh branch — upstream rewrote history, so there is no merge base and the resync is a
guided replay, not a merge. Plan of record: `docs/port-ledger-beta21.md`.

**Steps 1–4 (`90cf432..06645d6`).** Build plumbing, `-Os` + `--gc-sections`, the
`SHARED`/`BOOTMODE`/`CONFITEM` ABI merges, and the 68030 PMMU engine + host MMU harness.

**Steps 5–10 (`ebe5381..2fea74c`).** `main.cc` is gone upstream, so its six-hunk delta moved
into `main.cpp` (the `z3660_dbg_emu` accessor, the `a3000_scsi_irq` INT2 injection,
`PISCSI_CMD_PDT`, the AMP read-coherence stash+invalidate from `0a4c064`, the PDT read case,
the `UAE_030_MMU` dispatch). The emulated A3000 WD33C93+SuperDMAC lands whole with the AMIX
memory contract and the CD-ROM CDB test; `scsi.c` regains the 512-byte RDB scan and the
`amix_mode` direct-DMA gate; `config_file` regains `amix_mode`/`service_cadence`; the console
regains DEMU/PERF/SERV alongside beta 21's DENET.

- **§4g — the flash-safety gate (`ddb7caf`).** Beta 21 had weakened our direct-DMA cache
  maintenance to **L1-only** on both the piscsi READ and WRITE paths while keeping our
  "L1+L2" comments. The full-hierarchy `Xil_DCacheInvalidateRange`/`Xil_DCacheFlushRange`
  are restored. On real hardware the weakened form can send stale DRAM to disk and let an L2
  writeback land on freshly DMA-ed bytes; amiberry models no cache hierarchy, so the bench
  cannot test this — **the first flash is the test**.
- **`isoN` adopted (`71f9240`), `cdrom_units` dropped** (grep-absent tree-wide). An ISO is an
  `hdf[]` slot flagged as a CD, so **the slot number must equal the SCSI target**: `iso4` +
  `scsi4 4`. Documented in `z3660cfg.txt`, `config_file.h` and `docs/AMIX.md`.
- **`iso7`..`iso19` are now refused (`8dad55a`).** Upstream's parser computes `index` 0..19
  but `cd_target[]` is 7 deep, so those keys wrote past it into `adf_num[]` and the head of
  `hdf[0]` — silent config corruption in shipping upstream code. Guarded, with a console
  line naming the 0–6 limit. The shipped example moves to the correct `iso4`/`scsi4 4` form
  (upstream's `iso6` + `scsi4 6` pairing produced no working CD).

**Gates at final state:** docker build green with the full engine linked; `BOOT.BIN`
12,428,476 B = 11.853 MiB, **154,436 B under the 12 MiB threshold** (+16 KiB vs the
metal-proven golden `fee33b02`); host MMU harness **64/64**; CD-CDB test **34/34**; the two
`SHARED` copies byte-identical at 51 fields; `CONFITEM`/`config_item_names[]` **129/129**
(`CONFITEM_ISO0..19` inserted at 25–44); `COMMANDS`/`command_names[]` 83/83.

**Four beta-21 defects** are recorded in `docs/AMIX.md` as upstream-report candidates: the
L1-only cache maintenance, an `else`-after-`else` in `main.cpp`'s dead preprocessor arm, the
lost two-phase include guard in `machdep/m68k.h` (blocks any x86 build — fixed here in the
x86 arm), and the `isoN` out-of-bounds write.

## 2026-07-25

- **beta-21 resync port ledger (analysis only).** `docs/port-ledger-beta21.md` (`939936b`): the
  full commit-by-commit plan for rebuilding this fork on upstream firmware v1.03 beta 21.
  Derives the true fork set (90 commits, `main..amix-main`) despite upstream's history rewrite
  leaving no merge base — the resync must be a replay, not a merge — and classifies every commit
  ABSORBED / RE-PORT / DROP with file-level evidence (2/64/18 + 4 split). Contract verdicts:
  ethernet and piscsi-mailbox deltas both **none** (`z3660_scsi_enums.h` byte-identical), so the
  Amix drivers and the amiberry emulation need no changes. Notable hazards recorded: beta 21
  transcribed our direct-DMA coherency comments but weakened the calls to L1-only (§4g — real
  stale-L2 corruption risk on any non-bounce piscsi transfer; re-port + upstream-report
  candidate); a silent cross-core `SHARED`-struct ABI collision at `0xFFFF009C` (§5); an
  invalid-C++ `else`-after-`else` in beta 21 `main.cpp` hidden by preprocessing; BOOT.BIN size
  pressure (`-O3`, gc-sections off) sequenced to be re-proven before the MMU engine in P2.
  `docs/AMIX.md` gains a pointer.

- **piscsi READ: invalidate core1's L1 after the DMA (AMP coherence).** The last T2.P3
  metal bug, and the one that made direct-DMA CD reads unusable. Core0 owns the SD
  transfer and (since e37bb43) invalidates L1+L2 for the target range *before* it — but
  **core1**, the 68k emulator, is parked in `while(shared->write_scsi==1){NOP;}` for the
  whole transfer, and a parked Cortex-A9 is still live: it speculatively refills its own
  L1D over that same range while the DMA is in flight. Nothing ever invalidated core1's
  L1 *after* the transfer, so the guest read a mix of fresh DRAM and stale cache lines.
  `write_scsi_register()` now stashes the READ descriptor as the guest programs it
  (ADDR2 = byte length, ADDR3 = DMA target) and, after the completion spin, calls
  `Xil_L1DCacheInvalidateRange(addr,len)` for READ/READ64/READBYTES only — the WRITE path
  never reads its buffer back, and its trigger-time flush is unchanged. The BSP
  clean+invalidates only the unaligned edge lines, so neighbours of odd driver buffers
  survive. Covers disk **and** CD reads, so it plausibly also closes the rare disk-read
  corruption seen earlier on this platform (the cdfs one-sector cap had been masking it
  by shortening the speculation window).
  Metal-diagnosed on the real A4000+Z3660: with the forced bounce gone, a *quiescent*
  `mount -F cdfs` read the CD byte-correctly, but under combined CD+disk load the guest
  kernel's kmem daemon panicked on a wild freelist pointer (a stale line landing on a
  kmem pool page), and a second run silently corrupted the root filesystem — two failures
  out of two. With the fix: zero panics, zero corruption, and the full R4 read suite
  byte-identical to the ISO (all 7 files `cmp`-verified host-side, including a UTF-8
  name, a three-level directory and a symlink deref), clean umount + remount.

- **De-instrument the piscsi transfer path for production.** Revert the two TEMPORARY
  diagnostic commits that cornered the (now fixed) cdfs-mount fault: the A/B discriminator
  (af29745 — it force-bounced every non-root unit, masking the direct-DMA read path
  entirely) and the per-transfer crumb instrumentation (09520e0). The SD-controller
  coherence fix those crumbs uncovered (e37bb43) is retained verbatim: the WRITE-DIRECT
  revert conflict was resolved by keeping `Xil_DCacheFlushRange` and dropping only the
  crumb call. Net `scsi.c` state = d1da9f8 + bcc8227 + e37bb43. BOOT.BIN 12,412,164 B
  (< 12 MiB gate).

## 2026-07-11

- **firmware boot menu: SD-card file manager (key 'E').** New self-contained
  module `sd_fileops.c/.h` adds an interactive `SD>` command loop over the serial
  console, reachable from `show_options()`/`main_thread()` in mobotest.c. Mounts
  both SD volumes (0: FAT boot, 1: exFAT data) and offers DIR, COPY, REN, DEL,
  MKDIR, CRC, FREE, HELP and EXIT, with explicit `vol:/path` arguments. COPY
  generalizes the fixed BOOT.bin/FAILSAFE.bin/Z3660.bin copy shortcuts: streamed
  through a 4 MB→64 KB fallback buffer (so multi-GB hdf images work), progress
  every ~10%, read-total vs write-total verify, overwrite confirm and ESC-abort.
  CRC is a zlib-compatible CRC-32. Upstream-clean (mobotest.c + the new pair
  only); +8 KB core0 text, no BOOT.BIN size change. Verified on the board
  2026-07-11: CRC matches host zlib byte-exact (incl. the freshly TFTP'd
  Z3660.bin), 943 MB image copy in 101 s with verify green. Also on branch
  `pr-sd-fileops` (same two commits on upstream 5e216af) for an eventual
  upstream PR.

## 2026-07-10

- **scsi: DMA AMIX DDR CPU-RAM buffers directly instead of bouncing.** After the DDR move
  (22ce6f2 put AMIX RAM at $08000000) the firmware's direct/bounce boundary was gated on
  `cpu_ram` — force-disabled for AMIX — while both drivers key their bounce protocol on
  `BOUNCE_THRESH = 0x08000000`. The two sides disagreed about who copies. Gate on
  `cpu_ram || amix_mode` so a buffer inside the drct_bank window
  `[0x08000000,0x10000000)` is DMAed in place, honouring the driver contract (d1da9f8).

- **scsi: make direct-DMA piscsi transfers coherent with the SD controller DMA.** The SD
  controller DMAs to DRAM *below* the PL310 L2, while the guest's freshest bytes live in
  the cache hierarchy. Reads now `Xil_DCacheInvalidateRange` before the transfer, so a
  stale dirty L2 line cannot be written back on top of freshly DMAed bytes (observed on
  real HW: a UFS superblock read returning old guest memory). Writes now
  `Xil_DCacheFlushRange` before the transfer — the write path had **no** cache maintenance
  at all and so deterministically wrote stale DRAM to disk, which physically corrupted the
  SD image's UFS superblock (e37bb43).

- **emu: stop logging the per-command piscsi PDT register poll** — serial noise on every
  command; cosmetic only (bcc8227).

## 2026-07-07

- **piscsi: read-only 2048-byte CD-ROM units.** Expose a SCSI target as a
  read-only CD-ROM backed by a raw ISO/UDF image, alongside the existing RDB
  hardfiles. `struct piscsi_dev_` gains a `pdt` (peripheral device type) field;
  a new read-only mailbox register `PISCSI_CMD_PDT` (0xA0) returns the current
  drive's `pdt` and is the single source of truth for device type. A CD-flagged
  slot in `piscsi_map_drive()` bypasses the RDB scan (a bare optical image has no
  RDB), opens its backing image read-only, reports a 2048-byte block size
  (`BLOCKS = file_size / 2048`), sets `pdt = 0x05`, and uses benign non-zero CHS
  geometry; the normal disk path is unchanged. New config key
  `cdrom_units <id[,id...]>` flags SCSI target IDs as CD-ROM units, threaded
  through `piscsi_init()` into `piscsi_map_drive()`. Removability (start/stop,
  prevent/allow, media-change, unit attention) is deferred — the medium is
  assumed always present in this phase.
- **a3000_scsi (emulated A3000 WD33C93): present a read-only CD-ROM to the stock
  Amix kernel.** A backend unit flagged as a CD-ROM (via `PISCSI_CMD_PDT`, `pdt =
  0x05`) is shown as a read-only optical drive — INQUIRY device-type 0x05 + RMB +
  `"AMIX CD-ROM"`, READ CAPACITY at 2048-byte blocks, MODE SENSE(6)/(10)
  write-protect bit with the rigid-disk geometry pages dropped, WRITE(6)/(10)
  rejected with CHECK CONDITION / DATA PROTECT, and PREVENT/ALLOW MEDIUM REMOVAL as
  a GOOD no-op. The disk path (`pdt = 0x00`) is unchanged. New host unit test
  `test/host/scsi_cd_test.cpp` asserts the exact CDB response bytes (34/34) — the
  byte-level reference the native z3660scsi driver (Path A) must match. READ TOC
  and removability/media-change are deferred to a later increment.
