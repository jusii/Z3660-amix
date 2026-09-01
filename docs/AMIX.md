# Z3660 — Amix (Amiga UNIX) support

This is the Amix-oriented fork of [shanshe/Z3660](https://github.com/shanshe/Z3660). On top of
the upstream firmware it adds a **real 68030 PMMU emulator** and an **emulated A3000 mainboard
SCSI controller**, which together let a Z3660 + Z-turn boot **Amiga UNIX (AMIX) 2.1** — no
physical 68030 or A3000 SCSI hardware required.

Everything here is **optional**. If you don't run AMIX, none of it changes how your Z3660 boots
AmigaOS — the new options default to off and the new boot mode is just one more entry in the
existing list. Upstream Z3660 behaviour is unchanged.

> **Status in one line:** AMIX 2.1 **boots reliably to a stable multiuser login shell** under this
> firmware — SVR4 banner, multi-user, root login on the HDMI console and over telnet. It survives
> sustained fork/exec load with the firmware's wild-PC guard never latching; the **reboot count**
> that used to be quoted here ("a 27-reboot soak") is **indeterminate** and is being re-established
> with a fixed tool. One rare demand-paging edge case under extreme load is tracked but does not
> block normal use. See [Current status](#current-status) below.

> **Upstream base:** this fork sits on upstream firmware **v1.03 beta 21**. Upstream rewrote its
> history, so there is no merge base and the resync was a guided replay rather than a merge. The
> commit-by-commit plan — what upstream had already absorbed, what was hand-ported, and what was
> dead — is in [`docs/port-ledger-beta21.md`](port-ledger-beta21.md), and it is worth reading
> before the next resync. Everything in this document reflects the post-resync tree.

---

## What this fork adds

| Addition | Summary |
|---|---|
| **68030 PMMU emulation** | New `bootmode UAE_030_MMU` — a cycle-accurate 68030 interpreter **with a working PMMU** (demand paging, `mmu.library` sees it). AMIX needs a real MMU; the stock 68040-JIT modes don't provide the 030 PMMU semantics it expects. |
| **Emulated A3000 SCSI** | An emulated Commodore A3000 mainboard SCSI (SuperDMAC + WD33C93). This is AMIX's **kernel bootstrap** path — AMIX boots its kernel through the A3000 SCSI driver, then hands off to the native Z3660 drivers. |
| **Native AMIX drivers** | [`amix-z3660scsi` / `amix-z3660net`](https://github.com/jusii/amix-kerntools) run inside AMIX against the Z3660 PISCSI + ethernet register protocol this repo owns — they live in the companion **amix-kerntools** repo. |
| **AMIX warm-reboot fix** | A clean Zynq reboot on an AMIX `reboot`, instead of the register-flood / data-abort the naïve path produced. |
| **Emulator perf + debug knobs** | `service_cadence` (CPU throughput vs. interrupt latency) and per-category debug gating (`DEMU` / `DSCSI`) to silence the emulator's debug floods. |
| **Run-loop overhead switches** | `perf_stats` / `emu_probes` / `core0_stats` turn off the firmware's own per-instruction and per-loop-iteration instrumentation at runtime, so its cost can be measured rather than assumed. All default to on (unchanged behaviour). |
| **Interpreter profiler** | A third firmware variant (`make profile` -> `BOOT-prof.BIN`) with a wall-clock guest-PC sampler and per-stage ARM cycle attribution, so the interpreter's remaining cost is measured rather than argued about. Off unless asked for; see [docs/profiler.md](profiler.md). |
| **Sizable AMIX RAM window** | `amix_ram <MiB>` sizes the single contiguous window the AMIX memory contract presents (default 16, bounded at 128 by the combo board window). |

---

## Boot modes

`bootmode` (in `z3660cfg.txt` or a preset) selects how the CPU is provided. All eight values:

| `bootmode` | What it is |
|---|---|
| `MOBOCPU` | Motherboard 68060 (Z3660 out of the path) |
| `CPU` | The Z3660's own real 68060 |
| `MUSASHI` | Musashi 68k interpreter |
| `UAE_030` | UAE 68030 interpreter (no JIT, **no MMU**) |
| `UAEJIT_030` | UAE 68030 JIT (no MMU) |
| `UAE_040` | UAE 68040 interpreter |
| `UAEJIT_040` | UAE 68040 JIT — the fast everyday AmigaOS emulator (~120 MIPS) |
| **`UAE_030_MMU`** | **UAE 68030 interpreter with a real PMMU — the AMIX mode.** "Emulation mode 4". |

`UAE_030_MMU` is the only mode that provides the 68030 PMMU; it is interpreted (no JIT), so it is
slower than `UAEJIT_040` — that is the price of the MMU semantics AMIX requires.

The 030 MMU/CPU core is imported from WinUAE 4.4.0 (= Amiberry v5.6.0, the same lineage as the
existing Z3660 emulator). On real hardware `mmu.library` correctly detects the emulated 68030 PMMU.

---

## Running AMIX

### Getting started (the quick path)

1. **Firmware.** Grab a pre-built `BOOT.BIN` from this repo's Releases (no build needed) or build it
   yourself (see [`docker/README.md`](../docker/README.md)), and deploy it to the SD card.
2. **Supply the bits that can't ship here** — an A3000-variant Kickstart ROM and your AMIX `.hdf`
   (details just below).
3. **Put the `.hdf` on the SCSI id AMIX was installed on** — id 6 for the usual A3000 install. This
   matters: see the ⚠️ note under [Config](#config).
4. **Configure** `bootmode UAE_030_MMU`, point a `kickstartN` at the A3000 ROM, and map the disk with
   `hdfN` + `scsiN` (full example under [Config](#config)).
5. **Boot.** On the serial console you should see `Emulation mode 4` / `68030 MMU enabled`; AMIX then
   banners and reaches a `login:` prompt (log in as `root`).

### You must supply (not redistributable)

- **An A3000-variant Kickstart ROM.** AMIX boots through the A3000 mainboard SCSI driver, so it
  needs an A3000 Kickstart — a genuine **KS 3.1 r40.68 (A3000)** is known to work. **A4000 ROMs
  cannot boot AMIX** (no A3000 SCSI driver — you get "insert disk"). Lower A3000 KS (e.g. 2.04)
  also reaches the same point.
- **An AMIX hard-disk image** (`Amix.hdf`, an RDB/RDSK disk). AMIX itself is copyrighted
  Commodore software and is not included here.

Neither the Kickstart nor the AMIX disk image ships in this repo or in the release bundle.

### Config

A minimal AMIX `z3660cfg.txt` looks like:

```
bootmode UAE_030_MMU          # 68030 + PMMU
amix_mode YES                 # AMIX memory contract + A3000-SCSI bootstrap (default ON for this bootmode)
service_cadence 4             # AMIX-safe perf; see below

# Optional: switch off the firmware's own instrumentation (diagnostics only -- emulation
# is unaffected). All three default YES; see "run-loop overhead switches" below.
#perf_stats NO
#emu_probes NO
#core0_stats NO

# Optional: arm the interpreter profiler from power-on. Only the PROFILING firmware
# (`make profile` -> BOOT-prof.BIN) acts on these; the other two images ignore them.
# Both default to off. See docs/profiler.md.
#prof_hz 1000                  # guest-PC sampler rate in Hz; 0 or absent = off
#prof_buckets YES              # ...and the interpreter stage buckets; absent = NO

# Map an A3000-variant Kickstart (you supply the .rom file):
kickstart5 kicks/A3kKS31.rom
kickstart 5

# Put the AMIX disk image on the SCSI ID it was INSTALLED on (you supply the .hdf):
hdf0 hdf/Amix.hdf
scsi6 0                       # Amix.hdf on SCSI target 6 -- see the SCSI-ID note below
```

`amix_mode YES` is the default whenever `bootmode UAE_030_MMU` is selected, so it can be omitted;
it is shown for clarity. The same options work in a `presets/presetN.txt` quick-select file.

> **⚠️ The SCSI ID must match the ID AMIX was installed on.** AMIX bakes its root device into the
> kernel as a fixed *(controller, target)* pair — for the usual A3000 install that is **controller 0,
> target 6** (root then mounts as `/dev/dsk/c6d0s1`). So put `Amix.hdf` on **SCSI id 6** (`scsi6`).
> On any other id the kernel still loads and you get the SVR4 banner, but the root mount fails
> (`s5mountroot` → `VOP_OPEN EIO`) and you never reach login. If *your* image was installed on a
> different id, use that id instead. The firmware presents the disk on exactly the id you configure —
> there is no hidden id-0 aliasing.

---

## New configuration options

These options are specific to this fork (defined in
[`config_file.c`](../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/config_file.c)).

### `amix_mode YES|NO`

Applies the **AMIX memory contract** (a single contiguous RAM window — no separate `$08000000`
CPU-RAM board) and enables the **A3000-SCSI bootstrap**. The window is 16 MB unless
[`amix_ram`](#amix_ram-n--integer-mib) says otherwise.

- **Default:** `YES` when `bootmode` is `UAE_030_MMU`, otherwise `NO`. Existing non-AMIX configs
  are therefore unaffected.
- **Side effect:** enabling it forces `cpu_ram = NO` (the 128 MB CPU-RAM board is incompatible
  with the AMIX address map).
- It is a *separate* switch from `bootmode` on purpose: you can run plain 030+PMMU AmigaOS
  (`bootmode UAE_030_MMU`, `amix_mode NO`) without the AMIX memory layout.

### `amix_ram N`  (integer, MiB)

How much main RAM the AMIX memory contract presents. The firmware maps `N` MiB of Zynq DDR
1:1 at guest `$08000000` and dummies out everything above it, so the AmigaOS memory list —
which the AMIX kernel's `config()` copies verbatim into `bootinfo.memory[]` — holds exactly
**one** region. Only meaningful with `amix_mode YES`; ignored (with a note on the serial
console) otherwise.

- **Default:** `16`. With the key absent the map is bit-for-bit what it has always been, on
  both the 030 and the 040 line.
- **Valid range:** `1`–`128`.
- **⚠️ Values outside that range are REFUSED, not clamped.** The window starts at guest
  `$08000000` and must stop below `$10000000`, where the RTG/ethernet combo board is mapped
  (and where the `z3660eth` driver binds). A larger value would have the board silently punch
  a hole through memory AMIX believes is RAM, so the firmware prints a refusal block on the
  serial console and falls back to 16 MB. It never truncates to 128 — booting a machine at a
  size the operator did not ask for is worse than not booting at their size at all.
- **⚠️ 64 MB is the recommended cap for a shipping config.** Past the AMIX kernel's
  *describable-RAM* ceiling, `rmalloc()` returns `NULL` unchecked and the boot dies **silently**
  — no SVR4 banner, no memory line, no console output whatsoever. That ceiling is *computed*,
  has never been bisected, and **moves down as the kernel grows** (a kernel carrying
  `z3660scsi` + `z3660eth` + `cdfs` has less kernel-virtual space to spend on the frame array).
  Above 64 the firmware warns but still obeys — the knob exists so the ladder can find the
  real line.
- **68030 vs 68040.** The 4 KB-page (Model-B, 68040) kernel needs half as many `page[]`
  entries per MiB as the 2 KB-page (68030) kernel, so its ceiling sits roughly twice as far
  out. The knob is shared by both boot modes, but large values are 68040-line territory;
  under `UAE_030_MMU` the firmware says so.
- **Side effect above 16 MB:** the vestigial `a3000mem` window at guest `$07000000` is
  dropped, because its host backing store (`$09000000`) is inside the widened window. Guest
  `$07000000`–`$07FFFFFF` then reads as nothing — deliberately, so the motherboard SIMMs at
  that address stay invisible to the Kickstart's RAM sizer and cannot coalesce with our
  window into a second region (which panics the kernel in `vatosde()`).
- **Verify it took:** the serial console prints
  `[Core1] AMIX RAM window N MiB: guest $08000000-$...`, and AMIX's own
  `Total Unix memory = ...` line should scale with it.

### `service_cadence N`  (integer, `N ≥ 1`)

How many emulated instructions run between emulator interrupt-service polls. Lower = lower
interrupt latency; higher = more CPU throughput.

- **Default:** `1` (poll every instruction). Values `< 1` are clamped to `1`.
- **Throughput:** raising it speeds the emulated CPU up — measured **+27 %** at cadence 4 and
  **+34 %** at cadence 8 (dhrystone, 68040 AMIX on metal, 2026-08-22), knee at 8 as documented.
- **⚠️ It is not free for disk I/O.** On a root disk served by the emulated A3000 SCSI, raw-device
  read throughput **fell 16 %** going from cadence 1 to cadence 8 in the same session. See
  *“Cadence costs real disk throughput”* below before raising it on an I/O-bound workload.
- **⚠️ AMIX boot-safety:** the AMIX A3000-SCSI bootstrap is sensitive to INT2 latency.
  Cadence **2 and 4 boot; cadence 8 hangs the bootstrap.** **Use `service_cadence 4`** as the
  boot-time sweet spot for CPU-bound work; it must be raised to `8` *after* boot via the serial
  **SERV** menu, never in the config.
- **⚠️** A very high cadence combined with an unthrottled network flood (e.g. a fast telnet
  transfer) has been observed to panic AMIX — keep boot-time cadence ≤ 4.

### Sweeping `service_cadence` on metal — what to watch

The knob trades interrupt **latency** for CPU **throughput**, so the failure mode of a too-high
cadence is never "slower" — it is a missed or late interrupt somewhere. Named couplings, so a
calibration session knows what it is looking at when something breaks:

| Cadence | Status | Coupling that breaks first |
|---|---|---|
| 1 | default | none — maximum responsiveness, minimum throughput |
| 2–4 | **known good**, boots and runs | — |
| 8 | boots **only after** AMIX is up (set it with `SERV`, not in the config) | the **A3000-SCSI bootstrap**: `sd` open steps through SELECT and SRV_REQ **INT2**s, and at cadence 8 the emulator notices them too late — the bootstrap hangs before the kernel is loaded |
| 16–64 | throughput ceiling ~1.3× vs cadence 1, knee around 8 | **network RX under flood** (a fast telnet transfer has panicked AMIX); INT2/INT6 latency generally |

Things that are **not** coupled, and should not be blamed when they move:

- **The piscsi mailbox.** It has no interrupt at all — completion is the blocking bus cycle, and
  the native driver's `z3660intr()` is an empty stub that is never even registered. During a round
  trip core1 is spinning in `while(shared->write_scsi==1){NOP;}` and the cadence counter is not
  advancing. (`docs/piscsi-service-path.md` §2.) **This is still true, and it is still not a
  statement about “disk throughput” — see the next section for why those are two different
  things.**
- **The serial console.** It is core0's `debug_thread`, not core1's poll.
- **`do_specialties()`**, which stays per-instruction at every cadence: STOP, trace and mode-change
  remain exactly as responsive as they were.

### Cadence costs real disk throughput (measured 2026-08-22 — corrects an earlier claim)

An earlier revision of this file said disk throughput was *“measured flat across the whole 1→64
range”* and offered that flatness as confirmation of the piscsi model. **Both halves were wrong,
and in different ways.** The 2026-08-22 overhead-calibration run on A4000 + Z3660 metal (firmware
CRC-32 `3435373F`, 68040 AMIX at 128 MiB, all rungs inside one boot) measured:

| cadence | dhrystone | raw-device read (`/dev/rdsk`, 8 MiB span) |
|---|---|---|
| 1 | baseline | **665.4 KB/s** (661.5 / 661.5 / 673.3) |
| 4 | **+27 %** | — |
| 8 | **+34 %** | **557.6 KB/s** (559.2 / 544.9 / 570.9 / 555.4) — **−16.2 %** |

Reproducible, and the two sample sets do not overlap. Cadence raises guest CPU **and lowers real
disk throughput at the same time.**

**Where the flatness claim came from: an invalid test.** The benchmark row that was believed to
measure disk (`disk.seq.read.16384` against a file under `/tmp`) does **zero** device reads at
128 MiB — the scratch file is entirely buffer-cache resident, and the firmware's own C0STAT task
counters show it: `EMU>SCSIR 0c` across a window containing a full run. That row tracks guest CPU,
so it *rose* with cadence and was read as “flat/fine”. **A file-backed benchmark at ≥128 MiB of
guest RAM is not a disk benchmark.** Use a raw device, or a working set larger than the cache.

**Where the “piscsi is uncoupled” claim came from: the wrong device.** The piscsi mailbox really is
uncoupled — but the AMIX root disk in this configuration is **not on it**. `/dev/dsk/c6d0s1` is
controller 0 / target 6 of the **emulated A3000 SCSI**, served by AMIX's own `sd`/a3091 driver;
the binding is stated in the emulation itself (`Z3660_emu/src/uae/a3000_scsi.cpp`, the
`drvnum_for_target()` comment). piscsi is only the *backend* the emulation reads through. So
“piscsi is not cadence-coupled” and “disk throughput is not cadence-coupled” were being treated as
the same sentence, and they are not.

**The mechanism, which the table above this section already half-named.** The WD33C93 interrupt
**countdown** is advanced by `a3000_scsi_hsync()`, and the only thing that calls it is
`check_uae_int_request()` — the function the cadence gates:

```c
/* Z3660_emu/src/uae/newcpu.cpp, check_uae_int_request() */
if(a3000_amix_mode){ amix_tick++; static int scsi_hctr=0;
                     if(++scsi_hctr>=256){ scsi_hctr=0; a3000_scsi_hsync(); } }
```

so **one pump tick costs `256 × service_cadence` emulated instructions.** Under a live AMIX kernel
the per-status delay is honoured rather than zeroed (`a3000_scsi.cpp` `set_status()`:
`int eff = (wd_delay_mode || amix_mmu_on) ? delay : 0;`), `wd_check_interrupt()` decrements that
countdown by exactly **one per pump tick** and delivers only at 1, and after each delivery the next
queued status is re-armed to 2 ticks. Every SELECT / SRV_REQ / phase / completion interrupt of
every block command therefore waits on the pump. At cadence 8 a pump tick is 8× longer in
instructions and ~6.5× longer in wall-clock (guest IPS rose only ~1.23×), so per-command interrupt
latency grows while the SD transfer underneath it does not get any faster — and thousands of
requests later that is the 16 %.

This is the *same* coupling the cadence table above names for the A3000-SCSI **bootstrap**. What
was missed is that it does not stop after `sd` open: the pump governs A3000-SCSI interrupt pacing
for the entire session. The failure mode of a too-high cadence is “a missed or late interrupt
somewhere” exactly as documented — it just shows up here as throughput rather than a hang.

**Guidance until the pump is decoupled from the instruction cadence:** keep `service_cadence 1`
for I/O-bound work on an A3000-SCSI root. Cadence 4/8 is worth it for CPU-bound work, and costs
nothing measurable on a workload that does not touch that disk.

**Sweeping it properly.** Raw device only; several samples per rung (the guest's 60 Hz clock
quantises a ~4 s run into ~250 ticks, so one tick is ~0.4 % and a single sample cannot resolve a
few percent); every rung inside one boot, because DRAM layout varies between boots on this machine.

### `perf_stats YES|NO`, `emu_probes YES|NO`, `core0_stats YES|NO` — run-loop overhead switches

Three switches that remove **measurement and tracing**, never emulation. Off, the guest sees the
same machine; it just gets more of the ARM's time. All three default to `YES` = exactly what this
firmware has always done, so a config that omits them is unchanged.

| Key | Core | What it costs when ON | Serial toggle |
|---|---|---|---|
| `perf_stats` | core1 | the per-instruction instruction-rate accumulator (a 64-bit read-modify-write per emulated instruction) and its ~1 Hz tick | `PERFS` |
| `emu_probes` | core1 | the ISSUE-52-era run-loop probes: `[ROMENT]` `[RTE]` `[URTE]` `[USTEP]` `[IDCMP]` on the 040 loop, the wild-PC detector and retired-instruction ring on the 030 loop. Their output caps are long since exhausted on a normal boot — what remains is the per-instruction *test* | `PROBES` |
| `core0_stats` | core0 | C0STAT per-task span stamping: ~8 ARM global-timer reads per cooperative-loop iteration, on the critical path of every one of the ten cross-core round trips a 2 KB guest read costs | `CSTAT` |

Notes:

- **`PSTAT` and `CSTAT` are different things.** `PSTAT` has always gated only the *printing*;
  the stamping never stopped. `CSTAT` gates the stamping itself. C0STAT calibrates its own
  cost on the metal at the live ARM clock and prints it as the `ovh=` figure in every report and
  as `cost/boundary` at startup — read those first to decide whether it is worth switching off.
- **`emu_probes NO` is not deletion.** The instruments are one flash away — or one `PROBES` away,
  live. Building with `-DZ3660_EMU_PROBES=0` compiles them out entirely for a lean build; the
  default build keeps them.
- **`perf_stats NO` stops `[PERF]` kIPS**, obviously — the counter it reports is the counter being
  switched off. `SERV`, `PERFS` and `PROBES` all still reach the run loop with it off (the resync
  moved to `do_specialties()`, which runs only when there is already something to do).
- Toggling any of these live is safe under a running guest: they are read-only diagnostics.
- As with every YES/NO key here, a value that is neither `YES` nor `NO` is reported as an error in
  red and read as `NO` — so a typo switches an instrument **off**, it does not fall back to the
  default.

#### What the switches actually cost (measured 2026-08-22, metal, one boot)

Eight rungs against a live 68040 AMIX at 128 MiB, firmware CRC-32 `3435373F`. Repeatability of the
baseline was 0.7 %, so anything above ~1 % below is real:

| Switched off | dhrystone vs. defaults-on |
|---|---|
| `perf_stats` alone | **+2.0 %** |
| `emu_probes` alone | **+7.9 %** ← the dominant cost |
| `core0_stats` alone | **+0.1 %** — nil |
| all three | **+9.1 %** |

Three results worth carrying:

- **`emu_probes` is the expensive one**, and the reason is `[USTEP]`: its first term is `!regs.s`,
  true for *every user-mode instruction*, so a user-space workload paid the whole chain on the
  majority of its instructions long after the output cap was exhausted.
- **`core0_stats` buys the guest nothing, and `ovh=` must not be used to argue otherwise.** C0STAT
  honestly reports `ovh = 57–59 %`, but that is a share of **core0 loop time**, and core0 is not the
  bottleneck — it free-runs at ~900 k iterations/s with `ITERATION 99.7 % of uptime`. The guest is
  core1-bound. Removing 57 % of core0's work frees time nothing was waiting on. Leave C0STAT on.
- **Defaults-on is ~3.6 % SLOWER than the old hard-wired build.** Making the instrumentation
  switchable replaced unconditional code with a per-instruction predicate that the hard-wired build
  never paid. The switches only pay once one is actually thrown.

#### The lean build — `make lean`

That last point is what the **lean** firmware variant exists for. `make lean` builds core1 with
`-DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0` and emits **`sd_card/BOOT-lean.BIN`** beside the normal
`BOOT.BIN`: the instruments *and the predicates that gate them* are compiled out, which is strictly
less than either the old hard-wired build or all three switches thrown. Measured on the generated
ARM, `m68k_run_mmu040` goes from **376 instructions to 101** (0x5E0 → 0x194 bytes) and loses all
five `z3660_printf` probe calls, both `mmu_translate` calls and the `z3660_perf_tick` call; the
only calls left out of the loop body are `check_uae_int_request`, `do_specialties`, `Exception` and
`cpu_restore_fixup`. The `z3660_perf_count` / `z3660_perf_on` / `z3660_probes_on` symbols are not
in the binary at all. `service_cadence` is **not** an instrument and survives in both variants —
the A3000-SCSI interrupt pump rides on the poll it gates (see above), so removing it would change
emulation, not just measurement.

- **Production = lean; diagnostics = the default build, one flash away.** Both come out of one
  tree, and the two core1 objects never share a build directory (`Z3660_emu/build` vs
  `Z3660_emu/build-lean`) because a `-D` is not a tracked dependency.
- The variants are told apart by the boot menu's **CRC** of `Z3660.bin`, not by a banner — the
  default image is byte-for-byte the firmware it has always been.
- `Z3660_emu/src/uae/runloop_build.h` is the single copy of the rule, and the host harness asserts
  both shapes (`make run` and `make run-lean` under `Z3660_emu/test/host`).

#### The profiling build — `make profile`

Switching the instrumentation off, and then compiling it out, answered how much of the ARM's time
the *firmware's own diagnostics* were taking. It said nothing about where the remaining time goes,
which is the question that has to be answered before anything is optimised. `make profile` builds
a **third** core1 — the lean flags plus `-DZ3660_EMU_PROF=1` — and emits **`sd_card/BOOT-prof.BIN`**
beside the other two. It carries a wall-clock guest-PC sampler and per-stage ARM cycle attribution
for the interpreter (opcode fetch, extension fetch, data read, data write, MMU translate, table
walk, handler body, loop tail, fault), plus page-cache and ATC hit-rate counters.

It is built **on top of lean**, not on top of the default: a profile of the diagnostic loop would
describe a binary nobody runs, and the 9.1 % measured above is larger than several of the effects
the profile exists to rank.

It is a **measuring instrument, not a production image**. The sampler is cheap; the stage buckets
are not, and with them on the reported instruction rate is far below the lean rate — their *shares*
are the answer, their *rate* is not. The firmware measures its own probe cost at arm time and
prints it next to the totals so the inflation is subtractable rather than merely acknowledged.

Config keys `prof_hz` / `prof_buckets` arm it from power-on (both default to off), which is the
only way to profile the boot itself; the serial console has `PROF` / `PROF <hz>` / `PROF OFF` /
`PROFB` / `PROFD` / `PROFR` / `PROFZ` for everything after that. The **[full reference, wire
format and recommended metal session are in `docs/profiler.md`](profiler.md)** — including the
bias statement, which is not optional reading: samples are taken at instruction boundaries and
carry a `WEIGHT` field that a reader must honour, or the stalls this instrument exists to find are
the exact thing it deletes.

**Wire format v2** (`Z3P2`). The first metal session with this instrument, 2026-08-22, produced
four captures in which three printed numbers did not mean what they said — a probe cost priced per
*pair* but multiplied by a per-*transition* count, an ARM clock read from a compile-time constant
on a board that had retuned its PLL, and a counter called `ATC_HIT` that counted successful
translates. All three are fixed and the version is bumped; a v1 symbolizer must refuse a v2
capture. `docs/profiler.md` carries the full v1→v2 table and the correction factors for reading
an existing v1 capture.

Three operating rules, all learned from that session and all in `docs/profiler.md`:

- **`PROF OFF` is the lossless stop.** Every rung of the `PROF` rate ladder *arms*, and arming
  *zeroes* — so before v2 there was no keystroke that could freeze a config-armed boot profile
  without destroying it.
- **Size the ring to contain the workload** (`65536 / hz` seconds; `PROF <hz>` prints it). Two of
  three C1 workload captures wrapped and profiled the idle loop that followed the benchmark.
- **`PSTAT` off before any long dump** — see the runtime-toggle section below.

#### Every build archives its image — `sd_card/archive/`

Each variant is written to a **fixed** filename, so building the next one overwrites the last.
That is a trap whenever a build is being *judged*: on 2026-08-23 a rebuild overwrote
`BOOT-lean.BIN`, and `crc32 DEC21748` — the reference arm the day's A/B was measured against,
still live on the card — existed nowhere on the host afterwards, so the session could not flash
the revert it had just decided.

So `make bootbin`, `make lean` and `make profile` each end by copying their image, CRC-stamped,
into **`Z3660_system/Alfa/sd_card/archive/`** (`BOOT-lean-86C0BC18.BIN` and so on) — and, same
basename and CRC, the core1 emulator ELF it was linked from (`BOOT-lean-86C0BC18.elf`). The BIN
alone cannot be objdumped back to a named function; two build-integrity incidents (§0.13.4,
§0.15.2 in `docs/WARP-ENGINE.md`) turned on a flown, CRC-identified image whose source could no
longer be re-examined once its build directory's ELF had been overwritten by a later build. The
step is unconditional and needs `python3` on the build host (the docker image has it); the CRC is
`zlib crc32`, the same one the boot menu's `CRC` command prints, so an archived name can be
matched against a card without recomputing anything. The directory is untracked — everything
under `Alfa/` except the three `.bif` descriptors is a build artefact — and is never pruned by
the build: deleting old images is a human decision about disk.

### `isoN <file>` — CD-ROM images (upstream's key, with a trap)

`isoN` is upstream's, not this fork's, and it replaced the fork's own `cdrom_units` key during the
beta-21 resync. It fills **image slot N** exactly like `hdfN`, and additionally marks that the
image is optical: the target is then presented to the guest as a **read-only 2048-byte-block
CD-ROM** (SCSI peripheral device type `0x05`), the backing file is opened without write access, and
the RDB scan is skipped. On the AMIX side that is what `cdfs` mounts.

> **⚠️ The `isoN` slot number must equal the SCSI target number.**
>
> `isoN` records the CD flag **by image slot**, but the firmware consumes it **by SCSI target**:
> target *t* is presented as a CD if and only if slot *t* was declared with `isoN`. The `scsiN`
> line that actually assigns the image is never consulted for the flag. So the only configuration
> that works is one where the two numbers agree:
>
> ```
> iso2 iso/amix-tools.iso     # slot 2
> scsi2 2                     # target 2  <- same number
> ```
>
> A mismatch fails **silently and in both directions at once**: the image is mounted as an ordinary
> read-write RDB hard disk (so its ISO filesystem is never found), *and* whatever unrelated image
> happens to sit on the target whose number matches the iso slot is presented as a read-only
> CD-ROM. Only slots `iso0`–`iso6` are representable at all — `cd_target[]` is seven entries wide.
>
> Beta 21's own shipped `z3660cfg.txt` trips this: it pairs `iso6 iso/AmigaOS39.iso` with
> `scsi4 6`, which produces no working CD. Treat the shipped example as a demonstration of the
> trap, not as a template.

### `arm_frequency N` — Zynq ARM clock (upstream's key, with an AMIX trap)

Also upstream's, also worth a warning here. `arm_frequency` sets the Zynq PS clock; the accepted
values are the sixteen names in `arm_frequency_names[]` (`config_file.c:200`), 667 through 1300 MHz,
and the firmware's own default is **667**.

> ⚠️ **`arm_frequency 1100` corrupts kernel reads on the real-CPU path.** Measured 2026-09-01 on
> card B (`bootmode CPU`, a real MC68LC060; see [`KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) for the card
> naming). The SVR4 loader reaches the disk and then stops at:
>
> ```
> WARNING! Kernel file checksum mismatch. Expected 0x0000A7D9, found 0x000054A2.
> ```
>
> **`Expected` is constant and `found` differs on every boot** — 0x3A7E, 0xCC59, 0x8A7D, 0x54A2
> across one session. That is a corrupt read path, not a stale stored checksum, and it is the
> reading of earlier campaigns' "kernel checksum mismatch" rows too.

Three things are worth knowing before spending time on it:

- **`arm_frequency 667` reduces it but does not fix it.** It came back on a later cold power-on
  after three clean boots. Treat 667 as the value to run, not as a cure.
- **The SD card is not the source.** `CRC 1:/hdf/Amix.hdf` from the boot menu returned
  **522F16D2 (104857600 bytes) twice** — a stable, repeatable read of the same image the loader
  chokes on. The bit errors are downstream of the SD, in the 060↔Zynq transfer.
- **`service_cadence` is not a lever here.** It is an 030-MMU run-loop knob and is inert with a
  real CPU.

The measured `arm_frequency 1100` rows elsewhere in this repo — the WARP campaign sections of
[`WARP-ENGINE.md`](WARP-ENGINE.md) — were taken under an *emulated* boot mode on card A, before
this was known. They are not a recommendation to run 1100.

### Runtime debug toggles (not config-file options)

The emulator prints verbose debug traces that visibly slow interactive use. These are gated at
runtime from the serial **C** (console) menu — they are *not* `z3660cfg.txt` options:

- **`DEMU`** — gates the emulator floods (`[PC]`, `[RTE-B-IF]`, fixup traces). Turning these
  **off makes interactive AMIX feel substantially faster.**
- **`DSCSI`** — gates the `[PISCSI]` SCSI register-trace flood.

Both default to off (quiet) in normal operation.

#### Never send a console word to the boot menu

Every command on this page is a **word** typed at the running emulator's `C` console. The
**boot menu** — the pre-boot prompt reached by spamming `C` at power-on — reads **one key at a
time**, and several of its keys are destructive. Typing a console word there is not a no-op and
is not an error message: it is executed letter by letter.

`DOPC` at the boot menu is `D`, `O`, `P`, `C` — *download and flash the CPLD firmware*,
*overwrite `z3660scsi.rom`*, *start the HTTP server*, and then `C` catches the autoboot
countdown. `PSTAT`, `PROF`, `DEMU`, `DSCSI` and `SERV` all start with menu keys too.

So: **reach the running console first, then type the word.** Sending it earlier is not safer —
it is the one way to make it dangerous. The same applies to anything that emits an escape
sequence: a bare ESC on the serial console spins core0 forever (`core0-loop-blocking-audit.md`
B3), so arrow keys, terminal status queries and a `screen`/`tmux` partial sequence are all live
hazards on this port.

#### `SCT` is a write to the SD card

Most console words are toggles. **`SCT` (`SAVE_CURRENT_TIMINGS`) is not** — it calls
`write_timings()` (`debug_console.c:1038-1041`), which opens the timings file named by
`1:/timings/timings.txt` with `FA_CREATE_ALWAYS` and rewrites **all 15 clock-index blocks** from
the firmware's in-RAM table. No prompt, no confirmation, no backup, and any comments or
formatting the file carried are gone.

Whether that is harmless or destructive depends on something you cannot see from the console:

- if the firmware **read** the timings file at boot, RAM holds the card's own values and `SCT`
  writes them straight back — near enough a no-op;
- if it **failed** to read it — a missing, renamed or misspelt file, which
  `read_timings()` reports by returning 0 while printing **nothing** — RAM holds the
  compile-time fallback table, and `SCT` overwrites the card's file with that.

On a card whose timings have been adapted at all (and this fork's bench card is one — see
[`clock-timings.md`](clock-timings.md)), the second case destroys the only copy of the tuning.
The same `write_timings()` sits behind the ZTop timings-editor OK button, and behind the mobotest
`G` key, which loads the compiled defaults first and is therefore *always* destructive.

None of the three is on the boot path, and a failed read never writes anything by itself — so
the whole hazard is operator-side. Keep a copy of the card's timings file off the card, and treat
`SCT` the way you would treat any other console command that writes storage.

#### One UART, two cores: turn `PSTAT` off before any long dump

Core0 and core1 print through the **same UART behind the same semaphore**
(`SHARED.uart_semaphore`), taken and released around *every* `printf`. A core1 profiler ring dump
is ~65 500 sequential acquisitions spread over about two minutes.

For the whole of that, core0's periodic `PSTAT` piscsi and `C0STAT` loop reports are contending
for the same semaphore — and **core0 spins while it waits**, so the cooperative loop that services
piscsi requests is starved for as long as the dump runs. Two consequences, and the second is the
one that bites silently:

- interleaved lines break the dump's fixed-width grammar and a symbolizer rejects the capture;
- **guest I/O latency degrades for the duration**, which is invisible in the dump itself.

So: `PSTAT` off → dump → `PSTAT` on. The same contention is why a ring dump's first header line
arrives with its `[PROF] ` prefix eaten by the console echo of the command that requested it.

#### `PERFS` / `PROBES` answer even when there is nothing to switch

Both commands are handled by **core0**, which cannot tell which core1 image is running. On
`BOOT-lean.BIN` and `BOOT-prof.BIN` the instruments they gate are compiled **out**: the mailbox
bit flips, the console prints `PERF STATS ON`, and nothing happens. The same is true of `prof_hz`
/ `prof_buckets` on a non-profiling image and of every `PROF*` command.

That is deliberate — one `z3660cfg.txt` is shared across a flash of any variant, and a key that
made the default firmware refuse to boot would be far worse than one that quietly does nothing —
but **the console echo is not evidence that an instrument is armed**. The boot banner is. A sweep
that flips `PERFS`/`PROBES` on a lean image is measuring nothing, and will report the resulting
flat line as a result.

---

## How the A3000 SCSI bootstrap works

The emulated controller
([`a3000_scsi.cpp`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/a3000_scsi.cpp)) is
ported from WinUAE's `a2091.cpp` (Commodore SuperDMAC + WD33C93 core, GPLv2 — the same provenance
as the imported 030 MMU code), with two deliberate divergences:

1. **Interrupt model.** The Amiga INT2 line is recomputed as a pure level function of
   `(SDMAC.CNTR & INTEN) && WD.ASR_INT` after every register access (WinUAE's hsync-paced status
   queue has no analogue here).
2. **The SCSI target is the Z3660 backend.** Instead of WinUAE's `scsi.cpp`, the emulated
   controller synthesises `INQUIRY` / `READ CAPACITY` / `MODE SENSE` / `TUR` / `REQUEST SENSE`
   and moves `READ`/`WRITE` block data over the existing **PISCSI cross-core channel** to core 0's
   FatFS. The SCSI target id is the `devs[]` unit index set by `scsiN` in the config, and each id maps
   to its own backend disk — the disk answers on exactly the id you configure, with no id aliasing.
   AMIX's root lives on the id it was installed on (target 6 in the usual A3000 install).

It runs on core 1 (the 68k emulator); its MMIO lives at the `$00DD0000` page, gated by
`amix_mode`. Full design notes:
[`AMIX_SCSI_design.md`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/AMIX_SCSI_design.md).

---

## Verifying it booted (serial console)

On the JTAG/serial console (`/dev/ttyUSB0`) a correct AMIX boot shows:

```
Emulation mode 4
68030 MMU enabled
[Core1] Starting UAE_030_MMU emulator        (… _AMIX when amix_mode is on)
```

If you instead see `Emulation mode 1` or `[SD Init] FAIL`, the firmware fell back to the
68040-JIT path — re-check that `BOOT.BIN` deployed and that `bootmode UAE_030_MMU` is active.

AMIX then prints the SVR4 banner and memory sizing, runs its rc scripts, and reaches a `login:`
prompt on the HDMI console (log in as `root`). Once it is multi-user you can also reach it over the
network (`telnet` to the AMIX guest). If it banners but never reaches `login:`, the most common cause
is the disk being on the wrong SCSI id — see the SCSI-id note above.

When you issue `reboot` inside AMIX the emulator detects the warm-reset vector (the overlay isn't
restored on a 68k reset) and performs a **clean Zynq reboot** rather than flooding the bus; the
board comes back in ~2.5 minutes.

---

## Current status

AMIX 2.1 **boots reliably to a stable multiuser login shell** under this firmware: the dynamic
linker and `/sbin/init` run, the SVR4 banner and memory sizing appear, the system reaches
multi-user, and you get a root login on both the HDMI console and over telnet. It stays up under
sustained load, and warm/cold reboots work in day-to-day use.

> **Correction (2026-08-27) — the old "27-reboot soak with concurrent fork/exec load, all clean"
> claim is half retracted.**
>
> **The workload half stands.** That half is scored from the firmware's wild-PC guard, which
> `tools/amix-stress/amix_soak.sh` reads off the **serial UART over ssh** — a path completely
> independent of the telnet session the workload runs on. Nothing below touches it: the guard did
> not latch under sustained fork/exec and deep-recursion demand-paging load, and that result is
> unaffected.
>
> **The reboot half is indeterminate.** `amix_soak.sh` is the only recorded reboot-soak tool, and
> its reboot step was broken from its first commit. The shutdown it sent ended in `&`, and the
> telnet runner appends `; echo DONE_$?` to every command, so the box received `… &; echo DONE_$?`
> — a Bourne syntax error. The shutdown never executed. The script could not detect that either:
> its post-reboot check was a bare TCP connect to port 23, which a box that never rebooted answers
> instantly, so a no-op scored as `boot OK`. The 27 reboots may have been driven by hand — the
> record does not say, and the count does not match the script's every-fifth-cycle cadence. So the
> number is withdrawn rather than restated: it is not disproven, it is unestablished.
>
> Both defects are fixed. The shutdown is parenthesised, and a reboot must now be **proven** —
> telnet has to go *down* after the shutdown, and the `who -b` boot stamp has to *change* across
> the cycle — with an unprovable reboot a hard failure instead of a green line.
>
> **Re-establishing the number:** run `tools/amix-stress/amix_soak.sh <cycles>` against the bench.
> It reboots every 5th cycle, so **5 cycles = 1 proven reboot, roughly 30 minutes on the box**
> (~25 min of workload plus a ~2.5 min boot). The closing `SOAK DONE … reboots_proven=N` line is
> the count; that number, and no remembered one, is what belongs in this document.

The post-banner stalls that earlier blocked login are **resolved in practice.** The boot-time
failures came down to emulator-side 68030 bus-error-frame corruption on the demand-paging path —
fixed by the in-RTE retry-access frame fix and the multi-fault continuation fix, both carried onto
this base. AMIX now boots and runs through that path cleanly, so the earlier `a3091` /
completion-ordering stall no longer occurs in normal operation.

**Known remaining issue (does not block normal use):** under *extreme* sustained demand-paging load
a rare 68030 bus-error-frame SR-flip can still trip the emulator's user-PC sanity guard. It did not
recur across a multi-hour fork/exec soak — that workload result stands on its own, since the wild-PC
guard is read from the serial UART independently of the soak's telnet path — but the "27-reboot"
framing of that run is indeterminate; see the correction above. Normal boot and interactive use are
unaffected. It is tracked for a future fix.

The deeper guest-side analyses (the `a3091` driver disassembly, the `lpsched` memory-coherency
investigation) are retained under [`docs/investigations/`](investigations/) for reference and for
anyone who wants to push emulation fidelity further.

---

## Defects found in upstream beta 21 (candidates to report)

The beta-21 resync turned up six upstream defects. All four are fixed or worked around on this
branch; none has been reported upstream yet. They are recorded here so the reports can be written
from the evidence rather than from memory.

1. **Direct-DMA cache maintenance weakened to L1-only** (`Z3660/src/scsi/scsi.c`, piscsi READ and
   WRITE handlers). Beta 21 carries this fork's coherency comments verbatim — including the
   sentences that say "L1+L2" — but calls `Xil_L1DCacheInvalidateRange` /
   `Xil_L1DCacheFlushRange` underneath them instead of the whole-hierarchy
   `Xil_DCacheInvalidateRange` / `Xil_DCacheFlushRange`. On Zynq-7000 those are distinct APIs and
   the L1-only forms stop at the PL310 L2, which the SD controller's DMA sits below. The write path
   can therefore put stale DRAM on the disk, and the read path can have an L2 writeback land on top
   of freshly DMA-ed bytes mid-transfer. This affects any direct-DMA (non-bounce) piscsi transfer,
   not only AMIX. **The single highest-severity finding of the resync.**

2. **`else` following `else`** (`Z3660_emu/src/main.cpp`, the emulator-dispatch block inside the
   `#else` arm of `#ifdef MUSASHI_EMULATOR`). Not valid C++. It compiles today only because
   `main.cpp` defines both `MUSASHI_EMULATOR` and `UAE_EMULATOR`, so that arm is preprocessed away;
   it is a latent build break for any configuration with `MUSASHI_EMULATOR` off. Upstream took this
   fork's decision-#8 change (halt loudly on an unknown boot mode) but kept its own bare `else`
   instead of converting it to `else if(shared->cfg_emu==UAEJIT_040)`.

3. **`machdep/m68k.h` two-phase include guard lost in the x86 arm.** The header is included twice
   per translation unit, before and after `struct regs` exists, with `REGS_DEFINED` distinguishing
   the passes. Beta 21's rewrite (which imports the ARAnyM fast-flag implementation) preserved the
   split only in the `CPU_arm` branch; the x86 and generic branches now emit their declarations
   unconditionally and redefine `flag_struct` and `cctrue` on the second pass. Upstream never sees
   it — the firmware only builds for `CPU_arm` — but it breaks any host build of this tree, which
   for this fork is the MMU test harness. Fixed for the x86 arm; the generic arm has the same
   defect and is deliberately left alone, since nothing here compiles it and an untested edit is
   worth less than an accurate report.

4. **`isoN` slot/target mismatch**, described in detail above. Beta 21's shipped example config
   demonstrates the bug. Not fixed here: adopting upstream's parser unmodified was a deliberate
   choice, so this fork documents the constraint instead of diverging from it. **The same parser
   also wrote out of bounds** — `config.cd_target[index]` is indexed 0..19 against a 7-element
   array, so `iso7`..`iso19` scribbled into `adf_num[]` and the head of `hdf[0]`. That half *is*
   fixed here (`8dad55a`): the keys are refused with a console line naming the 0–6 limit.

5. **`httpd` sends a malformed 200 header** (`Z3660/src/lwip/httpd.c`, `send_http_header`). Each
   line of the status/header block ends with a bare `\r` instead of `\r\n`, so the response
   arrives as one run-on line with no blank line before the body:
   `HTTP/1.1 200 OKContent-Type: text/htmlConnection: close`. The file itself is transmitted
   intact — a raw socket read returns every byte — but a conforming client discards the body, so
   the web UI renders empty. Observed on real hardware 2026-08-02. `http_send_404()` two functions
   later already spells `\r\n`, which is what identifies this as a typo rather than a convention.
   Fixed here; `.json` was also added to `mime_type()`, since the shipped UI fetches
   `web/locales/*.json` and they were going out as `application/octet-stream`.
   **Both fixes flashed and verified on the board 2026-08-03** (firmware crc32 `59FAE8CA`): a
   strict client now receives `HTTP/1.1 200 OK` / `Content-Type: text/html` / `Connection: close`
   on three separate lines and gets the full 7,410-byte body — it received **zero** bytes before —
   and `locales/en.json` arrives as `application/json` and parses, which is what makes the UI's
   forced-English path actually apply instead of falling through to the Spanish markup.

6. **The web UI's own files are not installed anywhere.** `httpd` serves from `1:/web`, but beta 21
   ships those files only under `Z3660/src/web/` in the source tree — nothing copies them to the
   card, so a freshly updated board answers every request with `f_open failed for
   1:/web/index.html: 5` (FatFs `FR_NO_PATH`) and a 404. Fixed here by vendoring them into
   `z3660-zturn_SD_content/DATA Second Partition (exFat)/web/` so they travel with the card image.
   Note the firmware's own `PUT /upload/<name>?path=<dest>` endpoint can populate a running board
   without touching the boot menu — it creates the destination directory if missing.

---

## Benchmarking

`tools/amix-bench/` ships a portable Dhrystone 2.1 + disk-I/O benchmark with precompiled AMIX
binaries, used to compare real vs. emulated 030 throughput and to tune `service_cadence`. See
[`tools/amix-bench/README.md`](../tools/amix-bench/README.md).

---

## See also

- [`README.md`](../README.md) — project overview and quick start
- [**amix-kerntools**](https://github.com/jusii/amix-kerntools) — the AMIX guest-side drivers (`amix-z3660scsi`, `amix-z3660net`) that run inside AMIX
- [`KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) — hardware/firmware limitations
- [`clock-timings.md`](clock-timings.md) — how the clock table is loaded, what this fork's bench
  card tuned, and the `SCT` / ZTop / mobotest paths that overwrite it
- [`docker/README.md`](../docker/README.md) — building the firmware from source
- [`AMIX_SCSI_design.md`](../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/AMIX_SCSI_design.md),
  [`AMIX_ethernet_driver_plan.md`](../z3660-drivers/amix/AMIX_ethernet_driver_plan.md) — design notes
