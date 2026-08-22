# The interpreter profiler (`BOOT-prof.BIN`)

A third firmware variant that measures where the 68k interpreter's time goes. It exists
because the next round of emulator optimisation has to be ranked by measurement rather
than by argument, and until now nothing in this firmware could say what fraction of a
guest instruction is opcode fetch, address translation, or the handler body.

It is **not a production image**. It costs measured time to run and `BOOT-lean.BIN` is
what goes back on the card afterwards.

- Contract and wire format: `Z3660_emu/src/uae/z3660_prof.h`
- Engine: `Z3660_emu/src/uae/z3660_prof.cpp`
- Config keys: `Z3660/src/prof_config.h`
- Build: `make profile` → `Z3660_system/Alfa/sd_card/BOOT-prof.BIN`

---

## The three variants

| Image | Core1 built with | What it is for |
|---|---|---|
| `BOOT.BIN` | *(no flags)* | Diagnostic. ISSUE-52 probes + perf accumulator compiled in, switchable at runtime (`PROBES` / `PERFS`). |
| `BOOT-lean.BIN` | `-DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0` | Production. Instruments **and their predicates** gone. |
| `BOOT-prof.BIN` | the lean flags **plus** `-DZ3660_EMU_PROF=1` | Measurement. The lean loop plus the profiler. |

The profiling image is built **on top of lean**, not on top of the default. A profile of
the diagnostic loop would be a map of a binary nobody runs, and the 2026-08-22 metal
calibration priced the difference between the two loops at 9.1 % of guest throughput —
larger than several of the effects this instrument exists to rank. The host harness
asserts the lean base (`test_prof_build_variant`) rather than trusting the Makefile.

All three build into separate object directories (`build`, `build-lean`, `build-prof`).
Nothing in the build tracks a `-D` **or a header** as a dependency, so switching variants
without separate directories would let half the other variant's objects survive and the
link would happily succeed. Use `make clean-all` (or `rm -rf Z3660_emu/build*`) after any
header edit.

---

## Two instruments, deliberately separate

### The sampler — "where in the guest"

A wall-clock tick captures the guest PC, the opcode, and a few mode bits into a ring in
core1's `.bss`. The tick itself is cheap: one ARM PMU cycle-counter read and a modular
compare per emulated instruction.

This is the instrument that answers **"is the guest PC distribution concentrated?"**,
which is the same question as "would a decoded-op cache pay for itself?".

### The buckets — "where in the interpreter"

A phase machine charges every ARM cycle to exactly one interpreter stage. Its *shares* are
the answer; its *rate* is not.

### What each of them actually costs

Counted out of the generated ARM (`objdump -d` of the lean and profiling `Z3660_emu.elf`),
not estimated:

| | lean | profiling | delta |
|---|---|---|---|
| `m68k_run_mmu040` | 101 | 173 | +72 |
| `m68k_run_mmu030` | 205 | 268 | +63 |
| `mmu_translate` | 210 | 225 | +15 |
| `uae_mmu040_get_long` | 77 | 100 | +23 |
| `uae_mmu040_put_long` | 85 | 107 | +22 |
| `uae_mmu040_get_iword` | 68 | 96 | +28 |
| `z3660_prof_enter` / `_exit` / `_count` | — | 51 / 44 / 10 | (out of line) |

The shape that matters is not in the deltas above but in the last row. **Every instrumented
accessor makes three out-of-line calls** — `enter`, `count`, `exit` — and it makes them
whether or not the buckets are switched on, because `PROFB` gates the *bodies* of enter and
exit, not the calls to them. So:

- **the floor of this image's distortion is three calls per memory access**, paid even with
  the sampler and the buckets both off. Two or so accesses per emulated 68k instruction, so
  order a hundred extra ARM instructions per guest instruction before anything is armed;
- **with the buckets on**, those calls execute their full bodies (a PMU read, a 64-bit
  accumulate, a stack push/pop) instead of returning on a predicate, which is several
  hundred more.

ARM instructions are not ARM cycles — the interpreter's own work is memory-heavy and
stalls, while the probe work is register-heavy — so the time ratio will be better than the
instruction ratio, by an amount only metal can say. **Do not quote an absolute instruction
rate from this image, and do not compare one to the lean figure.** The four-point A/B in
the metal session below (lean → profiling idle → sampling → buckets) is what turns the
static counts above into real percentages, and it is an outstanding measurement, not a
result.

The sampler and the buckets are still switched independently (`PROF` / `PROFB`), because
the difference between them is large and a session wants the cheaper map and the richer one
separately. "Cheaper" is the honest word; neither is free, and only the metal A/B says how
much of the PC map's wall-time claim survives.

