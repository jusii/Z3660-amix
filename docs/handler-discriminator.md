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

---

## As built — 2026-08-23, counted from the lean binary

Everything above this line was written and committed (`d7f5470`) before the code existed
(`87d3f49`). This section is what the build turned out to be. **It corrects two of the
pre-registration's own numbers and leaves the bands alone** — a band that moves after the
binary exists is not a band.

### The two things gcc decided, and only one of them went the pre-registered way

| pre-registered | as built | |
|---|---|---|
| the specialised body is inlined into `m68k_run_mmu040` | **inlined** — no `z3660_spec_move` symbol exists, and the accessor calls it makes appear directly inside the run loop | **as designed** |
| site **D**: the accessor's call frame is removed *if gcc inlines it here* | **NOT inlined.** `uae_mmu040_get_long` / `_put_long` / `_get_word` / `_put_word` / `_get_byte` / `_put_byte` / `_get_iword` are all still `bl` from inside the run loop | **site D is not taken**, so `R` lands at the **low** end of its 7–11 % band |
| the disarmed hot path grows by **exactly 2** ARM instructions | **+5** | **MISS — understated 2.5×**, see below |

### The disarm penalty is +5 instructions, not +2, and the mechanism is register pressure

Counted along the same taken path in both binaries — dopc hit on way 0, not an idiom, the
switch disarmed, a cadence-4 pass that does not poll:

| | pre-discriminator (`DEC21748`) | as built |
|---|---|---|
| run-loop hot path, disarmed | **96** | **101** |

The five, named:

