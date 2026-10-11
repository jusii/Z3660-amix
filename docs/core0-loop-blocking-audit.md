# Core0 main-loop blocking audit: every callee that can stall for seconds

**Status:** static analysis of the tree at `piscsi-crawl`. Every line cited below was read and
re-verified against this working tree *after* the C0STAT instrumentation landed, so the
numbers match what you will see if you open the files at this commit. Nothing here has been
observed on hardware yet. The companion runtime instrumentation that will confirm or kill
each entry is `C0STAT` (`z3660-firmware/Z-TURN/vitis_ide/Z3660/src/core0_stats.[ch]`; see
`docs/piscsi-service-path.md` §10).
**Date:** 2026-08-17
**Branch:** `piscsi-crawl` (off `amix-main`)

> **Historical branch names.** `piscsi-crawl` and `amix-main` are the pre-resync line. This fork's
> branch of record is now **`amix-main-2`** (upstream v1.03 beta 21); see
> [`port-ledger-beta21.md`](port-ledger-beta21.md). Line numbers cited below are those of the tree
> named above.

## Why this audit exists

`docs/piscsi-service-path.md` §9 records the 2026-08-17 metal numbers. The short version: the
SD transaction is **flat** (io R avg 338 µs / W avg 1382 µs over 228278 requests) while the
wall time *between* serviced requests averaged 18.4 ms and contained **single gaps of 29 s and
118 s**. FatFs and the card are exonerated for the datapath. Something reachable from core0's
cooperative main loop stops the loop dead for tens of seconds.

The loop is `Z3660/src/main.c` :1803-1834 (the `CPU_EMULATOR` arm). It is cooperative: there
is no preemption, no work queue and no watchdog on any of its callees. **Any callee that
blocks, blocks the piscsi service, the serial console and the ethernet nag together.** So this
audit asks one question of every reachable path: *can this fail to return?*

Paths below are relative to `z3660-firmware/Z-TURN/vitis_ide/Z3660/src/`, except BSP paths,
which are relative to
`z3660-firmware/Z-TURN/vitis_ide/design_1_wrapper/ps7_cortexa9_0/standalone_domain/bsp/ps7_cortexa9_0/libsrc/`.

## The loop's callees, and which C0STAT row each becomes

| Loop callee | main.c | C0STAT row |
|---|---|---|
| `debug_thread()` (1 iteration in 20) | :1810 | `DEBUG` |
| `measures_thread()` | :1815 | `MEASURES` |
| `interrupt_thread()` | :1816 | `INTERRUPT` |
| `emulator_thread()` — the mailbox service | :1818 | `EMU` + `EMU>RTGW` / `EMU>RTGR` / `EMU>SCSIW` / `EMU>SCSIR` / `EMU>RSTE` / `EMU>HOLD` |
| `emulator_reset_thread()` (1 in 40) | :1824 | `EMURESET` |
| `ethernet_thread()` | :1829 | `ETHERNET` |
| `usb_thread()` | :1830 | `USB` |
| `other_tasks()` | :1831 | `OTHER` + `OTHER>MPEG` / `OTHER>ETHT` / `OTHER>AUDIO` |

---

## Ranking

Ranked by **worst-case blocking duration and unboundedness** — by capability, not by how
likely it was on any one night. The last two columns separate those: which C0STAT row would
convict it, and how well it fits the 2026-08-17 conditions (unattended Amix miniroot install,
no guest networking up, mostly-static 80x25 console, no MPEG stream playing, serial terminal
attached and being read).

