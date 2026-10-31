# Real-CPU AutoConfig and the extended-ROM window

**2026-08-24.** Investigation of the two anomalies BLIZZARD F0 raised on the first
`bootmode CPU` boot this project has ever run (real 68060, KS 3.2.3, A4000 + Z3660).
Evidence (held locally): the 2026-08-24 session's serial boot log, HDMI captures and config delta.

Written pre-registration-style: the verdicts first, then what each rests on, then what is
predicted of the changes made alongside it, so a later metal session can score this page
against itself rather than re-argue it.

---

## 0. Verdict, up front

Kickstart's Expansion Board Diagnostic listed two boards:

| Board | Type | Vendor | Product | Address | Size | Status |
|-------|------|--------|---------|---------|------|--------|
| 1 | Z2 | 15163 (`0x3B3B`) | 200 (`0xC8`) | `$40000000` | 4MB | **Bad** |
| 2 | Z3 | 5195 (`0x144B`) | 1 | `$10000000` | 128MB | OK |

1. **Board 2 is not a defect and not the FPGA.** It is the *extended ROM itself* — the
   image loaded at `$00F00000` runs a `RTF_COLDSTART` resident that hand-builds a
   `ConfigDev` and calls `AddConfigDev()`. Every one of the four fields above is a literal
   in `z3660-drivers/kick31_060/kick31_060.asm`. §3.

2. **Board 2 is therefore NOT Fast RAM**, and BLIZZARD's F0-M5 "BONUS" reading is wrong on
   this point. Its `er_Type` is `$93`, which has `ERTF_MEMLIST` **clear**, so AmigaOS does
   not add it to the free-memory list. **F0-M5's memory-contract question is still open.**
   §3.3 — this is the correction with the most downstream consequence.

3. **Board 1 is not an identity this firmware can emit.** Every AutoConfig identity the
   card owns — FPGA Zorro II, FPGA Zorro III, and the extended ROM — uses manufacturer
   `0x144B` (5195), and in both FPGA chains the manufacturer nibbles are **unconditional**
   assignments, so no stale or half-programmed chain state can change them. `0x3B3B`
   appears nowhere in the tree. Its shape is the signature of an **all-ones `er_Type`**
   read with no responder: `$FF` decodes as exactly "Zorro II, 4 MB". §4, and §4A for the
   two hypotheses tested against it after the ghost proved reproducible.
   **No device this card can present advertises 4 MB** — the only sizes it knows are
   64 kB, 128 MB and 256 MB (§4A.3).

4. **A1's premise is false in both halves.** The extended-ROM load did **not** fail — the
   ROM demonstrably executed (board 2 exists). And the `Unknown hunk type 00000000` line is
   not the extended ROM at all: it comes from `pifloppy_init()` parsing `z3660_scsi.rom` at
   a hardcoded offset that holds zeros, ~40 lines later in `main()`, with nothing printed
   in between. It was log adjacency, not causation. §5.

5. **There is nevertheless a real fail-open**, just not the one that fired: `load_romext()`
   validated *nothing* — not the size against the window, not `f_read`'s byte count, not the
   image id — and returned success unconditionally. Fixed here, fail-closed, with a host
   regression arm. §5.2, §7.

6. **One ARM-side ordering defect was found and is deliberately NOT shipped in this pass.**
   The real-CPU path never pulses `FPGA_RESET` after writing the FPGA's feature bits, so
   `MAPROM`/`MAPROMEXT` reach the FPGA only if the Amiga's own reset happens to latch them.
   It matches the WIP defect upstream documents, but it is **not** the cause of either board
   above, and F0 proves the current timing worked. Pre-registered in §6 for a metal session,
   not changed blind.

---

## 1. Who answers AutoConfig when the 060 is the real CPU

The ARM contributes **nothing**. `main.c:1694-1696` publishes `shared->z2_enabled` /
`shared->z3_enabled`, but those words are read only by the core-1 emulator
(`Z3660_emu/src/cpu_emulator.cpp:218-219`, decoded at `:274-334`), and core 1 never starts
in `bootmode CPU` — `main.c:1788` is the only site that starts it and is gated `emu==1`.

On the real-CPU path the answers are hard-wired in the FPGA:

- `z3660.v:853-932` — the Zorro II board (the piscsi boot board)
- `z3660.v:934-1007` — the Zorro III chain, two sub-boards: FastRAM and RTG

with the chains gated by registers latched from REG0:

```
z3660.v:405-409   enabled_z2       <= GPIO_s[14];   // FPGA_SCSI_AUTOCONFIG_EN
                  enabled_z3[0]    <= GPIO_s[12];   // FPGA_256MB_AUTOCONFIG_EN
                  cpu_ram_enable   <= GPIO_s[0];
                  maprom_enable    <= GPIO_s[3];
                  mapromext_enable <= GPIO_s[7];
z3660.v:393-401   GPIO_s[13] (RTG) selects between "autoconfig" and
                  "pre-configured aperture at $10000000, enabled_z3[1] <= 0"
```

`GPIO_IN` is REG0 (`z3660.v:1584`, `assign GPIO_IN = s01_slv_reg0`), and all of the above
sits **inside** the reset branch `if((RESET_IN==1'b1) || (GPIO_s[31]==1))` (`z3660.v:374`).
That detail is what §6 is about.

### The FPGA's identity constants, decoded

AutoConfig registers are read as nibble pairs, and everything after `er_Type` is read
inverted. Decoding `z3660.v:963-1004`:

| register | FastRAM sub-board | RTG sub-board |
|---|---|---|
| `er_Type` | `$A4` → Z3, `ERTF_MEMLIST` set, 256 MB | `$8B`/`$9B` → Z3, no `MEMLIST`, 128 MB |
| `er_Product` | `~$FD` = **2** | `~$FE` = **1** |
| `er_Manufacturer` | `~$EB,$B4` = **`0x144B` = 5195** | same, **5195** |

The Zorro II board (`z3660.v:877-910`) is **product 3, 64 kB**, manufacturer likewise 5195.

**So 5195 is the only manufacturer number this card can present, on any chain.**

---

## 2. What the session actually configured

From `04-CONFIG-DELTA-armA.txt` and the serial banner (`15-ARMB-boot.txt:64-78`):

```
bootmode CPU      scsiboot NO       autoconfig_ram NO
autoconfig_rtg NO cpu_ram YES       amix_mode NO      ext_kickstart 2
```

and the firmware agreed, twice: `FPGA RTG AUTOCONFIG DISABLED` at `:22`, and `mobotest`
printing `Z3 RAM Disabled` / `Z3 RTG AUTOCONFIG Disabled` / `SCSI BOOT Disabled` at
`:127-129`. **All three AutoConfig chains were switched off, and the firmware said so.**

---

## 3. Board 2 is the extended ROM, not the FPGA

`ext_kickstart 2` = `1:/kicks/kick060_scsi.rom`, 384 bytes — byte-identical to this repo's
`z3660-drivers/kick31_060/kick060_z3660scsi.rom` (`0x180` = 384; the SD copy is the same
file under a shorter name). It is **not** a hunk file: it is a raw DiagROM-format image
beginning `1111 007c` (`kick31_060.asm:56`, `dc.w $1111 ;magic DiagROM ID`).

Its `ExpansionIDMod` resident (`kick31_060.asm:159-229`) is compiled in — visible in the
shipped binary at offset `0xC5`:

```
203c 0000 144b   move.l #$144B,d0        ; manufacturer 5195
7201             moveq  #1,d1            ; product 1
4eae ffb8        jsr    -72(a6)          ; FindConfigDev   -- already registered?
4eae ffd0        jsr    -48(a6)          ; AllocConfigDev
217c 1000 0000 0020   move.l #$10000000,cd_BoardAddr(a0)
217c 0800 0000 0024   move.l #$08000000,cd_BoardSize(a0)     ; 128 MB
117c 0093 0010        move.b #$93,er_Type(a0)  ; ERT_ZORROIII|ERTF_DIAGVALID|3
117c 0001 0011        move.b #1,er_Product(a0)
317c 144b 0014        move.w #$144B,er_Manufacturer(a0)
4eae ffe2        jsr    -30(a6)          ; AddConfigDev
```

**Z3 / 5195 / 1 / `$10000000` / 128 MB** — an exact four-field match to board 2.

### 3.1 Which also proves the extended ROM worked

The resident is `RTF_COLDSTART`, so Kickstart's RomTagScanner had to find its `$4AFC`
matchword inside `$00F00000..$00F7FFFF` and call its init vector. **Board 2's existence is
proof that MAPROMEXT was enabled, the image was intact, and it executed.** Whatever else
A1 is, "the extended kickstart failed to load" is not it.

### 3.2 Why the FPGA is ruled out for board 2

