# The HANDLER discriminator — CYBERSTORM C2 §30.3, on one handler family

> ## STATUS — PRE-REGISTERED. Not built when this section was written, not measured.
>
> This document exists **before** the code, and the bands below are the whole point of
> it. A pre-registration that gets edited after the measurement is not one.

**What this is.** The cheapest experiment that can tell the C2 ladder whether its **#1
rung — handler specialisation — is worth funding at all**. It specialises *one* handler
family behind a console switch, and the **sign and size of the wall-clock row it produces
classifies the whole `HANDLER` bucket** as latency-hidden or dependency-chained.

**What this is NOT.** It is not a handler-specialisation design. It covers one family,
takes none of the things a real design would take (a pre-decoded EA in the cache entry, a
wider extension window, a threaded dispatch), and its verdict is about the *bucket*, not
about this family's own worth. If it pays, the design gets written. If it does not, the
ladder's #1 rung is struck and the map's ranking changes for the third time.

**Why it has to exist.** Map v3.2 §33.3 note 1 is blunt about how `HANDLER` got to the top
of the ladder:

> `HANDLER` leads because the bucket above it was measured hidden and the buckets below it
> are small or empty — **not** because anything says its work is recoverable. The 25 % is
> v2's estimate, carried unchanged for three versions now. **Its first move is not a
> design; it is the §30.3 discriminator on one handler family.**

and §34.2 says why an argument cannot substitute for the experiment:

> Two rungs, opposite signs, one week, one rig. That is enough to refute *"instruction
> counts predict wall clock"* and enough to name the mechanism, and it is **not** enough
> to predict a third rung's sign from armchair reasoning.

---

## The §30.3 steps, in order

### 1. Name the latency the removed work sits next to — per cost site

Not the bucket, the latency. The specialisation removes work at four distinct sites, and
they do not all sit next to the same thing.

| # | cost site removed | the latency it sits **next to** |
|---|---|---|
| **A** | **the indirect call and return** — `blx r3` through the decoded-op entry's `handler[way]`, plus the handler's `push {r4,r5,r6,lr}` / `pop {…,pc}` | the **guest PC dependency of the next dispatch**. The handler's last act is the `regs.pc` store; the next iteration's `z3660_dopc_find()` indexes on it. Nothing else in the loop is available to overlap this — the loop is serial through the PC. |
| **B** | **opcode field re-extraction** — `and r3,r0,#7`, `ubfx r5,r0,#9,#3`, and the reload of `regs.opcode` the run loop already had in a register | the same PC dependency as A, one step earlier. |
| **C** | **the extension-word call** — `bl uae_mmu040_get_iword` for the `d16(An)` displacement, which on a decoded-op hit is answered from `z3660_dopc_ext_val` after 9 instructions and **never touches guest DRAM at all** | **nothing.** This is the site that matters most for the classification. Rung 3's removed scaffolding sat next to a *real* guest-DRAM fetch; this one sits next to a window hit, which is a load from a resident global. There is no DRAM access here for the removal to hide behind. |
| **D** | **the accessor call frame** — `bl uae_mmu040_get_long` plus its `push {r0,r1,r4,r5,r6,lr}` / `add sp,sp,#8` / `pop`, when gcc inlines the (header-defined, `static`) accessor into the run loop | the **guest-DRAM access the accessor performs**. This is the one site where rung 3's answer plausibly repeats: a call frame beside a DRAM load is exactly what was measured to return nothing. |

**So the discriminator is not homogeneous, and that is deliberate.** Sites A, B and C sit
in front of the interpreter's own serial chain; site D sits beside a DRAM access. A rung-2
result would mean A+B+C dominate; a rung-3 result would mean the whole removal behaved
like D.

### 2. Ask what already hides it — and say the prediction is not trusted

The decoded-op cache hides the instruction stream's DRAM access. That is what struck rung
3. Nothing hides:

* the **guest PC chain** — dispatch → handler → PC → next lookup → next dispatch. It is
  the interpreter's loop-carried dependency and every handler sits inside it.
* a **window hit** (site C). It is a load of two resident globals; there is no slack
  behind it.
* the **indirect branch** (site A). One branch site with ~1 867 possible targets on a
  Cortex-A9 whose BTB holds one target per site. Its cost is a pipeline flush, which an
  instruction count cannot see and which nothing overlaps, because the flush *is* the
  loss of the overlap.

> ### The prediction, written down so it can be scored: **dependency-chained.**
> Sites A, B and C are in front of the interpreter's serial chain; only D is beside a
> latency the machine already hides.

> ### And the prediction is why the experiment exists, not a substitute for it.
> §34.2: the classification has to be **bought** per rung, cheaply, before the rung is
> funded — not asserted from the shape of the code. Rung 3's authors also reasoned that
> the fetch scaffolding was chained; they were wrong, from equally plausible armchair
> reasoning, and it cost a build. **The band below is scored against metal, not against
> this paragraph.** If the armed arm reads null, this section is what was refuted.

### 3. Count the added work separately and charge it

The disarmed path pays for the discriminator's existence, and it is counted from the
disassembly of the run loop before the change, not estimated.

The decoded-op hit path already loads the entry's `meta` byte into a register and tests it
twice (`tst r1,#2` for `M_EXT0`, `tst r1,#1` for `M_IDIOM`). The discriminator's verdict is
a **third bit in that same byte**, so the added work on a dispatch that is not specialised
is:

```
tst  r1, #4          test M_SPEC in the meta byte already in a register
bne  <spec body>     not taken when disarmed or when the opcode is not covered
                     = 2 ARM instructions per decoded-op HIT, and nothing else
```

Nothing is added on the decoded-op **miss** path (the bit is only read from an entry), and
nothing at all is added to any accessor.

**Priced.** The run loop's own hot path is **96 ARM instructions** per dispatch
(`m68k_run_mmu040`, lean, counted along the taken path from the loop top through the
decoded-op hit, the non-idiom skip, the dispatch and the cadence-4 tail), plus **~12** for
`do_cycles_cpu_fastest` through its indirect call. A dispatch of the family's hottest shape
costs a further ~72. So 2 instructions land on a base of **~180 per dispatch ⇒ +1.1 % of
instructions**, and at the decoded-op hit rate of 78.34 % that is **+0.87 % of all
dispatches' instructions**.

> ### Pre-registered added-work band: the `SPEC`-OFF arm sits **0.25 % – 0.90 %** below the reference build.
>
> The upper edge is the count model at constant IPC. The lower edge uses **rung 3's own
> measured deflator**: its +19 instructions were priced from the disassembly at
> −3.2 .. −3.5 % and arrived at **−1.58 %**, about half — and against a proportional
> instruction-count base rather than a bucket share, about 0.3×. Rung 3's added leg is the
> only measurement this lane has of what *added* work costs, so it sets the band's floor
> rather than an argument.
>
> **And the disarm penalty here is deliberately tiny compared with rung 2's −3.74 % and
> rung 3's −1.58 %**, because the verdict rides in a byte the hot path already loads.
> That is the whole reason to reuse the BLK idiom-verdict field instead of adding a
> lookup: it makes the same-boot switch a much better estimator of the shipping row.

**One added cost that is *not* an instruction count, and is not in the band:** the
specialised body is inlined into `m68k_run_mmu040`, so the run loop grows. Rungs 2 and 3
both left it at 2 288 B — "the specific I-cache regression the map warns about". This rung
cannot. The size change is reported in *As built* below and is a pre-registered risk, not a
priced one.

### 4. Buy the answer with one lean A/B plus a cross-build reference row

One console switch, both arms in one boot, and the outgoing lean image archived first
(`76fa64b` does this automatically now). The reference row is `DEC21748`, the reverted
rung-2-posture image the archive preserved.

