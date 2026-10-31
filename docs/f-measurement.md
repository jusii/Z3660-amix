# The `f` measurement — C2-ATTACK-MAP §38.6, and it is a removal

> **PRE-REGISTRATION. Committed before the implementation commit, and the git timestamps are
> the evidence.** Nothing below may be edited once a metal row exists — corrections go in a
> dated addendum at the foot of the file, exactly as `docs/handler-discriminator.md` does.

## What this is, and what it is not

It is **not** a physical-index design, and no part of one is built here. It is the cheap §30.3
experiment whose measured value of `f` decides whether that design round is ever funded.

C2 attack map v3.3 §38.6 returned **ADJUST** on the physical-index study: nothing about it got
worse, the prize per removed miss came in *richer* than the design had been sized against
(§38.2), and the cost site — the thing §17.3 named as the killer and §22.3 called thin — turned
out to have more break-even headroom than the whole fill it would be prepended to (§38.3). The
entire remaining risk is concentrated in **one unmeasured number**:

> `f` — the fraction of decoded-op-cache misses that would be removed by not invalidating the
> cache on the guest's `CPUSHL` / `CINV` traffic (the `IV_FLUSH` cause).

§24.4 carried `f` at **0.5096** — `IV_FLUSH`'s share of invalidation traffic, i.e. a response
slope of 1.0 — while its own calibration point measured **0.70 misses removed per invalidation
removed**, which puts `f` at **0.357** (§38.4). *"That is the same shape of error the `HANDLER`
round just paid a full metal session for."* So: buy the prize's size before funding the design.

**And `f` is buyable without building anything**, because the thing to be measured is a
*removal*:

> A perfect physical index invalidates only the entries whose physical address matches.
> A no-op invalidates none. The matching set is a subset of everything, so
> **no-op ≥ physical index in hit rate, by construction** — and a switch that makes `IV_FLUSH`
> not invalidate the decoded-op cache therefore reads a **strict upper bound on `f`**, with the
> cost side charged at **zero**.

That is the whole experiment. One console switch, one boot, two arms.

## The build is DELIBERATELY UNSOUND and must never ship

Skipping an invalidation the 68040 architecture requires is a correctness violation, not a
tuning choice. Everything in the safety shape below exists because of that one sentence.

* **Default OFF.** Disarmed is byte-exact standard behaviour. The mailbox word `dopc_ivsup`
  (`0xFFFF00F0`) resolves **unset → DISARMED**.
* **The polarity is the OPPOSITE of every other C2 switch, deliberately.** `blk_chunk`,
  `dopc_on`, `dfast_on` and the reverted `spec_on` all read *unset → the built-in default → ON*,
  because for those the shipped behaviour is the optimisation and a core0 built before the field
  existed must still run it. **The reasoning here is identical and the value is inverted: unset
  means the shipped behaviour, and the shipped behaviour of an unsound mode is OFF.** There is
  no "unset = ON" anywhere in this feature, and there must never be one.
* **Arming takes an exact 32-bit magic**, `Z3660_DOPC_IVSUP_ARM` — not `1`, not "anything
  non-zero". `SHARED` is hand-maintained in two headers that a beta resync has already
  mis-merged once (`docs/piscsi-service-path.md`), so a neighbouring field's value bleeding into
  this one must **disarm**, which is what a magic-word test guarantees and a truthiness test
  does not.
* **A loud serial banner** on arm, on disarm, and on **every** profiler dump — and the dump's
  banner fires on `armed || IV_SUPPRESSED != 0`, so a capture taken *after* a disarm still
  confesses if any suppression happened inside its window.
* **A confession counter.** `IV_SUPPRESSED` (profiler id 46) carries it per capture;
  an always-compiled 64-bit counter carries it per boot, so the **lean** arm — the one with no
  profiler at all, and the one the wall-clock number comes from — also carries its own count.
* **No config-file key exists.** The arm is reachable only by typing the console command.

### The arm state must not survive a reboot — and here is the mechanism

**The mailbox is DDR that survives a warm reset.** `config_file.c` says so in as many words,
about `prof_ctl`: *"a boot that leaves `prof_ctl` alone would let core1 act on the previous
boot's command."* So "the field defaults to disarmed" is **not** sufficient by itself: a word
written by the console in one boot is still sitting there in the next one.

Two mechanisms, and the first is the load-bearing one:

1. **core0 publishes `Z3660_DOPC_IVSUP_OFF` into `shared->dopc_ivsup` unconditionally, on every
   boot, from both config-resolve paths** (the no-preset path and the preset path), settled
   before core1 is dispatched. This is `resolve_prof_config()`'s pattern, verbatim, for the same
   reason: *writing it every time makes "not armed" an explicit instruction rather than the
   absence of one.*
2. **core1's own state is a static initialised to disarmed**, so a core1 that runs before core0
   has published anything is disarmed, and the resolve rule that turns a mailbox word into a
   state lives in exactly one header both cores compile (`z3660_dopc.h`).

Note that `blk_chunk` / `dopc_on` / `dfast_on` are **not** published this way and therefore *do*
survive a warm reset. That is benign for a switch between two sound machines. It is not benign
here, which is why this switch does not follow them.

### The correctness gate — §38.6's own, and it is not advisory

> **The armed run is VALID ONLY IF dhrystone's printed values are correct.**

**What to compare.** Dhrystone 2.1 ends with a `Final values of the variables used in the
benchmark:` block that prints each accumulated variable beside the value it *should* be
(`Int_Glob`, `Bool_Glob`, `Ch_1_Glob`, `Ch_2_Glob`, `Arr_1_Glob[8]`, `Arr_2_Glob[8][7]`,
`Ptr_Glob->Discr` and its fields, `Next_Ptr_Glob->…`, `Int_1_Loc`/`Int_2_Loc`/`Int_3_Loc`,
`Enum_Loc`, `Str_1_Loc`, `Str_2_Loc`). The metal protocol captures that whole block in **both**
arms and checks it **twice**:

* against the benchmark's own printed `should be:` values — the absolute check;
* against the disarmed arm's block — the differential check, which also catches a benchmark
  build whose `should be` lines are themselves wrong.

If the local dhrystone build prints only a score, the gate **degrades and is declared degraded
in the report**: score-only plus the full boot-message scan plus a post-run filesystem check is
weaker evidence, and the row must say so rather than quietly claiming the gate passed.

**What failure looks like, and why any anomaly is measurement-invalidating rather than a
finding.** A decoded-op entry caches a *resolved handler pointer* (`cpuop_func *handler[way]`)
plus the opcode and one extension word, keyed on the guest logical PC. Suppressing a required
`CPUSHL`/`CINV` means the entry for a PC whose bytes the guest has since replaced is still
believed, so the machine executes **the previous occupant's instruction** at that address. That
can produce, in rising order of visibility:

1. **wrong computed results** — a stale `MOVE`/`ADD` writes a plausible wrong value and nothing
   complains. This is the one the dhrystone value block exists to catch;
2. **wild control flow** — the stale entry's handler is an arbitrary opcode's handler, so the
   stale decode's operand fields are read out of unrelated bytes: a wild branch, a wild `JSR`,
   a wild address register;
3. **wild writes** — the same, on the store side, including into the kernel's buffer cache. This
   is why the armed arms run on a **work copy** of the root disk;
4. **panics and bus errors** — the visible tail of (2) and (3).

**The highest-risk guest path is demand paging.** AMIX must flush before executing a text page
it has just paged in; suppressing that flush means running the *previous* tenant's decodes for
that page. Dhrystone is small and resident and is therefore the workload least likely to trip
it — which is a reason to trust a clean dhrystone row, and **not** a reason to extrapolate the
mode's safety to anything else.

**A misbehaving guest is informative and it is not the measurement.** §38.6: *"If the guest
misbehaves, that is itself informative (it bounds how much of `IV_FLUSH` is genuinely
load-bearing)"* — record it as that bound and score the funding gate **NOT MET for want of a
valid armed row**, never as a partial pass.

## The §30.3 steps, in order

### 1. Name the latency the removed work sits next to

**Already named and already classified, and §38.6 says to restate rather than re-derive.** The
prize is *whole misses, not scaffolding*: each removed miss removes one `DOPCFILL` **and** the
opcode fetch a miss forces that a hit does not.

| side | what moves | classification |
|---|---|---|
| **prize** | `DOPCFILL` per miss (**75.9 – 81.9** cyc at `[54, 60]`) **+** `FETCHOP` per miss (**90.7 – 96.8**) = `S` = **166.6 – 178.7 cyc** | **not latency-hidden.** A miss's own fetch is *the one fetch the decoded-op cache is not hiding* — an inference from rung 3's measurement (§38.2), not a direct measurement of the miss path, and §34.2 applies |
| **cost** | nothing. A no-op invalidation costs **zero** by construction | — |