| # | Path | Bound | Worst case | C0STAT row | Fit to 08-17 |
|---|---|---|---|---|---|
| B1 | `config_file.c` :1337 SD-mount retry, **guest-reachable** | **none** | forever, in 20-40 s steps | `EMU>RTGW` / `EMURESET` | low-medium |
| B2 | BSP SD polled-driver timeout budget, with no error break above it | 10+5+10 s per call | ~15 s per failed `disk_read`, x N | `EMU>SCSIW` / `EMU>SCSIR` / `EMURESET` | **refuted for the datapath**, live elsewhere |
| B3 | `debug_console.c` :239, :243 bare-ESC UART wait | **none** | forever | `DEBUG` | **medium-high** |
| B4 | `cpu_emulator.c` :360-363 `core0_hold` spin on core1 | **none** | forever, 1 ms granular | `EMU>HOLD` | medium |
| B5 | `video.c` :763, :767 vblank edge waits | **none** | forever if no vsync | `EMURESET` | low |
| B6 | `emacps_v3_19/src/xemacps_control.c` :1001, :1081 MDIO idle poll | **none** | forever | `OTHER>ETHT` | low-medium |
| B7 | `piscsi_init()` from the reset thread — 7 CLMT builds | soft | 3-25 s healthy, x B2 if not | `EMURESET` | medium (bounded, legitimate) |
| B8 | `debug_console.c` :193 `dump_mem`, console-supplied length | length | ~4 min for `0x100000` | `DEBUG` | low (operator error) |
| B9 | `pl_mpeg_handler.c` :863-867 unclamped catch-up decode | **none** | `T x framerate` frames | `OTHER>MPEG` | **not live** (no stream) |
| B10 | `iicps_v3_18/src/xiicps_master.c` :386 I2C completion wait | 1 s x 8 | ~8 s, then self-disables | `MEASURES` | low |
| B11 | `scsi.c` :1445-1453 etc. un-rate-limited error printf | none | ~24 ms per failed request | `EMU>SCSIW` | low |
| B12 | `rtg/rtg.c` :1123 RTG single-step UART wait | **none** | forever | `EMU>RTGW` | none (off by default) |

---

### B1 — `config_file.c` :1337: an unbounded SD-mount retry loop, reachable from a guest RTG write

```c
1334	   Xil_ExceptionDisable();
1337	retry:
1338	   ret=f_clk_mount(&fatfs, Path, 1); // 1 mount immediately
1339	   if(ret!=0)
1341	      printf("Error opening SD media\nRetry in 5 seconds\n");
1342	      sleep(5);
1343	      goto retry;
```

Five instances of this shape: :516 and :990 (boot-only), and **:1337, :1598, :1649**, which are
not. There is no iteration cap and no escape — if `f_clk_mount` keeps failing, core0 never
returns. Each iteration costs `f_clk_mount` (which fans out to B2 on a sick card, so 15-35 s)
plus `sleep(5)`: 20-40 s per retry, unbounded retries.

`Xil_ExceptionDisable()` on the line above makes this worse than a loop stall: **no ISR runs
either**, so the video vblank ISR and the GEM RX ISR stop too. It is a whole-firmware freeze.

The reachability is what makes this rank first. `write_rtg_register()` is defined at
`rtg/rtg.c` :1105 and is the only function definition in the range that follows, so all four
of these call sites are inside it:

```
rtg/rtg.c:2103   if(write_env_files_boot(&env_file_vars_temp[preset_selected])==1)
rtg/rtg.c:2191   if(write_env_files(&env_file_vars_temp[preset_selected])==1)
rtg/rtg.c:2423   if(delete_selected_preset())
rtg/rtg.c:2431   if(write_env_files(&env_file_vars_temp[preset_selected])==1)
```

and `write_rtg_register()` is called from `emulator_thread()` (`cpu_emulator.c` :307) on a
**guest** bus write. Also reachable without the guest from `emulator_reset_thread()`
(`cpu_emulator.c` :405 `write_env_files`, :417 `remove_preset_file`).

**Precondition:** `f_mount` fails — card not answering, card just re-initialised
(`main.c` :811 `f_umount` calls `disk_reinitialize(0)`), or card mid-error-recovery. Exactly
the state a flaky card is in when it also produces B2.

**Fit to 08-17: low-medium.** An Amix guest has no reason to write the preset/config RTG
registers, so the guest arm probably did not fire; the reset-thread arm did have opportunities
(an install reboots). `EMU>RTGW` and `EMURESET` settle it.

### B2 — the BSP SD polled driver: ~15 s per failed transfer, and `read_disk` does not stop

Three timed waits, all via `Xil_WaitForEvent(s)`:

| Site | `Timeout` | Wall time |
|---|---|---|
| `sdps_v4_2/src/xsdps_host.c` :1568 `XSdPs_CmdTransfer` | `10000000U` | **>=10 s** |
| `sdps_v4_2/src/xsdps_host.c` :1638 `XSdps_CheckTransferDone` | `5000000U` | **>=5 s** |
| `sdps_v4_2/src/xsdps_host.c` :1687 `XSdPs_CheckBusIdle` | `10000000U` | **>=10 s** |

`Xil_WaitForEvent` (`standalone_v9_0/src/xil_util.c` :287-307) and `Xil_WaitForEvents`
(:327-349) decrement the poll count once per `usleep(1U)`. `SLEEP_TIMER_BASEADDR` is **not**
defined for this BSP, so `usleep` resolves to `usleep_A9`
(`standalone_v9_0/src/usleep.c` :67-83), which is a **busy-wait** on the global timer:

```c
 74	   XTime_GetTime(&tCur);
 75	   tEnd = tCur + (((XTime) useconds) * COUNTS_PER_USECOND);
 76	   do
 77	   {
 78	      XTime_GetTime(&tCur);
 79	   } while (tCur < tEnd);
```

So each timeout is >=10 s of core0 *spinning*, not sleeping. A failed read is typically
10 + 5 = **~15 s**; worst case, with `CheckBusIdle` and a `CMD16` also timing out, ~35 s.

The multiplier is in this repo, and it is a real defect: `read_disk()` (`scsi.c` :1248) and
`write_disk()` (:1266) loop over chunks and **print on error without breaking**:

```c
1256	      DRESULT res = disk_read(0,(uint8_t *)map+block_count*d->block_size,d->lba + d->start_block+block_count,blocks);
1257	      if(res!=RES_OK)
1258	      {
1259	         printf("SCSI ERROR!!! disk_read result=%d\n",res);
1260	         printf("SCSI ERROR!!! block count=%ld\n",blocks_to_read);
1261	      }
1262	      blocks_to_read_copy-=blocks;
```

`MAX_BLOCKS_AT_ONCE` is 4096, so a large request keeps issuing timing-out reads: N x 15 s.
29 s ~= 2 x 15 s and 118 s ~= 8 x 15 s, which is why this looked like the leading candidate
before the numbers came in.

**Fit to 08-17: refuted for the piscsi datapath.** The PISCSI_STATS `io` span brackets exactly
`f_read`/`f_write`, and its **maximum** stayed in the low milliseconds all run. No SD timeout
happened inside a piscsi transfer. It remains live for SD access *outside* that span:
`f_clk_mount` under B1, the CLMT builds under B7, and the FAT-window reads inside `f_lseek` —
which land in PISCSI_STATS' `seek` span. **`seek` was not quoted from the 08-17 run and must
be captured next time**; it is the one datapath span that could still hide a B2 hit.

### B3 — `debug_console.c` :239, :243: a bare ESC on the serial console hangs core0 forever

```c
 237	            if(c==ESC)
 238	            {
 239	               while(XUartPs_IsReceiveData(STDIN_BASEADDRESS)==0);
 240	               c=XUartPs_ReadReg(STDIN_BASEADDRESS, XUARTPS_FIFO_OFFSET);
 241	               if(c=='[')
 242	               {
 243	                  while(XUartPs_IsReceiveData(STDIN_BASEADDRESS)==0);
```

The outer `PT_WAIT_UNTIL(pt,XUartPs_IsReceiveData(...))` at :228 yields properly. These two
inner waits do **not**. Once a `0x1B` has been consumed, core0 spins until the next byte
arrives — and if that byte is `[`, until a third arrives.

**Worst case: unbounded, and at human/terminal timescale rather than hardware timescale.**
That is what makes it the best structural explanation for *arbitrary, non-round* durations like
29 s and 118 s: a hardware timeout produces multiples of 5 s or 10 s; a partial escape sequence
produces whatever interval elapses before the next keystroke or the next line-noise byte.