```
+1   the way-0 hit now reaches the fill-in block through `mov fp, lr` instead of
     falling into it -- the way index no longer survives in the register it used to
+1   `str r1, [sp, #12]`  -- the entry's meta byte is SPILLED
+3   the M_SPEC test is `ldr r3,[sp,#12]` / `tst r3,#4` / `bne`, because the byte has
     to come back from that spill slot
```

**The pre-registration's 2 assumed the meta byte would still be in a register at the
dispatch.** It is not, and the first build was worse: gcc rematerialized the address and
re-loaded the byte, costing 4. Hoisting the three reads into one local
(`z3660_dopc_meta`) removed the reload and left the spill — because the inlined MOVE body
raised the run loop's register pressure to the point where an 8-byte-larger frame and a
spilled byte are the allocator's answer. **That is a second-order cost of inlining into
the loop, it is exactly the class the footprint tables price at zero (§34.5), and here it
is the larger half of the charge.**

**The added-work band, re-derived at 5 and offered beside the pre-registered one rather
than in place of it:**

```
5 instr per dopc hit x 0.7834 = 3.9 ARM instructions per guest instruction
   on a dispatch of ~180-215                    ->  2.3 .. 2.8 % of instructions
count model, constant IPC                       ->  -2.3 .. -2.8 %
at rung 3's MEASURED added-work rate (~0.26x)   ->  -0.6 .. -0.7 %
                    pre-registered band was     ->  -0.25 .. -0.90 %
```

> **The verdict rows do not move.** The cross-build row against `DEC21748` is the shipping
> number and the bands above are scored against it; the disarm penalty only sets how far
> the same-boot switch overstates it. **Score the pre-registered −0.25 .. −0.90 % as
> written** — it is a miss if the arm comes in below −0.90 % — and use −0.6 .. −2.8 % as
> the expectation, not as the band.

### Footprint

| symbol (lean) | before (`DEC21748`) | after |
|---|---|---|
| **`m68k_run_mmu040`** | **2 288 B** | **3 516 B — +1 228 B, +307 ARM instructions** |
| run-loop stack frame | `sub sp,#68` | `sub sp,#76` |
| core1 `Z3660_emu.elf` text | 6 455 628 B | **6 457 132 B** (+1 504 B = +376 ARM instructions, +0.023 %) |
| new always-hot data | — | **4 bytes** (`z3660_spec_on`, `.data`). **No new table.** |
| `BOOT-lean.BIN` | 12 795 204 B, `DEC21748` | 12 795 196 B, **`6ADABE4A`** |

> **The run loop grew, and rungs 2 and 3 were both able to say it did not.** +1 228 B is
> the specific I-cache regression the map warns about, and it is unavoidable here: the
> whole point is to remove the *call*, which means the body has to be in the loop. Against
> a 32 KB L1I on the Cortex-A9 the loop is still ~3.4 KB, so the headroom is real — but it
> is headroom, not an argument, and if the armed arm reads null this is the second thing
> to suspect after coverage.

**No new always-hot table**, which matters because F6 is still open and rung 3 demoted its
D-cache candidate on exactly this axis. The verdict is a bit in a byte the hit path already
loads; the only new state is one 4-byte flag.

### Images

| image | bytes | `zlib crc32` |
|---|---|---|
| `BOOT.BIN` | 12 795 204 | `56EDCFD5` |
| `BOOT-lean.BIN` | 12 795 196 | **`6ADABE4A`** |
| `BOOT-prof.BIN` | 12 811 660 | `8B5DC5C3` |

The archive (`76fa64b`) preserved every outgoing image, **including `BOOT-lean-DEC21748.BIN`
— the reference arm this experiment is scored against.** It is on the host and on the card;
step 1 of the metal protocol does not need a rebuild.

### The guard

| variant | before | after |
|---|---|---|
| default | 3 186 | **3 288** |
| lean | 3 182 | **3 284** |
| profiling | 3 384 | **3 486** |

(The other three harness binaries — SCSI CDB, boot-mode and AMIX-RAM — are unaffected at
34 / 75 / 81.)

**The sweep is 6 052 encodings × 3 arms**, and it found nothing to report only after it
found two things during development. The mutation arm passes: with a long `MOVE` leaving V
set, 30 of the 36 swept shapes diverge and the six `MOVEA` shapes correctly do not, because
`MOVEA` sets no flags at all — a test that demanded all 36 would have been asserting the
wrong thing.

**One instrument change was needed and is not cosmetic.** `DOPC_EXT` counts extension words
the cache supplied, and its counter lives in the accessor's window arm — which the
specialised path no longer enters. It is therefore counted in the specialised path too, so
the counter keeps its meaning and stays comparable with every capture taken before this
experiment. `Z3660_PROF_CNT` compiles out below the profiling build, so the shipping image
pays nothing. **`FETCH` / `IFETCH_CALLS` is the counter that genuinely falls**, as declared
above.

---

# THE METAL VERDICT — 2026-08-23: **OFF-SCALE MISS**

> Everything above this line is the pre-registration and the *As built* correction to it,
> both written before the row existed. This section is the row. **Nothing above it has been
> edited**, which is the only reason the table below is worth reading.

Session 2026-08-23 14:29–15:34 EEST, A4000 + Z3660 metal, preset 7 (`amix_ram 128`,
`service_cadence 4`), root = piscsi card 1, installed 040 AMIX. Lean cadence-4 dhrystone,
median of five runs per arm, the estimator declared before the reference row was taken.

> ## The rung fires exactly as designed and makes the machine **7.55 % slower**.
>
> Coverage is **21.12 %** of guest instructions — dead centre of the pre-registered 18–27 %
> band — so this is not a build failure and not a non-event. The specialised body runs on
> one guest instruction in five, and every one of those dispatches gets **~33 % slower**.
>
> The pre-registration has three cells — NULL (±0.68 %), PAY (≥ +0.90 %), UNRESOLVED
> (+0.68 .. +0.90 %) — and all three assume the armed arm is **at worst flat**. A large
> negative is off the bottom of the scale, **6.87 pp below the NULL floor**. No cell applies.
>
> **The rung is not funded.** That is the conservative default and it is unambiguous.
> **But the classification was not bought.** The discriminator was built to read the *sign
> of a removal*. What it measured is that this particular inline body is a **worse
> implementation** than the generated handler it replaced. `HANDLER` is still unclassified.

## The rows

| arm | runs | **median** | mean | spread |
|---|---|---|---|---|
| boot 0 — as-found `86C0BC18` (rung-3 lean, superseded) | 8219 / 8162 / 8185 | 8185 | 8188.7 | 0.70 % |
| **boot 1 — REF `DEC21748`** | 8197 / 8141 / 8197 / 8197 / 8174 | **8197** | 8181.2 | 0.68 % |
| **boot 2 — `6ADABE4A`, SPEC ON — THE ROW** | 7578 / 7578 / 7578 / 7562 / 7552 | **7578** | 7569.6 | 0.34 % |
| boot 2 — `6ADABE4A`, SPEC OFF (same boot) | 8085 / 8119 / 8098 / 8109 / 8119 | 8109 | 8106.0 | 0.42 % |
| boot 2 — `6ADABE4A`, SPEC ON, **return leg** | 7547 / 7583 / 7573 | 7573 | 7567.7 | 0.48 % |

```
cross-build   SPEC ON  vs DEC21748 : 7578/8197 = 0.92448  ->  -7.55 %   <- THE VERDICT NUMBER
disarm        SPEC OFF vs DEC21748 : 8109/8197 = 0.98926  ->  -1.07 %
same-boot     SPEC ON  vs SPEC OFF : 7578/8109 = 0.93452  ->  -6.55 %
identity      0.98926 x 0.93452    = 0.92448  ==  0.92448 measured   CLOSES TO FIVE DIGITS
```

The as-found row reproduces the previous session's measurement of the **same image** to the
digit (mean 8188.7 against 8188.7) and the reference row reproduces its `DEC21748` median of
8197 exactly, so the bands are judged against a live rig. The return leg reproduces the first
armed arm to −0.07 % median / −0.03 % mean: the effect is the switch's, and it is reversible.

## The scored pre-registrations

| # | pre-registered | measured | verdict |
|---|---|---|---|
| **1** | **THE VERDICT BAND.** NULL within ±0.68 % (8141–8253); PAY ≥ +0.90 % (≥ 8271); UNRESOLVED 8253–8271 | **−7.55 %** — median 8197 → **7578** (mean 8181.2 → 7569.6, −7.48 %) | **OFF-SCALE MISS** — 6.87 pp below the NULL floor; no cell applies |
| 1b | this document's own prediction, written down to be scored: **dependency-chained ⇒ PAY** | the armed arm is a large **regression** | **REFUTED**, and not in the way a null would have refuted it |
| 1c | the count model, context not band: +1.25 .. +2.27 % (8287–8371) | −7.55 % | model wrong in **sign** |
| **2** | added-work band: the SPEC-OFF arm sits **0.25 – 0.90 %** below `DEC21748` | **−1.07 %** median (mean −0.92 %) | **MISS** — 0.17 pp below the floor |
| 2b | the *As built* corrected expectation beside it, **−0.6 .. −2.8 %** | −1.07 % | **INSIDE** |
| **3** | **`SPEC_HIT / INSNS` 18 – 27 %** — read FIRST | **21.12 %** (20 700 631 / 98 025 213); 0.00 % disarmed; 20.90 % with `DFAST` off | **HIT** — mid-band, engagement pinned both ways |
| **4a** | `DOPC_HIT` must not move > ±0.5 pp across the switch | 84.56 % vs 85.04 % = **−0.47 pp** | **HIT**, at the very edge of the band |
| **4b** | `DFAST_HIT` and the dpagecache rates must not move; the two switches independent both ways | DFAST 99.38 / 99.40 (−0.01 pp); dpagecache read 99.44 / 99.45, write 99.53 / 99.53; `DFAST_HIT = 0` with DFAST off; `SPEC_HIT` 21.12 → 20.90 % across the DFAST switch | **HIT** — independent in both directions |
| **4c** | `FETCH` / `IFETCH_CALLS` **WILL FALL** (declared, not a defect) | per guest instruction **0.5233 vs 0.5109 = +2.44 %** — it **ROSE** | **MISS**, and in the opposite direction to the declaration |
| **4d** | `IPAGE_HIT` / `IPAGE_MISS` must not move (±0.1 pp) | 88.51 % vs 88.66 % = **−0.150 pp** | **MISS** — 0.05 pp outside; same sign as 4c and the same cause |
| **5** | identity `cross-build × disarm = same-boot switch`, to **three** digits | 0.98926 × 0.93452 = **0.92448**; measured cross-build **0.92448** (mean basis 0.92524 = 0.92524) | **HIT — to five digits, on both bases** |
| **6** | *if the armed arm reads null*, suspect (a) coverage, (b) I-cache — in that order | the arm did not read null; **(a) is cleared** (21.12 %, mid-band) and the loss localises to `HANDLER`, not to a global slowdown | the pinned order was **followed and it worked** |
| **7** | the pre-registered failure mode: *the body serves 20 %, the handler bodies get measurably **shorter**, and dhrystone does not move* | the body serves 21.12 % and the **`HANDLER` bucket got 14–16 % LONGER**; dhrystone moved, downward | **the failure mode did NOT fire** — a third outcome the pre-registration did not enumerate |
| — | *As built* Site-D note: the accessors did not inline ⇒ read `R` at the **LOW** end of 7–11 % | confirmed statically — all six accessors still `bl` from the loop | **HIT** (and `R` never got the chance to matter) |

## Where the 6.55 % went — all of it inside `HANDLER`

Same-boot SPEC switch, prof image `8B5DC5C3`, stage buckets armed, corrected cycles **per
guest instruction** (the two ~11 s windows are not the same length, so nothing raw is
compared). The price is swept over this capture's own usable band: the firmware refuses the
boot line's 66 as arithmetically impossible here and prints a ceiling of 60, bound by
`TAILADV`.

| bucket | SPEC ON | SPEC OFF | delta @54 | delta @60 |
|---|---|---|---|---|
| **HANDLER** | **105.07** | **91.97** | **+14.25 %** | **+16.25 %** |
| DOPCFILL | 19.79 | 17.84 | +10.90 % | +11.69 % |
| TAILSPEC | 2.07 | 1.89 | +9.16 % | +9.33 % |
| FETCHEX | 57.97 | 56.54 | +2.52 % | +2.62 % |
| FETCHOP | 21.41 | 21.08 | +1.59 % | +1.63 % |
| WRITE | 34.35 | 33.71 | +1.90 % | +2.00 % |
| DOPCFIND | 77.83 | 76.75 | +1.40 % | +1.52 % |
| READ | 43.77 | 43.75 | +0.04 % | +0.03 % |
| LOOPRES | 56.64 | 56.86 | −0.38 % | −0.78 % |
| **corrected TOTAL** (instrument excluded) | **496.52** | **477.05** | **+4.08 %** | **+4.83 %** |

**Nothing got cheaper.** Not one bucket moved down by more than the 0.4–0.8 % `LOOPRES`
residue, and the bucket the rung exists to shrink is the one that grew most. The counters
say the *work* is identical: READ/INSN 0.4215 vs 0.4219, WRITE/INSN 0.3007 vs 0.3010,
XLATE/INSN 0.01700 vs 0.01683, FAULTS 3630 vs 3629. **Same work, more cycles, all of it in
one bucket.**

Per served instruction, two independent derivations:

```
A. from the lean wall clock (the shipping instrument)
   time ratio ON/OFF 1.07007 over 21.12 % coverage  ->  served dispatches +33.2 % SLOWER
   at ~71.8 ARM cyc per guest instruction (lean, disarmed) -> ~ +24 cyc per served instruction

B. from the bucket capture (the localising instrument)
   HANDLER +13.10 cyc/INSN over 20.15 % coverage    ->  +65 cyc per served instruction,
                                                        inside the HANDLER bracket alone
```

The two instruments disagree in size — the bucket capture's probes inflate everything and
the +65 sits inside a bracket that also carries the probe pair's own scaffolding — and agree
completely in **sign and location**. Against the model: this document predicted the served
dispatch would get **7–11 % cheaper**. It got **33 % more expensive**: sign wrong, magnitude
3–4× the predicted size in the other direction.

## The mechanism — the body is 2–3× the handler it replaced

Counted from the shipped lean ELF that produced `6ADABE4A`
(`arm-none-eabi-objdump -d`, every instruction issued, predicated-not-taken ones included).
The **served path** is traced through the inlined body for the four shapes the *Pre-registered
arithmetic* table priced; the **replaced path** is the run loop's dispatch triple plus the
generated handler plus the result move:

| shape | served SPEC body | generated-handler path replaced | ratio |
|---|---|---|---|
| `MOVE.L Dn,Dn` (`op_2000`) | **65** | 4 dispatch + 15 handler + 2 join = **21** | **3.1×** |
| `MOVE.L d16(An),Dn` (`op_2028`) | **85** | 4 + 24 + **9 window** + 2 = **39** | **2.2×** |
| `MOVEA.L d16(An),An` (`op_2068`) | **77** | 4 + 19 + **9 window** + 2 = **34** | **2.3×** |
| `MOVE.L (An)+,(An)+` (`op_20d8`) | **118** | 4 + 37 + 2 = **43** | **2.7×** |

*dispatch* = `add r3,fp,#6` / `ldrh r0,[r4,#84]` / `ldr r3,[r5,r3,lsl #2]` / `blx r3`;
*join* = the `mov r7,r0` / `b` that lands the handler's return on the tail; *window* = the
nine instructions `uae_mmu040_get_iword()` spends on a window hit, which is what site C
removes and which both `d16(An)` shapes pay in the reference arm. The four accessor calls
the body itself makes (`get_long_mmu040` and friends) are common to both arms and are
excluded from both columns.

On a dispatch of ~180 ARM instructions that is **+24 % to +40 % of the whole served
dispatch**, which is where the measured **+33 %** comes from. The model's `R` was
**7–11 % removed**; the binary's `R` is **40–100 % added**.

**Why, in one line: `gencpu`'s per-opcode handlers ARE the specialisation, and an inline
body that takes the opcode word at *runtime* is a DE-specialisation.** `op_2000_31_ff` is
15 instructions of straight-line code with **no call frame at all** — gcc made it a leaf
ending in `bx lr`, so §30.3 site A's "handler's `push`/`pop`" is worth **zero** on the
commonest shape. The specialised body spends **27 instructions on its prologue alone** —
five field extractions, the size ladder, the `srcmem`/`dstmem` tests, the length
computation — before it touches a register or a byte of guest state. It then walks a
six-way source ladder and a six-way destination ladder that gencpu resolved at compile
time. The four sites §30.3 priced as removals are real, and they are dwarfed by a mode
ladder the pre-registration never counted because it assumed the switches would be folded.

**And the design's Site C did not materialise as specified.** `z3660_spec_iword()` takes its
offset as a *runtime* value (`srcmode == 5 ? 4 : 2`), so the `have_ext0` test cannot fold and
both arms survive: **two `bl uae_mmu040_get_iword` call sites remain inside the specialised
body**, taken whenever the entry does not describe the word (`M_EXT0` clear, or the
destination displacement at `pc + 4` when both EAs are `d16(An)` — a word the entry cannot
hold). A static census of the whole run loop counts **nine** `get_iword` sites; seven of them
pre-date this experiment (the decoded-op fill's own ext0 fetch and rung 0's block-idiom
recognizer) and **two are the body's**.

## Finding 4c was arithmetically impossible as declared — and that is this document's error

The pre-registration declared, twice and emphatically, that `FETCH` / `IFETCH_CALLS` would
**fall** by the number of served `d16(An)` instructions. **It could not.** Site C removes a
*window hit*, and `uae_mmu040_get_iword()` answers a window hit **above** the bracket:

```c
if (addr == (uaecptr)z3660_dopc_ext_pc) {
        Z3660_PROF_CNT(Z3660_PROF_C_DOPC_EXT);      /* counted here ... */
        return (uae_u16)z3660_dopc_ext_val;
}
Z3660_PROF_ENTER_IFETCH();                          /* ... never reaching FETCH */
Z3660_PROF_CNT(Z3660_PROF_C_FETCH);
```

A served window hit never incremented `FETCH` in either arm, so removing the call could
only move `DOPC_EXT` — and `DOPC_EXT` was deliberately re-counted inside the body to keep
it comparable, which is why it reads **0.3385 vs 0.3391, unchanged, exactly as designed.**
The declaration confused "the accessor is not called" with "the fetch is not paid"; the
cache had already stopped paying it.

What actually moved `FETCH` is the miss rate. The armed arm takes **3.2 % more decoded-op
misses per dispatch** (0.15439 vs 0.14963, the −0.47 pp of check 4a), and a miss costs two
real instruction fetches a hit does not: the run loop's own `x_prefetch(0)` and the
extension lookahead. `+0.00476 misses × 2 = +0.0095` fetches per guest instruction against a
measured `+0.0124` — the right size and the right sign, and it carries `IPAGE` (4d) with it
for the same reason. **4c and 4d are one finding, and it is a bookkeeping error in this
document, not a defect in the build.**

## The owed side-measurement — THE FILL-PATH PRICE

Taken while the rig was up, because it replaces the stale **62–64 cyc/miss** input the
physical-index decoded-op study (map §33.4) is sized against.

```
DOPCFILL corrected cyc/miss = (acc[15] - spans[15] x price) / DOPC_MISS

  reference posture (SPEC OFF, prof 8B5DC5C3)   75.9 .. 81.9 cyc/miss
                                                at calibration band [54, 60] cyc/transition
  armed arm     (SPEC ON)                       84.0 .. 90.0 cyc/miss, same band
```

**The stale input is refuted UPWARD.** Even 75.9 — the cheapest end, taken at the ceiling
price, the most aggressive correction this capture permits — sits above the expensive end of
62–64. The fill path is **19–32 % dearer** than the design has been sized against. Fills run
0.218 per guest instruction and are **4.13 %** of the corrected profile. The band is part of
the number: this capture's printed ceiling is **60, bound by `TAILADV`**, and the firmware
rejects the boot line's in-bucket end of 66 as arithmetically impossible here. Provenance,
so it is not carried further than it was earned: measured on the **SPEC prof build**
`8B5DC5C3` with the switch **disarmed**, not on `DEC21748` itself.

## What this closes, and what it does not

**Closed.** The rung is not funded. Bands 1, 1b, 1c and 2 are refuted **as built**, and
4c/4d are refuted as *declarations* — the fault there is this document's.

**Not closed, and this is the honest half.** The experiment's premise was that the body
*removes* work. It adds work — 2–3× the path it replaces, on every shape measured. Every
band downstream of that premise is therefore measuring **the implementation**, not the
bucket. You cannot conclude "`HANDLER` is dependency-chained" from a regression, and you
cannot conclude "`HANDLER` is latency-hidden" either: a **null** was the reading that would
have said that, and this is not a null. **`HANDLER` remains unclassified.** The ladder's #1
rung is neither struck nor funded — it is unbought, and it now costs a rebuild to buy rather
than a measurement.

---

# THE GATE, AND THE VERDICT ON THE CODE — 2026-08-23, **REVERTED**

The metal row above decides the *rung*; it does not decide what to do with the *code*. That
was put on a hard static gate, checked on the built lean ELF, with reverting mandatory if it
could not be met:

> **(a)** ZERO fetch-accessor calls reachable from the served `SPEC` body.
> **(b)** the served body's instruction count **LESS THAN** the generated handler path it
> replaces.
> **(c)** sp-relative accesses in the body counted and stated.

## The gate, measured

Both loops disassembled with `arm-none-eabi-objdump -d`; `6ADABE4A` is the image metal
measured, `DEC21748` the reference arm.

| `m68k_run_mmu040` | reference `DEC21748` | as built `6ADABE4A` | delta |
|---|---|---|---|
| instructions | 572 | 879 | **+307** |
| `bl uae_mmu040_get_iword` sites | **7** | **9** | **+2** |
| sp-relative accesses | 65 (11.4 %) | 108 (12.3 %) | **+43** |
| frame | `sub sp,#68` | `sub sp,#76` | +8 B |

**(a) FAILED — 2, not 0.** The static census that prompted this round counted *nine*
`get_iword` sites in the run loop; **seven of them pre-date the experiment** (the decoded-op
fill's own ext0 fetch, and rung 0's block-idiom recognizer, which walks up to four
instruction words looking for a `CPUSHL` loop head). **Two are the body's**, and they are
real: `z3660_spec_iword()` takes its offset as a *runtime* value, so the `have_ext0` test
cannot fold and both arms survive into the binary.

**(b) FAILED, by 2.2–3.1×** — the table under *The mechanism* above. 65 / 85 / 77 / 118
against 21 / 39 / 34 / 43. The requirement was *shorter*.

**(c) STATED.** 108 sp-relative accesses in an 879-instruction loop — but **65 of them were
already there**, so the body's own contribution is **+43**, of which 34 sit inside its own
span and the rest are the pre-existing loop spilling harder under the added pressure (four
of those are the *As built* +5 disarm charge: the meta-byte spill store and its
three-instruction reload). **The 108 was smoke; +43 is the measurement.**

## Why the gate cannot be met — priced, not argued

**(a) is fixable and (b) is not**, and the second is what decides it.

Fixing (a) means never letting the body need a word the entry does not hold: require
`M_EXT0` before setting `M_SPEC` on a `d16(An)` shape, and drop `d16(An) → d16(An)`
altogether. **The entry cannot be extended to cover it.** The decoded-op set is *exactly*
64 bytes — `tag[4]` 16 + `stamp` 4 + `victim` 4 + `handler[4]` 16 + `opcode[4]` 8 +
`ext0[4]` 8 + `meta[4]` 4 + `pad[4]` 4 — behind a compile-time tripwire that refuses any
build where it is not, because one set is one pair of Cortex-A9 cache lines and that is the
property the hit path is designed around. A second extension word costs **8 bytes** and
**4 are spare**. So the second word is out, by the geometry, not by preference.

**(b) cannot be met at all**, and the reason is structural:

1. **The excess is a runtime mode/size ladder.** The body spends **27 instructions on its
   prologue alone** — five field extractions, the size ladder, the `srcmem`/`dstmem` tests,
   the length computation — before it touches guest state. `op_2000_31_ff`'s *entire*
   handler is 15 instructions.
2. **Getting under the handler therefore requires compile-time specialisation per shape** —
   36 straight-line bodies — because that is precisely what `gencpu` already did and what a
   single body taking the opcode at runtime undoes.
3. **Selecting among 36 inlined bodies is a jump table**, i.e. an indirect branch with 36
   targets on a Cortex-A9 BTB that holds one target per site. **That is §30.3 site A** — the
   largest removal the pre-registration priced. Removing the handler's indirect call by
   adding the body's own nets zero.
4. **What is left to remove, counted from this binary:**

```
   handler call frame        0 on op_2000 (a LEAF -- gcc ended it in `bx lr`), 2 elsewhere
   the loop's dispatch triple   3   -- but the jump table needs ~3 of its own
   site B, field re-extraction  2-3
   site C, the window hit       9   -- on the ~45 % of served shapes with a displacement

   weighted  0.45x(9+2+3) + 0.55x(0+3)  =  ~8 removed, ~3 added back  =  ~5 net
   on a dispatch of ~180 instructions                     ->  R ~= 2.8 %   (was 7-11 %)
   x 21.12 % measured coverage, constant IPC              ->  +0.59 %
   pre-registered PAY floor                                   +0.90 %
   the machinery's own measured cost                          -1.07 %
   ---------------------------------------------------------------------
   best case for a gate-compliant body                    ->  about -0.5 %
```

5. **A compliant body's own arithmetic still loses**, before any deflator — and this round
   has just demonstrated the count model optimistic by 3–4× in the other direction, while
   36 inlined bodies would raise the register pressure that produced the −1.07 % in the
   first place.

So the gate is unmeetable inside the entry's fields, and a body that met (b) would no longer
be this experiment: it would have given back site A to buy sites B and C, and the arithmetic
says that trade is a loss.

## The consequence

**The `SPEC` machinery is removed in full**, rung-3 style and for rung 3's reason: an
experiment that has returned its answer should not be left in the tree behind a switch,
because the switch is not free and the answer does not improve with age.
`Z3660_DOPC_M_SPEC`, `z3660_spec.h`, the inline body, the `SPEC` console command, the
`spec_on` mailbox word (SHARED's next free offset is `0xFFFF00F0` again), `SPEC_HIT`
(`Z3660_PROF_C_COUNT` back to **46**) and the harness's SPEC corpus all go.

**Proof that the removal is exactly a removal, and nothing else:** rebuilding the reverted
tree reproduces all three pre-experiment images **byte for byte** —

| image | bytes | `zlib crc32` |
|---|---|---|
| `BOOT.BIN` | 12 795 220 | `E687C5CF` |
| `BOOT-lean.BIN` | 12 795 204 | **`DEC21748`** |
| `BOOT-prof.BIN` | 12 811 676 | `4183F88C` |

`BOOT-lean.BIN` is now byte-identical to the image that produced the reference row this
document is scored against. *(The `BOOT-prof.BIN` row is the revert commit's. The very next
commit reworded the `DOPCFILL` gap line — a profiling-build-only change unrelated to this
experiment — so a prof image built from a later tree reads `2841AD27`. The other two stay
byte-identical.)*

> # PARKED — `HANDLER` remains unclassified.
>
> A classification attempt requires a body that **provably removes work**. Two compiler
> strikes and an off-scale loss say **the generated handlers are better than they look**:
> gcc left the accessors out of line (site D, strike one), it did not fold the extension-word
> test (site C, strike two), and it had already turned the commonest handler into a
> 15-instruction leaf with no frame to remove at all (site A, worth zero there).
>
> **What a future attempt would have to bring**, so it is not re-derived: compile-time
> per-shape bodies **and** a dispatch that is not an indirect branch — which on this loop
> means the pre-decoded EA in the entry plus a threaded dispatch. That is the *design* this
> experiment existed to decide whether to fund, so it cannot be the discriminator for it.
> The cheap classification of `HANDLER` is not available; the map's #1 rung is still
> unbought, and it now costs a design round rather than a measurement.
>
> **What this round did buy, and it is not nothing:** the fill-path price (75.9–81.9
> cyc/miss, refuting the stale 62–64 upward), the `DOPC_EXT`-vs-`FETCH` bookkeeping error in
> finding 4c, the −1.07 % standing cost of inlining *anything* into this run loop, and a
> counted demonstration that instruction-path length in the run loop is dominated by
> ladders the pre-registration did not count.
