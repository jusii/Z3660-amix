# The piscsi service path: mechanism map, cost model, and the 13x crawl candidates

**Status:** §1-§8 are code analysis written before any hardware run. **§9 records the first
metal measurement (2026-08-17) and it settles several of §3's candidates — read §9 before
acting on the §3 verdict table.** §10 documents the core0 instrumentation added in response;
the static-analysis follow-up it exists to test is `docs/core0-loop-blocking-audit.md`.
**Date:** 2026-08-17
**Branch:** `piscsi-crawl` (off `amix-main`)
**Firmware under analysis:** `amix-main` — the tree that produced the prod `Z3660.bin` CRC-32 `59FAE8CA`
currently on the card. **Not** beta 21; see §6.

> **Historical branch names.** The `piscsi-crawl` and `amix-main` named above are the pre-resync
> line. This fork's branch of record is now **`amix-main-2`** (upstream v1.03 beta 21); see
> [`port-ledger-beta21.md`](port-ledger-beta21.md). The mechanism this document maps is unchanged
> by the resync — the branch names simply date it.

This document answers three questions that came out of the 2026-08-14 → 08-17 install-workload
regression:

1. Where does a guest piscsi request actually get serviced — which core, which context, which loop?
2. What does `service_cadence` control, and is it on this path at all?
3. Which candidate mechanisms can produce a ~13x I/O slowdown with the observed signature, and which
   are refuted by the code?

Everything is cited to file and line in this repo, or to the read-only sibling `../amix-z3660scsi`.

---

## 1. End-to-end request path

### 1.1 The ten-round-trip cost model

A guest block I/O does **not** cost one mailbox transaction. It costs one transaction *per register
access*, because every access to the emulated board window is a cross-core handshake. For a 2 KB READ
— which is the realistic steady-state granularity, see §1.5 — the sequence is:

| # | Guest access (`../amix-z3660scsi/src/z3660.c`) | Offset | Core1 cache op before handing off | Carries data? |
|---:|---|---|---|:---:|
| 1 | `WRLONG(P_DRVNUMX, unit)` — `z3660queue` :532 | `0x90` | none | no |
| 2 | `RDLONG(P_BLOCKSIZE0 + unit*4)` — `z3660_blocksize` :327 | `0x200+` | none (offset >= `PISCSI_DBG_MSG`) | no |
| 3 | `WRLONG(P_DRVNUMX, unit)` — again, inside `z3660_pdt` :353 | `0x90` | none | no |
| 4 | `RDLONG(P_PDT)` — `z3660_pdt` :354 | `0xA0` | none (explicit `case PISCSI_CMD_PDT: break;`) | no |
| 5 | `RDLONG(P_BLOCKS0 + unit*4)` — `z3660_nblocks` :337, called :689 | `0x220+` | none | no |
| 6 | `WRLONG(P_READ_ADDR1, block)` — `z3660_rw` :383 | `0x20` | **`Xil_L1DCacheFlush()`** (whole L1D) | no |
| 7 | `WRLONG(P_READ_ADDR2, len)` :384 | `0x24` | stash `piscsi_len_stash` | no |
| 8 | `WRLONG(P_READ_ADDR3, buf)` :385 | `0x28` | stash `piscsi_addr_stash` | no |
| 9 | `WRLONG(P_READ, unit)` :386 — **the doorbell** | `0x04` | **`Xil_L1DCacheFlush()`** (whole L1D) | **yes** |
| 10 | `RDLONG(P_USED_DMA)` :387 | `0x9C` | **`Xil_L1DCacheFlush()`** (whole L1D) | no |

Then the guest `bcopy`s 2 KB out of the bounce window if `USED_DMA != 0`.

**Nine of the ten round trips carry no payload.** Accesses 1–5 are a per-CDB preamble of *static*
per-unit facts (unit select, block size, PDT, block count) re-fetched from the firmware on every
single command; access 3 is a redundant repeat of access 1. Three of the ten trigger a **whole-L1
data-cache clean+invalidate on core1**, not a range operation.

### 1.2 Core1 (emulator) side — the handshake and the spin

`z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cc`. Note this is `main.cc`, *not* `main.cpp` —
on this fork `main.cpp` is a stale twin that the Makefile filters out (`Z3660_emu/Makefile` :58-64).

`write_scsi_register()` (:~237) does the per-command cache maintenance, publishes the request into the
cross-core `SHARED` struct, and then **spins**:

```c
   shared->write_scsi_addr=zaddr;
   shared->write_scsi_data=zdata;
   shared->write_scsi_type=type;
   shared->write_scsi=1;
   dsb();
   shared->shared_data=1;

   while(shared->write_scsi==1){NOP;}
```

`read_scsi_register()` (:268) is the same shape with `read_scsi`, ending `while(shared->read_scsi==1){NOP;}`
and returning `shared->read_scsi_data`.

So core1 — the guest's CPU — is **hard-spinning, retiring no guest instructions**, for the entire
duration of every one of the ten round trips. From the guest's point of view the store instruction
itself is the wait; that is the documented contract (`../amix-z3660scsi/src/z3660.c` :31-39) and why
`z3660intr()` is an empty stub.

After the READ spin returns, core1 does `Xil_L1DCacheInvalidateRange(piscsi_addr_stash, piscsi_len_stash)`
— the AMP coherence fix (`0a4c064`), needed because a parked Cortex-A9 still speculatively refills L1D
while it spins.

### 1.3 Core0 (control) side — a cooperative protothread loop

`Z3660/src/cpu_emulator.c` `emulator_thread()` :289-352 is the service routine:

```c
      PT_WAIT_UNTIL(pt,shared->shared_data==1);
      shared->shared_data=0;
      if(shared->write_rtg==1)        { ... }
      else if(shared->read_rtg==1)    { ... }
      else if(shared->write_scsi==1)  { handle_piscsi_reg_write(addr,data,type); dsb(); shared->write_scsi=0; }
      else if(shared->read_scsi==1)   { shared->read_scsi_data=handle_piscsi_read(addr,type); dsb(); shared->read_scsi=0; }
```

**This is the single most important scheduling fact in the whole path:**

- The piscsi service runs on **core0, in ordinary main-loop (thread) context — not an interrupt
  handler.** There is no ISR, no DMA-completion callback, no work queue.
- It is a **cooperative protothread**. `emulator_thread()` is called once per iteration of core0's
  `while(1)` loop and services **at most one** mailbox request per iteration.