The cost side being zero is the whole point of measuring `f` this way: the reading is the
study's **true ceiling**, not a trade.

### 2. Ask what is already hiding it

Nothing hides a fill. §38.3: *a translation prepended to a fill is a dependency in front of the
fill, so it arrives in full* — which is the statement about the design's **cost** side, and it
is exactly why the design's cost side has to be measured separately later and is charged at zero
here.

### 3. Count the added work separately and charge it

The added work in this build is **one register compare plus one flag load, on the invalidation
path only** — not on the hit path, not on the fill path, not on the dispatch loop. The disarmed
arm pays it once per invalidation request.

Priced in advance: dhrystone's own invalidation traffic is order **10⁴–10⁵** requests per run
(rung 1c measured 36 472 performed after the CACR narrowing, 80 719 before it) against a
dhrystone run of order **10¹⁰** ARM cycles. At a generous 10 cycles per test that is
**< 0.01 % of wall clock** — three orders of magnitude below the gate.

> **§30.2's corollary still applies and its sign is unchanged**: a same-boot switch always
> overstates, because the OFF arm is not the pre-change machine. What is different here is the
> *size*: rung 2's disarm penalty was −3.74 % and rung 3's −1.58 %, and this one is predicted at
> **< 0.01 %**, which is why the same-boot number is usable directly. **The prediction is
> checked, not assumed** — see the cross-build reference row below.

### 4. Buy the answer with one lean A/B plus a cross-build reference row

`DEC21748` is archived, CRC-verified on the card, and rebuilt byte-for-byte from the repo
(§35.2), so step 4's precondition is met before the session starts and the trap that cost r3 its
reference arm cannot fire.

| arm | image | what it buys |
|---|---|---|
| **R** cross-build reference | `DEC21748`, as archived | the standing posture, **8197** (5 runs, mean 8181.2, spread 0.68 %) |
| **A** lean disarmed | this build, lean | the **disarm penalty**, measured against R rather than argued |
| **B** lean armed | this build, lean | the **wall-clock response** — folds `S`, the miss rate, the fill price and the fetch price into one number needing no band, no denominator and no instrument |
| **C** prof disarmed | this build, prof | the **hit-rate baseline**, and it doubles as **F6's clean fourth point** (see below) |
| **D** prof armed | this build, prof | the **hit-rate response** = `f`'s upper bound, directly |

**Three runs minimum per lean arm, means compared, spread reported.** The standing posture's own
run-to-run spread is 0.68 % over five runs and the gate is 1.5 %; a single run per arm cannot
separate them.

## THE PRE-REGISTERED FUNDING GATE

> ### **`f` ≥ 0.35 AND the wall-clock response ≥ ~1.5 % → the physical-index design round is FUNDED.**
> ### **Below either → NOT FUNDED.**
>
> The cost bar the design must then beat is **`T*` = 93 – 99 cycles at band `[54, 60]`**, at the
> calibrated `f` = 0.357 (§38.3), and a design that adds work to *every* fill starts below it.

Both clauses are required. They are not redundant — they fail in different ways and the
difference is the reading:

```
   f  =  (miss_rate_disarmed  -  miss_rate_armed)  /  miss_rate_disarmed        [arms C, D]

        where miss_rate = DOPC_MISS / (DOPC_HIT + DOPC_MISS)

   wall-clock response  =  dhry(B) / dhry(A)  -  1                              [arms A, B]

   gate line, against the standing 8185:   8185 x 1.015  =  dhry >= ~8308
```

`T*` is re-derivable at whatever `f` comes back, and the metal session should re-derive it
rather than quote the table:

```
   T*  =  f/(1-f)  x  S           S = 166.6 .. 178.7 cyc at [54, 60]

     f = 0.357  ->  T* =  93 ..  99 cyc     (the calibrated row, and the standing bar)
     f = 0.40   ->  T* = 111 .. 119 cyc
     f = 0.5096 ->  T* = 173 .. 186 cyc     (§24.4's headline)
```

**Remember which direction the measurement bounds.** This experiment returns an **upper bound**
on `f`, so the `T*` computed from it is an **upper bound on the budget**: a real physical index
achieves `f' ≤ f` and therefore has `T*' ≤ T*`. A design round funded on this row is funded on a
ceiling, and its own pre-registration inherits that caveat.

### The reading rules — null and pay, written before the row exists

