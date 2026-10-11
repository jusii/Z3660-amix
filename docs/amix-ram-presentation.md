# How `amix_mode` presents RAM to AMIX — and what more than 16 MB would cost

**Investigation only. No code or config in this repo was changed to produce it.** Every claim below
cites a file and line in this tree at the time of writing (branch `amix-main-2`), a commit in this
repo's history, or a named external document. Where the evidence runs out, the section says so
explicitly instead of guessing — the open questions are collected in
[§5](#5-open-questions--what-only-metal-can-answer).

**One-line answer:** the 16 MB is a **hardcoded emulator memory-map decision**, not a config knob and
not a kernel limit — one `RANGE_MAP` line at
[`uae_emulator.cpp:1141`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)
maps 16 MB of Zynq DDR at guest `$08000000` and the next line dummies out everything above it. The
address map allows at most **128 MB** there, for two independent reasons, and the AMIX kernel's own
~128 MB describable ceiling means the firmware must stop well short of that.

| Question | Answer | Confidence |
|---|---|---|
| Where does 16 MB come from? | `RANGE_MAP(0x0800, 0x0900, drct_bank)` — a literal, compile-time | **high** (source) |
| Is there a config knob? | **No.** `amix_mode` is YES/NO only; there is no size key anywhere | **high** (source) |
| Largest contiguous window the map allows | **128 MB** (`$08000000`–`$0FFFFFFF`) | **high** (source) |
| Largest window that is *safe* for AMIX | **64 MB** (2× margin under the ~128 MB ceiling) | **medium** (arithmetic on measured inputs) |
| Does anything else in the firmware bound it? | Host DDR is free up to `0x18000000`; the binding limits are the RTG board at guest `$10000000` and the core-1 ARM page table | **high** (source) |
| Will the guest actually *see* a bigger window? | **Unknown** — depends on whether the Kickstart sizer probes or reads a size register. This is the one thing metal must answer first | **low — see §5.1** |

---

## 1. Where the cap lives

### 1.1 `amix_mode` — one boolean, resolved on the control core

`amix_mode` is a plain YES/NO config key
([`config_file.c:148`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c) in the key table,
parsed at [`config_file.c:854`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)). It
defaults to "unset" (`-1`, [`config_file.c:220`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c))
and is resolved once the whole config is parsed:

- no-preset path — [`config_file.c:1070-1072`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)
- preset path — [`config_file.c:1471-1473`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)

Both do the same three things: default to `amix_mode = (boot_mode == UAE_030_MMU)`, force
`config.cpu_ram = 0` when it is on, and publish the result to the cross-core mailbox
(`shared->amix_mode`, `SHARED` field at offset `0xFFFF00C0`,
[`Z3660/src/main.h:124`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.h)). The emulator core
reads it back and passes it as the fourth argument to `uae_emulator()`
([`Z3660_emu/src/main.cpp:967`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cpp) and
[`:998`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cpp)).

**There is no size anywhere in that chain.** The key is a boolean; the struct field is a boolean; the
function parameter is a boolean
([`uae_emulator.h:11`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.h)).

### 1.2 The guest memory map under `amix_mode`

`uae_emulator()` builds the whole guest map by writing 64 KB-granular entries into `mem_banks[]`
(`RANGE_MAP`, [`uae_emulator.cpp:993-994`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp);
`bankindex(addr) = addr >> 16`, [`memory.h:499`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/include/memory.h);
65536 banks = the full 32-bit space, [`memory.h:46`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/include/memory.h)).

The AMIX arm is [`uae_emulator.cpp:1124-1143`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp):

| Guest range | Bank | Backed by | Line |
|---|---|---|---|
| `$01000000`–`$06FFFFFF` | `slow_bank` | the real A4000 bus | `:1139` |
| `$07000000`–`$07FFFFFF` | `a3000mem_bank` | host DDR `0x09000000` | `:1140` |
| **`$08000000`–`$08FFFFFF`** | **`drct_bank`** | **host DDR `0x08000000`, 1:1** | **`:1141`** |
| `$09000000`–`$0FFFFFFF` | `dmmy_bank` | nothing | `:1142` |
| `$10000000`–`$17FFFFFF` | `dmmy_bank`, then RTG | host DDR `RTG_BASE` = `0x18000000` | `:1149`, `:1189` → `cpu_emulator.cpp:224,236` |

The non-AMIX arm is one line for the same region:
`RANGE_MAP(0x0800, 0x1000, drct_bank)` — the full 128 MB
([`uae_emulator.cpp:1147`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)).

`drct_bank` itself is not the cap. Its descriptor declares `allocated_size` / `reserved_size` =
`0x08000000` = **128 MB** and `baseaddr_direct_r/w` = `0x08000000`
([`uae_emulator.cpp:818-834`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)),
and its accessors are unchecked 1:1 host pointer dereferences —
`direct_read_32(add)` is `swap32(*(uint32_t*)add)`
([`uae_emulator.cpp:121-144`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)),
`drct_check()` returns 1 unconditionally
([`uae_emulator.cpp:730-733`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)).
**Guest `$08xxxxxx` is host `0x08xxxxxx`, byte for byte, with no bound check.** The only thing that
limits the guest to 16 MB is how many `mem_banks[]` entries point at `drct_bank`.

### 1.3 How the 16 MB figure is produced

Three literals, all in
[`uae_emulator.cpp`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp):

1. `#define AMIX_A3000MEM_MB 16` — line `1123`, used only for the vestigial `$07000000` window and
   its startup `memset` (line `1192`). Line `1126` is a leftover no-op expression.
2. `RANGE_MAP(0x0800, 0x0900, drct_bank)` — line `1141`. `0x0900 - 0x0800 = 0x100` banks
   × 64 KB = **16 MB**. This is the number.
3. `RANGE_MAP(0x0900, 0x1000, dmmy_bank)` — line `1142`. This is what makes the window *end*.

`dmmy_bank` answers every read with `0xFFFFFFFF` and discards every write
([`uae_emulator.cpp:699-705`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp),
[`:979-985`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)) — i.e. it fails a
write/read-back RAM probe. `a3000mem_bank`'s own descriptor also carries a 16 MB
`allocated_size`/`reserved_size`/`startaccessmask`
([`uae_emulator.cpp:932-936`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)),
but that window is not in the guest's memory list (§1.4).

### 1.4 What the firmware does *not* do

**Under `amix_mode` the firmware never publishes a memory-list entry.** It has two mechanisms that
could, and both are off:

- The FPGA CPU-RAM board — `config.cpu_ram` drives `ENABLE_CPU_RAM_FPGA`
  ([`main.c:1417-1420`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.c)), and `amix_mode` forces
  `cpu_ram = 0` ([`config_file.c:1071`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c),
  [`:1472`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)). In emulator modes the
  hardware autoconfig is disabled wholesale anyway
  ([`main.c:1471-1473`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.c)).
- The emulated Zorro III autoconfig chain
  ([`cpu_emulator.cpp:312-373`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp),
  mapped at guest `$FF000000` by
  [`uae_emulator.cpp:1153`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)) can
  offer a 256 MB FastRAM board — but only when `config.autoconfig_ram` is set
  ([`main.c:1686`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.c) →
  `local.z3_enabled & 1` → [`cpu_emulator.cpp:334`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp)).
  See the hazard in [§2.9](#29-hazard-amix_mode-does-not-force-autoconfig_ram-off).

So the guest learns about `$08000000` by **discovering it**, not by being told. The firmware's whole
contribution is "this range behaves exactly like RAM, and everything above it behaves like nothing."
Whichever agent does the sizing — the A3000-variant Kickstart's motherboard-RAM sizer, or the Z3660
extended-Kickstart module (`kicks/kick060_scsi.rom`, referenced at
[`z3660cfg.txt:114`](../z3660-zturn_SD_content/DATA%20Second%20Partition%20%28exFat%29/z3660cfg.txt)) —
publishes what it finds, AmigaOS puts it in the memory list, the AMIX loader `AllocMem(MEMF_FAST)`s
the kernel into it, and the kernel brings up its 2 KB-page MMU **in place** and runs there. That
chain is recorded in commit `22ce6f2` and in the comment block at
[`uae_emulator.cpp:1127-1138`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp).

Two measured facts from that commit anchor it:

- **The `$07000000` window is never probed.** A temporary `[A3KMEM]` diagnostic counted **0** hits,
  even on the function path — so `a3000mem` never enters the memory list and AMIX never uses it
  (commit `22ce6f2`; restated at
  [`uae_emulator.cpp:1129-1131`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)).
  It is dead weight that happens to occupy host DDR `0x09000000` — the first megabytes we would want
  back (§2.5).
- **AMIX reports the window and only the window.** `Total Unix memory = 16775168` = `0x00FFF800`
  = 16 MB − one 2 KB page (commit `22ce6f2`). Chip RAM is not counted into the main store.

### 1.5 The in-tree rationale for "one window, ≤ 16 MB" — and what survives scrutiny

The comment at
[`uae_emulator.cpp:1112-1122`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)
is this fork's recorded reading of the AMIX kernel (`sys/immu.h`, `amiga/kernel/support.c`). It makes
two distinct claims, and they have aged differently:

| Claim | Status |
|---|---|
| "EVERY RAM region AmigaOS lists must lie INSIDE that one window, else the kernel … panics `vatosde()`" — i.e. **exactly one contiguous region** | **Stands.** Independently corroborated: a community report (2026-08-12) that Amix picks the **largest contiguous block**; and `/stand/README` + `makeiblk`, where the kernel info block's default preferred load address `0xFFFFFFFF` means "auto-bind to the largest chunk of contiguous, non-CHIP memory" (`amix-kerntools/docs/piscsi-rig.md` §6). |
| "`VSIZOFMEM = 16MB` … two adjacent banks COALESCE into a >16MB window (**also fatal**)" | **Contradicted for the size half.** The comment itself attributes the 16 MB to a `QUICK_KLUDGE` path for a **high-loaded** kernel — and *this* kernel does not relocate, it runs in place at `$08000000` ([`:1132-1135`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)). Against it: the emulated piscsi rig boots to multiuser on **32 MB at `0x08000000`** (`amix-kerntools/docs/piscsi-rig.md` §1, §6 — `Total Unix memory = 33552384`), and the community report has an A3000 running 48 MB with 32 MB stress-tested for months. |

The kernel limit that *is* real is a different one: **~128 MB of total describable RAM**, past which
`rmalloc()` returns `NULL` unchecked and `page_init()` bit-sets a 60-byte stride from physical
address `0` — no panic, no console output at all
(`grimoire-amix/docs/how-it-works/ram-ceiling.md`; mechanism, disassembly and six-run ablation in
`amix-kerntools/docs/kernel-52550-hang.md`).

> The practical reading: **`amix_mode`'s 16 MB is our own compile-time choice, made under a
> conclusion that the size half of has since been contradicted.** The contiguity half of that
> conclusion is what must be preserved by any change.

---

## 2. What presenting 32 / 64 / 128 MB entails

### 2.1 Route A — widen the `$08000000` window (the direct route)

The functional change is **one literal**:

```
RANGE_MAP(0x0800, 0x0900, drct_bank);   // 16 MB   uae_emulator.cpp:1141
RANGE_MAP(0x0900, 0x1000, dmmy_bank);   //         uae_emulator.cpp:1142
```

| Target | `drct_bank` end | `dmmy_bank` start |
|---|---|---|
| 32 MB | `0x0A00` | `0x0A00` |
| 64 MB | `0x0C00` | `0x0C00` |
| 96 MB | `0x0E00` | `0x0E00` |
| 128 MB | `0x1000` | *(none — `:1149` already dummies `0x1000`–`0x1800`)* |

It is code, not config. Making it a config key would mean a new `CONFITEM` in both `config_file.h`
enums, the parser, the preset writer/reader, the web/`ZTop` UI mirrors
([`rtg.c:2067`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/rtg/rtg.c),
[`win.c:33`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/ARM_ztop/win.c)) and a new `SHARED` field —
and the `CONFITEM` enum is a known resync hazard (`docs/port-ledger-beta21.md` §4f). **For a first
metal experiment, a compile-time constant is the right shape**; a config key is a later decision.

### 2.2 Host DDR budget — there is room

Zynq DDR is 1 GB (`0x00100000`–`0x3FFFFFFF`, `XPAR_PS7_DDR_0_S_AXI_*` in the BSP `xparameters.h`).
Committed regions:

| Host DDR | Owner | Evidence |
|---|---|---|
| `0x01000000`–`0x07FFFFFF` | **core-0 firmware link region** (112 MB) | [`Z3660/src/lscript.ld:24`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/lscript.ld) |
| `0x07CE0000`–`0x07EFFFFF` | audio + GEM BD lists + eth frame buffers (carved from the above) | [`memorymap.h:28-34`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/memorymap.h) |
| **`0x08000000`–`0x0FFFFFFF`** | **guest CPU RAM, 1:1** — free apart from the item below | [`uae_emulator.cpp:829-833`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp) |
| `0x09000000`–`0x09FFFFFF` | **`a3000mem` backing store** — the one obstacle | [`uae_emulator.cpp:903`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp) |
| `0x10000000`–`0x17FFFFFF` | unused, but **`RESERVED` in the core-1 page table** | [`Z3660_emu/src/main.cpp:887-888`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cpp) |
| `0x18000000`–`0x1FFFFFFF` | RTG framebuffer / soft3D / audio / eth / USB | [`memorymap.h:16,35-37`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/memorymap.h) |
| `0x20000000`–`0x2FFFFFFF` | Zorro III 256 MB FastRAM board backing | [`cpu_emulator.cpp:105,538`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp) |
| `0x30000000`–`0x3FEFFFFF` | **core-1 emulator link region** | [`Z3660_emu/src/lscript.ld:24`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/lscript.ld) |

So DDR is not the constraint: `0x08000000`–`0x17FFFFFF` (256 MB) is physically free.

### 2.3 Two independent 128 MB ceilings — and they agree

1. **Guest address map.** With the shipped `autoconfig_rtg NO`
   ([`z3660cfg.txt:113,117`](../z3660-zturn_SD_content/DATA%20Second%20Partition%20%28exFat%29/z3660cfg.txt)),
   `reset_autoconfig()` hard-maps the RTG board at guest **`$10000000`**, 128 MB wide
   ([`cpu_emulator.cpp:221-236`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp);
   `init_rtg_bank` covers `dir+0x0000`..`dir+0x0800` =
   [`uae_emulator.cpp:1009-1014`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)).
   That is where the `z3660eth` driver's board window lives, so it cannot move without a driver
   change. A window starting at `$08000000` therefore cannot exceed **128 MB**.
2. **Core-1 ARM page table.** `drct_bank` is 1:1, so guest `$1xxxxxxx` would be host `0x1xxxxxxx` —
   and core 1 marks host `0x10000000`–`0x17FFFFFF` `RESERVED`
   ([`Z3660_emu/src/main.cpp:887-888`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cpp)),
   which is the Xilinx BSP's **fault** descriptor (`RESERVED 0x0`, BSP `xil_mmu.h:57`). Any 68k
   access there would take a data abort on the emulator core.

Everything from `$08000000` to `$0FFFFFFF` is already `RAM_CACHE_POLICY` on **both** cores —
core 1 [`main.cpp:885-886`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cpp), core 0
[`Z3660/src/main.c:1073-1074`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.c) (core 0 covers
`0x080`–`0x180`). **No page-table change is needed for any window ≤ 128 MB.**

### 2.4 What does *not* need to change

- **Core-0 and core-1 page tables** — already cover the whole 128 MB (§2.3).
- **The piscsi direct-DMA gate.** Both handlers already accept the full `[0x08000000, 0x10000000)`
  range: READ [`scsi.c:1096`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c), WRITE
  [`scsi.c:1218`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c).
- **The `amix-z3660scsi` driver.** `BOUNCE_THRESH 0x08000000` with a `<` test
  (`amix-z3660scsi/src/z3660.c:71,302`) — every buffer at or above `$08000000` already takes the
  direct arm, with no upper bound in the driver.
- **The `amix-z3660net` driver.** Its rings are board-window offsets
  (`ZZ_RXBACKLOG_OFF 0x07ED0000`, `ZZ_TXFRAME_OFF 0x07EE0000`, `amix-z3660net/src/z3660eth.h:39-40`)
  resolved against the RTG/board base at guest `$10000000`, backed by host `RTG_BASE + …`
  ([`ethernet.c:98-99`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/ethernet.c)). It never DMAs into
  guest RAM, so it is indifferent to the window size — but it is exactly why the board base at
  `$10000000` must not move (§2.3).
- **The emulated A3000 SCSI bootstrap.** It stages through `SCSI_NO_DMA_ADDRESS` and scatters into
  the guest with `put_byte`/`get_byte` through the bank dispatch
  ([`a3000_scsi.cpp:325-334`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/a3000_scsi.cpp),
  [`:346-394`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/a3000_scsi.cpp)) — address-size
  agnostic by construction.
- **The 030 PMMU emulation.** The guest's own page tables live in guest RAM and are walked through
  the same bank dispatch; nothing in `cpummu030.cpp` is keyed to the window size. The one AMIX-keyed
  hook is a page-size test, not an address test
  ([`cpummu030.cpp:887`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/cpummu030.cpp)), and
  `AMIX_HIDE08` is already disabled
  ([`mmu_common.h:133`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/include/mmu_common.h)).

### 2.5 What *does* need to change — the checklist

1. **The two `RANGE_MAP` lines** (§2.1).
2. **`a3000mem` must go, or move.** Its host backing is `A3000MEM_HOST 0x09000000`
   ([`uae_emulator.cpp:903`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)) —
   the very next 16 MB the widened window would claim. It is already documented as **vestigial** and
   never in the memory list (§1.4), so removing its `RANGE_MAP` at
   [`:1140`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp) is the clean move.
   Three loose ends come with it:
   - the startup `memset((void*)A3000MEM_HOST, 0, 16 MB)`
     ([`:1192`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)) would zero
     guest `$09000000`–`$09FFFFFF`. Harmless at reset, but it must not survive as a stray wipe.
   - **`get_real_address()` aliasing.** [`memory.h:801-810`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/include/memory.h)
     redirects *any* guest `$07xxxxxx` to host `$09xxxxxx` whenever `a3000_amix_mode` is set. Drop
     the bank but keep this hook and a direct-path access to guest `$07xxxxxx` (`slow_bank`, the real
     A4000 bus) would silently alias into the **new guest RAM** at `$09000000`. **This is the single
     most dangerous line in the change.**
   - `AMIX_A3000MEM_MB` and the dead expression at
     [`:1123,1126`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp) become
     meaningless.
3. **Leave the `$0700`–`$0800` guest range on `slow_bank`** (the real bus), matching the
   `$0100`–`$0700` treatment at [`:1139`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp) —
   *and confirm the sizer does not then coalesce real motherboard SIMM RAM at `$07000000` with our
   window*. Any adjacency there recreates exactly the two-region/coalesce failure §1.5 says must be
   avoided.
4. **Re-check `autoconfig_ram`** (§2.9).
5. **`docs/AMIX.md`** — the `amix_mode` description says "a single ≤16 MB window"
   ([`AMIX.md:125-126`](AMIX.md)) and would need re-wording. The `z3660cfg.txt` comment block
   ([`z3660cfg.txt:16-17`](../z3660-zturn_SD_content/DATA%20Second%20Partition%20%28exFat%29/z3660cfg.txt))
   says the same.

### 2.6 Route B — the Zorro III autoconfig board (the alternative worth knowing about)

If §5.1 turns out badly — i.e. the Kickstart sizer refuses to publish more than 16 MB at `$08000000`
no matter what the emulator maps — the firmware **already has a second, fully plumbed RAM
presentation** that does not depend on the motherboard sizer at all:

- The emulated Z3 autoconfig offers a FastRAM board with the **pool-link bit set**, so AmigaOS adds
  it to the memory list ([`cpu_emulator.cpp:320`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp),
  size nibble at [`:334`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp)).
- On assignment it programs both cores' page tables and maps the bank
  ([`cpu_emulator.cpp:524-546`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp);
  `init_z3_ram_bank` at [`uae_emulator.cpp:1004-1008`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp),
  backed by host `0x20000000`).
- **The piscsi direct-DMA path already handles it**: `scsi.c` accepts `[0x40000000, 0x50000000)` and
  rebases to host `0x20000000`
  ([`scsi.c:1097,1100`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c) and
  [`:1219,1222`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c)). The driver's
  `BOUNCE_THRESH` test passes too — `$40000000 > 0x08000000`.

For AMIX this would only work as the **sole** region: the `$08000000` window would have to be
dummied out entirely so the memory list holds exactly one entry. Costs: the board's size is a
hardcoded autoconfig nibble and a hardcoded 256 MB `RANGE_MAP`, both of which would need a size
smaller than the ceiling; and the AMIX kernel would load and run from `$40000000+` instead of
`$08000000`, which is a bigger behavioural change than Route A. **Route A first; Route B is the
fallback, not the plan.**

### 2.7 Interplay with the §4g DMA cache doctrine

The direct-DMA path is only correct because of explicit whole-hierarchy cache maintenance:
`Xil_DCacheInvalidateRange` before a READ
([`scsi.c:1116`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c), rationale at
[`:1103-1115`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c)) and `Xil_DCacheFlushRange`
before a WRITE ([`scsi.c:1232`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c), rationale
at [`:1225-1231`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c)) — the L1-only forms stop
at the PL310 L2, which the SD controller's DMA sits below (`CHANGELOG.md:104-109`,
`docs/port-ledger-beta21.md` §4g). Plus the AMP half: core 1 is parked during the transfer but still
speculatively refills its own L1D, so its L1 must be invalidated after the DMA
(`CHANGELOG.md:146-151`).

**Widening the window does not change the doctrine — it widens its blast radius.** Today only
16 MB of guest addresses can be a direct-DMA target; at 64 MB, four times as much guest RAM is
eligible, so four times as much of the guest's working set is exposed to any residual coherency gap.
And the decisive point:

> amiberry models **no cache hierarchy at all**, so the bench cannot test this
> (`CHANGELOG.md:107-109`, `docs/port-ledger-beta21.md` §4g). **The first flash is the test.**

### 2.8 The `SHARED` mailbox is *not* an address-map bound

Worth stating because it is easy to assume otherwise. `SHARED` lives at **`0xFFFF0000`**
([`Z3660_emu/src/main.cpp:53`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/main.cpp)), which is
Zynq **OCM**, not DDR — core 1's second link region is `0xFFFF1000`
([`Z3660_emu/src/lscript.ld:26`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/lscript.ld)) and
core 0's is `0xFFFF0000` ([`Z3660/src/lscript.ld:27`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/lscript.ld)).
It bounds nothing in the guest RAM window. The `0xFFFF009C` hazard on record
(`docs/port-ledger-beta21.md` §5) is an **upstream-merge ABI hazard** — beta 21 and this fork both
appended fields at the same offsets — not a memory-map constraint. It matters here only in that a
size knob implemented as a new `SHARED` field would have to be appended after `perf_report`
(`0xFFFF00C8`, [`Z3660/src/main.h:126`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.h)) in
**both** copies of the struct.

### 2.9 Hazard: `amix_mode` does **not** force `autoconfig_ram` off

`amix_mode` forces `cpu_ram = 0` and nothing else
([`config_file.c:1071`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c),
[`:1472`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)). Meanwhile
`shared->z3_enabled |= config.autoconfig_ram ? 1 : 0`
([`main.c:1686`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/main.c)) enables the emulated Z3
FastRAM board, whose `er_Type` sets the pool-link bit
([`cpu_emulator.cpp:320`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp)) — i.e.
AmigaOS would add a **256 MB second region** to the memory list. That is both a second
(non-contiguous) region *and* far past the ~128 MB ceiling.

The shipped defaults have `autoconfig_ram YES`
([`z3660cfg.txt:110`](../z3660-zturn_SD_content/DATA%20Second%20Partition%20%28exFat%29/z3660cfg.txt)) and
so do all four shipped presets, and `docs/AMIX.md`'s minimal AMIX config
([`AMIX.md:91-103`](AMIX.md)) does not mention the key at all.

**This is a source-level observation, not a reproduced failure.** Our metal box boots AMIX today, so
either its preset sets `autoconfig_ram NO` or something else prevents the board from reaching the
memory list. **Reading the metal card's active preset is a prerequisite for the experiment** — and
whichever way it reads, it is a finding: either an undocumented requirement that belongs in
`docs/AMIX.md`, or a gap in this analysis that must be closed before any RAM size is touched.

---

## 3. Risk register and the staged metal experiment

### 3.1 What a too-large presentation breaks

| Size | Frames (2 KB pages) | `page[]` at 60 B/frame | Margin vs the measured ~4 MB free kernel-virtual entry | Verdict |
|---|---|---|---|---|
| 16 MB (today) | 8,192 | 0.47 MB | ~8× | proven on metal |
| **32 MB** | 16,384 | 0.94 MB | ~4× | **proven to multiuser on the bench rig** |
| **64 MB** | 32,768 | 1.88 MB | ~2× | **recommended production cap** |
| 96 MB | 49,152 | 2.81 MB | ~1.4× | experimental only, serial watched |
| 128 MB | 65,536 | 3.75 MB | ~1.07× | **do not present** — at the computed bound |
| >128 MB | — | — | none | silent death, no console output |

Inputs are measured, the arithmetic is ours: the kernel-virtual free list held one entry of
2048 pages × 2048 B = 4 MB at the time of failure, break-even ≈ `2048 × 2048 ÷ 60` ≈ 69,900 frames
≈ 136 MB, and **the same allocation must also cover the page-table array**, which is why the
practical bound lands near 128 MB (`grimoire-amix/docs/how-it-works/ram-ceiling.md`).

Two cautions that must not be lost:

- **The ceiling has never been bisected.** Nothing between 32 MB (works) and 512 MB (fatal) has been
  booted. 128 MB is *computed*, not tested.
- **The free-map size is a function of how much kernel virtual space the running kernel has already
  spent — a bigger kernel moves the line downward.** Our kernels carry `z3660scsi` + `z3660eth` +
  `cdfs`. A margin that holds for one kernel is not automatically valid for the next.

**Safe margin for the firmware: never present more than 64 MB in a shipping configuration.** That is
a 2× margin on a bound whose exact value we do not know and which moves with the kernel.

### 3.2 Failure modes and how each is recognised

| Failure | Signature | Where it bites |
|---|---|---|
| Total RAM past the ceiling | **Zero guest output** — no SVR4 banner, no memory line, black screen. Bus-timeout storm at low Zorro II addresses, constant stride `0x3C` | AMIX kernel |
| Two regions in the memory list | `vatosde()` panic — "address not in SCN1" | AMIX kernel |
| Guest access above `$0FFFFFFF` on `drct_bank` | ARM **data abort** on core 1 (`RESERVED` = fault descriptor) | firmware, immediate |
| `get_real_address` aliasing left behind after dropping `a3000mem` | silent wild reads/writes into guest RAM at `$09000000` | firmware, silent — worst case |
| Direct-DMA coherency gap over the larger window | stale bytes on disk, or an L2 writeback landing on freshly DMA-ed bytes | **metal only** — bench cannot model it |

### 3.3 Recoverable vs. not

**Recoverable — firmware is disposable.** The board's boot chain is `BOOT.bin` → `Z3660.bin` →
`FAILSAFE.bin`, and the serial boot menu explicitly states "*If `Z3660.bin` is invalid or missing, it
will attempt to boot from `FAILSAFE.bin`*"
([`mobotest.c:1207`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/mobotest.c)), with `'J'` = copy
`Z3660.bin` → `FAILSAFE.bin` and `'L'` = copy back
([`mobotest.c:1199-1201`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/mobotest.c)). **Confirm a
known-good `FAILSAFE.bin` is on the card before the first flash** — the bad-RAM signature is *total
silence*, which is indistinguishable from a bad flash, so the recovery path must be proven in advance
rather than discovered under a dead board.

**Not recoverable by firmware means — the disk.** A widened window widens the direct-DMA surface
(§2.7). The realistic corruption paths are:

- **The AMIX disk image** (`hdf/Amix.hdf` on the SD): a coherency gap on the WRITE path puts stale
  DRAM on the disk. **Run against a working copy, never a golden.** The on-device COPY path makes
  this cheap — the boot menu's `'E'` SD file manager copied a 943 MB image in 101 s with verify
  green ([`CHANGELOG.md:180-191`](../CHANGELOG.md); usage in
  [`docs/http-vs-tftp-upload.md:87-94`](http-vs-tftp-upload.md)).
- **The SD card generally**: piscsi writes go through core-0 FatFS to the same card that holds the
  firmware and every image. A wild guest write into a DMA descriptor region is the mechanism to fear;
  the `$09000000` aliasing landmine (§2.5) is the specific instance this change introduces.
- **Not at risk:** the AmigaOS goldens and anything not attached as a `scsiN` target in the active
  config — the firmware only writes files it has opened.

### 3.4 The staged experiment

**Prerequisites (before touching the board):**

- **P0.** Read the metal card's active preset and resolve `autoconfig_ram` (§2.9). If it is `YES`,
  stop and understand why AMIX boots today before changing anything else.
- **P1.** Confirm a known-good `FAILSAFE.bin` is present (boot menu `'J'` while the good firmware is
  live).
- **P2.** Work on a **copy** of `Amix.hdf`, made with the on-device SD `COPY`. Record its checksum.
- **P3.** Capture a 16 MB baseline: `Total Unix memory` line, `tools/amix-bench` results, a clean
  cold boot to `login:`.

**Stage 0 — the decisive cheap probe (32 MB).** Build with `drct_bank` ending at `0x0A00`, drop
`a3000mem` and its `get_real_address` hook, flash, boot. **Read one number**: AMIX's
`Total Unix memory`.

- `33552384` (32 MB − one page) → the sizer probes; Route A works; proceed.
- still `16775168` → **the sizer, not the map, is the gate.** Stop Route A. The size is being
  reported by something that does not scale with the mapped extent (§5.1), and the answer is Route B
  or a memory-list injection. **This single reading is the highest-value output of the whole
  experiment** and it costs one flash.

**Between every stage, the same battery.** Each item exists because it has caught something before:

| Check | Why | Tool |
|---|---|---|
| `Emulation mode 4` + `68030 MMU enabled` on serial | firmware actually took the AMIX path | serial console (`docs/AMIX.md` §Verifying) |
| `Total Unix memory` matches the intended window exactly | the guest saw what we mapped, and nothing extra | serial / console |
| **3 × cold boot to `login:`** | the ceiling failure is silent; a cold boot is the only honest detector | — |
| Disk write + read-back + double-read canaries; a written file survives a power cycle | the §4g direct-DMA coherency surface — **metal-only** | shell |
| `tools/amix-bench/runbench.sh` | RAM size should not cost throughput; a regression means a cache-policy mistake | [`tools/amix-bench`](../tools/amix-bench) |
| `tools/amix-stress` soak (`gsoak.sh` + `telnet_storm.py`), watching the wild-PC guard | demand-paging load over a larger page pool; `amix_soak.sh` hard-fails on a latch | [`tools/amix-stress`](../tools/amix-stress) |
| Tree digest of the installed base set, unchanged | the exact end-to-end oracle the install work already uses | host-side |

**Stage 1 — 32 MB.** Full battery. This is the size with independent positive evidence
(`amix-kerntools/docs/piscsi-rig.md` §1) — if it fails on metal but works on the bench, the fault is
firmware-side, not kernel-side, and that is a strong signal.

**Stage 2 — 64 MB.** Full battery, plus a longer soak. **This is the intended stopping point for any
shipping configuration.**

**Stage 3 — 96 MB, experimental only, never shipped.** Full battery. A failure here is an expected
outcome, not a setback: the value of the stage is a *data point in the never-bisected 32 MB–128 MB
gap*, which is a genuine contribution back to the family's knowledge whichever way it lands. Watch
the serial for the total-silence signature and be ready to fall back to `FAILSAFE.bin`.

**Do not run 128 MB.** It sits at the computed bound with ~1.07× margin, on a bound that moves with
kernel size.

**What only metal can prove** (the §4g lesson, stated plainly):

- amiberry has **no cache hierarchy**, so every §4g/AMP coherency question over the widened
  direct-DMA window is untestable on the bench. Only metal exercises PL310 L2 writeback and core-1
  speculative L1 refill.
- The bench also cannot answer §5.1 at all: its RAM comes from a `.uae` `debugmem_size` key, not from
  a Kickstart sizing a mapped window through Ramsey on a real A4000.
- What the **bench can** cheaply pre-answer: whether the AMIX kernel accepts a single 32/64 MB
  contiguous region and survives `page_init()`. It already answers that for 32 MB.

---

## 4. Upstream check — does stock beta 21 inform this?

Compared against `upstream/main` (shanshe v1.03 beta 21) in this clone. **Upstream has no
size-parameterised RAM presentation anywhere**, so there is no upstream mechanism to adopt — but the
comparison is still informative:

- **Identical emulator window.** Upstream maps `RANGE_MAP(0x0800, 0x1000, drct_bank)` — the full
  128 MB — for **every** emulator mode, with the same `allocated_size 0x08000000` descriptor
  (`git show upstream/main:…/uae_emulator.cpp`, lines `1034` and `825`). Our non-AMIX arm
  ([`uae_emulator.cpp:1147`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp))
  is unchanged from it. **The AMIX arm is the only divergence** — which is exactly the fork-boundary
  property we want, and it means widening it stays a self-contained fork change.
- **Identical page tables.** Upstream's core-1 setup is byte-for-byte the same policy: `0x080`–`0x100`
  `RAM_CACHE_POLICY`, `0x100`–`0x180` `RESERVED` (`git show upstream/main:…/Z3660_emu/src/main.cpp`,
  lines `833-836`). So the **128 MB ceiling is upstream's, not ours** — raising it would mean
  diverging from upstream in a second place, and is a strong argument for staying at or under 64 MB.
- **Only two RAM knobs upstream, both boolean.** `cpu_ram` (128 MB CPU RAM at `$08000000`, FPGA-side
  for the real 060) and `autoconfig_ram` (256 MB Zorro III board) — `git show
  upstream/main:…/config_file.c` lines `101`, `103`. No megabyte value is configurable anywhere in
  upstream.
- **The one upstream idea worth borrowing** is the Zorro III board's autoconfig size nibble
  ([`cpu_emulator.cpp:334`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/cpu_emulator.cpp)) — the
  only place in the firmware where a RAM size is expressed as data rather than as a `RANGE_MAP`
  literal. That is Route B (§2.6).

---

## 5. Open questions — what only metal can answer

### 5.1 Does the guest's sizer scale with the mapped extent? *(the blocking unknown)*

The firmware never publishes a memory-list entry (§1.4) — something in the guest discovers the
window. **Which agent, and by what method, is not determinable from this tree**, and the two
possibilities have opposite consequences:

- **If it probes** (write/read-back walk upward until `dmmy_bank` answers `0xFFFFFFFF`), then a wider
  `RANGE_MAP` is discovered automatically and Route A is a one-literal change. The bench's
  "9 logged `Illegal lget/lput` lines per boot are the **Kickstart's** RAM sizer"
  (`amix-kerntools/docs/piscsi-rig.md`) is consistent with a probing sizer.
- **If it reads a size register**, the number comes from the real A4000's Ramsey — and note that
  `$00DE0000`–`$00DFFFFF` is `mobo_bank`, forwarded to the **real motherboard**
  ([`uae_emulator.cpp:1083`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)).
  A real A4000's motherboard Fast RAM also lives at `$08000000` and tops out at 16 MB, so a
  register-derived answer would be **16 MB no matter what we map** — and, worse, could be 16 MB of
  *coincidence* rather than of our window.

Stage 0 of the experiment (§3.4) discriminates these two in a single flash and a single serial line.
Everything else in Route A is contingent on that reading.

### 5.2 Is `$07000000` real motherboard RAM on our metal box?

Commit `22ce6f2` records the box's motherboard SIMMs as the *slow* RAM AMIX used to run on, and the
map comments assert "no RAM lives below `$07000000` on the A4000"
([`uae_emulator.cpp:1128`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/uae_emulator.cpp)) —
which is a statement about `$01000000`–`$06FFFFFF`, not about `$07000000`. If real SIMM RAM is
visible adjacent-below a widened window, the coalesce risk of §2.5 item 3 is live. Resolvable by
reading the AmigaOS memory list on the box before changing anything.

### 5.3 What is `autoconfig_ram` actually set to on the metal card? (§2.9)

---

## Sources

- This tree, branch `amix-main-2` — every `file:line` citation above.
- Commit `22ce6f2` "AMIX: run on Zynq-local DDR (`$08000000` CPU-RAM); disable obsolete
  `AMIX_HIDE08`" — the `[A3KMEM]` zero-probe measurement, `Total Unix memory = 16775168`, and the
  in-place-at-`$08000000` kernel behaviour.
- Commit `886d8c2` "emu: the emulated A3000 WD33C93 + SuperDMAC, and the AMIX memory contract" — the
  contract as originally written down.
- [`docs/AMIX.md`](AMIX.md), [`docs/port-ledger-beta21.md`](port-ledger-beta21.md) §4g and §5,
  [`CHANGELOG.md`](../CHANGELOG.md) 2026-08-01 and 2026-07-25 entries.
- `upstream/main` (shanshe/Z3660 v1.03 beta 21), read via `git show` — §4.
- `grimoire-amix/docs/how-it-works/ram-ceiling.md` — the ~128 MB describable-RAM ceiling, the
  `page_init()`/`rmalloc()` mechanism, the "32 MB is the only oversized value with positive boot
  evidence" rule, and the never-bisected 32 MB–512 MB gap.
- `amix-kerntools/docs/piscsi-rig.md` §1 and §6 — 32 MB at `0x08000000` booting to multiuser
  (`Total Unix memory = 33552384`), the "no tunable exists" survey, and the Kickstart-sizer log lines.
  Its `kernel-52550-hang.md` holds the disassembly and the six-run ablation.
- `amix-z3660scsi/src/z3660.c:71,302` (`BOUNCE_THRESH`) and `amix-z3660net/src/z3660eth.h:36-43`
  (board-window offsets) — the two consumer-side contracts checked in §2.4.
- A community report, 2026-08-12 (relayed): no kernel 16 MB fast-RAM limit; an A3000
  running 48 MB, 32 MB stress-tested for months; Amix picks the largest **contiguous** block.
  **Second-hand and unverified here** — used only where it agrees with in-tree or bench evidence.