- `handle_piscsi_reg_write()` runs **to completion, non-preemptibly**, including the blocking
  `f_read`/`f_write` and the SD-card transaction underneath it. While it runs, nothing else on core0
  runs — not lwIP, not the ethernet backlog service, not the serial console.
- The `if/else if` chain is **priority-ordered, RTG first**. A pending RTG access is serviced before a
  pending SCSI access.

The enclosing loop is `Z3660/src/main.c` :1789-1817 (the `CPU_EMULATOR` arm):

```c
      while(1)
      {
         { static int debug_thread_counter=0; ... if(==20) debug_thread(&pt_debug); }
         measures_thread(&pt_measures);
         interrupt_thread(&pt_interrupt);
         emulator_thread(&pt_emulator);          /* <-- the piscsi service */
         { static int reset_thread_counter=0; ... if(==40) emulator_reset_thread(&pt_emulator_reset); }
         ethernet_thread(&pt_ethernet);
         usb_thread(&pt_usb);
         other_tasks();
         task_counter++;
      }
```

Everything in that list is on the critical path of **every one of the ten round trips**. The
per-request latency is therefore roughly:

```
  t_request  ~=  10 * ( t_core0_loop_iteration + t_handshake )  +  t_SD_transaction
                 + 3 * t_whole_L1_flush  +  t_guest_bcopy(2KB)
```

`t_SD_transaction` — the part everyone assumes dominates — is one term out of five, and the only one
that scales with the amount of data moved.

`other_tasks()` (`Z3660/src/rtg/rtg.c` :396) is worth naming explicitly, because it runs
**unconditionally on every iteration**:

- `pl_mpeg_arm_decode_progressive()` — every iteration, no gate at all.
- `ethernet_task()` — gated on `idle_task_count++ > idle_task_count_max`, where
  `IDLE_TASK_COUNT_MAX` is **300000** under `CPU_EMULATOR` (`rtg.c` :265).
- an `audio_request_init` check, and the ethernet backlog nag counter bump.

### 1.4 FatFs / SD layer

`Z3660/src/scsi/scsi.c` `handle_piscsi_reg_write()` :1043. For a READ (:1054-1168):

1. `d = &devs[val]`, then a **cold absolute `f_lseek`** — `f_lseek(d->fd, (FSIZE_t)src * d->block_size)`
   (:1075 for `PISCSI_CMD_READ`; :1068 / :1083 for `READBYTES` / `READ64`). The guest supplies an
   absolute LBA every time and the firmware keeps no per-unit sequential position (`../amix-z3660scsi/src/z3660.c`
   :383, `block += chunk`). **The return value of `f_lseek` is not checked at any of the six call sites**
   (:1068, :1075, :1083, :1190, :1202, :1211). *(Fixed on this branch — §7.6.)*
2. The direct-DMA gate (:1097-1099): `(config.cpu_ram || config.amix_mode) && map>=0x08000000 && map<0x10000000`.
   With `amix_mode YES` and the AMIX kernel in Zynq-local DDR at `$08000000` this is the taken branch.
3. `Xil_DCacheInvalidateRange((INTPTR)map, len)` — L1+L2, pre-transfer (:1117).
4. `f_read(d->fd, (uint8_t *)map, len, &n_bytes)` (:1121) → `disk_read()` → `XSdPs_ReadPolled()`.
5. `Xil_L1DCacheFlush()` (:1167) — another whole-L1 flush, on core0 this time.

The WRITE path (:1169-1276) mirrors it with `Xil_DCacheFlushRange` (:1233) and `f_write` (:1237).
Note `f_write`'s return value is **also unchecked** (:1237, :1261) — only the short-count is reported.
*(Fixed on this branch — §7.6.)*

Below FatFs, `disk_read`/`disk_write` (BSP `xilffs_v5_1/src/diskio.c`) each call `disk_status(pdrv)`
first. Because `XPAR_PS7_SD_0_HAS_CD` is **1**, `disk_status()` enters its card-detect branch and reads
the present-state register on **every request**; if the card-insert bit ever reads 0 it loops
`usleep(SD_CD_DELAY)` in **10 ms** steps up to 500 iterations. That is a 10 ms-granular stall per
request, silent, with no counter.

There is **no `f_sync()` anywhere on the piscsi datapath** (only in the filesystem-save helper,
:614-628). Directory entries and exFAT metadata for the mapped `.hdf` handles are never flushed
during a run.

### 1.5 Why the request granularity is 2 KB, not 64 KB

The protocol chunks at `MAXXFER = 65536`, i.e. 128 blocks at 512 bytes. The guest never asks for that
much: raw/`physio` I/O goes `dd.c -> breakup() -> amiga_dma_pageio()` staging through a **2 KB kernel
buffer**, Amix `NBPP` is **2048**, and a `vtop()`'d buffer is valid for exactly one page. So the
steady state is one full ten-round-trip transaction per **2 KB** — about 1/32 of the protocol's own
capacity. That ratio lives in the guest, above `z3660queue()`, and is out of scope for this repo.

---

## 2. What `service_cadence` actually controls

**It is not a piscsi knob. It is not on the piscsi path at all.**

`service_cadence` is a core1 emulator knob: the number of emulated 68k instructions run between calls
to `check_uae_int_request()` in the 030-MMU run loop. `Z3660_emu/src/uae/newcpu.cpp` :4452-4456, inside
`m68k_run_mmu030()`:

```c
            // perf investigation: instruction-rate benchmark + tunable IPL/cross-core poll cadence.
            // do_specialties() stays per-instruction (STOP/trace/mode-change correctness, and its
            // STOP loop polls internally); only check_uae_int_request (which DETECTS interrupts) is
            // throttled to every z3660_service_cadence instructions -> max ~N-instruction int latency.
            z3660_perf_count++;
            if((z3660_perf_count & 0xFFFFFu) == 0) z3660_perf_tick();
            { static int serv_ctr = 0;
              if(++serv_ctr >= z3660_service_cadence){ serv_ctr = 0; check_uae_int_request(); } }
```

Semantics, precisely:

| Property | Value |
|---|---|
| What it throttles | `check_uae_int_request()` — interrupt **detection** (the Amiga IPL line), core1 only |
| What it does *not* throttle | `do_specialties()` (stays per-instruction, for STOP/trace/mode-change correctness) |
| Units | emulated instructions between polls. `1` = poll every instruction |
| Plumbing | `config.service_cadence` -> `shared->service_cadence` (`config_file.c` :946-947, :1319-1320) -> picked up in `z3660_perf_tick()` (`newcpu.cpp` :4349), i.e. resampled roughly every 1M instructions |
| Clamp | `< 1` is forced to `1` at both config sites; `-1` is the "unset" sentinel |
| Config | `z3660cfg.txt` key `service_cadence N`, and per-preset (`e0e17d5`) |
| Runtime | serial console `SERV`, which **cycles** `1->2->4->8->16->32->64->1` (`debug_console.c` :366-374) |
| Cross-core address | `SHARED` offset `0xFFFF00A4`, mirrored in `Z3660/src/main.h` :117 and `Z3660_emu/src/main.h` :108 |

