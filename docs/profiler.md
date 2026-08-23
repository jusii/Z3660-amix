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
not estimated. **Re-measured 2026-08-23 on the rung-1d build** — the figures this table
carried before were from the C1-era loop, three rungs of optimisation ago, and
`m68k_run_mmu040` in particular had grown 5× since. Instruction counts exclude each symbol's
literal pool, so they are instructions and not words:

| | lean | profiling | delta |
|---|---|---|---|
| `m68k_run_mmu040` | 552 | 689 | +137 |
| `m68k_run_mmu030` | 187 | 253 | +66 |
| `mmu_translate` | 194 | 215 | +21 |
| `uae_mmu040_get_long` | 68 | 90 | +22 |
| `uae_mmu040_put_long` | 75 | 96 | +21 |
| `uae_mmu040_get_iword` | 64 | 95 | +31 |
| `z3660_dopc_fill` | 60 | 60 | **0** — the fill is bracketed at its *call site*, so the function itself is untouched |
| `z3660_prof_enter` / `_exit` / `_count` | — | 52 / 45 / 9 | (out of line) |

**Rung 1d's own share of that `+137`, isolated by building the profiling image with and
without it:** `m68k_run_mmu040` goes 687 → 689 instructions, and its `bl z3660_prof_enter`
sites go **6 → 9** (one each for `DOPCFIND`, `DOPCFILL`, `BLKREC`) with the matching exits
5 → 11 (gcc duplicates exits into branch tails; the count is code size, not per-dispatch
cost). Net **+2 instructions of straight-line code** — the register setup for three more
calls was absorbed by re-scheduling. **The per-dispatch cost that actually matters is the
transitions, not the instructions, and the dump prints it** (see "The LOOP split" below).

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
| 0 | `LOOPRES` | **rung 1d**: the dispatch loop's *residue* — run-loop bookkeeping outside every named stage. **In v2 id 0 was printed as `LOOP` and meant the whole of it.** |
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
| 14 | `DOPCFIND` | **rung 1d**: the decoded-op cache **lookup** — set index, generation compare, tag compares, and the extension-window arm/disarm. Every dispatch. |
| 15 | `DOPCFILL` | **rung 1d**: the decoded-op cache **fill**. The miss path only. |
| 16 | `BLKREC` | **rung 1d**: `z3660_blk_run()`, rung 0's block-idiom recognizer — the residual test, and the serviced chunk when it fires |

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

### The LOOP split (C2 rung 1d) — and why id 0 is now called `LOOPRES`

Same move, one bucket over, against a bucket that had become larger than the tail ever was.

C2 map v3 §15 measured `LOOP` at **26.38–26.54 %** of the interpreter — with per-bucket
spans, so a 0.31-point bracket and the best-measured share on the page — **ranked it first,
and then refused to give it a reclaim estimate**: *"a reclaim fraction for a 26 % bucket
whose contents have never been enumerated would be a guess dressed as a number."* §19 item 4
asks for one probe bracket around the decoded-op lookup and calls it *"the highest-value
instrument change available, in the same sense the tail split was in v1"*. Ids 14–16 are
that bracket and two more beside it.

**The hypothesis they exist to settle** is map §15.4's. Between the pre-rung-1 and rung-1b
sessions `FETCHOP` lost 8.7 points of raw share while `LOOP` gained 5.4 — so some of rung 1's
win may be **re-attribution rather than saving**: with the cache on a hit never enters
`FETCHOP` at all, and the cache's own hit path is in the run loop and was not bracketed. If
that is right, `LOOP` *contains* the thing rung 1 added, and attacking `LOOP` means attacking
rung 1's hit path. `DOPCFIND` decides it in one capture.

The four parts, in the order the loop runs them:

| id | name | what it is |
|---|---|---|
| 14 | `DOPCFIND` | `z3660_dopc_find()`: the set index, the generation compare, up to four tag compares — and on a hit, publishing the opcode and arming the extension-word window; on a miss, disarming it. Entered on **every** dispatch, and **closed before the miss path's opcode fetch**, so the fetch stays in `FETCHOP` where every earlier capture has it. |
| 15 | `DOPCFILL` | `z3660_dopc_fill()`: the set reclaim, the way choice, the entry publish. Entered once per **miss** — so a hit pays two transitions for this split, not four. |
| 16 | `BLKREC` | `z3660_blk_run()`. Entered only when the cached idiom verdict did not already veto the call. |
| 0 | `LOOPRES` | the residue: the flag snapshot, `mmu_restart`, `m68k_getpc()`, `do_cycles()`, `count_instr()`, the extension-lookahead reachability test, the cached idiom verdict's own test and its write-back, the handler selection and the `for(;;)` back edge. |

