# AMIX A3000 SCSI (WD33C93 + SuperDMAC) — implementation design

> Companion to `AMIX_SCSI_plan.md`. This is the *implementation* reference: the
> exact register map, the synchronous interrupt model, the **dual-core
> architecture discovery** that changes the disk seam, and every concrete edit.
> Source extraction (verbatim, with line numbers) is archived in
> `/tmp/amix_extract/*.md` (7 agent reports) + cross-checked first-hand against
> `/tmp/winuae_a2091.cpp` and the Z3660 tree.

## 0. Architecture discovery that reshapes the plan

The Z3660 runs **two ARM cores that do NOT share an address space the way the
plan assumed**:

- **core1 = `Z3660_emu`** — the 68k emulator. Owns the guest memory map (the
  `addrbank` dispatch), the MMIO banks, `intlev()`/`read_irq`. The guest's
  `$00DD0000` access lands here. **The WD33C93+SuperDMAC state machine runs on
  core1.**
- **core0 = `Z3660`** — owns the SD card / FatFS, `devs[]`, `handle_piscsi_reg_write`.
  **All disk I/O runs on core0.**

They talk through the `SHARED *shared` struct. core1's `write_scsi_register()` /
`read_scsi_register()` (`Z3660_emu/src/main.cpp:171,225`) post a request into
`shared->{write,read}_scsi*` and **busy-wait**; core0's protothread loop
(`Z3660/src/cpu_emulator.c:313-332`) dispatches it to
`handle_piscsi_reg_write/read()`.

**Consequence:** the plan's "add `a3000_scsi_block_io()` to `scsi.c` and call it
directly" is not directly callable from core1 — disk I/O must cross cores.
**Decision: reuse the existing, proven PISCSI cross-core READ/WRITE path** rather
than add a new shared-struct channel. Zero core0 / zero `SHARED` changes.

### Disk DMA path (the v1 decision — see §5)
core1 drives a disk transfer by staging the existing PISCSI registers over the
existing channel and letting core0's existing READ/WRITE handler do the FatFS
work into the shared **bounce buffer** `SCSI_NO_DMA_ADDRESS = RTG_BASE+0x80000 =
0x18080000` (host DDR, visible to both cores). core1 then scatters/gathers
between that bounce buffer and the guest's real RAM (chip / A3000-fast) using the
emulator's own `put_long/get_long` (bank dispatch) — which is correct regardless
of where AMIX's RAM is physically backed. This **decouples the SCSI work from
Step 7 (RAM presentation)**: it is correct for chip RAM (physical bus), the
not-yet-defined 8 MB fast RAM, and CPU RAM alike. It can later be optimised to a
direct host-pointer DMA once Step 7 fixes the fast-RAM backing (then core0's
existing `$08000000/$40000000` window translation handles it and `used_dma==0`).

PISCSI command codes (core1 `enum`, == core0 `z3660_scsi_enums.h`):
`READ=0x04 WRITE=0x00`, read params `ADDR1..3 = 0x20/0x24/0x28`
(→`piscsi_u32_read[0..2]` = block, byte-len, dst-addr), write params
`WRITE_ADDR1..3 = 0x240/0x244/0x248` (→`piscsi_u32_write[0..2]`), `USED_DMA=0x9C`.
devs[] **unit index == SCSI target ID** (`piscsi_init` maps `config.scsi_num[i]`
→ `devs[i]`), so **target ID 6 → `devs[6]`**. Backend is an `.hdf` FatFS file
(`devs[u].fd > (FIL*)1`); block size in `devs[u].block_size` (512).

## 1. Register map — A3000 SuperDMAC @ $00DD0000 (verbatim from a2091.cpp `mbdmac_*`)

Offset within the `$00DD` page (`addr & 0xfffe` for word; SASR/SCMD also at odd
byte offsets). Word registers:

| off | reg | action |
|----|----|----|
| 0x02 | DAWR | DMA address width (ignored) |
| 0x04 | WTC hi | `wtc = (val<<16)|(wtc&0xffff)` |
| 0x06 | WTC lo | `wtc = (wtc&0xffff0000)|(val&0xffff)` |
| 0x0a | CNTR | control; bit2 `SCNTR_INTEN`=int enable, bit4 `SCNTR_PREST`→reset |
| 0x0c | ACR hi | `acr = (val<<16)|(acr&0xffff)` |
| 0x0e | ACR lo | `acr = (acr&0xffff0000)|(val&0xfffe)` (word-aligned) |
| 0x12 | ST_DMA | start DMA: `if(dma<=0) dmac_dma=1` |
| 0x16 | FLUSH | `istr|=FE_FLG; dma=0` |
| 0x1a | CINT | clear-int: `istr=0; recompute` (does NOT clear WD ASR_INT) |
| 0x1e | ISTR (r) | read: v=istr; if INTS v|=INT_P; istr&=~15; if !dma v|=FE_FLG |
| 0x3e | SP_DMA | stop DMA: `dma=0; istr&=~E_INT` |
| 0x40/0x48 (w) 0x41/0x49 (b) | SASR | write→`wdscsi_sasr` (reg ptr); read→`wdscsi_getauxstatus` |
| 0x42/0x46 (w) 0x43/0x47 (b) | SCMD | write→`wdscsi_put`; read→`wdscsi_get` |