| outcome | reading | what happens |
|---|---|---|
| **`f` ≥ 0.35 and wall clock ≥ 1.5 %** | **PAY.** The prize is both real and worth the money. | Fund the design round, with §38.6's re-ordered pre-registration list: item 1 is discharged by this row; item 2 (the cost site must pay **only on a miss that was going to walk anyway**, *shown in the disassembly*) becomes the gate; items 3 and 4 are restated, not re-derived. |
| **`f` < 0.35** | **NULL, and it is a STRIKE-shaped null.** The removal fraction was carried at 1.0 when its own calibration said 0.70; if the direct measurement lands below 0.35 then even 0.70 was generous, and §38.4's three unmeasured reasons it might be (a physical index *narrows* rather than removes; `IV_FLUSH`'s locality need not resemble `IV_CACR_SKIP`'s; one calibration point is one point) all bite in the same direction. | **Do not fund.** Record `f` and strike the study from the ladder. Do not re-measure at a different band — `f` is a ratio of two hit rates in one boot and carries no band. |
| **`f` ≥ 0.35 but wall clock < 1.5 %** | **NULL, and it is the pricing law's own verdict.** The misses are there and removing them does not move the machine — which means the prize is not worth `S`, i.e. §38.2's inherited-and-untested assumption (a decoded-op hit pays *no* opcode fetch, so all of `FETCHOP` is chargeable to misses) is where to look. This is the §30 law arriving on the prize side for the third time. | **Do not fund.** This is a stronger negative than the first row, because the cost side was charged at zero: a design that also pays `T` on every surviving fill can only do worse. |
| **wall clock ≥ 1.5 % but `f` < 0.35** | **INVALID, not a pass.** Two measurements of the same removal that disagree in sign of size means the arms are not what they are labelled — the likeliest cause is a workload-window difference between arms, and `INSNS` is the row that says so. | Re-take the boot. Do not average. |
| **any correctness-gate failure in an armed arm** | **INVALID for funding, informative for the mode.** | Record the bound it puts on how much of `IV_FLUSH` is load-bearing. Score the gate NOT MET. |
| **arm A vs arm R differs by more than ±0.5 %** | the disarm penalty is not what §30.3 step 3 predicted. | The same-boot B/A number is then **not** usable directly; report B against **R** and say so. |

## The expected-direction table

Every direction below has been checked against the counter's **position in the call topology**
relative to the code being skipped — §37.1, which is in this file because the last round scored
two misses against the firmware for a bookkeeping error in a design document.

**What the armed build changes, exactly:** `z3660_dopc_invalidate(cause)` counts the cause as it
always did, and then — for `cause == Z3660_DOPC_INV_FLUSH` **only** — returns without calling
`z3660_dopc_do_invalidate()`. Everything downstream of that call is what can move.