**Four identities a capture can check**, and they are what makes these brackets worth their
cost. Neither the lookup nor the fill touches guest memory, so neither *function* can throw
— which is why the first two are *exact*, and why the firmware **warns** rather than notes
when they fail:

```
tcnt[DOPCFIND] == DOPC_HIT + DOPC_MISS      cache ON: one lookup per dispatch
tcnt[DOPCFIND] == INSNS + FAULTS            cache ON, via the dispatch identity below
tcnt[DOPCFILL] == DOPC_MISS - (misses whose opcode fetch or extension lookahead faulted)
tcnt[BLKREC]   <= tcnt[DOPCFIND] + (BLKREC's own nested spans)
```

> **The third identity needed a fault term, and the reason is that a bracketed function
> not throwing is the wrong property.** What matters is whether the **interval the identity
> spans** can throw, and this one can. `z3660_dopc_find()` counts `DOPC_MISS` and the
> `DOPCFIND` bracket **exits before the fetch** (`newcpu.cpp`, the exit at the end of the
> lookup); the miss arm then runs `x_prefetch(0)` — an MMU-translated opcode fetch — and an
> extension lookahead, either of which can fault; and only after both does the miss reach
> `z3660_dopc_fill()` and the `DOPCFILL` bracket. **A miss whose opcode fetch faults is
> counted as a miss and never reaches the fill**, so the fill bracket runs *short* of
> `DOPC_MISS` by exactly that number. It is bounded by `FAULTS` and, on the 2026-08-23
> captures, sat at 1349 against `FAULTS` 3632 (cadence 1) and 1348 against 3627 (cadence 4)
> — a stable 37.1 % / 37.2 %, reproducing to within one count on two cadences. **The
> firmware's warning is therefore correct in form and expected in substance on any capture
> with faults in it**: read the shortfall against `FAULTS`, and treat a shortfall *larger*
> than `FAULTS` as the defect. Same class and same shape of fix as `== INSNS + FAULTS`.

> **The fourth identity was stated about calls and compared over spans, and a cache-off
> capture surfaced it.** `tcnt[BLKREC] <= tcnt[DOPCFIND]` reads as "at most one recognizer
> call per dispatch", which is true — but `tcnt[]` counts **spans**, and `BLKREC` is the one
> bracket this document already calls impure: on a fire it holds the whole serviced chunk,
> whose guest stores nest through `WRITE`, and **every nested entry closes a span and opens
> another**. So `BLKREC`'s span count exceeds its call count by its nesting, and the
> comparison is not the one the sentence makes. On `51-CACHEOFF-profd.txt` it **failed**
> correctly — 75 914 707 against 67 689 758, an excess of 8 224 949 over 41 025 chunks =
> **200 nested spans per chunk against 308 guest instructions per chunk**, which is the
> right order. It passes on cache-**on** dumps only by accident of the dispatch count being
> ~4× larger there. **The call-level claim still holds and is still worth checking; it just
> cannot be checked by comparing these two span counts.** `BLK_HIT` is the call-level
> quantity for the fire arm.

**With the cache OFF all three change shape, and none of it is a defect.** The lookup returns
before it counts, so `DOPC_HIT + DOPC_MISS` is 0 while `DOPCFIND` goes on being entered every
pass; and `DOPCFILL`, whose bracket sits on the now-universal miss arm, prices the fill's own
switch test on every dispatch instead of on every miss. An off arm reads those two buckets as
the price of the two switch tests — a real number, and not the one a cache-on capture reports.
The dump says so on a `[PROF] l note:` line rather than warning.

> **`BLKREC` is the one that is not pure, and it is bracketed anyway.** On a **decline** —
> map §19.4's "recognizer's residual test", and the overwhelmingly common case — the bucket
> holds three masked compares and a return. On a **fire** it holds the whole serviced chunk,
> because a bracket cannot know which it will be until the call returns. `BLK_HIT` and
> `BLK_INSNS` separate the two in the same dump, and on a workload with `BLK_HIT == 0` the
> bucket *is* the residual test. `z3660_blk.h`'s objection to bucketing the fast path — *"the
> fast path's entire value is that it does not pay per-instruction machinery"* — survives
> intact, because this bracket is per **call**: a chunk of 256 guest instructions pays two
> transitions in total, not 512.

**What the split costs, and the firmware measures it rather than asserting it here.** Two
transitions per dispatch for `DOPCFIND`, two per miss for `DOPCFILL`, two per recognizer call
for `BLKREC` — so between two and six added to v2's twelve-to-eighteen. `PROFD` prints the
total on its own line, computed from the three buckets' own spans:

