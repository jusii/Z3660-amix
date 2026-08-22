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

**Wire format is version 2.** A symbolizer written against version 1 must refuse a v2
capture, and the magic changed from `Z3P1` to `Z3P2` so that even a tool which only greps
the magic refuses rather than mis-reads. See [v1 → v2](#v1--v2-what-moved-and-why) for the
complete delta; the short version is that v1 mislabelled three numbers and could not see
inside its own largest bucket.

---

## v1 → v2: what moved, and why

The 2026-08-22 "C1" metal session took four captures with the v1 instrument. The C2 map
audit (`amix-040-060-port/docs/C2-ATTACK-MAP.md`) found that three of the numbers those
captures printed did not mean what they said. **None of them was a wrong measurement** —
every bucket total, every cycle span and every counter in those captures is still good.
They were labelling defects, which is a worse failure than a wrong number, because a
mislabelled number still looks like a number and arithmetic built on it still balances.

| # | v1 | v2 | consequence for a v1 capture |
|---|---|---|---|
| 1 | `probe_cyc` is the cost of an **enter/exit pair**, multiplied by `TRANSITIONS`, which counts each enter **and** each exit | `probe_cyc` is the cost of **one transition**, calibrated with the probes **out of line** | v1 over-priced the instrument by exactly **2×**. At the reported 82.65 % the subtraction demanded 43.8 G probe cycles from a bucket holding 28.0 G — arithmetically impossible. The honest figure is 41.33 %, and correcting it **moved `LOOP` from first place to fifth**. |
| 2 | `cpu_hz`/`wall_hz` scaled by `COUNTS_PER_SECOND`, a compile-time BSP constant frozen at 666.67 MHz | scaled by the rate core0 publishes after it retunes the PLL; `clk=cfg\|bsp` says which | on an 1100 MHz board every Hz, every duration and every **actual sample rate** in a v1 capture is low by exactly **1.65**. Multiply, or re-take. |
| 3 | counter 12 named `ATC_HIT`, incremented on `mmu_translate`'s success return — which a table walk also reaches, via `goto atc_retry` | counter 12 is `XLATE_OK` (what it always counted); a real `ATC_HIT` is **new id 19** | `ATC_HIT + ATC_MISS` was meaningless in v1 — a set summed with one of its own subsets. The walk rate v1 printed was always correct. |
| 4 | `TAIL` (id 8) is the whole run-loop tail: **20 % of guest time, no breakdown** | id 8 is `TAILADV` (the residue) with `TAILSAMP`/`TAILPOLL`/`TAILSPEC` at 11/12/13 | not a defect — an addition. v1's `TAIL` == v2's `TAILADV + TAILSAMP + TAILPOLL + TAILSPEC`, and the firmware prints that sum on a `[PROF] t` line so the two are directly comparable. |
| 5 | `cyc_span` stops at the last sample; `wall_ticks` runs on to the dump | both are stamped **at the dump** | v1's wrap cross-check fired on the operator's own latency: 91 s on one C1 capture, reported as a suspected missed `PMCCNTR` wrap that never happened. |

**Ids were never renumbered.** Two were renamed to say what they had always counted, four
were appended. The **sample record and its flag bits are unchanged** — a v1 decoder for
`S` lines is still correct, and it is the header and the `PROFD` block that moved.

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
Header dependencies **are** tracked now (`-MMD -MP`), but a `-D` still is not and cannot be,
which is what the separate directories are for: sharing one would let half the other
variant's objects survive a variant switch and the link would happily succeed. Note also
that **core0 (`Z3660/build`) is shared by all three images** — it is the same `Z3660.elf` in
every `BOOT*.BIN` — so a core0 change lands in all three at once.

> **The inert-echo trap: `PERFS` and `PROBES` reply on images that do not have them.**
>
> Those two console commands write `SHARED.perf_stats` / `SHARED.emu_probes` and print
> `PERF STATS ON` / `EMU PROBES OFF` — from **core0**, which cannot tell which core1 image
> is running. On `BOOT-lean.BIN` and `BOOT-prof.BIN` the instruments they gate are compiled
> **out**, so the mailbox bit changes, the console cheerfully confirms it, and **nothing
> whatsoever happens**. The same applies to the `prof_hz` / `prof_buckets` config keys on a
> non-profiling image, and to every `PROF*` command (see "Which image am I talking to?").
>
> This is a deliberate design — a config file is shared across a flash of any variant, and
> a key that made the default firmware refuse to boot would be far worse than one that
> quietly does nothing — but it means **a console echo is not evidence that an instrument
> is armed.** The only evidence is the boot line. `z3660_knobs_get()` is honest about it
> from core1's side: on a lean build it reports 0 whatever the mailbox says, because that
> is the truth about the loop. Core0 has no such read-back.
>
> Practical consequence for an A/B: when comparing lean against default via `PERFS`/
> `PROBES`, you are measuring the *runtime switches* on the **default** image only. On lean
> there is no switch left to throw — that is the entire point of the variant — and a sweep
> that flips them on lean is measuring nothing and will report a flat line as a finding.

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
stalls, while the probe work is register-heavy — so the time ratio is better than the
instruction ratio. **Do not quote an absolute instruction rate from this image, and do not
compare one to the lean figure.** Metal has now said by how much: the four-point A/B at the
end of this document measures the floor of this image at **−23.2 %** of guest throughput
with everything switched off, the sampler at a further **−1.4 %**, and the buckets at
**−38.2 %**.

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
| 8 | `TAILADV` | **v2**: the tail *residue* — `adjust_cycles()`, the cadence counter's own test, the PC advance. **In v1 id 8 was `TAIL` and meant the whole tail.** |
| 9 | `FAULT` | last probe transition → the run loop's `CATCH` |
| 10 | `PROF` | the profiler's own dump work, kept out of the rest |
| 11 | `TAILSAMP` | **v2**: the profiler's own sampler hook + the `INSNS` counters — *instrument cost, not interpreter cost* |
| 12 | `TAILPOLL` | **v2**: `check_uae_int_request()` when the service cadence lets it through |
| 13 | `TAILSPEC` | **v2**: `do_specialties()` — trace, `STOP`, mode change |

Time is **exclusive**: a translate inside a data read appears in `XLATE` and *not* also in
`READ`. `sum(buckets) == total measured cycles` by construction, and the host harness
asserts exactly that against an injected clock.

### The tail split (v2)

The C2 map measured the tail at **20.01 % of guest time and 110.8 ARM cycles per guest
instruction** — the most disproportionate figure in the whole table, for what ought to be
a PC advance and a cheap event test — and could not act on it, because a fifth of the
profile with no internal breakdown cannot be attacked without guessing which part of it to
attack. Ids 11–13 name the three identifiable pieces; whatever is left stays in id 8.

Two things about the split are worth knowing before reading a number out of it:

- **`TAILSAMP` is the instrument, not the interpreter.** It is the sampler tick and the
  `INSNS`/`INSNS_SUPER` increments. Bucketing it separately is the point: subtract it
  rather than rank it. In v1 it was charged to `LOOP`, so **no v1 capture could see it at
  all** and the 110.8 cycles never included it.
- **The split costs four extra phase transitions per guest instruction**, taking the
  per-instruction total from 8–14 to 12–18. The profiling image therefore distorts *more*
  in v2 than in v1. That is the trade: the shares inside the tail are unobtainable any
  other way, and shares survive the distortion — which is the whole reason the transition
  count is reported next to them.

`PROFD` prints `TAILADV + TAILSAMP + TAILPOLL + TAILSPEC` on its own `[PROF] t` line, so a
v2 capture can be laid beside a v1 one without the reader having to know which four ids to
add up.

### Counters

| id | name | | id | name |
|---|---|---|---|---|
| 0 | `INSNS` | | 10 | `DPAGE_WMISS` |
| 1 | `INSNS_SUPER` | | 11 | `XLATE` |
| 2 | `FETCH` | | 12 | `XLATE_OK` ← **v2 rename** |
| 3 | `READ` | | 13 | `ATC_MISS` |
| 4 | `WRITE` | | 14 | `MISALIGN_R` |
| 5 | `IPAGE_HIT` | | 15 | `MISALIGN_W` |
| 6 | `IPAGE_MISS` | | 16 | `FAULTS` |
| 7 | `DPAGE_RHIT` | | 17 | `TRANSITIONS` |
| 8 | `DPAGE_RMISS` | | 18 | `STACK_OVF` |
| 9 | `DPAGE_WHIT` | | 19 | `ATC_HIT` ← **v2, new** |

Appended after v2, **without a version bump** — no id below them changed meaning, the binary
header layout is untouched, and a v2 symbolizer reports an unknown id uninterpreted rather
than mis-labelled. Bumping would make every existing tool refuse a capture it can read
correctly.

| id | name | what it counts |
|---|---|---|
| 20 | `IFETCH_CALLS` | entries into the shared `FETCHOP`/`FETCHEX` bracket — **calls, not words** |
| 21 | `BLK_HIT` | block-idiom fast-path **chunks** serviced (C2 rung 0) |
| 22 | `BLK_INSNS` | guest instructions those chunks retired |
| 23 | `BLK_BYTES` | bytes zeroed / copied / flushed by them |
| 24 | `DOPC_HIT` | decoded-op cache: opcode + decode served from a hit (C2 rung 1) |
| 25 | `DOPC_MISS` | ... consulted and not found; the fetch still ran |
| 26 | `DOPC_EXT` | ... extension words served from the entry's single slot |
| 27 | `DOPC_INVAL` | ... whole-cache invalidations, all causes |

Two identities worth checking in any capture that has them: `IFETCH_CALLS == FETCH`
identically (all five bracket sites increment `FETCH` exactly once), and
`DOPC_HIT + DOPC_MISS == INSNS` when the decoded-op cache is on — every dispatch consults it
exactly once, which is what makes `DOPC_HIT / (DOPC_HIT + DOPC_MISS)` a hit rate. With the
cache off all four `DOPC_` counters are **exactly zero**: the lookup returns before it can
count a miss.

All 64-bit — a 32-bit counter overflows in about seven minutes of hot interpretation,
which is shorter than the boot profile this exists to take.

Three tiers of address translation are separately visible, which is the point:

- **tier 0** — the one-entry instruction page cache and 256-entry data page caches
  (`IPAGE_*` / `DPAGE_*`). A hit here means `mmu_translate` is never called.
- **tier 1** — the 4-way ATC inside `mmu_translate`. `XLATE` counts entries (== tier-0
  misses); `ATC_HIT` (id 19) counts the ones served **without a walk**.
- **tier 2** — the descriptor table walk (`ATC_MISS`, and the `WALK` bucket).

Identities, all checkable in a dump:

```
IPAGE_MISS + DPAGE_RMISS + DPAGE_WMISS  ==  XLATE      tier 0 feeds tier 1
ATC_HIT + ATC_MISS                      ==  XLATE      when no translate faulted; short by
                                                       exactly the faulting ones otherwise
true ATC hit rate = ATC_HIT / XLATE      walk rate = ATC_MISS / XLATE
```

`XLATE_OK` counts translates that returned an address. **In v1 this counter was called
`ATC_HIT`** and it sat on `mmu_translate`'s success return — which is reached both by an
ATC tag match *and*, through `goto atc_retry`, by every translate that had just walked the
tables. So it counted successful translates, carried no ATC information whatsoever, and
made `ATC_HIT + ATC_MISS` a sum of a set with one of its own subsets. The C2 audit caught
it only by noticing that `ATC_HIT + FAULTS == XLATE` held exactly across four independent
seven-digit spans. **The walk rate the v1 firmware printed was correct throughout**; it was
the hit counter beside it that was not.

`STACK_OVF` must be zero. A dump showing anything else is not trustworthy.

> **68030 caveats — and they are not the same caveat.**
>
> 1. **No tier-0 counters.** Upstream has no scaffolding for the page caches on the 030
>    side, and adding twelve branch edits to a fork of someone else's emulator on the path
>    that is not the optimisation target was not worth it. The rows are absent, not
>    zero-valued by accident.
> 2. **No `XLATE_OK`.** The 030 translate is a bare `mmu030_logical_is_in_atc()` called
>    from fourteen accessors, with no single success point to hang a counter on. Reads
>    zero; zero means absent.
> 3. **`XLATE` over-counts on the 030 miss path.** Those accessors call the ATC search,
>    get `-1`, walk the tables, and then call it *again* to pick up the refilled line — so
>    every 030 table walk costs two `XLATE` increments and one `ATC_HIT`. **The 030's hit
>    and walk rates are internally consistent but are not comparable to the 040's.**
> 4. `ATC_HIT` on the 030 was always genuine, which is why that site needed no edit when
>    the counter moved to id 19 — the defect was on the 040 side only.

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
   counter read, a subtract, a 64-bit accumulate and a stack push/pop, and there are 12–18
   transitions per emulated instruction (v2's tail split added four to v1's 8–14). The
   firmware **measures** this at arm time and reports it as `probe_cyc` alongside the
   `TRANSITIONS` count, so the inflation is computable and, if you care, subtractable per
   bucket by its share of transitions.

   **`probe_cyc` is per TRANSITION.** `TRANSITIONS` increments in `enter()` *and* in
   `exit()`, so the cost that multiplies it must be the cost of one of them. v1 calibrated
   a *pair*, and two numbers of the right type and the wrong unit multiply to a perfectly
   plausible answer that no arithmetic check ever objects to. It reported 82.65 % where
   the honest figure was 41.33 % — an over-subtraction large enough to demand more probe
   cycles from `LOOP` than `LOOP` contained, and to invert the top of the ranking the
   instrument exists to produce.

   **And it is priced out of line.** The real accessors reach `enter`/`exit` through a
   `bl`; v1's calibration inlined them, so the calibrated figure was an under-estimate of
   production cost by an unknown margin — and "unknown" was load-bearing, because `LOOP`'s
   corrected share falls to zero at a per-transition cost only 28 % above v1's number. The
   calibration now calls `__attribute__((noinline))` wrappers, so what is timed is a real
   call to a real out-of-line body. It is still an *estimate* (different register pressure,
   friendlier branch prediction than a real call site); what it is no longer is a *known*
   under-estimate.

   **And it is the MARGINAL cost of an armed transition, not the cost of the call that
   carries it.** This is the third unit error in the same number and it is the one the C2
   map ran into. The first v2 calibration timed the armed loop *alone*, charging the `bl`,
   the callee prologue, the `if (!bucket_on)` predicate, the epilogue and the `for` loop's
   own compare-and-branch to the probe, and reported **59 cyc/transition**. At 59 the
   subtraction is arithmetically impossible: the modelled probe cost exceeds what `LOOP`
   holds by 3.0 G cycles and what `TAILADV` holds by 1.2 G — 4.42 % of the whole span with
   nowhere to put it — and the symbolizer clamps two buckets and disowns every share
   derived from them. Sweeping the price finds `LOOP` exhausted at 50.59 and `TAILADV` at
   54.11, so 59 sits above the entire usable range, and above the 45.5–58 band predicted
   at the end of this document.

   It is a double count, and it follows from what the subtraction is *for*. Probe
   subtraction describes **the prof-build interpreter with its probe bodies removed** — it
   is explicitly not trying to describe the lean interpreter, which does not make these
   out-of-line accessor calls at all and whose separate cost is priced at −23.2 % under
   *What the profiling build costs* below. Every term in that list survives in the machine
   the subtraction leaves behind. Charging them as probe subtracts them once and leaves
   them there.

   So the calibration now times the **same loop twice**, differing in one bit
   (`z3660_prof_bucket_on`), and reports the difference over `2 × PAIRS`. Unarmed, the
   calls still happen, the branch is still predicted and the loop is still paid;
   `enter`/`exit` return at their first line, before the cycle read. What survives in the
   difference is the probe body alone — the `PMCCNTR` read, the 64-bit accumulate, the
   transition counter, the stack push/pop, the timestamp store — which is exactly the
   quantity `TRANSITIONS × probe_cyc` is meant to name. Both spans are now printed on the
   boot line:

       [PROF] probe calibration: armed <n> cyc, unarmed <n> cyc, 1024 pairs -> <n> cyc/transition marginal

   so the subtraction can be checked rather than taken on faith. The host harness asserts
   the arithmetic exactly against the injectable clock (`test_prof_probe_calibration`):
   armed span `= STEP × (2·PAIRS + 1)`, unarmed `= STEP`, difference over `2·PAIRS` `=
   STEP` on the nose.

   **A prediction, stated in advance so that it can be wrong.** The terms removed are one
   call, one predicate and three or four loop instructions, so the reported figure should
   fall by roughly 8–14 cycles — from 59 into the 45–51 region, at or below the 50.59 at
   which `LOOP` is exhausted, and close to the **50** the C2 map independently picked as
   the highest price at which every bucket stays positive. If instead the figure barely
   moves, the price was never the problem and the *landing model* is; that is the other
   half of map §10.3 and it is **not** settled here. (The map's leading candidate for it —
   that the shared ifetch bracket is entered fewer times than `FETCH` counts — is refuted
   by the source: all five `Z3660_PROF_ENTER_IFETCH` sites increment `FETCH` exactly once,
   so `IFETCH_CALLS == FETCH` identically. The new counter id 20 makes a capture say so.)
3. **A guest page fault is a C++ throw** and unwinds past every pending probe exit. The
   `CATCH` charges the abandoned span to `FAULT` and resets the phase stack. `FAULT` is
   therefore "the fault path plus a fragment of whatever stage it interrupted" —
   deliberately not split finer, because the split would be a guess.
4. **`PMCCNTR` is 32 bits** and wraps every ~3.9 s at 1.1 GHz. Bucket spans are bounded by
   one run-loop iteration, so modular subtraction is exact. The long-run cycle base is
   extended to 64 bits at each sample **and at every profiler command poll** — i.e. from
   `do_specialties()`, which a running guest reaches on every interrupt, so the base stays
   fresh even with the sampler off, which is the state a bucket-only run leaves it in. If
   core1 leaves the run loop for longer than one wrap, the base still loses 2³² cycles
   silently. **That is why the dump reports both the PMU span and an independent ARM
   global-timer span** — if they disagree by more than a percent, a wrap was missed.

   **v2 stamps `cyc_span` at dump time.** This was a choice between documenting a bias and
   removing it, and removing it won. v1 reported the cycle base as it stood after the last
   *sample* while `wall_ticks` ran on to the *dump*, so the two spans covered different
   intervals and the cross-check above fired on the gap between a workload ending and a
   human typing `PROFR` — 91 s on one C1 capture, reported as a suspected missed wrap that
   never happened. A cross-check that cries wolf on operator latency is one nobody will
   believe on the day it is right. Both spans now end at the dump.
5. **The ARM clock's *ratio* is measured; its *absolute rate* comes from the running
   config.** These are two different claims and v1 conflated them, at a cost of 65 %.

   What the calibration can actually measure is the ratio of `PMCCNTR` to the ARM global
   timer. On this part that ratio is a **fixed 2:1 in silicon** (CPU runs at `CPU_6x4x`,
   the timer at `CPU_3x2x`), so it always comes back ~2.0 and says nothing about the PLL.
   v1 took the absolute rate from `COUNTS_PER_SECOND` — a **compile-time** BSP constant
   baked at the 666.67 MHz default — while `arm_frequency` is a config key that
   `ps7_init_custom()` applies to the PLL at boot. On the C1 rig, configured for 1100 MHz,
   the constant was wrong by 1.65× and every rate, duration and *actual sample rate* in
   that session's captures was low by exactly that factor.

   **Why nothing caught it, which is the part worth remembering.** The wrap cross-check in
   note 4 compares `cyc_span` against `wall_ticks / wall_hz × cpu_hz`. Because CPU:timer is
   a fixed 2:1, `cyc_span` and `wall_ticks` scale *together* and the erroneous constant
   cancels out of the ratio: **the check passes to three decimal places while the clock is
   65 % wrong.** A wrap check is not a clock check. The only tell was a boot line reading
   666.67 MHz seven seconds after the board's own log said 1100.

   So core0 — which resolves `arm_frequency` and retunes the PLL — publishes the resulting
   global-timer rate in `SHARED.arm_wall_hz`, and the profiler scales its measured ratio by
   that. The header reports the rate **and its source** (`clk=cfg` or `clk=bsp`), so a
   future stale constant is visible in the capture rather than inferred from a boot log two
   minutes earlier. `clk=bsp` also prints a warning line at boot.

---

## Controls

### Serial console

| command | effect |
|---|---|
| `PROF` | cycles the sampler: off → 250 → 1000 → 4000 Hz → off. Arming **zeroes** buckets, counters and ring. **If the armed rate is not one of those, the press STOPS instead** — see below. |
| `PROF <hz>` | **v2**: arms at an explicit rate, 1–20000 (also zeroes). Prints the wrap-free window in seconds. `PROF 0` is a stop. |
| `PROFO` / `PROF OFF` | **v2**: freezes sampler **and** buckets, keeping every byte. The lossless stop. |
| `PROFB` | toggles the stage buckets. |
| `PROFD` | dumps stage buckets + counters (~40 lines). |
| `PROFR` | dumps the newest 4096 samples (~90 KB, about 8 s at 115200). |
| `PROF RING` | dumps the whole ring: up to 65535 samples, ~1.4 MB, about two minutes. |
| `PROFZ` | zeroes buckets, counters and ring **without** disarming. |

### Stopping without destroying (v2)

**Every rung of the `PROF` ladder arms, and arming zeroes.** That is harmless while the
ladder is the only way in — you always know what the next press will destroy. It is not
harmless for the one capture the console structurally *cannot* take: the boot profile,
armed from `z3660cfg.txt` at whatever rate the file says.

The C1 session armed at `prof_hz 200`, and the first `PROF` press mapped 200 into the
`< 1000` arm and re-armed at 1000 Hz — wiping a multi-minute boot capture that could only
be retaken by cold-booting the board. There was no keystroke that meant "stop".

Two changes, both in v2:

- **`PROF OFF`** (short form `PROFO`) is a direct, always-available lossless stop from any
  armed state whatsoever. This is the command to reach for the moment a workload ends.
- **A bare `PROF` at an off-ladder rate stops rather than steps up.** A rate that is not
  0/250/1000/4000 did not come from the ladder, so it was set deliberately — from the
  config file or from `PROF <hz>` — and the press refuses to arm over it, saying so on the
  console. The ladder itself is unchanged for anyone who started on it.

The rule lives in `Z3660/src/prof_config.h` (`z3660_prof_cfg_next_rate()`), not in the
console, so the host guard can assert every transition; the console code that runs it needs
the Xilinx BSP and cannot be compiled on the host at all.

### Which image am I talking to?

Core0 cannot tell which core1 image is running — there is no "profiler present" bit, and
adding one would mean another hand-mirrored `SHARED` field. So these commands report what
they **asked for**. The discriminator is the boot line the profiling image prints:

```
[PROF] profiling build v2: ARM clock 1100000000 Hz (clk=cfg, PMU:wall 2.00), probe 46 cyc/transition (92 cyc/pair), ring 65536 x 8 B
```

No `[PROF]` reply to a command means core1 is not the profiling image.

Every unit on that line is spelled out and the clock's source is on it, because both were
scars: v1 printed `enter/exit pair 91 cyc` next to a per-transition count, and
`ARM clock 666667585 Hz (measured)` seven seconds after the same console printed
`ARM PLL successfully configured for 1100 MHz`. Neither was catchable from the line itself,
and the line is the only place a reader looks. **`clk=bsp` on that line means the clock is
the compile-time constant and every Hz below it is suspect** — the firmware prints a
warning of its own when that happens.

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

**Version 2.** A symbolizer **must refuse a version it does not know** rather than guess —
and since the magic encodes the version (`Z3P` + digit), a tool that only greps the magic
refuses too. See [v1 → v2](#v1--v2-what-moved-and-why) for what changed and why.

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
| 7:4 | reserved, **always 0** — mask them off |
| 15:8 | `WEIGHT` — tick periods this sample stands for, 1–255, saturating |

**The record and its flags are byte-for-byte identical in v1 and v2.** A v1 decoder for
`S` lines needs no change; only the header and the `PROFD` block moved.

### Ring header — 80 bytes

`magic`(4) `version`(4) `rec_size`(4) `rec_count`(4) `ring_entries`(4) `hz`(4)
`period_cyc`(4) `cpu_hz`(4) — then, 8-byte aligned at offset 32 — `samples`(8) `drops`(8)
`cyc_span`(8) `wall_ticks`(8) — then `wall_hz`(4) `build_flags`(4) `probe_cyc`(4)
`clk_src`(4).

v2 spent v1's two `reserved` words rather than resizing; **the header is still 80 bytes**
and every earlier field is at the same offset.

- `build_flags`: bit 0 = run-loop probes compiled in, bit 1 = perf accumulator compiled in,
  bit 2 = profiler compiled in. The shipped profiling combination is **`0x04`**.
- `probe_cyc` (**v2**, offset 72): ARM cycles per **one phase transition**. Multiply by
  `TRANSITIONS`, never by half of it.
- `clk_src` (**v2**, offset 76): `0` = `COUNTS_PER_SECOND`, the compile-time BSP constant;
  `1` = the rate core0 published after retuning the PLL. **`0` means `cpu_hz` and `wall_hz`
  may be stale** and every derived rate with them.
- `cpu_hz` / `wall_hz` are the **runtime** clock in v2 (see bias note 5).
- `cyc_span` and `wall_ticks` both end **at the dump** in v2, so they cover the same
  interval and the cross-check below compares like with like.

`samples` is the total taken; `drops` is how many the ring overwrote. **`drops > 0` means
the dump shows only the tail of the run** — a wrapped ring presented as a whole-run profile
is the single easiest way to get a wrong answer out of this, and it is exactly how two of
the three C1 workload captures ended up profiling the scheduler idle loop.

### Serial rendering

Version 2. Fixed-width lowercase hex, no `0x` prefix, oldest first.

```
[PROF] ring hdr magic=Z3P2 version=2 rec_size=8 rec_count=4096 ring_entries=65536 hz=1000 period_cyc=1100000 cpu_hz=1100000000
[PROF] ring hdr samples=124311 drops=58775 cyc_span=41234567890 wall_ticks=20617283945 wall_hz=550000000 build=0x04 probe_cyc=46 clk=cfg
[PROF] ring begin
S 08001234 4e71 0103
S 08001236 6100 0103
...
[PROF] ring end n=4096
```

Changes from v1, all on the second header line and all additive except the magic:
`magic=Z3P1`→`Z3P2`, `version=1`→`2`, `wall_hz`/`cpu_hz` now runtime, and two new
key=value pairs `probe_cyc=` (per transition) and `clk=cfg|bsp`. Sample lines unchanged.

Fields are fixed width so a symbolizer can slice by column and a **truncated serial capture
is detectable rather than merely mis-parsed**. `rec_count` in the header must equal the `n`
on the `ring end` line; if it does not, the capture lost lines.

The stats dump (`PROFD`) is line-oriented in the same way: `[PROF] b <id> <NAME> cyc=<n>
<pct>%` per bucket, one `[PROF] t` whole-tail rollup, `[PROF] c <id> <NAME> <n>` per
counter, then a few `[PROF] r` derived rates. Its header line carries
`probe_cyc_per_transition=` and `clk=` too.

> **Capture-path defects a symbolizer should tolerate, not refuse.** The C2 audit found
> that `prof-symbolize.py` rejected all seven C1 captures for two reasons that are in the
> *capture path*, not in the records: the KVM logger prefixes every line with `HH:MM:SS `,
> and each ring dump's first header line loses its literal `[PROF] ` prefix because the
> console echo of the request and the firmware's first header line collide in the one UART.
> All `key=value` payload survives in both cases, magic included. A tool should strip a
> line prefix and repair-with-a-warning a header line whose payload is complete and whose
> magic is present, rather than refusing a capture that is provably intact. The firmware
> cannot fix either from its end; the second one is a shared-UART property (see the PSTAT
> law below).

---

## What the symbolizer has to do

Symbolization is the port lane's business; the firmware emits raw records only. The
minimum correct implementation:

1. **Check `magic` and `version`.** Refuse anything else. `Z3P2` / `2` today.
2. **Weight every sample by `WEIGHT`.** A histogram that counts samples instead of weight
   is wrong in exactly the places that matter.
3. **Report `drops`.** If non-zero, say plainly that the profile covers only the last
   `rec_count` samples, not the run.
4. **Cross-check `cyc_span` against `wall_ticks / wall_hz × cpu_hz`.** A divergence over a
   percent means a `PMCCNTR` wrap was missed and cycle totals are short by a multiple of
   2³². **This check cannot detect a wrong clock** — CPU:global-timer is a fixed 2:1, so
   both sides scale together and any error in the absolute rate cancels out of the ratio.
   Check `clk_src` for that, separately, and say so when it is `bsp`.
4b. **Price the probe per transition.** `probe = TRANSITIONS × probe_cyc`, with `probe_cyc`
   read from the header (v2) — not halved, not doubled. If the modelled probe exceeds
   `total_cyc`, do **not** clamp silently: report it. A clamp is how v1's 2× over-pricing
   produced a plausible table instead of an error.
4c. **Sum the tail from four ids.** `TAILADV + TAILSAMP + TAILPOLL + TAILSPEC` is what v1
   called `TAIL`. `TAILSAMP` is instrument cost — subtract it rather than ranking it.
4d. **Use `ATC_HIT` (id 19) for the ATC hit rate, never id 12.** Id 12 is `XLATE_OK` and
   counts successful translates. `ATC_HIT + ATC_MISS == XLATE` when nothing faulted.
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

Flash `BOOT-prof.BIN`, take the profiles below, then flash `BOOT-lean.BIN` back. Capture
the serial console to a file throughout — the dumps are long and are meant to be read by a
script, not a human.

**Before anything: record the boot line.** `[PROF] profiling build v2: ARM clock … Hz
(clk=…), probe … cyc/transition` is the calibration every later number depends on.
**If it says `clk=bsp`, stop and find out why** — every rate below it is then scaled by a
compile-time constant that the board may have retuned away from.

### The two operating laws this lane learned the hard way

**1. A ring must CONTAIN its workload.** The wrap-free window is `65536 / hz` seconds, and
`PROF <hz>` prints it when you arm. Two of the three C1 workload captures wrapped — 86 %
and 93 % of their samples overwritten — and the retained tail landed *after* the benchmark
finished, on the scheduler idle loop. They were not noisy profiles of dhrystone; they were
clean profiles of an idle machine, and `swtch`+`idle` at 62 % and 72 % is what gave it
away. The `MANIFEST` note that "dhry is a uniform loop so the tail is representative" was
refuted by its own data: **the tail is not the loop, it is the machine idling after it.**

So: arm at the **lowest** rate that still resolves the question, size it against the
workload's duration, `PROF OFF` the moment the workload ends, and check `drops=0` before
believing any share. At the corrected clock, nominal 250 Hz gives 262 s of wrap-free
history — comfortably more than a dhrystone run — and a boot fits at 200 Hz.

**2. Turn `PSTAT` off before any long dump.** Both cores print through **one UART behind
one semaphore** (`SHARED.uart_semaphore`, taken and released per `printf`). A full ring
dump is 65 538 sequential acquisitions over about two minutes. Core0's periodic piscsi and
`C0STAT` reports contend for the same semaphore for the whole of it — and core0 *spins*
while it waits, so the cooperative loop that services piscsi is starved for as long as the
dump runs. The visible damage is interleaved lines that break the fixed-width grammar; the
invisible damage is guest I/O latency during the dump. `PSTAT` off, dump, `PSTAT` back on.
This is also why a ring dump's first header line arrives with its `[PROF] ` prefix eaten by
the console echo of the command that requested it.

### The captures

1. **Boot profile.** Set `prof_hz 200` and `prof_buckets NO` in `z3660cfg.txt`, cold boot,
   and once AMIX is at a login prompt run **`PROF OFF`** (freezes it losslessly — a bare
   `PROF` at 200 Hz now stops rather than re-arming, but say what you mean), then `PROFD`
   and `PROF RING`. 200 Hz across a multi-minute boot fits inside the 65536-sample ring;
   confirm with `drops=0`. Answers "which guest code dominates a boot".
2. **Dhrystone profile.** `PROF 250`, run the benchmark, **`PROF OFF` as soon as it
   prints its result**, then `PROFD` and `PROF RING`. 250 Hz holds 262 s, which contains a
   dhrystone run several times over — the C1 session used 4000 Hz, got 9.9 s of window, and
   captured the idle loop that followed. Do not repeat that.
3. **Stage attribution.** `PROFB` on, `PROFZ`, re-run the same dhrystone, `PROF OFF`,
   `PROFD`. Expect the reported instruction rate to be far below the lean figure — that is
   the instrument, not the interpreter. Read the per-bucket **shares**, the whole-tail
   rollup and the `probe overhead inside the totals` line, and take the counters
   (`IPAGE_*`, `DPAGE_*`, `ATC_*`, `MISALIGN_*`) from this run: they are exact regardless
   of the timing distortion.
4. **Install-workload profile** (optional, and the one most likely to surprise): repeat 2
   during a `pkgadd` or a large file copy, at a rate sized for how long that actually takes
   — an I/O workload runs for minutes, so 250 Hz is already marginal and the `WEIGHT`
   histogram matters more than the rate. It is the workload with real device traffic, so it
   is where `WEIGHT` and the tail buckets earn their keep: a mailbox round trip is invisible
   to instruction counting and obvious to a wall-clock sampler.

**The overhead A/B — measured 2026-08-22, no longer owed.** A4000 + Z3660, preset 7,
`amix_ram 128`, `service_cadence 1`, ARM 1100 MHz, AMIX on piscsi `ced0s1`, fieldkit
dhrystone. `PSTAT` off at every point (see the shared-UART law above), applied uniformly so
the deltas are clean.

| point | dhry/s | vs lean | vs previous |
|---|---|---|---|
| `BOOT-lean.BIN` — production | **4500** | — | — |
| `BOOT-prof.BIN`, nothing armed | 3457 | −23.2 % | −23.2 % |
| + sampler at 1 kHz | 3409 | −24.2 % | −1.4 % |
| + stage buckets as well | 2106 | −53.2 % | −38.2 % |

Supplementary, same boot: sampler at 4 kHz → 3418; buckets only, sampler off → 2132.

Three separate questions live in those four points and they are routinely conflated:

- **What compiling the profiler in costs: −23.2 %**, paid with *everything switched off*,
  because the instrumented accessors make three out-of-line calls whichever way `PROFB` is
  set (bias note 2b). It is the largest cost after the buckets and the one a reader is most
  likely to forget, precisely because nothing is "on".
- **What running the sampler costs: −1.4 %.** *This is the number that matters most*, being
  the one that says whether the guest-PC map can be read as wall time. At 1.4 % it can —
  and the 4 kHz point at −1.1 % against idle says the tick is genuinely cheap, as designed.
- **What the buckets cost: −38.2 %.** They nearly halve guest throughput. Read the bucket
  *shares*; never quote an instruction rate out of a bucket run.

The lean 4500 reproduced the C3b cadence-1 figures (4492 / 4506 / 4498) to within 0.1 % on
a different flash, which is what makes the deltas trustworthy.

> **Two v1 numbers in that session's output to disbelieve, both now fixed.** The
> stage-attribution run printed `probe overhead inside the totals: … = 82.65 % (966080190
> transitions × 91 cyc)`. The 91 was cycles per *pair* against a per-*transition* count, so
> the true share was 41.33 %; and the 91 itself was measured with the probes *inlined*,
> where production calls them, so it was also an under-estimate of the real per-call price.
> v2 reports cycles per transition, calibrated out of line — expect a number **above** 45.5
> and **below** roughly 58, the point at which `LOOP`'s corrected share reaches zero.
>
> **The first v2 metal calibration came back at 59 — above that band, and above the 50.59
> at which the C2 span exhausts `LOOP` — and the band was right.** The calibration was
> timing the armed loop alone and charging the call, the predicate and the loop
> scaffolding to the probe, all of which stay in the machine the subtraction leaves
> behind. It now differences the armed loop against the identical unarmed one; see bias
> note 2's *marginal cost* paragraph for the arithmetic and for the pre-registered
> expectation (45–51) that the next metal capture will confirm or refute.
> Also note that both the v1 header example and this document once showed `11` cyc where
> metal measured `91`: anyone sizing bucket inflation from the documented example would
> have understated it ~8×. The examples in this file are now taken from real metal output.