| counter / quantity | armed direction | why, and the topology check |
|---|---|---|
| `IV_FLUSH` (28) | **UNCHANGED** | `z3660_dopc_count_cause()` runs **above** the suppression test, deliberately, so the guest's own flush rate stays measurable in both arms. A counter above the skipped branch **cannot** move — §37.1 in its refuting form, applied on purpose. |
| `IV_SUPPRESSED` (46, new) | **0 → `IV_FLUSH`** | Incremented on the suppressed arm only. Over a window armed throughout, `IV_SUPPRESSED == IV_FLUSH` exactly. |
| `DOPC_INVAL` (27) | **FALLS**, by exactly `IV_SUPPRESSED` | Incremented **inside** `z3660_dopc_do_invalidate()` — below the skipped branch, so it can fall, and by construction it falls by the whole suppressed count. |
| `sum(IV_*)` vs `DOPC_INVAL` | the standing identity **BREAKS by design** | With the cache ON the armed identity is `sum(IV_*) − IV_SUPPRESSED == DOPC_INVAL`. The disarmed case is the `IV_SUPPRESSED == 0` special case of it. |
| `IV_ROOT` `IV_DMA` `IV_TABLE` `IV_KNOB` `IV_WRAP` `IV_MAP` `IV_PFLUSH` `IV_PFLUSHA` `IV_CACR` (29–37) | **UNCHANGED**, and each still **performs** | Same topology as `IV_FLUSH` for the counting, and none of them reaches the skipped branch: the test is on the cause id and nothing else. |
| `IV_CACR_SKIP` (38) | **UNCHANGED** | A different function entirely (`z3660_dopc_cacr_skipped()`), never reached by the suppression. |
| `DOPC_MISS` (25) | **FALLS** | Not a topology question but a state question: fewer generation bumps ⇒ fewer stale sets ⇒ fewer lookups that miss. This is the measurement. |
| `DOPC_HIT` (24) | **RISES** | Ditto, and `DOPC_HIT + DOPC_MISS == INSNS + FAULTS` is preserved — every dispatch still consults the cache exactly once, so the pair moves as a see-saw and the **rate** is what to read. |
| `FETCH` (2) and `IFETCH_CALLS` (20), **per guest instruction** | **FALL** | **The §37.1 check, coming out the other way this time.** `FETCH` sits *below* the extension-word window's early return and is reached only when the decoded-op cache did **not** answer — so removing misses removes reaches, and it genuinely can fall. §37.1's own arithmetic gives the size: a miss costs two real fetches a hit does not (the loop's `x_prefetch(0)` and the extension lookahead), so expect `Δ(FETCH/INSN) ≈ −2 × Δ(misses/INSN)`. |
| `DOPC_EXT` (26) | **RISES** | Counted in the window-hit arm, *above* `FETCH`. More hits ⇒ more armed windows ⇒ more window serves. |
| `IPAGE_HIT` / `IPAGE_MISS` (5, 6) | absolute counts **FALL**; the **ratio** rises slightly | Both sit below a real fetch, so they are only reached when the window did not answer. §37.1 measured the ratio move −0.150 pp for *more* misses; expect the opposite sign here, ±0.2 pp. **Not a gate** — an `IPAGE` row outside that band is a note, not a failure. |
| `DOPCFILL` spans (bucket 15) | **FALL**, proportionally with `DOPC_MISS` | The bucket is entered once per fill; §37.2 validated `DOPCFILL` spans/INSN 0.2167 against fills/INSN 0.2178. This is the independent cross-check on `DOPC_MISS`. |
| `DOPCFIND` spans (bucket 14) | **UNCHANGED** | One lookup per dispatch, `tcnt[DOPCFIND] == DOPC_HIT + DOPC_MISS`. If it moves, `INSNS` moved. |
| `TRANSITIONS` (17) | **FALLS** slightly | Carried by the `DOPCFILL` and `FETCHOP` bracket entries above. Not a gate. |
| `INSNS` (0) | **UNCHANGED**, ±1 % | Dhrystone runs a fixed iteration count. A larger move means the two arms did not run the same workload, which is the **INVALID** row of the reading rules, not a finding. |
| `READ` `WRITE` `DFAST_HIT` `DFAST_XPAR` `MISALIGN_*` `BLK_*` | **UNCHANGED** | Data side. Untouched by an instruction-cache-coherence switch. |
| **dhrystone score** | **RISES** by the `f`-dependent amount | The gate. §38.5's re-derived ceiling is 1.7 – 2.7 % *net of a 15-cycle translation*; this arm charges no translation, so it reads the **gross** figure, §38.5's 2.05 – 3.24 %. |

> **Where the armed row is expected to land, stated in advance so a surprise is visible:**
> at the calibrated `f` = 0.357 the gross response is 2.05 – 3.24 %, i.e. **dhry ≈ 8353 – 8450**.
> At §24.4's naive `f` = 0.5096 it is 2.93 – 4.62 %, i.e. **dhry ≈ 8425 – 8563**. A reading far
> **above** 8563 is as suspicious as one below the gate and the first thing to check is whether
> the correctness gate really passed.

## The metal protocol

**The armed arms run on a WORK COPY of the root disk.** Stale decoded ops can wild-write
(failure mode 3 above), and the installed system is not the test bed. Restore staging and the
work-copy discipline are the standing care gates and they are not waived for a cheap experiment.

Order, and it is chosen so the two riskiest arms come last:

1. **R** — flash nothing. `DEC21748` is already on the card and CRC-gated there; take three
   dhrystone runs as the session's own reference row. *(If the card has been reflashed since,
   restore `DEC21748` first and CRC-gate it. Create `FAILSAFE.bin` before any flash.)*
2. **C** — boot the **prof** image, **unarmed**, and take the capture. **This is also F6's clean
   fourth point**: it is the first prof build since s19 with no third table and no `SPEC`
   machinery, so `DOPCFIND` cyc/dispatch is finally measurable without the −1.07 % disarm cost
   §39.3 could not separate out. Take it in-band, sweep the capture's own printed band, and
   record the ceiling line's verdict. **Do not arm anything during this boot.**
3. **A** — boot the **lean** image, disarmed, three dhrystone runs. Check A vs R: the disarm
   penalty must land inside ±0.5 %.