With `autoconfig_rtg NO`, `z3660.v:393-396` sets `configured_z3[1] <= 1` and
`enabled_z3[1] <= 0`; with `autoconfig_ram NO`, `enabled_z3[0] <= 0`. Both terms of
`AUTOCONFIG_Z3_RANGE` (`z3660.v:280`) then resolve false, so the FPGA never claims
`$FF000000`. The FPGA's *own* RTG identity would in any case be reached only through that
chain, and the software-built ConfigDev above explains all four fields without it.

### 3.3 The correction to BLIZZARD F0-M5

`docs/BLIZZARD.md` records M5's bonus as *"`bootmode CPU` + `amix_mode NO` + `cpu_ram YES`
delivers a 128 MB Z3 board at `$10000000`, Status OK — the AMIX memory question moves to
'does the kernel take autoconfig Z3 RAM there'"*. That is not what was observed:

- The board is **declared in software**, by a ROM module, not autoconfigured. It would
  appear with `cpu_ram` set to anything, because nothing in that code path consults it.
- `er_Type = $93` = `ERT_ZORROIII | ERTF_DIAGVALID | 3`. **`ERTF_MEMLIST` ($20) is clear.**
  AmigaOS adds a board's memory to the free list only on `ERTF_MEMLIST`. So this board
  publishes an *address range*, not usable Fast RAM.
- `cd_Flags` is set to `CDF_CONFIGME` and `ConfigBoard()` is commented out
  (`kick31_060.asm:200, 208-213`) — the board is registered as "still needs configuring".
- What actually lives at `$10000000` in this posture is the FPGA's **RTG aperture**:
  with RTG autoconfig disabled, `z3660.v:394` hard-parks `autoConfigBaseRTG = $1000` and
  `RTG_CONFIGURED_RANGE` (`z3660.v:285`) decodes `$10000000..$17FFFFFF`. The ROM module's
  job is to tell AmigaOS that aperture exists — that is exactly the "Option 2" behaviour
  `CHANGES.md:79` describes.

**F0-M5 is therefore unanswered, not answered, and F4's pre-registration must still carry
both memory shapes.** Nothing about AMIX Fast RAM follows from board 2.

---

## 4. Board 1 — the ghost

`Z2 | 15163 (0x3B3B) | 200 (0xC8) | $40000000 | 4MB | Bad`.

### 4.1 It is not the card

Manufacturer `0x3B3B` is not in this tree. The card's three possible AutoConfig identities
are FPGA-Z2 (5195/product 3/64 kB), FPGA-Z3 (5195/product 2 or 1), and the ROM module
(5195/product 1). None can produce 15163/200/4MB.

### 4.2 Its shape is an all-ones `er_Type`

`er_Type` is the one AutoConfig byte read **non-inverted**. `$FF` decodes as:

```
bits 7-6 = 11  -> ERT_ZORROII      -> the diagnostic prints "Z2"
bits 2-0 = 111 -> size code 7      -> the diagnostic prints "4MB"
```

**"Z2 + 4MB" is precisely what an all-ones `er_Type` read produces**, with no board present.
The remaining fields are read inverted, so they report the varying bus residue rather than
`$00`: vendor `0x3B3B` = `~0xC4C4`, product `0xC8` = `~0x37`. Repeated-byte patterns like
`C4 C4` are what a bus holding its previous cycle's charge looks like. The `Bad` status and
the `$40000000` address column follow from Kickstart being unable to place the thing.

### 4.3 Why F0 was the first boot that could see it

`AUTOCONFIG_Z2_RANGE` (`z3660.v:279`) is claimed only while `enabled_z2` is set, which
tracks `FPGA_SCSI_AUTOCONFIG_EN`, which is set only when `config.scsiboot`
(`main.c:1439-1442`, `:1502-1505`). When it is clear the FPGA forwards the cycle to the
CPLD and the motherboard (`z3660.v:457-458`).

F0 was the **first `bootmode CPU` boot in this project's recorded history**, and it ran
`scsiboot NO` (a ratified session deviation, taken for image safety). Every earlier metal
session ran an emulated mode, where core 1 answers the chain in software regardless
(`main.c:1468-1470`). So F0 is the first boot in which **nothing at all answered the Zorro
II chain** — which is exactly the condition under which an unanswered probe is possible.

### 4.4 Honest limit