### The counters — always on in this image

The hit-rate and event counters are a third thing and are **not** switchable: in a
profiling build they always run. Each is a 64-bit increment on an accessor that was going
to be called anyway; they are exact regardless of what the timing instruments are doing;
and they are the numbers the optimisation ranking needs most. Making them switchable would
add a predicate to buy back an increment — precisely the trade the C3b calibration
measured and rejected. In `BOOT.BIN` and `BOOT-lean.BIN` they do not exist at all.

The practical consequence is worth stating: **take the counters from the sampler-only run**
as well as the bucket run. They are unaffected by the bucket distortion, so the cheap run
gives you exact hit rates for free.

### Buckets

| id | name | what lands here |
|---|---|---|
| 0 | `LOOP` | run-loop bookkeeping outside every named stage |
| 1 | `FETCHOP` | the run loop's own opcode fetch (`x_prefetch`) |
| 2 | `FETCHEX` | extension / EA words fetched by the handler |
| 3 | `READ` | data reads, **excluding** translation |
| 4 | `WRITE` | data writes, **excluding** translation |
| 5 | `XLATE` | `mmu_translate` / ATC search, **excluding** the table walk |
| 6 | `WALK` | the descriptor table walk itself |
| 7 | `HANDLER` | handler body remainder (dispatch minus everything above) |
| 8 | `TAIL` | service-cadence poll + `do_specialties()` |
| 9 | `FAULT` | last probe transition → the run loop's `CATCH` |
| 10 | `PROF` | the profiler's own dump work, kept out of the rest |

Time is **exclusive**: a translate inside a data read appears in `XLATE` and *not* also in
`READ`. `sum(buckets) == total measured cycles` by construction, and the host harness
asserts exactly that against an injected clock.

### Counters

`INSNS`, `INSNS_SUPER`, `FETCH`, `READ`, `WRITE`, `IPAGE_HIT`, `IPAGE_MISS`,
`DPAGE_RHIT`, `DPAGE_RMISS`, `DPAGE_WHIT`, `DPAGE_WMISS`, `XLATE`, `ATC_HIT`, `ATC_MISS`,
`MISALIGN_R`, `MISALIGN_W`, `FAULTS`, `TRANSITIONS`, `STACK_OVF`. All 64-bit — a 32-bit
counter overflows in about seven minutes of hot interpretation, which is shorter than the
boot profile this exists to take.

Three tiers of address translation are separately visible, which is the point:

- **tier 0** — the one-entry instruction page cache and 256-entry data page caches
  (`IPAGE_*` / `DPAGE_*`). A hit here means `mmu_translate` is never called.
- **tier 1** — the 4-way ATC inside `mmu_translate` (`XLATE` = tier-0 misses, `ATC_HIT`).
- **tier 2** — the descriptor table walk (`ATC_MISS`, and the `WALK` bucket).

`STACK_OVF` must be zero. A dump showing anything else is not trustworthy.

> **68030 caveat.** The 030 path gets the same bucket split and the ATC/walk tiers, but
> **not** the tier-0 page-cache hit counters: upstream has no scaffolding for them on that
> side, and adding twelve branch edits to a fork of someone else's emulator on the path
> that is not the optimisation target was not worth it. The row is absent, not zero-valued
> by accident.

---

## Bias — read this before believing a number

1. **The sampler fires at instruction boundaries, not on an interrupt.** There is no timer
   IRQ on core1; the tick is a deadline compared against `PMCCNTR` in the run-loop tail.
   - A sample is attributed to the instruction that was **executing** when the deadline
     expired — the tail still holds that instruction's start PC. That is why the poll is
     in the tail and not before the fetch.
   - Time spent **off** the run loop (a `STOP` spin, `Exception()`, the `CATCH`) accrues no
     samples while it lasts. The first tail afterwards takes one sample whose **`WEIGHT`**
     says how many tick periods it stands for. **A symbolizer must weight every sample by
     this field.** Treating each sample as 1 silently deletes exactly the stalls a
     profiler exists to find.
   - An instruction longer than one tick period (a mailbox round trip to core0 through an
     emulated device) yields one heavy sample, not several: correct total, coarse
     resolution.
2. **The buckets charge the probe's own cost to the buckets.** Each transition is a
   counter read, a subtract, a 64-bit accumulate and a stack push/pop, and there are 8–14
   transitions per emulated instruction. The firmware **measures** this at arm time and
   reports it as `probe_cyc` alongside the `TRANSITIONS` count, so the inflation is
   computable and, if you care, subtractable per bucket by its share of transitions.