4. **D** — same prof boot as C or a fresh one, `IVSUP` armed. Capture. **Read the banner back
   from the serial log before trusting anything in the dump.**
5. **B** — lean, `IVSUP` armed, three dhrystone runs, correctness gate on every one.
6. **Return leg**: disarm, re-run one dhrystone, confirm it reproduces arm A. A return leg that
   does not reproduce means the disarm did not invalidate and the whole session is suspect.

**Boot-message scan on every boot**, per the standing rule: new lines on a first-of-a-kind boot
are findings by default, and this is a first-of-a-kind boot.

**Park the card back on `DEC21748` before the session closes.** This image must not be left on
the card, and it must never be the production image.

## What this measurement does NOT establish

1. **It does not measure a physical index.** It measures the *ceiling* a physical index could
   approach and cannot exceed. Every number it returns is an upper bound and must be quoted as
   one.
2. **It does not test the design's cost site.** `T` is charged at zero by construction. §38.6
   item 2 — the cost site must pay only on a miss that was going to walk anyway, **shown in the
   disassembly** — is untouched by this row and remains the design round's gate.
3. **It does not settle §38.2's inherited assumption.** That a decoded-op hit pays no opcode
   fetch, so all of `FETCHOP` is chargeable to misses, is carried unchanged from §24.4 and is
   still untested. The wall-clock arm is the one place it *partly* bites: if the assumption is
   wrong, the wall clock will say so by coming in below what `S` predicts, and that is the
   third reading rule above.
4. **It does not resolve the miss-rate confound.** §38.5's two bases (0.2178 vs 0.1496
   misses/INSN, 46 % apart, two honest windows of one boot) are a property of the instrument's
   window, not of this switch. `f` is a *ratio* of two hit rates measured the same way in one
   boot, so it is immune; the **ceiling in percent** that §38.5 derives from it is not.
5. **It does not license the mode for anything but this measurement.** A clean dhrystone row
   says the small resident workload survived. It says nothing about paging, and nothing about
   the disk.

---

# THE METAL VERDICT — 2026-08-23: **`f` = 0.0787, NOT FUNDED, STRUCK**

> Everything above this line is the pre-registration. **Nothing above it has been edited**,
> which is the only reason the scorecard below is worth reading. This section is the row.

Session 2026-08-23 17:27–18:37 EEST, A4000 + Z3660 metal, preset 7 (`amix_ram 128`,
`service_cadence 4`), armed arms on a **work copy** of the installed 040 root. Four boots,
three flashes (prof `2530C3F4`, lean `E663B44A`, close-out back to `DEC21748`), six CRC
gates passed, serial span 290 079 bytes captured with **zero drops**.

> ## `f` = **0.0787** · wall clock = **+0.00 %** median / **+0.15 %** mean
> ## → **NOT FUNDED.** The physical-index decoded-op cache design round is **STRUCK**.

| clause | required | measured | verdict |
|---|---|---|---|
| `f` ≥ 0.35 | 0.35 | **0.0787** | **MISS by 4.4×** |
| wall clock ≥ ~1.5 % → dhry ≥ 8308 | 8308 | **8173** | **MISS**; the gate needed +135, it moved **+0** |

## `f`, from the two arms it is defined on

Arms **C1** (prof, unarmed) and **D1** (prof, armed), **same boot**, same instrument state
(stage buckets off in both), each `PROFZ`-fenced around one dhrystone run at a **pinned
100 000 count**:

```
   C1 disarmed   DOPC_HIT 61 996 672   DOPC_MISS 2 341 153   miss_rate = 0.03638844
   D1 ARMED      DOPC_HIT 62 174 023   DOPC_MISS 2 156 677   miss_rate = 0.03352485

   f = (0.03638844 - 0.03352485) / 0.03638844  =  0.0787
```

`T*` **re-derived at the measured `f`**, as this document requires rather than quoted from
the table above:

```
   T* = f/(1-f) x S = 0.0854 x (166.6 .. 178.7) = 14.2 .. 15.3 cyc
```

against the standing bar of **93 – 99 cyc** at the calibrated `f` = 0.357. **And it is an
upper bound**, exactly as pre-registered: a no-op invalidates a superset of what a physical
index would, so a real index achieves `f' ≤ 0.0787` and inherits `T*' ≤ 15.3 cyc`. A design
that prepends a translation to every fill starts an order of magnitude above its own budget.

## Which reading rule fires