That an unanswered probe is *sufficient* to produce the ghost is a leading hypothesis, not
a proven fact — a stock A4000 with empty slots does not normally raise this requester, so
something about how this rig terminates an unclaimed AutoConfig cycle is part of the story,
and that part is in the CPLD and the board, not in code this repository can change. §8
states what cannot be reached from here.

---

## 4A. Addendum — reproducibility, and two hypotheses tested to destruction

Added after §4 was written, on two pieces of live input: the ghost reappeared with the
**same signature** in a second, independent session (the thermal run following F0), and two
mechanism hypotheses were put forward. Both are refuted below, one of them three ways. The
reproducibility does **not** move the verdict, for the reason in §4A.1.

### 4A.1 "Reproducible, therefore not floating" does not follow

An undriven bus read is *deterministic* whenever the cycles preceding it are. Between
AutoConfig reads the 68060 fetches instructions from the same ROM, executing the same
expansion.library code, in the same order, on every boot. If the data lines hold the
previous cycle's value, the residue is the **same residue every time**. Two sessions
agreeing is therefore exactly what an unanswered read predicts — it rules out *randomness*,
not *no responder*. What it does rule out is anything genuinely uninitialised in DDR, which
would vary with boot history.

### 4A.2 The size-code arithmetic — a correction that inverts the inference

The Zorro II size field is `er_Type` bits 2-0, and **`000` is 8 MB, not 4 MB. 4 MB is
`111`** — the all-ones end of the field, not the zero end. Corroborated three ways inside
this tree, against the FPGA's own annotated constants:

| `z3660.v` | nibble | comment | standard code |
|---|---|---|---|
| `:880` Z2 | `0001` | *"64 kB"* | Z2 `001` = 64 kB ✓ |
| `:971` Z3 FastRAM | `0100` | *"256MB"* | Z3 ext. `100` = 256 MB ✓ |
| `:973` Z3 RTG | `1011` | *"128MB"* | Z3 ext. `011` = 128 MB ✓ |

So the record behind the ghost is at the **all-ones** end, not the half-programmed or
mostly-zero end. That matters twice over:

- A mostly-**zero** `er_Type` has bits 7-6 = `00`, which expansion.library reads as **"no
  board"** and uses to terminate the chain. A half-programmed record does not become a
  visible board — it becomes an *absent* one. It cannot be what is on screen.
- `$FF` gives bits 7-6 = `11` (Zorro II) and bits 2-0 = `111` (4 MB). **"Z2 + 4 MB" is
  precisely and only what an all-ones `er_Type` reads as.**

Note also that **4 MB is not expressible at all in the Zorro III size encoding** (its
minimum is 16 MB), so no Zorro III record, in any state of corruption, can display 4 MB.

### 4A.3 Every AutoConfig identity this firmware can present — and none is 4 MB

Asked directly: what might the 4 MB device be? The complete enumeration:

| # | Device | Chain | `er_Type` raw | Shows as | Advertised size | Vendor | Product | Gated by |
|---|---|---|---|---|---|---|---|---|
| 1 | piscsi boot board | Zorro II `$E80000` | `$D1` | Z2, DIAGVALID | **64 kB** | 5195 | 3 | `scsiboot` → REG0 b14 |
| 2 | Z3 Fast RAM | Zorro III `$FF000000` | `$A4` | Z3, MEMLIST | **256 MB** | 5195 | 2 | `autoconfig_ram` → b12 |
| 3 | Z3 RTG/combo | Zorro III `$FF000000` | `$8B`/`$9B` | Z3 | **128 MB** | 5195 | 1 | `autoconfig_rtg` → b13 |
| 4 | ext-ROM ConfigDev | *none* — `AddConfigDev` | `$93` | Z3, DIAGVALID | 128 MB | 5195 | 1 | the ext-ROM image |

Non-AutoConfig apertures, which never appear in any chain: CPU RAM (`$08000000`, 128 MB),
the RTG aperture when RTG AutoConfig is off (`$10000000`, 128 MB), MAPROM (`$00F80000`,
512 kB), MAPROMEXT (`$00F00000`, 512 kB).

**The only sizes this card knows how to advertise are 64 kB, 128 MB and 256 MB. There is no
4 MB device, and no combination of enable bits can make one.**

### 4A.4 Hypothesis A — a stale or half-programmed FPGA chain record. Refuted, three ways

The proposal: with RTG/SCSI/256MB AutoConfig all disabled, one of the disabled boards' chain
state is left half-initialised and answers with a corrupt record.