Direction of effect: **higher cadence = fewer interrupt polls = more 68k throughput, worse interrupt
latency.** So `service_cadence 1` — the value in the active preset — is the *maximum-responsiveness,
minimum-throughput* end of the range, not a degraded setting.

Three reasons it cannot be the crawl:

1. **The piscsi mailbox has no interrupt.** Completion is the blocking bus cycle; `z3660intr()` is an
   empty stub. `check_uae_int_request()` has nothing to do with it.
2. It is a *core1 instruction-counting* throttle. During a piscsi round trip core1 is not executing
   instructions at all — it is spinning in `while(shared->write_scsi==1){NOP;}`. The cadence counter
   does not advance.
3. It was `1` on the clean 08-14 run **and** on every crawling run. It is constant across exactly the
   comparison that needs explaining.

The documented hardware sweep agrees on the CPU half: cadence 1->64 buys up to ~1.3x CPU, knee
around 8; cadence 2 and 4 boot, 8 hangs the A3000-SCSI bootstrap. That ceiling is also an upper
bound on how much of *any* workload this knob can move — nowhere near 13x, which is the point being
made here and it stands.

> **CORRECTION 2026-08-22.** This paragraph used to add "with **disk I/O essentially flat**", citing
> `docs/AMIX.md`, which in turn cited this section. It was circular, and it was wrong. A raw-device
> sweep on metal measured real disk throughput **falling 16 %** from cadence 1 to cadence 8. The
> three numbered reasons above are all still correct **about the piscsi mailbox** — but the AMIX root
> disk is on the emulated **A3000 SCSI**, whose WD33C93 interrupt countdown is advanced by
> `a3000_scsi_hsync()` from inside the very function the cadence gates. `service_cadence` is
> therefore not on the *piscsi* path and *is* on the *disk* path; those were being treated as one
> sentence. See `docs/AMIX.md`, "Cadence costs real disk throughput", for the mechanism and the
> file:line. Nothing about the 13x crawl verdict changes: cadence was `1` throughout that evidence
> set, and a 16 % effect at cadence 8 cannot produce a 13x slowdown at cadence 1.

---

## 3. Candidate ranking

Evidence set (all metal, A4000 + Z3660, prod `Z3660.bin` CRC-32 `59FAE8CA` unchanged throughout;
preset `service_cadence 1`, `amix_mode YES`, `bootmode UAE_030_MMU`, KS slot 6, scsi0 + scsi6 mapped to
`.hdf` files on the exFAT volume 1):

- **E1** 08-14: install workload clean, apkg phase ~5 min, cold rig.
- **E2** 08-15/16: "storm" episodes — guest RTT spikes to ~943 ms that self-heal, console-read hangs,
  4 install runs silent in apkg and power-cut at >=28 min, across three different kernels.