ACR is full 32-bit (the guest DMA address). WTC is the 32-bit word(!) transfer
count — but the WD33C93 `WD_TRANSFER_COUNT` (24-bit byte count, regs 0x12/0x13/0x14)
is the authoritative length for the data phase; WTC is set by the driver too. We
key transfers off the WD TC byte count (`gettc()`).

CNTR bits: `SCNTR_TCEN=0x20 PREST=0x10 PDMD=0x08 INTEN=0x04 DDIR=0x02 IO_DX=0x01`.
ISTR bits: `INT_F=0x80 INTS=0x40 E_INT=0x20 INT_P=0x10 UE=0x08 OE=0x04 FF=0x02 FE=0x01`.

### WD33C93 registers (indirect via SASR index / SCMD data)
`WD_OWN_ID=0x00 CONTROL=0x01 TIMEOUT=0x02 CDB_1..12=0x03..0x0e TARGET_LUN=0x0f
COMMAND_PHASE=0x10 SYNCH=0x11 TC_MSB/_2/_LSB=0x12/0x13/0x14 DEST_ID=0x15
SRC_ID=0x16 SCSI_STATUS=0x17 COMMAND=0x18 DATA=0x19 QUEUE_TAG=0x1a AUX_STATUS=0x1f`.
`incsasr`: index auto-increments after each SCMD access **except** AUX_STATUS,
DATA, COMMAND (sticky), and SCSI_STATUS on write only.
ASR bits: `INT=0x80 LCI=0x40 BSY=0x20 CIP=0x10 PE=0x02 DBR=0x01`.
WD cmds: `RESET=0x00 ABORT=0x01 ASSERT_ATN=0x02 NEGATE_ACK=0x03 DISCONNECT=0x04
SEL_ATN=0x06 SEL=0x07 SEL_ATN_XFER=0x08 SEL_XFER=0x09 TRANS_INFO=0x20 TRANSFER_PAD=0x21`.
CSR codes: `SELECT=0x11 SEL_XFER_DONE=0x16 XFER_DONE=0x18 SRV_REQ=0x88 DISC=0x85
MSGIN=0x20 TIMEOUT=0x42 UNEXP=0x48 INVALID=0x40`. Phases (low nibble of CSR):
`DATA_OUT=0 DATA_IN=1 COMMAND=2 STATUS=3 MSG_OUT=6 MSG_IN=7`.
COMMAND_PHASE progression: `0x00 idle, 0x10 sel+msgout, 0x20 sel+cmd, 0x30+ CDB,
0x44/0x45 data, 0x46 status, 0x47 status-accepted, 0x50 msg-in, 0x60 complete`.

## 2. Interrupt model — synchronous port (THE high-risk part)

WinUAE level equation (memorise):
```
INT2 = (SDMAC.CNTR & INTEN) && ( (WD.auxstatus & ASR_INT) || (SDMAC.ISTR & E_INT) )
```
`ASR_INT` is the master; `ISTR.INTS` mirrors it. The Amiga line is **level-driven**
(no edge latch) — recompute after every event. WinUAE delivers statuses through a
2-deep queue spaced by an hsync countdown; **we drop the countdown but keep the
queue + the "one pending at a time" rule**:

- `set_status(code, delay)` → append `{code}` to a 2-entry FIFO. (delay ignored.)
- `wd_check_interrupt()`: if no INT pending (`!(auxstatus&ASR_INT)`) and FIFO
  non-empty → `doscsistatus(fifo[0])` = `wdregs[SCSI_STATUS]=code; auxstatus|=ASR_INT`,
  pop, recompute INT2. Call it at the end of every command handler.
- Guest reads `WD_SCSI_STATUS` → clears `ASR_INT`, clears `ISTR.INTS`, then
  `wd_check_interrupt()` again → **pumps the next queued status** (re-raises).
- **`SEL_XFER_DONE(0x16)` then `DISC(0x85)`** — `wd_cmd_sel_xfer` queues `SEL_XFER_DONE`,
  and `DISC` **only when `WD_CONTROL & CTL_EDI(0x08)` is clear** (matches WinUAE
  a2091.cpp:1257-1266). When both are queued they are TWO interrupts the driver acks
  with two separate SCSI_STATUS reads — never collapse them; but never emit the
  trailing `DISC` when the driver enabled the ending-disconnect interrupt either.