**Precondition:** one `0x1B` on the console RX not immediately followed by the rest of a CSI
sequence. Sources: an arrow or function key pressed in the attached terminal, a
terminal-emulator status query, a `screen`/`tmux`/`ssh` session emitting a partial sequence, or
RX line noise.

**Fit to 08-17: medium-high.** A serial terminal was attached and being read for the whole run
— that is where the PSTATR numbers came from. `DEBUG` convicts or clears it outright, and the
`*BLOCK*` line will name it the moment the wait ends.

### B4 — `cpu_emulator.c` :360-363: the `core0_hold` handshake has no deadline

```c
 356	      else if(shared->core0_hold==1)
 357	      {
 359	         shared->core0_hold_ack=1;
 360	         do
 361	         {
 362	            usleep(1000);
 363	         }while(shared->core0_hold==1);
```

No cap, no deadline. Core0 waits exactly as long as core1 leaves `core0_hold` set, in 1 ms
steps — so **29000 or 118000 iterations give exactly 29 s or 118 s**. The loop also does not
`dsb()` or invalidate before re-reading `shared->core0_hold`, so if `shared` is not mapped
strongly-ordered the clear can in principle be missed indefinitely.

**Precondition:** core1 raises `core0_hold` and then blocks, faults, or takes an abort before
clearing it. **Fit: medium** — `EMU>HOLD` is a clean yes/no, and this is the only suspect whose
granularity matches the observed magnitudes arithmetically.

### B5 — `video.c` :763, :767: unbounded vblank edge waits inside `reset_run()`

```c
 763	   while(video_formatter_read(0)==1) //wait vblank
 764	   {
 765	      play_sound();
 766	   }
 767	   while(video_formatter_read(0)==0)
 768	   {
 769	      play_sound();
 770	   }
```

Reached from `emulator_reset_thread()` (`cpu_emulator.c` :390 `reset_run(...)`), which calls it
once per iteration of its own reset loop — 240+ times per 68k reset. Normally ~16.7 ms per call
(one 60 Hz frame), which is *by design* and is what makes the reset loop take 4 s. **Worst
case: forever**, if the video formatter is not producing vsync — HDMI PLL unlocked,
`video_mode_reset()` in flight, or the formatter clock stopped. `play_sound()` inside the loop
touches the RTG registers, so it spins at full speed. Same construct unguarded at `video.c`
:241-242 and :267-268.

**Fit: low** — needs a guest reset coinciding with a dead vsync. Shows up as `EMURESET`.

### B6 — `xemacps_control.c` :1001: MDIO idle poll, no timeout, retried forever while the link is down

```c
 997	do {
 998		Ipisr = XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,
 999					  XEMACPS_NWSR_OFFSET);
1000			IpReadTemp = Ipisr;
1001		} while ((IpReadTemp & XEMACPS_NWSR_MDIOIDLE_MASK) == 0x00000000U);
```

(identical in `XEmacPs_PhyWrite` at :1081.) No cap. Reached as `other_tasks()`
(`rtg/rtg.c` :461) -> `ethernet_task()` (`ethernet.c` :389) -> `ETH_TASK_NEGOTIATE` (:402) ->
`micrel_auto_negotiate_step2()` (`ethernet.c` :922) -> two `XEmacPs_PhyRead` calls.
**`ethernet_task` stays in `ETH_TASK_NEGOTIATE` indefinitely while the link is down** (:405
only advances on success), so this re-runs every `idle_task_count_max` = 300000 loop passes,
forever.

Normally ~25 µs per MDIO frame. Unbounded if MDIO never returns to idle — a live risk here
because `xEmacPsClkSetup()` reprograms the SLCR GEM0 TX divisors; a bad divisor or a gated GEM
clock leaves `MDIOIDLE` stuck at 0.

**Fit: low-medium.** Independent of the guest — this is about the PHY and the cable, not about
whether Amix has `z3660eth` loaded, so "no guest networking" does **not** clear it.
`OTHER>ETHT` settles it.

### B7 — `piscsi_init()` from the reset thread: the largest *bounded* chunk of SD I/O in the loop