- **E3** 08-17: cold rig, card health-tested clean (on-card 200 MB reference re-CRC'd byte-perfect),
  pristine source re-uploaded, fresh blank target — same workload completed in ~60-70 min (~13x),
  SD activity audibly continuous.
- **E4** Firmware's own sequential `COPY` (SD file manager, 200 MB in ~20-40 s) stayed fast throughout
  the bad days.
- **E5** Pathology is kernel-independent, card-health-independent, media-independent,
  uptime/heat-independent, survives power cycles, survives a card fsck, and self-completes.

### Verdicts

> **Superseded in part by §9.** The table below is the pre-measurement, code-only ranking, kept
> as written. The 2026-08-17 metal run refuted **C5** and **C9** outright and demoted **C4**;
> what survives is a sharpened form of **C3**. Read §9 for the current state.

| # | Candidate | Verdict | Basis |
|---|---|---|---|
| C1 | FatFs cluster-chain re-walk on a fragmented `.hdf` (the stated hypothesis) | **REFUTED** | §4 — code-level |
| C2 | `service_cadence` | **REFUTED** | §2 — not on the path; constant across E1/E3 |
| C3 | Core0 cooperative-loop latency x10 round-trip amplification | **CONFIRMED as the dominant cost model**; unproven as the 08-14 -> 08-17 *delta* | §1.1-1.3 |
| C4 | RX-backlog / IPL4 positive feedback | **PLAUSIBLE for E2 only**; insufficient for E3 | §3.1 |
| C5 | SD card FTL / erase-block state, no TRIM | **STRONGEST surviving candidate for E3** | §3.2 |
| C6 | Per-CDB preamble waste (5 data-free round trips) | **REFUTED as the crawl** (constant); **CONFIRMED as standing headroom** | §1.1 |
| C7 | 2 KB request granularity vs 64 KB capability | **REFUTED as the crawl** (constant); guest-side | §1.5 |
| C8 | Whole-L1 cache flushes per register access | **REFUTED as the crawl** (constant); real fixed overhead | §1.1, §1.4 |
| C9 | `disk_status()` card-detect stall (10 ms granular) | **UNRESOLVED — instrumentable** | §1.4, §3.3 |
| C10 | Guest kernel | **REFUTED** | E2 spans three kernels; E1 vs E3 same workload |

The honest summary: **C1 and C2 — the two hypotheses that were on the table — are both refuted by the
code.** The cost model (C3) explains why piscsi I/O is slow *in absolute terms* and why `COPY` is
immune, but a constant cost model cannot by itself explain a day-to-day 13x change. The change must
come from something that lengthens core0 loop iterations (C4, C9) or slows the SD transaction itself
(C5). Distinguishing those three needs measurement, which is what the instrumentation in §5 is for.

### 3.1 C4 — RX backlog / IPL4 feedback (plausible for E2, insufficient for E3)

The guest holds **IPL 4** across every mailbox cycle — `z3660_enter()` brackets the whole transaction
at `spl6`, and on this kernel `spl5`/`spl6`/`spl7` all emit `_spl4` (`SR 0x2400`)
(`../amix-z3660scsi/src/z3660.c` :62-65, :510-518). So the clock tick is masked for the full device
service time of every 2 KB chunk.

Meanwhile `ethernet_thread()` (`rtg/rtg.c` :321-333) bumps `eth_backlog_nag_counter` whenever
`interrupt_enabled_ethernet && ethernet_get_backlog()>0`, and every
`eth_backlog_nag_counter_max` (**1500**, `main.h` :53-54) counts it raises `AMIGA_INTERRUPT_ETH` again.
The backlog is filled independently of the interrupt-enable flag.

That closes a loop: install load keeps the guest at IPL4 -> RX drains poorly -> backlog stays > 0 ->
nag interrupts keep being re-raised -> more guest interrupt work per unit of real progress -> even less
draining. It matches E2 well (RTT spikes that self-heal, console hangs, silence under apkg).

It does **not** explain E3: a cold rig with no described network load still crawled 13x. Ranked as a
contributor to the storm episodes, not the mechanism of the 08-17 crawl.

### 3.2 C5 — SD card FTL state (strongest surviving candidate for E3)

`FF_USE_TRIM` is **0** in the compiled config: `ffconf.h` gates it on `FILE_SYSTEM_USE_TRIM`, and
`xparameters.h` :879-886 defines `FILE_SYSTEM_INTERFACE_SD`, `FILE_SYSTEM_FS_EXFAT`,
`FILE_SYSTEM_USE_LFN 1`, `FILE_SYSTEM_MULTI_PARTITION`, `FILE_SYSTEM_NUM_LOGIC_VOL 2`,
`FILE_SYSTEM_USE_STRFUNC 1`, `FILE_SYSTEM_SET_FS_RPATH 0`, `FILE_SYSTEM_WORD_ACCESS` — and **not**
`FILE_SYSTEM_USE_TRIM`. Nothing in the firmware ever issues a discard.

Consequence: after days of writing and deleting multi-hundred-MB `.hdf` images, every block in the
card's free pool is "written but not erased" from the FTL's point of view. Small random writes then
each trigger a read-modify-erase-write cycle inside the card, and random-write throughput collapses
while large sequential reads stay fast.

Checked against the evidence, this is the only single candidate that fits every line:

| Evidence | Fit |
|---|---|
| E5 kernel-independent | yes — entirely below the firmware |
| E5 card-health-independent (200 MB re-CRC byte-perfect) | yes — CRC proves *integrity*, says nothing about *latency* |
| E3 fresh blank target still crawls | yes — a newly created file still allocates into already-dirty NAND |
| E5 survives power cycles | yes — FTL/NAND state is persistent |
| E5 survives fsck | yes — fsck issues no discards |
| E5 cold start | yes — not thermal |
| E4 sequential COPY spared | yes — read-dominated and sequential; the FTL-hostile pattern is small random writes |
| E1 vs E3 ("absent on the fresher 08-14 volume layout") | yes — more genuinely-erased free space then |
| E3 self-completes, SD audibly continuous | yes — the card is busy, not stuck |
| ~13x magnitude | plausible; small-random-write collapse on a worn consumer card is commonly 10-50x |

It is **not refutable from code alone.** Its discriminating prediction is sharp, though: WRITE latency
inflated far more than READ latency, with the inflation concentrated in the `f_write`/`disk_write` span
rather than in seek or in the core0 handshake. §5 measures exactly that split.

### 3.3 C9 — `disk_status()` card-detect stall (unresolved, instrumentable)

`XPAR_PS7_SD_0_HAS_CD` is 1, so every `disk_read`/`disk_write` reads the present-state register, and a
transient card-insert deassertion costs 10 ms per step, silently. This would produce a large,
self-healing, media-independent slowdown that survives power cycles (bad contact / socket) and spares
nothing in particular. It is cheap to rule in or out by timing `disk_status` separately — worth doing
before accepting C5, because the two are confusable and one of them is a hardware-contact problem
rather than a wear problem.

---

## 4. The fragmentation / fast-seek hypothesis: code-level verdict

**REFUTED.** The firmware already implements CLMT fast seek per open `.hdf` handle, and the compiled
FatFs configuration enables it unconditionally. There is no regime in which piscsi I/O gets *gradually*
slower as the backing file fragments.

### 4.1 The compiled configuration

BSP `.../standalone_domain/bsp/ps7_cortexa9_0/include/ffconf.h`. `#ifdef SDT` is false (no generated
`xilffs_config.h` in the include tree), so `xparameters.h` supplies the `FILE_SYSTEM_*` macros.

| Knob | Value | How |
|---|---|---|
| `FF_USE_FASTSEEK` | **1** | line 68, **unconditional** — not gated on any `FILE_SYSTEM_*` macro |
| `FF_FS_EXFAT` | **1** | `FILE_SYSTEM_FS_EXFAT` defined |
| `FF_USE_LFN` | 1 | `FILE_SYSTEM_USE_LFN 1` |
| `FF_FS_TINY` | 0 | line 300 — each `FIL` keeps its own `buf[]` sector cache |
| `FF_MIN_SS` / `FF_MAX_SS` | 512 / 512 | lines 265-266 |
| `FF_FS_READONLY` | 0 | `FILE_SYSTEM_READ_ONLY` not defined |
| `FF_LBA64` | 1 | line 277 |
| `FF_USE_TRIM` | **0** | `FILE_SYSTEM_USE_TRIM` not defined — see §3.2 |
| `FF_FS_LOCK` | 0 | line 343 |

### 4.2 The firmware builds a CLMT per mapped drive

`Z3660/src/scsi/scsi.c` :768-783, in `piscsi_map_drive()`:

```c
   if(d->fd>(FIL *)1) // only for hdf files
   {
      DEBUG("Enabling fast seek.\n");
      d->fd->cltbl = d->SeekTbl;         // Enable fast seek (set address of buffer)

      d->SeekTbl[0] = sizeof d->SeekTbl / sizeof d->SeekTbl[0];   // Buffer size
      int res = f_lseek(d->fd, CREATE_LINKMAP);   // Create link map table
      if (res == FR_OK) { ... }
      if (res == FR_NOT_ENOUGH_CORE) {
         printf("[FASTSEEK %d] %lu items required to create the link map table.\n",index, d->SeekTbl[0]);
      }
   }
```

### 4.3 Why that makes fragmentation free

In the compiled `ff.c` (BSP `xilffs_v5_1/src/ff.c`), `cltbl != NULL` diverts **both** the seek and the
transfer loops to an in-RAM table lookup with **zero disk access**:

- `f_lseek()` fast-seek arm: `fp->clust = clmt_clust(fp, ofs - 1);` then `clst2sect()`. The only
  conditional disk read is `if (fp->fptr % SS(fs) && dsc != fp->sect)` — a sector-cache refill for a
  **non-512-aligned** offset. AMIX requests are always 512-aligned multiples, so that never fires.
- `f_read()` / `f_write()` cluster-advance: `if (fp->cltbl) { clst = clmt_clust(fp, fp->fptr); } else
  { clst = get_fat(...) / create_chain(...); }`.
- `clmt_clust()` is a pure linear scan of the fragment table: `ncl = *tbl++; if (cl < ncl) break; cl -= ncl; tbl++;`.

So per-request cluster resolution is O(number of fragments) **in RAM**, never O(clusters) on disk.
Fragmentation up to the table's capacity costs a few dozen ARM instructions, not one SD transaction.

Two further reasons the hypothesis has no room to operate even without the CLMT:

- **exFAT contiguous files never touch the FAT.** `get_fat()`'s `case FS_EXFAT` returns `clst + 1` by
  arithmetic when `obj->stat == 2` (contiguous), and for `obj->stat == 3` within the first fragment.
  A freshly written `.hdf` is normally contiguous.
- **Even without a CLMT, forward seeks do not restart.** The generic `f_lseek` arm continues from
  `fp->clust` when `(ofs - 1) / bcs >= (ifptr - 1) / bcs`. Only *backward* seeks restart from
  `fp->obj.sclust`. So the worst case is a partial forward walk, not a full chain re-walk per request.

### 4.4 The predicted signature does not match either

Even granting a hypothetical per-request chain walk, its cost would scale with **offset within the
file**, so it would hit the large source image far harder than the small target, and it would grow
monotonically as the workload progressed through the file. E3 reports a *uniform* ~13x with continuous
SD activity. And E4 is decisive on its own: the firmware's `COPY` uses the same FatFs, the same volume,
and the same fragmentation, and stayed fast.

### 4.5 Latent defect found while checking this (real, unrelated to the crawl) — FIXED, see §7.6

`d->SeekTbl` is `DWORD SeekTbl[64]` (`scsi/scsi.h` :90). A FatFs CLMT is `[size][ncl,tcl]...[0]`, so 64
entries hold **at most 31 fragments**. At 32 or more fragments, `f_lseek(CREATE_LINKMAP)` returns
`FR_NOT_ENOUGH_CORE` — and in `ff.c` the terminating zero is written **only** in the success branch:

```c
			*fp->cltbl = ulen;	/* Number of items used */
			if (ulen <= tlen) {
				*tbl = 0;		/* Terminate table */
			}
			else {
				res = FR_NOT_ENOUGH_CORE;
			}
```

`scsi.c` :780-782 prints a message and **leaves `d->fd->cltbl` pointing at the truncated, unterminated
table**. `clmt_clust()` then walks off the end of `SeekTbl[64]` into adjacent `PISCSI_DEV` fields:

- if it eventually reads a zero -> returns 0 -> `clst2sect()` returns 0 -> `ABORT(fs, FR_INT_ERR)`,
  which sets the **sticky** `fp->err`. Every later `f_read`/`f_write`/`f_lseek` on that handle
  short-circuits on `fp->err` — the drive is dead for the rest of the boot.
- if it reads in-range garbage -> returns a **wrong but valid cluster** -> silent read or write to the
  wrong SD sector.

Both are hard/loud/corrupting rather than slow, which is precisely why this is *not* the crawl. It is
still a defect worth fixing, and it is aggravated by the same six unchecked `f_lseek` call sites noted
in §1.4. It is also a plausible upstream report: the same code path exists in `shanshe/Z3660`.

**Fixed on this branch** (§7.6), and a minimal upstream-facing version of the fix — uninstall the
table on failure, plus the `SeekTbl` size bump — has been prepared as a separate patch series against
`upstream/main`.

---

## 5. Instrumentation shipped with this branch

`Z3660/src/scsi/scsi.c`, gated on `PISCSI_STATS` (default **on**; `#define PISCSI_STATS 0` to compile
out entirely). Counters are updated inside `handle_piscsi_reg_write()`; a summary line is emitted at
most once per `PISCSI_STATS_PERIOD_S` (default 5 s) **and only when at least one request has been
serviced in that window**, so an idle box stays silent.

Per window it reports, split READ vs WRITE: request count, bytes, requests/s, MB/s, and mean/max
microseconds for each of three spans measured separately with `XTime_GetTime()`:

- **seek** — the `f_lseek` call alone (tests C1 directly: a chain re-walk would show here).
- **io** — the `f_read`/`f_write` call alone, i.e. FatFs + `disk_*` + `XSdPs_*Polled` (tests C5).
- **cache** — the `Xil_DCache*Range` + `Xil_L1DCacheFlush` maintenance (quantifies C8).

It also reports a **gap** figure — wall time between the end of one serviced request and the start of
the next — which is the direct measurement of C3/C4/C9: core0 loop latency plus the nine data-free
round trips. Plus running totals of short transfers, `f_read` errors, and CLMT state per unit.

Console commands (`debug_console.c`): `PSTAT` toggles the periodic report, `PSTATR` prints the
cumulative totals and resets them.

Reading the output against the ranking:

| Observation | Points to |
|---|---|
| `io` mean inflated, WRITE >> READ | **C5** (card FTL / no TRIM) |
| `io` mean inflated in ~10 ms quanta | **C9** (`disk_status` card-detect stall) |
| `io` mean normal but `gap` inflated | **C3/C4** (core0 loop starvation; correlate with backlog) |
| `seek` mean non-trivial or growing with offset | **C1** would be resurrected — expected ~0 |
| `cache` a large fraction of the total | **C8** worth attacking |

What this instrumentation **cannot** do, and why §10 exists: `gap` is one number for "everything
that happened between two serviced requests". It cannot name the callee that consumed it, and it
cannot even tell a core0 block from the guest going quiet. §9 is the run that made that limitation
the whole problem.

---

## 6. Beta-21 delta on this loop

**For the record: beta 21 has not been ported.** `docs/port-ledger-beta21.md` is analysis only, and the
firmware on the card is built from `amix-main`. Nothing in §1-§5 above describes beta-21 code. The
deltas that *would* land on this path when the re-port happens:

| Area | Beta-21 state vs `amix-main` | Effect on this loop |
|---|---|---|
| **piscsi mailbox contract** | `z3660_scsi_enums.h` is **byte-identical**. `NUM_SCSI_UNITS_MAX` 18 in both; per-unit arrays 8 deep in both; upstream absorbed our `PISCSI_CMD_PDT = 0xA0` verbatim | **no protocol drift** |
| **Direct-DMA cache maintenance** | **Regression.** Beta 21 kept our comment blocks verbatim but weakened the calls to L1-only: `Xil_L1DCacheInvalidateRange` (its :1051) and `Xil_L1DCacheFlushRange` (its :1159) where we have `Xil_DCacheInvalidateRange` (:1117) / `Xil_DCacheFlushRange` (:1233). Ledger §4g | **correctness**, not speed — stale L2 over a DMA target. Must be re-ported |
| **AMP core1 L1 invalidate** (`0a4c064`) | Not present as code. Beta 21 defines `PISCSI_CMD_ADDR3 = 0x28` but has **no case for it** in the write switch; the post-spin `Xil_L1DCacheInvalidateRange` is absent. Ledger §4a hunk 4 | **correctness.** Must be re-ported |
| **`FIL fd[]`** | `fd[16]` upstream vs our `fd[8]`. Host-side handle array only, not mailbox-visible | take upstream's; no effect |
| **`hunk-reloc` arity** | `process_hunks`/`reloc_hunks`/`process_hunk` gained a trailing `int debug`. Boot/driver-relocation path only | not on the I/O datapath |
| **`SHARED` struct** | Upstream appended 8 floppy fields at `0xFFFF009C..0xFFFF00B8` — **exactly where our `debug_emu`/`amix_mode`/`service_cadence`/`perf_report` sit**. Ledger §5 | **silent ABI collision.** Ours must move to `0xFFFF00BC+`. A mis-merge would alias `service_cadence` onto `write_floppy_data` |
| **`CONFITEM` enum** | 48 entries inserted mid-enum (`iso0..19`, `adf_file0..19`, `adf0..7`). Our `service_cadence` is appended at the tail and referenced symbolically | no numeric collision; `config_item_names[]` order must be re-verified |
| **core0 loop / `emulator_thread()`** | Beta 21 added floppy dispatch branches to `emulator_thread()`, and `other_tasks()` gained the web-UI/httpd work | **directly relevant to C3.** More `else if` arms in the same priority chain and more work per core0 iteration both lengthen `t_core0_loop_iteration` for every one of the ten round trips. The re-port should re-measure with §5 before and after |
| **030-MMU run loop** | `cpummu030.cpp`, `cpuemu_32.cpp`, `newcpu_mmu_glue.cpp` unchanged upstream; `newcpu.cpp` +6/-5 | the `service_cadence` hook re-ports onto a near-untouched substrate |
| **Emulated ethernet contract** | **Zero** register/frame-transfer drift; `FRAME_MAX_BACKLOG` still 64. Ledger §6 | no change to C4's mechanics |

The one forward-looking item worth flagging: beta 21's added core0 work is a **throughput risk for
piscsi specifically**, because of the ten-round-trips-per-2 KB amplification in §1.1. That is a
re-port acceptance criterion, not a porting detail.

---

## 7. Fix candidates

Ordered by expected benefit per unit of risk. Status is marked per item: this branch shipped as
measurement-only, and §7.1 and §7.6 have since landed.

### 7.1 Cache the per-unit static geometry in the guest driver (removes 4 of 10 round trips) — IMPLEMENTED

Accesses 2-5 in §1.1 — `BLOCKSIZE0`, `PDT`, `BLOCKS0`, and the redundant second `DRVNUMX` — fetch
values that cannot change while a unit is mapped. Caching them per unit at first use removes **4 of
the 10 round trips**, a ~1.7x reduction in handshakes per 2 KB with no protocol change. **This is a
guest-driver change (`amix-z3660scsi`), out of scope for this repo**, but it is the single largest
cheap win and it is listed here because this repo owns the contract the driver is coding against.

**Landed** in `../amix-z3660scsi` as `29b2482` ("z3660: cache per-unit static geometry -- 10 -> 6
round trips per 2KB READ"). Nothing on the wire changed and no firmware change was needed, so the
contract this repo owns is unaffected — but note the one thing the driver now relies on: a mapped
unit's `block_size`, `pdt` and block count are written *only* by `piscsi_map_drive()`, and every
remap is bracketed by a guest reset. A future firmware that gains a live remap or a media change
would have to announce it on the wire, which would be a protocol change owned by this repo.

### 7.2 Batch the descriptor: one round trip instead of four (protocol change)

Accesses 6-9 program three address registers and then ring a doorbell. A `PISCSI_CMD_DESC` that takes
the triple in one transaction — or simply having the firmware treat the trigger as also carrying the
last descriptor word — collapses four round trips into one. This is a **cross-repo contract change**
(firmware + `amix-z3660scsi` + the AmigaOS `z3660_scsi.c` + the amiberry emulation) and must be agreed
before either side moves.

### 7.3 Raise piscsi's priority in the core0 dispatch chain

`emulator_thread()` services **one** request per core0 loop iteration, RTG-first. Two cheap variants:
(a) loop the `if/else if` chain while `shared->shared_data` is still set, so a burst of register
accesses drains in one iteration instead of ten; (b) re-check the SCSI flags immediately after
servicing one, before returning to the outer loop. Either turns ten loop-latency payments into
roughly one. **Lowest-risk, highest-leverage change inside this repo.** Needs care: the loop must
remain bounded so RTG and the reset thread cannot be starved.

**Do not implement this yet — see §9.** Drain amortises loop *latency* across a burst of round
trips. It cannot shorten a single 118-second blocking call, and on the 08-17 evidence that single
call is where the time went. Landing drain first would improve the 18.4 ms mean, muddy the
before/after comparison, and leave the actual defect in place. Measure with §10, name the callee,
then decide.

### 7.4 Gate the per-iteration core0 work

`other_tasks()` calls `pl_mpeg_arm_decode_progressive()` unconditionally on **every** iteration, and
`ethernet_task()` every 300000. Gating the MPEG decode on "a stream is actually playing" removes a
fixed cost from the critical path of every round trip. Measure with §5's `gap` figure first.

### 7.5 Persistent per-unit file positions instead of cold absolute seeks

The firmware seeks absolutely on every request because the guest sends an absolute LBA and no position
is kept. With the CLMT in place the seek is already cheap (§4.3), so this is **not** expected to be a
win — §5's `seek` figure should confirm it is ~0 before anyone spends effort here. Recording it as
evaluated-and-deprioritised.

### 7.6 CLMT robustness (correctness, from §4.5) — IMPLEMENTED

Three small fixes, all inside this repo:

1. On any `f_lseek(CREATE_LINKMAP)` result other than `FR_OK`, **clear `d->fd->cltbl = NULL`** so FatFs
   falls back to the safe generic chain walk instead of walking a malformed table.
2. Size `SeekTbl` from a named constant and log actual vs required fragment counts at map time, so a
   `.hdf` that outgrows the table is visible rather than silent.
3. Check the return value of all six `f_lseek` call sites (:1068, :1075, :1083, :1190, :1202, :1211)
   and both `f_write` sites (:1237, :1261), and fail the request instead of proceeding.

**Landed on this branch.** (1) `piscsi_map_drive()` now uninstalls the table on any non-`FR_OK`
result, and the `[FASTSEEK n]` lines became unconditional `printf`s that say whether fast seek is ON
or OFF and why — so the fallback is visible at boot instead of hiding behind the `DSCSI` toggle.
(2) `SeekTbl` grew from 64 to 256 items, i.e. from 31 to 127 fragments; the cost is BSS only. Every
consumer derives its bounds from `sizeof SeekTbl`, so no separate constant was needed.
(3) All six seeks are checked and a failure now **refuses** the request — a refused READ issues no
`f_read` and leaves `used_dma` at 0 so the guest copies nothing out of the bounce window; a refused
WRITE writes nothing. Both `f_write` sites report failure rather than only counting it. Reporting is
rate limited (first eight, then every 1024th) because this path is non-preemptible on core0.

The limit worth recording: **the mailbox has no per-request status register.** `SCSIERR_*` exists in
`z3660_scsi_enums.h` and the AmigaOS driver decodes it, but nothing on the wire supplies it and the
guest's only readback is `USED_DMA`. So "fail the request" means refuse-and-log; giving the drivers a
real status channel is a protocol change, and it belongs with §7.2 if that is ever taken up.

### 7.7 Reduce whole-cache flushes to range operations

`main.cc` `write_scsi_register`/`read_scsi_register` call `Xil_L1DCacheFlush()` (whole L1D) for a long
list of register offsets, and `scsi.c` :1167/:1275 do it again on core0. The descriptor length and
address are already stashed on core1 for the AMP fix, so several of these could become range ops.
**Touches the coherence fixes that took real effort to get right** (`e37bb43`, `0a4c064`) — do not
attempt without §5 showing the `cache` span is material, and re-validate on metal.

### 7.8 If C5 is confirmed

Not a firmware bug, but firmware can mitigate: enable `FF_USE_TRIM` and implement `CTRL_TRIM` in
`disk_ioctl()` so deleted image space is discarded, and/or move to a card-level erase before a fresh
install target is written. The operational mitigation — full-card erase / rewrite, or a different card
— is a bench decision, not a code change.

---

## 8. Open questions for the next metal session

**Executed on 2026-08-17. Items 1, 3 and 4 are answered — see §9.** Item 2 (`[FASTSEEK n]`) was
not captured and should be next time.

1. Flash this branch's build, run the install workload, capture the `PSTAT` stream. The `io`-vs-`gap`
   split alone separates C5 from C3/C4.
2. Note the `[FASTSEEK n]` lines at boot for both mapped units — they say whether each `.hdf` is
   contiguous, how many fragments it has, and whether the CLMT actually built (§4.5).
3. `PSTATR` before and after an idle period, to see whether the crawl is load-dependent.
4. If `io` is inflated in 10 ms quanta, suspect C9 and reseat the card before assuming wear.

---

## 9. The 2026-08-17 metal measurement: what it settled

First fully-instrumented install run on real hardware (A4000 + Z3660, `amix_mode YES`,
`bootmode UAE_030_MMU`, guest = Amix miniroot installer, no guest networking up, mostly-static
80x25 console, serial terminal attached throughout). `PSTATR` at the end of the run:

| Figure | Value |
|---|---|
| requests serviced | **228 278** |
| `gap` mean | **18 384 µs** |
| `gap` **max** | **118 019 148 µs** — *118 seconds, one gap* |
| second-largest gap observed | ~29 s |
| `io` READ mean | **338 µs** |
| `io` WRITE mean | **1 382 µs** |

The `io` figures are the crux: that span brackets *exactly* `f_read` / `f_write`, i.e. FatFs plus
`disk_read`/`disk_write` plus `XSdPs_*Polled` plus `disk_status()`. It stayed **flat, in the
hundreds of microseconds to low milliseconds, for the entire run**.

### What that refutes

- **C5 (SD card FTL / erase-block wear) — REFUTED.** C5's discriminating prediction (§3.2) was
  "WRITE latency inflated far more than READ, concentrated in the `f_write`/`disk_write` span."
  The span is flat. The card is not the crawl.
- **C9 (`disk_status()` card-detect stall) — REFUTED.** Its signature was "`io` inflated in 10 ms
  quanta." `disk_status()` is *inside* the `io` span; the span never inflated. No card-detect
  bounce occurred.
- **C1 (FatFs cluster-chain re-walk) — REFUTED by measurement as well as by code (§4).** The one
  caveat: the `seek` span was **not** captured from this run, so a chain walk inside `f_lseek`
  remains formally unexcluded. Capture `seek` next time; §4 predicts ~0.

### What that leaves

Nearly all of the wall time is `gap` — 228 278 x 18.4 ms is ~70 minutes, i.e. essentially the
whole run. `gap` is measured end-of-one-serviced-request to start-of-the-next, so it contains the
nine payload-free round trips of §1.1 *and* every core0 loop iteration in between *and* the
guest's own thinking time. **This is a refinement of C3, not a confirmation of it:** the ten-round-trip
cost model explains the 18.4 ms *mean*, but a constant cost model cannot produce a **single 118-second
gap**. That is one thing blocking, once.

### The ambiguity that forced the next instrumentation

`gap` cannot distinguish two very different worlds:

1. **Core0 blocked.** Some callee in the main loop failed to return for 118 s. Then the guest's
   store instruction was stalled in the mailbox handshake the whole time (§1.2), and this is a
   firmware bug.
2. **The guest went quiet.** The guest simply issued no piscsi request for 118 s — computing,
   waiting on something else, or wedged on its own account. Then core0 was healthy and the
   investigation belongs on core1 or in the guest.

Nothing in PISCSI_STATS separates these, and they lead to opposite fixes. §10 is built to
separate them, and `docs/core0-loop-blocking-audit.md` pre-ranks the candidates for world 1 with
file:line evidence so that a named row converts straight into a code site.

**Do not implement §7.3 (mailbox drain) on the strength of these numbers.** Drain amortises loop
latency across a burst of round trips; it cannot shorten a single 118-second blocking call. If
world 1 is confirmed, the fix is whatever the named callee needs; §7.3 is a separate, smaller win
against the 18.4 ms mean.

---

## 10. Core0 loop instrumentation (C0STAT)

`Z3660/src/core0_stats.[ch]`, gated on `CORE0_STATS` (default **on**; set to 0 to compile out
entirely). Wired into the `CPU_EMULATOR` main loop at `Z3660/src/main.c` :1803-1834, with nested
spans inside `emulator_thread()` (`cpu_emulator.c`) and `other_tasks()` (`rtg/rtg.c`).

### What it measures

Every callee in the loop's dispatch chain is timed individually, and the RTG-first priority chain
is split into its arms so an RTG blit is never confused with an SD transfer. Per span: **call
count, cumulative time, and worst single call** — each kept twice, once per reporting window and
once cumulatively. Seventeen spans; `docs/core0-loop-blocking-audit.md` has the callee-to-row map.

Three design points carry the requirement from §9:

- **Measured around each call, never sampled.** The max is written the instant the call returns,
  so a 118-second block is recorded even though the reporting window was stalled for the whole of
  it.
- **A single call at or above `CORE0_STATS_BLOCK_US` (default 100 ms) prints its own line
  immediately.** Naming the offender does not wait for the window. Rate limited to the first 32
  then every 16th, because this path is non-preemptible on core0.
- **Chained stamps.** The end timestamp of callee N is the start timestamp of callee N+1, so a
  loop iteration costs one `XTime_GetTime()` per timed callee rather than two.

### Output

Rate-limited to one report per `CORE0_STATS_PERIOD_S` (default 5 s), and **idle-silent**: a
window is printed only if the guest moved piscsi traffic in it, or the window's worst single call
reached `CORE0_STATS_NOTABLE_US` (default 2 ms). A healthy idle box prints nothing; a blocked
idle box still prints.

```
[C0STAT] core0 loop accounting ON: 17 spans, window 5s, block>=100000us; cost/boundary 291ns (timer read 78ns) -- PSTAT/PSTATC/PSTATZ
[C0STAT] win 5013ms it=241930 (48261/s) iter avg 20us max 118019148us ovh=0.4% worst=DEBUG 118019148us
[C0STAT]  DEBUG            1c cum   118019ms max  118019148us 99.9%
[C0STAT]  EMU>SCSIW     1187c cum        1204ms max     4123us  0.0%
[C0STAT]  OTHER       241930c cum         901ms max       38us  0.0%
[C0STAT] *BLOCK* #1 DEBUG 118019148us (loop iter 18442213, call 922110 of that task)
```

`A>B` rows are **contained in** the `A` row above them (`EMU>SCSIW` is inside `EMU`), so parent
and child both appear and their times overlap by design. The worst-call scan walks the table
backwards, so the most specific row wins a tie with its parent — the report names the leaf.

### Console commands

| Command | Effect |
|---|---|
| `PSTAT` | toggles the periodic reports — **both** the piscsi one and the core0 one, in lockstep |
| `PSTATC` | dumps core0 per-task cumulative totals, the whole-iteration row, the worst-single-call-ever latch, and the measured overhead. **Non-destructive** — safe to poll |
| `PSTATZ` | zeroes the core0 cumulative totals and the worst-ever latch, for scoping a measurement to one phase |
| `PSTATR` | unchanged: piscsi cumulative totals + per-unit CLMT state, then reset |

### Overhead, and how it is reported honestly

The instrumentation **measures its own cost on the metal at init** and prints it. Two numbers,
both from `c0_calibrate()`: one `XTime_GetTime()` (three reads of the SCU global timer), and one
**complete span boundary** — the timer read plus everything `c0stat_acc()` does — obtained by
running 256 real boundaries and dividing. The `ovh=` field on every window line is
`boundaries x measured-boundary-cost / window`, so it is evidence rather than an estimate, and
the very first window of a run tells the operator whether the instrumentation is material.

Two things were done to keep that number small, both verified in the disassembly: the reporting
helpers and `c0_recalc()` are `noinline` (at `-O3` gcc inlines them into the two hot functions,
and the inlined `printf` machinery then costs every boundary a 52-156 byte stack frame and a
`vpush`/`vpop` of `d8-d9`), and blocking console output is bracketed so it is charged to no span
at all — otherwise C0STAT's own report, and PISCSI_STATS' report (which is emitted from inside
`handle_piscsi_reg_write()`, i.e. inside the `EMU>SCSIW` span), would each fake a stall in
whatever ran next.

### Known attribution limits — read these before believing a row

- **Interrupt time is not separated.** `isr_video` (~60 Hz) and `isr_audio_tx` land in whichever
  span happens to be open. That is a small error spread evenly across all spans; it cannot
  manufacture a multi-second max, which is why the ISRs were deliberately left untouched.
- **Only the `CPU_EMULATOR` loop is instrumented.** The real-CPU loop (`main.c` :1871+) is not:
  the crawl is an emulated-guest workload, and that loop's `M68K_RESET` arm would need its own
  re-arming to avoid charging a whole reset episode to whichever callee was stamped next.
  `PSTATC` says so explicitly rather than printing zeros.
- **A skipped gated callee emits no stamp.** `debug_thread` (1 in 20) and
  `emulator_reset_thread` (1 in 40) are only stamped on the iterations they actually run, so
  their call counts stay honest; the few instructions of counter bookkeeping on the skipped
  iterations are charged to the next span.
- **The timebase caveat from §5 applies unchanged.** The Cortex-A9 global timer runs at CPU_CLK/2
  and this board retunes its ARM clock at runtime, so microseconds derive from the live
  `counts_per_second`. Ratios and step changes discriminate; absolute values are uncalibrated.

### Reading the output against §9's two worlds

| Observation | Conclusion |
|---|---|
| a `*BLOCK*` line names a row, and its max ~= the `gap` max | **world 1**: core0 blocked. Go to that row's entry in `core0-loop-blocking-audit.md` |
| `gap` max is large but **no** row's max is large, and the `ITERATION` max is small | **world 2**: the guest went quiet. Core0 was healthy; move to core1 |
| `EMU>SCSIW` cumulative inflated with a *small* max | the un-rate-limited error printf amplifier (audit B11), not a block |
| `EMURESET` shows multi-second calls | expected and legitimate — `piscsi_init()` rebuilds 7 CLMTs (audit B7) |
| `ovh=` is more than a percent or two | the instrumentation is perturbing the thing it measures; trim spans or set `CORE0_STATS 0` |