```
[PROF] l whole loop (LOOPRES+DOPCFIND+DOPCFILL+BLKREC) cyc=<n> <pct>% -- this is what v2 reported as LOOP
[PROF] l split cost: <n> transitions (<pct>% of <total>) -- 2 per span of the three; a capture taken before rung 1d has none of them
```

so a capture can be re-priced *as if this decomposition had not been taken* by subtracting
that figure from `TRANSITIONS`. The first line is the v2-comparable quantity, and it exists
for the same reason `[PROF] t` does: the map quotes `LOOP` at 26.4 % and id 0 no longer means
that, so the number a reader would otherwise form by habit is printed outright.

**The lean loop is unchanged, and that is measured rather than argued.** One thing rung 1d
had to move in *lean* source: the two extension-slot locals used to be declared after the
lookup, and bracketing the lookup would have put their two zero-stores inside `DOPCFIND` —
roughly a sixth of a bucket whose whole content is a generation compare and four tag
compares. They are hoisted two lines instead, and neither is readable by
`z3660_dopc_find()`, so the reorder cannot be observed. Checked, not assumed:

| | result |
|---|---|
| host `newcpu.o`, `-O0`, lean and default | identical instruction multiset; the two stores change position and **nothing else differs** |
| **ARM `Z3660_emu.elf`, `-Os`, lean — `m68k_run_mmu040`** | **byte-for-byte identical, at the same addresses** (552 instructions before and after) |
| ARM lean `.text`, whole image | **one** instruction differs: a `movw r2, #6286` → `#6351` — the `__LINE__` operand of `m68k_go`'s *"Bug! m68k_go is not reentrant"* `abort()` path |

At `-Os` the scheduler put the two stores back where they were. The lean `BOOT-lean.BIN`
checksum still moves, and only because the **bucket name table is compiled into every
variant** by design (it is what makes a dump self-describing) — `LOOPRES` plus three names is
rodata that no lean code path reads.

**The version is deliberately not bumped**, and the reasoning is the tail split's. Ids 14–16
are a pure append — the binary header and the sample record are untouched, every id 0–13 sits
where it did, and a v2 symbolizer reports an id past the end of its table uninterpreted rather
than mis-labelled. What *does* move is what id 0 means, and that is handled the way v2 handled
`TAIL`: **by moving the name with the meaning.** A dump that says `LOOP` is a v2 dump and holds
the whole dispatch loop; one that says `LOOPRES` holds the residue. Bumping would instead make
every existing tool refuse a capture it can read correctly, and the capture session that needs
this decomposition is the next one. (The *enum symbol* stays `Z3660_PROF_B_LOOP`, exactly as
`Z3660_PROF_B_TAIL` prints as `TAILADV` — the phase machine's floor, the loop reset and the
unwind recovery all name it, and renaming them would say nothing new.)

### Per-bucket transition spans — the `[PROF] s` lines (C2 rung 1b)

The successor to `IFETCH_CALLS`, and the answer to the "subtract per bucket by its share of
transitions" instruction the dump has always given without supplying the shares.

```
[PROF] === per-bucket spans and the corrected shares (price 32 cyc/span; sweep to the boot line's in-bucket figure) ===
[PROF] s ceiling: min_b(acc/spans) = 59 cyc/span, bound by TAILADV -- the price above which a bucket's subtraction goes negative
[PROF] s WARNING: the boot line's in-bucket end 68 is ABOVE this capture's ceiling 59 and is arithmetically impossible. Sweep [57, 59]; do NOT sweep to 68.
[PROF] s 0  LOOP     spans=203419144    corrected_cyc=15194182811     23.73%
...
[PROF] s -- corrected_total=64034387415  spans=851345830  (must equal TRANSITIONS 851345830 -- ...)
```

The `ceiling` line is 2c's; read it before the shares, not after. It is the only line in the
dump that can tell you the printed bracket's upper end is unusable.

**What a "span" is, precisely** — a counter wired to the wrong quantity is worse than none.
Every transition ends one span and begins the next. The span that ends is charged to
`z3660_prof_cur`, and so is one probe's worth of the probe machinery: the *tail* of the probe
that opened the span plus the *head* of the probe that closed it. So the right divisor for a
bucket is the number of **spans charged to it**, and `spans` is incremented at exactly the two
sites that do the charging. Note that this is *not* "calls into the bucket": entering a nested
stage closes a span just as exiting does, so a `FETCHOP` with a translate inside it is charged
twice.

Two identities follow, both asserted in the host harness against a hand-driven clock:

```
sum(spans)                ==  TRANSITIONS
corrected cyc for b       ==  acc[b] − spans[b] × probe_cyc
```

The second is the point: it makes the global correction (`total − TRANSITIONS × probe_cyc`)
**decompose exactly**, so no bucket's corrected share can disagree with the total's. The
firmware prices the `s` lines at the same `probe_cyc` the aggregate line uses, for that reason.
The boot line's `[marginal, in-bucket]` bracket still applies and still has to be swept by hand
— it is a range, and printing one end of it as if it were the answer is the error this
instrument exists to stop.

#### ★ Use the spans. The `2 × IFETCH_CALLS` method they replace is REFUTED, not merely bettered

Before the `s` block existed there was no per-bucket transition count except for the ifetch
bracket, so rung 1's stage analysis assumed the two ifetch buckets carried `2 × IFETCH_CALLS`
transitions — one enter and one exit per call. **Metal 2026-08-23 measured both quantities on
the same capture and they are not close:**

| | transitions charged to the ifetch buckets | share of all transitions |
|---|---|---|
| assumed, `2 × IFETCH_CALLS` | 99 049 270 | 12.37 % |
| **measured spans** | **50 194 621** | **6.27 %** |

**The assumption over-charges the fetch buckets by 1.97×**, so every corrected share derived
from it was biased **low** — rung 1's corrected `FETCHOP + FETCHEX` of 11.27 % @32 / 10.48 % @43
included.

**The mechanism is the decoded-op cache itself: with the cache ON, a hit never enters
`FETCHOP` at all.** `FETCHOP spans = 15 547 429` against `DOPC_MISS = 15 436 987` — the bucket
is entered once per **miss**, not once per fetch. `IFETCH_CALLS == FETCH` is still exactly true
(all five bracket sites increment `FETCH` once); what is false is that `FETCH` predicts bucket
*entries* once something upstream of the bucket answers fetches without entering it. Rung 1's
own instrument could not have seen this, which is why the spans exist.

So: **take per-bucket transition counts from the `s` block, never from a counter that merely
correlates with them.** With them, `LOOP` — the bucket the whole probe-pricing argument turns
on — becomes a number:

| | corrected `LOOP` share |
|---|---|
| rung 1 (`2 × IFETCH_CALLS` era) | 33.90 % – 39.71 %, and those were **upper bounds** — "LOOP's own probe load not derivable" |
| **metal 2026-08-23, measured spans** | **24.80 % @39 .. 25.11 % @50** |

A **0.31 pp bracket in place of a 5.8 pp interval of upper bounds**, and the whole `s` block
self-checks (`sum(spans) == TRANSITIONS`, 800 774 653 on that capture; boot-line price bracket
39 (marginal) .. 50 (in-bucket) from `armed 105342, unarmed 23707, empty 2090, 1024 pairs`).
This is the `LOOP`-interval closure the C2 map was owed; it can be consumed directly.

> **Both rows above are the v2 `LOOP` — the whole dispatch loop.** From rung 1d the bucket
> that carries id 0 is the *residue* and prints as `LOOPRES`; the quantity these rows measure
> is now `LOOPRES + DOPCFIND + DOPCFILL + BLKREC`, printed on the `[PROF] l` line. See "The
> LOOP split" above before comparing a new capture against 24.80–25.11 %.

**`FAULT` is reported uncorrected**, and deliberately. An unwind (`z3660_prof_unwound()`) charges
FAULT cycles but counts no span, because an unwind is not a probe transition and has never been
in `TRANSITIONS` — counting it per bucket would make the two corrections disagree. At the
0.06 % FAULT measures on this interpreter that is a rounding error, and it errs by
over-reporting the bucket rather than under-reporting it.

Adding this changed no id and no wire structure, so **the version stays 2**; the `s` block is
additive text that an older symbolizer ignores.

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
| 27 | `DOPC_INVAL` | ... whole-cache invalidations **actually performed** |
| 28–37 | `IV_FLUSH` `IV_ROOT` `IV_DMA` `IV_TABLE` `IV_KNOB` `IV_WRAP` `IV_MAP` `IV_PFLUSH` `IV_PFLUSHA` `IV_CACR` | ... invalidation **requests**, one per cause, in `Z3660_DOPC_INV_*` order (C2 rung 1b) |
| 38 | `IV_CACR_SKIP` | ... a `MOVEC` to CACR proved **inert at its call site** (C2 rung 1c) — **not a cause**, and not a member of the block above |

Two identities worth checking in any capture that has them: `IFETCH_CALLS == FETCH`
identically (all five bracket sites increment `FETCH` exactly once), and

