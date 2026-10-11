# Core0 asynchronous data abort, 2026-08-25 — DFSR `0x00001c06` in `Xil_L2CacheFlush()`

**Status:** single occurrence, not reproduced. Static analysis against the exact binary that was
running, plus the board's own five serial lines. **No firmware change is proposed or made by this
document** — it decodes the event, locates it in the binary, tables the candidate mechanisms with
per-candidate discriminators, and specifies what the firmware would have to record for a second
sighting to be a measurement rather than another five lines.
**Date:** 2026-08-25
**Firmware:** `BOOT-lean`, CRC-32 **`9CE3F6A6`** (12 631 004 bytes), branch `amix-main-2`, BETA 21 lineage.
**Board configuration at the time:** `bootmode CPU` (real 68060 — **core1 was never started**),
`cpufreq 50` → clock index 0, `autoconfig_rtg NO`, `cpu_ram YES`, RTG up at 800x600x16 60 Hz.
**Fork-local, additive.** Nothing here modifies an upstream file.

---

## 1. Bottom line

Core 0 of the Zynq took an **asynchronous (imprecise) external data abort** and the firmware's own
handler rebooted the board out from under the running guest.

The abort was taken at `0x0100b2c8`, which this document identifies **byte-exactly** as the
completion-poll branch of **`Xil_L2CacheFlush()`** — the Xilinx BSP routine that issues the PL310's
*Clean-and-Invalidate by Way* over the **entire** L2 cache and spins until the way mask reads back
zero. The serial line printed immediately before the abort pins the foreground caller to
`scsi.c:1877-1878`, the `PISCSI_CMD_SETFSH` "no filesystem handler" path.

That is not a bystander loop. **The loop is what generates the write traffic**: a whole-cache
clean-and-invalidate writes back every dirty line in the L2, and it is the only cache operation in
the firmware that touches lines the caller does not own.

Two things about that loop are, independently of this event, defects worth naming:

1. **`Xil_L2CacheFlush()` masks no interrupts** (unlike its sibling `Xil_L1DCacheFlush()`, which
   masks IRQ+FIQ for its whole duration), and
2. the **60 Hz vblank ISR calls `Xil_L2CacheFlush()` itself** on this exact boot mode
   (`video.c:1054-1055` → `handle_cache_flush()` → `video.c:172`).

So a PL310 *background operation* can be, and periodically will be, re-programmed from interrupt
context while it is still running. That is the leading candidate mechanism and it explains the
rarity (a microseconds-wide window sampled 60 times a second) without appealing to anything
electrical.

**What is NOT established:** the physical address that errored, the direction of the access, and
whether candidate A below is actually what happened. All three are recoverable only by
instrumenting the handler — §7.

---

## 2. What the board printed

From the serial log of the discarded run (all timestamps 2026-08-25):

```
15:47:42  [PISCSI] RDB at sector 2 (sector 0 empty).
15:47:42  [PISCSI] Partition 0: UNIX_Root (9) / 1: UNIX_Swap (9) / 2: UNIX_Boot (9)
15:47:42  Reset active (DOWN)... / Video reset from reset_init() / video mode: 800x600 16 bit color, 60 Hz
15:47:42  GIC: SCU GIC enabled / GIC: Interrupts enabled          <- vblank ISR live from here
15:47:44  EMAC: XEmacPs_Start done.
15:47:46  [!!!PISCSI] Found no handler for file system UNI/1
15:47:46  [Core0] DataAbortHandler()!!!!
15:47:46  Data abort with Data Fault Status Register 0x00001c06
15:47:46  Address of Instruction causing Data abort 0x0100b2c8
15:47:46  Write Fault to 0x00000000
15:47:46  Instruction 0x1AFFFFFB
15:47:46  [SD Init] Done (Z3660.BIN)                              <- the board has REBOOTED ITSELF
```

**Why the truncation is the tightest clue in the log.** On the three boots that did *not* abort, the
same second carries **three** consecutive lines:

```
[!!!PISCSI] Found no handler for file system UNI/1
[!!!PISCSI] Found no handler for file system res/118
[!!!PISCSI] Found no handler for file system UNI/0
```

The aborting run printed the **first** and then nothing. So the fault landed between the `UNI/1`
`SETFSH` and the `res/118` one — a window a few hundred microseconds wide, inside which the only
firmware work is the two cache flushes at `scsi.c:1877-1878` and the return to the register-write
dispatcher.

`[Core0] DataAbortHandler()` appears nowhere in the three preceding bench sessions. It is
first-of-a-kind for this board.

---

## 3. DFSR `0x00001c06`, decoded

ARMv7-A short-descriptor format (`DFSR[9]` = LPAE = 0 confirms short descriptor):