`cpu_emulator.c` :448 calls `piscsi_init()` (`scsi.c` :354) on every guest reset. Per
invocation: mount, read the whole `z3660_scsi.rom`, `process_hunks`, a full
`Xil_DCacheFlush`/`Invalidate`, then **7 x `piscsi_map_drive()`** (`scsi.c` :425), each of
which does an RDB scan (`scsi.c` :619, `RDB_BLOCK_LIMIT` iterations of `f_lseek`+`f_read`),
`piscsi_find_filesystems()` — which contains an on-disk-data-driven
`while ((BE(fhb->fhb_ID) == FS_IDENTIFIER))` at `scsi.c` :769 with an `f_read` per iteration —
and `f_lseek(d->fd, CREATE_LINKMAP)` (`scsi.c` :1002), i.e. a full cluster-chain walk of the
entire `.hdf`. Plus the SD-partition maps at :452 and :465.

**Bounded but large: 3-25 s on a healthy card**, and 15 s x N if any of those reads hits B2.
This is the most likely source of a *reproducible* multi-second `EMURESET` row, and it is
legitimate work — worth knowing so it is not mistaken for a defect.

### B8 — `dump_mem` with a console-supplied length

`debug_console.c` :193, with the loop at :197: `for(uint32_t i=0;i<length;i+=4*8)` and nine
~85-char `printf`s per line. At 115200 8N1 (86.8 µs/char) a `length` of `0x100000` is 32768
lines x 85 chars x 86.8 µs = **~4 minutes** of blocking UART inside `debug_thread`. One
mistyped hex length on the console produces a multi-minute stall. **Fit: low**, but worth
knowing that `DEBUG` can show a multi-minute row for a purely operator-side reason.

### B9 — `pl_mpeg_handler.c` :863-867: unclamped catch-up decode (a stall that breeds a stall)

```c
 863	    if (last_frame_time == 0 ||
 864	        video_local_interrupt) {
 865	        int number_of_frames_to_decode=video_local_interrupt;
 866	        video_local_interrupt=0;
 867	        for(int i=0;i<number_of_frames_to_decode;i++) {
```

`video_local_interrupt` is incremented by `isr_video_timer` at the stream framerate. There is
**no clamp** on `number_of_frames_to_decode`, so it is a pure accumulator of missed deadlines:
after a T-second stall from *any* other cause, the next call runs `T x framerate` full
`plm_decode_video()` calls. At 25 fps, T = 29 s is 725 frames; at even 20-40 ms/frame that is
another 14-29 s. One stall reliably breeds a second, larger one.

Two things keep this off the top of the list. First, `other_tasks()` calls it on every
iteration (`rtg/rtg.c` :453) but it early-outs immediately when no stream is open:

```c
 834	    static int run_counter=0;
 835	    run_counter++;
 836	    if(run_counter==2)
 837	    {
 838	        run_counter=0;
 839	        return 0;
 840	    }
 841	    if (!plm_shared || !plm_decoder) {
```

so with `plm_decoder == NULL` the per-call cost is two byte-swapped stores. Second, no MPEG
stream was playing on 08-17. **Fit: not live** — but `OTHER>MPEG` is instrumented anyway,
because "unconditional on every iteration" is exactly the shape that deserves a number rather
than an argument, and because the amplifier is real once a stream *is* open. Note also that
the `run_counter==2` early-out halves the call *rate*, not the per-call work: `video_local_
interrupt` keeps accumulating during the skipped call, so each executed call decodes twice as
many frames.

Separate latent defect visible in the same lines: the `!plm_shared || !plm_decoder` guard at
:841 is followed by two **unconditional dereferences of `plm_shared`** at :842-843. If
`plm_shared` were NULL that is a data abort, not a stall.

### B10 — the LTC2990 I2C path: bounded, self-disabling, and mostly cleared

`measures_thread()` (`rtg/rtg.c` :488) drives `iic_write_ltc2990` / `iic_read_ltc2990`
(`ltc2990/ltc2990.c` :41, :85) through `PT_WAIT_UNTIL`. The protothread yields between
attempts, but each evaluation runs one `XIicPs_Master*Polled` synchronously to completion.