```
DOPC_HIT + DOPC_MISS  ==  INSNS + FAULTS        with the decoded-op cache on
```

— every dispatch consults the cache exactly once, which is what makes
`DOPC_HIT / (DOPC_HIT + DOPC_MISS)` a hit rate. **The `+ FAULTS` term is not slack**: a
faulting instruction *consults the cache* (so it is a dispatch) and then *throws before
retiring through the run-loop tail* (so `INSNS` never counts it), and twelve metal captures
across three sessions and three firmware builds put the sum above `INSNS` by exactly `FAULTS`,
every time, to the unit. This document previously stated the identity as `== INSNS`, and the
rung-1 session read the gap as a live-print artefact of the dump loop; it is a mechanism. A
difference that reproduces another counter in the same dump exactly, twelve times, is not skew.

> `IFETCH_CALLS == FETCH` says nothing about how often the ifetch **buckets** are entered — see
> the `s`-block section above, where the measured spans refute that inference by 1.97×.

A third arrives with the `IV_` block: on a capture taken entirely with the cache on,
**`sum(IV_*) == DOPC_INVAL`** — every request that reaches `z3660_dopc_invalidate()` performs
one. `sum(IV_*) < DOPC_INVAL` is impossible and the firmware warns if it sees it. The gap in
the other direction is the requests made while the cache was **off**, which is a fact about the
window rather than a defect.

> **`IV_CACR_SKIP` is separate because the row that would otherwise score it cannot see it.**
> C2 rung 1b briefly defined "the narrowing's yield" as `sum(IV_*) − DOPC_INVAL`. That is
> **false for any narrowing implemented at a call site**: the CACR compare lives in
> `newcpu_common.cpp`, ahead of the invalidator, so an inert write increments nothing at all —
> it is not a skipped request, it is an uncounted one. The 2026-08-23 session measured a 54.8 %
> fall in invalidations that the row could account for only 7.3 % of, and reconstructed the
> rest by hand across two sessions' captures.
>
> **A narrowing that decides before `z3660_dopc_invalidate()` must bring its own request
> counter, or it is invisible to its own verdict.** The guest's true request rate is
> `sum(IV_*) + IV_CACR_SKIP`, and the firmware prints that row with the narrowed share out of
> it.

**With the decoded-op cache off**, the four `DOPC_` counters are exactly zero — `HIT`/`MISS`/
`EXT` because the lookup returns before it can count, and `DOPC_INVAL` because
`z3660_dopc_invalidate()` consults the switch too. **The `IV_*` counters still move**,
by design: they count what the *guest* asked for, and the guest issues `CPUSHL` and `PFLUSH`
whatever the switch says. That makes a cache-off arm a free, uncontaminated measurement of the
guest's own invalidation rate. (Before rung 1b, `DOPC_INVAL` also counted with the cache off —
`z3660_dopc_invalidate()` had no switch test at all — and the 2026-08-23 metal session found it
reading 164 311 and 2 951 874 in the two off arms while the docs claimed zero.)