### IRQ injection into the core (Step 6) — minimal, verified
`intlev()` (`main.cpp:82`) just returns global `read_irq` (physical IPL).
`INT_IPL_ON_THIS_CORE==1` (`main.h:20`) so the live gate is `newcpu.cpp:2623`.
- `main.cpp`: add `volatile int a3000_scsi_irq=0;` (the level: 0 or 2). Make
  `intlev()` return `max(read_irq, a3000_scsi_irq)`.
- `newcpu.cpp:2623`: change gate to compute the effective level so a fresh level-2
  raises `SPCFLAG_DOINT`: `int eff=intlev(); if(eff>regs.intmask||eff==7){...}`.
- `check_uae_int_request()` runs every instruction → the level-2 is taken at the
  next boundary when `regs.intmask < 2`. Level-driven: clearing `a3000_scsi_irq`
  drops it safely (do_specialties re-checks `intr>intmask`).
- Do **NOT** use `amiga_interrupt_set()` — that drives a physical FPGA INT6 pin.

## 3. The MMIO bank (Steps 4-5)
`addrbank` (memory.h:114, order: `lget,wget,bget, lput,wput,bput, xlateaddr,check,
baseaddr,label,name, lgeti,wgeti, flags, jit_r,jit_w, …`). Handlers get the **full
68k address**; subtract `0x00DD0000`. Model the bank literal on `z2scsi_bank`
(uae_emulator.cpp:814) but **without** the RTG-aperture coupling. `RANGE_MAP`
granularity is one 64 KB page. Replace `uae_emulator.cpp:956`
`RANGE_MAP(0x00DD,0x00DE,slow_bank)` → `RANGE_MAP(0x00DD,0x00DE,a3000_scsi_bank)`.
Leave `$00DC` (RTC) and `$00DE-$00E0` (mobo/custom) alone. Call `a3000_scsi_init()`
once in `uae_emulator()` before the loop.