**The whole metal protocol:**

1. Take the reference row on the archived **`DEC21748`** — lean, cadence 4, three runs.
2. Flash the new lean image. `SPEC` **ON** (the default), three runs — this is the row the
   verdict turns on, and it is a **cross-build** row.
3. `SPEC` **OFF**, three runs — the disarm penalty, scored against step 1.
4. `PROFB` / `PROFZ` / dhry / `PROFD` on the prof image, both switch states, for coverage
   (`SPEC_HIT / INSNS`) and the bucket movement.

`cross-build × disarm-penalty = same-boot switch` must close, as it did to four digits for
rung 2 and to five for rung 3. It is the cheapest available check that the two arms are the
two machines they are supposed to be.

---

## The family, chosen with numbers

### There is no per-family counter, and there is a per-family measurement

The profiler exposes no opcode or handler histogram, and no dopc-entry histogram. What it
does expose is the **sampler ring**: one record per tick carrying the guest PC and *the
opcode word that was executing*. The sampler fires at the run-loop tail, so a record's
probability is proportional to that dispatch's elapsed time — **a ring dump is a
time-weighted opcode histogram**, which is exactly the denominator "how much wall clock can
this family possibly reach" needs.

Three `DHRY` ring dumps exist, all from 2026-08-22. Their supervisor share is the thing to
check first, because the metal cadence-4 window the ladder is scored on runs **48.74 %
supervisor instructions** (rung 3's `IFAST`-on capture):

| ring (2026-08-22) | samples | supervisor share (weighted) | representative of the metal window? |
|---|---|---|---|
| `c2-blk-verdict/09-DHRY-BLKON` | 11 235 | **43.31 %** | yes |
| `c1v2-profiles/05-DHRY-ring-full` | 17 031 | **54.10 %** | yes |
| `c1-profiles/10-DHRY-ring-full` | 65 535 | 87.91 % | **no** — a kernel-dominated window |

Time-weighted family shares, the two representative rings:

| family (opcode line) | `09-DHRY-BLKON` | `05-DHRY-ring-full` |
|---|---|---|
| **`MOVE` / `MOVEA` (lines 1, 2, 3)** | **31.51 %** | **27.50 %** |
| ‑ line 2, `MOVE.L` / `MOVEA.L` | 24.40 % | 22.19 % |
| ‑ line 1, `MOVE.B` | 6.60 % | 4.87 % |
| ‑ line 3, `MOVE.W` / `MOVEA.W` | 0.51 % | 0.44 % |
| `Bcc` / `BRA` / `BSR` | 13.93 % | 12.57 % |
| `ADDQ` / `SUBQ` | 6.96 % | 11.03 % |
| `CMP` / `CMPA` / `EOR` | 6.17 % | 5.08 % |
| `JSR` / `JMP` | 4.66 % | 4.76 % |
| `MOVEM` | 4.47 % | 4.07 % |
| `MOVEQ` | 2.31 % | 2.82 % |

**`MOVE` is the largest single family by a factor of two**, and it is one family in
gencpu's own sense — every entry in lines 1, 2 and 3 of `op_smalltbl_31_ff` is generated
from one template and commented `/* MOVE */` or `/* MOVEA */`.

### The shapes taken, and the coverage that buys

Restricting both effective addresses to `Dn`, `An`, `(An)`, `(An)+`, `-(An)`, `d16(An)`
keeps the specialised body to two six-arm switches and **excludes every mode that needs a
second extension word, a full-format brief extension, or a PC-relative base**:

| | `09-DHRY-BLKON` | `05-DHRY-ring-full` | *(kernel-heavy ring)* |
|---|---|---|---|
| `MOVE`/`MOVEA` family, all shapes | 31.51 % | 27.50 % | *15.96 %* |
| **covered: both EAs in the six simple modes** | **26.36 %** | **22.91 %** | *9.19 %* |
| covered fraction of the family | 83.7 % | 83.3 % | *57.6 %* |
| largest uncovered shape | `(xxx).L → (xxx).L`, 0.99 % | `(xxx).L → (xxx).L`, 1.41 % | *`(xxx).L → (xxx).L`, 5.50 %* |

> **The mix is a property of the window, not a constant — finding F5's standing warning.**
> The kernel-heavy ring is in the table precisely so this is not forgotten: there,
> `(xxx).L → (xxx).L` alone is 37 % of the family, and coverage falls to 57.6 %. The two
> representative rings are the ones whose supervisor share matches the scored window, and
> the pre-registration is built on them. **Read `SPEC_HIT / INSNS` off the capture you are
> interpreting; do not re-pin 26 %.**

**The family only reaches the specialised path on a decoded-op hit**, because the verdict
rides in the entry's `meta` byte. At the measured hit rate of **78.34 %** the served share
is

```
covered share x dopc hit rate = 22.91 .. 26.36 %  x  0.7834  =  17.9 .. 20.6 %
                                                                of dispatch time
```

### The family's share of `HANDLER`, and why the signal is not priced from it

Counted from the lean disassembly, the family's handler bodies are 15–24 ARM instructions
(`op_2000` 15, `op_2068` 18, `op_2028` 23, `op_20d8` 24) against a whole dispatch of
123–224. The family is therefore roughly **25 % of the `HANDLER` bucket** (band 20–30 %),
which against §33.1's 18.02–18.87 % would say 4.5–5.7 % of runtime is in this family's
handlers.

**That number is not used below, and the reason is §31.1 applied to a second bucket.**
`HANDLER` carries **1.906 spans per guest instruction** (136 394 026 spans over 71 556 834
instructions) because every nested `READ`/`WRITE`/`FETCHEX` bracket splits it — so it pays
about two probe pairs per instruction, more than any bucket except `LOOPRES` and `TAILADV`.
The correction subtracts the calibrated bracket pair; §31.1 established that it does not
subtract the out-of-line call cost around it, and that row H was consequently inflated ~3×
as a sizing tool. Against counted handler bodies of ~20 ARM instructions on a dispatch of
~180, `HANDLER`'s *lean* share looks nearer 11 % than 18 %.

> **This is an arithmetic sketch and is offered as one, exactly as §34.4 offers row H's.**
> No counter separates the instrument from this bucket either. The *direction* is
> established by the same mechanism §31.1 named and by the span count; the *size* is not.
> **The consequence for this document is only that the bands below are built from counted
> instruction paths and a measured hit rate, and never from `HANDLER`'s profile share.**

---

## The design

**One bit in the decoded-op entry, one inline body, one switch.** No new table.

`Z3660_DOPC_M_SPEC` (0x04) joins `M_IDIOM` (0x01) and `M_EXT0` (0x02) in the entry's
existing `meta` byte. `z3660_dopc_fill()` sets it when the opcode is a covered `MOVE` shape
**and the switch is armed**; the run loop tests it with the two instructions §30.3 step 3
prices. This is the **idiom-verdict field pattern rung 0 established** — a static verdict
computed once at fill time so the hot path pays a bit test instead of a recognizer.

Because the bit is only written while armed, **`z3660_spec_set()` invalidates the
decoded-op cache**, exactly as `z3660_dopc_set()` does and for the same reason: entries
filled in one state must not be believed in the other. `DFAST` does not need this and this
does; the difference is that `DFAST`'s switch changes what a table *means* while this one
changes what an entry *says*.

**The specialised body is derived from the generated C, mechanically.** For each covered
shape it does what `cpuemu_31`'s handler does, in the same order, with three things
removed: the call frame, the opcode field re-extraction the run loop already performed, and
— for a `d16(An)` displacement at `pc+2` when `M_EXT0` is armed — the call into
`uae_mmu040_get_iword()`, whose window would have returned `z3660_dopc_ext_val` anyway. The
displacement is read straight out of the entry.

**The memory access is the existing accessor, not a second implementation.** The body calls
`get_long_mmu040()` / `put_long_mmu040()` and their byte and word siblings — the same
`static` header functions every generated handler calls, carrying rung 2's fast path,
the fault semantics, the write-back contract, the M-bit re-walk, the device decline and the
misalignment split. **None of it is reimplemented, so none of it can drift**, and whether
gcc inlines them into the run loop at `-Os` is a *measurement* reported in *As built*, not
an assumption — `ALWAYS_INLINE` in this tree is a bare hint and gcc has declined it here
before (`accessor-fastpath.md`).

**Anything not covered falls through to the unmodified dispatch**, byte for byte: `(xxx).W`,
`(xxx).L`, `(d8,An,Xn)`, all PC-relative and immediate modes, `MOVE.B` to or from `An`
(which is not a `MOVE` at all), and — belt and braces — `MOVE.B` with `(A7)+` or `-(A7)`,
whose stack-alignment special case is the classic trap in this family and is simply not
entered.

### What the profile will do, declared in advance

The specialised path answers a `d16(An)` displacement from the entry instead of calling the
fetch accessor, so **`FETCH` / `IFETCH_CALLS` and the `FETCHEX` bucket will fall** by the
number of such instructions served. Rung 3 was careful to keep its bucket comparable —
"the bucket gets cheaper; it does not get emptier" — and **this rung deliberately does the
opposite at that one site.** It is declared here so a capture is not misread as a fetch-side
change. `IPAGE_HIT` / `IPAGE_MISS` are **not** affected: a window hit returns before the
instruction-page cache is consulted, in both arms.

### The one new counter

`SPEC_HIT` (id 46, `Z3660_PROF_C_COUNT` 46 → 47, append-only, no wire-version bump):
dispatches served by the specialised body. It exists because the existing buckets cannot
answer the first question a null result raises — *did it fire?* — which is the question
rung 2 and rung 3 both made their coverage counter's job. `SPEC_HIT / INSNS` is also the
direct measurement of the family's instruction share that the ring dumps can only
approximate.

---

## Pre-registered — judge against these

### The arithmetic

`R` is the fraction of a served instruction's **whole dispatch path** that the
specialisation removes, counted from the lean disassembly (`arm-none-eabi-objdump -d`,
every instruction issued, predicated-not-taken ones included):

| shape | dispatch path, before | removed | `R` |
|---|---|---|---|
| `MOVEA.L d16(An),An` (`op_2068`, 18.0 % of the family) | 108 loop + 18 handler + 9 window + 44.6 read = **179.6** | 17 – 21 | 9.5 – 11.7 % |
| `MOVE.L d16(An),Dn` (`op_2028`, 9.4 %) | 108 + 23 + 9 + 44.6 = **184.6** | 17 – 21 | 9.2 – 11.4 % |
| `MOVE.L (An)+,(An)+` (`op_20d8`, 10.3 %) | 108 + 24 + 44.6 + 47 = **223.6** | 12 – 17 | 5.4 – 7.6 % |
| `MOVE.L Dn,Dn` (`op_2000`, 6.7 %) | 108 + 15 = **123** | 8 – 10 | 6.5 – 8.1 % |

The 108 is the run loop's own hot path — **96 counted instructions** through
`m68k_run_mmu040` plus **~12** for `do_cycles_cpu_fastest` — and the read/write figures are
`accessor-fastpath.md`'s counted 24 %/76 % `XPAR`/`PAGE` weighting. Weighted across the
covered shape mix (~45 % carry a `d16(An)` displacement, ~55 % do not):

```
R (weighted)                                        =   7 .. 11 %
served share of dispatch time                       =  17.9 .. 20.6 %
gain at constant IPC = served share x R             =  +1.25 .. +2.27 %
                                                       (dhry 8185 -> 8287 .. 8371)
```

### The bands

> ### **NULL — `HANDLER` is LATENCY-HIDDEN. The ladder's #1 rung is STRUCK.**
> Cross-build lean cadence-4 dhrystone within **±0.68 %** of the `DEC21748` reference.
>
> 0.68 % is the top of the rig's own within-arm spread (§34.1). Rung 3's null read
> −0.05 % against the same band and was decisive. **A null here is the decisive outcome**:
> it says that even work sitting in front of the interpreter's serial PC chain returns
> nothing, and no handler design can then be funded from a bucket share.

> ### **PAY — `HANDLER` is DEPENDENCY-CHAINED. The design gets written.**
> Cross-build lean cadence-4 dhrystone **≥ +0.90 %** (dhry **≥ 8259**).
>
> +0.90 % is 72 % of the count model's floor and 1.3× the noise ceiling. It is deliberately
> below the count model, because rung 3 taught this lane that a count-derived projection can
> arrive at half its size and still be the same phenomenon.

> ### **UNRESOLVED — between +0.68 % and +0.90 %.**
> The rung stays **unfunded**; that is the conservative default and it is chosen now rather
> than argued later. The cheap next step if this happens is named under *Not taken* below,
> and it is not a redesign.

### The other pre-registrations

* **`SPEC_HIT / INSNS`: 18 – 27 %.** The covered family is 22.9–26.4 % of dispatch time in
  the representative rings and cheaper than the mean dispatch, so its *instruction* share
  should be at or above its time share, scaled by the 78.34 % hit rate. **Read coverage
  first if the wall clock moves**: a gain with low coverage moved something else, and a
  null with zero coverage is a build failure, not a result.
* **`DOPC_HIT` must not move by more than ±0.5 pp** across the switch. The specialisation
  changes nothing about what is cached; if the hit rate moves, the comparison is not
  like-for-like.
* **`DFAST_HIT` and the dpagecache hit rates must not move**, and the two switches must be
  independent in both directions. The harness pins this; metal confirms it.
* **`FETCH` / `IFETCH_CALLS` WILL fall**, by the count of served instructions carrying a
  `d16(An)` displacement — declared above, and the one place this rung is deliberately not
  like-for-like.
* **`IPAGE_HIT` / `IPAGE_MISS` must not move** (±0.1 pp).
* **The `SPEC`-OFF arm sits 0.25 – 0.90 % below `DEC21748`** (§30.3 step 3).
* **`cross-build × disarm penalty = same-boot switch`**, to three digits.

### The pre-registered failure mode

*The specialised path serves 20 % of dispatches, the handler bodies get measurably shorter,
and dhrystone does not move — because the interpreter is not issue-limited at all, and the
removed instructions were filling slots beside stalls the guest's own memory traffic
already owns.* If that happens, read `SPEC_HIT` coverage first and `DOPC_HIT` second, and
then **believe it**: it is rung 3's result on the other side of the dispatch, and it strikes
the ladder's last large rung.

---

## The guard: byte-exact equivalence over the whole covered opcode space

Host harness, `Z3660_emu/test/host`. The reference arm is the **disarmed** path — the
original generated handler, not a second implementation.

**`test_spec_opcode_sweep`** is exhaustive rather than sampled, which is the whole argument
for restricting the shapes to six EA modes: 3 sizes × 6 source modes × 8 source registers ×
6 destination modes × 8 destination registers, minus the encodings that are not `MOVE`, is a
few thousand opcodes and every one of them is run **armed and disarmed** from the same
seeded machine state, comparing the returned cycle count, all sixteen registers, `PC`,
`SR`, `CCR`, the destination memory, the fault outcome, `mmu_ssw`, and the four page-cache
counters. A corpus that hand-picks shapes in a family this size is a corpus that will miss
one.

**`test_spec_state_equivalence`** covers what a sweep over a quiet machine cannot:
supervisor and user; the same VA in both privilege states; first and last aligned access in
a page; a `d16(An)` displacement that crosses into the next page; **faults mid-instruction**
— an unmapped destination after a successful source read, a write-protected destination, a
supervisor-only page from user state — each asserted to leave the same fault address, the
same `regs.instruction_pc` and the same registers, because the 68040 restarts the
instruction; a device bank (declines, and the declined access is asserted to have reached
the bank's own put function); `(An)+`/`-(An)` with source and destination naming the **same**
address register, in both orders; MMU off; a rebuild while disarmed; and **the whole
machine** — a real instruction sequence through `m68k_run_mmu040()`'s own dispatch, run
armed and disarmed and compared on everything, with `DFAST` held ON in both arms so the data
rung's serve count is asserted unchanged across this switch.

**Engagement is pinned on every case.** *"The two paths agree"* is trivially true when the
specialised body never fired, and half of this corpus is decline cases.

**The mutation arm.** `test_spec_mutation` compiles a deliberately wrong specialised body —
`MOVE.L` leaving the V flag set instead of cleared, a single wrong bit in one flag of one
size — and asserts the corpus **fails**. A guard that has never been seen to fail is not
known to be a guard.

---

## What it does *not* take, and what each is worth

Priced, so the next round does not re-derive them:

* **`(xxx).W` and `(xxx).L` EAs.** 4.1 % of the family in `09-DHRY-BLKON` and 6.4 % in
  `05-DHRY-ring-full` for the `(xxx).L → (xxx).L` shape alone, and **37 % in the
  kernel-heavy ring** — so this is the largest uncovered thing and its worth is highly
  window-dependent. `(xxx).L` needs a *longword* extension fetch and the decoded-op entry
  caches one word, so it would not gain site C at all. **Not taken: the site that makes the
  discriminator interesting is the one this shape cannot use.**
* **`(d8,An,Xn)`.** ~2 % of the family. Needs the brief-extension decoder
  (`x_get_disp_ea_020`), which is a call this rung would have to either keep or
  reimplement. Keeping it removes almost nothing; reimplementing it trades the rung's whole
  safety argument for 2 %.
* **A second family.** If the result lands in the UNRESOLVED band, the cheap next step is
  **`Bcc`/`BRA` (13.93 % / 12.57 %)** behind the *same* switch — counted at ~19 ARM
  instructions removed per served instruction, worth roughly +0.3 .. +0.6 % on its own,
  enough to lift a marginal reading clear of the noise. It is one more inline body and no
  new mechanism. **Deliberately not taken now**: a discriminator that specialises two
  families cannot say which one produced the number, and this one is meant to be cheap.
* **A pre-decoded EA in the decoded-op entry.** The real design's centrepiece — resolve the
  EA once at fill time and store it. It changes the entry layout (§33.4's physical-index
  study is already queued against the same structure) and it is a *design*, which is the
  thing this experiment exists to decide whether to fund.
* **Inlining the accessor's resolver by hand.** Worth ~4 instructions per access over
  calling `get_long_mmu040()` and letting gcc decide. Not taken: it would put a second copy
  of rung 2's fast path in the tree, and `accessor-fastpath.md`'s own rule is that there is
  exactly one copy of the fork's identity-mapping assumption.
* **The indirect branch's misprediction.** *Not a not-taken item — an unpriced gain.* Site A
  removes a `blx` through a pointer with ~1 867 possible targets from a Cortex-A9 BTB that
  holds one target per branch site. A flush there is worth several cycles that no
  instruction count in this document sees, and it is charged at zero in both bands. It is
  named the way rung 3 named its own stall-recovery possibility: **a mechanism by which the
  measurement may exceed the model, stated in advance, and not folded into the number.**