**Row 2 — `f` < 0.35 → NULL, and it is a STRIKE-shaped null.** Record `f`, strike the study
from the ladder, and **do not re-measure at a different band**: `f` is a ratio of two hit
rates in one boot and carries no band.

Row 4 (`wall clock ≥ 1.5 % but f < 0.35 → INVALID`) **did not fire** — the wall clock missed
too. Row 3 did not fire either. The two instruments are different (hit-rate counters on the
prof image, dhrystone wall clock on the lean one, different boots) and they agree on both the
sign and the size of the null, which is the strongest form this negative could take. The
"too good to be true" guard also stayed silent: the armed row is 8173, nowhere near the 8563
this document named in advance as equally suspicious.

## The rows

| arm | image | median | mean | spread | n |
|---|---|---|---|---|---|
| **R0** rig continuity | `DEC21748`, installed root | 8163 | 8164.8 | 0.28 % | 5 |
| **R** reference | `DEC21748`, work copy | **8185** | 8178.4 | 0.56 % | 5 |
| **A** lean disarmed | `E663B44A` | 8173 | 8157.8 | 0.70 % | 5 |
| **B** lean **ARMED** | `E663B44A` | **8173** | 8169.6 | 0.13 % | 5 |
| **A′** return leg | `E663B44A` | 8174 | 8174.3 | 0.04 % | 3 |

```
   disarm penalty       A / R  - 1 = -0.147 %  median   (-0.252 % mean)
   SAME-BOOT RESPONSE   B / A  - 1 = +0.000 %  median   (+0.145 % mean)   <<< THE GATE
   return leg          A' / A  - 1 = +0.012 %  median
```

The §30.3 step-3 prediction is **checked, not assumed**: the disarm penalty was priced in
advance at < 0.01 % of wall clock and measured −0.147 %, inside the ±0.5 % band — so the
same-boot B/A number is usable directly and no fallback to reporting B against R was needed.
R lands on **8185 exactly**, so the live reference and the pre-registered 8308
gate line coincide. The return leg reproduces arm A to +0.012 %, so the disarm did invalidate
and step 6's "the whole session is suspect" condition did not fire.

## The expected-direction table — **15 PASS, 2 NOTE, 0 MISS**

Every **gated** row moved the way it was written to move, months of reasoning ago. The
identity rows are exact rather than approximate:

```
   sum(IV_*) - IV_SUPPRESSED == DOPC_INVAL
       disarmed   5741 -    0 = 5741 == 5741      the standing identity's special case
       ARMED      5738 - 2676 = 3062 == 3062      the standing identity, broken by design
   IV_SUPPRESSED == IV_FLUSH        2676 == 2676  exact, window armed throughout
   DOPC_HIT + DOPC_MISS == INSNS + FAULTS         exact, BOTH arms
   DOPCFIND spans == DOPC_HIT + DOPC_MISS         exact, BOTH arms
   DOPCFILL spans -6.55 % vs DOPC_MISS -6.55 %    the independent cross-check, 2 dp
```

`IV_FLUSH` moved **+0.04 %** — §37.1's refuting form, used deliberately: a counter above the
skipped branch cannot move, and did not. `DOPC_MISS` fell **7.88 %**; that is the
measurement. The two NOTE rows were both declared not-gates in advance:

* **D11**, and it is the one pre-registered direction metal refutes — the **unbanded half**.
  `FETCH` fell 3.25 % while `IPAGE_HIT`/`IPAGE_MISS` did not move (+0.011 % / +0.013 %). The
  mechanism is in the ratio: `IPAGE_HIT+MISS` is 6.98 M against 13.80 M fetches, **one lookup
  per two fetches**, so the ipage cache is not consulted per fetch on this path and the
  fetches the suppression removed were the ones reusing an already-resolved page. The
  **ratio** — the half this document put a ±0.2 pp band on — moved −0.0001 pp and is inside
  it. Scored as the document scores it: a note, not a failure.
* **D14**, `TRANSITIONS` −0.41 %, direction right.

**D9 is a PASS with a recorded overshoot.** §37.1 prices a miss at exactly two real fetches a
hit does not pay; metal says **2.43**, 21 % above prediction. Recorded rather than rounded
away — it is a small upward correction to the prize side of any future dopc sizing, and it
makes this null slightly *worse* for the design, not better: each removed miss is worth a
little more than assumed and there are still far too few of them.