1. **Manufacturer is unconditional.** In *both* chains the manufacturer nibbles are plain
   assignments with **no `if` guard at all** — `z3660.v:900-911` (Zorro II) and
   `z3660.v:993-996` (Zorro III). Any state in which the FPGA answers those addresses
   yields `~$EB,$B4` = **`0x144B` = 5195**. The ghost reads **15163**. No chain state,
   stale or otherwise, can produce that.
2. **Product's high nibble is unconditional too** — `z3660.v:883`, `data[31:28] <= ~4'h0`,
   so the product byte can never exceed `$0F` after inversion. The ghost reads **200**
   (`$C8`). Impossible independently of (1).
3. **The fall-through that would give "Z2 + 4 MB" is unreachable.** The Zorro II read path
   *does* default to `data <= 32'hFFFFFFFF` (`z3660.v:874`) with `er_Type` gated on
   `configured_z2==0 && enabled_z2==1` — so a range-claimed-but-gate-false read would
   indeed return `$FF` and display as **Z2, 4 MB**. But `AUTOCONFIG_Z2_RANGE`
   (`z3660.v:279`) is `~shutup_z2 && ~(configured_z2 | ~enabled_z2)`, i.e. the range
   condition is a strict **subset** of the field gate: whenever the FPGA claims the range,
   the gate is true. The same holds for Zorro III (`z3660.v:280` vs the arms at `:965-986`).
   The fall-through cannot be entered — and even if it were, (1) says it would still carry
   vendor 5195.

**Verdict: the FPGA did not answer these cycles at all.**

### 4A.5 Hypothesis B — the ghost is the SCSI-boot ROM's vehicle board, left armed. Refuted

The proposal is structurally well-aimed: device 1 in §4A.3 genuinely *is* an AutoConfig
board that exists as a vehicle for a boot ROM. It sets `ERTF_DIAGVALID` (`z3660.v:877`,
nibble `1101`) and advertises `er_InitDiagVec = $6000` (`z3660.v:913-923`), so a board whose
DiagArea is missing or corrupt failing validation and being flagged **Bad** is a real and
plausible failure mode. It is nevertheless not what happened:

- **Wrong fields, three of them.** 64 kB not 4 MB; vendor 5195 not 15163 (unconditional,
  §4A.4.1); product ≤ `$0F` not 200 (unconditional, §4A.4.2).