| Bits | Field | Value | Meaning |
|------|-------|-------|---------|
| `[3:0]` | `FS[3:0]` | `0b0110` | |
| `[7:4]` | `Domain` | `0b0000` | |
| `[9]` | `LPAE` | `0` | short-descriptor format — the table below applies |
| `[10]` | `FS[4]` | `1` | |
| `[11]` | `WnR` | `1` | nominally "write" — **see the caveat** |
| `[12]` | `ExT` | `1` | external-abort qualifier, **IMPLEMENTATION DEFINED** |

`FS = 0b10110` = **`0x16` = asynchronous external abort**. That is the whole finding in one number.

### 3.1 What "asynchronous" costs us

Three of the five printed lines are **not measurements**:

* **`Write Fault to 0x00000000` — the address is meaningless.** The handler reads DFAR
  (`main.c:543`), and for an asynchronous fault the ARM ARM leaves DFAR **UNKNOWN**. The printed
  zero is a stale register. Reading it as "the firmware stored to NULL" would be reading garbage as
  data.
* **`Write` — the direction is also not established.** The handler selects the word from
  `FaultStatus & (1<<11)` (`main.c:549-553`), and the ARM ARM states that on an **asynchronous**
  fault `WnR` is likewise **UNKNOWN**. This corrects the field note taken at the bench, which read
  `WnR=1` as fact. The write hypothesis survives on a different, weaker footing: the core was
  executing a cache *clean* operation whose entire purpose is emitting writes, so a write is the
  strong prior — but the register did not tell us so.
* **`Address of Instruction causing Data abort 0x0100b2c8` is where the abort was TAKEN.** The BSP
  vector computes `DataAbortAddr = lr_abt - 8` (`asm_vectors.S:151-153`). For a *synchronous* abort
  that is the faulting instruction; for an *asynchronous* one it is the next instruction that would
  have executed. The offending access had already retired. Also worth carrying: on this core, when
  several accesses are outstanding and the first takes an imprecise external abort, a *second*
  access can falsely report one too — so even the count of events is soft.

### 3.2 What "asynchronous" buys us

It is not all loss. Imprecision is itself a filter, and on this firmware it is a sharp one.

An external abort is imprecise only for accesses that the core is allowed to retire before the
fabric answers — i.e. **buffered** accesses. Accesses to **Strongly-ordered** memory are not
buffered and their external aborts are **synchronous**. Cross that rule with core 0's actual page
table (`main.c:1071-1088`, over the Xilinx default table) and most of the address map is excluded:

| Physical range | Attribute in core 0's table | Can it produce an *imprecise* abort? |
|---|---|---|
| `0x00000000-0x3FFFFFFF` DDR (default WB; `0x08000000-0x17FFFFFF` re-marked `RAM_CACHE_POLICY` = WB/WA) | Normal cacheable write-back | **YES** — write-backs and evictions |
| `0x18000000-0x181FFFFF` RTG registers | `NORM_NONCACHE` (bufferable) | **YES** |
| `0x1FC00000-0x1FFFFFFF` audio / ethernet buffers | WT / non-cacheable | **YES** |
| `0x40000000-0x77FFFFFF` PL slave 0 | **`RESERVED` = fault descriptor** | **NO** — a translation fault is `FS=0b00101`, synchronous |
| `0x78000000-0x7FFFFFFF` PL (audio formatter `0x7FC00000`, clk wizards `0x7FC30000`/`0x7FC50000`) | **`STRONG_ORDERED`** | **NO** — synchronous |
| `0x80000000-0xBFFFFFFF` PL slave 1 (**`XPAR_Z3660_0_BASEADDR = 0x80000000`** — the FPGA register block) | Strongly-ordered (BSP default) | **NO** — synchronous |
| `0xE0000000-0xE02FFFFF` IOP peripherals (GEM `0xE000B000`, GPIO `0xE000A000`, SD, UART) | **`STRONG_ORDERED`** | **NO** — synchronous |
| `0xF8000000-0xF8FFFFFF` APB: SLCR, **A9 private region `0xF8F00000-0xF8F01FFF`**, **PL310 `0xF8F02000`** | `DEVICE_MEMORY` (BSP default; bufferable) | **YES** |
| `0xFFF00000-0xFFFFFFFF` OCM (the AMP `shared` struct at `0xFFFF0000`) | `NORM_NONCACHE` (`main.c:1071`) | **YES** |

**This is the single most useful thing the DFSR gives us:** every path that touches the Amiga side —
the FPGA register block, the CPLD interface, the clock wizards, the on-chip peripherals — is
Strongly-ordered or unmapped in core 0's table, so **none of them can be the source of an imprecise
abort**. Whatever wrote, wrote to DDR, to the PS APB space, or to OCM.