## 4. CDB target emulation (we synthesize — no oracle in scsi.c)
The WD33C93 only transports CDBs; the *target* (disk) interpretation is ours
(WinUAE's `scsi_emulate_*` is the part we deliberately do NOT port). We synthesize
standard responses; `z3660_scsi.c` is only a partial cross-check (it has no
REQUEST SENSE and uses RMB=1 — we use clean fixed-disk values):
- **INQUIRY (0x12):** 36 bytes — type 0 (direct-access), **RMB=0**, **ANSI ver 2**,
  resp-format 2, addl-len 31, vendor `"Z3660   "` product `"AMIX SCSI Disk  "`
  rev `"0.1 "`.
- **READ CAPACITY (0x25):** `[last_LBA(4 BE)][block_len=512(4 BE)]`, last_LBA = blocks-1.
- **MODE SENSE (6/0x1A & 10/0x5A):** 4-byte header + 8-byte block descriptor; page
  0x03 (sectors/track), 0x04 (cyls/heads), 0x3F all. Clean (not the z3660_scsi.c bug).
- **TEST UNIT READY (0x00) / START STOP (0x1B):** GOOD, no data.
- **REQUEST SENSE (0x03):** fixed 0x70 sense, key/ASC/ASCQ, addl-len 0x0A.
- **READ 6/10 (0x08/0x28), WRITE 6/10 (0x0A/0x2A):** parse LBA+len → disk DMA (§5).
Status: GOOD=0x00 / CHECK CONDITION=0x02 (then REQUEST SENSE returns the latched key).

## 5. Data phase = `a3000_scsi_dma(unit, lba, bytecount, guest_acr, write)` (core1)
- READ: stage `ADDR1=lba, ADDR2=bytecount, ADDR3=guest_acr`; `write_scsi_register
  (PISCSI_CMD_READ, unit)`. Read `USED_DMA`; if non-zero (core0 bounced into
  `SCSI_NO_DMA_ADDRESS`) → invalidate that range, `put_*` copy bounce→guest_acr.
- WRITE: `get_*` gather guest_acr→`SCSI_NO_DMA_ADDRESS`, flush; stage
  `WRITE_ADDR1..3`; `write_scsi_register(PISCSI_CMD_WRITE, unit)`.
- Chunk if `bytecount >= 0x180000` (core0 bounce-buffer cap). One WD command at a
  time ⇒ the single shared bounce buffer is safe (no AmigaOS PISCSI driver under AMIX).

## 6. Open items (need Step 7 / hardware — out of SCSI scope)
1. **RAM presentation (Step 7):** present 2 MB chip + 8 MB A3000 fast, hide the
   256 MB Z3 + 128 MB CPU RAM. Where the 8 MB is *backed* decides the DMA fast-path.
2. **Cache coherency** across the core0/core1 bounce buffer — invalidate on core1
   after a READ, flush before a WRITE (mirror `handle_piscsi_reg_write`'s L1 flush).
3. **Which WD command AMIX uses** (`SEL_ATN_XFER` autonomous vs step-by-step
   `SELECT`+`TRANS_INFO`) — we port both paths to be safe; confirm by trace.
4. Hardware boot (Milestones 0/1) on the target board.

## 7. Hardware-boot findings (2026-06-01) — SCSI works; vatosde panic FIXED

The controller is hardware-validated (KS 2.04 `a3k204.rom` + raw `Amix.hdf`): the real A3000
`scsi.device` drives it through SEL/INQUIRY/TUR/READ CAPACITY/MODE SENSE/READ(6) + DMA + level-2 IRQ.
Boot then progressed in two hard stages:

### 7a. The 52× INTREQR fix
With a 16 MB fast-RAM cap AMIX crept at ~2 blocks/s. The guest was spin-polling Paula
INTREQR ($DFF01E) / INTENAR ($DFF01C) ~39 K times/block waiting for the SCSI INT2, because the
software level-2 (`a3000_scsi_irq`) never set the real Amiga INTREQ. Fix (`cpu_emulator.cpp`
`ps_read_8/16/32`): OR the PORTS bit (0x0008) into INTREQR reads while `a3000_scsi_irq` is pending →
846 ms → 16 ms/block. AMIX then loads the full ~1.45 MB kernel from UNIX_Boot.

### 7b. `vatosde() address not in SCN1` — the RAM-base panic, FIXED
AMIX (confirmed by WinUAE's twilen) supports only **≤16 MB mainboard RAM in `$07000000–$08FFFFFF`**,
and a real A3000 has it at **`$07000000`**, where AMIX maps and *runs* its kernel. The Z3660 only had
RAM at `$08000000`, so the kernel ran from the wrong base → `vatosde`.

Every attempt to add RAM at `$07000000` failed until the real blocker was found: **`get_real_address()`
in `uae/include/memory.h` is hardcoded `return (uae_u8*)addr` (guest==host 1:1) and ignores a bank's
`xlateaddr`.** So a `$07000000` bank backed elsewhere was read at the wrong host address (host
`$07000000` is core0's video `DECODED` buffer). Fix: `get_real_address()` translates the `$07xxxxxx`
window → host `$09xxxxxx` under `a3000_amix_mode`. Then present (gated `enable_mmu`):

- **8 MB a3000mem @ guest `$07000000`**, backed by free host DDR `$09000000` (`a3000mem_bank`,
  swap32, `a3000mem_xlate`), and
- **8 MB CPU RAM @ `$08000000`** (`drct_bank`, host 1:1) = 16 MB total.

The AmigaOS loader still needs `$08000000` (it loads/decompresses the kernel at `$08008FD8`; hiding
`$08000000` makes it loop at prompt 1). After decompression the kernel **relocates to `$07000000`** —
`68030 MMU enabled. Page size = 2048 PC=07000fe6` (baseline was `08000fe6`) — and runs *past* the panic.

### 7c. STILL OPEN: relocating wasn't enough — `vatosde` still panics, now at `$07xxxxxx`
A fresh panic photo (kernel at `$07000000`) shows the SAME `PANIC: vatosde() address not in SCN1`, but the
backtrace return addresses are now `$07xxxxxx` (`7000044`, `703E640`, `70B74A4`, …) with the kernel virtual
stack at `$40001Exx`. So §7b relocated the kernel correctly but did **not** cure the panic. (The screen
"mouse poll" after relocation is AMIX's **panic-reboot prompt**, not a boot prompt — earlier
left/right/middle button injection experiments were dismissing a *panic*; dead ends, ignore.)

**AMIX-guru root cause:** AMIX's kernel MMU has a limited mapping/address space; **too much RAM leaves no
address space for I/O**, so a translation lands outside its segment (`vatosde … not in SCN1`). 16 MB (our
8 + 8) is over the line; **Amiberry boots AMIX with 8 MB**. Fix direction = **reduce the RAM AMIX sees to
~8 MB**, not relocate it. Knobs: `AMIX_A3000MEM_MB` / `AMIX_CPURAM_MB` in `uae_emulator.cpp`. Tension: the
loader needs direct-access RAM at `$08000000`, AMIX wants ~8 MB at `$07000000`, and AMIX seems to count
both banks — so options: split 4 + 4; binary-search the limit (12 → 10 → 8); make a3000mem
`ABFLAG_DIRECTACCESS` to run a true 8 MB-only-at-`$07000000` Amiberry config; or stop AMIX counting
`$08000000`.

Success signal: SCSI reads of **UNIX_Root (LBA 128–1638527)** = root mounted. AMIX has no serial console
(display only). Diagnostics (`A3000SCSI_LOG`, `[CLICK]`, `[BUS]`, `ps_*` tally) are temporary.