A **NACKing** (absent) chip cannot spin: every BSP loop's exit condition includes the NACK bit,
the helpers return 1 ("done") on error so the protothread advances rather than retrying, and a
failure counter disables the chip entirely after 99 failures. The one timed wait is
`iicps_v3_18/src/xiicps_master.c` :386, capped by `TX_MAX_LOOPCNT` = `1000000U` (:72) —
**~1 s**, x 8 steps per measurement round = ~8 s worst case, once.

The residual risk is a chip that **ACKs and then clock-stretches indefinitely**: that would
hang without bound and without any `usleep` at `xiicps_master.c` :344 or
`iicps_v3_18/src/xiicps_xfer.c` :165. **Fit: low.** `MEASURES` covers it.

### B11 — un-rate-limited error printf on the piscsi request path

The `f_lseek`/`f_write` failure sites all funnel through `piscsi_request_failed()`
(`scsi.c` :1309), which correctly limits to the first eight and then every 1024th. The
**short-count and `f_read`-result** reporting does not:

```
scsi.c:1445  printf("SCSI ERROR!!! f_read result=%d\n",res);
scsi.c:1446  printf("SCSI ERROR!!! d->fd->obj=%ld\n",(uint32_t)&d->fd->obj);
scsi.c:1452  printf("SCSI ERROR!!! bytes_to_read=%ld, bytes_read=%d\n",...);
scsi.c:1453  printf("SCSI ERROR!!! d->fd=%ld\n",(uint32_t)d->fd);
```

plus :1479, :1485-1486 (the bounce-buffer READ arm), :1628 and :1666 (the WRITE short counts).
Four ~70-char lines is ~24 ms of blocking UART **per failed request**; at the ~1000 req/s a UFS
restore can drive, that alone saturates core0. Not a seconds-scale block, but a genuine
amplifier that turns a burst of failures into a visible crawl. Would appear as an inflated
`EMU>SCSIW` cumulative *without* a large max — a distinctive signature worth recognising.

### B12 — RTG single-step UART wait

`rtg/rtg.c` :1123, inside `write_rtg_register()`:
`while(!XUartPs_IsReceiveData(STDIN_BASEADDRESS)){}` — unbounded, blocks every guest RTG write
until a key is pressed. Gated on `debug_console.debug_rtg && debug_console.step`, both default
0. **Fit: none** unless someone enabled RTG debug + step. Listed because it is one console
command away from wedging the box.

---

## Checked and cleared

- **`interrupt_thread()`** (`rtg/rtg.c` :310) — `PT_WAIT_UNTIL` on a `volatile` read. Yields.
- **`emulator_thread()`'s `PT_WAIT_UNTIL(pt,shared->shared_data==1)`** (`cpu_emulator.c` :296)
  — yields correctly. Only the `core0_hold` arm (B4) blocks.
- **`usb_thread()`'s `PT_WAIT_UNTIL(pt,usb_proxy_pending)`** (`rtg/rtg.c` :364) — yields. Every
  USB wait below it is capped: `usb_proxy.c` reset-port ~65 ms total; `usb/ehci-hcd.c` :609-626
  bounded by `USB_TIMEOUT_MS` (500 ms bulk / 100 ms control), `handshake(...)` 100 ms. Worst
  realistic `usb_proxy_handle_command` ~0.6-0.7 s. Not a seconds-to-minutes source, but 100%
  busy-wait, so it does cost core0 time — hence the `USB` row.
- **`WAIT_COUNTER`** (`rtg/rtg.c` :481, `PT_WAIT_UNTIL(pt,task_counter>=1000000)`) —
  `task_counter++` is unconditional at `main.c` :1832, so the condition always becomes true.
- **`ethernet_task()` itself** (`ethernet.c` :389) — a four-state switch with no loops and no
  sleeps; `ETH_TASK_READY` is `break;`. `RXBD_CNT` is **1** (`ethernet.c` :61), so
  `init_ethernet_buffers()` and both the RX and TX handlers have single-iteration loops. Only
  B6 blocks.