3. **A guest page fault is a C++ throw** and unwinds past every pending probe exit. The
   `CATCH` charges the abandoned span to `FAULT` and resets the phase stack. `FAULT` is
   therefore "the fault path plus a fragment of whatever stage it interrupted" —
   deliberately not split finer, because the split would be a guess.
4. **`PMCCNTR` is 32 bits** and wraps every ~6.4 s at 666 MHz. Bucket spans are bounded by
   one run-loop iteration, so modular subtraction is exact. The long-run cycle base is
   extended to 64 bits at each sample; if core1 leaves the run loop for longer than one
   wrap, the base loses 2³² cycles silently. **That is why the dump reports both the PMU
   span and an independent ARM global-timer span** — if they disagree by more than a
   percent, a wrap was missed.
5. **The ARM clock is measured, not assumed.** Arming times `PMCCNTR` against the global
   timer over ~10 ms and reports `cpu_hz`. Changing the ARM clock after arming invalidates
   the Hz→cycles conversion; the header records the clock actually calibrated at.

---

## Controls

### Serial console

| command | effect |
|---|---|
| `PROF` | cycles the sampler: off → 250 → 1000 → 4000 Hz → off. Arming **zeroes** buckets, counters and ring. |
| `PROFB` | toggles the stage buckets. |
| `PROFD` | dumps stage buckets + counters (~35 lines). |
| `PROFR` | dumps the newest 4096 samples (~90 KB, about 8 s at 115200). |
| `PROF RING` | dumps the whole ring: up to 65535 samples, ~1.4 MB, about two minutes. |
| `PROFZ` | zeroes buckets, counters and ring **without** disarming. |

Core0 cannot tell which core1 image is running — there is no "profiler present" bit, and
adding one would mean another hand-mirrored `SHARED` field. So these commands report what
they **asked for**. The discriminator is the boot line the profiling image prints:

```
[PROF] profiling build: ARM clock 666666666 Hz (measured), enter/exit pair 11 cyc, ring 65536 x 8 B
```

No `[PROF]` reply to a command means core1 is not the profiling image.

### Config file (`z3660cfg.txt`)

```
prof_hz 1000          # sampler rate; 0 or absent = off
prof_buckets YES      # stage buckets from power-on; absent = NO
```

Both default to **off**: an absent key must produce a board that does not profile, which
is the same "absent means unchanged" rule as the `perf_stats` / `emu_probes` switches even
though it lands on the opposite value.

These exist for the one profile the serial console structurally cannot take: **the boot
itself**. By the time a human can type `PROF`, the interesting part is over. They are read
from the main config file only, not per preset. On a non-profiling image they parse,
publish, and are ignored — a config file is shared across a flash of any variant, and a
key that made the default firmware refuse to boot would be far worse than one that quietly
does nothing.

---

## Wire format

Version 1. A symbolizer **must refuse a version it does not know** rather than guess.

### Sample record — 8 bytes

| offset | size | field |
|---|---|---|
| 0 | 4 | `pc` — guest PC of the instruction executing at the tick |
| 4 | 2 | `opcode` — its opcode word (0 if the fetch itself faulted) |
| 6 | 2 | `flags` |

`flags`:

| bits | meaning |
|---|---|
| 0 | `SUPER` — SR.S, supervisor mode |
| 1 | `MMU` — translation was enabled |
| 2 | `AMIX` — the AMIX memory contract was armed |
| 3 | `CPU040` — 1 = 68040 run loop, 0 = 68030 |
| 7:4 | reserved, **always 0** in version 1 — mask them off |
| 15:8 | `WEIGHT` — tick periods this sample stands for, 1–255, saturating |

### Ring header — 80 bytes

`magic`(4) `version`(4) `rec_size`(4) `rec_count`(4) `ring_entries`(4) `hz`(4)
`period_cyc`(4) `cpu_hz`(4) — then, 8-byte aligned at offset 32 — `samples`(8) `drops`(8)
`cyc_span`(8) `wall_ticks`(8) — then `wall_hz`(4) `build_flags`(4) `reserved[2]`(8).

`build_flags`: bit 0 = run-loop probes compiled in, bit 1 = perf accumulator compiled in,
bit 2 = profiler compiled in. The shipped profiling combination is **`0x04`**.

`samples` is the total taken; `drops` is how many the ring overwrote. **`drops > 0` means
the dump shows only the tail of the run** — a wrapped ring presented as a whole-run profile
is the single easiest way to get a wrong answer out of this.

### Serial rendering

Frozen at version 1. Fixed-width lowercase hex, no `0x` prefix, oldest first.