> **Read that arm in a `PROFZ`-zeroed window or it will look broken.** The counters accumulate
> from power-on and the console cannot fire before core1 starts, so an OFF arm armed during
> boot necessarily carries a warm-up: the 2026-08-23 session read `DOPC_HIT` 2 480 711 with the
> cache off and had to chase it before continuing. It was 2.49 M dispatches — 1.5 % of the boot
> — taken before the arm landed. `PROFZ`, then the workload, then read: all four go to exactly
> zero.

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
   counter read, a subtract, a 64-bit accumulate and a stack push/pop, and there are 14–24
   transitions per emulated instruction (v2's tail split added four to v1's 8–14; rung 1d's
   LOOP split adds two to six more, and **prints its own share** on the `[PROF] l` line). The
   firmware **measures** this at arm time and reports it as `probe_cyc` alongside the
   `TRANSITIONS` count, so the inflation is computable and, if you care, subtractable per
   bucket by its share of transitions.

   **And from C2 rung 1b it reports that share directly** (the `s` lines — see above).
   "Subtractable per bucket by its share of transitions" was an instruction the dump did not
   supply the numbers for: only the ifetch bracket had a per-bucket transition count, from
   `IFETCH_CALLS`. The 2026-08-22 stage capture paid for that in the most visible way
   available — `LOOP` came out the **largest single bucket at 23.78 % raw** and its corrected
   share had to be published as an interval (≤ 33.90 % at 32 cyc, ≤ 39.71 % at 43) because the
   number was not computable from the dump.

   **That interval is now closed and both of its ends were wrong.** With per-bucket spans,
   metal 2026-08-23 puts `LOOP` at **24.80 % @39 .. 25.11 % @50** — and the `2 × IFETCH_CALLS`
   substitute the interval rested on over-charges the ifetch buckets by 1.97×, so it was not a
   conservative approximation but a biased one. **Never derive a bucket's transition count from
   a counter that merely correlates with it; take it from the `s` block.**

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

       [PROF] probe calibration: armed <n> cyc, unarmed <n> cyc, empty <n> cyc, 1024 pairs -> <n> cyc/transition marginal, <n> in-bucket
       [PROF] price bracket: <n> (bodies only, what PROFB gates) .. <n> (bodies + the call scaffolding, which is inside the buckets too) cyc/transition -- sweep it, do not pick

   so the subtraction can be checked rather than taken on faith. The host harness asserts
   the arithmetic exactly against the injectable clock (`test_prof_probe_calibration`):
   armed span `= STEP × (2·PAIRS + 1)`, unarmed `= STEP`, difference over `2·PAIRS` `=
   STEP` on the nose.

   **The prediction was stated in advance so that it could be wrong, and it was wrong.**
   It said the reported figure should fall by roughly 8–14 cycles, from 59 into the 45–51
   region. Metal 2026-08-22 came back at **32** — armed 89894, unarmed 22623, 1024 pairs,
   `(89894 − 22623) / 2048 = 32.85`, self-consistent and a fall of 27 cycles, not 8–14.
   See *the price bracket* (2c) immediately below for what that means and what a capture should
   actually be priced at; the short version is that **45.5 was never a floor for this
   quantity**, and the third calibration pass added for it now measures both ends of the
   range instead of predicting one.

   (The landing-model half of map §10.3 remains open either way. The map's leading
   candidate for it — that the shared ifetch bracket is entered fewer times than `FETCH`
   counts — is refuted by the source: all five `Z3660_PROF_ENTER_IFETCH` sites increment
   `FETCH` exactly once, so `IFETCH_CALLS == FETCH` identically. The new counter id 20
   makes a capture say so.)

   **2c. The price bracket — what a capture should actually be priced at.**

   **The 45.5 floor and the 32 measurement are not the same quantity, and neither bounds the
   other.** 45.5 is v1's `91 cyc/pair` halved. v1's 91 was measured with the probes **inlined**
   and timed the **whole** loop — a whole-cost, inlined number. 32 is a **marginal**,
   **out-of-line** number: the difference between two identical loops that differ only in the
   `z3660_prof_bucket_on` bit. Comparing them is comparing a total against a difference, and
   "32 is below 45.5, so the subtraction now under-corrects" does not follow.

   Nor is the unarmed loop compiling differently, which is the other thing worth ruling out
   explicitly: `z3660_prof_probe_call_enter/_exit` are `noinline` and the flag is a global read
   **inside the callee**, so both passes execute the *same machine code at the same addresses*.
   The only difference is a branch outcome inside the callee — and the memory traffic and the
   `PMCCNTR` coprocessor read that the taken arm performs, which are the probe body and belong
   in the figure.

   What *is* true is that the marginal figure is not the whole of what the probes put inside
   the buckets, and the reason is the order inside `enter()`:

   ```
   z3660_prof_enter():   now = z3660_prof_cycles();   <-- the span for the entered bucket starts HERE
                         acc[cur] += now - last;  TRANSITIONS++;  push;  cur = bucket;
                         last = now;                   <-- stamped with the PRE-body reading
                         <epilogue, bx lr>              <-- still inside the entered bucket's span
                         ... the accessor's real work ...
                         <bl exit, prologue, if (!bucket_on)>   <-- also inside it
   z3660_prof_exit():    now = z3660_prof_cycles();   <-- the span ends HERE
   ```

   So `enter()`'s epilogue and return, and the `bl`/prologue/predicate on the way into
   `exit()`, are **inside a bucket** and are **absent from the marginal difference**, because
   they are identical in the armed and unarmed passes. Symmetrically, `exit()`'s own body and
   epilogue land in the *parent* bucket. Every instruction of the pair lands in some bucket;
   only some of them are in the marginal price.

   Hence the third calibration pass, added 2026-08-23: **the same loop with no calls in it at
   all**. It isolates the one term that belongs to neither — the `for` compare, branch and
   increment, which exist in this calibration and nowhere in the interpreter — and turns
   `unarmed` from a single conflated number into two separable ones:

   | quantity | arithmetic | what it names | metal 2026-08-22 | **metal 2026-08-23** |
   |---|---|---|---|---|
   | **marginal** | `(armed − unarmed) / 2N` | the probe **bodies**: the `PMCCNTR` read, the 64-bit accumulate, `TRANSITIONS++`, the stack push/pop, the timestamp store. What `PROFB` gates off. | **32.8** | **39.86** |
   | call scaffolding | `(unarmed − empty) / 2N` | the `bl`, prologue, predicate, epilogue and `bx lr` — inside the buckets, and still there when the bodies are gone | ~9.8 (est.) | **10.56** |
   | **in-bucket** | `(armed − empty) / 2N` | bodies **plus** scaffolding: the price if the instrumentation calls go away entirely | ~**42.6** (est.) | **50.42** |
   | whole armed call | `armed / 2N` | everything the armed loop pays, loop scaffolding included | 43.9 | 51.44 |

   **The `empty` pass is no longer an estimate.** Metal 2026-08-23 measured it directly —
   `armed 105342, unarmed 23707, empty 2090, 1024 pairs` — and the 2026-08-22 estimates for the
   scaffolding term were good (~9.8 predicted, 10.56 measured). **The two sessions' brackets do
   not agree, and that is the point of printing the calibration per boot rather than tabulating
   it here**: `[32.8, 42.6]` against `[39.9, 50.4]` is a ~20 % shift between captures on the
   same board. **Price a capture from its OWN boot line, never from this table** — the columns
   are here to show what the three passes mean and roughly where they land, not to supply a
   constant.

   **What to price a capture at: sweep `[marginal, in-bucket]`, exactly as the C2 map swept
   50 → 59, and report the range.** Do not pick an end, and do not treat one as a correction of
   the other — the marginal figure is right for "the prof build with the probe bodies gated
   off", the in-bucket figure is right for "the prof build without the instrumentation calls",
   and the map's readers usually want the second while the firmware subtracts the first.

   **…but not past the CEILING, and the boot line cannot know where that is.** A bucket's
   corrected cycles are `acc[b] − spans[b] × price`, so the largest price the capture can be
   swept to at all is

   ```
   ceiling = min over b with spans[b] > 0 of  acc[b] / spans[b]
   ```

   That is a property of the **capture**; the bracket is a property of the **calibration**;
   and until 2026-08-23 nothing forced the two to be consistent. **Twice they have not
   been.** The C2 map's v2 *picked* 59 when the arithmetic allowed at most ~50 — the error
   that produced the "`LOOP` is empty" claim. Then the 2026-08-23 boot line *advertised* an
   in-bucket end of **68** while that boot's own ceiling was **59.17** (bound by `TAILADV`),
   so the printed end of the printed bracket was arithmetically impossible and the shares
   at it were unusable. Both times it was caught by hand, after the shares had been
   computed.

   So `PROFD` now prints the ceiling beside the per-bucket price, names the bucket that
   binds it, and **warns** when the boot line's in-bucket end is above it:

   ```
   [PROF] s ceiling: min_b(acc/spans) = <n> cyc/span, bound by <bucket> -- the price above which a bucket's subtraction goes negative
   [PROF] s WARNING: the boot line's in-bucket end <n> is ABOVE this capture's ceiling <n> and is arithmetically impossible. Sweep [<marginal>, <ceiling>]; do NOT sweep to <n>.
   [PROF] s usable sweep: [<marginal>, <in-bucket>] -- the boot line's in-bucket end fits under the ceiling
   ```

   **It warns rather than clamping**, and that is deliberate: `z3660_prof_probe_cyc_in_bucket()`
   is what tools read to reproduce a capture's own bracket, and quietly returning a smaller
   number would relabel every capture already read at the advertised one. The rule for a
   reader is one line: **sweep `[marginal, min(in-bucket, ceiling)]`, and if the two ends
   disagree say which one you used.** A ceiling *below* the marginal figure means the price
   is wrong for this capture outright and no sweep is defensible — the existing
   "modelled probe cost exceeds its measured cycles" warning fires on the same condition.

   **And one consequence that matters to the C2 map.** Even the *upper* end of the bracket,
   and even the whole armed call at 43.9, sit **below** the 50.59 at which the map's span
   exhausts `LOOP` and the 54.11 at which it exhausts `TAILADV`. So §8.3's conclusion that
   "`LOOP` is empty" is reached at a price this instrument does not support at either end.
   `LOOP`'s share is small but it is not established to be zero, and the C2 map should say so.

   **Measured, 2026-08-23: `LOOP` is 24.80 % @39 .. 25.11 % @50 — not empty, and not small.**
   The per-bucket spans settle it, and the map can consume that bracket directly instead of
   the interval of upper bounds it was given.
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