- **`scsiboot NO` does suppress it on the real-CPU path.** REG0 bit 14 is cleared at
  `main.c:1442` (common) *and* at `main.c:1505` (the real-CPU branch's own copy), both
  before the `FPGA_RESET` pulse at `main.c:1577-1580` that latches `enabled_z2 <= GPIO_s[14]`
  (`z3660.v:405`). This is the one place the eight-way copy-paste class does **not** bite:
  the real-CPU branch carries its own correct copy. Independently, `piscsi_init()` returns
  at `scsi/scsi.c:356-360` when `scsiboot==0`, so `z3660_scsi.rom` is never loaded and the
  DiagArea at `$E96000` is never populated — there is no vehicle *and* no cargo.
- **The ext ROM is not this board's boot ROM.** The vehicle board's DiagArea is served from
  `z3660_scsi.rom`, which `piscsi_init()` copies to `$00E96000` and `$10006000`
  (`scsi/scsi.c:396-402`) — i.e. Zorro II base `$E90000` + the `$6000` DiagVec.
  `kick060_scsi.rom` at `$00F00000` is a **different artefact** doing a different job (the
  full-060 FPU disable, and the `AddConfigDev` of §3). The two are not the same chain, and
  `CHANGES.md:79`'s "tiny Zorro II board of 64 kB" is device 1, gated by `scsiboot`, not by
  `ext_kickstart`.
- **The ext ROM cannot inject a second entry.** Its module calls `FindConfigDev` first and
  returns if already present, then does exactly one `AllocConfigDev` → fill-every-field →
  `AddConfigDev` (`kick31_060.asm:182-207`). There is no path to a partially-filled record.

### 4A.6 What the ghost most likely is — revised, and better than §4

Combining §4A.2 with the one fact §4 could not explain — **the address `$40000000`**:

A genuine Zorro II board can never be assigned a Zorro III address. But a board discovered
in the **Zorro III** AutoConfig space (`$FF000000`, which the FPGA does *not* claim in this
posture) whose `er_Type` reads all-ones would present type bits `11` = "Zorro II" **in a
Zorro III slot** — an inconsistent record, hence **Bad** — while being placed at the Zorro
III base the OS was about to use. That accounts for the type, the 4 MB, the Bad, *and* the
`$40000000`, which the Zorro II reading of §4 could not.

It also fixes §4's ordering argument, which was wrong: board 2 is added by an
`RTF_COLDSTART` resident, which runs **after** expansion.library's chain walk. So the ghost
being listed first says nothing about which chain found it — it only says it was found
during the walk and board 2 was appended later. §4's inference from list order is withdrawn.

**Consequence for F4: `scsiboot YES` is now expected NOT to remove the ghost.** Every chain
walk ends with a probe one slot past the last real board, so adding boards moves the
end-of-chain read later without eliminating it. §7 is revised accordingly.

---

## 5. A1, corrected

### 5.1 The hunk error is not the extended ROM

The F0 log reads:

```
Loading Extended Kickstart at address: 0x00F00000
Extended Kickstart file size: 0x00000180
File read OK
Hunk ID: 00000000 (UNKNOWN HUNK TYPE)
[!!!HUNK_RELOC] Unknown hunk type 00000000! Can't process!
Done processing hunks.
```

which reads as cause and effect and is not. `load_romext()` (`cpu_emulator.c:187`) never
calls `process_hunks()`; it ends at `File read OK`. The hunk lines come from **40 lines
later in `main()`**:

- `main.c:1746` `piscsi_init()` — returns immediately, silently, because `config.scsiboot==0`
  (`scsi/scsi.c:356-360`). That is why nothing was printed in between.
- `main.c:1750` `pifloppy_init()` — `floppy/floppy.c:196-201` opens `1:/z3660_scsi.rom`,
  seeks to a hardcoded `0x2600`, and calls `process_hunks(..., debug=1)`. `DEBUG2` is
  `if(debug) printf` (`scsi/hunk-reloc.c:18`), and **this is the only call site in the tree
  that passes `debug=1`** — so it is the only possible source of those lines.

And the parse cannot succeed: the shipped `z3660_scsi.rom` is 16384 bytes, and offset
`0x2600` in it is **all zeros** — hence `Hunk ID: 00000000`. The offset is also not
`PISCSI_DRIVER_OFFSET` (`0x1000`, `scsi/scsi.h:74`), which is what the SCSI loader uses on
the same file. So `pifloppy_hreloc` has always been empty and `reloc_hunks()` at
`floppy.c:550` has always relocated nothing, in every mode, on every boot.

**Left as found.** The correct offset is an upstream question; guessing it here would be
worse than the current no-op. What is fixed is that the open and the seek are now checked,
so the failure names its file instead of walking an uninitialised stack `FIL` into the
parser and printing an alarming line about a different subsystem.

### 5.2 The real fail-open

`load_romext()` checked nothing:

- `size = fil.obj.objsize` was read into a **fixed 512 KB window with no bound** — an
  oversized file overruns `$00F00000+512K` into whatever the ARM map holds above it.
- `f_read`'s `NumBytesRead` was **discarded**; `File read OK` printed unconditionally.
- The function **returned 1 regardless**, so `main.c:1723` enabled MAPROMEXT regardless.
- The window tail behind a short image was left as **uninitialised DDR**.

That last one is the one that matters. Once MAPROMEXT is on, the FPGA answers
`$00F00000..$00F7FFFF` out of DDR (`z3660.v:281`) and Kickstart *scans* the window for
`$4AFC` matchwords and calls what it finds. A 384-byte file published ~512 KB of
uninitialised DDR to the guest. In F0 it happened to be harmless — which is exactly why a
fail-open of this shape survives for years.

`load_rom()` (the main Kickstart window) had the same unbounded read and the same
uninitialised tail; it did check the per-chunk byte count.

---

## 6. The ARM-side latch-ordering defect — pre-registered, NOT shipped

Found while investigating, real, and **not the cause of anything F0 observed**.

The FPGA's feature registers (§1) are assigned **only inside its reset branch**
(`z3660.v:374`), so a REG0 write does nothing until a reset edge. The two paths differ:

- **Emulated** (`main.c:1468-1490`) brackets its feature writes in an explicit
  `DiscreteSet(REG0, FPGA_RESET)` … `DiscreteClear(REG0, FPGA_RESET)` pair — it latches on
  purpose, and it deliberately latches MAPROM/MAPROMEXT **off**, because core 1 emulates
  them and the FPGA must not also claim `$F80000`.
- **Real CPU** (`main.c:1491-1509`) has **no pulse of its own**. Its only latch is the
  incidental pulse at `main.c:1577-1580`, whose actual purpose is `monitor_switch()`.

And REG0 keeps changing for ~160 lines after that pulse: `load_rom()` sets MAPROM (bit 3)
at `main.c:1706`, `load_romext()` sets MAPROMEXT (bit 7) at `:1725`, and `main.c:1771` is a
bare `DiscreteClear` — a no-op, not a pulse. So on `bootmode CPU` those bits reach the FPGA
only if a later, uncommanded reset edge (the Amiga's own `RESET_IN`) latches them.

This is the class of defect upstream documents at `CHANGES.md:41`:

> *"the reset of ARM is not attached to the reset of the Amiga, but sometimes the ARM
> doesn't reinit the Amiga autoconfig (that is done in FPGA when using the 060. EMU is not
> affected) so sometimes you will not see the SCSI, Z3 RAM and RTG boards... WIP..."*

### 6.1 Why it is not changed here

- It explains **neither** board. Board 2 is software (§3); board 1 carries no identity the
  chains could emit (§4).
- **F0 is evidence the current timing worked**: the extended ROM executed, so MAPROMEXT
  *was* latched — by the Amiga's reset, just in time.
- It is a change to the real-CPU boot path of a card that reached first light hours ago,
  and **no metal proof is available yet** (nothing is being flashed, and the
  thermal kill clause has parked long real-CPU runs until cooling is fitted).

### 6.2 The pre-registered change, for whoever picks it up

*Shape.* On the real-CPU path only, immediately before `CPLD_RESET_ARM(1)` / `NBR_ARM(1)`
(`main.c:~1885`, the last moment before the 060 is released), pulse
`DiscreteSet(REG0, FPGA_RESET)` / short delay / `DiscreteClear(REG0, FPGA_RESET)`. It must
be gated off the emulated path, or the FPGA would newly latch MAPROM/MAPROMEXT **enabled**
behind the emulator, which §6's first bullet says would be a regression on the 030/AMIX line.

*Predictions to score.* (a) No visible change to a `bootmode CPU` cold boot, because the
Amiga's own reset already latches the same final REG0 — this is a determinism fix, and "no
change" is the expected result, not a null one. (b) It should remove the *"sometimes"* from
`CHANGES.md:41` for cold boots. (c) It should **not** affect either board in §0's table; if
the ghost disappears when this lands, §4.4's hypothesis is wrong and this page needs
rewriting.

---

## 7. What F4 must do

*Revised after §4A. The single-knob fix in the first draft is withdrawn: `scsiboot YES` is
no longer expected to remove the ghost (§4A.6). What follows is a three-boot ladder, one
variable each, ordered by information per boot. **The vendor field is the discriminator
throughout** — anything the card emits reads 5195 (§4A.4.1), so a ghost that still reads
15163 is off-card.*

1. **Boot A — `ext_kickstart 0`, everything else exactly as F0.** One config edit, and by
   far the highest-information boot in the ladder, because it tests §3 and §4A.5 at once.
   Pre-registered predictions:
   - **Board 2 DISAPPEARS.** It is the ext ROM's own `AddConfigDev` and nothing else
     creates it. If board 2 survives without an extended ROM, §3 is wrong and this page
     needs rewriting from §3 down.
   - **Board 1 is UNCHANGED** — still `Z2 / 15163 / 200 / 4MB / Bad`. If it disappears,
     §4A.5 is wrong, the ext ROM *is* implicated, and the two anomalies really are one.
   - The requester should still appear (raised by board 1), now listing one board.

2. **Boot B — F4's intended posture: `scsiboot YES` + `cpu_ram YES` + `ext_kickstart 2`.**
   Note the firmware requires `cpu_ram YES` alongside `scsiboot` — `main.c:1450-1463`
   disables SCSI boot outright otherwise, and says so on the serial. Predictions:
   - A **new** board appears: `Z2 / 5195 / 3 / 64 kB` (device 1 of §4A.3), **OK** if
     `1:/z3660_scsi.rom` is present and its DiagArea valid, **Bad** if not.
   - **The ghost persists unchanged at 15163 / 4 MB** (§4A.6). If instead the ghost's
     vendor becomes **5195**, then it was the card's own record all along and §4A.4 is
     wrong — that single field decides it.

3. **Boot C — only if A and B disagree with the predictions:** `autoconfig_ram YES` puts a
   256 MB Zorro III board (product 2) on the Zorro III chain. If the ghost's address or
   position moves when the *Zorro III* chain gains a member but not when the Zorro II chain
   does, it is a Zorro III end-of-chain artefact, confirming §4A.6.

4. **Budget for the requester rather than assuming it away.** §4A.6 predicts the ghost is
   an end-of-chain artefact, i.e. **not config-removable**. If boots A and B confirm that,
   F4 needs a way past a modal requester, and the only two candidates left are a working
   remote dismissal (item 5 — currently unproven) or a Kickstart/ROM-side change, which is
   a much larger piece of work than F4 was scoped for. Escalate rather than improvise.

5. **Do not plan on dismissing the requester remotely.** In the F0 evidence the requester
   is *still on screen* in both `18-HDMI-armB-after-enter.jpg` (after a keypress) and
   `19-HDMI-armB-after-click.jpg` (after a mouse click on Continue). Neither dismissal was
   demonstrated. That may be a capture-before-redraw artefact, but it must not be assumed
   away: treat "make the ghost not appear" as the primary path and "dismiss it" as the
   unproven fallback.

6. **`ext_kickstart 2` is correct and should stay** for a SCSI-boot posture — that ROM is
   what registers the `$10000000` aperture and what `CHANGES.md:79` says makes z3660 SCSI
   bootable. Note it is **not** the FPU-disable ROM BLIZZARD D1 wants avoided: the
   FPU-disable path (`kick31_060.asm:83-89`) is gated on `PCR` high word `== $430`, i.e.
   **full 060 only**, and takes the `bne.s no_060_FPU` branch on an LC060 (`$431`). Both
   `kick060.rom` and `kick060_scsi.rom` contain that same gated code.

7. **Re-open F0-M5.** §3.3 — the 128 MB board is not Fast RAM and the AMIX memory contract
   on the real-CPU path is still undetermined.

---

## 8. What this repository cannot reach

**The FPGA image is a vendored binary that this repo cannot rebuild.** `make build` /
`make bootbin` never invoke Vivado; the newest `design_1_wrapper.bit` in the tree dates
from beta 20 (git `c141c85`, 2026-05-10) and carries an internal build stamp of
**2026/02/09 15:39:38**. Beta 21 (`50e98e0`) changed `z3660.v` by exactly one line — an
unrelated `enable_clk_output` metastability fix — and shipped **no new bitstream**.

So `z3660.v` is the best available *description* of the FPGA running on the card, not a
guaranteed one, and every RTL citation on this page should be read with that caveat. Any
residue that turns out to live inside the bitstream is out of reach here by construction,
independent of the dispatch's scope rule.

---

## 9. Pre-registration for the changes made alongside this page

**Changed:** `Z3660/src/rom_window.h` (new), `Z3660/src/cpu_emulator.c`,
`Z3660/src/floppy/floppy.c`, `Z3660_emu/test/host/rom_window_test.cpp` (new) + its Makefile
wiring, `KNOWN_ISSUES.md`.

Predictions, to be scored on the next real-CPU boot:

1. **The F0 posture still boots identically.** `kick060_scsi.rom` is 384 bytes with id
   `$1111` and is admitted unchanged, so board 2 must still appear and the ROM must still
   execute. If it does not, the admission predicate is wrong.
2. **The window tail is now zero.** `$00F00180..$00F7FFFF` reads as zero rather than DDR
   residue. No behavioural change is predicted from this in the F0 posture — it removes a
   hazard that did not fire, and "nothing changed" is the expected outcome.
3. **A bad `ext_kickstart` now fails closed.** Naming a non-ROM file must produce
   `extended kickstart "...": not an extended ROM (missing $1111 id) -> not mapped` and
   leave MAPROMEXT **disabled** (`main.c:1729-1730`), where it previously printed
   `File read OK` and enabled the decode.
4. **The floppy hunk line changes shape or disappears.** With the open and seek checked, a
   missing `z3660_scsi.rom` says so by name; a present one still parses at `0x2600` and
   still reports `Hunk ID: 00000000`, because §5.1 says that offset holds zeros. Seeing the
   *unchanged* message on a card that has the file is the correct result, not a failure.
5. **Nothing in the emulated path moves.** No emulated-mode boot output should differ; the
   AMIX 030 line is untouched.

Not predicted, because not changed: anything about board 1, and anything about §6.