```
[PROF] ring hdr magic=Z3P1 version=1 rec_size=8 rec_count=4096 ring_entries=65536 hz=1000 period_cyc=666666 cpu_hz=666666666
[PROF] ring hdr samples=124311 drops=58775 cyc_span=41234567890 wall_ticks=123703703 wall_hz=333333333 build=0x04
[PROF] ring begin
S 08001234 4e71 0103
S 08001236 6100 0103
...
[PROF] ring end n=4096
```

Fields are fixed width so a symbolizer can slice by column and a **truncated serial capture
is detectable rather than merely mis-parsed**. `rec_count` in the header must equal the `n`
on the `ring end` line; if it does not, the capture lost lines.

The stats dump (`PROFD`) is line-oriented in the same way: `[PROF] b <id> <NAME> cyc=<n>
<pct>%` per bucket, `[PROF] c <id> <NAME> <n>` per counter, then a few `[PROF] r` derived
rates.

---

## What the symbolizer has to do

Symbolization is the port lane's business; the firmware emits raw records only. The
minimum correct implementation:

1. **Check `magic` and `version`.** Refuse anything else.
2. **Weight every sample by `WEIGHT`.** A histogram that counts samples instead of weight
   is wrong in exactly the places that matter.
3. **Report `drops`.** If non-zero, say plainly that the profile covers only the last
   `rec_count` samples, not the run.
4. **Cross-check `cyc_span` against `wall_ticks / wall_hz × cpu_hz`.** A divergence over a
   percent means a `PMCCNTR` wrap was missed and cycle totals are short by a multiple of
   2³².
5. **Split by `SUPER`** — kernel and user-space hot spots are different questions and both
   are in the same ring.
6. **Symbolize `pc` against the right image**: with `SUPER` set and `AMIX` armed, against
   the AMIX kernel's symbol table; in user mode, against the running binary. `MMU` tells
   you whether `pc` is a translated virtual address.
7. **Histogram `opcode` as well as `pc`.** The opcode distribution is the direct input to
   ranking dispatch specialisation, and it costs nothing extra to produce.
8. **Report the `WEIGHT` histogram itself.** It is the honest measure of how much of the
   run the sampler could not observe.

---

## Recommended metal session

Flash `BOOT-prof.BIN`, take the three profiles below, then flash `BOOT-lean.BIN` back.
Capture the serial console to a file throughout — the dumps are long and are meant to be
read by a script, not a human.

**Before anything: record the boot line.** `[PROF] profiling build: ARM clock … Hz
(measured), enter/exit pair … cyc` is the calibration every later number depends on.

1. **Boot profile.** Set `prof_hz 200` and `prof_buckets NO` in `z3660cfg.txt`, cold boot,
   and once AMIX is at a login prompt run `PROFD` then `PROF RING`. 200 Hz across a
   multi-minute boot fits inside the 65536-sample ring without wrapping; confirm with
   `drops=0` in the header. Answers "which guest code dominates a boot".
2. **Dhrystone profile.** `PROFZ`, `PROF` up to 4000 Hz, run the benchmark, `PROF` down to
   off, then `PROFD` and `PROFR`. A dhrystone run is short enough that 4 kHz will not wrap
   the ring, and it is the workload the 5427 figure came from — so this profile is
   directly comparable to the number C2 will be trying to move.
3. **Stage attribution.** `PROFB` on, `PROFZ`, re-run the same dhrystone, `PROFD`. Expect
   the reported instruction rate to be far below 5427 — that is the instrument, not the
   interpreter. Read the per-bucket **shares** and the `probe overhead inside the totals`
   line, and take the counters (`IPAGE_*`, `DPAGE_*`, `ATC_*`, `MISALIGN_*`) from this run:
   they are exact regardless of the timing distortion.
4. **Install-workload profile** (optional, and the one most likely to surprise): repeat 2
   during a `pkgadd` or a large file copy. It is the workload with real device traffic, so
   it is where the `WEIGHT` histogram and the `TAIL` bucket earn their keep — a mailbox
   round trip is invisible to instruction counting and obvious to a wall-clock sampler.

**The overhead A/B, which is the one number this lane still owes.** Run the same dhrystone
at four points and report all four:

1. `BOOT-lean.BIN` — the 5427 baseline;
2. `BOOT-prof.BIN`, nothing armed — isolates the cost of the three unconditional accessor
   calls, i.e. the floor of this image;
3. `BOOT-prof.BIN`, `PROF` at 1 kHz — adds the sampler tick;
4. `BOOT-prof.BIN`, `PROFB` on as well — adds the bucket bodies.

Three separate questions live in those four points and they are routinely conflated: what
compiling the profiler in costs, what running the sampler costs, and what the buckets cost.
Only 2→3 is the number that says whether the guest-PC map can be trusted as wall-time.