**D5 was first scored MISS by the session's own scorer, and the scorer was the artefact.** A
bare >2 % rule flagged `IV_DMA` 6 → 4 — two DMA invalidations on a base of six, a count at
which "unchanged" is not a measurable claim. Rescored on a rule *stated* rather than tuned
(|Δ| ≤ 1 % **or** |Δ| ≤ 5 counts): all nine causes unchanged. The row's other half — "each
still **performs**" — is directly checkable and checks out: in the armed arm the non-flush
causes sum to **3062** and `DOPC_INVAL` is **3062**.

## The correctness gate — **PASSED, and not degraded**

Every measurement window ran `dhry` directly at a pinned 100 000 runs, because one checked
variable's correct value *is* `Number_Of_Runs + 10` and a differential diff is meaningless at
a varying count.

| capture | absolute check | differential |
|---|---|---|
| C1 / C2 prof disarmed | 20 checked, **0 wrong** | C2 == C1, byte-identical |
| **D1 / D2 prof ARMED** | 20 checked, **0 wrong** | **== C1 / C2, byte-identical** |
| A lean disarmed | 20 checked, **0 wrong** | — |
| **B lean ARMED** | 20 checked, **0 wrong** | **== A, byte-identical** |
| A′ return leg | 20 checked, **0 wrong** | == A, byte-identical |

All seven are additionally **byte-identical to the `DEC21748` reference block** taken on boot
0, before any IVSUP image existed. No wrong value, no wild branch, no wild write, no
filesystem error, no panic, no bus error in either armed window.

**Armedness is attested, not assumed.** Every prof capture carries the profiler's own banner
and a non-zero `IV_SUPPRESSED`; the lean image has no profiler at all and is attested by
core1's per-boot confession on disarm — **122 823 invalidation(s) suppressed** in the lean
armed window (13 382 on the prof boot). The decision to compile the confession counter into
*every* variant is what made arm B attestable.

**And that licenses nothing.** §38.6 pre-empted the wrong conclusion and it stands: dhrystone
is small and resident and is the workload least likely to trip the mode. The highest-risk
guest path is demand paging and nothing paging-heavy was run armed, deliberately. A clean row
here says the measurement is valid; it says nothing about the mode. The build must never
ship, and it is off the card. It also means the session bought **no** bound on how much of
`IV_FLUSH` is genuinely load-bearing — this document offers that as the consolation reading
if the guest misbehaves, and the guest did not.

## The error chain, recorded rather than smoothed over

| carried at | by | value |
|---|---|---|
| §24.4 | `IV_FLUSH`'s share of invalidation traffic, i.e. an assumed response slope of 1.0 | **0.5096** |
| §38.4 | that row's *own* calibration point (0.70 misses removed per invalidation removed) | **0.357** |
| **metal** | measured, upper-bounded, two agreeing instruments | **0.0787** |

The response slope is not 1.0 and not 0.70: it is **0.154**. This document opened by saying
*"that is the same shape of error the `HANDLER` round just paid a full metal session for"* —
which was correct, and understated. §38.4's three named reasons the 0.70 might be generous (a
physical index *narrows* rather than removes; `IV_FLUSH`'s locality need not resemble
`IV_CACR_SKIP`'s; one calibration point is one point) all bit in the same direction, exactly
as pre-registered.

**The instrument is not what failed. The prize is not there.** That distinction is the whole
value of having written the direction table down in advance — a null with a broken instrument
and a null with a working one are different findings, and this one is provably the second.

## What this closes, and what it does not

**Closes:** §38.6 item 1. `f` is measured, it is an upper bound, and the physical-index
design round is struck from the ladder.

**Does not close, and must not be read as closing:** the design's **cost site** (`T` was
charged at zero by construction; §38.6 item 2 — the cost site must pay only on a miss that
was going to walk anyway, *shown in the disassembly* — is the gate any successor design
inherits); §38.2's inherited assumption that a decoded-op hit pays no opcode fetch (with `f`
this small the wall clock had no signal to carry either way); §38.5's miss-rate confound (`f`
is immune, the ceiling in *percent* derived from it is not, and is not quoted here); and any
licence whatsoever for the IVSUP mode.

---

**2026-08-23, after the verdict above: the `IVSUP` machinery was removed from the tree.** The
console command, the `dopc_ivsup` mailbox word, the suppression, the banners and the
`IV_SUPPRESSED` counter (id 46) are all gone — everything this file describes as *present* is
now history, and the profiler's live identity is `sum(IV_*) − IV_WRAP == DOPC_INVAL`. The
measurement is not to be retaken: the reading rules above forbid it.