- **`ethernet.c` :1081** TX wait — 11 x `usleep(100)` = 1.1 ms, capped, with an error return.
- **`audio_silence()`** (`ax.c` :441) and **`audio_debug_timer()`** (`ax.c` :110) — bounded,
  sub-millisecond. `audio_init_i2s()` (`ax.c` :47) has no polling loop in repo code; its
  audioformatter BSP internals were not audited, which is why `OTHER>AUDIO` exists.
- **`ethernet.c` :910-914** `while(1) { ... continue; else break; }` PHY-reset poll — genuinely
  unbounded, but it is in `micrel_auto_negotiate` (**step 1**), and `ethernet_task` only calls
  `micrel_auto_negotiate_step2` (:922). Reachable from `ethernet_init()`, which the reset thread
  does call (`cpu_emulator.c` :440) — so it belongs to `EMURESET`, not `OTHER>ETHT`.
- **`ltc2990/ltc2990.c` :150-168 `test_i2c()`** — 3.18 s of `usleep`, but the only reference
  (:234) is commented out. Unreachable.
- **`rtg/rtg.c` :736** CPLD ack handshake and **`main.c` :471 / :499** `arm_write_amiga` /
  `arm_read_amiga` bus-ack spins — all unbounded, none reachable from the `CPU_EMULATOR` loop
  (`rtg_thread`, and `monitor_switch`/`flash_colors` respectively). Note `cpu_emulator.c` :469
  already documents that `arm_read_amiga()` *did* hang the firmware exactly this way.
- **`main.c` :1776 `while(shared->shared_data==1){}`** and **:1784
  `while(READ_NBG_ARM()!=0);`** — unbounded, but before the loop. Boot-only.
- **`FF_FS_EXFAT` / `FF_USE_TRIM` "defined twice" in `ffconf.h`** — **not a finding**, and worth
  recording because it is an easy misread. Lines 286/288 and 308/310 are the two arms of
  `#ifdef FILE_SYSTEM_USE_TRIM` and `#ifdef FILE_SYSTEM_FS_EXFAT`.
  `piscsi-service-path.md` §4.1 is correct as written: `FF_FS_EXFAT` is **1** and `FF_USE_TRIM`
  is **0** on this build.

## Two related findings that are not stalls

Both were found while chasing the above. Neither can produce a multi-second core0 block, and
neither is in scope for the fix wave, but they should not be lost.

1. **`scsi.c` :2060-2061 — a whole-boot-ROM double cache flush on every boot-ROM read.**

   ```c
   2060	         Xil_L1DCacheFlushRange((INTPTR)piscsi_rom_ptr,BOOT_ROM_SIZE);
   2061	         Xil_L2CacheFlushRange((INTPTR)piscsi_rom_ptr,BOOT_ROM_SIZE);
   ```

   `BOOT_ROM_SIZE` is `0x20000` (`memorymap.h` :38) = 128 KB = 4096 cache lines, cleaned in
   both L1 and L2, **per byte/word/longword the guest reads from the boot ROM**. Order
   100-300 µs per access, and the guest issues thousands during boot-ROM execution. The ROM is
   read-only after `piscsi_init()`, so this is pure waste — it makes early boot slow, not the
   steady state.

2. **`eth_backlog_nag_counter_max` can be wrapped from the console.** It is `uint32_t`
   (`rtg/rtg.c` :264) while `debug_console.c` :521-522 does `-=100` and then tests `< 0` —
   always false for an unsigned. A few `DNE` presses past zero silently disable the ethernet
   backlog interrupt for the rest of the boot. Separately, with a non-empty backlog the counter
   is bumped **twice per loop pass** (`rtg/rtg.c` :330 and :477), so
   `amiga_interrupt_set(AMIGA_INTERRUPT_ETH)` fires every ~750 passes forever while the guest is
   not draining — a guest-side interrupt storm (this is `piscsi-service-path.md` C4) that does
   **not** stall core0.

## What to do with this next

Do not fix anything from this list yet. Flash the C0STAT build, run the workload, and let the
`*BLOCK*` lines name the row. The ranking above exists so that when a row is named the
file:line is already in hand — and so that if **no** row is named, that is itself the finding:
it would mean the 118 s gap was the guest going quiet rather than core0 blocking, and the
investigation moves to core1.