The two calibration lines follow it and are part of the same record — the second one is new
as of 2026-08-23 and is what a capture should be priced against (§2c):

```
[PROF] probe calibration: armed 89894 cyc, unarmed 22623 cyc, empty <n> cyc, 1024 pairs -> 32 cyc/transition marginal, <n> in-bucket
[PROF] price bracket: 32 .. <n> cyc/transition -- sweep it, do not pick
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
<pct>%` per bucket, one `[PROF] t` whole-tail rollup, **the `[PROF] l` whole-loop rollup and
its split-cost line (rung 1d)**, the `[PROF] s` per-bucket span block, `[PROF] c <id> <NAME>
<n>` per counter, then a few `[PROF] r` derived rates. Its header line carries
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
>
> **And a third, of the same family: the stats dump's banner.** `PROFD`'s first line
> (`[PROF] === stage attribution ===`) collides with the console echo of the `PROFD` that
> requested it, in exactly the way the ring dump's first header line does and for exactly the
> same reason — one UART behind one semaphore, and the echo is already in flight when the
> firmware takes it. The banner arrives mangled or short; **every `b`, `t`, `l`, `s`, `c` and
> `r` line after it is intact**, because each is a separate `printf` that acquires the
> semaphore cleanly. **This one is handled host-side and the firmware cannot fix it**: there
> is no ordering the dumping core can impose on an echo the other core has already begun. A
> symbolizer must key on the `[PROF] ver=` line rather than on the banner, and must not treat
> a missing or corrupt banner as a truncated capture.

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
4c2. **And the loop from four more.** `LOOPRES + DOPCFIND + DOPCFILL + BLKREC` is what v2
   called `LOOP`. **Do not report id 0 alone against a figure quoted from a v2 capture** —
   check the name: `LOOP` is the whole loop, `LOOPRES` is the residue. Report the four parts
   *and* the rollup, exactly as for the tail, and carry the `[PROF] l split cost:` figure
   beside them so a share can be re-priced without rung 1d's own transitions.
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
   rollup and the `probe overhead inside the totals` line, the **`s` per-bucket span block**
   (which is what makes each bucket's corrected share a number rather than an interval), the
   **`l` whole-loop rollup and its split-cost line**, and take the counters (`IPAGE_*`,
   `DPAGE_*`, `ATC_*`, `MISALIGN_*`, `IV_*`, `IV_CACR_SKIP`) from this run: they are exact
   regardless of the timing distortion.

   **The `l` block is what a rung-1d capture is for.** Its four parts are the first
   enumeration `LOOP` has ever had, and the one question they settle is map §15.4's: is
   `DOPCFIND` large? If it is, the top of the ladder is rung 1's own hit path and `LOOP`'s
   26 % is partly re-attributed `FETCHOP` rather than new cost. `DOPCFILL` is a second
   number the session should bank whether or not it is large, because map §17.3's
   physical-index study has to be judged against **the fill path's measured price** and this
   is the first capture that has one. Check the two identities on the `l` lines before
   reading any of it.

   **Sweep the corrected shares across THIS boot's `[marginal, in-bucket]` bracket** — the boot
   line prints both ends and they move ~20 % between sessions. Report the range; picking an end
   is the error the third calibration pass exists to prevent. And take each bucket's transition
   count from the `s` block: deriving it from a correlated counter is what the `2 ×
   IFETCH_CALLS` method did, and it was wrong by 1.97×.
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
> v2 reports cycles per transition, calibrated out of line. The band this paragraph used to
> quote — "above 45.5 and below roughly 58" — **has been withdrawn**: 45.5 is v1's
> `91 cyc/pair` halved, and v1's 91 was measured with the probes *inlined* and timed the
> whole loop, so it is a whole-cost inlined quantity and not a floor for a marginal
> out-of-line one. See **bias note 2c, the price bracket**, above.
>
> **The first v2 metal calibration came back at 59**, above the 50.59 at which the C2 span
> exhausts `LOOP`, because it was timing the armed loop alone and charging the call, the
> predicate and the loop scaffolding to the probe — all of which stay in the machine the
> subtraction leaves behind. It now differences the armed loop against the identical
> unarmed one and came back at **32** (metal 2026-08-22), a fall of 27 rather than the
> predicted 8–14. A third pass over the same loop with no calls in it now measures the
> other end of the bracket too, so a capture can be swept across `[marginal, in-bucket]`
> instead of priced at a number chosen from one end.
> Also note that both the v1 header example and this document once showed `11` cyc where
> metal measured `91`: anyone sizing bucket inflation from the documented example would
> have understated it ~8×. The examples in this file are now taken from real metal output.