`ExT = 1` is tempting to read as "AXI SLVERR rather than DECERR" — the Cortex-A9 does report SLVERR
distinctly (its TRM says illegal accesses to the MPCore private memory region "cause an abort to the
requesting Cortex-A9 processor and the Fault Status Register shows this as a SLVERR"). But the
architecture calls `ExT` IMPLEMENTATION DEFINED and this fork has never pinned it against silicon.
**Treat SLVERR as a hypothesis, not a decode** — §7 item 2 is how to settle it.

---

## 4. Where core 0 was: `Xil_L2CacheFlush()`, called from `scsi.c:1878`

### 4.1 Establishing that the on-card image is the tree in this repo

Four steps, each mechanical:

1. The card reported `0:/Z3660.bin` CRC-32 `9CE3F6A6`, 12 631 004 bytes, both before and after the
   session (no firmware was written).
2. `zlib.crc32` of
   `z3660-firmware/Z-TURN/vitis_ide/Z3660_system/Alfa/sd_card/archive/BOOT-lean-9CE3F6A6.BIN`
   = `9CE3F6A6`, same byte count.
3. `z3660-firmware/Z-TURN/vitis_ide/Z3660/build/Z3660.bin` (core 0, 2 015 248 bytes) occurs
   byte-identically inside that `BOOT.BIN` at offset **`0x409240`** — so
   `Z3660/build/Z3660.elf` (md5 `8120bd6fed424b09eb22955af1a20075`) is the core-0 image that was
   executing.
4. Core 0 links at `ps7_ddr_0_memory_0 : ORIGIN = 0x01000000` (`Z3660/src/lscript.ld:24`), which is
   why an ARM PC of `0x0100b2c8` is core-0 text at all.

(The archived `BOOT-lean-9CE3F6A6.elf` is the **core 1** emulator ELF, entry `0x30000000`. It is not
the binary the abort was in.)

### 4.2 The loop, from the binary

```
$ arm-none-eabi-objdump -d --start-address=0x0100b2a4 --stop-address=0x0100b2e8 \
      z3660-firmware/Z-TURN/vitis_ide/Z3660/build/Z3660.elf

0100b2a4 <Xil_L2CacheFlush>:
 100b2a4:  e3a02a02   mov  r2, #8192          ; r2 = 0xF8F02000  (XPS_L2CC_BASEADDR)
 100b2a8:  e3a01003   mov  r1, #3
 100b2ac:  e34f28f0   movt r2, #63728
 100b2b0:  e30f3fff   movw r3, #65535         ; 0xFFFF = all ways
 100b2b4:  e5821f40   str  r1, [r2, #3904]    ; 0xF40 DEBUG_CTRL <- 3  (errata 588369 workaround)
 100b2b8:  e58237fc   str  r3, [r2, #2044]    ; 0x7FC CACHE_INV_CLN_WAY <- 0xFFFF  == START
 100b2bc:  e59237fc   ldr  r3, [r2, #2044]    ; <-- loop head
 100b2c0:  e6ff3073   uxth r3, r3
 100b2c4:  e3530000   cmp  r3, #0
 100b2c8:  1afffffb   bne  100b2bc            ; <-- THE ABORT WAS TAKEN HERE
 100b2cc: ...         str  0, [.., #0x730]    ; CACHE_SYNC
 100b2dc: ...         str  0, [.., #0xF40]    ; DEBUG_CTRL <- 0
 100b2e0:  f57ff04f   dsb  sy
```

The serial printed `Instruction 0x1AFFFFFB`; the binary at `0x0100b2c8` **is** `0x1afffffb`. Encoded
`imm24 = 0xFFFFFB` = −5, target `PC + 8 − 20` = `0x0100b2bc`. A four-instruction backward poll —
exactly the "tight backward branch" the bench note read off the encoding, now with a name attached.

`addr2line` resolves `0x0100b2c8` to `Xil_L2CacheFlush` at BSP `xil_cache.c:1428`. The C source is
`design_1_wrapper/.../standalone_v9_0/src/arm/cortexa9/xil_cache.c`:

```c
void Xil_L2CacheFlush(void)
{
    Xil_L2WriteDebugCtrl(0x3U);                                    /* disable WB + linefill  */
    Xil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_INV_CLN_WAY_OFFSET, 0x0000FFFFU);
    ResultL2Cache = Xil_In32(... CACHE_INV_CLN_WAY ...) & 0x0000FFFFU;
    while (ResultL2Cache != 0U) { ResultL2Cache = Xil_In32(...) & 0x0000FFFFU; }   /* <-- here */
    Xil_L2CacheSync();
    Xil_L2WriteDebugCtrl(0x0U);
    dsb();
}
```

**What it touches while polling.** Two addresses only, both in the PL310 at `0xF8F02000`:
`+0x7FC` (Clean-and-Invalidate by Way, read every iteration, written once to start) and `+0xF40`
(Debug Control, written before and after). But the *operation* it started walks every one of the
L2's ways and sets and **writes back every dirty line in the cache** to the L2's master ports. With
`RAM_CACHE_POLICY` = write-back/write-allocate over `0x08000000-0x17FFFFFF` (`main.c:1073-1074`) —
the guest RAM window — plus core 0's own image and heap in DDR, that is up to a full cache's worth
of write traffic issued while the core sits in those four instructions.

### 4.3 The caller

`Xil_L2CacheFlush` is called from eleven sites in the core-0 image. The two inside
`handle_piscsi_reg_write` are at `0x1184f90` and `0x1185ae0`. The one at **`0x1185ae0`** is
immediately preceded by a `printf` whose format string is at `0x011db4bc`:

```
 11db4bc  5b212121 50495343 53495d20 466f756e   [!!!PISCSI] Foun
 11db4cc  64206e6f 2068616e 646c6572 20666f72   d no handler for
```

and the two instructions before that printf are

```
 1185ab0:  f3c70e30   vmov.i64 d16, #0xffffffff00000000
 1185ab4:  e2844020   add      r4, r4, #32
 1185ac0:  f444078f   vst1.32  {d16}, [r4]      ; node->dn_SegList = 0; node->dn_GlobalVec = -1
 1185ad8:  fafa81f1   blx      printf
 1185adc:  ebfa1493   bl       Xil_L1DCacheFlush
 1185ae0:  ebfa15ef   bl       Xil_L2CacheFlush
```

which is `scsi.c:1874-1878` verbatim:

```c
node->dn_GlobalVec = 0xFFFFFFFF;
node->dn_SegList   = 0;
printf("[!!!PISCSI] Found no handler for file system %c%c%c/%d\n", ...);
Xil_L1DCacheFlush();
Xil_L2CacheFlush();
```

`node` points into the guest's own memory — the range check at `scsi.c:1858-1860` admits only
`0x08000000-0x0FFFFFFF` (`cpu_ram`) or `0x40000000-0x4FFFFFFF` remapped down by `0x20000000`, and
the run's config had `cpu_ram YES`, `autoconfig_ram NO`. So the guest-visible `DeviceNode` write
that immediately precedes the flush lands in the WB-cacheable guest RAM window, and the flush is
what pushes it — and everything else dirty — out to DDR.

**One caveat on the call site.** The abort record prints no link register, and `Xil_L2CacheFlush` is
shared by every caller, so the *abort* alone cannot say which call was on the stack. The serial
ordering is what pins the foreground: the `UNI/1` line came out, the `res/118` line never did.

---

## 5. Candidate mechanisms, with discriminators

Ranked. "Discriminator" means: the observation that would confirm or refute it on a second sighting,
not an argument.

| # | Mechanism | Where the erroring access went | Why it fits | Discriminator |
|---|---|---|---|---|
| **A** | **Re-entrant PL310 background operation.** `Xil_L2CacheFlush()` masks no interrupts. In `bootmode CPU` the 60 Hz vblank ISR calls it too (`video.c:1054-1055` → `handle_cache_flush()` → `Xil_L1DCacheFlush(); Xil_L2CacheFlush();`, `video.c:161-174`; the `bl Xil_L2CacheFlush` is present in `isr_video` at `0x104b24c` in this build). An ISR landing inside the foreground's by-way operation re-writes `CACHE_INV_CLN_WAY` while it is in progress and, on return, clears `DEBUG_CTRL` back to 0 **underneath** the still-running outer operation — dropping the errata-588369 write-back/linefill inhibit mid-clean. | Either the Device-mapped PL310 register write itself (bufferable → imprecise) or a write-back the confused controller emitted. | Explains the rarity arithmetically: the by-way walk is microseconds, sampled 60×/s, so the overlap probability per flush is ~1e-3 — one hit across six sessions is the right order. Explains why the PC is in *that* loop rather than anywhere else. Explains why the same code path then ran cleanly on three later boots. | A **nesting depth counter** (§7.1). Depth > 1 at abort time confirms A outright. Independently: mask IRQ+FIQ across the flush (as `Xil_L1DCacheFlush()` already does) and see whether the event ever recurs. |
| **B** | **An ordinary L2 write-back was refused by the fabric.** The by-way clean is the only operation in the firmware that touches *every* line, including stale tags no current caller owns. | A DDR physical address that the DDR controller or the interconnect answered with an error. | Fits "imprecise + write" without needing any software defect; fits the fact that the abort happens in the one operation that emits unbounded write traffic. | **PL310 latched status** (§7.2). `ISR (0x21C)` bit 8 `DECERR` / bit 7 `SLVERR` set ⇒ the error came back on the L2's own master port, i.e. a write-back — B (or A) rather than C/D. Clean ⇒ the error was on a core-issued buffered access, not an L2 eviction. |
| **C** | **A buffered Device write from interrupt context to the A9 private memory region** (`0xF8F00000-0xF8F01FFF`: GIC CPU interface `0xF8F00100`, distributor `0xF8F01000`, private timer/watchdog `0xF8F00600`). The A9 TRM makes LDM/STM, halfword and doubleword accesses, partial-strobe word writes, and any access to offsets `0x700-0xFFF` return SLVERR. | The GIC/SCU/timer block, mapped `DEVICE_MEMORY` by the BSP default ⇒ bufferable ⇒ imprecise. | The only mechanism for which the vendor documents SLVERR on this exact core, and `ExT=1` is at least consistent with SLVERR. Every ISR entry/exit writes the GIC, and an ISR is exactly what was plausibly running during the poll. | PL310 status **clean** while the abort fires (separates C from A/B), plus a static audit of every access to `0xF8F0xxxx` for size and multiple-register form. Also `SPSR_abt` mode bits (§7.4) — C requires the access to have come from a handler. |
| **D** | **Buffered write to OCM `0xFFF00000-0xFFFFFFFF`** — the AMP `shared` struct at `0xFFFF0000`, mapped `NORM_NONCACHE` (`main.c:1071`). | OCM / central interconnect. | Region is bufferable, so mechanically possible. | Low prior and easy to separate: an unmapped-OCM access answers DECERR, and `bootmode CPU` never starts core 1 (`main.c:1788-1800`, guarded on `emu==1`), so the AMP handshake was idle. PL310 status + `ExT` settle it. |
| **E** | **PL / CPLD / Amiga-bus AXI slave error** — the FPGA register block at `0x80000000`, the clock wizards at `0x7FC3xxxx`, the on-chip peripherals at `0xE00xxxxx`. | — | — | **Architecturally excluded, no experiment needed.** Every one of those sections is `RESERVED` (fault) or `STRONG_ORDERED` in core 0's table (§3.2). A Strongly-ordered external abort is *synchronous*; a fault descriptor gives `FS=0b00101`. Neither can present as `FS=0b10110`. |
| **F** | **DDR / bus signal-integrity marginality.** | — | — | **Excluded on kind, not on degree.** This board's DDR has no ECC and the PHY has no path to raise an AXI error response. Marginality there corrupts data *silently*. An abort is a slave **refusing** a transaction, which is a different physical event from a bit arriving wrong. |

### 5.1 What was plausibly in flight from that loop and its interrupt context

Enumerated rather than asserted, since §3.1 forbids reading the direction off the register:

* **From the loop's own operation:** every dirty L2 line — core 0's `.data`/`.bss`/stack/heap in DDR
  at `0x01000000+`, the guest RAM window `0x08000000-0x17FFFFFF` (write-back/write-allocate), and
  whatever the just-completed RDB/`SETFSH` work had dirtied. This is the largest write burst the
  firmware ever issues, and it is issued *by* the instruction stream the abort interrupted.
* **From the loop's own instruction stream:** the two Device writes to `0xF8F027FC` and `0xF8F02F40`
  (buffered ⇒ imprecise-capable).
* **From the immediately preceding foreground code:** the 64-bit NEON store `vst1.32 {d16},[r4]` into
  the guest `DeviceNode` (WB-cacheable, so it lands in L1, then is pushed out by the very flush pair
  that follows), and the `printf` chain to the UART (`0xE0001000`, Strongly-ordered ⇒ excluded).
* **From interrupt context, live at 60 Hz:** `isr_video` — `video_formatter_read/write` to RTG
  registers (`0x18000000`, `NORM_NONCACHE` ⇒ imprecise-capable), `init_vdma_irq` VDMA programming,
  GIC acknowledge/EOI writes into the A9 private region (Device ⇒ imprecise-capable), and its own
  `Xil_L1DCacheFlush(); Xil_L2CacheFlush()` pair.
* **From the GEM:** `XEmacPs_Start` had completed 2 s earlier; its descriptor rings live at
  `RTG_BASE+0x07E00000` = `0x1FE00000`, mapped `ETHERNET_CACHE_POLICY` (non-cacheable). DMA errors by
  the GEM master are not CPU aborts, but *core-issued* descriptor writes there are buffered.

---

## 6. Family: is this the beta-20 `default_timings` index-0 family, or a software defect?

**The evidence separates them, and it points away from the timing family.** Four independent legs:

1. **The architectural leg (strongest).** The `default_timings` finding is about the CPLD/motherboard
   bus interface — `pclk`, `bclk`, `cpuclk`, `clk90` and their phases, i.e. the PL-side clock domain
   that meets the Amiga bus. Core 0 cannot take an *imprecise* abort from anything in that domain,
   because every physical window it lives in is `RESERVED` or `STRONG_ORDERED` in core 0's page
   table (§3.2, row E). If the Amiga-side bus had misbehaved toward the ARM, the abort would have
   been **synchronous**, with a usable DFAR. It was not.

2. **The direction-of-marginality leg.** The run was at **clock index 0** (`cpufreq 50`), and the
   board reported `AXICLK 100.00 MHz`. Sessions at index 10 (`cpufreq 100`) report
   `AXICLK 200.00 MHz`. The PS-PL bridge and PL fabric were therefore running at **half** the rate
   they run at in the configuration this fork uses by default. A marginality story has to explain why
   the *slower* setting broke.

3. **The provenance leg.** `1:/timings/default_timings.txt` on the card was byte-identical to the
   previous session's copy and was never written; the on-card firmware CRC was `9CE3F6A6` before and
   after. The board's own `Clock index 0` line and the printed `PCLK 050.00 / BCLK 025.00 PHASE 060`
   match that file's index-0 block, so the file (not a compiled table) was in force, unchanged.
   *(Correction, 2026-08-28: `config_clk.c:534+` is `set_default_timings()`, which runs only when
   something calls it and writes itself to the SD when it does. The table a failed
   `read_timings()` silently leaves in place is the compile-time initialiser at
   `config_clk.c:13-149`, which is different again — see [`clock-timings.md`](clock-timings.md)
   §1.)*

4. **The failure-kind leg.** Timing marginality on a non-ECC DDR interface or a CDC boundary produces
   **wrong data**, silently. This event is a **refusal**: some slave answered an access with an error
   response. Those are different physical failures, and the second one requires a slave that decoded
   the transaction and objected.

**What would move it back into the electrical family:** a recurrence that correlates with rail
voltage or die temperature (§7.6), or one whose PL310 status shows a **DECERR** (nothing decoded)
rather than a slave error — a decode failure is closer to "the fabric was in a state nobody
intended". Neither is available from a single sighting, which is exactly the gap §7 exists to close.

**And the honest counterweight:** this fork's own `KNOWN_ISSUES.md` records (2026-08-24, measured)
that with a real 68060 only `cpufreq 100` was demonstrated to boot, and quotes the CPLD README's
"only some *magic* frequencies work". This run used `cpufreq 50` with a real CPU — a configuration
the fork documents as unproven. That does not make the abort electrical (leg 1 stands regardless),
but it does mean **this run was not in the fork's known-good clock configuration**, and any
recurrence hunt should A/B index 0 against index 10 rather than assume they are equivalent.

---

## 7. Survivability: what the handler does, and what it could do

### 7.1 What it does today

`main.c:538-558`:

```c
void DataAbortHandler(void *data) {
   FaultAddress = mfcp(XREG_CP15_DATA_FAULT_ADDRESS);
   FaultStatus  = mfcp(XREG_CP15_DATA_FAULT_STATUS);
   xc3_usleep(100000);                 // "let other printf to finish"
   printf(... 5 lines ...);
   xc3_usleep(10000);
   hard_reboot();                      // never returns
   while(1);
}
```

`hard_reboot()` (`cpu_emulator.c:312-329`) drops the CPLD reset line, writes the SLCR
`PS_RST_CTRL` software-reset bit twice, then arms the SCU watchdog at `0xFF` and spins — which is
why the log shows `[SD Init] Done (Z3660.BIN)` seven seconds later and a full re-banner. **The board
reboots under the guest with no warning to the guest and no record that survives the reboot.**

`CPSR.A` is cleared in the BSP's `boot.S:378-380`, so asynchronous aborts are live from early boot
onward; there is no masking window that could have deferred or batched this one.

### 7.2 Could it be survived?

**Architecturally, yes.** The BSP vector returns with `SUBS PC, LR, #8` (`asm_vectors.S:159`). For an
asynchronous Data Abort, `LR_abt` = preferred return address + 8, so that return resumes at the next
instruction to execute without re-executing anything. Nothing about `FS=0x16` makes the core state
unrecoverable. The cost is precise and unpleasant: **one write of unknown provenance was lost**, and
the firmware cannot say which.

**Four things would have to change before survival is safe, and none is a one-liner:**

1. **The abort stack is 1024 bytes** (`lscript.ld:16`, `_ABORT_STACK_SIZE`) and the handler makes
   five `printf` calls on it. It works today; it is not headroom to build on. A surviving handler
   must write a fixed-size record into `.bss` and print it from the main loop, not from the handler.
2. **`printf` from the handler is not re-entrant.** The existing `xc3_usleep(100000)` comment ("let
   other printf to finish") is an admission that the abort can land inside the UART path. Reboot
   hides that; surviving would not.
3. **Escalation is mandatory.** A persistently erroring access would re-abort on every resume and
   livelock in the handler. Any survive path needs "N aborts within T ⇒ reboot as today", with N and
   T fixed in advance.
4. **Survival is not free for a measurement session.** If the lost write was into guest RAM, the
   guest continues with silently corrupted memory — arguably worse than a clean reboot for a run
   whose whole purpose is to sample a distribution. The recommendation is therefore **survive *and*
   loudly taint the run** (a banner line, a counter the console can read), never survive silently.

**Assessment:** survivable and worth doing, but as the *second* change. The first change is §8 —
because a survived abort with today's five-line record teaches exactly as little as a rebooted one.

---

## 8. Registered discriminators — what the firmware must record next time

Proposal for a future round. Listed so that a second sighting is a measurement. Items 1, 2 and 7 are
the ones that actually separate the candidates in §5; the rest are cheap and taken while we are
there.

1. **Call-site tag and nesting depth.** A `volatile uint32_t l2_op_site` and `volatile int l2_op_depth`
   incremented/decremented around every `Xil_L2CacheFlush()` / `Xil_L1DCacheFlush()` entry, with the
   site encoded as a small enum (`SETFSH`, `LOADFS`, `COPYFS`, `ISR_VIDEO`, `piscsi_init`, …). The
   handler prints both. **`l2_op_depth > 1` confirms candidate A outright** and is the single highest-
   value bit in this list.
2. **PL310 latched error status.** Read and print, before anything else:
   `ISR (0xF8F0221C)` raw interrupt status and `IPR (0xF8F02218)` masked status, decoding
   `DECERR (0x100)`, `SLVERR (0x080)`, `ERRRD (0x40)`, `ERRRT (0x20)`, `ERRWD (0x10)`, `ERRWT (0x08)`
   (`xl2cc.h:128-133`); plus the live operation state `CACHE_INV_CLN_WAY (0x7FC)` and
   `DEBUG_CTRL (0xF40)`. Clear via `IAR (0x220)` at init so the next event's bits are fresh.
   This is what separates "the L2's master port got an error response" (A/B) from "a core-issued
   buffered access got one" (C/D), and it is also the only way to turn `ExT` from a guess into a
   cross-check.
3. **Optionally enable the PL310's own interrupt** (`XPS_L2CC_INT_ID = 34`, `xparameters_ps.h:188`)
   via `IER (0x214)`, with a handler that records and clears. The controller then reports its errors
   on its own line, independent of whether the CPU ever takes an abort — which would also catch
   errors that today are absorbed silently.
4. **Fuller CP15 capture:** `ADFSR` (c5,c0,1) and `AIFSR` (c5,c0,3) alongside DFSR/DFAR, plus
   `SPSR_abt` and the raw `LR_abt`. `SPSR_abt`'s mode field says whether the abort interrupted
   handler or foreground context — a direct test for candidate C. Print DFAR and `WnR` still, but
   **label them UNKNOWN-for-async in the output text**, so a future reader is not misled the way this
   one's five lines misled the bench.
5. **A short ring of recent cache-maintenance operations** — 16 entries in `.bss`, each
   `{op, addr, size, global-timer timestamp}`, written by the maintenance wrappers, dumped by the
   handler. No allocation, no `printf` in the handler.
6. **Board telemetry at the moment of the abort.** The handler already sleeps 100 ms; sampling the
   LTC2990 rails (`Z3660/src/ltc2990/`) and the XADC die temperature (`Z3660/src/adc.c`) before the
   reboot is affordable, and it is **the only measurement in this list that speaks to the electrical
   family** (§6). Without it, a recurrence still cannot be assigned.
7. **Persist the record across the reboot.** Write the whole capture to a fixed OCM address (or the
   SLCR reboot-status scratch bits) with a magic word, and have the post-reset banner print
   "previous boot ended in a data abort: …". Otherwise every occurrence costs a session and yields
   five lines that scroll past before anyone can act.

**Two cheap experiments to register alongside** (code changes, so out of scope for this round):
mask IRQ+FIQ across `Xil_L2CacheFlush()` the way `Xil_L1DCacheFlush()` already does, and/or replace
the whole-cache by-way flush on the `SETFSH`/`LOADFS` paths with a by-range flush over the
`DeviceNode` only. Either one, if the event stops recurring, is evidence for A or B respectively —
but only if a recurrence rate exists to compare against, which today it does not.

---

## 9. What this document does not claim

* **It does not claim a root cause.** One occurrence. Candidate A is ranked first on mechanism and
  rarity arithmetic, not on evidence that it happened.
* **It does not claim the access was a write.** `WnR` is UNKNOWN for an asynchronous fault (§3.1).
  The write hypothesis rests on the context, not on bit 11.
* **It does not claim an address.** DFAR is UNKNOWN. `0x00000000` is a stale register.
* **It does not claim `ExT=1` means SLVERR.** The architecture calls the bit IMPLEMENTATION DEFINED
  and this fork has not pinned it.
* **It does not attribute the event to the guest, to the 68060, or to anything on the Amiga side.**
  §3.2 excludes those windows architecturally, and the guest cannot reach core 0's page table.
* **It does not claim the firmware is unsafe to run.** Three boots of the same firmware, same config,
  same code path, in the same session did not reproduce it. The disk image the crashed run had open
  was CRC-identical afterwards — nothing was written wrongly to media.
* **It does not propose or make a firmware change.** §7 and §8 are assessments and proposals.

---

## 10. Reproducing the identification

```bash
cd z3660-firmware/Z-TURN/vitis_ide

# 1. the archived image is the one the card reported
python3 -c "import zlib,sys; b=open(sys.argv[1],'rb').read(); \
  print('%08X'%(zlib.crc32(b)&0xffffffff), len(b))" \
  Z3660_system/Alfa/sd_card/archive/BOOT-lean-9CE3F6A6.BIN
# -> 9CE3F6A6 12631004        (card reported: 9CE3F6A6, 12631004 bytes)

# 2. the core-0 partition inside it is this build tree's Z3660.bin
python3 -c "b=open('Z3660_system/Alfa/sd_card/archive/BOOT-lean-9CE3F6A6.BIN','rb').read(); \
  c=open('Z3660/build/Z3660.bin','rb').read(); i=b.find(c[:4096]); \
  print(hex(i), b[i:i+len(c)]==c)"
# -> 0x409240 True

# 3. the abort PC, and the instruction the board read back
arm-none-eabi-objdump -d --start-address=0x0100b2a4 --stop-address=0x0100b2e8 Z3660/build/Z3660.elf
arm-none-eabi-addr2line -f -e Z3660/build/Z3660.elf 0x0100b2c8
# -> Xil_L2CacheFlush   .../standalone_v9_0/src/xil_cache.c:1428

# 4. every caller of that routine in the running image
arm-none-eabi-objdump -d Z3660/build/Z3660.elf | grep 'bl.*<Xil_L2CacheFlush>'
# 0x1185ae0 is handle_piscsi_reg_write, preceded by the "Found no handler" printf (scsi.c:1878)
# 0x104b24c is isr_video  -- the 60 Hz vblank ISR calls the same whole-cache flush
```

## 11. Source references

Firmware (paths relative to `z3660-firmware/Z-TURN/vitis_ide/`):

| What | Where |
|---|---|
| Abort handler, reboot policy | `Z3660/src/main.c:535-576` |
| `hard_reboot()` | `Z3660/src/cpu_emulator.c:312-329` |
| Core-0 page table | `Z3660/src/main.c:1071-1088`; RTG/board alias `main.c:634-695` |
| Cache policy constants | `Z3660_emu/src/defines.h:9-19` (`RAM_CACHE_POLICY` = WB/WA) |
| `SETFSH` "no handler" path | `Z3660/src/scsi/scsi.c:1854-1890` |
| Vblank ISR whole-L2 flush | `Z3660/src/video.c:161-174`, `video.c:1052-1056` |
| Abort stack size | `Z3660/src/lscript.ld:16`; core-0 load address `lscript.ld:24` |
| `CPSR.A` cleared at boot | BSP `.../cortexa9/gcc/boot.S:378-380` |
| Abort vector, `DataAbortAddr = lr-8`, return `SUBS PC, LR, #8` | BSP `.../cortexa9/gcc/asm_vectors.S:146-159` |
| `Xil_L2CacheFlush` / `Xil_L1DCacheFlush` (only the latter masks IRQ+FIQ) | BSP `.../cortexa9/xil_cache.c` |
| PL310 base and register offsets | `xparameters_ps.h:178,188`; `xl2cc.h:43-95,128-133` |
| Default translation table (PL = Strongly-ordered, APB = Device, DDR `0x00100000-0x3FFFFFFF`) | BSP `.../cortexa9/gcc/translation_table.S:24-35,89-102` |

External:

* ARM Architecture Reference Manual, ARMv7-A/R (DDI 0406) — DFSR field definitions, short-descriptor
  fault status encodings (`0b10110` = asynchronous external abort), DFAR/`WnR` UNKNOWN on
  asynchronous faults, external-abort synchronous/asynchronous rules.
* ARM Cortex-A9 Technical Reference Manual (DDI 0407) — MPCore private memory region access rules and
  their SLVERR reporting; `ExT` as an IMPLEMENTATION DEFINED classification of external aborts.
* ARM L2C-310 (PL310) TRM — background cache operations and their completion-poll contract;
  errata 588369 and the `DEBUG_CTRL` workaround the BSP routine implements.
