# Plan WARP ENGINE — a JIT under the MMU

*Named for MacroSystem's WarpEngine, the 68040 accelerator an A4000 took in the same slot this
card sits in. The interpreter campaign took the emulated 68040 from 3 818 to 8 185 dhrystones
(2.14×) and closed with one row left on the board. This is the plan for that row.*

> ## **STATUS: `M1 CLOSED — PARKED AT GENERATION-ONE SCOPE`, by user decision 2026-08-24.**
>
> Six rounds, five metal sessions, **zero correctness failures**, and **no verdict in either
> direction** — the kill line's 60 % coverage precondition was never met, so §0.2's kill line
> is **armed, unfired and uncleared**. The armed row rose **5 301 → 7 185 → 7 536 → 7 563**
> dhrystones — each against its own session's reference, **0.8778× → 0.92432× → 0.92401×** —
> and never crossed 1.0×; coverage topped out at **20.26 %** against the 60 % bar. **Round 6
> landed the dormant-tax fix**
> (§§0.17 – 0.18), so the engine now ships **compiled in, disarmed, and — on every static
> measure — free**: the disarmed run loop is instruction-for-instruction the loop of an image
> built with no engine at all, and the `struct regstruct` layout term collapsed from **7 912
> changed function bodies to 466**, the toolchain's own ambient churn. **Nothing was flashed;
> the card's standing image is unchanged.** §0.19 is the park: what it preserves, and what
> reopening would require — **an `E`-round before any new families**.

**Status:** `M0 DONE`, **`M1 round 1 DONE`** — the grill ran on **2026-08-23** and every question
in §7 is answered; the metal half ran the same day and **Q2 came back `TERMINATED`** (§0.4.1) with
the §1.5 base re-taken at **48.68 – 49.91 %** (§0.4.2); **§0.5 is the committed M1
pre-registration**, the last of M0's seven items. **M1's first round shipped on 2026-08-24**: the
engine's skeleton end to end with **no code generation at all** — §0.6 pre-registers it and §0.7
scores it, and its one number is a *cost*, not a speed. **`M1 round 2 DONE`** — §0.8
pre-registers it and §0.9 scores it: the first round that generates code. The lookup structure that
recompiled 3.07 million times is fixed and the fix is proven both statically and against its own
mutation; the entry cost is amortised over real blocks and re-measured; and 7 616 opcodes are
compiled and compared against the interpreter, instruction for instruction, over 10⁸ boundaries.
Its numbers are still costs and static counts -- **round 2 takes no metal row, so the kill line
cannot fire on it**. §0 is the freeze: it is the authority for everything below it, and where a §2 claim
disagrees with §0's censuses, §0 wins and the §2 text has been corrected in place with the
correction marked.

**`docs/warp-gen2-prospectus.md`** prices the M1 fork — park generation one, or fund a generation
two — as scenarios against the R5 metal rows, now scored in full at **§0.16**. It is a paper
instrument, it carries no recommendation, and it scores nothing here: no number in this document
is moved by it. **The fork itself was with the user, and it was ruled on 2026-08-24: `PARK + TAX
FIX`** — the prospectus's §8 row 2. Round 6 landed the fix and §0.19 is the park. The prospectus
still decides nothing and still scores nothing; it priced the options and the choice was made
outside it, which is what its §9 says must happen.

**Status-ledger convention:** every item is `TODO` / `IN PROGRESS` / `DONE` / `GATED(<on what>)`,
as in `docs/AMIX.md` and the C2 rung docs. This document is the tracker for the project; it is
updated as items move, and its pre-registrations are never rewritten after the fact.

**Confidence convention.** Every claim below carries one of three tags, and the tag is part of
the claim:

| tag | means |
|---|---|
| **measured** | a number taken off metal, off a capture, or off a disassembly — with the doc, file or commit it came from |
| **derived** | arithmetic over measured numbers, with the arithmetic shown |
| **assumed** | neither. Every `assumed` in this document is either an open question in §7 or something a milestone gate will settle. |

There are no unnamed magic numbers. Where a number has a band, the band is quoted, and where a
band is a price artefact rather than a quantity, it says so.

## The evidence base

Everything in this plan traces to these. Where a claim traces to none of them, it is tagged
`assumed`.

| source | what it supplies |
|---|---|
| `docs/decoded-op-cache.md` | the dopc's geometry, keying, generation machinery, invalidation causes, and the campaign's scored lessons |
| `docs/accessor-fastpath.md` | rung 2 — the DTT verdict table, the inline data-access path, the TTR-call mechanism find |
| `docs/ifetch-fastpath.md` | rung 3 — the ITT verdict table, and **the pricing law** |
| `docs/handler-discriminator.md` | why `gencpu`'s generated handlers are already the specialisation, and what a future attempt must bring |
| `docs/f-measurement.md` | `f` = 0.0787, the physical-index strike, and exactly what it does and does not license |
| `docs/block-idiom-fastpath.md` | rung 0 — the chunked-execution pattern, the page-bounded validity gate, the yield contract |
| `docs/profiler.md` | the 17-bucket phase machine, band comparability, price artefacts |
| `docs/AMIX.md` | the service-cadence contract and its measured couplings |
| `docs/warp-objdiff.py` | the function-granularity object diff §0.7's disarmed-equivalence number is read from |
| `docs/core0-loop-blocking-audit.md`, `docs/piscsi-service-path.md` | core0's cooperative loop, its blocking paths, and the latency budget |
| `CHANGES.md` + `0a4c064`, `e37bb43`, `8be8949` | the AMP coherence history and the JIT's own I-cache lesson |
| the 68040 port lane's `docs/C2-ATTACK-MAP.md` §§30–47 | the pricing law, the ladder, and **row J** |
| `Z3660_emu/src/uae/` (`newcpu.cpp`, `cpummu.cpp`, `z3660_dopc.h`, `z3660_dfast.h`, `jit/`) | what exists today |

---

# 0. Grill decisions — design freeze (2026-08-23)

The grill ran on **2026-08-23** against §7's thirteen questions. All thirteen are answered.
Each §7 entry now carries a `RESOLVED` line pointing here, with its original recommendation
and cheapest-experiment text left **byte-untouched** beside it — a recommendation rewritten
to match its own outcome is not a recommendation, and §3.4 makes that a rule rather than a
preference.

**This section is the authority.** §§1–6 keep their reasoning; where one of their claims is
refuted by the censuses in §0.3 it has been corrected in place and the correction marked
`(corrected at M0)`. Nothing pre-registered has been rewritten.

## 0.1 The decisions

### Q1 — route: **C, the hybrid** `DECIDED`

Keep the plumbing that is already proven on this silicon: the raw ARMv7 emitters
(`jit/codegen_arm.h`), the block manager, the code-cache allocator, `flush_cpu_icache`, the
flags machinery, and the register-allocator core. **Replace the `jnf_MEM_*` midfunc helpers**
with the MMU verdict-table fast path plus bail-to-interpreter, and impose a state-sync
discipline at faultable sites. Per-opcode compilers are inherited **family by family, and
only where a family survives the bail contract** — never wholesale.

> **The contract that comes with the route, and it is a pre-registration, not an aspiration:**
> the **state-sync overhead per faultable access is a pre-registered M1 measurement**. The
> allocator gets redesigned toward Route B's shape **only if the numbers say that is the
> ceiling** — not because a redesign looks tidier from the outside.

The census that motivated the choice is §0.3(a). The grill recorded its finding as *"the memory
model is confined to the `jnf_MEM_*` layer, all emitting via `R_MEMSTART`"*, and that
confinement is what makes "replace the layer" a route rather than a wish — inheriting a
per-opcode compiler means inheriting whatever memory model it emits, and the whole bet is that
there is only one such model and it lives in one place.

**The exact version is slightly less tidy than the note, and the untidiness is named here rather
than discovered during codegen.** §0.3(a): **16 `jnf_MEM_*` helpers, 14 of them emitting through
`R_MEMSTART`** — the other two are the `special_mem` arm and dispatch through the memory-bank
function-pointer table instead — plus **three sites that reach `R_MEMSTART` from outside the
layer**: `jnf_MOVE16`, the five FPU emitters in `codegen_arm.cpp`, and the one line that loads
the base register at block entry. All three are closed by exclusion rather than by porting
(`MOVE16` is a bail family; the FPU is interpreter-only by Q13; the base load is the line Route C
deletes), so the confinement holds where it has to.

### Q2 — exceptions: **design as if a throw cannot cross generated code** `DECIDED, MEASURED`

The posture is assumed-terminates: **bail before the fault, everywhere** (§2e option 2), so no
`throw` is ever raised on a stack that has a generated frame on it. Every fault-visible
decision in §2e follows from this and none of them is contingent on the probe's answer.

> **Measured on metal 2026-08-23: `B: RESULT = TERMINATED`.** The EHABI unwinder refused the
> generated frame and `std::terminate` was reached, exactly as §2e derived. The posture is no
> longer assumed-terminates; it **is** terminates. §0.4.1.

**The probe confirms or kills the posture; it does not choose it.** `make q2probe` builds
`BOOT-q2probe.BIN`, a fourth image that exists only for this question: it emits a
four-instruction ARM stub into the heap, calls it, has it `bl` into C++, and provokes an
unwind across it — once with the throw contained inside the helper and once with it crossing.
It prints its verdict on serial before emulation starts, so one boot's capture is the whole
result. See §0.4 and `Z3660_emu/src/warp_q2probe.cpp`. A negative answer here ends the project
for the price of that boot, which is exactly why it is M0's first item.

### Q3 — cache maintenance: **add ISB and a branch-predictor invalidate before JIT volumes** `DECIDED`

The `ISB` and the branch-predictor invalidate are added to the I-cache sequence **before** the
engine raises flush volume, not after a symptom appears. The **`UAEJIT` bootmode A/B is the
settling experiment** — it already runs generated code today — and it is a metal session, later.
The standing platform caveat rides with it unchanged: *adding barriers is not evidence of a fix;
only a soak on metal is.*

### Q4 — where compilation runs: **core1, inline, in bounded units** `DECIDED`

Compilation runs on core1 inline, in units bounded by rung 0's measured **~42 µs** budget.
**Core0 offload stays gated behind the unexplained 118-second core0 gap** — that investigation
closes first, or the offload does not happen.

### Q5 / Q6 — **deferred to their own pre-registered M1 counters** `DEFERRED`

Q5 (a per-address-space generation) is settled by the **`IV_ROOT` blocks-discarded ratio**; Q6
(resurrecting rung 3's `ifetch` table on the compile path) by a **compile-path fetch counter**.
Both counters are pre-registered at M1. **Neither is built early.** Re-funding either from a
bucket share before its counter exists is the one thing §30's law strikes outright.

> **Both are now pre-registered, with their decision rules and their thresholds: §0.5.6.**
> Q5 is `WARP_DISC_ROOT` (id 57) against `WARP_DISC` (id 56); Q6 is `WARP_CFETCH` (id 55).

### Q7 — generation-one families: **MOVE/MOVEA covered shapes + register-register ALU + Bcc/DBcc** `DECIDED`

Chosen **on the dhrystone window**, following Q11. The kernel-window `(xxx).L` shape — 37 % of
the `MOVE` family where the family's covered share falls to 57.6 % — is a **named M4 ladder
candidate, contingent on the boot rows**, not a generation-one family.

### Q8 — rung 0: **stays untouched through M3** `DECIDED`

The block-idiom fast path is not modified, not subsumed and not retired. **The JIT declines its
PCs.** Revisit at M4 with its own gate, and read the campaign's rule out loud first.

### Q9 — switch polarity: **`WARP` unset = OFF through M4** `DECIDED`

A deliberate inversion of the house rule, and the reason is recorded: the house rule exists so a
zeroed `SHARED` struct from an older core0 still runs *shipped behaviour*, and an unshipped
engine's shipped behaviour is "off". **Flip to unset = ON only at M5, on the user's word.**
**Arm attestation rides every capture** — arm state is read from a counter, never from a banner.

### Q10 — the cadence contract: **blocks feed the counter with retired guest instructions** `FROZEN`

**The timebase contract is FROZEN.** A block feeds the cadence counter with the number of guest
instructions it retired, so one pump tick stays `256 × service_cadence` *guest* instructions.
This is the item with **cross-repo reach** (§6.2), and the **PSTAT gap fingerprint is a proof
obligation on every metal verdict** — not a check to run if there is time.

### Q11 — the perf gate: **dhrystone, with a boot row beside it** `DECIDED`

**Dhrystone stays THE gate.** Every verdict capture additionally records **a boot-window row and
`INSNS_SUPER` beside it**. The kernel-heavy story accumulates evidence from M1 onward and
**gates nothing before M4**.

### Q13 — model-agnostic engine, interpreter-only FPU `DECIDED`

The engine is written **68k-model-agnostic**; the code-organisation review that keeps it so
happens when implementation starts. **The FPU is interpreter-only permanently** — the
vector-11 / FPSP contract of R6 is a correctness decision, not a scheduling one, and it does
not expire at M5.

### Schedule

**M0 now, and WARP runs solo** — no parallel projects until the user says otherwise.

## 0.2 The kill line — agreed, binding, and written before any code

> **At M1, with coverage in band (≥ 60 %) and no named defect: cross-build dhry ≤ 8 185 + rig
> spread (≈ 8 242) → the project STOPS and the code reverts same-day; rung-3 precedent binding;
> no re-scoping. Positive but far under the 1.25–1.55× band → continue ONLY with a named
> measured mechanism + a re-registered M1b.**

That is the whole line, quoted as agreed. Three things about it are worth saying once, here:

- **"No re-scoping" is the operative clause.** The failure mode a kill criterion exists to
  prevent is not a bad number; it is a bad number followed by a redefinition of what was being
  measured. Row J's 2.11–2.35× ceiling and §1.5's 1.25–1.55× band are both *already* written
  down and neither may be re-derived after a result.
- **"Coverage in band and no named defect" is the guard on the other side.** A miss at 20 %
  coverage is a bug report, not a verdict, and it does not trigger the kill line — but "we think
  there might be something wrong" is not a named defect either. The defect has to be named.
- **The rung-3 precedent is binding by name.** Rung 3 engaged at 91.10 % coverage, made its
  bucket 23 % cheaper per instruction, hit or beat every premise it declared, moved the wall
  clock −0.05 %, and came out **the same day**. A JIT that does the same is the same finding.

## 0.3 M0 evidence — the zero-build censuses (2026-08-23)

All three censuses read source only: no build, no toolchain, no card. Censuses (a) and (b) are
reproduced by `docs/warp-m0-census.py`, committed beside this document so the numbers can be
re-derived by whoever doubts them — including after the code they count has moved. Every number
below is **measured** unless it says otherwise, and every one carries its `file:line`.

### (a) The `jnf_MEM` census, and the family-inheritance survey Route C needs

**The layer itself.** There are **16** `jnf_MEM_*` midfuncs, all in
`Z3660_emu/src/uae/jit/compemu_midfunc_arm2.cpp` (`:7899`–`:8141`). **14 of the 16 emit through
`R_MEMSTART`**; the two that do not — `jnf_MEM_READMEMBANK` (`:8090`) and `jnf_MEM_WRITEMEMBANK`
(`:8122`) — are the `special_mem` arm and dispatch through the memory-bank function-pointer
table instead. **(measured)**

**Three leaks out of the layer, and Route C owns all three.** `R_MEMSTART` is also emitted by:

| site | what it is | consequence for Route C |
|---|---|---|
| `jnf_MOVE16` (`compemu_midfunc_arm2.cpp:4872`) | a per-opcode midfunc that computes its own addresses and does **not** go through the accessor API | `MOVE16` is a bail family regardless — its 68040 fault semantics need `MMU_FSLW_TT_16` — so this leak is closed by exclusion, not by porting |
| 5 sites in `codegen_arm.cpp` (`:979`, `:1012`, `:1038`, `:1099`, `:1117`) | `raw_fp_from_exten_mr` / `raw_fp_to_exten_rm` / `raw_fp_from_double_mr` / `raw_fp_to_double_rm` — the FPU's own memory path | closed by **Q13**: the FPU is interpreter-only permanently, so `compemu_fpp_arm.cpp` stays unreachable under the MMU |
| `codegen_arm.cpp:535` | the one *definition* site: `LDR_rRI(R_MEMSTART, R_REGSTRUCT, offsmem)` loads `regs.natmem_offset` into the base register at block entry | this is the line Route C deletes; §0.3(c) says what replaces it |

**So the memory model really is one layer, two named exclusions, and one line to delete** —
which is the finding that makes Route C a retrofit rather than a rewrite, and it is worth
saying out loud that the finding could have gone the other way. A memory model smeared across
the 3 054 per-opcode compilers would have made Route B the only honest answer. **(measured)**

**The compile table.** `compstbl_arm.cpp` holds **two** tables, `op_smalltbl_0_comp_ff` (`:15`)
and `op_smalltbl_0_comp_nf` (`:1884`), and they are structurally identical:

```
   entries per table              1 867
   NULL (no compiler at all)        340   18.21 %
   per-opcode compilers           1 527
     ... can reach guest memory   1 213   79.44 % of the compilers
     ... pure register ops          314   20.56 %
```

with **3 054** compiler bodies defined in `compemu_arm.cpp` (1 527 × 2). **(measured)**

> **Q1's own cheapest experiment, answered: "nearly all of them" is 79.44 %.** The plan said
> that if it were nearly all of them, *"Route A is a rewrite wearing a retrofit's clothes and
> the grill should say so out loud"*. It is, and the grill did. **Four fifths of the existing
> per-opcode compilers cannot be inherited without the new memory model underneath them.**

**By mnemonic family** — 69 families across the 1 527 compilers, split three ways:

- **21 fully register-only** (no entry reaches memory): `ANDSR ASL ASR Bcc DBcc EORSR EXG EXT
  FBcc FPP FScc LEA LSL LSR NOP ORSR ROL ROR ROXL ROXR SWAP`
- **18 fully memory-touching** (every entry reaches memory): `ASLW ASRW BSR CMPM JMP JSR LINK
  LSLW LSRW MOVE16 MVMEL MVMLE PEA ROLW RORW RTD RTS UNLK`
- **30 mixed** — some shapes pure, some not: `ADD ADDA ADDX AND BCHG BCLR BFINS BSET BTST CLR
  CMP CMPA DIVL DIVS DIVU EOR MOVE MOVEA MULL MULS MULU NEG NEGX NOT OR SUB SUBA SUBX Scc TST`

**This is why Q1 says "family by family" and not "family set".** The generation-one families
land on all three sides of that split:

| family | entries | reach memory | pure | what Route C inherits |
|---|---|---|---|---|
| `Bcc` | 45 | **0** | 45 | inheritable as-is — no memory model involved |
| `DBcc` | 16 | **0** | 16 | inheritable as-is |
| `MOVE` | 281 | 272 | 9 | **not** inheritable as-is; the covered EA shapes are exactly the ones that go through the accessor API |
| `MOVEA` | 24 | 18 | 6 | same |

and register-to-register ALU lives inside the **mixed** families, so its inheritance is decided
**per shape**, never per family. **(measured)**

**The accessor API, and the faultable-site density M1's cost measurement needs.** The
per-opcode compilers reach guest memory through exactly ten symbols in
`compemu_support_arm.cpp` (`:1682`–`:1809`), all funnelling into the 16 midfuncs — that funnel
*is* the choke point Route C replaces. Usage across both tables' 3 054 bodies:

```
   readword 730   writebyte 690   readlong 626   readbyte 606
   writeword 492  writelong 432   get_n_addr 56  get_n_addr_jmp 32
   writelong_clobber 24            writeword_clobber 0   (declared, never called)
   jnf_MOVE16 10   (the API bypass above)
```

```
   total guest-memory call sites   3 738
   bodies with 0 sites               628
              1 site               1 146
              2 sites              1 258
              3 sites                 12
              4 sites                 10
   mean per body                    1.22
   mean per memory-touching body    1.54
```

> **The denominator for Q1's pre-registered M1 measurement.** State-sync cost is charged **per
> faultable access**, and this says a compiled instruction has **one or two** of them in
> essentially every case and never more than four. A design that costs *n* instructions per
> faultable site therefore costs ~1.5*n* per memory-touching guest instruction — which is the
> arithmetic M1 has to beat, computed from `objdump` before any metal time, in §30.3 step 3's
> form. **(measured; the ratio is derived)**

### (b) The state-discipline census — what a bail would have to sync

**The virtual register file.** `VREGS 22` (`compemu_arm.h:157`), bound to home locations in
`init_comp()` (`compemu_support_arm.cpp:1409`–`:1425`):

| virtual | home | note |
|---|---|---|
| v0 – v15 | `regs.regs[0..15]` (`:1411`) | D0–D7, A0–A7; `INMEM` at block start |
| v16 `PC_P` (`:197`) | `regs.pc_p` (`:1417`) | **a host pointer**, seeded `set_const(PC_P, (uintptr)comp_pc_p)` |
| v17 `FLAGX` (`:198`) | `regs.ccrflags.x` (`:1420`) | |
| v18 `FLAGTMP` (`:199`) | `regs.ccrflags.nzcv` (`:1423`) | |
| v19 – v21 `S1..S3` (`:201`) | `regs.scratchregs[0..2]` (`:1414`) | the spill area §2c named |

Five statuses (`compemu_arm.h:160`–`:164`): `INMEM`, `CLEAN`, `DIRTY`, `UNDEF`, `ISCONST`.
`DIRTY` means the host register holds the truth and the home copy is stale; `ISCONST` means the
value is known at compile time and written **nowhere**. **(measured)**

**The host register budget, and it is smaller than §2c said.** `codegen_arm.cpp:83`–`:85`:

```c
#define R_MEMSTART 10
#define R_REGSTRUCT 11
uae_s8 always_used[]={2,3,R_MEMSTART,R_REGSTRUCT,12,-1};
```

so r2, r3, r10, r11 and r12 are locked and **8 host registers are allocatable** (r0, r1, r4–r9),
not the 11 the nominal `N_REGS` suggests. `freescratch()` (`compemu_support_arm.cpp:1571`)
repeats the same five by hand, which is a second, independent statement of the same fact.
**(measured — see §0.3(c) for what this corrects.)**

**The three lazinesses a bail has to undo.** The block-local discipline is lazy in exactly three
dimensions, and each is a thing a bail must materialise:

1. **Registers** — a guest register may be `DIRTY` in a host register or `ISCONST` in nothing at
   all. `tomem()` (`:858`) and `writeback_const()` (`:871`) are the two writebacks; `evict()`
   (`:881`) is the pair plus disassociation.
2. **The PC** — `m68k_pc_offset` accumulates guest bytes and is folded into `PC_P` only by
   `sync_m68k_pc()` (`:1320`), as `arm_ADD_l_ri(PC_P, m68k_pc_offset)`. Between folds **no PC
   exists anywhere**, in any form.
3. **The flags** — three-state: `live.flags_in_flags` (live in the ARM CPSR),
   `live.flags_on_stack` (in `regs.ccrflags.nzcv` via `FLAGTMP`), and `live.flags_are_important`;
   plus `flags_carry_inverted`, because ARM's carry is inverted on subtract and compare
   (`:236`–`:260`). `flags_to_stack()` (`:799`), `clobber_flags()` (`:820`),
   `flush_flags()` (`:828`).

`flush(save_regs)` (`:1466`) is the composite the engine already has: `flush_flags()`, then
`sync_m68k_pc()`, then a walk of v0…v18 writing back `CLEAN`/`DIRTY` via `tomem()` and `ISCONST`
via `writeback_const()` (`PC_P` excepted). `prepare_for_call_1/2` (`:1602`, `:1609`) add the
disassociation a C call needs. **(measured)**

**And here is the census's actual result.** Comparing what `flush(1)` writes against the
checkpoint the interpreter maintains at a guest-instruction boundary (§2e, `newcpu.cpp:5851`–
`:5853` and the `CATCH` recovery):

| the interpreter's boundary state | does `flush(1)` produce it? |
|---|---|
| the 16 data/address registers | **yes** — the v0…v15 walk |
| `regs.ccrflags.nzcv` | **yes** — `flush_flags()` |
| `regs.ccrflags.x` | **yes** — v17 in the same walk |
| `regs.pc` (the guest **logical** PC) | **no** — and see the refinement below, which is the most useful thing this census found |
| `regs.instruction_pc` (the restart PC) | **no** |
| `mmu_restart` | **no** |
| `mmufixup[0..1]` | **no** |

> **Three of the four are not "not written yet": the strings `instruction_pc`, `mmu_restart` and
> `mmufixup` do not occur anywhere in `jit/` — in any of its thirteen files, at all.** The
> engine has no notion of the interpreter's restart checkpoint, which is exactly what a JIT
> without an MMU is entitled to. **(measured; `docs/warp-m0-census.py` census (b5))**

> **⚠ `regs.pc` is the fourth, and the first pass of this census got it wrong — so it is
> recorded here with the correction rather than the tidy version.** A first count keyed on
> `regs.pc` with a trailing space returned zero and the draft concluded *"the existing JIT has
> no concept of a guest logical PC"*. Tightened to `\bregs\.pc\b(?!_)`, the count is **32**:
> `mov_l_mr((uintptr)&regs.pc, srca)`, in exactly **16 compiler bodies per table** — **7 `JMP`
> shapes, 7 `JSR` shapes, `RTS` and `RTD`** — one store each
> (`compemu_arm.cpp:13306`–`:13569` and the `nf` twins at `:38888`–`:39151`).
>
> **Every one of them is a block terminator whose target is not a compile-time constant, and no
> intra-block instruction boundary writes it.** That is a better finding than the one it
> replaces, and it points the same way: **the primitive exists and is exercised; what is missing
> is the discipline of holding a guest PC at arbitrary boundaries.** It also makes shape (ii)
> below cheaper than it looked — at a block head the guest PC *is* a compile-time constant, so
> the bail arm's materialisation is the immediate-store form of a store the emitter set already
> has. **(measured; `docs/warp-m0-census.py` census (b6))**

**What that hands M1, stated as design input and not as a design.** Three of the four are cheap
*because generation one already excludes the instructions that use them*:

- **`mmufixup[0..1]`** are the predecrement/postincrement address-register fixups. §2e's
  write-back-family exclusion already keeps every instruction that arms them in the interpreter,
  so the engine's obligation reduces to an invariant it can assert rather than maintain:
  `mmufixup` is disarmed at every block head, because only the interpreter ever arms it.
- **`mmu_restart`** is one bool, set to the same value the interpreter's boundary sets.
- **`regs.instruction_pc` and a real `regs.pc`** are the one with genuine cost, and they are the
  same cost paid twice: the engine must be able to name the **guest logical PC** of the
  instruction it is bailing from, and laziness 2 above says it does not hold one between folds
  — the 32 stores it does emit are all at block terminators, never at an intra-block boundary.

  Two shapes are available and M1 prices them against each other, not by taste: **(i)**
  materialise the guest PC at every faultable site — pays on the taken path, which §30's law
  says arrives in full; **(ii)** keep the block head's guest PC plus a per-site constant delta
  and materialise **only on the bail arm** — pays nothing on the taken path and one constant
  materialisation on a path that is already going to cost thousands of cycles. **(ii) is the
  obvious candidate and it is still a candidate**: it is what Q1's pre-registered state-sync
  measurement is for, and the number that decides it is emitted-instructions-per-faultable-site
  from `objdump`, read before any metal time.

**One more input the census makes free.** The bail writeback's *ceiling* is 19 stores plus the
flags word (v0…v18), but only the `DIRTY` and `ISCONST` entries actually emit anything. With 8
allocatable host registers against 22 virtual ones, the interesting quantity is the *emitted*
count, not the ceiling — so **M1's counter counts emitted stores per bail**, and pre-registering
the ceiling instead would be pre-registering a number that cannot move.

### (c) §2's claims, checked against the code

Cheap checks only, and the two that failed are corrected in place in §2 with the correction
marked. `Z3660_emu/src/` paths are relative to `z3660-firmware/Z-TURN/vitis_ide/`.

| §2 claim | verdict | evidence |
|---|---|---|
| `blockinfo` / `cache_tags` structures exist and are reusable | **confirmed, with the keying refuted** — see below | `compemu_arm.h:54`–`:57`, `:63`–`:66`; `compemu_support_arm.cpp:162` |
| the code cache is 32 MB from `malloc()` | **confirmed, and stronger than stated** — see below | `include/newcpu.h:42`; `compemu_support_arm.cpp:182`–`:215`, `:2094` |
| the guest's N/Z/C/V live in one word in native ARM CPSR bit positions | **confirmed** | `machdep/m68k.h:477` is the live `flag_struct` for `CPU_arm`; `FLAGBIT_N 31` at `:487`, `Z 30`, `C 29`, `V 28`, `X 0` |
| no literal pool is used on this target | **confirmed, and upgraded from `derived` to `measured`** | the data-buffer machinery is inside `#if defined(CPU_arm) && !defined(ARMV6T2) && !defined(CPU_AARCH64)` (`compemu_support_arm.cpp:697`) and `Z3660_emu/Makefile` passes `-DARMV6T2`, so it is not compiled at all |
| `flush_cpu_icache` at `jit/compemu_midfunc_arm.cpp:603`, four call sites | **confirmed exactly** | `:603`; called at `:636` (`write_jmp_target`), `compemu_support_arm.cpp:2189`, `:2213`, `:2739` |
| `m68k_run_mmu040` is 2 288 B | **confirmed** | `nm -S build-lean/Z3660_emu.elf` → `T _Z15m68k_run_mmu040v` size `0x8f0` = 2 288, 2026-08-23 |
| `m68k_run_jit()` at `newcpu.cpp:4321`; the MMU arm precedes the JIT arm in `run_func` | **confirmed** | `newcpu.cpp:4321`; `:6570`–`:6578` |
| `enable_jit = 0` for MMU bootmodes so `cachesize` stays 0 | **confirmed** | comment at `uae_emulator.cpp:1039`–`:1042`; the assignment at `:1079` |
| **r11 is the guest-memory base; r0–r10 are allocatable** | **REFUTED** | `codegen_arm.cpp:83`–`:85`: `R_MEMSTART` is **r10**, `R_REGSTRUCT` is **r11**, and `always_used[]` locks r2, r3, r10, r11, r12 → **8** allocatable. The `N_REGS 11` comment at `compemu_arm.h:124` is **stale**: it says *"11 holds memstart and 12 is scratch"*, and the code has said otherwise for as long as `always_used[]` has listed five registers. §2c corrected. |
| **`cache_tags` can serve as a block-entry table under the MMU** (implied by "reusable") | **REFUTED as keying, confirmed as structure** | `cacheline(x)` is `((uintptr)x) & TAGMASK` with `TAGMASK 0x0000ffff` (`compemu_arm.h:54`–`:57`), applied to **`regs.pc_p`** — a host pointer (`compemu_support_arm.cpp:1973`, `:2061`). Under the MMU there is no `pc_p`. §2b corrected. |

**Two findings worth more than the claims that produced them.**

**1. The 32 MB is already allocated, in the shipping MMU firmware, today.** `create_popalls()`
calls `cache_alloc(POPALLSPACE_SIZE + MAX_JIT_CACHE * 1024)` = `malloc(33 556 480)`
(`compemu_support_arm.cpp:2094`; `cache_alloc` is a plain `malloc` at `:207`, its `mmap` arm
commented out). `create_popalls()` is called from `build_comp()` (`:2370`), which
`build_cpufunctbl()` calls **unconditionally** under `#ifdef JIT` (`newcpu.cpp:1065`) — before
and regardless of `alloc_cache()` returning early on `cache_size == 0` (`:1886`). So every image
that boots today, MMU modes included, already takes 32 MB of the 96 MB core1 heap for a code
cache it never uses, and it says so on the serial: `Cache_Alloc of 33556480 at 0x........`
(`:212`). **WARP therefore costs no new heap, and the allocation's success is already proven on
this card every boot rather than assumed.** §2b's size budget is right for a better reason than
it gave. **(measured)**

**2. The `nf` arm doubles the inheritance surface, and generation one need not build it.**
`op_smalltbl_0_comp_nf` is a complete second copy — same 1 867 entries, same 340 NULLs, same
1 527 compilers, same 1 213 memory-reaching. Half of `compemu_arm.cpp`'s 1.6 MB is the
no-flags-needed arm. Whether generation one has an `nf` arm at all is an M1 scoping question
this census puts a number on rather than leaving to discovery. **(measured)**

## 0.4 M0 status

| item | state | note |
|---|---|---|
| 1. the unwind probe (Q2) | `DONE` | **`B: RESULT = TERMINATED`**, metal 2026-08-23 22:21:46. §2e's derivation is a measurement — §0.4.1. |
| 2. the route audit (Q1), zero build | `DONE` | §0.3(a) — 79.44 % |
| 3. the family census (Q7), zero build | `DONE` | §0.3(a), read against the dhrystone window per Q11 |
| 4. the block-length census, zero build | `DONE` | §0.3(a) — faultable-site density; block length is predicted, not read out, in §2a |
| 5. the §1.5 base re-take (optional) | `DONE` | **base = 48.68 – 49.91 % on one capture**; the stitched ~50 % survives — §0.4.2 |
| 6. the state-discipline census | `DONE` | §0.3(b) — added at the grill as Q1's contract input |
| 7. the M1 pre-registration commit | `DONE` | §0.5 — scope, kill line, band, cost measurement, gates, counters, sessions. **Committed before any engine code exists.** |

> ### M0 is `DONE`. All seven items are closed; M1 opens.

**The probe image.** `make q2probe` builds `Z3660_emu/build-q2probe` and
`Z3660_system/Alfa/sd_card/BOOT-q2probe.BIN` from `system_cli_q2probe.bif`, sharing core0, the
FSBL and the bitstream with the other three images. `Z3660_emu/src/warp_q2probe.cpp` is filtered
out of the other three variants' source lists **and** is empty unless `Z3660_WARP_Q2_PROBE` is
defined, and its one call site in `main.cpp` sits inside the same `#if` — two gates, either of
which alone would be sufficient. It runs after the abort and undefined-instruction handlers are
registered, so a probe that fails by executing garbage reports itself instead of rebooting
silently, and before emulation starts, so one boot's serial capture holds the whole answer.

> **This is not a shipping image and it is not an instrument either.** It exists to be booted
> once. `BOOT-lean.BIN` goes back on the card afterwards.

**The image, built 2026-08-23:** `BOOT-q2probe.BIN`, CRC **`FCC0CB3F`**, archived as
`sd_card/archive/BOOT-q2probe-FCC0CB3F.BIN`. Built in the same session as the three shipping
images, whose CRCs are **`BF56576A` / `F294E75A` / `5225BD9C`** — unchanged from before the
probe existed, which is the exclusion proof. `nm` finds **9** `warp_q2` symbols in
`build-q2probe/Z3660_emu.elf` and **0** in `build/`, `build-lean/` and `build-prof/`.

**What the capture decides, and how.** The probe runs experiment **A** (containment) first
because it is the safe one, then **B** (crossing), which is the one that can end the boot. Every
call that could kill the boot is preceded by a sentinel line that names what silence after it
would mean, so the capture is decidable in all outcomes:

| line on the serial | reading |
|---|---|
| `[WARP-Q2] A: RESULT = CONTAINED (status=…, prb=…)` | generated code ran, called C++, and a `throw` was caught **inside** the helper — §2e option 1 is available |
| silence after `[WARP-Q2] A: SENTINEL` | generated code did not execute at all. **An I-cache or execute-permission failure, not an unwind one** — a different bug, and the probe says so rather than letting it masquerade |
| `[WARP-Q2] A: RESULT = NO-CALL` / `WRONG-STATUS` | the stub ran but the call or the return value did not — probe defect, not a verdict |
| `[WARP-Q2] B: RESULT = CROSSED (caught outside, prb=…)` | the throw unwound **through** the generated frame. §2e's derivation is wrong; option 1 is also available; **the bail-before-fault posture stands regardless** |
| `[WARP-Q2] B: RESULT = TERMINATED` | `std::terminate` was reached — the EHABI unwinder refused the generated frame. **The predicted answer, and it confirms §2e** |
| silence after `[WARP-Q2] B: SENTINEL` | terminated harder than `std::terminate` could report. **Read as TERMINATED, and note it was worse** |
| `[WARP-Q2] ==== probe end ====` | both experiments completed and the boot survived |

One line that arrives free in the same capture is worth reading beside them:
`Cache_Alloc of 33556480 at 0x........`, the code cache's 32 MB, allocated on every boot of
every image today (§0.3(c)).

### 0.4.1 The Q2 capture — `TERMINATED`, and what it settles

**Metal, 2026-08-23 22:21:46, one boot of `BOOT-q2probe.BIN` (on-card CRC `FCC0CB3F`), preset 7
(`UAE_040_MMU`, `amix_ram 128`, `service_cadence 4`, `arm_frequency 1100`), piscsi root.**

> *`arm_frequency 1100` is a record of the configuration these rows were taken under, not a
> recommendation. It was later found to corrupt kernel reads on the real-CPU path — see
> [`AMIX.md`](AMIX.md#arm_frequency-n--zynq-arm-clock-upstreams-key-with-an-amix-trap).*

```
[WARP-Q2] A: stub @ 0x308edca0 = e92d4010 e12fff30 e8bd4010 e12fff1e  (icache seq: shipped, as-is)
[WARP-Q2] A: RESULT = CONTAINED (status=0x57515221, prb=22353)
[WARP-Q2] B: stub @ 0x308edcb8 = e92d4010 e12fff30 e8bd4010 e12fff1e  (icache seq: shipped + BPIALL/DSB/ISB)
[WARP-Q2] B: RESULT = TERMINATED -- std::terminate reached; the ARM EHABI
[WARP-Q2] B:          unwinder refused the generated frame (no .ARM.exidx entry).
[WARP-Q2] ==== probe end ====
```

Three results, in the order they matter:

1. **`B: TERMINATED` is the predicted row.** A `throw` raised in a helper called *from* generated
   code cannot be unwound on this target: the unwinder finds no `.ARM.exidx` entry for a stub in
   `malloc`'d DDR, returns `_URC_FAILURE`, and `__cxa_throw` terminates. **§2e's derivation is
   promoted from `derived` to `measured`, and bail-before-fault is the fault contract by
   measurement rather than by posture.** It terminated *cleanly* — terminate itself got to speak —
   so this is the `TERMINATED` row and not the silence-after-`B: SENTINEL` row, and the probe
   recovered through a `longjmp` out of the terminate handler, so the boot survived and the guest
   reached a TCP accept 49 s later. **The probe cost exactly one boot, as designed.**
2. **`A: CONTAINED` says generated code executes at all, with the shipped sequence.** Four
   hand-encoded ARM words published with `arm_flush_cache` + `arm_icache_invalidate` **only** — no
   `ISB`, no `BPIALL` — ran, called into C++, and returned. §2e option 1 is available. The
   silence-after-`A: SENTINEL` anomaly, which would have been an I-cache or execute-permission
   failure and *a different bug*, did not occur. This is one stub and not a volume: **Q3's
   `UAEJIT` A/B stays booked separately and the platform caveat rides unchanged** — adding
   barriers is not evidence of a fix; only a soak on metal is.
3. **The stubs were allocated from the code cache's own arena.** `Cache_Alloc of 33556480 at
   0x30A6DD60` on the same boot, against stubs at `0x308edca0` / `0x308edcb8` — 1.6 MB apart on
   the same heap, from the same allocator (`cache_alloc` *is* `malloc`), with the same page
   attributes. **Experiment A is therefore evidence about the actual code cache, not an analogue
   of it** — which is more than §0.3(c) could claim from source. On the lean boot the same line
   reads `0x30A6DAB8`, 680 B lower: the probe's two stub `malloc`s plus bookkeeping, taken and
   given back.

**The load-bearing check made before the boot, and it is why the result means anything:**
`mmu_common.h:11`–`:24` selects the **C++ arm** of `TRY`/`CATCH`/`THROW` under `__cplusplus` — a
real `throw` and a real EHABI unwind, not the `setjmp`/`longjmp` arm at `:26`–`:45`. Against the
`longjmp` arm the experiment would have been vacuous: a `longjmp` needs no unwind tables and
would "cross" any frame whatever. **(measured)**

*(One probe cosmetic, pre-registered before it was seen and recorded rather than tidied away:
`status = MAGIC | (prb << 16)` ORs `MAGIC`'s high half `0x5741` into the field the probe then
prints as `prb`, so a thrown `0x5750` = 22 352 prints as 22 353. The gate the probe actually
checks — `status & 0xffff == 0x5221` — is unaffected. Worth one line if the probe is ever
reused.)*

### 0.4.2 The §1.5 base, re-taken on one capture — 48.68 – 49.91 %

**One profiling boot, `BOOT-prof.BIN` (on-card CRC `5225BD9C`), same preset, same session.** The
capture measured its own price bracket — `probe calibration 40 marginal / 51 in-bucket`,
`s usable sweep: [40, 51]` — against map v3.4's `[40, 50]`: comparable, not identical.

| bucket | spans | share @40 | share @51 | pre-registered (map v3.4 §46.1) | |
|---|---|---|---|---|---|
| `DOPCFIND` | 65 607 634 | 15.18 % | 17.35 % | 16.10 – 17.96 | overlaps → hit |
| `LOOPRES` | 207 822 213 | 16.48 % | 13.38 % | 16.89 – 19.35 | **no overlap → MISS, low** |
| `FETCHOP` | 2 764 650 | 0.94 % | 1.13 % | — | |
| `FETCHEX` | 11 832 901 | 6.24 % | 7.74 % | pair 7.23 – 8.60 vs 7.18 – 8.87 | overlaps → hit |
| `TAILPOLL` | 65 607 043 | 9.49 % | 9.86 % | 6.8 (c4) | **no overlap → MISS, high** |
| `TAILSPEC` | 414 709 | 0.35 % | 0.44 % | 0.2 | high, below noise |
| **§1.5 base** | | **48.68 %** | **49.91 %** | 49.68 – 50.45 | overlaps → hit |

> ### The base is **48.68 – 49.91 %** — one capture, one window, one band.
> §1.5's stitched three-window estimate was 49.68 – 50.45 %, *"call it ~50 %"*. The re-take
> **overlaps it at 49.68 – 49.91 %. The base survives**, §1.5's declared comparability caveat is
> closed with a measurement instead of a hedge, and the first-generation band **1.25× – 1.55×**
> that stands on it does not move. **(measured)**

**The two per-bucket misses are the finding, and they cancel.** §1.5 named `TAILPOLL` as the one
row whose window disagreement is *not* below the noise, and it was right — on one capture it reads
9.49 – 9.86 %, above all three prior windows. What it could not know is that `LOOPRES` moves the
other way by about the same amount. **The stitched base was right for a reason it had not earned.
It has now earned it.**

**Free in the same capture, and carried into M1's cost side:** `TAILADV` reads **10.22 % at price
40 and 5.58 % at price 51** — it nearly halves inside one capture. That is §1.4's price artefact
reproducing on this session's own window. It is excluded from the base on purpose and it stays
excluded.

**The rows that ride beside every verdict, per Q11**, from the same boot:

| row | window | dhry | `INSNS` | `INSNS_SUPER` | super share |
|---|---|---|---|---|---|
| boot | MMU-enable → first guest TCP accept = **68 s** | — | 194 120 148 | 137 590 570 | **70.88 %** |
| dhry, counters | `PROFZ`-fenced, 100 000 pinned runs | **4 870.1** | 64 321 345 | 10 346 526 | **16.09 %** |
| dhry, buckets | `PROFZ`-fenced, 100 000 pinned runs | 1 977.6 | 65 607 043 | 11 677 205 | **17.80 %** |

**The discriminator earned its keep inside one boot:** the same pinned workload reads 16.09 %
supervisor in the counters window and 17.80 % in the bucket window, because the bucket window
takes ~53 s against ~16 s and absorbs ~3× the clock-tick interrupts. That is exactly why a share
swept at one band is not comparable with one swept at another. **(measured)**

### 0.4.3 The standing lean, and one instrument anomaly that is owed a mechanism

**`BOOT-lean.BIN` `F294E75A` is validated and is the standing lean.** Its first metal boot, same
session, same preset: dhry `8164 / 8151 / 8163 / 8129 / 8151`, **median 8 151**, mean 8 151.6,
**within-arm spread 0.43 %** (inside the rig's banked 0.04 – 0.70 %); direct-binary cross-check
8 152.2 in the same boot, 0.01 % from the median; **−0.15 %** of the same-day cross-build
reference (`DEC21748` median 8 163) and −0.42 % of the standing 8 185; boot window **46 s**; the
dhrystone value block printed in full with `Arr_2_Glob[8][7] = 100010 == Number_Of_Runs + 10`;
236 console line shapes against the reference boot's 236, **no new first-of-a-kind line**; clean
`haltsys`. **This is the interpreter lineage M1 is measured against. (measured)**

> **⚠ One pre-registered miss, and it is an instrument property until someone names the
> mechanism.** The *profiling* build's own dhrystone row moved **+7.22 %** between two builds
> (`2530C3F4` 4 542.0 → `5225BD9C` 4 870.1) on **identical work**: `INSNS` 64 337 234 vs
> 64 321 345 (0.025 % apart), same ARM clock, same probe calibration, same fence, same pinned
> count. Predicted 4 500 ± 250; measured 4 871. **MISS.**
>
> - **Candidate 1 — the `IVSUP` removal took work off a hot path: refuted by call count.** The
>   removed code sits in `z3660_knobs_sync()`, `z3660_dopc_do_invalidate()` and file-scope
>   accessors. This capture counts **32 372 invalidation requests against 65 607 043 retired
>   instructions — 1 per 2 026.** A per-request check cannot buy 7 % at that ratio, and nothing
>   was removed from the per-instruction dispatch path. **(measured)**
> - **Candidate 2 — code layout: plausible, unproven.** −881 source lines shifted every later
>   function while the image shrank by **24 bytes** (12 811 700 → 12 811 676): the code *moved*
>   far more than it *shrank*, and an interpreter dispatch loop in a 32 KB Cortex-A9 L1I is
>   exactly the workload sensitive to that. **(assumed)**
>
> **It damages neither §0.4.2 nor §1.5** — those shares are computed inside one capture against
> that capture's own `corrected_total`, and a build that runs 7 % faster in absolute terms
> redistributes nothing between its own buckets. **And the lean answers the question that made it
> worth chasing:** the lean carries the same two commits and lands at −0.15 % of the same-day
> reference — inside the noise, in the opposite direction, nowhere near +7 %. **The delta is
> confined to the instrument.**
>
> **Standing rule this imposes on every WARP verdict, adopted here:** *counters come from the
> profiling build, wall clock comes from the lean build, and **no WARP row ever quotes a
> wall-clock comparison between two different profiling builds** until that mechanism is named.*
> It is an open F-class item; it blocks nothing and it is not allowed to be forgotten.

**Session posture, for the record.** Four boots, three images, one config that was never edited —
`z3660cfg.txt`, `preset.txt` and `preset7.txt` byte-identical to as-found and to the previous
session's close-out, which is a proof of *absence* of change. This session ran the **installed**
040 system as its test bed (all three images are sound builds; the probe runs before emulation
even starts), so the installed root's CRC moved `30FE1733` → `9C5E9CF8` — expected, three boots to
multiuser and three `haltsys` writes — and the frozen-root pairing of the previous session is
neither claimed nor available here. No panic, no bus error, no corruption, no assertion, in any of
the four boots. `[PROF] l WARNING` appears nowhere in the session span; the `DOPCFILL` note
(`6afc8f9`) printed as a `note:` with its fault term, `spans=2747114` 222 short of
`DOPC_MISS=2747336` and bounded by `FAULTS=591`, in its first metal boot.

## 0.5 The M1 pre-registration — written before the first line of engine code

**This is M0 item 7 and the second half of M0's deliverable.** It is the contract M1 is scored
against: the scope, the kill line in its operational form, M1's own band derived bottom-up, the
one cost measurement Q1 owes, the correctness gates, the counter directions with their topology
checks, and what M1 will not establish.

Two mechanical rules from §3.4 govern it, and both are the reason it exists as a separate commit:

- **The pre-registration commit precedes the code commit, provably.** The SPEC round's did by 26
  minutes, the `f` round's by 29. This one precedes it by however long M1 takes to start.
- **A pre-registration that gets rewritten is not one.** Everything below is scored as written.
  Where a number here turns out wrong, it is marked `MISSED` beside the measurement and the
  prediction is left byte-untouched — `ifetch-fastpath.md`'s standard, and this document has
  already applied it to itself twice in §0.4.2.

Nothing in this section rebuilds anything: the three shipping images are untouched and their CRCs
remain **`BF56576A` / `F294E75A` / `5225BD9C`**.

### 0.5.1 Scope — what generation one compiles, enumerated

**Families, from §0.1 Q7, intersected with §2e's exclusion rule.** The intersection is narrower
than the family names alone, and naming it now is the whole point of enumerating scope before
code:

| in scope | detail | inheritance, per §0.3(a) |
|---|---|---|
| `Bcc` (incl. `BRA`) | 45/45 entries pure — no entry reaches guest memory | memory model inherited; **linkage is not** — see below |
| `DBcc` | 16/16 entries pure | same |
| `MOVE` / `MOVEA` | **both EAs in `Dn`, `An`, `(An)`, `d16(An)`** | 272 of 281 `MOVE` entries reach memory: **built new** on the bail contract |
| register-register ALU | destination `Dn`/`An`; source `Dn`/`An`, **plus** the quick-immediate shapes of the same families (`ADDQ`/`SUBQ` with a register destination) — see the reading note. Families: the *mixed* `ADD ADDA AND CMP CMPA EOR OR SUB SUBA TST` and the shift/rotate families the census already found fully pure | decided **per shape**, never per family |

> **The two dropped EA modes, and why they are dropped.** Q7's covered shape set is six modes;
> **`(An)+` and `-(An)` are excluded from generation one**, because §0.3(b) states the freeze's own
> invariant — *"`mmufixup` is disarmed at every block head, because only the interpreter ever arms
> it"* — and predecrement/postincrement EAs are exactly what arms it. §2e's write-back rule points
> the same way: `op_20c8_31_ff` advances the destination register, sets the flags, advances the PC
> and *then* stores, so a fault there is a write-back fault on an architecturally complete
> instruction. **Generation one never compiles one.** The cost of the exclusion is priced in
> §0.5.3 rather than waved at.
>
> Whether a predecrement/postincrement **source** can be admitted without violating §2e is a
> **named M1 scoping question with a static answer** — read the generated handler, before any
> metal time. If it resolves permissive the shapes are added under a re-registered **M1a**, never
> folded back into this band.

> **The reading note: Q7 says "register-register ALU", and the quick-immediate shapes are a
> *reading* of it — made here, before the measurement, and not afterwards.** `ADDQ`/`SUBQ` with a
> register destination touch no memory, arm no fixup and restart cleanly; they are the same
> codegen problem as `ADD Dn,Dn` with the source folded into the encoding. They are also
> **6.96 – 11.03 %** of dispatch time, which is why the reading is worth stating rather than
> leaving to discovery. The **strict** reading — both operands registers — is priced beside it:
> it takes §0.5.3's covered share from 30.86 – 54.85 % to **27.38 – 43.82 %**, coverage from
> 0.19 – 0.62 to **0.16 – 0.49**, and the band from 1.04× – 1.33× to **1.03× – 1.25×**
> (dhry ≈ 8 420 – 10 180). **Both are stated; the inclusive reading is the one M1 builds, and it
> is frozen here so that it cannot be adopted after a disappointing row.** `MOVEQ` is covered by
> *neither* reading — it is a move-immediate, not an ALU op — and it is M1a's first extension.

> **"Inheritable as-is" is a statement about the memory model, and only about it.** `Bcc`/`DBcc`
> carry no `jnf_MEM_*` call, which is why §0.3(a) calls them inheritable — but the existing
> compilers compute branch targets and chain blocks through `regs.pc_p` and `cache_tags`, and
> §0.3(c) refuted that keying under the MMU. **The branch bodies are inherited; the target
> arithmetic and the block linkage are rebuilt on the guest logical PC.** Anyone reading "45/45
> pure" as "45/45 free" will mis-estimate M1.

**Everything else bails, and the list is checkable statically before any metal time:** every
write-back family; every FPU opcode (`Q13`, permanently); every privileged or mode-changing
instruction; `MOVE16`; every supervisor-mode instruction — **generation one is user-mode only, and
a block is not entered with `regs.s` set**; every instruction the engine does not compile; and
every PC rung 0 claims (`Q8` — the JIT declines those PCs and does not modify rung 0).

**The switch.** `WARP` console knob, `SHARED.warp_on` at `0xFFFF00F0`, resolved by
`z3660_warp_resolve()` in one header and nowhere else. **`unset = OFF` through M4** (`Q9`).

> **Arm attestation, and it is a build requirement rather than a habit.** The `f` round's own
> conclusion is the precedent and it is adopted verbatim in shape: *"The decision to compile the
> confession counter into every variant is what made arm B attestable."* The profiler's counters
> exist **only** in the profiling build (`Z3660_PROF_CNT` expands to nothing when `Z3660_EMU_PROF`
> is unset), so a lean verdict boot has no counter to read — and a banner is not admissible,
> because two cores share one unlocked UART and an arm banner arrives spliced. **Therefore:
> `warp_entered` is a plain counter compiled into every variant, printed once on halt/disarm, and
> its non-zero value is the arm attestation for every capture, lean included.** Its cost is one
> increment per **block entry** (not per instruction), it is counted in the `objdump` budget of
> §0.5.4, and it is a shipped cost by choice. **One increment site, not two:** the profiling
> build's `WARP_ENTER` (id 47, §0.5.6) is the same quantity read out of the counter block, so the
> lean confession and the profiled counter can never disagree.

**The M1a extension list, frozen now, in priority order.** §0.5.3 predicts coverage below the
60 % bar for the scope above; if the counter says so, these are added **in this order**, each
under its own re-registered pre-registration commit, and **never** to rescue a row that has
already been measured:

1. `MOVEQ.L #<data>,Dn` — a register-only compiler already exists at `compemu_arm.cpp:17481`;
   2.31 – 2.82 % of dispatch time.
2. The remaining register-destination shapes of `ADD`/`SUB`/`AND`/`OR`/`CMP`/`EOR` not already in
   scope, per shape.
3. `MOVE`/`MOVEA` with an `(An)+` / `-(An)` **source**, *if* the static read of §2e resolves
   permissive.
4. `(xxx).L` `MOVE` shapes — §0.1 Q7's named M4 ladder candidate, pulled forward only if the boot
   rows say so.

### 0.5.2 The kill line, restated and operationalised

The line as agreed in §0.2, quoted and not paraphrased:

> **At M1, with coverage in band (≥ 60 %) and no named defect: cross-build dhry ≤ 8 185 + rig
> spread (≈ 8 242) → the project STOPS and the code reverts same-day; rung-3 precedent binding;
> no re-scoping. Positive but far under the 1.25–1.55× band → continue ONLY with a named
> measured mechanism + a re-registered M1b.**

**Operational form, and the primary comparison is a same-session row.** The absolute number 8 185
was written when the standing lean was the 8 185-class image; the M1-era interpreter lineage is
`F294E75A`, whose validated median is **8 151** with a within-arm spread of **0.43 %** (§0.4.3).
Cross-session drift on this rig is ~9 %, so an absolute number carried across sessions is the one
comparison this campaign has repeatedly found untrustworthy:

```
   R  = the engine-OFF cross-build reference row, taken in the SAME SESSION on the
        then-current interpreter lean (the F294E75A lineage), n >= 5, median
   s  = the GREATER of that session's own measured within-arm spread and the rig's
        banked ceiling 0.70 % -- which is 0.2's own construction (8185 x 1.007 = 8242).
        A session whose measured spread EXCEEDS 0.70 % is a measurement error and
        produces no verdict at all.
   KILL if   engine-on median  <=  R x (1 + s)
```

With this session's numbers (`R` = 8 151, measured spread 0.43 %, so `s` = 0.70 %) that threshold
is **8 208**. The user-agreed absolute floor **8 242** is quoted beside every M1 row and is never
removed — **both are stated; the same-session row governs.** A row that clears one and not the
other is reported as exactly that, and the session that produces it is repeated before anything is
concluded.

Three riders, all inherited rather than invented:

- **Every A/B is same-boot, with a cross-build reference row beside it**, and the identity
  `cross-build × disarm-penalty = same-boot switch` must close (§3.5). It has closed to four or
  five digits three times; a WARP verdict where it does not close is a measurement error, not a
  result.
- **The outgoing lean image is archived before anything is rebuilt.** A rebuild once destroyed the
  only copy of a reference arm.
- **Counters are read before wall clock**, and the counters come from the profiling build while
  the wall clock comes from the lean build (§0.4.3's standing rule).

**The coverage bar, and the counter it is read from.** ≥ **60 %** of retired guest instructions
executed inside compiled blocks. The counter and its arithmetic are pre-registered here because
the obvious ratio is wrong — rung 0 already walked into this and `z3660_prof.h` records it:

```
   coverage  =  WARP_INSNS / ( (INSNS - WARP_ENTER) + WARP_INSNS )
```

`INSNS` (id 0) counts instructions **retired through the run-loop tail** — it is incremented once
per pass of the loop, inside `Z3660_PROF_RUNLOOP_SAMPLE`. A block that retires *n* guest
instructions is **one** pass, so it contributes 1 to `INSNS` and *n* to `WARP_INSNS`:
`WARP_INSNS` is **not a subset of `INSNS`** and the literal ratio is not a share of anything.
This is `BLK_INSNS`'s reconstruction with `WARP_ENTER` in `BLK_HIT`'s place, and rung 0's metal
boot is the warning: the literal ratio read 59.28 % where the correct one read 37.25 %, *"not
merely inflated — outside the range in which the prediction could be judged at all."*

> **Topology check on `WARP_ENTER`, and it is the one that decides whether the number means
> anything.** `WARP_ENTER` must be incremented **only at the run-loop entry into a block** — the
> site that costs exactly one `INSNS` — and **never** on a chained block-to-block transfer, which
> costs none. Incrementing it on chained entries over-subtracts and reports coverage high. That is
> the FETCH-fall error's exact shape (*"a counter reached only after the branch being deleted
> cannot fall when it is deleted"*), so the two sites are separated by construction:
> `WARP_ENTER` at the loop hook, `WARP_BLK` at every block execution including chained ones.

**What a coverage miss is, and what it is not.** §0.2's guard is explicit: *"A miss at 20 %
coverage is a bug report, not a verdict, and it does not trigger the kill line."* So:

| measured at M1 | reading |
|---|---|
| coverage ≥ 60 %, dhry ≤ `R × (1+s)`, no named defect | **the kill line fires.** Revert same day, rung-3 precedent binding, no re-scoping |
| coverage ≥ 60 %, dhry inside §0.5.3's band | on-prediction; M1 passes and M2 opens |
| coverage ≥ 60 %, dhry positive but **under §0.5.3's band** | §0.2's second clause: continue **only** with a named measured mechanism and a re-registered **M1b** |
| **coverage < 60 %** | **not a verdict in either direction.** The engine engaged too little to price. Extend the family set in §0.5.1's frozen order under a re-registered **M1a**, and take the verdict at the first wave whose coverage is in band |
| coverage < 60 % **and** a named implementation defect | a bug report. Fix, re-run, and the defect is named in the record |

**M1a and M1b are different things and are not interchangeable.** M1a is a *scope* wave that
happens before any verdict exists, and it is legitimate precisely because its family list is
frozen in §0.5.1 **now**, before the first measurement. M1b is §0.2's *continuation* clause after
an in-band-coverage, under-band-positive result, and it requires a named measured mechanism.
Neither may be used to reinterpret a row that has already been taken.

> **The band this section's under-band rule is scored against.** §4's M1 gate already says it:
> the verdict is taken *"with the first-generation band of §1.5 recorded beside it for
> information, not as the bar"*. §1.5's **1.25× – 1.55×** is the **full-engine** number, derived
> at coverage 0.70 – 0.90 which generation one does not deliver. **M1 is scored against
> §0.5.3's band, derived below from what generation one actually covers, and §1.5's band is
> quoted beside every M1 row.** This mapping is written before the measurement, which is the only
> time it can honestly be written.

### 0.5.3 M1's own band, derived bottom-up from what generation one covers

§1.5 derived the full engine's band. This derives generation one's, from the same measured base
and the same banked rings, with every deflator named and its direction stated.

**Step A — the covered share of dispatch time.** Time-weighted family shares from the two
sampler rings whose supervisor share is representative (`09-DHRY-BLKON` 43.31 %,
`05-DHRY-ring-full` 54.10 %; the 87.91 % ring is excluded as kernel-dominated, as it was for the
handler study):

| component | ring range | deflator | contribution |
|---|---|---|---|
| `MOVE`/`MOVEA`, covered EA shapes | 22.91 – 26.36 % | `d` = 0.70 – 0.90 — removes `(An)+` / `-(An)` traffic (§0.5.1). Anchor: `MOVE.L (An)+,(An)+` alone is **10.3 %** of the family; the rest of the predec/postinc traffic is unmeasured, so the 0.70 end allows twice as much again | **16.04 – 23.72 %** |
| `Bcc` / `BRA`, excluding `BSR` | 12.57 – 13.93 % (line total) | `b` = 0.70 – 1.00 — `BSR` is a fully memory-touching family (§0.3(a)) and bails; its share **inside** the line is unmeasured | **8.80 – 13.93 %** |
| `DBcc` | inside the `ADDQ`/`SUBQ`/`Scc`/`DBcc` line, not separately quantified | — | **0** in the floor; inside the next row's ceiling |
| register-only ALU, listed lines only | `ADDQ`/`SUBQ` 6.96 – 11.03 % + `CMP`/`CMPA`/`EOR` 5.08 – 6.17 % | `g` = 0.50 – 1.00 — the register-destination fraction of those lines is unmeasured | **6.02 – 17.20 %** |
| the unlisted ALU tail | ~30 % of each ring is below the published histogram's cut and contains `ADD`/`SUB`/`AND`/`OR`/shifts | — | **charged 0** — a deliberate, stated undercount |

```
   gen-1 covered share of DISPATCH TIME   =   30.86 % .. 54.85 %      (derived)
```

Each row's range is the envelope across **both** rings *and* its deflator, taken per line rather
than per ring — deliberately wider than either ring alone would give, because the two rings
disagree in both directions and neither is the verdict window.

**Step B — three conversions, each with its direction.** Coverage is a share of *retired
instructions*; the rings measure a share of *time*, on a window that is not the verdict window:

| factor | band | direction | why |
|---|---|---|---|
| `k` time-share → instruction-share | 1.00 – 1.25 `assumed` | **up** | generation-one instructions are the cheap ones: the handler study prices `MOVE.L Dn,Dn` at 123 cycles against 179.6 – 223.6 for the memory-touching shapes, so their instruction share is ≥ their time share |
| `β` block formation | 0.60 – 0.90 `assumed` | **down** | coverage counts instructions inside **blocks**, and a block ends at the first instruction the engine declines (§2a). A lone compilable instruction between two declines yields a block not worth entering |
| `w` window mismatch | charged **1.00** | **not taken** | the rings read 43.31 % / 54.10 % supervisor; the M1 verdict window measured **16.09 %** (§0.4.2). Across the three banked rings the `MOVE` family's covered fraction rises monotonically as the supervisor share falls (87.91 % → 57.6 % covered; 54.10 % → 83.3 %; 43.31 % → 83.7 %), so a 16 % window can only cover **more**. The correction is one-sided in our favour and is therefore **not taken** |

```
   c  =  0.3086 x 1.00 x 0.60   ..   0.5485 x 1.25 x 0.90
```

> ### Pre-registered coverage for generation one as frozen: **c = 0.19 – 0.62**. (derived, on six assumed factors — `d`, `b`, `g` in step A and `k`, `β`, `w` here, every one of them named)
>
> **The 60 % bar sits at the very top of that band, and that is a finding, not a hedge.** The
> arithmetic says generation one is *more likely than not* to engage below the bar — so **M1 is
> planned as M1 + M1a from the start** (§0.5.1's frozen extension list, §0.5.2's decision table),
> and the perf verdict is taken at the first wave whose coverage is in band. Writing this down
> before the code is what stops a later family extension from looking like a rescue.

**Step C — the removal side, per bucket, with return fractions.** §30's law forbids converting
counts to wall clock without asking what was hiding the work. Each of the §1.5 base rows is
charged a return fraction `ρ`, on the M0 capture's own shares (sweep `[40, 51]`, §0.4.2):

| bucket | share @40 / @51 | `ρ` | chained or hidden — the reason |
|---|---|---|---|
| `DOPCFIND` | 15.18 / 17.35 % | **0.80 – 1.00** | **chained.** The lookup's result *is* the handler pointer the indirect branch consumes; nothing overlaps a dependency the next step waits on. Becomes one lookup per block |
| `LOOPRES` | 13.38 / 16.48 % | **0.50 – 0.80** | **mixed.** The PC advance and the cadence counter are on the chain; the rest is loop bookkeeping that partly overlaps |
| `FETCHOP`+`FETCHEX` | 7.18 / 8.87 % | **0.00 – 0.50** | **measured hidden.** Rung 3 removed 42 instructions from exactly this call chain and moved the wall clock **−0.05 %**; §1.4 records the pair as partly latency-hidden. Charged at **at most half, never in full** |
| `TAILPOLL` | 9.49 / 9.86 % | **0.00 – 0.50** | the poll **survives** — once per block instead of once per `cadence` instructions. At cadence 4 and a mean block length `L` = 4 – 8 the removable fraction is `1 − 4/L` = 0.00 – 0.50, and that is before asking how much of it overlaps |
| `TAILSPEC` | 0.35 / 0.44 % | **1.00** | below the noise either way |

> **Mean block length `L` is pre-registered at 4 – 8 guest instructions**, and it is a prediction,
> not a readout (§2a claim 1). Basis: control transfers are 17.2 – 18.7 % of dispatch time in the
> two rings (`Bcc`/`BRA`/`BSR` 12.57 – 13.93 % + `JSR`/`JMP` 4.66 – 4.76 %, `RTS` unlisted and
> additional), so runs between control transfers are ~5 – 6 instructions before any other block
> ender; declined instructions and page boundaries only shorten them. **If `L` measures above 8,
> `TAILPOLL`'s row is re-read at the measured `L` and the band is not.**

```
   t_low  = (15.18x0.80 + 13.38x0.50 + 7.18x0.00 + 9.49x0.00 + 0.35x1.00) x 0.19
          =  19.18 % x 0.19  =  3.65 %      ->  1/(1-0.0365)  =  1.04x
   t_high = (17.35x1.00 + 16.48x0.80 + 8.87x0.50 + 9.86x0.50 + 0.44x1.00) x 0.62
          =  40.34 % x 0.62  =  25.01 %     ->  1/(1-0.2501)  =  1.33x
```

> ### M1's band: **1.04× – 1.33×**, i.e. **dhry ≈ 8 460 – 10 870** against the same-session reference row (8 151 on the F294E75A lineage). (derived, on coverage's six factors plus five `ρ` and `L` — all named above)

**Cross-check, in §1.5's own form and it lands in the same place.** Taking §1.5's Amdahl shape
with the M0-measured base and generation one's coverage: `t = base × c × r` with
base = 0.4868 – 0.4991, `c` = 0.19 – 0.62, `r` = 0.60 – 0.80 gives **1.06× – 1.33×** — the same
top to two decimal places and a bottom 0.02× apart. Two derivations, one per-bucket and one
aggregate, and they do not disagree.

**Three things must be said beside the band or it is a target chosen after the result:**

- **This is the removal side only.** It charges nothing for the block-entry lookup, the block
  linkage, the arm-attestation increment, the I-cache pressure of generated code, or compilation.
  §30's law is unambiguous about the first: *"work added in front of the path arrives"* — rung 3
  measured +19 prepended instructions costing 1.58 % of wall clock, in full. **The cost side is
  unquantified and it is subtracted from this band, not added to it.**
- **The floor is close to the kill line.** 1.04× on the same-session reference is dhry ≈ 8 460
  against a kill threshold of **8 208** and an absolute floor of 8 242 — **3.1 %** and 2.6 %
  respectively. The bottom of this band lies within a couple of hundred dhrystones of the line,
  and that is the honest shape of this experiment. Under the strict ALU reading (§0.5.1) the
  floor is 1.03× = dhry ≈ 8 420, which is **2.6 %** above the threshold.
- **The downside case is a null and it has a precedent on this rig.** Rung 3 engaged at 91.10 %
  coverage, made its bucket 23 % cheaper per instruction, hit or beat every premise it declared,
  moved the wall clock −0.05 %, and came out the same day.

### 0.5.4 Q1's contract measurement — state-sync cost per faultable access

§0.1 Q1 pre-registered this as a measurement, and §0.3(a)/(b) supplied its denominator and its
inventory. Here is how it is measured, what it is predicted to read, and what would trigger the
Route-B allocator redesign.

**What is measured.** The **emitted ARM instructions per faultable site** attributable to
maintaining the interpreter's boundary state, split into the two paths that matter:

```
   on-path cost   instructions executed at a faultable site when the access HITS
   bail-arm cost  instructions executed when the site declines and the engine bails
```

**How, and in this order.** (1) **`objdump`, before any metal time**, in §30.3 step 3's form:
compile a fixed corpus of one instruction per generation-one shape, disassemble, and count. This
is a static number and it decides shape (i) vs shape (ii) on its own. (2) **A same-boot A/B on
metal**, engine-on with the state-sync discipline against engine-on with it elided — which is
**incorrect code** and therefore a **measurement build**, never shipped, gated exactly as the
withdrawn `IVSUP` build was, and removed from the tree when the measurement lands. Its removal
carries the `IVSUP` lesson with it (see §0.5.6 on counter id 46: a withdrawn measurement build
leaves a poisoned counter id behind it, and the tools that must refuse it outlive the build).

**The denominator, measured at M0.** 3 738 guest-memory call sites across 3 054 compiler bodies:
**1.22 faultable sites per body**, **1.54 per memory-touching body**, one or two in essentially
every case and never more than four (§0.3(a)). So a design costing *n* instructions per faultable
site costs ~1.5*n* per memory-touching guest instruction.

**The inventory a bail must materialise, measured at M0** (§0.3(b)): `flush(1)`'s ceiling is 19
stores plus the flags word, but only `DIRTY` and `ISCONST` virtuals emit anything, and with **8**
allocatable host registers against 22 virtuals the emitted count is what matters — **so the
counter counts emitted stores per bail, not the ceiling.** The four boundary items `flush(1)` does
*not* produce are `regs.pc`, `regs.instruction_pc`, `mmu_restart` and `mmufixup[0..1]`; of these,
`mmufixup` reduces to an asserted invariant and `mmu_restart` is one bool, leaving the guest
logical PC as the one with genuine cost.

**The pre-registered numbers.** Shape (ii) — block-head PC plus a per-site constant delta,
materialised **only on the bail arm**:

| quantity | pre-registered band | how scored |
|---|---|---|
| on-path state-sync, per faultable site | **0 – 2 ARM instructions** | `objdump`, before metal |
| on-path state-sync, per memory-touching guest instruction | **0 – 3.1** (= 1.54 × 2) | derived from the above |
| bail-arm materialisation, per bail | **2 – 4 instructions** + the emitted register writebacks | `objdump` + `WARP_BAIL_ST / WARP_BAIL` |
| emitted stores per bail, mean | **3 – 8** | counter ratio, read before any wall clock |
| shape (i) — materialise at every site — if measured instead | **2 – 3 per site, 3.1 – 4.6 per memory-touching instruction** | `objdump`; recorded even though shape (ii) is expected to win, so the comparison is priced rather than asserted |

**The decision rule for the Route-B redesign, and both conditions are measured.** §0.1 Q1's
contract is that the allocator is redesigned *"only if the numbers say that is the ceiling — not
because a redesign looks tidier from the outside"*. Operationally, **both** must hold:

1. **The static condition:** on-path state-sync cannot be brought to ≤ 2 instructions per
   faultable site inside the existing allocator, and the `objdump` shows *why* structurally —
   spills forced by the 8-register budget, visible as spill/reload instructions in the emitted
   sequence rather than as a coding choice; **and**
2. **The dynamic condition:** the measurement-build A/B shows the state-sync discipline costs
   **≥ 8 %** of the covered path's wall clock, on a same-boot A/B with the cross-build reference
   row beside it and the disarm identity closing.

Either alone is a finding to record and not a trigger. Neither is a licence to re-scope M1.

### 0.5.5 Correctness gates — every one of them mandatory, none of them wall clock

**G1 — the lockstep oracle, clean over ≥ 10⁸ retired guest instructions.** Mode: **per-block**
(§3.2) — run a block jitted from a checkpoint, restore the checkpoint, run the same span
interpreted, compare the whole set. The comparison set is rung 2's, unchanged, plus the JIT's four
(§3.1): returned value, fault outcome, fault address, guest PC, write-back slots, `mmu_ssw`,
`mmu_cache_state`, destination memory, all sixteen data and address registers, the page-cache
counters, **and** `mmufixup[0..1]`, `mmu_restart`, `regs.instruction_pc`, retired-instruction
count.

**Where it runs, and the honest limit stated once more:** the host harness runs on x86-64 and
**cannot execute one instruction of ARM codegen**. So the harness proves block discovery, keying,
invalidation, bail decisions and state write-back — the *decisions* — and everything provable
there is proven before a metal session is booked; the **emitted sequence is proven on target and
only on target**, in a gated build whose lockstep arm is compiled out of every shipping image.
The 10⁸ is accumulated on metal across the boot window and pinned dhrystone runs, counted by
`WARP_LS_INSNS`, and the gate is `WARP_DIVERGE == 0` over that span. A single divergence is a
stop, not a statistic.

**G2 — the counter identities, exact.** See §0.5.6 for the table; the three that are gates:

```
   DOPC_HIT + DOPC_MISS  ==  (INSNS - WARP_ENTER) + FAULTS
   FAULTS(on)            ==  FAULTS(off)                     +/- 1
   sum(IV_*) - IV_WRAP   ==  DOPC_INVAL                       (unchanged, and WARP reconciles to it)
```

The first is the existing identity `DOPC_HIT + DOPC_MISS == INSNS + FAULTS` corrected for the
engine: a jitted instruction never consults the decoded-op cache, and a block costs one `INSNS`.
With the engine off, `WARP_ENTER` = 0 and it reduces to the identity as it stands today. The
second is the bail contract's own check — the engine never faults inside a block by construction,
so every fault is still the interpreter's; the SPEC round read 3 630 vs 3 629 and that is the
resolution to expect.

**G3 — zero `throw` sites reachable from generated code, proven statically before metal.** Q2
measured the consequence of getting this wrong: not a fault, `std::terminate`. The proof is a
closed-world audit, and it is cheap because the world *is* closed — generated code calls only the
helper set the engine emits calls to. For every symbol in that set, `objdump` the reachable call
graph and require that no reachable function references `__cxa_throw`, `__cxa_allocate_exception`
or `_Unwind_RaiseException`, and that no reachable function contains a `TRY` block under the C++
arm of `mmu_common.h`. **Unmet ⇒ the build does not go to metal** — the SPEC round's form of a
static gate, mandatory, no exceptions.

**G4 — the harness corpus for the `jnf_MEM` replacements, engagement-pinned.** Generation one
replaces six of the ten accessors (`readbyte`/`readword`/`readlong`,
`writebyte`/`writeword`/`writelong`); the corpus is built per accessor × per verdict class, and
**every case declares MUST-serve or NEVER-serve and a counter checks it** — because *"'the two
paths agree' is trivially true when the fast path silently never fired"*, and a JIT's decline
surface is larger than any rung's:

| axis | cases |
|---|---|
| accessor | 6 |
| verdict class | `XPAR`, `PAGE`, `SLOW` (= decline → bail) |
| engagement polarity | MUST serve / NEVER serve |
| structural fences, per accessor | page-crossing refusal, bank-boundary refusal, device refusal, misalignment refusal |
| the asymmetry that must not be copy-pasted away | `TTR_NO_WRITE` ⇒ read `XPAR`, write `SLOW` |

`6 × 3 × 2 = 36` verdict cases, `6 × 3 = 18` fence cases (misalignment is one fence per accessor
and the other three are per verdict-eligible class), plus the 2 asymmetry cases — **≈ 56 new
cases**, each asserting byte-exactness against the interpreter on the returned value, the
destination memory, `mmu_cache_state`, the fault outcome and the fault address. Half the corpus is
decline cases, and without the engagement check they would all pass against a fast path that was
never compiled in.

**G5 — the mutation set, pre-registered here and grown at each milestone.** Every rung in this
campaign injected defects and required the corpus to catch them. M1's arms:

| # | mutation | must be caught by |
|---|---|---|
| M-1 | `TTR_NO_WRITE` asymmetry inverted (write → `XPAR`) | the asymmetry cases in G4 |
| M-2 | the page-crossing structural fence removed | the fence cases in G4 |
| M-3 | state-sync omitted at one faultable site (bail with a stale guest PC) | per-block lockstep, `regs.pc` compare |
| M-4 | `mmufixup` invariant not asserted at block head | lockstep `mmufixup` compare |
| M-5 | the block key drops the supervisor bit | the dopc's own empty-marker trap case, reused |
| M-6 | one `IV_*` cause not wired to the code cache | the invalidation identity in G2 |
| M-7 | the cadence counter fed 1 per block instead of retired instructions | the cadence-tick assertion, and on metal the PSTAT gap fingerprint |
| M-8 | the bail arm writes back `DIRTY` but not `ISCONST` virtuals | lockstep register compare |

### 0.5.6 The counter table, and every direction checked against call topology

**The law being obeyed:** *"Check a pre-registered counter direction against the counter's
position in the call topology. A counter reached only after the branch being deleted cannot fall
when it is deleted — a declared direction was once arithmetically impossible."*

**Existing counters, engine ON against engine OFF on the same pinned workload:**

| counter (id) | direction | topology check |
|---|---|---|
| `INSNS` (0) | **falls** by `WARP_INSNS − WARP_ENTER` | incremented once per **run-loop pass** inside `Z3660_PROF_RUNLOOP_SAMPLE`; a block is one pass. It **cannot** stay equal, and §2e's claim 3 in its literal form ("`INSNS` identical between arms") is superseded by the reconstruction in §0.5.2 |
| `INSNS_SUPER` (1) | **unchanged in absolute terms** | same per-pass site, gated on `regs.s`; generation one is user-mode only, so no block contributes. **The window discriminator must be computed on the reconstructed stream**, not on raw `INSNS`, or it moves with the arm |
| `FETCH` (2) / `IFETCH_CALLS` (20) | **fall**, together, ≈ by the covered share | incremented inside the interpreter's ifetch bracket. **This direction is only possible because the compile path does not enter that bracket** — compiler reads are counted by `WARP_CFETCH`. Wire the compiler through the bracket and the declared fall becomes impossible |
| `READ` (3) / `WRITE` (4) | **unchanged** | *requires the counting emitter*: in the profiling build the emitted access sequence increments them at the same point the accessor does. Without it they fall by the covered share and the fall is an instrument artefact, not guest behaviour. **Consequence, declared: the profiling build's generated code is not the lean build's, so code-bytes-per-instruction is read off the lean emitter's `objdump` and never the profiling one** |
| `IPAGE_HIT`/`MISS` (5, 6) | **fall, by less than `FETCH`** | the compile path reads guest instruction words through the same instruction accessor (sharing the arithmetic is how equivalence is guaranteed), so it still consults the instruction page cache. `WARP_CFETCH` is the term that reconciles the two falls |
| `DPAGE_*` (7 – 10), `XLATE`/`XLATE_OK` (11, 12), `ATC_MISS`/`ATC_HIT` (13, 19) | **unchanged** | reached from inside the shared translation arithmetic, which generated code calls rather than reimplements |
| `MISALIGN_R`/`W`/`I` (14, 15, 43) | **unchanged** | generated code never fast-paths a misaligned access — it bails, and the interpreter performs it |
| `FAULTS` (16) | **unchanged ± 1** | run-loop `CATCH` entries; the bail contract means every fault is still taken by the interpreter. A move here is a leak in the contract |
| `TRANSITIONS` (17) | **falls** | profiling-build-only phase transitions; fewer interpreted instructions, fewer transitions |
| `STACK_OVF` (18) | **0** | must be, in every capture, as today |
| `BLK_HIT`/`BLK_INSNS`/`BLK_BYTES` (21 – 23) | **unchanged** | rung 0 is untouched and the JIT declines its PCs (`Q8`). **A fall here is the engine poaching rung-0 PCs**, and this counter is how that is detected rather than argued |
| `DOPC_HIT`/`DOPC_MISS` (24, 25), `DOPC_EXT` (26), `DOPC_WAY0..3` (39 – 42) | **fall** ≈ by the covered share | consulted at the interpreter's decode site, which a jitted instruction never reaches; the compile path does **not** consult the dopc |
| `DOPC_INVAL` (27), `IV_*` (28 – 38) | **unchanged** | the guest's invalidation demand is a guest property (91 k measured, cache-independent). WARP adds a **consumer**, not a cause |
| `DFAST_HIT` (44) / `DFAST_XPAR` (45) | **unchanged**, share within 0.5 pp | with the counting emitter, both arms of the generated sequence increment where the accessor does. **Without it the ratio would not merely fall — it would be selection-biased**, because the interpreted remainder is enriched in exactly the accesses the engine declined. §2d's claim 2 ("stays at 99.1 %+") is therefore **conditional on the counting emitter**, and that dependency is a design requirement derived from a counter direction |

**New counters — appended ids, no wire bump, no new buckets.**

> **Id 46 is permanently retired and WARP's block starts at 47.** `Z3660_PROF_C_COUNT` is 46
> today, so 46 is the next free index — but it is not free: the withdrawn `f`-measurement build
> used it for `IV_SUPPRESSED`, and `profiler.md`'s standing reading rule is to **refuse to
> interpret a capture carrying a non-zero id 46 as a profile of a correct machine**. A WARP
> counter at 46 would make every legitimate capture refusable by a tool obeying that rule. So 46
> stays retired, carries a placeholder in the name table so the indices align, and WARP occupies
> **47 – 59**, taking `Z3660_PROF_C_COUNT` from 46 to **60**. **No existing id moves and no field
> changes shape, so the dump version stays 2** — the same reasoning `IFETCH_CALLS` (id 20) and the
> withdrawn id 46 were appended under. That is the `IVSUP` lesson's real cost, paid once here
> rather than discovered in a symbolizer later.

| id | counter | what it counts | topology position |
|---|---|---|---|
| 47 | `WARP_ENTER` | block entries **from the run loop** | the loop hook — the one site that costs exactly one `INSNS`. Never the chained-transfer site |
| 48 | `WARP_BLK` | block executions, **including** chained entries | every block prologue |
| 49 | `WARP_INSNS` | guest instructions retired inside blocks | block epilogue, from the block's compile-time count; this is also what feeds the **cadence** counter (`Q10`, frozen) — a different counter from `INSNS`, and the two must not be confused |
| 50 | `WARP_BAIL` | bails to the interpreter | the bail arm, once per bail |
| 51 | `WARP_BAIL_ST` | writeback stores executed on bail arms | added on the bail arm from the compile-time emitted-store count (§0.5.4) |
| 52 | `WARP_COMPILE` | blocks compiled | the compiler's exit, success only |
| 53 | `WARP_CINSNS` | guest instructions compiled | ditto — denominator for code bytes per instruction |
| 54 | `WARP_CBYTES` | ARM code bytes emitted | ditto — R4's first-class metric |
| 55 | `WARP_CFETCH` | guest instruction words read **by the compiler** | the compile path's fetch site, and **not** the interpreter's ifetch bracket — this is what makes `FETCH`'s declared fall possible. **This is Q6's counter** |
| 56 | `WARP_DISC` | blocks discarded by invalidation, all causes | the code cache's hook on `mmu_flush_cache()`'s choke point, summing live blocks per invalidation |
| 57 | `WARP_DISC_ROOT` | ... of those, at `IV_ROOT` invalidations | the same hook, on the `IV_ROOT` cause only. **This is Q5's counter** |
| 58 | `WARP_FLUSH` | full code-cache flushes (bump allocator exhausted) | `flush_icache_hard()` |
| 59 | `WARP_DECLINE` | block-head PCs the engine declined | the loop hook's decline arm — rung-0 PCs, trace on, unsupported head |

C symbols are `Z3660_PROF_C_WARP_*`; the short names above are the table's. `WARP_LS_INSNS` and
`WARP_DIVERGE` belong to the **gated lockstep build** and are not part of the shipping counter
block. `warp_entered` — §0.5.1's every-variant arm confession — is the **same increment site** as
id 47, not a second one.

**No new buckets at M1, and that is deliberate.** Bracketing the compile path would charge the
instrument into the thing being measured — rung 0's own reasoning for being counters rather than
buckets applies here in its strongest form. **Compile cost per block is measured on target
*before* integration** (§2g claim 3), with a direct cycle bracket in a micro-benchmark, not in a
capture.

**Q5's decision rule, pre-registered.** Fund a per-address-space generation **only if both**:
`WARP_DISC_ROOT / WARP_DISC ≥ 0.10` **and** the recompilation attributable to it —
`WARP_DISC_ROOT × (measured compile cycles per block)` — is ≥ **2 %** of the window's cycles. The
dopc declined at 1.32 – 3.27 % of *requests* because a per-entry stamp cost three compares on a
path that runs every dispatch; for the JIT the lookup runs once per block, so the arithmetic
reverses — but it reverses on measured discard cost, not on the argument that it reverses.

**Q6's decision rule, pre-registered.** Resurrect rung 3's struck `ifetch` verdict table
(`3087af9`) on the **compile path only** if `WARP_CFETCH × 42` — 42 being the instructions the
struck table removed from `iword` on the 87 % ITT-matched arm — is ≥ **20 %** of the measured
compile-path cycles. The classification argument (nothing hides the compiler's fetch, because
there is no dopc hit in front of it and the compiler reads several words back to back) is a
hypothesis to price, not a claim to act on, and re-funding it from a bucket share before this
counter exists is the one thing §30's law strikes outright.

### 0.5.7 Sessions, and what M1 does not establish

**Session estimate — `assumed`, stated so it can be wrong in public, and not a commitment.**
§4 estimates M1 at 6 – 10. With the two-wave structure §0.5.3 predicts:

| | sessions | card needed |
|---|---|---|
| codegen for the frozen families, block cache, linkage, bail contract | 3 – 5 | no |
| host corpus (G4/G5), the static gates (G3), the `objdump` cost counts (§0.5.4) | 1 – 3 | no |
| metal: engagement + coverage on the profiling build; verdict A/B on the lean build; one contingency | 2 – 3 | yes |
| **M1** | **6 – 11** | |
| M1a, if coverage lands below the bar — one family wave plus its own metal verdict | +2 – 4 | yes |

**What M1 explicitly does not establish:**

1. **That a JIT pays on this machine.** M1 prices generation one on dhrystone. §8 stands
   unchanged and unrewritten.
2. **Anything about faults or demand paging.** The bail contract's fault path is exercised by
   construction at M1 but not *loaded*: dhrystone is small and resident and is *"the workload
   least likely to trip"* the paging path. **M2 is the first honest test**, and everything known
   about invalidation cost on this guest was measured on the same easy workload.
3. **Anything about the supervisor-heavy story.** Generation one is user-mode only; the boot row
   rides beside every M1 capture per `Q11` and **gates nothing before M4**.
4. **That the cache-maintenance sequence is correct at JIT volumes.** Q2's experiment A is one
   stub, not a volume. Q3's `UAEJIT` A/B is a separate metal session and the caveat stands: adding
   barriers is not evidence of a fix; only a soak on metal is.
5. **That the cadence contract is unmoved.** M1 asserts it by construction and checks it in the
   harness (mutation M-7); the behavioural proof is M3's PSTAT gap fingerprint against the
   card-1/piscsi class at 1 177 – 1 402 µs.
6. **That the ~42 µs budget holds for compilation.** It is inherited from rung 0's measured chunk
   and measured for compilation at M1 *per block*, not soaked.
7. **That the profiling build's absolute wall clock means anything.** §0.4.3's +7.22 % is
   unexplained; counters come from the profiling build, wall clock from the lean build, and no
   WARP row compares wall clock across two profiling builds until that mechanism is named.
8. **That any of these session counts are right.**

## 0.6 M1 round 1 — the plumbing slice, pre-registered (2026-08-23)

**This section is written before the first line of engine code and is scored as written.**
§3.4's two rules govern it exactly as they govern §0.5: the pre-registration commit precedes
the implementation commits, provably, by commit timestamp in this repository; and a
prediction that turns out wrong is marked `MISSED` beside the measurement with the
prediction left byte-untouched.

### 0.6.1 What round 1 is, and what it deliberately is not

**Round 1 builds the engine's skeleton end to end and generates no code.** Blocks are
discovered, keyed, cached, invalidated, evicted, entered and exited — and every block is a
**null block**: a generated stub that syncs nothing, retires nothing, and returns to the
interpreter at the PC it was entered at. There is no per-opcode compiler, no `MOVE` codegen,
no `Bcc`/`DBcc` inheritance, no `jnf_MEM_*` replacement. **No performance claim of any kind
is made or is admissible from this round**, and §0.5.2's own table says why in advance:
coverage is **0 by construction**, and *"coverage < 60 % … is not a verdict in either
direction"*. The kill line cannot fire on round 1 and must not be quoted against it.

What it exists to prove is the machinery §0.5 will be scored with: the switch, the counters,
the block cache, the invalidation wiring, the cadence contract, the bail accounting and the
lockstep oracle — **all of it before there is a compiler whose numbers could distract from
it.** The one number round 1 does produce is a **cost**: what it costs to look a PC up, enter
a block and come straight back out. §30's law says work prepended to the path arrives in
full, and this round measures the arrival before anything is built on top of it.

### 0.6.2 The no-empty-pass invariant, and the counter binding it forces

A null block creates a case §0.5.6 did not have to model, and the resolution is written here
rather than discovered in a capture.

> **THE NO-EMPTY-PASS INVARIANT.** A block entry either **retires ≥ 1 guest instruction and
> consumes the run-loop pass**, or **retires 0 and falls through to the interpreter inside the
> same pass**. There is no third outcome, and in particular no pass that retires nothing.

It is not a tidiness rule. Without it a null block at every dispatched PC would burn one
whole run-loop pass per guest instruction: `INSNS` would double, the engine would need a
re-entry guard to avoid an infinite loop at a bailed PC, and the wall clock would halve. With
it, progress is guaranteed by construction — a pass always retires at least one instruction,
whoever retires it — and no guard is needed at all.

**The counter binding that follows, and §0.5.6's own topology law is the authority for it.**
§0.5.6 defines `WARP_ENTER` (id 47) as *"the loop hook — the site that costs exactly one
`INSNS`"*, and warns that incrementing it anywhere that does **not** cost an `INSNS`
*"over-subtracts and reports coverage high"*. A fall-through entry costs no `INSNS` of its
own: the interpreted instruction in the same pass costs it. Therefore:

- **`WARP_ENTER` (id 47) counts pass-consuming entries only.** In round 1 it is **0**, always,
  because no block consumes a pass. The §2e claim-3 identity
  `DOPC_HIT + DOPC_MISS == (INSNS − WARP_ENTER) + FAULTS` therefore reduces to the standing
  identity in **both** arms, and holds exactly rather than approximately.
- **`WARP_BLK` (id 48) counts every block execution**, null ones included, and is therefore
  the quantity the **arm attestation** rides on. §0.5.1 requires the always-compiled
  `warp_entered` confession to be *"one increment site, not two"* and to be the same quantity
  as a counter in the profiled block; that requirement is met exactly, with id 48 in id 47's
  place. **The binding moves; the property §0.5.1 asked for does not.** Binding it to id 47
  instead would have made a lean arm-attestation read zero on an armed boot, which is the one
  thing an attestation may not do.

### 0.6.3 The six things round 1 builds

1. **The switch.** `WARP` console command; `SHARED.warp_on` at **`0xFFFF00F0`** (the slot
   `IFAST`, `SPEC` and the withdrawn `IVSUP` each briefly held and gave back), `SHARED` grows
   to `0xF4`. **`unset = OFF`** (§0.1 Q9) — the deliberate house-rule inversion, with the
   reason recorded at the switch itself and not only here. **Arming takes an exact 32-bit
   magic**, `0x57415250`, on the `IVSUP` precedent: `SHARED` is hand-maintained in two headers
   that a beta resync has already mis-merged once, so a neighbouring field's value bleeding
   into this one must **disarm**, which a magic-word test guarantees and a truthiness test does
   not. Core0 **republishes the OFF sentinel unconditionally on every boot** from both
   config-resolve paths, because the mailbox is DDR that survives a warm reset — and the
   harness carries the **anti-vacuity check** that a reader who deletes the republish as
   redundant fails. Banner on arm and on disarm, printed by **core1**, because the console can
   only write a mailbox word. `warp_entered` is compiled into **every** variant.
2. **Counters 47 – 59** exactly as §0.5.6 pre-registered them, with **id 46 permanently
   retired** behind a name-table placeholder so the indices align, and `Z3660_PROF_C_COUNT`
   46 → **60**. No id below 46 changes meaning, no field changes shape, **the dump version
   stays 2**.
3. **The block manager.** Discovery at interpreter dispatch, armed only. **Keyed on guest
   virtual PC with the supervisor bit in bit 0**, in the dopc's encoding, with the dopc's
   **set-relative empty marker** — the M0-refuted `regs.pc_p` host-pointer keying (§0.3(c),
   §2b) is replaced, not adapted. Generation invalidation hung off the existing
   `z3660_dopc_invalidate()` choke point for **all** causes, so the code cache is opted *out*
   of a new upstream cause rather than into it; `WARP_DISC` / `WARP_DISC_ROOT` (Q5's counters)
   live from day one. Eviction is §2b's decision unchanged: bump-allocate, then throw
   everything away and bump the generation (`WARP_FLUSH`).
4. **The bail contract, skeleton form.** Block entry records the block-head guest PC; the null
   block syncs nothing because nothing is dirty; the cadence feed exists **now**, feeds the
   service-cadence counter with **retired guest instructions** (Q10, frozen), and carries its
   M-7 mutation arm. Stubs are emitted through the code cache's own arena and published with
   the **Q3-augmented** sequence — the shipped `arm_flush_cache` + `arm_icache_invalidate`
   plus **BPIALL + DSB + ISB**, which is byte-for-byte the sequence stub B of the Q2 probe ran
   under on metal (§0.4.1).
5. **The lockstep oracle, host-harness mode.** Armed against disarmed over a corpus, compared
   per instruction across §3.1's set plus the JIT's four. `WARP_LS_INSNS` / `WARP_DIVERGE`
   live in a **gated build** that is compiled out of every shipping image.
6. **The harness corpus and the mutation arms** that apply to this slice.

**What round 1 does NOT build, and it is listed so that its absence is a decision rather than
an omission:** the pass-consuming arm of the run-loop hook (it arrives with the first block
that retires something, in round 2, and its price is what round 1 measures); the rung-0 PC
decline (Q8 is honoured in round 1 by a stronger property — a null block retires nothing, so
rung 0 runs in the same pass exactly as it does today, and `BLK_HIT`/`BLK_INSNS`/`BLK_BYTES`
must read **identical** in both arms); and the supervisor gate's own counter — supervisor PCs
are declined **before** discovery and are deliberately not counted in `WARP_DECLINE`, because
one decline per supervisor instruction would make that counter a second copy of `INSNS_SUPER`
and say nothing. The supervisor rate is read from `INSNS_SUPER`, which already exists.

### 0.6.4 The round-1 gates, with their predictions

Every one of these is a prediction made before the measurement exists.

| gate | prediction |
|---|---|
| **G-R1.1 harness** | green (`0 failed`) in all three variants. New `CHECK`s: **+150 … +320** in the default variant, the **same** number in lean, and lean+prof gaining a further **+10 … +40** counter-only checks. Baseline is 3 186 / 3 182 / 3 390 |
| **G-R1.2 disarmed object equivalence** | per-function normalised-disassembly diff of the **lean** `Z3660_emu.elf` against `3f33a6f`, addresses and branch-target immediates normalised away: **≤ 12 functions changed**, and every changed function on the WARP path, `m68k_run_mmu040`, `z3660_knobs_sync` or `z3660_dopc_invalidate`. `m68k_run_mmu040` grows **+8 … +40 bytes** from its standing 2 288 |
| **G-R1.3 lockstep identity** | `WARP_DIVERGE == 0` over `WARP_LS_INSNS ≥ 10⁷`, and the two arms' final register file **and** guest memory byte-identical. A single divergence is a stop, not a statistic |
| **G-R1.4 counter identities** | `DOPC_HIT + DOPC_MISS == (INSNS − WARP_ENTER) + FAULTS` **exact** in both arms with `WARP_ENTER == 0` armed; `FAULTS(on) == FAULTS(off)` **exact** on the deterministic host corpus (the ±1 of §0.5.5 is a metal allowance); `sum(IV_*) − IV_WRAP == DOPC_INVAL` unchanged; `WARP_DISC` reconciles against the same invalidation stream; `BLK_*` identical between arms |
| **G-R1.5 mutation arms** | each **caught**, and each shown to be caught in the log rather than merely asserted: **M-5** the block key drops the supervisor bit; **M-7** the cadence counter fed 1 per block instead of retired instructions; **M-ICACHE** a null block entered without the publish sequence; **M-ARM** a truthy word arms the switch; **M-REPUB** the per-boot republish deleted |
| **G-R1.6 images** | the three shipping images rebuilt with the switch present, CRCs recorded, the archive firing for each. `BOOT-q2probe.BIN` is **not** rebuilt |
| **G-R1.7 entry cost** | from the **lean** `objdump`, before any metal time: the **disarmed** gate costs **3 … 6** ARM instructions per guest instruction; the **armed** null-block path costs **24 … 40** ARM instructions per *user-mode* guest instruction |

### 0.6.5 The metal expectation, derived and stated before the session

The naive expectation is that a block which bails immediately costs nothing, so an armed
null-block row should land inside the disarmed row's own spread. **The arithmetic disagrees,
and it is written down here rather than after the row arrives.**

```
   lean dhry 8 151 x 643 guest instructions per dhrystone iteration (M0 capture, S0.4.2)
        = 5.24 M guest instructions/s at 1.1 GHz  ->  ~210 ARM cycles per guest instruction
   an added instruction costs ~1 cycle at IPC ~ 1, less where the A9 dual-issues it
```

| row | derived band | notes |
|---|---|---|
| disarm penalty — the WARP image **disarmed** against the `F294E75A` cross-build row (8 151) | **−0.5 % … −2.9 %**, dhry **7 915 … 8 110** | the shipped cost of *having* the switch, on 3 – 6 gate instructions. §3.5's table says this shape is normal: the three shipped rungs measured disarm penalties of −3.74 %, −1.58 % and −1.07 % |
| same-boot switch — **armed** against **disarmed** on the same image | **−5 % … −16 %**, armed dhry ≈ **6 700 … 7 700** | 24 – 40 instructions on dhrystone's **83.91 %** user-mode share (§0.4.2's 16.09 % supervisor), i.e. 20 – 34 cycles on ~210 |

**This is a cost, not a verdict**, and the reading rule is stated before the row exists:

- The M1 kill line **does not fire** on it. Coverage is 0 by construction, and §0.5.2's table
  is explicit that a sub-band coverage row is not a verdict in either direction.
- A row **inside** the band confirms the pricing law's first bite and sizes the entry
  sequence that round 2 must amortise over a block's length. It is the input to the obvious
  round-2 lever — restricting discovery to control-transfer targets instead of every
  dispatched PC — which is thereby **priced by measurement rather than adopted by taste**.
- A row **above** the band (cheaper than predicted) is scored `MISSED` in the cheap direction
  and the `objdump` count is re-read before anything is concluded from it.
- The identity `cross-build × disarm-penalty = same-boot switch` must close, as it has three
  times (§3.5). A round-1 row where it does not close is a measurement error, not a result.

### 0.6.6 What round 1 explicitly does not establish

1. **Anything about speed.** There is no compiler. Every number this round produces is a cost.
2. **That the emitted sequence is correct on ARM.** The host harness proves the *decisions*
   and asserts the emitted words **byte-exactly**, but it executes not one of them (§3.2's
   honest limit). The stub's execution is proven on target and only on target.
3. **That the cache-maintenance sequence is correct at volume.** Q3's `UAEJIT` A/B is still a
   separate metal session and the platform caveat stands: adding barriers is not evidence of a
   fix; only a soak on metal is.
4. **That the cadence contract is unmoved on metal.** Round 1 asserts it by construction and
   in the harness (M-7); the behavioural proof remains M3's PSTAT gap fingerprint.
5. **Anything about faults or demand paging.** No block can fault, because no block does
   anything.

## 0.7 M1 round 1 — built, and its gates scored (2026-08-24)

**The pre-registration in §0.6 was committed at 23:43:16 on 2026-08-23; the first
implementation commit landed at 00:15:34 on 2026-08-24, 32 minutes later. The order is a
fact in this repository's log, which is what §3.4 asks of it** — the SPEC round's
pre-registration preceded its code by 26 minutes and the `f` round's by 29. Every prediction in §0.6 is left
byte-untouched below and scored beside itself, `ifetch-fastpath.md`'s standard.

### 0.7.1 What was built

| piece | where |
|---|---|
| the contract — switch rule, block key, geometry, the no-empty-pass invariant | `Z3660_emu/src/uae/z3660_warp.h` |
| the engine — block cache, discovery, the null-block emitter, publish, invalidation, eviction, the lockstep oracle | `Z3660_emu/src/uae/z3660_warp.cpp` |
| the run-loop hook, the knob resync + banner, the cadence feed, the invalidation join | `Z3660_emu/src/uae/newcpu.cpp` |
| the code cache's arena accessor | `Z3660_emu/src/uae/jit/compemu_support_arm.cpp` |
| counters 47 – 59, id 46 retired behind a placeholder, `Z3660_PROF_C_COUNT` 46 → 60 | `z3660_prof.h`, `z3660_prof.cpp` |
| `SHARED.warp_on` at `0xFFFF00F0`, both copies | `Z3660/src/main.h`, `Z3660_emu/src/main.h` |
| the `WARP` console command; the unconditional per-boot republish, both config paths | `Z3660/src/debug_console.c`, `Z3660/src/config_file.c` |
| the corpus, the five mutation arms, the lockstep run | `Z3660_emu/test/host/harness_main.cpp` |
| the object-equivalence tool | `docs/warp-objdiff.py` |

### 0.7.2 The gates

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| **G-R1.1** harness | green all three variants; **+150 … +320** checks in default, the same in lean, lean+prof a further **+10 … +40** | green, `0 failed`: **3 250 / 3 246 / 3 473** against 3 186 / 3 182 / 3 390 — **+64 / +64 / +83**; default and lean gained the same number, prof a further +19 | green; **count MISSED, low** |
| **G-R1.2** disarmed object equivalence | **≤ 12 functions changed**; every one on the WARP path or `m68k_run_mmu040` / `z3660_knobs_sync` / `z3660_dopc_invalidate`; `m68k_run_mmu040` **+8 … +40 bytes** | **12 changed of 25 125 common** (lean; 13 in each of default and prof), +11 new symbols and −1; `m68k_run_mmu040` `0x8f0` → `0x914` = **2 288 → 2 324 B, +36** | **HIT**, with one honest qualification below |
| **G-R1.3** lockstep identity | `WARP_DIVERGE == 0` over `≥ 10⁷`; final registers and guest memory byte-identical | `WARP_DIVERGE = 0` over **10 000 000** compared boundaries; final register file, pc, restart pc and data page byte-identical; the armed arm executed **10 000 000** blocks (the anti-vacuity check) | **HIT** |
| **G-R1.4** counter identities | `DOPC_HIT + DOPC_MISS == (INSNS − WARP_ENTER) + FAULTS` exact both arms with `WARP_ENTER == 0`; `FAULTS` equal; `sum(IV_*) − IV_WRAP == DOPC_INVAL`; `WARP_DISC` reconciles; `BLK_*` identical | all exact, in the profiling variant, on a 20 000-instruction window: `INSNS`, `DOPC_HIT+DOPC_MISS`, `FAULTS`, `DOPC_INVAL` and all three `BLK_*` **identical between the arms**; `WARP_ENTER = 0`, `WARP_INSNS = 0`, `WARP_BAIL == WARP_BLK`, `WARP_CBYTES == 8 × WARP_COMPILE`, `WARP_CFETCH = 0` | **HIT** |
| **G-R1.5** mutation arms | each caught, and shown to be caught | **M-5** supervisor lookup of a user block: hits 0 clean / 1 mutated. **M-7** 41 retired instructions at cadence 4: 10 polls clean / 0 mutated. **M-ICACHE** unpublished entries: 0 clean / 200 mutated. **M-ARM** 196 608 swept words arm nothing, and no value any neighbouring field can hold arms it. **M-REPUB** a resync that finds the same word leaves it armed; the boot publish disarms it | **HIT**, all five printed as well as asserted |
| **G-R1.6** images | three rebuilt, CRCs recorded, archive fires; `q2probe` not rebuilt | `BOOT.BIN` **`9C7A2AD3`** (was `BF56576A`), `BOOT-lean.BIN` **`E80D04EE`** (was `F294E75A`), `BOOT-prof.BIN` **`423CB391`** (was `5225BD9C`); all three archived CRC-stamped, the outgoing three still in the archive; `BOOT-q2probe.BIN` untouched at `FCC0CB3F` | **HIT** |
| **G-R1.7** entry cost | disarmed gate **3 … 6** ARM instructions; armed null-block path **24 … 40** per user-mode guest instruction | disarmed **5**; armed **60** | disarmed **HIT**; armed **MISSED, high** |

**G-R1.2's qualification, stated rather than buried.** The build is `-flto`, so adding a
translation unit re-runs whole-program optimisation and a handful of functions change for
reasons that have nothing to do with WARP. Of the twelve: `z3660_dopc_invalidate`
(1 → 27 instructions — `z3660_dopc_do_invalidate` got inlined into it and the WARP hook
added), `z3660_knobs_sync` (42 → 67 — the WARP resync block) and `m68k_run_mmu040`
(572 → 581 — the hook) are the change; `z3660_dopc_invalidate_dma` (1 → 2, a tail call that
stopped being one because its callee grew) and seven others — `build_cpufunctbl`,
`fill_prefetch`, `flush.part.0`, `op_d188_5_ff`, `op_53f0_14_ff`, `op_54c0_14_ff`,
`floatx80_tentox`, `floatx80_twotox` — are LTO churn, each ±1 instruction or a reordering.
**Under `-flto` there is no such thing as a perfectly confined object-level change, and a
method that claimed one would be hiding the compiler rather than measuring it.**

The method is `docs/warp-objdiff.py`, committed beside this document so the number can be
re-derived: per-function disassembly with addresses, branch targets and `movw`/`movt`
address immediates normalised away. **Whole-image byte identity — the right test after a
revert, and used as one here twice — is the wrong test after an addition**: a new
translation unit moves every later symbol, and on this target (`-DARMV6T2`, no literal
pool) every global's address is baked into the instruction stream, so a byte compare
reports 21 430 changed functions and tells you nothing. The relaxed mode canonicalises
`movw`/`movt` immediates outright, which removes relocation entirely at the price of making
a changed *constant* invisible; what it proves is that the instruction stream's shape and
every branch target are unchanged, and the harness proves the behaviour.

### 0.7.3 The entry cost, read off the lean disassembly — and the prediction it missed

**Pre-registered 24 – 40 ARM instructions on the armed path. Measured 60. MISSED, high.**
The prediction is left where it was written; here is what the disassembly actually shows,
counted instruction by instruction along the steady-state path (block already cached,
`build-lean/Z3660_emu.elf`, `E80D04EE`):

| segment | instructions |
|---|---|
| the hook in `m68k_run_mmu040` — flag load, test, branch, the two argument loads, `bl` | 9 |
| `z3660_warp_dispatch` prologue, trace gate, odd-pc/supervisor gate | 9 |
| the lazy `warp_inited` check | 4 |
| the lookup — index, key, stamp compare, tag compare | 13 |
| the 64-bit arm-attestation increment | 8 |
| the indirect call and the null block itself (`mov r0,#0` / `bx lr`) | 4 |
| `z3660_warp_cadence_feed(0)` — call, prologue, early-out, return | 10 |
| epilogue | 3 |
| **total, armed, per user-mode guest instruction** | **60** |
| **total, disarmed** — flag load, argument load, flag test, branch taken | **5** |

**Four of the eight rows are not the engine; they are the shape of the code around it, and
naming them now is what makes the next round's work a measurement rather than a taste.**
The lazy init check (4) belongs at arm time, not on the dispatch path. The cadence feed's
call and prologue (10) are a function call to do nothing — its `n <= 1` early-out belongs at
the call site. The 64-bit attestation (8) is eight instructions for one increment. And
`z3660_warp_dispatch` is out of line, so the hit path pays a full AAPCS prologue and
epilogue for a lookup — the decoded-op cache's `always_inline` on `z3660_dopc_find()` exists
for exactly this reason, and its comment says why: at `-Os` gcc will happily move a hot
lookup out of line to save space, *"which is the exact trade this rung exists to refuse"*.
**Together those are ~22 of the 60.** They are recorded here and **not fixed in this round**:
tuning a number after seeing it, against the pre-registration it was scored on, is the
re-scoping §0.2's kill line strikes outright. They are round 2's first work, and round 2
re-registers its own band.

**The metal expectation, re-derived from the measurement.** §0.6.5's band was derived from
24 – 40 instructions and is left as written; at 60 the same arithmetic gives:

```
   armed:     60 x 0.8391 user-mode share  =  50 cycles on ~210      ->  -19.2 % at IPC 1
                                              33 cycles at IPC 1.5   ->  -13.6 %
   disarmed:   5 instructions               =   5 cycles on ~210     ->   -2.3 % at IPC 1
```

| row | §0.6.5 as pre-registered | re-derived from the measured counts |
|---|---|---|
| disarm penalty vs the `F294E75A` cross-build row (8 151) | −0.5 % … −2.9 %, dhry 7 915 … 8 110 | **−1.6 % … −2.3 %, dhry 7 963 … 8 021** |
| same-boot switch, armed vs disarmed | −5 % … −16 %, armed dhry ≈ 6 700 … 7 700 | **−13.6 % … −19.2 %, armed dhry ≈ 6 450 … 6 900** |

The reading rules are §0.6.5's, unchanged: **this is a cost, not a verdict**, the kill line
cannot fire on a round whose coverage is 0 by construction, and the identity
`cross-build × disarm-penalty = same-boot switch` must close or the session is a measurement
error rather than a result.

### 0.7.4 Three things worth recording that were not gates

- **The null block's two words are emitted as one NEON store.** `-Os` turned the emitter's
  two 32-bit stores into `vldr d16, [pc, #152]` + `vst1.32 {d16}, [r3]!` — the two words
  `e3a00000` / `e12fff1e` sit in the function's own literal pool. The emitted bytes are
  exactly the two the harness asserts; only the way they get written changed.
- **G3 — zero `throw` sites reachable from generated code — is trivially met in round 1**,
  and is recorded rather than skipped: generated code is two instructions and calls nothing,
  so the reachable call graph from it is empty. The audit becomes real work in round 2, when
  the emitted sequence starts calling helpers.
- **The engine refuses the arena if the classic JIT owns it.** `z3660_warp_cache_arena()`
  returns NULL when `compiled_code != NULL`, which is the `UAEJIT_*` bootmodes. Two engines
  bump-allocating one 32 MB arena is a wild-write generator; refusing makes it impossible
  rather than unlikely, and WARP then declines every block and reports it in `WARP_DECLINE`.

## 0.8 M1 round 2 — pre-registered, before the first line of round-2 code (2026-08-24)

**This section is written before any round-2 engine code exists and is scored as written.**
§3.4's two rules govern it exactly as they governed §0.5 and §0.6: the pre-registration
commit precedes the implementation commits, provably, by commit timestamp in this
repository; and a prediction that turns out wrong is marked `MISSED` beside the measurement
with the prediction left **byte-untouched**. Round 1's own pre-registration preceded its code
by 32 minutes, the SPEC round's by 26 and the `f` round's by 29.

**Round 2 is the round that generates code.** Round 1 proved the plumbing with a null block
and priced the entry; §0.7.3 named ~22 of its 60 armed instructions as *"the shape of the
code around it"* and refused to fix them in the round they were scored in. This is that
round, and it re-registers its own band rather than inheriting one.

### 0.8.0 The measured inputs round 2 is built on

Every number below is from the metal cost session of 2026-08-24 (evidence
`2026-08-24-warp-r1-metal/MATRIX.md`) or from §0.7, and none of it is re-derived here.

| input | value | what it decides in round 2 |
|---|---|---|
| clean reference `R`, `F294E75A` lineage, dhry c4, n = 7 | **8 174**, spread 0.55 % | the denominator of every ratio below |
| disarm penalty, round-1 image | **7 988 = −2.276 %** of `R`, on **5** gate instructions | 5 instructions cost 4.88 cycles on 214.2 → **the disarmed gate runs at IPC ≈ 1.03** |
| armed null-block row | **5 301 = −33.638 %** of the disarmed row | the entry sequence's arrival, in full |
| the conversion factor, measured | **IPC 0.46** on the armed entry path (129.4 cycles for 60 instructions, per user-mode guest instruction) | **one ARM instruction on the entry path costs 2.17 cycles**, not 0.67–1.0. This is the single most important number round 1 bought |
| `WARP_BLK` / user-mode `INSNS` | **0.999878** | zero amortisation: round 1 paid the whole entry for one guest instruction, every time |
| `WARP_COMPILE` | 3 066 944 = 5.68 % of entries | one compile per 17.6 executions |
| `WARP_DISC` / `WARP_FLUSH` | 146 536 / **0** | invalidation explains 4.8 %, eviction 0 → **95.2 % unexplained** |
| `WARP_CBYTES` | 24 535 552 B = **73 %** of the 32 MiB arena in one ~34 s window | the eviction path is untested, not absent |
| Q5 condition 1 | `WARP_DISC_ROOT / WARP_DISC` = **0.274 ≥ 0.10** ✔ | condition 2 still unmeasured |

> **The kill line still cannot fire, and this is restated rather than assumed.** §0.5.2's
> operational form needs a **same-session** engine-off reference row `R` and an engine-on
> median taken beside it. **Round 2 takes no metal row at all** — it is a build-and-gate
> round, exactly as round 1's implementation half was. No number in this section is a
> verdict, none of them is admissible against §0.2, and the M1 kill line fires for the first
> time only at the verdict session that takes its rows. **`kill-R` is the CLEAN INTERPRETER
> LINEAGE row** — the engine-off cross-build reference on the then-current interpreter lean,
> not a WARP image disarmed — per the ruling of 2026-08-24. A disarmed WARP image already
> carries the switch's own shipped cost and using it as `R` would grade the engine against a
> baseline it had already slowed down.

### 0.8.1 What round 2 builds

Four things, in the order their evidence demands, plus the first code generation.

**1. The entry cost, against the four shapes §0.7.3 named.** The lazy `warp_inited` check
(4 instructions) moves to arm time. The cadence feed's call-and-prologue-to-do-nothing (10)
becomes an early-out at the call site. The 64-bit arm attestation (8) becomes a split
32-bit counter with a wrap arm. `z3660_warp_dispatch` stops being an out-of-line call: the
steady-state lookup-and-enter path is `always_inline` in the header, on the decoded-op
cache's own precedent (*"at `-Os` gcc will happily move a hot lookup out of line to save
space, which is the exact trade this rung exists to refuse"*).

**2. The discovery policy: control-transfer TARGETS only.** §0.6.5 named this in advance as
the round-2 lever and round 1 priced it: at `WARP_BLK`/`INSNS` = 0.999878 there was no
amortisation to have. Fall-through **continues in the interpreter** until the next target.
The target test is armed-only and costs the disarmed path nothing: it is a PC-discontinuity
test against the previous pass's PC, held in the engine's own state, so no store is added to
the interpreted path. It is an **over-approximation by construction** — a forward branch
shorter than the longest instruction it could have been reads as a fall-through — and that is
a coverage property, never a correctness one.

**3. The lookup structure — THE MUST-FIX.** Diagnosed in §0.8.2 and fixed there.

**4. Generation one's compilers**, per §0.5.1's frozen scope and no wider.

### 0.8.2 The lookup-structure diagnosis, and the fix

**The diagnosis is structural and reads off the round-1 source without a capture.**

```
   Z3660_WARP_SETS           1024          one block per set -- DIRECT MAPPED
   Z3660_WARP_INDEX(pc)      (pc >> 1) & 1023
```

The index consumes **PC bits 1..10 and nothing else**, so the table addresses a **2 KiB
window of guest text**. Two block heads 2 048 bytes apart share one slot, and with one way
each arrival evicts the other. Round 1 discovered at **every dispatched PC**, so the whole of
dhrystone's user text — tens of KiB, against a static working set of order 10³ distinct PCs —
folded into a 2 KiB alias window with no associativity to absorb it. That is the shape the
counters saw: a compile rate of 5.68 % of executions with `WARP_DISC` explaining 4.8 % of it
and `WARP_FLUSH` explaining none.

> **The decoded-op cache uses the same index formula and does not thrash, and the difference
> is the whole finding.** `Z3660_DOPC_INDEX` is bit-for-bit `(pc >> 1) & 1023` — but the dopc
> has **four ways** with a round-robin victim, and it caches a *decode*, which costs one to
> three orders of magnitude less to rebuild than a *block*. Round 1 inherited the index and
> not the associativity, and wrote down why (*"a way count is a decision to take against a
> measured head distribution rather than by analogy"*). The measured head distribution is now
> in: it says take the ways.

**The fix, and every part of it is a decision with a reason:**

| change | from | to | why |
|---|---|---|---|
| ways | 1 | **4** | the dopc's own geometry, now with a measured head distribution behind it. Round-robin victim, the dopc's `victim` field verbatim |
| sets | 1024 | **2048** | 2048 × 4 × 16 B = **128 KiB**, and a set is **64 B = one Cortex-A9 line**, so a lookup still touches one line and never two. That is the dopc's `Z3660_DOPC_SET_BYTES 64` constraint, inherited |
| index | `(pc >> 1) & 1023` | `((pc >> 1) ^ (pc >> 12) ^ (pc >> 23)) & 2047` | **high PC bits participate.** Two heads 2 048 (or 4 096, or 1 MiB) apart no longer collide by construction. It costs **two `eor`s on a path that runs once per BLOCK** — which is exactly the arithmetic §0.5.6 says reverses for a block table and does not for the dopc |
| declined heads | re-attempted on every arrival | **cached as a null block** | a head the engine declines is a permanent property of that PC until an invalidation. Round 1 re-ran the whole discovery every time. A declined head compiles to round 1's two-word null block, so the fast path needs no NULL test and the decline costs one lookup |
| the instrument | none | **`WARP_EVICT`, counter id 60** | §0.7's own words: *"A table-occupancy counter settles it in round 2."* It counts a **live** block overwritten in its set by a different key — the one term that separates conflict from invalidation and from flush |

**Counter id 60 is appended and nothing moves.** `Z3660_PROF_C_COUNT` goes 60 → **61**, id 46
stays retired behind its placeholder, no existing id changes meaning, no field changes shape,
**the dump version stays 2** — the same reasoning ids 20, 46 and 47–59 were appended under.

**The recompile budget, decomposed, and this is the form the fix is scored in:**

```
   WARP_COMPILE  =  first-compile of a head          (irreducible)
                 +  WARP_DISC        (invalidation)  (a guest property, not ours)
                 +  WARP_EVICT       (conflict)      (OURS -- this is the term being fixed)
                 +  blocks lost to WARP_FLUSH        (capacity)
```

### 0.8.3 Generation one's compilers, and the design that makes the bail contract cheap

**The scope is §0.5.1's, frozen, and round 2 does not widen it.** `Bcc`/`BRA`, `DBcc`,
`MOVE`/`MOVEA` with both EAs in `Dn`/`An`/`(An)`/`d16(An)`, and register-destination ALU
(`ADD` `SUB` `AND` `OR` `EOR` `CMP` register-to-register, `ADDQ`/`SUBQ` with a register
destination, `ADDA`/`SUBA`/`CMPA` with a register source). `MOVEQ` is **M1a item 1 and is not
built**; `(An)+`/`-(An)` are excluded in either operand; `BSR` is a fully memory-touching
family and bails; every write-back family, every FPU opcode, every privileged or
mode-changing instruction, `MOVE16`, every uncompiled opcode and every rung-0 PC bail.

**The audit §0.5.1 demanded, done before the code and answered against expectation.**
§0.3(a) calls `Bcc` (45/45) and `DBcc` (16/16) *"inheritable as-is"*, and §0.5.1 already
narrowed that to *"a statement about the memory model, and only about it"*. Read against the
bail contract the answer is stronger and it is recorded here as a finding rather than
discovered in the diff: **the existing `Bcc`/`DBcc` compiler bodies are not inheritable.**
They compute targets and chain blocks through `regs.pc_p` and `cache_tags` — the host-pointer
keying §0.3(c) refuted under the MMU — and they operate on `compemu`'s lazy virtual register
file, whose three lazinesses (§0.3(b): `DIRTY` registers, an unfolded `m68k_pc_offset`, a
three-state flag machine) are exactly the state a bail would have to materialise. **What is
inherited is the raw ARMv7 emitter layer (`jit/codegen_arm.h`) and nothing above it.**

**The state discipline round 2 adopts, and it is shape (ii) taken to its limit.**
§0.3(b) offered two shapes for the guest logical PC and §0.5.4 pre-registered shape (ii) —
block-head PC plus a per-site constant delta, materialised **only on the bail arm**. Round 2
takes it, and takes the same decision one level further:

> **THE GUEST REGISTER FILE STAYS IN MEMORY.** Generated code loads its operands from
> `regs.regs[]` and stores its results back within the same guest instruction. No guest
> register is ever live in a host register across an instruction boundary, so at **every**
> instruction boundary in a block the architectural state is already exactly what the
> interpreter's boundary state is.

The consequence is the whole reason for the choice, and it is a pre-registered measurement
rather than a claim: **on-path state-sync per faultable site becomes 0 emitted instructions,
and a bail materialises the PC and nothing else.** §0.5.4 pre-registered 0–2 on-path and
3–8 emitted stores per bail; the second of those is now predicted to miss **low**, and the
prediction is written here so the miss is scored rather than celebrated.

It also costs something, and the cost is named before the measurement: a memory-resident
register file re-loads operands the interpreter would also have re-loaded, so generation one
buys its speed from **dispatch, fetch and decode**, not from register allocation. Route B's
allocator remains what §0.1 Q1 said it was — a redesign to be funded by §0.5.4's two measured
conditions, not by taste.

**The `jnf_MEM_*` replacement, and one declared deviation from the round's brief.** The
memory access emitted for `(An)` and `d16(An)` is the **DFAST verdict-table fast path plus
bail-to-interpreter**, per Q1 — but it is emitted as a **call to one shared helper that
contains `z3660_dfast_read()` / `z3660_dfast_write()` itself**, not as an open-coded copy of
the verdict test, the page-cache probe and the bank test.

> **The reason is equivalence by construction rather than by transcription.** The helper
> calls the *same* `always_inline` resolver the interpreter's accessors call, so "the two
> paths agree" is a property of there being one path, not of two transcriptions matching. An
> open-coded sequence would be ~35 emitted instructions reproducing `mmu_pagemaski`,
> `mmu_pageshift1m`, the ATC tag compare, `mem_banks[]` and `addrbank_is_direct_*` by hand —
> five separate opportunities to ship a divergence under the banner of a speed change.
> **And in this design a call is nearly free**: nothing is dirty, so there is no state to
> flush before it. The open-coded expansion is therefore **priced, not skipped**: the
> `objdump` cost of both shapes is read in this round and the inline expansion becomes a
> round-3 item with a measured saving in front of it rather than an assumption behind it.

The helper is where the structural fences live, all of them refusals rather than handled
cases: misalignment bails, an access that crosses a page bails, a `SLOW` verdict bails, an
ATC miss bails, a non-direct bank bails, and `TTR_NO_WRITE`'s asymmetry (read `XPAR`, write
`SLOW`) is the resolver's own and is not re-implemented. **G3 is met by construction and is
checked, not assumed:** the helper's reachable call graph must contain no `__cxa_throw`,
`__cxa_allocate_exception`, `_Unwind_RaiseException` and no `TRY` block — which is exactly
why it declines instead of translating.

**Block shape.** A block ends at the first uncovered opcode (`WARP_DECLINE`), at a control
transfer per the block-shape rules, at the §2e write-back-family exclusions, and at a page
boundary. Every exit — normal or bail — stores the guest PC and returns the number of guest
instructions **fully retired**, which is a compile-time constant at every exit site. The
no-empty-pass invariant is unchanged and its pass-consuming arm, absent by design in round 1,
lands here.

**One behavioural property that changes, declared before it is measured.** With a block
consuming a pass, `do_specialties()` runs once per **block** instead of once per
**instruction**, so interrupt *recognition* is deferred by up to `L` guest instructions.
The interrupt *pump* is not affected — the cadence feed still advances the service counter by
retired instructions and polls once per crossing (Q10, frozen, and M-7 still bites). At the
pre-registered `L` below, the deferral is under ten guest instructions against a pump tick of
`256 × service_cadence`. Trace stays excluded at the entry gate, so no trace exception can be
owed by a block.

### 0.8.4 The round-2 gates, with their predictions

Every one of these is a prediction made before the measurement exists.

| gate | prediction |
|---|---|
| **G-R2.1 harness** | green (`0 failed`) in all three variants. New `CHECK`s: **+120 … +400** in the default variant, the **same** number in lean, lean+prof gaining a further **+10 … +60** counter-only checks. Baseline is 3 250 / 3 246 / 3 473 |
| **G-R2.2 disarmed object equivalence** | per-function normalised-disassembly diff of the **lean** `Z3660_emu.elf` against `b16967d`: **≤ 25 functions changed**, and every changed function on the WARP path, `m68k_run_mmu040`, `z3660_knobs_sync` or `z3660_dopc_invalidate`, plus LTO churn named individually. `m68k_run_mmu040` grows **+40 … +160 bytes** from its round-1 2 324 |
| **G-R2.3 lockstep identity** | `WARP_DIVERGE == 0` over `WARP_LS_INSNS ≥ 10⁸`, with the armed arm **executing the emitted ARM words** (§0.8.5), and the two arms' final register file, flags, `instruction_pc`, `mmufixup`, `mmu_restart` and guest memory byte-identical. A single divergence is a stop, not a statistic |
| **G-R2.4 counter identities** | `DOPC_HIT + DOPC_MISS == (INSNS − WARP_ENTER) + FAULTS` **exact** in both arms — and now with `WARP_ENTER > 0`, which is the first round in which that correction is load-bearing rather than vacuous. `FAULTS(on) == FAULTS(off)` **exact** on the deterministic host corpus. `sum(IV_*) − IV_WRAP == DOPC_INVAL` unchanged. `WARP_INSNS == WARP_CINSNS`-weighted block lengths reconcile. `BLK_*` identical between arms (Q8: the JIT declines rung-0 PCs) |
| **G-R2.5 fault injection** | across every covered family, a fault made reachable at a mid-block faultable site must produce a **bail**, never a fault inside generated code: `FAULTS` equal between arms, and at the fault boundary all sixteen registers, the guest PC, `instruction_pc`, the CCR + X, `mmu_restart` and `mmufixup[0..1]` **byte-identical** to the interpreted arm. Predicted: **0 faults taken inside a block, in every injection case** |
| **G-R2.6 mutation arms** | each **caught**, and each shown to be caught in the log rather than merely asserted. Round 1's five (**M-5**, **M-7**, **M-ICACHE**, **M-ARM**, **M-REPUB**) still bite, plus: **M-3** state-sync omitted at one faultable site; **M-4** the `mmufixup` invariant not asserted at block head; **M-8** the bail arm writes back one class of state and not another; **M-CONFLICT** the round-1 lookup geometry restored; **M-XFER** the discovery gate fires at every PC; **M-FENCE** the helper's misalignment/page-cross fence removed; **M-VERDICT** the verdict table's decline arm made permissive; and **one wrong-flag mutation per covered family** (`MOVE` test-flags, `ADD` carry, `SUB`/`CMP` carry-inversion, `Bcc` condition, `DBcc` counter) |
| **G-R2.7 images** | the three shipping images rebuilt with the compiler present, CRCs recorded, the archive firing for each. `BOOT-q2probe.BIN` is **not** rebuilt |
| **G-R2.8 entry cost, from the lean `objdump`** | the **disarmed** gate costs **3** ARM instructions per guest instruction (round 1: 5). The **armed** steady-state path costs **26 … 38** ARM instructions **per BLOCK** (round 1: 60 per *guest instruction*), and the armed non-target dispatch — the discovery gate that does not enter a block — costs **3 … 6** |
| **G-R2.9 the state-sync contract (§0.5.4)** | on-path state-sync per faultable site: **0** emitted instructions. Bail-arm materialisation: **2 … 4** instructions. Emitted stores per bail (`WARP_BAIL_ST / WARP_BAIL`): **1 … 2** — which **misses §0.5.4's pre-registered 3 … 8 low**, and the miss is predicted here rather than explained afterwards |
| **G-R2.10 the lookup fix** | on the host conflict corpus, `WARP_COMPILE` **== the number of distinct block heads, exactly** — zero recompiles for a head set the table can hold, across a text span ≥ 64 KiB that the round-1 geometry folds into 2 KiB. `WARP_EVICT == 0` on that corpus, and **> 0** under M-CONFLICT |
| **G-R2.11 block length** | `WARP_INSNS / WARP_BLK` = **2.0 … 4.5** guest instructions per block on the harness corpora. §0.5.3 pre-registered `L` = 4 – 8 for a *fully covered* engine; generation one excludes `MOVEQ`, `(An)+`/`-(An)`, `BSR` and every memory-destination ALU shape, so its blocks are shorter and saying so now is what stops the shortfall from being read as a defect later |
| **G-R2.12 cadence** | the §0.6 invariant re-verified with **multi-instruction** blocks feeding real retired counts: 41 retired instructions at cadence 4 poll exactly **10** times, whether they arrive as one block, as several, or interleaved with interpreted passes; and the counter's phase survives every boundary |

### 0.8.5 The harness executes the emitted words, and what that does and does not close

Round 1's honest limit (§3.2, §0.6.6 item 2) was that the host harness *"cannot execute one
instruction of ARM codegen"*, so it proved the decisions and asserted the emitted words
byte-exactly without running them. With real code generation that limit stops being
acceptable: a wrong emitted word is now a wrong *result*, not a wrong constant.

**Round 2 therefore builds an ARM-subset executor into the harness** — a decoder and
interpreter for exactly the instruction forms the emitter produces, executing the **same
bytes** the target executes, against the **same** `regs` structure and the **same** helpers.
Generated code addresses everything through one base register pointing at a small environment
block (the register-file base and the helper pointers), so the emitted words are
pointer-width-independent and a 64-bit host runs the identical stream.

**What this closes:** the semantics of every emitted sequence, on the real bytes, under the
lockstep oracle, at 10⁸ boundaries, before any metal time.

**What it does not close, stated in the same breath:** the executor is a *second
implementation of ARM*, so a shared misreading of the architecture is invisible to it — which
is why each form is additionally asserted against a hand-decoded golden word. And nothing
about the **silicon** is proven by it: I-cache publication, the Q3 barrier sequence, memory
ordering and the behaviour of generated code at volume are proven **on target and only on
target**, exactly as before. The platform caveat rides unchanged: adding barriers is not
evidence of a fix; only a soak on metal is.

### 0.8.6 Round 2's band, derived from the measured inputs

**Coverage.** §0.5.3's pre-registered `c` = **0.19 – 0.62** stands unchanged: round 2 builds
exactly the scope §0.5.3's six factors were derived over, and re-deriving coverage after
seeing round 1's counters would be re-scoping. §0.5.3's own finding rides with it and is
restated so it cannot later look like an excuse: **the 60 % bar sits at the very top of that
band, so generation one is more likely than not to engage below it, M1 was planned as
M1 + M1a from the start, and the perf verdict is taken at the first wave whose coverage is in
band.** A round-2 coverage row below 60 % is **not a verdict in either direction** (§0.5.2).

**Block length.** `L` = **2.0 … 4.5** (G-R2.11), against §0.5.3's 4 – 8 for a full engine.

**Entry cost.** `E` = **26 … 38** ARM instructions per block (G-R2.8), at the **measured**
IPC 0.46 → **2.17 cycles per ARM instruction** on this path.

**The conversion, chained versus hidden, per §30's pricing law.** The removal side is
§0.5.3's, unchanged and not re-derived — `t` = **3.65 % … 25.01 %** of the disarmed baseline,
built per bucket with each bucket's return fraction `ρ` stated (`DOPCFIND` **chained** at
0.80–1.00; `LOOPRES` **mixed** at 0.50–0.80; `FETCHOP`+`FETCHEX` **measured hidden** at
0.00–0.50, never charged in full; `TAILPOLL` at `1 − 4/L`; `TAILSPEC` 1.00). §0.5.3 said the
cost side *"is unquantified and it is subtracted from this band, not added to it"*. Round 1
quantified it, so round 2 subtracts it:

```
   cost per user-mode guest instruction, in cycles
      entry   =  c x (E / L) x 2.17
      gate    =  (1 - c) x 4 x 2.17        the armed non-target discovery test
   against the disarmed 214.2 cycles per guest instruction

   LOW CORNER   c = 0.19, L = 2.0, E = 38   ->  entry 7.84 + gate 7.03 = 14.9 cy = 6.95 %
                removal t = 3.65 %
                net = 1 / (1 - 0.0365 + 0.0695)  =  0.97x

   HIGH CORNER  c = 0.62, L = 4.5, E = 26   ->  entry 7.77 + gate 3.30 = 11.1 cy = 5.18 %
                removal t = 25.01 %
                net = 1 / (1 - 0.2501 + 0.0518) =  1.25x
```

> ### Round 2's band for armed dhry c4: **0.97× – 1.25×** of the clean reference `R`, i.e. **dhry ≈ 7 910 – 10 190** against `R` = 8 174. (derived, on §0.5.3's six coverage factors and five `ρ`, plus `L` and `E` pre-registered above and IPC 0.46 measured)

**Three things must be said beside it or it is a target chosen after the result:**

- **The floor is below the kill threshold, and that is the honest shape of this experiment.**
  0.97× is dhry ≈ 7 910 against a same-session threshold that would sit near 8 231
  (`R × 1.007`) and the standing absolute floor of 8 242. §0.5.3's floor was already *"within
  a couple of hundred dhrystones of the line"*; subtracting the measured cost side puts the
  bottom corner **under** it. **That does not make round 2 a kill — the kill line needs a
  same-session pair and round 2 takes no metal row.** It makes the verdict session's outcome
  genuinely open, which is what a pre-registration is for.
- **§1.5's full-engine band 1.25× – 1.55× is quoted beside every M1 row and is not the bar**
  (§0.5.2). Round 2's high corner touches its bottom.
- **The downside case is a null and it has a precedent on this rig.** Rung 3 engaged at
  91.10 % coverage, made its bucket 23 % cheaper per instruction, hit or beat every premise it
  declared, moved the wall clock −0.05 %, and came out the same day.

### 0.8.7 The other pre-registered targets

| target | pre-registered value |
|---|---|
| **disarm penalty** | **−0.9 % … −2.0 %** of `R`, dhry **8 011 … 8 100** — and the falsifiable half of it is the gate's instruction count, **3** (round 1: 5, measured −2.276 % at the expensive edge of its band). Round 2's requirement is *strictly better than −2.276 %* |
| **recompile ratio** | `WARP_COMPILE / WARP_BLK` **≤ 0.5 %** (round 1: 5.68 %) |
| **recompile attribution** | `(WARP_COMPILE − first-compiles − WARP_DISC − WARP_EVICT − flush losses) / WARP_COMPILE` **≤ 0.05** — i.e. at least 95 % of every recompile is attributable to a **named** cause. Round 1's unattributed share was **95.2 %** |
| **conflict term on the host corpus** | `WARP_EVICT == 0` clean, **> 0** under M-CONFLICT (G-R2.10) |
| **coverage** | `c` = **0.19 – 0.62** (§0.5.3, unchanged), read as `WARP_INSNS / ((INSNS − WARP_ENTER) + WARP_INSNS)`, with the M1a prediction restated: **below the 60 % bar is the more likely outcome and is not a verdict** |
| **`WARP_INSNS` / `WARP_BLK`** | **2.0 … 4.5** |
| **eviction** | the `WARP_FLUSH` path exercised at least once in the harness, against round 1's untested 0 — a 73 %-full arena in a 34 s window is *"not yet", not "never"* |
| **Q5 condition 2** | the compile-cycles-per-block micro-benchmark is a **cycle bracket on target, deliberately not a capture** (§0.5.6) and there is no card in this round. It is **pre-registered for the verdict session** and Q5 stays *"first condition met, second unmeasured"* |
| **Q6** | `WARP_CFETCH` becomes non-zero for the first time — the compiler reads guest instruction words. Its decision rule (`WARP_CFETCH × 42 ≥ 20 %` of measured compile-path cycles) needs the same micro-benchmark and is likewise **deferred to the verdict session** |

### 0.8.8 What round 2 explicitly does not establish

1. **Anything about speed on this machine.** Round 2 takes no metal row. Every number it
   produces is a static count, a host-harness count or a derivation.
2. **That the emitted sequence is correct on the silicon.** The harness now executes the
   emitted words (§0.8.5), which closes their semantics and closes nothing about I-cache
   publication, barriers or ordering. That stays on target and only on target.
3. **Anything about faults or demand paging under load.** G-R2.5 injects faults at faultable
   sites and proves the bail; dhrystone and the harness corpora are *"the workload least
   likely to trip"* the paging path. **M2 is still the first honest test.**
4. **Anything about the supervisor-heavy story.** Generation one is user-mode only.
5. **That the cadence contract is unmoved on metal.** G-R2.12 asserts it by construction and
   in the harness with real retired counts; the behavioural proof remains M3's PSTAT gap
   fingerprint against the card-1/piscsi class at 1 177 – 1 402 µs.
6. **That the ~42 µs compile budget holds.** It is measured per block, on target, in a
   micro-benchmark that this round does not run.
7. **That the A2 core0 reset wedge (§ MATRIX A2) has anything to do with WARP.** It is
   separately owned and it cost the round-1 session no row.


## 0.9 M1 round 2 — built, and its gates scored (2026-08-24)

**The pre-registration in §0.8 was committed at 02:06:31 on 2026-08-24; the first
implementation commit follows it in this repository's log, which is what §3.4 asks of it** —
round 1's pre-registration preceded its code by 32 minutes, the SPEC round's by 26 and the
`f` round's by 29. Every prediction in §0.8 is left byte-untouched above and scored beside
itself, `ifetch-fastpath.md`'s standard.

### 0.9.1 What was built

| piece | where |
|---|---|
| the contract — the gate struct, the 2048×4 hashed table, the block ABI, the ctx | `Z3660_emu/src/uae/z3660_warp.h` |
| the **ARM emitter** — encodings, the one-kind exit, the bail arm | `Z3660_emu/src/uae/z3660_warp.cpp` |
| **generation one's compilers** — MOVE/MOVEA, register ALU, ADDQ/SUBQ, ADDA/SUBA/CMPA, Bcc/BRA, DBcc | same file |
| the **`jnf_MEM` replacement** — `z3660_warp_rd` / `z3660_warp_wr`, the verdict-table fast path plus every fence | same file |
| the **host ARM-subset executor** — the harness runs the emitted words | same file, `HOST_TEST_HARNESS` only |
| the inlined run-loop fast path, the pass-consuming arm, the cadence call-site early-out | `z3660_warp.h` + `newcpu.cpp` |
| counter **id 60 `WARP_EVICT`**, `Z3660_PROF_C_COUNT` 60 → 61 | `z3660_prof.h`, `z3660_prof.cpp` |
| the opcode sweep, the fault corpus, the conflict corpus, twelve mutation arms | `Z3660_emu/test/host/harness_main.cpp` |

### 0.9.2 The gates

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| **G-R2.1** harness | green all three; **+120 … +400** in default, the **same** in lean, prof a further **+10 … +60** | green, `0 failed`: **3 280 / 3 276 / 3 507** against 3 250 / 3 246 / 3 473 — **+30 / +30 / +34**; default and lean gained the **same** number, prof a further **+4** | green; **count MISSED, low** (as round 1's did) |
| **G-R2.2** disarmed object equivalence | **≤ 25** functions changed; `m68k_run_mmu040` **+40 … +160 bytes** | **21 changed of 25 135 common**, +18 new symbols and −1 (`z3660_warp_dispatch`, replaced by the inlined step plus `z3660_warp_discover`/`z3660_warp_run`); `m68k_run_mmu040` `0x914` → `0x9f4` = **2 324 → 2 548 B, +224** | changed-count **HIT**; growth **MISSED, high** |
| **G-R2.3** lockstep identity | `WARP_DIVERGE == 0` over `≥ 10⁸`, executing the emitted words, arms byte-identical | **`WARP_DIVERGE = 0` over 100 000 000 guest instructions**, 12 500 000 block executions, **100 % of them retired inside blocks**, every 200 000-instruction chunk ending with the two arms in a byte-identical machine state; the armed arm executed the emitted ARM words through the harness's decoder | **HIT** |
| **G-R2.4** counter identities | all exact, with `WARP_ENTER > 0` | all exact: `DOPC_HIT+DOPC_MISS == (INSNS − WARP_ENTER) + FAULTS` in both arms with **`WARP_ENTER` = 4 000**; the reconstruction `(INSNS − WARP_ENTER) + WARP_INSNS` closes against the disarmed `INSNS` exactly; `FAULTS`, `READ`, `WRITE`, `MISALIGN_R/W`, `DOPC_INVAL`, `sum(IV_*) − IV_WRAP` and all three `BLK_*` unchanged; `INSNS` **falls** (4 000 vs 32 000) | **HIT** |
| **G-R2.5** fault injection | bail, never a fault inside a block; state byte-exact | both injections (an **unmapped page** and a **misaligned** access) retired exactly the four instructions before the faultable site and bailed there; `WARP_BAIL` 1, `WARP_BAIL_ST` 1, the decline raised by the **resolver** and not a stub; sixteen registers, pc, `instruction_pc`, CCR, X, `mmu_restart`, `mmufixup[0..1]` and the data page **byte-identical** to the interpreter after the same four instructions | **HIT** |
| **G-R2.6** mutation arms | each caught and shown | **M-5**, **M-7**, **M-ICACHE**, **M-ARM**, **M-REPUB** still bite; **M-3** caught (bail pc 0040 0008 vs 0040 0000); **M-4** caught (violations >0 clean, 0 mutated); **M-8**/`BAILX` caught; **M-CONFLICT** caught (16 compiles → **1 600**, 0 evictions → **1 596**); **M-XFER** caught (1 compile → 4, 80 block executions → 200); **M-FENCE** caught (state diverges); and **one wrong-flag arm per covered family** — MOVE, ADD, SUB, CMP, Bcc, DBcc — **all six caught**. **M-VERDICT is REACHED but NOT CAUGHT** and it is recorded as an owed arm, not a passing one | **HIT with one named gap** |
| **G-R2.7** images | three rebuilt, CRCs recorded, archive fires; `q2probe` not rebuilt | `BOOT.BIN` **`A8CEC0C5`** (was `9C7A2AD3`), `BOOT-lean.BIN` **`DBF9A5AF`** (was `E80D04EE`), `BOOT-prof.BIN` **`21464241`** (was `423CB391`); 12 795 284 / 12 795 268 / 12 828 132 B; all three archived CRC-stamped; `BOOT-q2probe.BIN` untouched at `FCC0CB3F` | **HIT** |
| **G-R2.8** entry cost | disarmed **3**; armed non-target **3 … 6**; armed steady state **26 … 38 per BLOCK** | disarmed **5**; armed non-target **11**; armed per block **≈ 108** | **all three MISSED, high** — §0.9.3 |
| **G-R2.9** state-sync | on-path **0**; bail-arm **2 … 4**; stores per bail **1 … 2** | on-path **0** ✔; bail-arm **8** (an exit is 8 or 10 words); `WARP_BAIL_ST / WARP_BAIL` = **1** | on-path **HIT**; bail-arm **MISSED, high**; stores per bail **HIT** (and it **MISSES §0.5.4's 3 … 8 LOW**, exactly as §0.8.4 predicted it would) |
| **G-R2.10** the lookup fix | `WARP_COMPILE ==` distinct heads exactly; `WARP_EVICT == 0`; **> 0** under M-CONFLICT | **`WARP_COMPILE` = 16 == the 16 distinct heads, over 6 400 block executions**; `WARP_EVICT` = **0**; ratio **0.250 %**, inside the pre-registered ≤ 0.5 %; under M-CONFLICT **1 600 compiles and 1 596 evictions** | **HIT** |
| **G-R2.11** block length | `WARP_INSNS / WARP_BLK` = **2.0 … 4.5** | **8.00 on the harness corpora** — which are written entirely inside generation one's scope and are therefore not a mix. **NOT MEASURED**: the prediction is about a representative instruction stream and only the verdict session has one | **UNMEASURED, prediction stands** |
| **G-R2.12** cadence | 41 → 10 polls; the same run polls the same however it arrives | 41 retired instructions at cadence 4 poll **10** times clean and **0** mutated (M-7); and through the real run loop, **800 guest instructions poll 200 times interpreted and 200 times in eight-instruction blocks** | **HIT** |

**Free in the same run, and worth more than some of the gates.** The compiler was offered
**all 65 536 opcodes** one at a time and **accepts 7 616 of them (11.62 %)** — that is
generation one's scope as BUILT rather than as described. Every accepted opcode was then run
both ways from six seeded states and compared across the whole §3.1 set: **45 696
differential pairs, 0 divergences.** Code bytes per compiled guest instruction: **50.0**.

### 0.9.3 The entry cost, read off the lean disassembly — and the prediction it missed

**Pre-registered 26 – 38 ARM instructions per BLOCK. Measured ≈ 108. MISSED, high.** The
prediction is left where it was written; here is what the disassembly shows, counted along
the steady-state path (block cached, way 0 hit, `build-lean/Z3660_emu.elf`, `DBF9A5AF`):

| segment | instructions |
|---|---|
| the gate — address materialisation, flag load, test, branch, and gcc's hoisted pc load | 5 |
| the control-transfer-target test — last-pc load, store, two subs, compare, branch | 6 |
| supervisor, odd-pc and trace gates | 8 |
| the lookup — hashed index, set base, generation compare, way-0 tag compare | 18 |
| `z3660_warp_run` before the block — prologue, the split attestation, the ctx set-up, the entry load | 33 |
| the block's own prologue — `push` plus the four bases | 6 |
| the block's exit — pc, ipc, restart, count, `pop` | 8 |
| `z3660_warp_run` after the block — the flag commit, pc, ipc, `mmu_restart`, epilogue | 17 |
| the run loop's pass-consuming arm — cycles, the cadence early-out | 7 |
| **total, armed, per BLOCK** | **≈ 108** |
| **armed, per non-target dispatch** (pre-registered 3 … 6) | **11** |
| **disarmed** (pre-registered 3; round 1 measured 5) | **5** |

**Four things about that table are findings rather than numbers.**

- **The disarmed gate did not move: 5, exactly as round 1.** Two of the five are the
  address materialisation, which is unavoidable on a target with no literal pool
  (`-DARMV6T2`, §0.3(c)); one is the argument load, which gcc schedules above the branch
  whether or not the arm is taken. Packing the flag and the previous pc into ONE struct
  removed a second materialisation from the ARMED path and nothing from the disarmed one.
  **The pre-registered −0.9 % … −2.0 % disarm penalty therefore has no mechanism behind it
  and should be expected to reproduce round 1's −2.276 %.** Said here, before the row.
- **Two of the reductions §0.7.3 asked for landed and are visible.** The lazy `warp_inited`
  check is gone (it moved to arm time, and it now RETRIES the arena rather than latching a
  first failure — round 1's known gap, closed). The cadence feed's call-and-prologue-to-do-
  nothing is gone: the `n > 1` test is at the call site and costs one compare.
- **Two more were measured into existence rather than argued.** Passing `regs.s ? 1 : 0` as
  an argument made gcc evaluate it above the inlined body, so the supervisor byte, its
  compare and a predicated store were paid on EVERY armed dispatch: reading it inside, after
  the test that leaves, took the armed non-target path from **18 to 11**. And writing the
  flag commit through the architectural `SET_*FLG` macros made gcc vectorise a four-bit
  update into a NEON load/and/or/store pair; on the machdep arm whose `flag_struct` IS the
  ARM CPSR layout the commit is a masked store, and taking that path took `z3660_warp_run`
  from **65 instructions to 50**.
- **The exit grew, and the differential is why.** §0.8.4 priced the bail arm at 2 – 4
  instructions; it is 8, because `regs.instruction_pc` and `mmu_restart` are in §0.5.5's
  comparison set and two generation-one shapes move them (§0.8.3's write-back MOVE and
  DBcc's `m68k_setpci` exits). The opcode sweep found it on its first run. **A cheaper
  contract was available and was refused: excluding memory-destination MOVE as a write-back
  family would have removed the obligation and halved the family's coverage.** The scope is
  frozen; the words were paid.

**The metal expectation, re-derived from the measurement.** §0.8.6's band was derived from
E = 26 … 38 and a 4-instruction gate and is left as written; at E ≈ 108 and a gate of 11 the
same arithmetic, at the same measured IPC 0.46 (2.17 cycles per ARM instruction), gives:

```
   LOW  c = 0.19, L = 2.0  ->  entry 0.19 x (108/2.0) x 2.17 = 22.3 cy
                              gate  0.81 x 11 x 2.17         = 19.3 cy   -> 19.4 % of 214.2
                              removal 3.65 %   ->  1/(1 - 0.0365 + 0.194) = 0.86x
   HIGH c = 0.62, L = 4.5  ->  entry 0.62 x (108/4.5) x 2.17 = 32.3 cy
                              gate  0.38 x 11 x 2.17         =  9.1 cy   -> 19.3 %
                              removal 25.01 %  ->  1/(1 - 0.2501 + 0.193) = 1.06x
```

| row | §0.8.6 as pre-registered | re-derived from the measured counts |
|---|---|---|
| armed dhry c4 against `R` = 8 174 | 0.97× – 1.25×, dhry 7 910 – 10 190 | **0.86× – 1.06×, dhry ≈ 7 030 – 8 660** |
| disarm penalty | −0.9 % … −2.0 %, dhry 8 011 – 8 100 | **no mechanism moved it; expect round 1's −2.276 % class** |

**The reading rules are §0.6.5's, unchanged.** This is a cost and a static count, not a
verdict; **round 2 takes no metal row at all**, so the M1 kill line cannot fire on it, and
`kill-R` remains the CLEAN INTERPRETER LINEAGE row rather than a disarmed WARP image (the
ruling of 2026-08-24). What the re-derivation says is that **the entry cost is still the
dominant term and the amortisation is not yet paying for it** — at L = 8 the same arithmetic
gives 1.14×, so the lever with the most left in it is block length, which generation one's
own exclusions (`MOVEQ`, `(An)+`/`-(An)`, `BSR`, every memory-destination ALU shape) are what
bound. That is M1a's argument, and it is now an arithmetic one.

### 0.9.4 The lookup fix, and what the counters say about it

§0.8.2's diagnosis is proven twice over, once without executing anything:

- **Statically:** sixteen block heads 2 KiB apart occupy **one set** under round 1's
  `(pc >> 1) & 1023` and **sixteen** under `((pc>>1) ^ (pc>>12) ^ (pc>>23)) & 2047`. The
  index addressed a 2 KiB window of guest text; every head in a wider span collided.
- **Dynamically:** the same sixteen heads, chained and executed 6 400 times, compile
  **exactly sixteen times** with `WARP_EVICT` = 0. Restore round 1's geometry with
  M-CONFLICT and the same run compiles **1 600 times with 1 596 evictions** — one compile
  per execution, which is the 3 066 944-compile pathology reproduced in a fixture and then
  *attributed*, which round 1 could not do at all.

**The recompile budget now closes**: `WARP_COMPILE = first-compiles + WARP_DISC + WARP_EVICT
+ flush losses`, with id 60 supplying the term that was missing. Round 1's unattributed share
was **95.2 %**; on the fixed geometry it is **0 %**, and on metal the pre-registered target is
≤ 5 % unattributed.

**Two more round-1 gaps closed beside it.** A **declined head is now cached** as round 1's
two-word null block, so a PC the engine refuses costs one lookup instead of a fresh discovery
on every arrival. And the **`WARP_FLUSH` path is exercised**: round 1 filled 73 % of the arena
in a 34 s window and never wrapped, which was "not yet" rather than "never"; the harness
lowers the limit and reaches it deliberately, with both invariants intact.

### 0.9.5 Four things worth recording that were not gates

- **The harness now executes the emitted words** (§0.8.5), and that is what caught every real
  defect this round: the write-back MOVE's restart bookkeeping, a refused opcode leaking its
  post-state into the previous instruction's exit, and a bail arm handing the interpreter the
  wrong per-site pc. None of them is visible in a source diff and none would have been caught
  by asserting the emitted bytes without running them. The honest limit narrows but does not
  vanish: the executor is a **second implementation of ARM**, so a shared misreading is
  invisible to it, and **nothing about the silicon** — I-cache publication, the Q3 barrier
  sequence, ordering, behaviour at volume — is proven by it.
- **Q8 is honoured by construction, and the construction is worth naming.** Rung 0's
  recogniser claims exactly three head shapes — `MOVE (An)+`, `MOVE (An)+,(An)+` and `CPUSHL`
  — and all three use predecrement/postincrement or are privileged, so all three are outside
  generation one's frozen scope. WARP refuses them as heads and a block ends in front of one
  rather than swallowing it. `BLK_HIT` / `BLK_INSNS` / `BLK_BYTES` are identical between the
  arms, measured.
- **The compiler's instruction fetch is NOT the interpreter's.** Written through
  `uae_mmu040_get_iword()` it put eight compile-path fetches inside the interpreter's own
  ifetch bracket, and the harness read `FETCH` = 8 armed against 0 disarmed — which would
  have made §0.5.6's declared fall of `FETCH`/`IFETCH_CALLS` arithmetically impossible, the
  exact law that section exists to enforce. It reads `mmu_get_iword()` and charges
  `WARP_CFETCH`, which is now non-zero for the first time. **Q6's counter is live; its
  decision rule needs the compile-cycle micro-benchmark and is deferred to the verdict
  session with Q5's second condition.**
- **The `jnf_MEM` replacement is a shared helper, not an open-coded sequence**, and §0.8.3
  registered the deviation before it was built. What it bought is equivalence by
  construction — the helper calls the same `always_inline` resolver the interpreter's
  accessors call — and what it costs is a `blx` plus an AAPCS boundary on a path where
  nothing is dirty. **The corpus also proved it SERVES**: without a data page inside the
  harness's direct-bank window every generated access declined, every MOVE bailed, and the
  whole accessor half of the engine would have gone untested with every test still passing.
  That is the failure a state-equivalence corpus is blindest to, and it was caught by an
  engagement counter rather than by luck.

### 0.9.6 What round 2 leaves owed

1. **M-VERDICT is reached but not caught.** The TTR_NO_WRITE asymmetry needs a read-only TTR
   region, which the WARP corpus does not have; §0.5.5's G4 asymmetry cases do. Wiring the
   accessor corpus to the engine is a round-3 item and it is an **owed arm**, not a passing
   one.
2. **`WARP_INSNS / WARP_BLK` is unmeasured** against a representative stream. §0.8.4's
   2.0 – 4.5 stands as written and the verdict session takes it.
3. **The open-coded expansion of the memory fast path** is priced by this round's objdump and
   not built. So is the inlining of `z3660_warp_run`'s prologue, and the 33 instructions it
   spends before the block runs are the largest single row in §0.9.3's table.
4. **Q5's second condition and Q6's decision rule** both need the compile-cycles-per-block
   micro-benchmark, which is a cycle bracket on target (§0.5.6) and needs a card.
5. **Everything §0.8.8 said round 2 would not establish, it did not establish.**


## 0.10 M1 round 3 — the BUG-REPORT round, pre-registered before the first line of round-3 code (2026-08-24)

**§0.5.2's last decision row routed round 2's metal session here, by name:** *"coverage < 60 %
**and** a named implementation defect → a bug report. Fix, re-run, and the defect is named in
the record."* The armed round-2 engine did not survive on silicon — four arms, two images,
four crashes, zero armed rows (`2026-08-24-warp-r2-verdict/MATRIX.md`). **This section is the
bug report, and it is written before any round-3 code exists.** §3.4's two rules govern it as
they governed §0.5, §0.6 and §0.8: the pre-registration commit precedes the implementation
commits, provably, by commit timestamp in this repository; and a prediction that turns out
wrong is marked `MISSED` beside the measurement with the prediction left **byte-untouched**.

**Round 3 takes no metal row either.** Every number below is a static count, a host-harness
count, an arithmetic derivation, or a protocol for a session that has not happened. The kill
line cannot fire on it and `kill-R` remains the clean interpreter lineage row (the ruling of
2026-08-24).

### 0.10.1 THE ROOT CAUSE — the publish sequence does not cover the block it publishes

**Named, with its arithmetic, and it is a defect of RANGE and not of sequence.** §0.8.8 item 2
and Q3's standing caveat both said adding barriers is not evidence of a fix; the R2 session
confirmed the Q3 sequence is *present* at `z3660_warp.cpp:1007–1018` and correctly ordered.
It is. **The bytes it is pointed at are wrong.**

`warp_publish(p, bytes)` calls the firmware's own two CP15 line loops, whose bodies are in
`jit/compemu_midfunc_arm.cpp:566` and `:578` and whose shape is identical:

```
   length = (length + 31) & ~31;
   while (length) { <CP15 op on `addr`>; addr += 32; length -= 32; }
```

**The length is rounded up; the ADDRESS IS NEVER ROUNDED DOWN.** With `k = ceil(L/32)`
iterations the loop touches `A, A+32, … , A+32(k-1)`, i.e. the `k` consecutive lines starting
at `A & ~31`. Its byte coverage is therefore `[A & ~31, (A & ~31) + 32k)` — and the block
occupies `[A, A + L)`. So:

```
   THE BLOCK'S LAST CACHE LINE IS LEFT UNMAINTAINED  iff  (A & 31) + L  >  32 x ceil(L / 32)
```

**The D-cache half is the load-bearing half, and this is why the failure is a wrong control
transfer rather than a wrong result.** `arm_flush_cache` is `DCCIMVAC` — clean-and-invalidate
to PoC. A line it does not touch stays **dirty in L1D and never reaches PoC**. The Cortex-A9
has no I/D coherency: the instruction-side linefill for that address reads **PoC**, which
still holds what was there before the compiler wrote the block. So the CPU executes the
block correctly up to its last line and then executes **stale memory**.

**Two stale contents, two manifestations, and the four crashes are one mechanism:**

| stale content | when | what happens | R2 occurrence |
|---|---|---|---|
| **zeros** (virgin arena — 32 MB, `cache_alloc`ed and zeroed, never yet written) | before the bump pointer has wrapped or been reset | `0x00000000` decodes as `andeq r0,r0,r0`, a NOP. Execution walks **past the block's own `pop {r4-r8,pc}`** and forward through the arena until it leaves the mapped heap → core1 prefetch abort / undefined instruction, with no handler left standing to print → **the board restarts, silently** | **1, 2, 3** |
| **a previous block's bytes** | after `warp_flush_cache()` or `z3660_warp_arm()` has reset `warp_cp` over already-written arena | execution lands mid-block in straight-line code. **The stack is balanced** — entry went through a real prologue — so it reaches some later `pop {r4-r8,pc}` and RETURNS to `z3660_warp_run` carrying a **stray `ctx.pc`** (written by whatever `str r1,[r7,#0]` it ran through) and a **stray retired count** in r0. `z3660_warp_run` then does `regs.pc = z3660_warp_ctx.pc` → **the guest's control flow has been handed a wrong address** | **4** |

Occurrence 4's capture is the mechanism's own signature and needs no interpretation:
`[XT07] vec=2 ipc=C100F348 pc=C100F348 fa=C100F348 ssw=0542 sr=0000` — `fa == pc` is an
instruction-fetch fault **at** the PC, in **user mode**, which is the only mode generation one
runs in; and `a7=8080002A`, an odd stack pointer, is what a guest gets after a `MOVEA`-shaped
exit ran with a stray immediate. The MATRIX's reading — *"a wrong control transfer out of
generated code; guest address space → occurrence 4, host ARM address space → occurrences
1–3"* — was correct in both halves and this is the mechanism under it.

**THE DISCRIMINATOR, AND IT IS EXACT: round 1 could not hit this, and round 2 hits it 43.75 %
of the time.** The R2 session's strongest observation was that round 1's null blocks ran a
full armed five-row set on the same silicon. That is not luck and it is not a difference in
"real code":

| round | block length `L` | bump step | `A & 31` reachable | `(A&31) + L > 32·ceil(L/32)` |
|---|---|---|---|---|
| **1** | **always 8** (`Z3660_WARP_NULL_BYTES`, `mov r0,#0` + `bx lr`) | 8 | {0, 8, 16, 24} | `≤ 24 + 8 = 32` — **never**, for every reachable offset |
| **2** | `c.n × 4`, 8 … 4096, ~50 B per compiled guest instruction | `L` | every multiple of 4 in [0,32) | **7 of every 16** publishes, by the count below |

With `a = A & 31` and `r = L mod 32` both walking the eight 4-aligned residues: the defect
fires for `r = 0, a > 0` (7 of 64 pairs) and for `r > 0, a > 32 − r` (21 of 64 pairs) —
**28/64 = 43.75 % of all published blocks leave their last line unmaintained.** Round 1 was
structurally immune; round 2 is a coin flip per block. An armed guest reaches its first
user-mode control-transfer target within milliseconds, which is why occurrence 2 died in
under a second, and why occurrence 1 survived eighteen seconds of an *idle shell* — a window
with almost no user-mode block in it — and died one second into dhrystone.

**And the mid-arm PROFD race is the positive control the R2 session bought without knowing
what it was controlling for.** A window that was 100.00 % supervisor with all fourteen WARP
counters at zero compiled and published **nothing**, so nothing was stale, and the guest
survived it. The crash tracks user-mode execution because publication is what user-mode
execution reaches.

> **This is a defect the campaign has already paid for once, in this repository, and the
> receipt is quoted in the source that carries it.** `warp_compile()`'s own comment names
> `8be8949` — *"the first `pushall_call_handler()` fetched stale I-cache lines and the board
> rebooted with an ARM Undefined Instruction, caused by a compiler-flag change."* The
> ordering lesson from that receipt was learned and applied (publish before the block is
> reachable). The **range** was not, because the range was inherited.

### 0.10.2 THE FIDELITY GAP — what the harness executes versus what the silicon executes

**§0.8.5 claimed the harness "runs the words the compiler emits", and §0.9.2 scored G-R2.3
green over 10⁸ boundaries. Both statements are true and neither of them touched the defect.**
The gap is not that the harness's ARM model is a second implementation — §0.8.5 declared that
and §0.9.5 restated it. The gap is one line of the harness:

```
   static void warp_publish(void *p, unsigned bytes)
   {
   #ifndef HOST_TEST_HARNESS
      ... the whole Q3 sequence ...
   #else
      (void)p; (void)bytes;                       <-- THE GAP, IN ONE LINE
   #endif
      WARP_STAT(Z3660_WARP_STAT_PUBLISH);
   }
   ```

**So the harness proves the words and never the PUBLICATION of the words.** Stated as the
four questions the round-2 dispatch asked, answered honestly:

| question | round-2 answer |
|---|---|
| does generated ARM code actually run in the host harness? | **No — it is INTERPRETED**, by `warp_host_exec()`, a decoder for exactly the forms the emitter produces. It reads the emitted words as **DATA**, through ordinary loads. |
| is the real entry/exit trampoline exercised? | Partly. `z3660_warp_run()` is the real function; the block's `push`/`pop` are decoded for **shape only** and the harness's stack is a token array. |
| is the real `warp_run` preamble exercised? | Yes for the C half (ctx set-up, flag pack, commit); no for the ARM call itself. |
| is the real address materialisation of exit stubs exercised? | Yes as **words** — `movw`/`movt` of a guest pc are decoded and executed. **The emitted stream contains no host address at all** (§0.8.5's design constraint), so there is nothing there to differ between address spaces. |

**Every candidate the dispatch listed is therefore EXCLUDED by construction, and saying so is
part of the report:** exit-stub address materialisation cannot differ between harness and
target because no host address is ever emitted; a PC-relative branch cannot reach in one and
not the other because every branch this compiler emits is a forward skip over a known-length
sequence with the offset fixed at emit time, and the harness decodes every one of them and
checks it lands on an instruction boundary inside the block; the block-exit guest-PC
computation for Bcc/DBcc is pure arithmetic on constants, executed identically in both; the
bail path's PC materialisation is the same two `movw`/`movt` words in both. **And the odd
`a7` is a consequence, not a cause** — the audit is in §0.10.4 G-R3.6 and every generation-one
family that can write `A7` (`MOVEA`, `MOVE` to `An`, `ADDQ`/`SUBQ` to `An`, `ADDA`/`SUBA`)
writes it as the 68040 does, full width and without an alignment rule, so an odd `A7` is
architecturally legal and is evidence of a wrong VALUE arriving, not of a wrong emitter.

> **The one-sentence statement of the gap, for the record: the host harness reads the emitted
> words through the DATA side and the silicon executes them through the INSTRUCTION side, and
> the only thing that stands between those two sides is the publish sequence — which the
> harness compiles out. A harness that cannot see the D-side/I-side seam cannot see any defect
> that lives in it, and this one did.**

**What now covers it** is §0.10.3 item 2 and §0.10.5: an on-target self-test whose oracle is
*the same block, executed on the silicon and interpreted from its own words*, which is the
D-side and the I-side compared against each other on the machine that has both.

### 0.10.3 What round 3 builds

**1. The fix, in two independent parts, because one of them must survive the other being
changed.**

| part | change | why both |
|---|---|---|
| **A — correctness** | `warp_publish()` maintains `[p & ~31, (p + bytes + 31) & ~31)` — the range rounded OUT to whole cache lines at both ends | This is correct for **any** allocation policy. It is the part a future reader must not be able to un-fix by changing where blocks land. The two CP15 loops are shared with the classic JIT and are **not** modified: the range is corrected at WARP's call site, so no behaviour outside the engine moves. |
| **B — hardening** | the arena base is aligned up to 32, its usable size rounded down to 32, and the bump pointer advances by `(bytes + 31) & ~31` so **every block starts on a cache line and no two blocks share one** | With `A & 31 == 0` the defect cannot fire even under part A's absence, the publish range is exact rather than rounded, and a future concurrent publisher can never touch a neighbour's line. Costs ≤ 28 B per block out of 32 MB. |

**2. The proof the round-2 harness could not carry — the on-target armed self-test.** Designed
in §0.10.5, with its serial protocol. It ships in **every** variant, inert until a console verb
runs it, and it is the gate on every future arm.

**3. The regression arms**, in the harness **and** in the on-target self-test, per the round's
brief. A publish-coverage invariant (`Z3660_WARP_BAD_PUBRANGE`), a mutation arm
(`M-PUBRANGE`) that restores round 2's shipped range, a unit assertion of the coverage
arithmetic against hand-computed cases **including round 1's geometry** — which is the arm
that keeps the discriminator honest — and an on-target alignment sweep that compiles and runs
the same block at every 4-byte offset within a line.

**4. The disarm penalty.** §0.10.6.

**5. The owed items that ride.** M-VERDICT's `TTR_NO_WRITE` asymmetry arm (§0.9.6 item 1) and
the R6 compile-cycles-per-block micro-benchmark (§0.9.6 item 4, §0.8.7's Q5 condition 2 and
Q6's decision rule), the latter folded into the self-test verb so the next metal session can
run it without a new build.

### 0.10.4 The round-3 gates, with their predictions

Every one of these is a prediction made before the measurement exists.

| gate | prediction |
|---|---|
| **G-R3.1 harness** | green (`0 failed`) in all three variants. New `CHECK`s: **+25 … +90** in the default variant, the **same** number in lean, prof gaining a further **0 … +10**. Baseline is 3 280 / 3 276 / 3 507 |
| **G-R3.2 the defect reproduces, then goes green** | with the publish range as round 2 shipped it, the new invariant `Z3660_WARP_BAD_PUBRANGE` reads **> 0** on the existing WARP corpora; with the fix it reads **0**. The mutation `M-PUBRANGE` restores round 2's range and is **caught**. This is the failing-test-then-green the round is scored on |
| **G-R3.3 the discriminator, asserted** | the coverage predicate, evaluated over round 1's geometry (`L = 8`, `A ≡ 0 mod 8`), reports **covered for all four reachable offsets** — and over round 2's (`L` a multiple of 4, `A` a multiple of 4) reports **exactly 28 of the 64 (a, r) pairs uncovered = 43.75 %** |
| **G-R3.4 disarmed object equivalence** | per-function normalised-disassembly diff of the **lean** `Z3660_emu.elf` against `a392fbe`: **≤ 20 functions changed**, every changed function on the WARP path or `m68k_run_mmu040`. `m68k_run_mmu040` **shrinks** from 2 548 B to **2 330 … 2 400 B** (§0.10.6) |
| **G-R3.5 lockstep identity, unmoved** | `WARP_DIVERGE == 0` over `WARP_LS_INSNS ≥ 10⁸` again, with byte-identical arms. Round 3 changes no emitter, so a divergence here would mean the fix moved something it had no business moving |
| **G-R3.6 the odd-`A7` audit** | a static enumeration of every generation-one shape that can write `A7`, each shown to write it exactly as `cpuemu_31` does. Predicted: **no shape can produce an `A7` the interpreter would not have produced**, so occurrence 4's `a7=8080002A` is downstream of the wrong control transfer and not a second defect |
| **G-R3.7 the on-target self-test exists and is inert** | it is present in all three shipping images, reachable only through the `WARPTEST` console verb, touches **no guest memory at all** (neither instruction words nor operands), leaves `regs`, the block table, the generation and the bump pointer exactly as it found them, and adds **0 bytes** to `m68k_run_mmu040` |
| **G-R3.8 M-VERDICT, the owed arm** | a write-protected data TTR window is wired into the WARP corpus so a compiled store into it must **bail**; under `M-VERDICT` it is **served** and the arms diverge. Predicted: **caught**, closing §0.9.6 item 1 |
| **G-R3.9 R6 is runnable** | the compile-cycle bracket exists behind the same verb, uses the ARM **global timer** (`XTime_GetTime`, always running in every variant, CPU:global fixed 2:1 per §z3660_prof.h note 5) rather than the PMU — so no counter has to be armed and the lean image measures the same way the profiling one does. It prints cycles per block, cycles per compiled guest instruction and `WARP_CFETCH` per block. **Not run this round: there is no card in it** |
| **G-R3.10 images** | the three shipping images rebuilt, CRCs recorded, the archive firing for each. `BOOT-q2probe.BIN` is **not** rebuilt and stays at `FCC0CB3F` |

### 0.10.5 The on-target self-test — design, and the protocol the metal session runs

**THE RULE IT EXISTS TO ENFORCE, stated first because it is the deliverable:**

> **No metal session arms the WARP engine on a live guest until `WARPTEST` reports PASS on the
> image that is about to be armed.**

**Where it lives, and the decision stated with its reason.** It **ships in every variant —
default, lean and prof — inert until invoked.** It is not excluded from the shipping images
and it is not a fourth build. The reason is the one this whole round is about: *the wall-clock
arm of every verdict is a LEAN boot* (§0.5.1, §0.4.3's standing rule), so a self-test that
existed only in a debug variant would attest a **different binary** from the one armed — which
is exactly the class of error §0.10.2 is a report about. It is the same argument the arm
attestation is shipped under, and it is paid the same way: cold `.text` that never executes,
with G-R3.7 measuring that `m68k_run_mmu040` does not grow by a byte.

**The oracle, and it is the whole design.** Each block is run **twice, from the same starting
architectural state**:

1. **executed on the silicon** — a real call through the real function pointer, so the words
   arrive through the **instruction side**: I-cache, the publish sequence, the barriers;
2. **interpreted from its own words** — by `warp_host_exec()`, the decoder §0.8.5 built,
   promoted out of `HOST_TEST_HARNESS` so it compiles on the target too. It reads the same
   bytes with ordinary loads, i.e. through the **data side**.

Then the two results are compared: the sixteen guest registers, the block context's `pc`,
`nzcv`, `xflg`, `ipc` and `restart`, and the retired count.

> **A publish defect makes exactly those two disagree, and nothing else in the engine can.**
> That is the sentence the round-2 harness had no way to write, because it had only arm 2.

**What it does NOT do, declared here rather than discovered later.** It does not re-prove
semantics — the harness did that at 10⁸ boundaries against the interpreter, on the real
emitted words, and a second oracle on target would only re-run a second implementation of ARM.
It does not compile from guest memory: the compiler's instruction fetch is fed from a
core1-resident opcode array, so **no guest instruction page is read**. It does not execute a
memory-touching family: `(An)` and `d16(An)` operands would need a guest data page and the
self-test must be safe to run **on a live guest at any time**. Generation one's memory helper
therefore stays a harness-proven path this round, and that is **re-owed explicitly** in
§0.10.7 rather than waved at.

**The families, in the order they run, and the alignment sweep is first on purpose.**

| # | family | what it establishes |
|---|---|---|
| **F1** | **publish / coherency alignment sweep** — one register-ALU block compiled and run at every 4-byte offset in a 32-byte line, at several lengths | **the defect's own reproducer on silicon.** Under round 2's range, the cases satisfying `(A&31)+L > 32·ceil(L/32)` fail or hang; under the fix all pass |
| **F2** | `MOVE`/`MOVEA`, register operands only | the emitter's flag and size discipline, executed |
| **F3** | register ALU — `ADD SUB AND OR EOR CMP` | ditto, with X and the carry inversion |
| **F4** | `ADDQ`/`SUBQ`, both `Dn` and `An` destinations | the quick-immediate reading of Q7 |
| **F5** | `ADDA`/`SUBA`/`CMPA` | the `An`-destination shapes |
| **F6** | `Bcc`/`BRA` — taken and not-taken | a **real control transfer out of generated code**, which is the defect class |
| **F7** | `DBcc` — expired, taken and always-true | three exits from one block |
| **F8** | a **multi-instruction block** ending at a terminator | the exit that is not the first one |
| **F9** | a **declined head** — the two-word null stub | the shortest publish there is, and round 1's exact geometry |

**THE SERIAL PROTOCOL, and it is designed to survive the crash class it hunts.** Every family
prints a `>>>` sentinel **before** it runs and a result line **after**. An occurrence-1-style
silent death therefore **names the family by the sentinel that has no result line under it** —
which is the one diagnostic the R2 session could not get, because the machine died with
nothing on the wire.

```
[WTST] WARP on-target self-test -- 9 families, arena 0x........ +........ B, cp +........
[WTST] >>> F1 publish/coherency alignment sweep
[WTST]     F1 PASS  64/64 cases   (offsets 0..28, lengths 8..264)
[WTST] >>> F2 MOVE/MOVEA register operands
[WTST]     F2 PASS  12/12 cases
...
[WTST] >>> F9 declined head (null stub)
[WTST]     F9 PASS   1/1 cases
[WTST] R6 compile bracket: <n> blocks, <cyc> cyc/block, <cyc> cyc/compiled-insn, CFETCH <n>/block
[WTST] VERDICT: PASS 9/9 families, 0 mismatches -- the engine may be armed on this image.
```

A failure prints the family, the case, and the first field that differs, then **stops** —
because a self-test that keeps going after a coherency failure is walking through a machine
whose instruction stream it no longer trusts:

```
[WTST]     F1 FAIL  case off=12 len=52: exec pc=........ vs decode pc=........
[WTST] VERDICT: FAIL at F1 -- DO NOT ARM. docs/WARP-ENGINE.md 0.10
```

**How it is invoked, and why it cannot arm anything.** Console verb `WARPTEST` writes the magic
`0x57545354` (ASCII `WTST`) into the same `SHARED.warp_on` mailbox. `z3660_warp_resolve()` is
**unchanged** and returns 0 for it — only `Z3660_WARP_ARM` arms, which is Q9's rule and the
withdrawn IVSUP switch's precedent. Core1 notices the word at its next knob resync,
**consumes it by writing the OFF sentinel back** — `z3660_prof.cpp:1275`'s pattern verbatim,
*"consume before acting: a dump takes seconds"* — runs the test, prints the report, and leaves
the engine disarmed. No SHARED field is added, no config key exists, and a self-test request
that arrives twice runs twice.

**The metal session's steps, in order:**

1. Boot the image that is about to be armed. Do not arm anything.
2. `WARPTEST` on the serial console. Capture the whole `[WTST]` block.
3. **If the verdict is not `PASS 9/9`, or if a `>>>` sentinel has no result line under it, the
   session takes no armed row and the family named by that sentinel is the bug report.**
4. Only then `WARP`, and only then the armed rows.
5. Record the `[WTST]` block in the evidence directory beside the arm — it is the attestation
   that the arm was gated, and a session without one is unverified.

### 0.10.6 The disarm penalty — the pre-registered target and its named mechanism

**The measurement being attacked: −5.848 % (dhry 7 696, n = 7, spread 0.27 %) against a
pre-registration of −0.9 % … −2.0 % and a caveat of −2.276 %. 2.57× round 1's cost, and the
card ships it.**

**The named candidate is G-R2.2's own row and §0.4.3's own mechanism:** `m68k_run_mmu040` grew
**2 324 → 2 548 B (+224, +9.6 %)**, and §0.4.3 already recorded a +7.22 % unexplained
wall-clock move on identical work between two builds whose only difference was code motion,
with the mechanism named — *"an interpreter dispatch loop in a 32 KB Cortex-A9 L1I is exactly
the workload sensitive to that."*

**Where the +224 bytes came from, read off §0.9.3's own table:** `z3660_warp_step()` is
`always_inline`, so the **armed-only** half of it is inlined into the run loop and occupies
L1I on every disarmed pass that branches over it — the control-transfer-target test (6
instructions), the supervisor/odd-pc/trace gates (8) and the four-way hashed lookup (18) =
**32 ARM instructions ≈ 128 B**, plus the address materialisations gcc hoists for the table
and the generation.

**The change: `z3660_warp_step()` goes OUT OF LINE.** The run loop keeps only what it must —
one address materialisation, one load, one test, one perfectly predicted branch — and every
byte behind that branch leaves the loop's cache footprint. This **reverses §0.8.1 item 1's
inlining decision on the disarmed side, deliberately and with the measurement that forces
it**, and the cost is stated before it is paid: the armed path gains one AAPCS call, so the
armed non-target dispatch goes from 11 to roughly 15 instructions and the armed per-block from
≈ 108 to ≈ 114. **That trade is correct under this campaign's own rules** — the disarmed cost
is a SHIPPED cost until the M5 gate, the armed cost is not yet a verdict of any kind, and
§0.9.3 already found the entry cost dominated by `z3660_warp_run`'s 33-instruction preamble
rather than by the gate.

> ### The pre-registered targets
>
> - **static, measured this round:** `m68k_run_mmu040` = **2 330 … 2 400 B**, down from 2 548,
>   i.e. at least 148 of the 224 bytes returned. Derivation: the 32 inlined armed-only
>   instructions above are 128 B, and the hoisted materialisations they need are the rest.
> - **wall clock, scored at the NEXT metal session and not here:** the disarm penalty returns
>   to the round-1 class — **−2.276 % or better, dhry ≥ 7 988** — and the requirement is
>   *strictly better than −5.848 %*, which is the only half of it that is not already a
>   guess about a machine no one has run.
> - **the boot row rides beside it** (Q11): the `DBF9A5AF` class read 51 s / 49 s against the
>   clean lineage's 45 – 47 s. Predicted back into the **46 … 48 s** band.

**Two levers are named and NOT taken, so that the one that is taken can be scored alone.**
§0.9.6 item 3's open-coded memory fast path stays priced and unbuilt; and `z3660_warp_run`'s
33-instruction preamble is **not** reduced this round unless the fix touches it anyway — it is
the largest single row in §0.9.3's table, it is entirely on the **armed** path, and mixing an
armed-path reduction into the round whose one wall-clock target is a **disarmed** number is
how a result stops being attributable.

### 0.10.7 What round 3 explicitly does not establish

1. **That the fix works on silicon.** Round 3 takes no metal row. The reproducer is an
   arithmetic predicate and a harness invariant; the on-target self-test is *built* here and
   *run* by the next session. **Adding a range fix is not evidence of a fix; only the
   self-test's PASS on metal is**, and that is Q3's standing caveat applied to this round's
   own work.
2. **Anything about speed.** No wall-clock number in §0.10.6 is measured here.
3. **That the publish defect was the ONLY defect.** It explains all four occurrences and it is
   the one defect this round names. A second one would show as an on-target self-test failure
   in a family other than F1, which is precisely why the self-test is per-family.
4. **The memory-touching families on silicon.** The self-test is guest-memory-free by design
   (§0.10.5), so `(An)` and `d16(An)` remain harness-proven only. **Re-owed**, with the M2
   demand-paging session as the first honest test either way.
5. **`WARP_INSNS / WARP_BLK` against a representative stream** (§0.9.6 item 2). Still the
   verdict session's to take, still 2.0 … 4.5 as written in §0.8.4.
6. **Q5's second condition and Q6's decision rule.** The bracket is built and runnable; the
   numbers need a card.
7. **Everything §0.8.8 said round 2 would not establish**, unchanged.


## 0.11 M1 round 3 — built, and its gates scored (2026-08-24)

**The pre-registration in §0.10 was committed before the first line of round-3 code; the
implementation commits follow it in this repository's log**, which is what §3.4 asks of it —
round 1's preceded its code by 32 minutes, round 2's by an hour, the SPEC round's by 26 and
the `f` round's by 29. Every prediction in §0.10 is left byte-untouched above and scored
beside itself, `ifetch-fastpath.md`'s standard.

### 0.11.1 What was built

| piece | where |
|---|---|
| **the fix, part A** — `warp_publish()` maintains the range rounded OUT to whole cache lines; the shared CP15 loops are untouched | `Z3660_emu/src/uae/z3660_warp.cpp` |
| **the fix, part B** — the arena base aligned up, its size rounded down, and the bump advancing in whole lines so no two blocks share one | same file, `warp_init()` / `warp_compile()` |
| **the coverage predicate** — `Z3660_WARP_LINE`, `z3660_warp_line_lo/hi`, `z3660_warp_publish_covers`, `z3660_warp_publish_len`, transcribed from the loops and shared with the harness | `z3660_warp.h`, plain C, outside the engine guard |
| **two shipped invariants** — `WARP_BAD_PUBRANGE`, `WARP_BAD_UNALIGNED`, checked on every publish in **every** variant | `z3660_warp.cpp` |
| **the disarm-penalty change** — `z3660_warp_step()` out of line and `noinline`, the header keeping only the declaration | `z3660_warp.h` + `z3660_warp.cpp` |
| **the on-target self-test** — nine families, the alignment sweep, the exec-vs-decode oracle, the R6 bracket, and a scan that REFUSES any block containing a `blx` (the two helper registers are handed in as NULL on purpose, so the guest-memory-free property is enforced and not merely stated) | `z3660_warp.cpp`, `z3660_warp_selftest()` |
| **the ARM word executor promoted out of the harness**, so the target has a data-side oracle | `z3660_warp.cpp`, `warp_host_exec()` |
| **the compiler's self-test opcode source** — `warp_cc::src`, so no guest instruction page is read | `z3660_warp.cpp`, `warp_iword()` |
| **the `WARPTEST` console verb** and core1's one-shot consume at the knob resync | `Z3660/src/debug_console.c`, `uae/newcpu.cpp` |
| **the regression arms** — the discriminator, the live invariant, `M-PUBRANGE`, the self-test's machinery, and `M-VERDICT` at last | `Z3660_emu/test/host/harness_main.cpp` |

### 0.11.2 The gates

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| **G-R3.1** harness | green all three; **+25 … +90** default, the **same** in lean, prof a further **0 … +10** | green, `0 failed`: **3 301 / 3 297 / 3 528** against 3 280 / 3 276 / 3 507 — **+21 / +21 / +21** | green; **count MISSED, low** (rounds 1 and 2 missed it low too); "same in lean" **HIT**; prof's further **+0** **HIT** |
| **G-R3.2** the defect reproduces, then goes green | invariant **> 0** with round 2's range, **0** with the fix; `M-PUBRANGE` **caught** | clean: `PUBRANGE` **0** and `UNALIGNED` **0** over the real corpus; under `M-PUBRANGE` both **> 0**; and inside the self-test's own 64-case sweep, **0** range misses clean against **12** under the mutation | **HIT** |
| **G-R3.3** the discriminator | round 1's geometry covered at all four offsets; round 2's uncovered for **exactly 28 of 64** pairs = 43.75 % | **4 of 4** covered; **28** uncovered, to the pair; and **64 of 64** covered once the range is rounded out | **HIT** |
| **G-R3.4** disarmed object equivalence | **≤ 20** functions changed; `m68k_run_mmu040` **2 330 … 2 400 B** | **208 changed** of 25 073 common, 80 base-only / 26 new-only; `m68k_run_mmu040` **`0x9f4` → `0x894` = 2 548 → 2 196 B, −352** | changed-count **MISSED, high** (attributed below); size **MISSED, LOW** — 2 196 is **128 B below round 1's 2 324**, better than the band |
| **G-R3.5** lockstep identity, unmoved | `WARP_DIVERGE == 0` over `≥ 10⁸`, arms byte-identical | **0 over 100 000 000 guest instructions, 12 500 000 block executions, 100.0 % retired inside blocks** — bit-identical to round 2's run | **HIT** |
| **G-R3.6** the odd-`A7` audit | no generation-one shape can produce an `A7` the interpreter would not | enumerated in §0.11.5: five shapes can write `A7`, every one of them full-width and identical to `cpuemu_31`; `e_put_reg()` — the only narrow write-back — is never handed an address register; no shape in scope moves `A7` implicitly | **HIT** |
| **G-R3.7** the self-test exists and is inert | in all three images, verb-only, no guest memory, state restored, **0 bytes** on `m68k_run_mmu040` | present in all three; `m68k_run_mmu040` measures **`0x894` with the self-test and `0x894` without it** — measured by deleting the call, rebuilding and re-reading `nm`, not asserted; **+3 924 B** of cold `.text`; the corpus is a core1 opcode array and its own register/ctx bases, so **no guest page is read or written** | **HIT** |
| **G-R3.8** M-VERDICT, the owed arm | caught | the helper **serves** a read from a `TTR_NO_WRITE` window and **declines** a write to it; under `M-VERDICT` the write is **served and lands in the read-only region** — caught | **HIT**, and §0.9.6 item 1 is **CLOSED** |
| **G-R3.9** R6 is runnable | built behind the verb, global timer not PMU, prints cycles per block and per compiled instruction; **not run** | built; prints the bracket on target and *"no clock on this build"* on the host, which is the honest host answer | **HIT (built)**; **NOT RUN** — there is no card in this round |
| **G-R3.10** images | three rebuilt, CRCs recorded, archive fires; `q2probe` untouched | `BOOT.BIN` **`C8224D10`**, `BOOT-lean.BIN` **`C89953E2`**, `BOOT-prof.BIN` **`D9BD4449`**; 12 762 604 / 12 762 572 / 12 795 484 B; all three archived CRC-stamped; `BOOT-q2probe.BIN` untouched at **`FCC0CB3F`** | **HIT** |

### 0.11.3 The object-equivalence miss, attributed rather than excused

**208 against a pre-registered ≤ 20 is the round's largest miss and it is an LTO result, not a
behaviour result.** The attribution was measured, not argued: the self-test's call site was
deleted, the image rebuilt, and the diff re-run.

| build | changed functions | `m68k_run_mmu040` |
|---|---|---|
| round 3, complete | **208** | 2 196 B |
| round 3 with the self-test unreachable (`--gc-sections` drops it) | **205** | **2 196 B** |

**So 205 of the 208 come from taking one hot function out of line, and 3 from the self-test.**
`-flto` re-decides inlining across the whole program when a translation unit's call graph
changes materially, and the visible signature is exactly that: dozens of unrelated functions
moving by one or two instructions (`fill_prefetch` 30 → 29, `exception3_read` 50 → 51,
`floatx80_is_nan` 18 → 19), the C++ demangler dropped from the link and libstdc++'s
emergency-pool initialiser pulled in, and the lean image **shrinking 32 696 bytes**.

**Two link-set changes were chased down and removed rather than absorbed**, because they were
mine and not LTO's: nine function-local `static const` tables in the self-test were given
guard variables by gcc, which pulled `__cxa_guard_*` in, and a 64-bit division in the R6
bracket pulled `__aeabi_uldivmod` in. Both are gone — the tables are at file scope and the
bracket does 32-bit tick arithmetic on a timer that wraps every 7.8 s against a bracket of
milliseconds.

**What supports "behaviour-neutral" is not the diff.** It is that the same sources are green
across all three harness variants at 3 301 / 3 297 / 3 528 with zero failures, including the
10⁸-boundary lockstep oracle returning bit-identical totals to round 2's, and that the one
function the round set out to change is the one that changed on purpose. **The remaining
honest gap is the same one this whole round is about: a host cannot prove the silicon, and the
`WARPTEST` protocol of §0.10.5 is what closes it on the card.**

### 0.11.4 The disarm penalty — what the objdump says, and what is still owed

**Pre-registered `m68k_run_mmu040` = 2 330 … 2 400 B. Measured 2 196. MISSED, and in the cheap
direction — the band was not wide enough downwards.**

| build | `m68k_run_mmu040` | disarmed gate | armed non-target | disarm penalty |
|---|---|---|---|---|
| round 1 (`E80D04EE`) | 2 324 B | 5 | 60 per guest instruction | −2.276 % (metal) |
| round 2 (`DBF9A5AF`) | 2 548 B | 5 | 11 | **−5.848 % (metal)** |
| round 3 (`B6304B9C`) | **2 196 B** | **4** | **≈ 15** | **unmeasured — the next metal session** |

> **`corrected at R5`, rounds 1 and 2 only.** Clean rebuilds of those two rounds' own sources
> (§0.15.2) produce **2 144 B** and **2 348 B**, roughly 180 – 200 B below the flown images
> this table measures, and round 1's gate is **four instructions at depth 2** rather than five
> at depth 1. The flown images and their metal penalties are unchanged; what the SOURCES build
> is not what this table says. Round 3's row is confirmed exactly and is the only one that was.


**The disarmed gate also fell, 5 → 4, and it was not predicted to.** §0.9.3's five were two
address materialisations, a flag load, a test and gcc's hoisted argument load. gcc now uses a
literal-pool load from the function's own constant island for the gate's address (one
instruction, not two) and — the part that matters — **the argument load is below the branch
rather than above it**, because with the body out of line there is nothing above the branch to
hoist it for. The disarmed path is now `ldr` (gate address), `ldr` (the flag), `cmp`, `beq`,
and nothing else.

**The armed side paid what §0.10.6 said it would**, and slightly less: the early-out through
`z3660_warp_step()` reaches its return **before** the AAPCS prologue — `push {lr}` is emitted
only after the trace test — so an armed non-target dispatch is the 4-instruction gate, the
argument load, the `bl`, seven instructions in the callee, the return and the caller's test:
**≈ 15**, against the pre-registered ~15 and round 2's 11.

> **The wall clock is NOT scored here and the pre-registration says so.** The target stands as
> written for the next metal session: **the disarm penalty returns to −2.276 % or better
> (dhry ≥ 7 988), and the requirement is strictly better than −5.848 %**, with Q11's boot row
> beside it in the 46 … 48 s band. A 352-byte reduction in the loop that carries the mechanism
> §0.4.3 named is a *reason to expect* that and not a substitute for measuring it.

**`z3660_warp_run`'s 33-instruction preamble was NOT touched, deliberately** (§0.10.6): the
function is `0xec` = 236 bytes in both round 2 and round 3, unchanged. It is entirely on the
armed path, it is the largest single row in §0.9.3's table, and mixing an armed-path reduction
into the round whose one wall-clock target is a disarmed number would have made neither
attributable. **Re-owed.**

### 0.11.5 The odd-`A7` audit, in full

§0.10.4 G-R3.6 asked for a static enumeration. Every generation-one shape that can write
`regs.regs[15]`, with what it writes:

| shape | what it writes to `An` | the interpreter |
|---|---|---|
| `MOVE.L <ea>,A7` / `MOVEA.L` | the source, verbatim, 32 bits | identical |
| `MOVEA.W <ea>,A7` | `e_sxth` then a full 32-bit store | `(uae_s32)(uae_s16)` — identical |
| `MOVE.B …,An` | **refused** (`warp_emit_insn` returns 0) | — |
| `ADDQ`/`SUBQ` `#n,A7` (`.W`/`.L`) | a full 32-bit add/sub of the whole register, no flags | identical; `.B` to `An` is refused |
| `ADDA`/`SUBA` `<ea>,A7` | `.W` source sign-extended, then a full 32-bit add/sub | identical |
| `CMPA` | writes nothing | — |

**The one narrow write-back path in the compiler, `e_put_reg()`, is never handed an address
register**: every call site passes a data-register number (`reg` in the `ADDQ`/`SUBQ` arm,
`rdst` in the ALU arm, both 0 – 7). And nothing in generation one's frozen scope moves `A7`
implicitly — `BSR`, `LINK`/`UNLK`, `MOVEM`, and `(An)+` / `-(An)` are all out of scope, which
leaves no path by which a block can push, pop or auto-adjust a stack pointer at all.

**Dynamically the same conclusion was already green and is worth naming:** the all-65 536-opcode
sweep runs every accepted opcode both ways from six seeded states and compares the whole §3.1
set, `A7` included — 45 696 differential pairs, 0 divergences.

> **So `a7=8080002A` in occurrence 4 is a consequence and not a cause.** An odd address
> register is architecturally legal on a 68040 — only its *use* as a stack pointer faults —
> and the guest reached it after control had already been handed somewhere it should not have
> been. There is no second defect hiding behind it.

### 0.11.6 Three things worth recording that were not gates

- **The fix has two halves and the corpus proves each one alone.** In the self-test's
  alignment sweep every block is placed at a deliberate non-zero offset — 56 of the 64 are off
  a cache line — and **not one of them is published with a range that misses part of it**. That
  is part A (the rounded-out range) working with part B (the aligned bump) deliberately
  defeated, which is the only way to show the correctness fix is not leaning on the hardening.
- **The invariant is shipped, not harness-only, and that is a change of policy.** The other four
  `WARP_BAD_*` counters are harness assertions about state a host can see. These two are about
  the one seam a host cannot see, so they are compiled into every image and read back by the
  self-test. Their cost is two compares on the **compile** path — once per block against Q4's
  ~42 µs budget — and nothing at all on dispatch.
- **`mkbootimage` was missing from its expected path and the images were built with the
  container's copy.** `z3660-firmware/Z-TURN/vitis_ide/Makefile` resolves
  `BOOTGEN_EXE_HOST` to `mkbootimage` on `PATH` or `$HOME/git/zynq-mkbootimage/mkbootimage`,
  and neither existed; the binary the docker build image ships at `/usr/local/bin/mkbootimage`
  was used instead, against the same host-built ELFs. The three images are therefore packaged
  by the same tool the container would have used, but the host's copy is worth restoring so
  `make lean` works without one.

### 0.11.7 What round 3 leaves owed

1. **The metal proof of the fix.** §0.10.7 item 1 stands exactly as written: adding a range fix
   is not evidence of a fix. The next session runs `WARPTEST` first, on the image it intends to
   arm, and takes no armed row unless it ends `VERDICT: PASS 9/9`.
2. **The memory-touching families on silicon.** The self-test is guest-memory-free by design,
   so `(An)` and `d16(An)` stay harness-proven. Re-owed, with M2 as the first honest test.
3. **`z3660_warp_run`'s preamble**, unchanged at 236 bytes and still the largest row in
   §0.9.3's table. Re-owed, on the armed path, for a round that measures the armed path.
4. **The open-coded expansion of the memory fast path** (§0.9.6 item 3), still priced and not
   built.
5. **`WARP_INSNS / WARP_BLK` against a representative stream**, still 2.0 … 4.5 as written in
   §0.8.4 and still the verdict session's to take.
6. **Q5's second condition and Q6's decision rule.** The bracket is built and prints; the
   numbers need a card.
7. **Everything §0.8.8 and §0.10.7 said would not be established, was not.**


## 0.12 M1a round 4 — pre-registered, before the first line of round-4 code (2026-08-24)

**This section is written before any round-4 engine code and is scored as written.** §3.4's
two rules govern it exactly as they governed §0.6, §0.8 and §0.10: the pre-registration commit
precedes the implementation commits, provably, by commit timestamp in this repository; and a
pre-registration that gets rewritten is not one, so every number below is left byte-untouched
and scored beside itself.

**Round 4 is the M1a wave §0.5.2's decision table routed round 3 into**, and it is legitimate
only because §0.5.1 froze its family list before the first measurement existed. It extends the
engine in that frozen order and it does not touch the kill line, which remains armed and
unfired for the first wave whose coverage is in band.

### 0.12.0 THE CHURN, ATTRIBUTED — the cause is named, and the framing it came with is refuted

§0.11's verdict flagged `WARP_DISC` 37 399 against `WARP_COMPILE` 37 499 — *"99.7 % of
everything compiled in the window was discarded inside it, across `WARP_DISC_ROOT` = 8 125
discard-all events (~271/s). The block cache never accumulates a working set."* — and claimed
no mechanism. **The attribution is available in the R3 evidence directory itself, it names the
cause exactly, and it also corrects two numbers in that sentence.**

**First correction: 8 125 is not an event count.** `warp_discard_all()` charges
`WARP_DISC_ROOT` with `warp_live`, the number of live blocks, exactly as it charges
`WARP_DISC` — so 8 125 is *blocks discarded at `IV_ROOT` causes*, not events. The event count
is `DOPC_INVAL`, because `z3660_warp_invalidate()` hangs off the decoded-op cache's own choke
point and is called once per performed invalidation.

**Second correction: the per-cause split is already measured, by the `IV_*` block (ids 28–38),
and the identity closes exactly.**

| cause (id) | events | share | what it is |
|---|---|---|---|
| `IV_FLUSH` (28) | **3 171** | **46.87 %** | `CPUSHL` / `CINV` — the guest's own cache maintenance. Names a PHYSICAL address |
| `IV_PFLUSHA` (36) | **2 712** | **40.09 %** | `PFLUSHA`, and `mmu_set_tc()`/`mmu_reset()` through it — flush every ATC entry |
| `IV_PFLUSH` (35) | **591** | **8.74 %** | `PFLUSH` of ONE page. The only cause that names a LOGICAL address |
| `IV_ROOT` (29) | **263** | **3.89 %** | `MOVEC` to `URP`/`SRP` — the address space changed. **Q5's cause** |
| `IV_DMA` (30) | **28** | **0.41 %** | core0 wrote guest DRAM |
| `IV_TABLE`/`KNOB`/`WRAP`/`MAP`/`CACR` | **0** | — | none in the window |
| **total** | **6 765** | | **= `DOPC_INVAL` 6 765, EXACTLY** — §0.5.5 G2's third identity |
| `IV_CACR_SKIP` (38) | 14 886 | — | narrowed at the call site; **never reaches the choke point**, so it costs WARP nothing. 68.75 % of 21 651 requests were already refused before this |

> ### The named cause: **87.0 % of the events that empty WARP's code cache are cache- and ATC-maintenance instructions the guest executes — `CPUSHL`/`CINV` (46.87 %) and `PFLUSHA` (40.09 %).** `IV_ROOT`, the only cause Q5 can address, is **3.89 %** of them.

**The rate, corrected and made window-independent.** The armed profiling window is 34 s
(`PROF ZERO` 06:48:18 → `PROFD` 06:48:52, `SWITCH-STATE-LOG.txt`), so 6 765 events is
**199 per second**, not 271. The rate that does not depend on reading a clock off a serial log
is **108.3 discard-all events per million retired guest instructions**, and that is the number
this section is scored on.

**And now the framing.** *"The block cache never accumulates a working set"* is not what these
counters say. Blocks per event and executions per compile are both computable from the same
dump and they point the other way:

```
   discard-all events                          6 765   (= DOPC_INVAL)
   live blocks discarded, all causes          37 399   -> 5.53 blocks per event
   live blocks discarded at IV_ROOT            8 125   ->  30.89 per IV_ROOT event
   ... at every other cause          37399-8125=29 274 ->   4.50 per event

   WARP_BLK / WARP_COMPILE  = 13016813 / 37499  =  347.1 executions per compile
     real blocks   3558475 entries / 10404 real compiles  =  342.0 each
     decline stubs 9458338 exec    / 27095 stubs          =  349.1 each

   compile cost  37499 x 1.427 us (R6 lean)  =  53.5 ms  /  34 s  =  0.157 % of one core
```

> ### **A compiled block is executed 342 times before it is discarded.** That is amortisation, not churn. `WARP_DISC ≈ WARP_COMPILE` is a DEFINITIONAL artefact of a small hot working set discarded often — `WARP_DISC` sums *live* blocks at each event, so in any steady state where nothing is evicted and nothing wraps, discards must equal compiles minus the residue. `WARP_EVICT` = 0 and `WARP_FLUSH` = 0 say exactly that. **The 99.7 % is an identity, not a pathology, and round 3 read it as one because the counter that separates events from blocks did not exist.**

**Therefore round 4 builds the counters and does NOT build a fix, and the reason is measured
rather than preferred.** The churn costs 0.157 % of a core, the cache amortises 342×, and the
coverage miss is fully explained by the two counters that already agree — bail rate 72.66 %,
decline rate 72.26 %. There is nothing here to buy back.

**Q5 is NOT re-opened, and the new evidence makes it deader rather than closer.** §0.11's R6
row closed it on cost (`c2` = 0.0426 % against a 2 % bar, failing by 47×). This attribution
adds a second, independent reason: a per-address-space generation addresses `IV_ROOT`, which
is **3.89 % of the events**. Funding the smallest cause of a cost that is already 0.157 % of a
core is not a trade this campaign takes. **`IV_FLUSH` and `IV_PFLUSHA` — the 87 % — cannot be
narrowed by a generation stamp at all**: the first names a physical address while every WARP
tag is logical, and the second is a statement that *every* logical→physical mapping may have
moved, which is precisely the event a logically-keyed cache must honour in full.

**What round 4 builds instead is the instrument that turns this inference into a counter**
(§0.12.4 D). The attribution above is arithmetic over counters that were not designed to
answer it; ids 61–63 make it a direct reading.

### 0.12.1 THE DISARM-PENALTY RESIDUAL — the mechanism, read off the shipping objdump

**§0.10.6's named mechanism is refuted and §0.11.4 said so: round 3's `m68k_run_mmu040` is
2 196 B, 128 B BELOW round 1's 2 324, and its penalty is 1.78× round 1's.** A smaller loop
costing more is not an L1I-footprint story. Here is what the disassembly of the image that was
actually measured says, and it is a *shape* result rather than a size one.

**The gate, `build-lean/Z3660_emu.elf` (`m68k_run_mmu040` = `0x894` = 2 196 B, the C89953E2
lineage), verbatim:**

```
   3024f490:  blx  r3                       <- the interpreter handler returns here
   3024f494:  ldr  r3, [pc, #2028]          <- literal pool: the ADDRESS of z3660_warp_gate
   3024f498:  ldr  r3, [r3]                 <- the flag  -- DEPENDENT on the load above
   3024f49c:  cmp  r3, #0
   3024f4a0:  beq  3024f4d0                 <- the disarmed exit, perfectly predicted
```

> ### The named mechanism: **round 3's four-instruction gate contains a SERIAL LOAD-TO-LOAD ADDRESS DEPENDENCY WITH ZERO SLACK.** `ldr r3,[pc,#2028]` produces the base that `ldr r3,[r3]` immediately consumes, and the pair is issued in the shadow of nothing — it sits directly after the handler's `blx` returns, so there is no independent work to hide either load's use latency behind. Round 1's five instructions were **two ALU immediates** (`movw`/`movt`, no memory port, freely schedulable) feeding **one** load. **The instruction count fell 5 → 4 and the dependency depth rose 1 → 2, and it is the depth that is billed.**

**The arithmetic, and it is a check rather than the claim.** At §0.9.3's own measured
214.2 cycles per interpreted guest instruction:

| build | gate instructions | measured penalty | implied cycles | implied cycles/instruction |
|---|---|---|---|---|
| round 1 `E80D04EE` | 5 (movw, movt, ldr, test, b) | −2.276 % | **4.87** | **0.97** |
| round 3 `C89953E2` | 4 (ldr, ldr, cmp, b) | −4.056 % | **8.69** | **2.17** |

**2.17 cycles per instruction is exactly §0.9.3's measured IPC 0.46 on this loop**, and 0.97 is
what a pair of dual-issuable ALU ops plus one load resolves to. **The same loop, one fewer
instruction, 2.24× the cost per instruction.** Two independent readings — the objdump's shape
and the two metal rows' arithmetic — name the same thing.

> **`corrected at R5`.** §0.15.2 clean-rebuilt round 1's own source and it produces
> **four instructions at dependency depth 2** — `ldr [pc,#1976]` → `ldr [r3]` → `cmp` → `beq`
> — not the five ALU-immediate instructions at depth 1 this table charges it with. **There was
> never a depth difference between the two rows this argument is built on.** The paragraph is
> left byte-untouched; the row it rests on is not what round 1's source produces.

**Why gcc did it.** `-Os` prefers one literal-pool load to a `movw`/`movt` pair, and with the
body moved out of line (§0.10.6) there was nothing left above the branch to keep an address
register alive for. §0.11.4 recorded the change and read it as a win — *"one instruction, not
two"* — which is the instruction-count-is-not-a-cost error §0.4.3 warned about, made in the
other direction.

**THE FIX: the gate reads a base register the loop already holds.** The same disassembly shows
`r4` is live across the whole dispatch and is `&regs`:

```
   3024f448:  ldr  r4, [pc, #2088]   -> 0x307aeee8  =  B regs
   ... every pass: ldr [r4,#64] [r4,#68] [r4,#72] [r4,#76] [r4,#80], str [r4,#88]
```

So the arm flag moves **into `regstruct`**, beside the pc fields the loop already reads and
writes on every single pass, and the gate becomes one load off a live base and a hot line:

```
   ldr  rX, [r4, #off]
   cmp  rX, #0
   beq  ...
```

**Three instructions, ONE load, ZERO address materialisation, and the load hits a cache line
the loop has already touched this pass.** `struct regstruct` is already carrying a block of
fork-added MMU state (`mmu_fslw` … `fc030`, `mmu_enabled`, `mmu_page_size`), so this is inside
the existing fork boundary rather than a new one; no offset in the tree is hardcoded (the JIT
uses `offsetof`, checked: zero literal offsets), and nothing memsets or memcpys `regs`
(checked: zero sites). `z3660_warp_gate.last_pc` stays where it is — it is read only on the
ARMED path, which pays its own address materialisation inside the out-of-line `step()`.

### 0.12.2 THE `(An)+` / `−(An)` STATIC READ — §0.5.1's named M1a question, ANSWERED

§0.5.1 left this as *"a named M1 scoping question with a static answer — read the generated
handler, before any metal time."* Here is the read, on `cpuemu_31.cpp`, the table the 040 loop
actually dispatches.

**`MOVE.L (An)+,Dn` — `op_2018_31_ff`:**

```
   srca = m68k_areg(regs, srcreg);              <- the PRE-increment value
   mmufixup[0].reg   = srcreg;
   mmufixup[0].value = m68k_areg(regs, srcreg); <- armed with the CURRENT value
   uae_s32 src = get_long_mmu040(srca);         <- THE ONLY FAULTABLE SITE
   m68k_areg(regs, srcreg) += 4;                <- the write-back is AFTER the access
   mmufixup[0].reg = -1;
```

**`MOVE.L −(An),Dn` — `op_2020_31_ff`** has the identical shape: `srca = An − 4`, fixup armed
with the unmodified `An`, the access, and only then `An = srca`.

> ### The answer: **PERMISSIVE, and the reason is that `mmufixup` is a NO-OP RESTORE on the source side.** The register write-back happens strictly *after* the faultable access, so at the moment the access can fault the register still holds its original value — and `mmufixup[0].value` is that same original value. §2e's write-back rule bars a family whose register state is architecturally complete before its access; on a source auto-adjust it is not. **A bail before the access is byte-identical to the interpreter's post-fault state, and generated code never needs to arm `mmufixup` at all.**

**The contrast that makes the read binding rather than convenient.** The DESTINATION side is
exactly what §2e says it is — `MOVE.L Dn,(An)+`, `op_20c0_31_ff`:

```
   m68k_areg(regs, dstreg) += 4;      <- the write-back is BEFORE the store
   ... m68k_incpci(2);
   regs.instruction_pc = m68k_getpci();
   mmu_restart = false;
   mmufixup[0].reg = -1;
   put_long_mmu040(dsta, src);        <- the faultable site, on a COMPLETE instruction
```

**and so does the double form** `MOVE.L (An)+,(An)+` (`op_20d8_31_ff`): the source increment
lands before the destination EA is even read, so a decline at the store cannot be restarted.

> ### **THE RULE ROUND 4 ADOPTS, and it is narrower than §0.5.1 item 3 as written: an `(An)+` / `−(An)` SOURCE is compiled ONLY when the instruction has EXACTLY ONE FAULTABLE SITE — that is, only with a REGISTER destination.** Auto-adjusting sources feeding a memory destination are **refused**, because the source's register side effect lands between the two faultable sites and a bail at the second one would re-execute an instruction whose base register has already moved. This is stated before the corpus is counted, and §0.12.3 prices what the restriction costs: 22 of the 240 instructions in the hot corpus, the single largest thing round 4 leaves on the table.
>
> The alternative — emitting an undo on the bail arm — was considered and refused. It is not equivalent in general: for `MOVE.L (A0)+,(A0)` the interpreter computes the destination EA from the *incremented* `A0`, so deferring or undoing the write-back changes the address. A correct version needs a source/destination aliasing analysis per shape, and that is tranche 2's to price, not a rule to smuggle in here.

**`A7` is handled and it is not a footnote.** `areg_byteinc[] = { 1,1,1,1,1,1,1,2 }`
(`newcpu.cpp:165`), and every byte-size auto-adjust in the generated table indexes it — one
handler covers all eight registers. The emitter uses the same table, and the all-65 536-opcode
differential sweep is what proves it did.

### 0.12.3 THE CORPUS — what the engine refused, derived because no counter recorded it

**There is no decline histogram in the R3 evidence directory** (checked: `WARP_DECLINE` is a
scalar and nothing samples the refused opcode). So the pre-registered decline drop is
**derived from the workload's own text**, by `docs/warp-corpus.py`, committed with this section
so the derivation can be re-run. It disassembles the AMIX dhrystone binary every WARP row has
been measured on (`sum -r 52076`, 8 135 bytes) and classifies each opcode word under a hand
transcription of `warp_emit_insn()`'s accept predicate.

**The transcription is not trusted** — gate G-R4.2 requires the ENGINE's own accept set over
all 65 536 opcode words to agree with the model's, opcode for opcode.

| corpus | insns | R3 accepts | R4 accepts | R3 mean accepted run | R4 mean accepted run |
|---|---|---|---|---|---|
| whole `.text` | 632 | 21.99 % | 28.48 % | 1.655 | 2.500 |
| hot set (`main` + callees) | 609 | 21.67 % | 28.41 % | 1.714 | 2.662 |
| **`Proc_`/`Func_` callees only** | **240** | **31.67 %** | **43.75 %** | **1.583** | **2.763** |

> ### **The corpus model reproduces the metal row it was not fitted to.** Round 3 measured **1.581** compiled guest instructions per real block on silicon (`WARP_CINSNS`/(`COMPILE` − `DECLINE`) = 16 448 / 10 404). The callees-only static run length is **1.583**. Three digits, on a number derived from a disassembly and a source transcription with no metal input at all. **That is what licenses using the same model to predict round 4**, and it is the strongest single piece of evidence in this pre-registration.

**The tier split is load-bearing, not decoration.** `main` carries both the measured loop and a
printf banner that runs once; on a 632-instruction image that banner is **102 `pea <abs.L>`**
argument pushes — 16 % of the text and ~0 % of the executions. The callees-only tier is the
closest a static count honestly gets to the executed mix, and the gap between the tiers is the
measure of how far to trust it. Two further limits are stated rather than buried: **libc is
absent** (the binary is dynamically linked, so `strcpy`/`strcmp` — which Dhrystone leans on —
are in no corpus here at all), and **a switch table inside `Proc_6` disassembles as
instructions** (11 words are identified as data and dropped; the count is reported).

**What round 4 gains, and what it still refuses, on the callees tier:**

```
   GAINS (29 of 240)   MOVEQ 15 | TST.L 5 | ALU-mem 4 (addl,cmpb,cmpl) | IMM 4 | shift-imm 1
   REFUSES (135)       MOVE to -(An)      22   <- write-back destination, S2e, permanent for gen 1
                       MOVE (An)+,mem     22   <- S0.12.2's one-faultable-site rule
                       LINK/UNLK          22   | RTS 11 | LEA 9 | JSR 7 | CLR 6 | MOVEM 4
                       PEA 3 | EXTB 3 | ORI to mem 3 | the rest 23
```

> **Named here, before the row, so it cannot be discovered afterwards: the next three levers are `LEA`, `CLR` and `EXT`/`EXTB` — all three in the M0 census's *fully register-only* family list — and they are in NEITHER §0.5.1's generation-one table NOR its frozen M1a list. Round 4 does not build them.** Adding a family because the corpus says it would help is exactly the rescue §0.5.1's freeze exists to forbid. They are tranche 2's to pre-register, on their own commit, and this paragraph is the record that the temptation was priced and declined.

### 0.12.4 What round 4 builds

**A — generation one's own frozen scope, COMPLETED. This is a shortfall being closed, not an
extension, and it is scored separately from the M1a tranche.** §0.5.1's generation-one table
names *"the mixed `ADD ADDA AND CMP CMPA EOR OR SUB SUBA TST` and the shift/rotate families the
census already found fully pure"*. **`TST` and every shift/rotate register form were named in
the frozen scope and were never built** — `warp_emit_insn()` refuses all of them today. Round 4
builds them and reports the gap as a finding.

| A1 | `TST.B`/`.W`/`.L` `Dn`, and `TST.W`/`.L` `An` |
| A2 | `ASL`/`ASR`/`LSL`/`LSR`/`ROL`/`ROR`, immediate and register count, `.B`/`.W`/`.L`, `Dn` destination. **`ROXL`/`ROXR` are excluded and the exclusion is named**: they take `X` *in* as well as out, which no other generation-one shape does, and the block-entry flag seam packs `X` as a separate word |

**B — M1a tranche 1, in §0.5.1's frozen order.**

| B1 | item 1 — `MOVEQ.L #<data>,Dn` |
| B2 | item 2 — the remaining **register-destination** shapes of `ADD`/`SUB`/`AND`/`OR`/`CMP`/`EOR`: a **memory source** (`(An)`, `d16(An)`) with a `Dn` or `An` destination, and the **immediate** forms `ADDI`/`SUBI`/`ANDI`/`ORI`/`EORI`/`CMPI` with a `Dn` destination. Mode 7 (absolute, PC-relative, immediate-as-EA) stays out — `(xxx).L` is item 4 |
| B3 | item 3 — `MOVE`/`MOVEA` **and** the B2 ALU shapes with an `(An)+` / `−(An)` **SOURCE**, restricted by §0.12.2's one-faultable-site rule to a register destination |

**C — the disarm-penalty fix** (§0.12.1): the arm flag moves into `regstruct`; the gate becomes
`ldr`/`cmp`/`b` off `r4`.

**D — the churn instrument** (§0.12.0), append-only, no id moves, no field changes shape, **dump
version stays 2**, `Z3660_PROF_C_COUNT` 61 → **64**:

| 61 | `WARP_DISC_EV` | discard-all **events**, one per call — the quantity round 3 had to borrow `DOPC_INVAL` for |
| 62 | `WARP_DISC_ATC` | blocks discarded at `IV_PFLUSH` + `IV_PFLUSHA` |
| 63 | `WARP_DISC_CPUSH` | blocks discarded at `IV_FLUSH` |

**E — the harness and the on-target self-test grow with the engine** (§0.12.5).

**Explicitly NOT built, and each has a reason rather than an omission:** `z3660_warp_run`'s
33-instruction preamble (§0.11.7 item 3) — it is on the ARMED path and round 4's one wall-clock
target is a DISARMED number, which is §0.10.6's own rule for keeping a row attributable, so it
stays **re-owed**; the open-coded memory fast path (§0.9.6 item 3); `LEA`/`CLR`/`EXT`
(§0.12.3); any churn fix (§0.12.0); Q5 (§0.12.0); Q6 (dead, §0.11).

### 0.12.5 Correctness — the machinery grows with the engine, and the new arms are enumerated

**G4/G5's standing rule applies to every new family: a family is not in the engine until it is
in the sweep, in the differential, in a mutation arm and — if it is register-only — in the
on-target self-test.**

**The 65 536-opcode sweep** runs every ACCEPTED opcode both ways from six seeded states and
compares §3.1's whole set. Round 3's run was 45 696 differential pairs, 0 divergences; the pair
count rises with the accept set.

**`WARPTEST` grows from 9 families to 13.** The self-test is guest-memory-free by design and
**refuses any block containing a `blx`** (§0.11.1), so the memory-touching additions — B2's
memory-source ALU and B3's auto-adjust sources — **cannot** be covered on target and stay
harness-proven, exactly as `(An)`/`d16(An)` `MOVE` already does (§0.10.7 item 4, still re-owed).
The four new families are all register-only:

| F10 | `MOVEQ` | F11 | `TST` | F12 | shifts and rotates | F13 | immediate ALU |

**The ARM word executor must grow too, and that is a gate rather than a detail.** The
exec-vs-decode oracle is a second implementation of ARM; any host instruction the new emitters
produce that `warp_host_exec()` cannot decode makes the oracle silently weaker. Every newly
emitted ARM encoding is added to it and G-R4.6 requires the self-test to exercise one.

**New mutation arms, one per new family and one per new decision** (`Z3660_WARP_MUT_COUNT`
16 → 24):

| M-MOVEQ-FLAG | `MOVEQ`'s keep-mask made `0xF0000000` — C and V survive instead of being cleared |
| M-TST-FLAG | `TST` commits C and V |
| M-SHIFT-C | the shift's carry taken from the wrong bit |
| M-SHIFT-X | the shift's `X` written on a zero count (the 68k rule is that a zero count leaves `X` alone) |
| M-IMM-CARRY | the immediate ALU's carry inversion dropped |
| M-ALUMEM-EARLY | the memory-source ALU commits flags BEFORE the source read can decline |
| M-AUTO-BYTEINC | the auto-adjust uses 1 for `A7` at byte size instead of `areg_byteinc[]`'s 2 |
| M-AUTO-ORDER | the auto-adjust write-back emitted BEFORE the access — the defect §0.12.2's read exists to forbid, put back so the arm that catches it is shown to catch it |

**Fault injection on the new families**, and `M-AUTO-ORDER`'s corpus is the load-bearing one:
a declining source read on an `(An)+` shape must leave `An` **unchanged** and bail with the
per-site pc, `instruction_pc` and `mmu_restart` of the instruction *before* it.

**G3** (no `throw` reachable from generated code) is re-run over the new helper set. **G1**
lockstep is re-run to ≥ 10⁸ with `WARP_DIVERGE == 0`.

### 0.12.6 The round-4 gates, with their predictions

| gate | pre-registered |
|---|---|
| **G-R4.1** harness green, all three variants | `0 failed`; count **+90 … +260** over 3 301 / 3 297 / 3 528, and the **same** delta in lean |
| **G-R4.2** the accept-set discriminator | the engine's accept set over all 65 536 opcode words equals `docs/warp-corpus.py`'s model **exactly**; model counts **R3 7 616 → R4 18 168**, and the engine must land on **18 168** |
| **G-R4.3** the sweep and the differential | every accepted opcode run both ways from six seeded states; **0 divergences**; pair count **≥ 100 000** |
| **G-R4.4** the eight new mutation arms | every one **caught**; every one **clean** when disarmed |
| **G-R4.5** lockstep | `WARP_DIVERGE == 0` over **≥ 10⁸** retired guest instructions |
| **G-R4.6** `WARPTEST` | **13** families, sentinels = results, F1's 64/64 alignment sweep still `+0` range misses, and the new families exercise at least one newly emitted ARM encoding through the word executor |
| **G-R4.7** the disarmed gate, static | **3** ARM instructions, **one** load, **zero** address materialisations, off `r4`; `m68k_run_mmu040` **≤ 2 196 B** |
| **G-R4.8** the armed entry cost, static | `E` per block re-read off the lean objdump; pre-registered **108 … 120** (round 3's ≈ 114 after the out-of-line change), armed non-target gate **13 … 15** |
| **G-R4.9** the churn instrument | `WARP_DISC_EV` == `DOPC_INVAL` exactly, and `DISC_ROOT + DISC_ATC + DISC_CPUSH + other == DISC` exactly |
| **G-R4.10** images | three rebuilt, CRCs recorded, archive fires; `q2probe` untouched at `FCC0CB3F` |
| **G-R4.11** object equivalence | not gated. §0.11.3 established that `-flto` re-decides inlining program-wide when a call graph moves; round 4 moves it again and a changed-function count is not evidence of anything |

### 0.12.7 The round-4 metal bands, derived before the row

**Every band below is derived at the MEASURED entry cost `E ≈ 114` the way §0.9.3 derived
round 3's — the band that contained the result — and NOT at a guessed `E`.** §0.8.6's band A
was derived at a guessed `E` and missed low by 754 dhrystones; §0.9.3's band B was derived at
the measured one and contained the row with 146 to spare. This section uses band B's method.

**Step 1 — the mix, from §0.12.3's corpus, carried onto the measured R3 row.** The static
model reads 3.93 pp optimistic against R3's measured decline rate (68.33 % static against
72.26 % measured), and that offset is carried:

```
   productive-entry fraction  R3 measured 3558475/13016813 = 27.34 %
                              R3 static 31.67 %  ->  measured/static = 0.8633
                              R4 static 43.75 %  x 0.8633  =  37.77 %
   retired per productive entry  R3 measured 5422222/3558475 = 1.5238
                              scaled by the run-length ratio 2.763/1.583 = 1.7455
                              R4  =  1.5238 x 1.7455  =  2.660
   I_true = (INSNS - WARP_ENTER) + WARP_INSNS = 64 320 252, a workload constant
   WARP_INSNS(R4) = 13016813 x 0.3777 x 2.660 = 13.08 M   ->  c = 13.08/64.32 = 20.3 %
```

| row | pre-registered band | point estimate |
|---|---|---|
| **coverage `c`** | **0.15 – 0.28** (low: block length rises, entries do not; high: +40 % on the arrival shift) | **0.20** |
| **`WARP_INSNS`/`WARP_BLK`** | **0.75 – 1.35** | **1.005** |
| **compiled length, `CINSNS`/(`COMPILE`−`DECLINE`)** | **2.2 – 3.2** | **2.76** |
| **decline rate `DECLINE`/`COMPILE`** | **55 – 65 %** | **60.2 %** |
| **bail rate `BAIL`/`BLK`** | **56 – 68 %** | **62.2 %** |
| **`WARP_DISC_EV` per M retired insns** | **80 – 140** | **108** |

> ### **THE COVERAGE CALL, MADE BEFORE THE ROW: this tranche does NOT reach 60 %, and M1a tranche 2 is predicted now rather than after a disappointing number.** 0.15 – 0.28 against a 60 % bar is not close, and §0.12.3 names the three families that stand between here and there. The kill line's precondition therefore stays unmet for a second consecutive wave, the kill line stays armed and unfired, and **round 4 takes no verdict of any kind.** Saying so in the pre-registration is the only place it can honestly be said.

**Step 2 — the armed dhrystone band, §0.9.3's arithmetic with its gate term calibrated on
round 3's own row.** §0.9.3's raw form over-predicts the cost of round 3's armed row by
6.4 – 10.2 cycles per guest instruction, because it charges the armed non-target gate at the
full 2.17 cycles per instruction when most of those fifteen instructions are independent. The
calibration is done once, on the row that exists, and then applied:

```
   R3 measured 0.8778x  ->  T' = 214.2/0.8778 = 244.0 cy   (net +29.8 cy per guest insn)
   entry(R3) = 0.0843 x (114/1.5238) x 2.17 = 13.68 cy
   removal(R3) = 19.18%..40.34% x 0.0843 x 214.2 = 3.5..7.3 cy
   -> the gate term must be 29.8 - 13.68 + (3.5..7.3) = 19.6..23.4 cy over 0.9157 of dispatches
   -> G_eff = 15 instructions costing 22.1 cy  =  1.47 cy each   (CALIBRATED, once)

   R4 LOW    c=0.15, L=2.2, E=114, G_eff = 22.1 x 14/15 = 20.6 cy
      entry   0.15 x (114/2.2) x 2.17          = 16.87 cy
      gate    0.85 x 20.6                      = 17.51 cy
      removal 19.18% x 0.15 x 214.2            =  6.16 cy
      T' = 214.2 - 6.16 + 34.38 = 242.4        ->  0.883x
   R4 HIGH   c=0.28, L=3.2, E=114, G_eff = 22.1 - 4.3 = 17.8 cy  (the load chain removed)
      entry   0.28 x (114/3.2) x 2.17          = 21.63 cy
      gate    0.72 x 17.8                      = 12.82 cy
      removal 40.34% x 0.28 x 214.2            = 24.19 cy
      T' = 214.2 - 24.19 + 34.45 = 224.5       ->  0.954x
```

> ### **The round-4 armed band: 0.883× – 0.954× of the same-session clean-interpreter reference row `R`.** Against R3's `R` = 8 185 that is **dhry ≈ 7 227 – 7 810**. Round 3 measured 0.8778× = 7 185, so the floor of this band is **above** round 3's row by 0.6 % and the ceiling by 7.7 %. **This band does not cross 1.0× and it is not supposed to** — the engine is still narrow, the entry cost still dominates, and §0.5.3's arithmetic says amortisation only starts paying above `L` ≈ 8.
>
> **`TAILPOLL`'s removal row is charged ZERO in both ends and the correction is declared.** §0.5.3 priced it as `1 − 4/L` at a pre-registered `L` of 4 – 8. At `L` = 2.2 – 3.2 that is *negative*: the service poll now runs once per block, which is MORE often than once per four instructions, so it is a net ADD of `(4/L − 1)` polls per four instructions and not a removal at all. It is charged 0 rather than negative, which leaves the band's floor optimistic by exactly that amount, and this sentence is the record of it.

**Step 3 — the disarmed row, which is the one that gates the card.**

> ### **The disarm penalty returns to the round-1 class: −2.276 % or better, i.e. dhry ≥ 7 988 against the same-session reference.** Mechanism: §0.12.1 removes one of the two serially dependent loads, which by the same 214.2-cycle arithmetic is worth ~4.3 cycles = ~2.0 pp. **This is the card-blessing gate** — round 3's card was rolled back to `F294E75A` because 7 853 missed the ≥ 7 988 class by 135 dhrystones, and the pre-declared end-state condition is unchanged: **the card ships round 4's lean image IF and ONLY IF its disarmed row meets the ≥ 7 988 class.**
>
> **Q11's boot row rides beside it: 45 … 47 s**, the clean lineage's own band, tighter than round 3's 46 … 48 because the change removes work from the loop rather than adding it.

**Every band above is quoted against `R` measured in the SAME session, `n` ≥ 5, with §0.5.2's
absolute floor 8 242 quoted beside it and never removed.**

### 0.12.8 What round 4 explicitly does not establish

1. **A verdict.** Coverage is pre-registered below the bar (§0.12.7). The kill line's
   precondition is unmet, the kill line does not fire and is not cleared, and no row taken this
   round may be read as pricing the engine.
2. **That the disarm fix works on silicon.** Round 4 takes no metal row. A three-instruction
   gate in an objdump is a *reason to expect* the round-1 class and not a substitute for
   measuring it — §0.11.4's own words about a 352-byte reduction, which then missed.
3. **The memory-touching families on silicon.** B2's memory-source ALU and B3's auto-adjust
   sources emit a helper call, so the guest-memory-free self-test cannot reach them. They are
   harness-proven only, alongside `(An)`/`d16(An)` `MOVE`. **Re-owed**, with M2 as the first
   honest test.
4. **That the churn attribution's *cost* claim survives a wider workload.** 0.157 % of a core
   is a dhrystone number. `IV_FLUSH` and `IV_PFLUSHA` are kernel behaviour, and a
   paging-heavy workload may invalidate far more often — which is M2's question, not this
   round's.
5. **`z3660_warp_run`'s preamble**, unchanged and still the largest row in §0.9.3's table.
6. **Everything §0.8.8, §0.10.7 and §0.11.7 said would not be established**, unchanged.


## 0.13 M1a round 4 — built, and its gates scored (2026-08-24)

**The pre-registration in §0.12 was committed at 07:30:43 +0300 (`6afab03`), before the first
line of round-4 engine code; the implementation commits follow it in this repository's log.**
Every prediction in §0.12 is left byte-untouched above and scored beside itself.

### 0.13.1 What was built

| piece | where |
|---|---|
| **A1** `TST.B/.W/.L Dn` and `TST.W/.L An` — generation one's own frozen scope, never built | `z3660_warp.cpp`, `warp_emit_insn()` |
| **A2** `ASL`/`ASR`/`LSL`/`LSR`/`ROL`/`ROR`, **immediate count only**, `.B/.W/.L`, `Dn` | same, `e_shift_rot()` |
| **B1** `MOVEQ.L #<data>,Dn` | same |
| **B2** the remaining register-destination ALU: memory sources `(An)`/`d16(An)`, and the immediate forms `ADDI`/`SUBI`/`ANDI`/`ORI`/`EORI`/`CMPI` | same, `e_alu_rr()` factored out and shared by three arms |
| **B3** `(An)+` / `−(An)` **sources**, register destination only | same, `e_src_load()` / `e_auto_wb()` / `warp_auto_step()` |
| **C** the arm flag moves into `struct regstruct` | `include/newcpu.h`, `z3660_warp.h` |
| **D** the churn split — `WARP_DISC_EV`, `WARP_DISC_ATC`, `WARP_DISC_CPUSH` (ids 61–63) | `z3660_prof.h/.cpp`, `warp_discard_all()` |
| **E** four new self-test families, eight new mutation arms, the auto-adjust bail corpus, the accept-set discriminator | `z3660_warp.cpp`, `harness_main.cpp`, `docs/warp-corpus.py` |
| **the build-integrity fix** — `-include $(OBJS:.o=.d)` | `Z3660_emu/Makefile` |

### 0.13.2 The gates

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| **G-R4.1** harness | `0 failed`; **+90 … +260**; **same** delta in lean | green, `0 failed`: **3 320 / 3 316 / 3 547** against 3 301 / 3 297 / 3 528 — **+19 / +19 / +19** | green; **count MISSED, low** (as rounds 1–3 all did); "same in lean" **HIT**; prof's further delta **231 both rounds = +0** **HIT** |
| **G-R4.2** accept-set discriminator | engine == model **exactly**; model **18 168** | engine **17 016**, model 18 168, **agree on 64 384 of 65 536**; the entire disagreement is **1 152 opcodes, every one tagged `shift-reg`**, in ONE direction | ***MISSED***, and attributed to the opcode: they are the register-count shift forms, which §0.12.4 A2 promised and the emitter deliberately refuses. **The engine accepts nothing the model refuses**, so the transcription is exact everywhere it is exercised |
| **G-R4.3** sweep and differential | 6 seeded states, **0** divergences, **≥ 100 000** pairs | **102 096 pairs over 17 016 accepted opcodes, 0 divergences** (round 3: 45 696 over 7 616) | **HIT** |
| **G-R4.4** the eight new mutation arms | every one caught, clean when disarmed | **15 of 15 caught**, plus `M-AUTO-ORDER` on four shapes and `M-ALUMEM-EARLY` | **HIT**, with the caveat in §0.13.5 |
| **G-R4.5** lockstep | `WARP_DIVERGE == 0` over **≥ 10⁸** | **0 over 100 000 000 guest instructions, 12 500 000 block executions, 100.0 % retired inside blocks** | **HIT** |
| **G-R4.6** `WARPTEST` | **13** families, F1 still **+0**, a new ARM encoding through the executor | **PASS 13/13, 0 mismatches**; F1 **64/64**, **+0** range misses; **FAIL 13/13 with 12** under `M-PUBRANGE`; the new encodings found a real oracle defect (§0.13.5) | **HIT** |
| **G-R4.7** the disarmed gate, static | **3** instructions, **one** load, **zero** address materialisations, off `r4`; `m68k_run_mmu040` **≤ 2 196 B** | **4** instructions, **2** loads, **zero** address materialisations, off `r4`, **dependency depth 1**; **2 372 B** | shape **HIT** (the mechanism), count **MISSED** by one, size **MISSED, high** — and §0.13.4 is why the size row cannot be read as written |

> **G-R4.7 `corrected at R5`.** The measured column above describes `C62AE6E2`, the image that
> flew. §0.15.2 proves byte-wise that round 4's **source** builds a different run loop —
> **3 instructions, one load, 2 188 B** — so **on round 4's source G-R4.7 is a HIT on every
> clause, including the size bound**. The verdict as written is left standing beside this note.
| **G-R4.8** the armed entry cost | `E` **108 … 120**; armed non-target **13 … 15** | **E ≈ 112**; armed non-target **16** | `E` **HIT**; gate **MISSED, high by 1** — though it is **−1 against the like-for-like control**, which measures 17 |
| **G-R4.9** the churn instrument | `DISC_EV == DOPC_INVAL`; the cause buckets partition `DISC` | built and structurally exact — one increment per `warp_discard_all()`, and the three buckets are mutually exclusive arms of one `if`/`else if` chain | **BUILT; NOT MEASURED.** The identity is a metal reading and there is no card in this round |
| **G-R4.10** images | three rebuilt, CRCs recorded, archive fires; `q2probe` untouched | `BOOT.BIN` **`D7ECBCFF`**, `BOOT-lean.BIN` **`C62AE6E2`**, `BOOT-prof.BIN` **`69FC5996`**; 12 811 748 / 12 811 732 / 12 828 212 B; all three archived CRC-stamped; `BOOT-q2probe.BIN` untouched at **`FCC0CB3F`** | **HIT** |
| **G-R4.11** object equivalence | not gated | 7 899 of 25 157 common function bodies differ — but **25 205 of 25 215 are byte-identical in SIZE (99.96 %)**, and exactly **10** changed size | not gated; **attributed** in §0.13.3 |

### 0.13.3 The object diff, and why 7 899 changed bodies is a layout result

`warp-objdiff.py` reports 7 899 changed bodies, which looks catastrophic and is not. **Adding
one field to `struct regstruct` shifts the displacement of every member after it**, so every
function that touches `regs.s`, `regs.vbr`, `regs.usp` or any later field differs by an
immediate constant while doing exactly the same work. The measurement that says so is the size
census, which needs no judgement:

```
   common .text symbols        25 215
   identical SIZE              25 205   (99.96 %)
   size changed                    10
```

**and all ten are named:**

| function | Δ | why |
|---|---|---|
| `warp_emit_insn` | **+1 232** | the round's actual work — six new families |
| `z3660_warp_selftest` | +164 | F10 – F13 |
| `warp_st_compare` | +100 | the family walk |
| `e_exit`, `e_store_flags`, `e_load_flags` | +24 / +16 / +4 | emitter helpers reshaped by new callers |
| `ExceptionX`, `m68k_reset_newcpu`, `m68k_run_1_ce` | +12 / +12 / +8 | they initialise `regstruct`, which gained a field |
| **`m68k_run_mmu040`** | **−8** | **the round's one deliberate target, and it went down** |

### 0.13.4 THE BUILD-INTEGRITY FINDING — round 3's shipped image is not what its source builds

**This is the round's most consequential discovery and it was found by accident, which is the
only reason to write it down this loudly.**

The disarm fix touched `include/newcpu.h` and nothing else in the run loop's own translation
unit. `make lean` recompiled `z3660_warp.cpp` **alone**, and the resulting image still carried
a run loop reading the OLD gate — provably, because the literal in `m68k_run_mmu040`'s constant
pool still pointed at `z3660_warp_gate` rather than into `regs`.

**The cause: `-MMD -MP` has been in `CFLAGS` since CYBERSTORM C1 and the `.d` files have been
written correctly ever since — and nothing ever `-include`d them.** The dependency data was
generated and thrown away. `newcpu.d` lists `include/newcpu.h` four times; make never read the
file. The comment above `CFLAGS` claims the hazard was fixed; only half of it was.

**What that does to round 3's numbers, measured rather than supposed.** Round-3 source was
checked out and rebuilt **clean**, as a like-for-like control:

| build | `m68k_run_mmu040` | the disarmed gate |
|---|---|---|
| round 3 **as shipped** (`C89953E2`, the image metal measured at −4.056 %) | **2 196 B** | `ldr r3,[pc,#2028]` → `ldr r3,[r3]` → `cmp` → `beq` — **4 instructions, depth 2, argument load BELOW the branch** |
| round 3 **source, clean rebuild** | **2 380 B** | `ldr r3,[pc,#2212]` → `ldr r0,[r4,#88]` → `ldr r3,[r3]` → `cmp` → `beq` — **5 instructions, depth 2** |
| round 3 **source, clean rebuild** — `corrected at R5` | **2 196 B** | `ldr r3,[pc,#2028]` → `ldr r3,[r3]` → `cmp` → `beq` — **4 instructions, depth 2**, i.e. **byte-for-byte the shipped image's gate, down to the literal offset** |
| **round 4, clean rebuild** | **2 372 B** | `ldr r3,[r4,#96]` → `ldr r0,[r4,#88]` → `cmp` → `beq` — **4 instructions, depth 1, ZERO address materialisations** |

> ### **§0.11.2's `2 196 B` and §0.11.4's whole size table describe a build that cannot be reproduced from round 3's own source.** They are not wrong about the image that flew; they are wrong about what the source produces. The honest like-for-like comparison is **2 380 → 2 372, i.e. −8 B**, and against that control round 4's gate is **one instruction shorter with half the dependency depth**.

> **`corrected at R5` — THIS PARAGRAPH IS REFUTED AND THE FINDING MOVES TO ROUND 4.** §0.15.2
> rebuilt round 3's source clean, in a fresh worktree, on the same host and toolchain, and got
> **2 196 B with the shipped image's exact gate**. Round 3's source reproduces its image.
> **Round 4's does not** — proven on the archived artifact by a byte search for the two gate
> encodings. §0.11.2's and §0.11.4's numbers stand; this section's attribution does not.

**§0.12.1's named mechanism survives this and is not weakened by it.** The serial
dependent-load pair with no slack is exactly what the *shipped* `C89953E2` gate contains, and
`C89953E2` is the image that measured −4.056 %. What the control adds is that round 3's source
would have scheduled it slightly better — which makes the shipped image's penalty a
*combination* of the mechanism and a stale-object build, and means **the −4.056 % baseline is
not a clean measurement of round 3's source at all**. The next metal session compares round 4
against a fresh `R`, and that is the only comparison that was ever going to be trustworthy.

**The fix is one line and it is in this round**: `-include $(OBJS:.o=.d)`.

### 0.13.5 Four things worth recording that were not gates

- **THE HOST ARM EXECUTOR NEVER IMPLEMENTED THE SHIFTER CARRY-OUT, and round 4's shift family
  is the first emitter that reads it.** On ARM a *logical* data-processing instruction with `S`
  set takes C from the barrel shifter, not the ALU. Rounds 1–3 emitted `movs rd, rm lsl #k`
  only for N and Z and masked C away at every commit, so `warp_host_exec()` could leave C
  untouched and nothing noticed. The differential caught it on its first run — **2 207 pairs,
  every one in `0xE0xx`, every one `ccr 9 vs 8`** — and it was the ORACLE that was wrong, not
  the emitter. This is why §0.12.5 made growing the executor a gate rather than a detail: a
  second implementation of ARM that models one flag wrongly turns every future divergence in
  that flag into a false alarm, or hides a real one.
- **Three mutation arms could not bite until the corpus was widened, and that is a finding
  about mutation testing rather than about the engine.** `int d0` could not express
  `0x80000000`, so a carry-out mutation on a left shift had nothing to lose — `warp_seed()`'s
  own D0 values never reach bit 31 for these keys. And A7 seeds to the stack page, which the
  helper does not serve, so the byte-step mutation bailed in both arms and compared equal.
  **A mutation arm that cannot be caught at all is worse than none: it reports CAUGHT-shaped
  silence.** The overrides are now 64-bit and there is one for A7.
- **The auto-adjust bail corpus was written wrong first, and the wrong version passed nothing.**
  It compared the armed run against the PRE-STATE, on the theory that a declining block changes
  nothing. A run-loop pass whose block bails with nothing retired **falls through to the
  interpreter inside the same pass**, so the interpreter takes the fault and the machine is
  legitimately not what it was. What the bail contract claims is not "nothing happened" but
  "the interpreter saw exactly what it would have seen", and only an armed-vs-disarmed pair can
  say that.
- **`warp_boot()` clears the mutant**, so a mutation set before it is silently disarmed. Two
  round-4 arms were written that way and reported NOT CAUGHT until the calls were reordered.

### 0.13.6 What round 4 leaves owed

1. **Every metal row.** Round 4 takes none. Coverage, block length, the decline rate, the
   armed band, the disarm penalty and G-R4.9's identity are all pre-registered in §0.12.7 and
   all unmeasured.
2. **The register-count shift forms** (G-R4.2's 1 152). Either build them or amend the model —
   and the model is part of a pre-registration, so it is the emitter that moves.
3. **The memory-touching families on silicon**, now larger: B2's memory-source ALU and B3's
   auto-adjust sources join `(An)`/`d16(An)` `MOVE` as harness-proven only, because the
   self-test refuses any block containing a `blx`. §0.10.7 item 4, **re-owed and grown**.
4. **`z3660_warp_run`'s preamble**, unchanged at 236 bytes, still the largest row in §0.9.3's
   table. **Re-owed** for a round that measures the armed path.
5. **Whether any earlier image was mis-built.** §0.13.4 proves round 3's was. Rounds 1 and 2
   are not re-examined here and their objdump numbers carry the same risk.
6. **Everything §0.8.8, §0.10.7, §0.11.7 and §0.12.8 said would not be established**, unchanged.

## 0.14 M1a round 5 — pre-registered, before the first line of round-5 code (2026-08-24)

**This section is written before any round-5 engine code and is scored as written.** §3.4's
two rules govern it exactly as they governed §0.6, §0.8, §0.10 and §0.12: the pre-registration
commit precedes the implementation commits, provably, by commit timestamp in this repository;
and a pre-registration that gets rewritten is not one, so every number below is left
byte-untouched and scored beside itself.

**Round 5 is routed by the same clause round 4 was.** §0.5.2's decision table sends a
below-band coverage wave to the next tranche of §0.5.1's frozen extension order, and §0.12.7
pre-registered — before round 4's row existed — that *"this tranche does NOT reach 60 %, and
M1a tranche 2 is predicted now rather than after a disappointing number."* Coverage measured
**19.10 %**. The kill line is untouched, still armed, still unfired.

### 0.14.0 THE METHOD THAT IS BEING RETIRED, AND THE ONE THAT REPLACES IT

**Two named mechanisms for the disarm-penalty residual have now been refuted on metal, and
both were produced the same way: read the shipping objdump, name a shape, predict a number,
measure it, miss.**

| round | named mechanism | where it was named | metal verdict |
|---|---|---|---|
| 3 | **L1I footprint** — the disarmed loop got bigger | §0.10.6 | ***REFUTED*** (§0.11.4): round 3's loop was 128 B **smaller** than round 1's and cost **1.78×** as much |
| 4 | **dependency depth** — a serial load-to-load pair with no slack | §0.12.1 | ***REFUTED*** (round-4 metal): the gate landed at depth 1 with zero address materialisations, exactly as designed, and the penalty got **0.2 pp worse** |

> ### **THE METHOD IS RETIRED FOR THIS PROBLEM.** Guessing a mechanism from a disassembly and then buying a metal row to test it has been tried twice, has cost two flashes and two card rollbacks, and has produced two refutations and no attribution. A third guess would be the same experiment a third time. **Round 5 does not name a mechanism at all. It builds an instrument that bisects the cost, and it pre-registers the decision rule that reads the instrument** (§0.14.1).

**What survives the two refutations, and it is worth stating because it constrains round 5.**
Both refuted mechanisms were **local to the gate** — a property of the four or five ARM
instructions the run loop gained. Two independent local mechanisms have failed. **The one
change round 4 made that is not local is `struct regstruct` gaining a field**, which
`warp-objdiff.py` measured as **7 899 changed function bodies** across the image (§0.13.3).
That is the shape of a candidate the previous two were not, and §0.14.1 pre-registers it as
the favourite — but as a *variant to be measured*, not as a mechanism to be announced.

**A second, quieter thing the two refutations cost, and it is fixed here rather than
absorbed.** Every disarm penalty this campaign has quoted is the difference between two
separately-built images. Nothing has ever measured how much of that difference is simply *two
builds*. Round 5 carries a **negative control** — a variant whose ablated layer is provably
off the disarmed path — and whatever it measures is the round's **noise floor**. No edge
smaller than the floor may be read as a mechanism, by anyone, afterwards.

### 0.14.1 THE DISARM-RESIDUAL ABLATION KIT — the instrument, its variants, and its decision rule

**The disarmed cost being decomposed is round 4's measured −4.256 %** — `C62AE6E2`'s disarmed
dhrystone against the same-session clean-interpreter `R`. The question the kit answers is not
*"what shape is the gate"* but *"which of the layers WARP adds to a disarmed image carries the
cost"*, and it answers it by **removing one layer at a time and re-measuring**.

**Four candidate layers, enumerated from the code rather than from a theory:**

| layer | what it is, concretely | why it could cost |
|---|---|---|
| **L-REG** | `int warp_on` inside `struct regstruct` (`include/newcpu.h`) | **displacement.** Every member after it moves, so every function that touches `regs.s`, `regs.vbr`, `regs.usp` or any later field re-encodes its offsets. **7 899 bodies changed** (§0.13.3). Some offsets can cross an addressing-mode boundary; the whole image's I-cache layout moves |
| **L-HOOK** | the run loop's `if (z3660_warp_on) … z3660_warp_step()` and the block-consumes-the-pass branch (`newcpu.cpp`) | **the hot path.** Four ARM instructions on every disarmed dispatch, plus a restructured loop body around them |
| **L-ATT** | the split-pair arm attestation and the lean-side stat plumbing (`z3660_warp.cpp`) | **presence only.** It lives inside `z3660_warp_run`, which a disarmed image never calls. **This is the negative control** |
| **L-WTST** | `z3660_warp_selftest()`, its 13 family corpora and the ARM word executor, linked into every shipping lean image | **cold code and link layout.** Never executed until its verb arrives; it still occupies address space and moves everything after it |

**Six images, built from ONE source tree by `-D` flags, so the matrix is reproducible and no
variant is a different checkout.** Each build gets its own build directory and its own `.bif`,
because a `-D` is not a tracked dependency and this repository has already been bitten by a
variant switch leaving another variant's objects in place.

| variant | flag | removes | edge it reads |
|---|---|---|---|
| **A0** | *(none — the shipping lean)* | nothing; all four layers present | the control; its penalty is the number being decomposed |
| **A1** | `Z3660_WARP_ABLATE=1` | **L-WTST** | A0→A1 = cold code + link layout |
| **A2** | `Z3660_WARP_ABLATE=2` | **L-ATT** | A0→A2 = **THE NOISE FLOOR** |
| **A3** | `Z3660_WARP_ABLATE=3` | **L-HOOK** | A0→A3 = the hot-path gate |
| **A4** | `Z3660_WARP_ABLATE=4` | **L-HOOK + L-REG** | **A3→A4 = struct displacement, ALONE** |
| **A5** | `Z3660_WARP_ABLATE=5` | the engine entirely (`z3660_warp.cpp` filtered out) | A0→A5 = **closure** |

> ### **A3 AND A4 ARE DYNAMICALLY IDENTICAL AND DIFFER ONLY IN `struct regstruct`'S LAYOUT.** With `L-HOOK` removed nothing on the disarmed path reads the flag at all, so the only difference between the two images is where every field after `warp_on` sits. **That edge is the cleanest measurement of layout displacement this campaign can construct**, and it is the reason `L-REG` is not offered as an independent removal: taking the field out while leaving the hook in would put the flag back in a standalone global, which is round 3's arrangement — a different change, not an ablation.

**THE PRE-REGISTERED FAVOURITE, STATED BEFORE ANY CRC EXISTS: `L-REG`, the struct
displacement, read on the A3→A4 edge.** The reasoning is not a new objdump — it is that the
two mechanisms already refuted were both *local to the gate*, and `L-REG` is the only layer
round 4 added that is *global to the image*. It is named here so that it cannot be adopted
after a row, and so that it can be **wrong in public** the way §0.10.6's and §0.12.1's were.

**Predicted signs and magnitudes, each stated per edge:**

| edge | predicted recovery of the −4.256 % | direction |
|---|---|---|
| A0→A1 (`L-WTST`) | **0.0 – 0.5 pp** | ~0. Cold code that never executes on a disarmed pass |
| A0→A2 (`L-ATT`) | **0.0 – 0.3 pp** | **0 by construction.** Whatever it measures IS the floor |
| A0→A3 (`L-HOOK`) | **0.6 – 1.4 pp** | +. Four instructions at §0.9.3's 214.2 cy/guest-insn, priced the way round 1's five were |
| **A3→A4 (`L-REG`)** | **1.8 – 3.4 pp** | **+, and this is the favourite** |
| A0→A5 (whole engine) | **3.9 – 4.6 pp**, i.e. A5's own penalty within the floor of **0** | +. The closure |

**THE DECISION RULE, pre-registered:**

```
   1. THE FLOOR is |penalty(A0) - penalty(A2)|.  No edge smaller than the floor is
      read as a mechanism, in this round or any later one.

   2. A single edge that recovers >= HALF the measured penalty NAMES the mechanism.
      Round 4 measured -4.256 %, so the bar is 2.128 pp.

   3. If NO edge recovers >= half, the cost is DISTRIBUTED OR EMERGENT.  That is a
      FINDING and the doc says so in those words -- it retires "one layer carries it"
      as a hypothesis, and the method that would find it is a different one again.

   4. If A5's own penalty is NOT within the floor of zero, the four named layers do
      not SPAN the cost.  Something outside all four carries it, and that is a bigger
      finding than any of the four.

   5. ADDITIVITY GATE:
         (A0-A1) + (A0-A2) + (A0-A3) + (A3-A4)  ==  (A0-A5)   +/- 2 x floor
      A miss here says the layers INTERACT, which is itself the attribution.
```

**The flight order, and the drop order, both fixed before the session.** The metal session
runs A0, A2, A3, A4, A5, A1 **in that order**: the control, then the floor, then the two edges
that carry the pre-registered prediction, then closure, then the cheapest hypothesis last. **If
the session runs short, images are dropped from the END of that list** — A1 first, then A5.
Saying so now is what stops a short session from looking like a choice made after a number.

**What the kit costs and what it does not.** Six flashes plus their boots and dhrystone arms;
the standing `/etc/haltsys`-before-deploy discipline and both CRC gates apply to every one of
them. **No variant except A0 is ever a candidate to ship**, and A5 in particular is a
measurement build in exactly the sense §0.5.4's elided-state-sync build was — it is removed
from nothing, because it is a flag rather than a code path, but it must never be left on a
card. The end-state rule is unchanged and unrelated to the kit: **the card ships round 5's A0
lean image if and only if A0's own disarmed row meets the ≥ 7 988 class** (§0.12.7 step 3,
carried forward verbatim), and otherwise goes back to `F294E75A`.

### 0.14.2 THE BAIL-ATTRIBUTION COUNTER — and a prediction the existing counters already constrain

**Round 4 opened a 7.5 pp gap between two rates that round 3 had 0.4 pp apart:** bail rate
`BAIL/BLK` **68.486 %** against decline rate `DECLINE/COMPILE` **60.958 %**. §0.13's verdict
named a mechanism — *"round 4's auto-adjust `(An)+`/`−(An)` sources compile successfully but
bail at run time on the memory helper"* — and called it **not proven**. It is not proven, and
the counters that exist already say something about it.

**The arithmetic, off round 4's own dump, and it is a measured fact rather than a model:**

```
   WARP_BLK   13 446 848      WARP_ENTER  4 238 096      WARP_BAIL  9 209 263
   BLK - ENTER = 9 208 752   <- block executions that retired NOTHING
   BAIL - (BLK - ENTER) = 511 <- block executions that retired SOME and then bailed
```

> ### **99.994 % of round 4's bails retired nothing at all.** `WARP_ENTER` is incremented only when a block retired ≥ 1 instruction, so `BLK − ENTER` is exactly the zero-retirement population, and `WARP_BAIL` exceeds it by **511** in 9.2 million. **Whatever the gap is, it is not blocks that got part-way through and then met a memory helper** — that population is five hundred executions.

**So the zero-retirement population is the whole question, and it has exactly two members:** a
**cached decline stub** (`len == 0`, the engine refused this head and remembers) and a **real
block that declined at its very first faultable site**. The existing counters cannot separate
them; one bit at the existing site can.

**The counters, append-only, no id moves, no field changes shape, dump version stays 2,
`Z3660_PROF_C_COUNT` 64 → 69:**

| id | counter | site | topology check |
|---|---|---|---|
| 64 | `WARP_BAIL_STUB` | `z3660_warp_run`'s bail arm, `s->len[way] == 0` | a cached decline stub executed. **Uncovered opcode at run time, in its cached form.** Costs no emitted code |
| 65 | `WARP_BAIL_MEM` | same arm, `len > 0` | a real block bailed. **Every bail from a real block is a memory-helper decline, by construction:** `e_bail_if()` is emitted at exactly two kinds of site — after `blx R_RD` and after `blx R_WR` — and nowhere else |
| 66 | `WARP_BAIL_FENCE` | inside `z3660_warp_rd`/`_wr`, the `warp_fence()` refusal | the sub-cause: misaligned, or the access crosses a guest page |
| 67 | `WARP_BAIL_VERDICT` | same helpers, the `z3660_dfast_*` refusal | the sub-cause: `SLOW` verdict, ATC miss, or a non-direct bank |
| 68 | `WARP_BAIL_OTHER` | the bail arm, neither of the above | **the residual, and it is pre-registered at exactly 0.** A bail that is neither a stub nor a real block cannot exist |

**Where the "fault" bucket went, stated rather than omitted.** The dispatch's fourth cause has
no site in this engine: **generated code never takes a fault**, because every faultable access
is a helper call that *declines* first and the block leaves through its bail arm. That is the
bail contract, and the instrument that watches it is §0.5.5 G2's existing
`FAULTS(on) == FAULTS(off) ± 1`, not a new counter. **If the contract ever leaked, the leak
would arrive as a fault the interpreter did not take — which G2 catches — and not as a bail.**
`WARP_BAIL_OTHER` is the tripwire for the other direction.

**Two structural identities, and both are gates:**

```
   WARP_BAIL      ==  WARP_BAIL_STUB + WARP_BAIL_MEM + WARP_BAIL_OTHER     exact
   WARP_BAIL_MEM  ==  WARP_BAIL_FENCE + WARP_BAIL_VERDICT                  exact
   WARP_BAIL_OTHER == 0                                                    exact
```

**The second one is the load-bearing one** and it is a claim about the emitter, not about
arithmetic: one helper decline produces one bail, because the `CMP`/`e_bail_if` pair follows
every `blx` immediately. If it does not close, either a helper declined without a bail
following it or a block bailed without a helper declining — and either is a defect in the bail
contract, found by an identity rather than by a soak.

> ### **THE PREDICTION, MADE BEFORE THE SPLIT EXISTS: THE GAP IS THE DECLINE STUB, AND THE SESSION'S NAMED CANDIDATE IS PREDICTED TO BE REFUTED.** `WARP_BAIL_STUB / WARP_BAIL` is pre-registered at **0.80 – 0.97, point 0.88**; `WARP_BAIL_MEM / WARP_BAIL` at **0.03 – 0.20, point 0.12**. **The rule that makes it falsifiable: if `WARP_BAIL_MEM / WARP_BAIL` ≥ 0.30, the memory-helper candidate is CONFIRMED and this prediction is marked MISSED beside itself.**
>
> The derivation, so the number is not a feeling: if a cached stub is entered about as often as a real block — round 3 measured **349.1** executions per stub against **342.0** per real block, 2.1 % apart (§0.12.0) — then the stub share of `WARP_BLK` is approximately the decline rate itself, and the bail rate exceeds it by exactly the real blocks that bailed at their first site. Round 4's 7.53 pp gap over a 68.49 % bail rate is **11 %** of the bails, which is the point estimate. **The alternative that the band's width admits is that stubs are entered *more* often than real blocks** — a short refused head arrives more often than a long compiled one — in which case the stub share exceeds the decline rate and the memory-helper term is smaller still.

**The sub-split's own prediction:** `WARP_BAIL_FENCE / WARP_BAIL_MEM` ≤ **0.10, point 0.02**.
The guest's own compiler aligns its accesses and a byte access cannot be misaligned at all, so
the fence should fire almost only on a long that straddles a 4 K boundary.

**What was priced and declined: a per-emitter-arm attribution.** Splitting `WARP_BAIL_MEM` by
*which family* bailed — auto-adjust source against plain `(An)`/`d16(An)` source — needs a
compile-time cause constant stored at every bail arm, which is two emitted ARM instructions per
bail site in every block. That charges the instrument into the armed path this round is not
measuring, and §0.5.6's own rule about not bracketing the compile path applies in the same
form. **It is declined here, before the number, so that it cannot be adopted afterwards to
rescue a reading.**

### 0.14.3 M1a TRANCHE 2 — the accept set, pre-registered, and what it is predicted to buy

**§0.12.3 named the next three levers before round 4's row, exactly so this section could not
look like a rescue:** *"the next three levers are `LEA`, `CLR` and `EXT`/`EXTB` — all three in
the M0 census's fully register-only family list … They are tranche 2's to pre-register, on
their own commit, and this paragraph is the record that the temptation was priced and
declined."* This is that commit. Round 5 builds those three, and closes G-R4.2's owed 1 152.

| | family | scope, and every exclusion named |
|---|---|---|
| **T1** | **`LEA`** | destination `An`, modes **`(An)`, `d16(An)`, `(xxx).W`, `(xxx).L`, `d16(PC)`**. No memory is read — the effective address **is** the operand — so every one of these is a pure register computation, and `d16(PC)` resolves to a **compile-time constant** |
| **T2** | **`CLR`** | **`Dn` destination only**, `.B`/`.W`/`.L` |
| **T3** | **`EXT.W` / `EXT.L` / `EXTB.L`** | `Dn` only. 24 opcodes |
| **T4** | the **register-count** shift/rotate forms | `ASL`/`ASR`/`LSL`/`LSR`/`ROL`/`ROR`, count in `Dn`, `.B`/`.W`/`.L`, `Dn` destination — **G-R4.2's 1 152**. `ROXL`/`ROXR` stay excluded for round 4's reason, unchanged |

**T1's one refusal, named before the corpus is counted, because it is the expensive one.**
`d8(An,Xn)` and `d8(PC,Xn)` — the **indexed** modes — are **refused**. The reason is not the
brief extension word's arithmetic, which is cheap; it is that **bit 8 of that extension word
selects the 68020 FULL format, and a full-format extension can dereference memory**
(memory-indirect addressing). Admitting the brief form means admitting a decode surface whose
immediate neighbour reaches guest memory from a family that otherwise never does, and getting
the discrimination wrong ships a silent memory read inside a block that declares itself
register-only. **That is its own pre-registration and it is not smuggled in here.** §0.14.3's
corpus table prices the refusal: **6 of the 240 hot instructions**, the largest single thing
tranche 2 leaves behind.

**T2's one refusal is the round's whole out-of-scope statement in miniature.** `CLR` to
`(An)`, `d16(An)`, `(An)+`, `−(An)` or an absolute address is a **memory-DESTINATION** family,
and round 5 declares that entire class out of scope with a statement (§0.14.3's closing
paragraphs) rather than a build.

**T3's encoding trap, recorded because the model walked into it and the engine would have.**
`EXTB.L Dn` is `0100 1001 1100 0rrr`, which matches `LEA`'s `0100 rrr 111 eeeeee` pattern
exactly with `rrr = 100` and an EA mode of `000` — an encoding the 68k reassigns to `EXTB.L`
precisely *because* `LEA Dn,An` is illegal. **`EXT` must be tested before `LEA`**, in the
emitter and in `docs/warp-corpus.py` alike, or every `EXTB.L` is classified as a refused `LEA`.

**T4 is the repair §0.13.6 item 2 specified, in the direction it specified.** The gen-4 model
accepted the register-count forms and the gen-4 engine refused them; the gate scored MISSED
over exactly those 1 152 opcodes. *"The model is part of a pre-registration, so it is the
emitter that moves."* **`docs/warp-corpus.py`'s gen-4 line is therefore left unamended and
still reads 18 168** — editing it to 17 016 would have made a scored gate pass retroactively,
which is the one repair this discipline forbids — and round 5 builds the forms instead.

**How T4 is emitted, and the one uniform device that makes 18 shapes tractable.** ARM's own
register-specified shifts already implement the 68k's rules for every count ≥ 1, at every
size, once the operand is top-aligned for the left forms and sign/zero-extended for the right
ones: `MOVS rd, rm, LSL rs` takes C from bit `32−rs` for `1 ≤ rs ≤ 32` and clears it above,
which is the 68k's "last bit shifted out" verbatim. **The 68k and ARM disagree in exactly one
place — a count of zero**, where the 68k clears C, leaves X alone and takes N/Z from the
unshifted operand at its own width, and ARM leaves the flags where they were. So every
register-count shape is the immediate shape's body plus one branch-free tail:

```
   movs  r3, <operand>, lsl #s     ; N,Z of the OPERAND at its own width
   mrs   rX  /  and rX, #0xC0000000 ; -> the count==0 flag word: N,Z kept, C and V cleared
   cmp   r1, #0                     ; r1 = the count, already masked to 0..63
   moveq r12, rX                    ; count == 0 -> use it
   strne r3x, [ctx, #XFLG]          ; X is written ONLY when the count is non-zero
```

**`ASL` needs one thing more and it is stated because it is the only variable-width test in
the round.** Its overflow rule is the interpreter's — *the top `cnt+1` bits are neither all set
nor all clear* — which at a runtime count needs `t >> (31−c)` with `t = v ^ (v asr #31)`, and
`31−c` is meaningless for `c > 31`. **Clamping `c` to 31 before the `rsb` is exactly correct**
rather than approximately so: at `c ≥ 31` the rule degenerates to `t ≠ 0`, which is what a
shift by zero tests.

**The seeds must reach the count edges or the differential is vacuous there, and this is
§0.13.5's own lesson applied forward.** `warp_seed()` derives the data registers from a key, so
the all-65 536 sweep reaches an arbitrary count and not a chosen one. **Round 5 adds a targeted
arm** that drives every T4 shape across counts `{0, 1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64,
65, 255}` — the zero case, each size's width and width+1, the 32 and >32 boundaries, and the
mod-64 wrap — against the interpreter. *"A mutation arm that cannot be caught at all is worse
than none"*, and so is a differential that cannot reach the case the emitter was written for.

**THE CORPUS, re-run on the same disassembly and the same script** (`docs/warp-corpus.py`,
gen 5; the dhrystone binary every WARP row has been measured on, `sum -r 52076`, 8 135 bytes):

| corpus | insns | R4 accepts | **R5 accepts** | R4 mean run | **R5 mean run** |
|---|---|---|---|---|---|
| whole `.text` | 632 | 28.48 % | **31.80 %** | 2.500 | **2.544** |
| hot set (`main` + callees) | 609 | 28.41 % | **31.69 %** | 2.662 | **2.644** |
| **`Proc_`/`Func_` callees only** | **240** | **43.75 %** | **47.92 %** | **2.763** | **3.026** |

```
   model accept over all 65536 opcode words:  R4 18 168  ->  R5 18 368   (+200)
     LEA 152  +  CLR 24  +  EXT 24
   and the ENGINE moves 17 016 -> 18 368  (+1 352):  the same 200, plus G-R4.2's 1 152
```

**The tiers disagree more this round than last, and that is reported rather than smoothed.**
Round 4's callees run length was 2.763 against the hot set's 2.662 — 3.8 % apart. Round 5's is
**3.026 against 2.644 — 14 % apart**, because `main`'s once-only printf banner is 102 `pea`
pushes that tranche 2 does not touch while the callees gain `CLR`/`EXT` runs that lengthen
theirs. §0.12.3's own words are the rule: *"the gap between the tiers is itself the measure of
how far to trust it"*, and the band below is widened for it rather than the point estimate
being moved.

### 0.14.4 `WARPTEST` grows to 17 families — and the `blx` refusal gets the statement it is owed

**Every tranche-2 family is register-only, so for the first time since round 2 an entire
tranche is coverable on target.** `T1`'s accepted `LEA` modes read no memory, `CLR Dn`,
`EXT`/`EXTB` and the register-count shifts touch none, so all four go into the self-test:

| F14 | `LEA` | F15 | `CLR` | F16 | `EXT`/`EXTB` | F17 | register-count shifts |

**THE OWED STATEMENT (§0.10.7 item 4, §0.12.8 item 3, §0.13.6 item 3 — re-owed three times and
answered here as a design note).** `z3660_warp_selftest()` refuses any block containing a `blx`
(§0.11.1), so `MOVE` with `(An)`/`d16(An)`, round 4's memory-source ALU and round 4's
auto-adjust sources **have never executed on silicon under a test that checks them**. They are
harness-proven only. **What would make them self-testable, stated so it can be built or
rejected on its merits rather than deferred a fourth time:**

1. **The refusal exists for one reason and it is a good one.** The self-test runs on a **live
   guest** at any moment, and it must touch **no guest memory** — neither instruction words
   (the compiler is fed a core1-resident opcode array) nor operands. A `blx` in a compiled
   block is a call to `z3660_warp_rd`/`_wr`, which reads or writes **the guest's address
   space**. A self-test that let one through would corrupt a running AMIX.
2. **The smallest honest fix is a scratch page the helpers may serve, and it is not small.**
   The helpers resolve through `z3660_dfast_read`/`_write`, which consult the guest's ATC and
   `mem_banks[]`. Serving a core1-resident scratch buffer means either a synthetic bank
   (which changes the resolver the test is supposed to be testing) or a real guest page
   borrowed for the duration (which is exactly the "touch no guest memory" property being
   given up).
3. **The version that keeps the property is a HELPER SUBSTITUTION, not a memory window.** The
   self-test already hands each block its two helper pointers at entry (`R_RD`, `R_WR` are
   arguments, not globals — §0.8.5's pointer-width-independence made them so). **A self-test
   arm can therefore pass its own pair of helpers** — ones that serve a core1-resident 64-byte
   array and count their calls — and the block under test executes the real emitted `blx`
   sequence, the real bail arm and the real fence, against memory that is not the guest's.
   **What it would prove:** the emitted call sequence, the argument marshalling, the bail arm's
   state, the auto-adjust write-back's *position*. **What it would still not prove:** the
   resolver's own verdicts, which is the half `WARPTEST` was never going to reach and which M2
   is the first honest test of.
4. **It is not built in round 5**, and the reason is scope rather than difficulty: it is a
   fifth deliverable in a round that already carries four, and it is the *armed* path while
   round 5's one wall-clock question is a *disarmed* number. **Re-owed, with the design above
   as the specification** — the next round that touches the self-test builds it or says why not.

### 0.14.5 THE ROUNDS-1 AND -2 OBJDUMP RE-DERIVATION — cheap, and owed by §0.13.6 item 5

§0.13.4 proved round 3's shipped image is not what round 3's source builds, and §0.13.6 item 5
left the same question open for rounds 1 and 2: *"their objdump numbers carry the same risk."*
The `.d` fix landed in round 4, so the re-derivation is now a checkout and a clean build.

**Method, identical to §0.13.4's:** check out each round's scored commit, build `lean` **from
scratch** (no stale objects, so the absent `-include` cannot bite), and read `m68k_run_mmu040`'s
size and its disarmed gate off the ELF.

| round | scored at | claimed in the doc | to be re-derived |
|---|---|---|---|
| 1 | `b16967d` | `m68k_run_mmu040` **2 324 B**, gate **5 insns** (`movw`,`movt`,`ldr`,test,`b`) — §0.7.3, §0.12.1 | size and gate shape from a clean build |
| 2 | `a392fbe` | **2 548 B** — §0.9.3, §0.12.1 | same |
| 3 | `1adb64c` | 2 196 B as shipped; **2 380 B** clean (§0.13.4) | re-confirmed, as the control that the method reproduces |

**What is scored:** whether each clean rebuild reproduces the size the doc claims. **Every
claim that moves is marked `corrected at R5` beside itself and the original is left
byte-untouched**, which is this document's house style and has been applied to itself in
§0.4.2, §0.11.3 and §0.13.4 already. **A round whose number does not move is worth exactly as
much as one whose does**, and both outcomes are recorded.

**What it explicitly does NOT do:** re-open any metal row. The images that flew are the images
that flew and their measured penalties stand. This corrects what the *source* produces, which
is what a later round has to build on.

### 0.14.6 What round 5 builds

| A | **the ablation kit** — `Z3660_WARP_ABLATE=1…5`, six build directories, six `.bif`s, six images (§0.14.1) |
| B | **the bail-attribution split** — ids 64–68, `Z3660_PROF_C_COUNT` 64 → 69, dump version stays 2 (§0.14.2) |
| C | **M1a tranche 2** — `LEA` (non-indexed), `CLR Dn`, `EXT`/`EXTB`, and the 1 152 register-count shift forms (§0.14.3) |
| D | **`WARPTEST` F14–F17**, the targeted shift-count differential arm, and new mutation arms one per new family and one per new decision (§0.14.7 G-R5.4) |
| E | **the rounds-1/2 re-derivation** (§0.14.5) |

**Explicitly NOT built, each with a reason rather than an omission:** the indexed `LEA` modes
(§0.14.3 T1); every memory-**destination** family (§0.14.8); `(xxx).L` `MOVE` (§0.5.1 item 4,
still M4's); `ROXL`/`ROXR` (round 4's reason, unchanged); the helper-substitution self-test arm
(§0.14.4 item 4, **re-owed with a specification**); the per-emitter-arm bail attribution
(§0.14.2, priced and declined); `z3660_warp_run`'s 33-instruction preamble (**re-owed** for a
round that measures the armed path); the open-coded memory fast path (§0.9.6 item 3); Q5
(§0.12.0, deader than before); Q6 (dead, §0.11).

### 0.14.7 The round-5 gates, with their predictions

| gate | pre-registered |
|---|---|
| **G-R5.1** harness green, all three variants | `0 failed`; count **+120 … +400** over 3 320 / 3 316 / 3 547, and the **same** delta in lean |
| **G-R5.2** the accept-set discriminator | the engine's accept set over all 65 536 opcode words equals `docs/warp-corpus.py` gen 5 **exactly**; model **18 368**, engine **18 368**, symmetric difference **0**. **G-R4.2's owed 1 152 closes in this gate or not at all** |
| **G-R5.3** the sweep and the differential | every accepted opcode both ways from six seeded states; **0** divergences; pair count **≥ 108 000** |
| **G-R5.4** the new mutation arms | every one **caught**, every one **clean** when disarmed; and every arm demonstrated to be *catchable* — no CAUGHT-shaped silence (§0.13.5) |
| **G-R5.5** the shift-count differential | every T4 shape × the 15 pre-registered counts, armed against disarmed, **0** divergences |
| **G-R5.6** lockstep | `WARP_DIVERGE == 0` over **≥ 10⁸** retired guest instructions |
| **G-R5.7** `WARPTEST` | **17** families, sentinels = results, F1's 64/64 alignment sweep still **+0** range misses, and the new families exercise at least one newly emitted ARM encoding through the word executor |
| **G-R5.8** the bail-split identities | `BAIL == BAIL_STUB + BAIL_MEM + BAIL_OTHER`, `BAIL_MEM == BAIL_FENCE + BAIL_VERDICT`, `BAIL_OTHER == 0` — all exact, in the harness before any metal |
| **G-R5.9** the ablation matrix, static | six images build from one tree; each variant's removed layer **absent from its objdump** (`z3660_warp_selftest` gone from A1, the run-loop gate gone from A3/A4/A5, `regs.warp_on`'s displacement changed between A3 and A4, `z3660_warp_step` absent from A5); every CRC recorded in the manifest |
| **G-R5.10** object equivalence, ablation flags OFF | **the shipping lean is the all-layers build.** A0 built with `Z3660_WARP_ABLATE` unset must be byte-identical in the `.text` symbol census to A0 built without the flag existing — i.e. the flag's OFF path adds nothing. Gated: **0** changed function bodies |
| **G-R5.11** images | the shipping trio plus five ablation variants, CRCs recorded, archive fires; `q2probe` untouched at `FCC0CB3F` |
| **G-R5.12** the rounds-1/2 re-derivation | both clean builds complete and their sizes recorded, whether or not they match the doc |

### 0.14.8 The round-5 metal bands, derived before the row

**Derived by §0.12.3/§0.12.7's method, on round 4's own measured row as the calibration
base** — the method that landed coverage within 0.9 pp and the decline rate within 0.76 pp of
their point estimates, which is the only reason it is used again.

```
   R4 measured:  BLK 13 446 848   ENTER 4 238 096   INSNS 12 193 164   I_true 63 845 050
   productive-entry fraction  R4  4238096/13446848 = 31.517 %
                              R4 static (callees) 43.75 %  ->  measured/static = 0.7204
                              R5 static 47.92 %  x 0.7204  =  34.52 %
   retired per productive entry  R4  12193164/4238096 = 2.8770
                              scaled by the run-length ratio 3.026/2.763 = 1.0952
                              R5  =  2.8770 x 1.0952  =  3.151
   WARP_BLK held at R4's value -- an ASSUMPTION with a direction: block ENTRIES are driven by
   the workload's control-transfer arrivals, not by the engine, and R3->R4 moved it +3.3 %
   (13 016 813 -> 13 446 848) across six new families.  Charged 1.00.
   WARP_INSNS(R5) = 13 446 848 x 0.3452 x 3.151 = 14.63 M  ->  c = 14.63/63.85 = 22.9 %
```

| row | pre-registered band | point estimate |
|---|---|---|
| **coverage `c`** | **0.20 – 0.28** | **0.229** |
| **`WARP_INSNS`/`WARP_BLK`** | **0.92 – 1.32** | **1.088** |
| **compiled length `CINSNS`/(`COMPILE`−`DECLINE`)** | **2.30 – 3.10** | **2.651** |
| **decline rate `DECLINE`/`COMPILE`** | **51 – 62 %** | **56.8 %** |
| **bail rate `BAIL`/`BLK`** | **58 – 70 %** | **64.3 %** |
| **`WARP_DISC_EV` per M retired insns** | **75 – 110** | **90.6** |
| **`WARP_BAIL_STUB`/`WARP_BAIL`** | **0.80 – 0.97** | **0.88** |
| **`WARP_BAIL_MEM`/`WARP_BAIL`** | **0.03 – 0.20** | **0.12** |
| **`WARP_BAIL_FENCE`/`WARP_BAIL_MEM`** | **≤ 0.10** | **0.02** |

**The `DISC_EV` band is TIGHTENED rather than repeated, and that is a stronger claim.** Round 4
pre-registered 80 – 140 and measured 90.55; the quantity is the guest's own cache- and
ATC-maintenance rate, which no engine change can move, so round 5 re-registers it at
**75 – 110**. A miss now means something happened to the guest, not to the engine.

> ### **THE COVERAGE CALL, MADE BEFORE THE ROW, FOR THE SECOND CONSECUTIVE ROUND: TRANCHE 2 DOES NOT REACH 60 % EITHER, AND IT IS NOT CLOSE.** 0.20 – 0.28 against a 60 % bar. The kill line's precondition stays unmet for a third wave, the kill line stays armed and unfired, and **round 5 takes no verdict of any kind.**
>
> **AND HERE IS WHAT WOULD REACH IT, PRICED AND NOT BUILT.** The callees corpus refuses 125 of 240 instructions after tranche 2. The three largest refusals are **`MOVE` to `−(An)` (22)**, **`MOVE (An)+,mem` (22)** and the **`LINK`/`UNLK`/`RTS`/`JSR` frame quartet (40)**. Together they are **35 % of the corpus** — and every one of them is a **memory-DESTINATION or a stack-frame family**, which is to say **§2e's write-back exclusion is what stands between generation one and the bar**, not the family list.
>
> **What a tranche 3 would need, stated without building any of it:** (1) a **restartable write-back contract** — the interpreter advances the register, sets `instruction_pc` to the advanced pc and clears `mmu_restart` *before* it stores, so a block that bails at a memory destination must reproduce that exact post-state rather than the pre-state, which means the exit sequence gains a per-site *write-back shape* rather than a per-site pc; (2) a **source/destination aliasing analysis per shape**, because `MOVE.L (A0)+,(A0)` computes its destination EA from the *incremented* `A0` (§0.12.2 refused the undo-on-bail shortcut for exactly this); (3) `JSR`/`BSR`/`RTS` need a **stack write inside a control transfer**, which is two faultable sites and a block boundary in the same instruction. **None of it is scoped here, and none of it is promised.** The honest reading of three rounds of coverage — 8.43 %, 19.10 %, and a predicted 22.9 % — is that **generation one's frozen scope has a ceiling around a quarter of this workload**, and the 60 % bar is a *different engine*, not three more families.

**Step 2 — the armed dhrystone band.** §0.12.7's arithmetic, with its gate term re-calibrated
on round 4's own row rather than round 3's:

```
   R4 measured 0.92432x  ->  T' = 214.2/0.92432 = 231.7 cy   (net +17.5 cy per guest insn)
   entry(R4) = 0.1910 x (112/2.4208) x 2.17 = 19.17 cy       (E = 112, measured, G-R4.8)
   removal(R4) = 19.18%..40.34% x 0.1910 x 214.2 = 7.8..16.4 cy
   -> the gate term must be 17.5 - 19.17 + (7.8..16.4) = 6.1..14.7 cy over 0.809 of dispatches
   -> G_eff = 16 instructions costing 10.4 cy = 0.65 cy each   (RE-CALIBRATED, once, on R4)

   R5 LOW    c=0.20, L=2.30, E=112 (held; T4 and LEA are short bodies)
      entry   0.20 x (112/2.30) x 2.17          = 21.14 cy
      gate    0.80 x 10.4                       =  8.32 cy
      removal 19.18% x 0.20 x 214.2             =  8.22 cy
      T' = 214.2 - 8.22 + 29.46 = 235.4         ->  0.910x
   R5 HIGH   c=0.28, L=3.10, E=112
      entry   0.28 x (112/3.10) x 2.17          = 21.95 cy
      gate    0.72 x 10.4                       =  7.49 cy
      removal 40.34% x 0.28 x 214.2             = 24.19 cy
      T' = 214.2 - 24.19 + 29.44 = 219.5        ->  0.976x
```

> ### **The round-5 armed band: 0.910× – 0.976× of the same-session clean-interpreter reference row `R`.** Round 4 measured 0.92432×, inside its own band at the first attempt; this band's floor is **below** that row and its ceiling is **above** it, which is the honest shape of a tranche that widens coverage while the entry cost per block is unchanged. **It still does not cross 1.0×, and it is still not supposed to** — §0.5.3's arithmetic says amortisation starts paying above `L` ≈ 8 and the predicted `L` is 3.1.
>
> **`TAILPOLL` is charged ZERO again and the correction is restated rather than inherited silently.** At `L` = 2.3 – 3.1 the service poll runs *more* often per four instructions than interpretation would, so its removal row is negative and is charged 0. The band's floor is optimistic by exactly that amount, in both rounds, and this sentence is the record of it for the second time.

**Step 3 — the disarmed row, which is the one that gates the card, and it is UNCHANGED.**

> ### **The blessing gate is carried forward verbatim: −2.276 % or better, i.e. dhry ≥ 7 988 against the same-session reference.** It is an **absolute class**, it has now been missed twice (−4.056 %, −4.256 %), and round 5 **does not predict that it will be met** — because round 5 does not fix the residual, it *bisects* it. **The pre-declared end-state condition is unchanged: the card ships A0 if and only if A0's disarmed row meets the ≥ 7 988 class, and otherwise goes back to `F294E75A` for a third consecutive round.** Naming that outcome as the likely one, here, before the row, is the alternative to being disappointed by it afterwards.
>
> **Q11's boot row rides beside it: 45 … 50 s**, widened at the top from round 4's 45 … 47 because tranche 2 adds emitter code to an image whose boot is disarmed and because round 4's own row measured 49 s.

**Every band above is quoted against `R` measured in the SAME session, `n` ≥ 5, with §0.5.2's
absolute floor 8 242 quoted beside it and never removed.**

### 0.14.9 What round 5 explicitly does not establish

1. **A verdict.** Coverage is pre-registered below the bar for the third consecutive wave
   (§0.14.8). The kill line's precondition is unmet, the kill line does not fire and is not
   cleared, and no row taken this round may be read as pricing the engine.
2. **A mechanism for the disarm residual.** The kit **bisects**; it does not explain. A variant
   that recovers half the penalty names a *layer*, and a layer is not yet a mechanism —
   §0.10.6 and §0.12.1 are what naming a mechanism without an instrument produced twice.
3. **That the memory-touching families work on silicon.** Round 4's memory-source ALU and
   auto-adjust sources remain harness-proven only. §0.14.4 specifies the arm that would change
   that and round 5 does not build it. **Re-owed, now with a design.**
4. **That the bail split's *causes* generalise.** Dhrystone is small and resident. The fence
   and verdict sub-causes are exactly the ones a paging-heavy workload would move, which is
   M2's question.
5. **That any ablation edge transfers to a different image.** Every edge below the noise floor
   is unreadable by construction, and every edge above it is a property of *this* build of
   *this* toolchain at `-Os -flto`. A layout result is not a portable one.
6. **`z3660_warp_run`'s preamble**, unchanged and still the largest row in §0.9.3's table.
7. **Everything §0.8.8, §0.10.7, §0.11.7, §0.12.8 and §0.13.6 said would not be established**,
   unchanged.


## 0.15 M1a round 5 — built, and its gates scored (2026-08-24)

**The pre-registration in §0.14 was committed at `d152a50`, before the first line of round-5
engine code; the implementation commits follow it in this repository's log.** Every prediction
in §0.14 is left byte-untouched above and scored beside itself.

### 0.15.1 What was built

| piece | where |
|---|---|
| **A** the ablation kit — `Z3660_WARP_ABLATE=1…5`, five build dirs, five `.bif`s, five images | `Z3660_emu/Makefile`, `vitis_ide/Makefile`, `src/uae/z3660_warp_ablate.h`, `Z3660_system/Alfa/system_cli_abl*.bif` |
| **B** the bail attribution — `WARP_BAIL_STUB` / `_MEM` / `_FENCE` / `_VERDICT` / `_OTHER`, ids 64–68 | `z3660_prof.h/.cpp`, `z3660_warp.cpp` |
| **C** tranche 2 — `LEA` (non-indexed), `CLR Dn`, `EXT`/`EXTB`, and the 1 152 register-count shift forms | `z3660_warp.cpp`, `e_shift_rot_reg()` / `e_shift_reg()` |
| **D** `WARPTEST` F14–F17, the targeted shift-count differential, seven new mutation arms | `z3660_warp.cpp`, `harness_main.cpp` |
| **E** the rounds-1/2 re-derivation | four clean rebuilds in disposable worktrees; §0.15.2 |
| — | **`tools/warp-gate.sh`**, which was NOT in §0.14.6 and is there because §0.15.2 happened |

### 0.15.2 THE BUILD-INTEGRITY FINDING, CORRECTED AND MOVED — §0.13.4 named the wrong round

**§0.14.5 asked for a cheap re-derivation and expected round 3 to serve as the control that
reproduces. It does not reproduce. It refutes.**

Four clean rebuilds, each in a fresh worktree at that round's own scored commit, on **this
host with the toolchain that built every one of the images that flew** (`arm-none-eabi-g++
14.2.1 20241119`, the same one, the same day):

| round | scored at | the doc's claim | **clean rebuild** | the gate, clean |
|---|---|---|---|---|
| 1 | `b16967d` | **2 324 B**, gate **5 insns** (`movw`,`movt`,`ldr`,test,`b`), depth 1 | **2 144 B** | `ldr [pc,#1976]` → `ldr [r3]` → `cmp` → `beq` — **4 insns, depth 2** |
| 2 | `a392fbe` | **2 548 B** | **2 348 B** | `ldr [pc,#2172]` → `ldr [r3]` → `cmp` → `beq` — **4 insns, depth 2** |
| 3 | `1adb64c` | 2 196 shipped; §0.13.4's clean control **2 380 B**, 5 insns | **2 196 B** | `ldr [pc,#2028]` → `ldr [r3]` → `cmp` → `beq` — **4 insns, depth 2** |
| 4 | `64d66dd` | **2 372 B**, **4** insns, **2** loads (G-R4.7 scored MISSED) | **2 188 B** | `ldr [r4,#96]` → `cmp` → `beq` — **3 insns, ONE load, zero materialisations** |

> ### **§0.13.4 IS CORRECTED AT R5 IN BOTH DIRECTIONS. Round 3's source reproduces its shipped image EXACTLY — 2 196 B, the same four instructions, down to the same literal-pool offset `#2028`. Round FOUR's does not.**

**And it is proven on the artifact rather than argued from a rebuild.** The gate words of the
flown image and of the clean rebuild are different byte sequences, and each appears in exactly
one of the two:

```
   ldr r3,[r4,#96] ; ldr r0,[r4,#88] ; cmp r3,#0     e5943060 e5940058 e3530000
      in the flown BOOT-lean-C62AE6E2.BIN : 1        in the round-4 clean ELF : 0

   ldr r3,[r4,#96] ; cmp r3,#0 ; beq +0x98 ; ldr ..  e5943060 e3530000 0a00000a e5940058
      in the flown BOOT-lean-C62AE6E2.BIN : 0        in the round-4 clean ELF : 1
```

> ### **THE IMAGE THAT MEASURED −4.256 % IS NOT WHAT ROUND 4'S SOURCE BUILDS, AND THE CONSEQUENCE IS THAT §0.12.1'S FIX WAS NEVER FLOWN.** The design was three instructions, one load off a live base, zero address materialisations. The clean build produces exactly that. The image that went to metal carries a fourth instruction — the argument load `ldr r0,[r4,#88]` hoisted ABOVE the branch, so the disarmed path executes it on every pass. **G-R4.7 was scored `MISSED` against an image its own source does not produce; on round 4's source it is a HIT on every clause, including the size bound it missed by 176 B.**

**How, given that round 4 is the round that fixed the `.d` hazard.** The images were built at
09:28 and `-include $(OBJS:.o=.d)` was committed at 09:33. **The fix landed after the images
did, and nothing rebuilt them.** `-MMD` writes the dependency data and `-include` finally
reads it — but a tree whose objects are *already* stale still links happily, and neither half
of that mechanism can repair a build that has already happened. Only a clean can.

**What this does and does not move:**

- **It does not re-open a single metal row.** The images that flew are the images that flew
  and their measured penalties stand as measurements of *those images*. −2.276 %, −5.848 %,
  −4.056 % and −4.256 % are all still true of the binaries they were taken on.
- **It moves what those rows are EVIDENCE FOR.** §0.12.1's mechanism argument compared round
  1's *"five instructions, two ALU immediates, depth 1"* against round 3's *"four, depth 2"* —
  and round 1's **source** produces **four instructions at depth 2**, the same shape as round
  3's. **There was never a depth difference between the two rows the argument was built on.**
  That is an independent reason §0.12.1's prediction failed, and a better one than "the
  mechanism was wrong".
- **§0.13.4's "the −4.056 % baseline is not a clean measurement of round 3's source" falls.**
  Round 3's baseline is clean. Rounds 1, 2 and 4's are the ones that are not.
- **`tools/warp-gate.sh` exists so the next round cannot do this again.** It reads
  `m68k_run_mmu040`'s size and its disarmed gate straight off the built ELF and prints them.
  An image is a container nobody disassembles; an ELF is not.

**One caveat, stated rather than buried.** These rebuilds link the prebuilt BSP from the main
tree by symlink and build `z3660_emu_lean` alone rather than the whole `lean` target. Neither
reaches core1's `.text` — `make lean`'s only core1 step *is* `z3660_emu_lean`, and `libxil.a`
is the same file in all four builds and in the builds that flew. **The artifact-side byte
search above needs none of that to hold**, which is why it is the evidence and the rebuilds
are the explanation.

### 0.15.3 THE ABLATION MANIFEST — the matrix as built, before any metal row exists

**All eight images built from one tree after `make clean-all`, CRC-stamped and archived.**

| variant | CRC-32 | image B | removes | `m68k_run_mmu040` | `.text` | predicted recovery |
|---|---|---|---|---|---|---|
| **A0** `BOOT-lean.BIN` | **`7A5EC595`** | 12 680 148 | nothing — **the shipping lean** | **2 188** | 6 344 904 | — (the control) |
| **A1** `BOOT-abl1.BIN` | **`0BAEECE1`** | 12 663 716 | `L-WTST` — the self-test, its 17 corpora, the ARM word executor | 2 188 | 6 340 032 | **0.0 – 0.5 pp** |
| **A2** `BOOT-abl2.BIN` | **`161412F2`** | 12 680 148 | `L-ATT` — the arm attestation | 2 188 | 6 344 840 | **0.0 – 0.3 pp — THE NOISE FLOOR** |
| **A3** `BOOT-abl3.BIN` | **`5E2592EF`** | 12 680 116 | `L-HOOK` — the run-loop gate | **2 104** | 6 342 612 | **0.6 – 1.4 pp** |
| **A4** `BOOT-abl4.BIN` | **`459E51A3`** | 12 680 116 | `L-HOOK` + `L-REG` — and the `regstruct` field | **2 104** | 6 342 580 | **1.8 – 3.4 pp — THE FAVOURITE** |
| **A5** `BOOT-abl5.BIN` | **`BBED0509`** | 12 663 644 | the engine entirely | 2 104 | 6 326 448 | **3.9 – 4.6 pp — closure** |
| `BOOT.BIN` | `F48DDEED` | 12 680 180 | (the default variant, rebuilt) | — | — | — |
| `BOOT-prof.BIN` | `6973E8B5` | 12 696 676 | (the profiling variant, rebuilt) | — | — | — |
| `BOOT-q2probe.BIN` | `FCC0CB3F` | 12 795 260 | **untouched**, as gated | — | — | — |

**`tools/warp-gate.sh` reads the whole matrix and its answer is the layer census restated
independently** — it recognises the gate by its SHAPE (a load, a compare against zero, a
conditional branch) rather than by position, so a build without one says so rather than
printing six lines of interpreter dispatch under the wrong heading:

```
   A0 / A1 / A2 :  2188 B   ldr r3,[r4,#96] / cmp r3,#0 / beq      loads on the disarmed path: 1
   A3 / A4 / A5 :  2104 B   NO WARP GATE -- the run-loop hook is not in this build
```

**The static half of the matrix behaves exactly as the design says it must**, and three rows
are worth reading before any metal time:

```
   A0 -> A2   removing the attestation :   -64 B of .text, run loop UNCHANGED at 2188
   A0 -> A3   removing the hook        : -2292 B of .text, run loop 2188 -> 2104  (-84)
   A3 -> A4   removing the field       :   -32 B of .text, run loop UNCHANGED at 2104
```

> ### **A3 AND A4 DIFFER BY 32 BYTES OF CODE AND 7 910 CHANGED FUNCTION BODIES.** `warp-objdiff.py` over the pair: 25 096 functions, **7 910 changed**, 0 added, 0 removed — reproducing §0.13.3's 7 899 on a pair that is **dynamically identical**. One added struct member re-encodes an immediate constant in **31.5 % of the image's functions** while changing nothing whatever about what the code does. If layout is what the disarm penalty is made of, this is the edge that will show it.

**And the kit's own noise floor has a STATIC reading too, which was not asked for and is the
most useful thing the matrix produced before the card:**

```
   A0 vs A2  (remove 64 B of COLD code)         :   457 function bodies changed
   A1 vs A2  (two different single layers)      :   462
   A0 vs A3  (remove the hook)                  :   459
   round-4 clean vs round-5 A0 (a whole round)  :   457
   A3 vs A4  (the layout edge)                  : 7 910
```

> ### **Under `-Os -flto`, ANY change to this image re-decides inlining in about 460 function bodies.** Four completely different edges — one cold-code removal, one hot-path removal, two unrelated layers, and an entire round of engine work — all land within five of each other. **That is the ambient churn of the toolchain, not a property of any of the changes**, and it is why §0.14.1 insisted on a negative control before any of these rows are read. The layout edge stands **17×** above it. Nothing else does.

### 0.15.4 The gates

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| **G-R5.1** harness | `0 failed`; **+120 … +400**; the **same** delta in lean | green, `0 failed`: **3 334 / 3 330 / 3 561** against 3 320 / 3 316 / 3 547 — **+14 / +14 / +14** | green; count ***MISSED, low*** (as rounds 1–4 all did); "same in lean" **HIT**; prof's further delta **231 both rounds = +0** **HIT** |
| **G-R5.2** accept-set discriminator | engine == model gen 5 **exactly**; **18 368**; symmetric difference **0** | engine **18 368**, model **18 368**, **agree on 65 536 of 65 536**, difference **0 in both directions** | **HIT. G-R4.2's owed 1 152 is closed** |
| **G-R5.3** sweep and differential | 6 seeded states, **0** divergences, **≥ 108 000** pairs | **110 208 pairs over 18 368 accepted opcodes, 0 divergences** (round 4: 102 096 over 17 016) | **HIT** |
| **G-R5.4** the new mutation arms | every one caught, every one clean disarmed, none demonstrably uncatchable | **22 of 22 caught.** Four of the seven new arms first reported NOT CAUGHT and each was diagnosed rather than dropped (§0.15.6) | **HIT**, and the diagnosis is the finding |
| **G-R5.5** the shift-count differential | every T4 shape × the 15 pre-registered counts, **0** divergences | **1 080 armed/disarmed pairs over 18 shapes × 15 counts × 4 operands, 0 divergences** | **HIT** |
| **G-R5.6** lockstep | `WARP_DIVERGE == 0` over **≥ 10⁸** | **0 over 100 000 000 guest instructions, 12 500 000 block executions, 100.0 % retired inside blocks** | **HIT** |
| **G-R5.7** `WARPTEST` | **17** families, F1 still **+0**, a new ARM encoding through the executor | **PASS 17/17, 0 mismatches**; F1 **64/64**, **+0**; F17 drives the register-specified shift and `RSB` through `warp_host_exec` | **HIT** |
| **G-R5.8** the bail-split identities | all three exact, in the harness before any metal | **BAIL 22 = STUB 12 + MEM 10 + OTHER 0**; **MEM 10 = FENCE 3 + VERDICT 7**; **OTHER 0** — and both sub-arms non-zero, so nothing is asserted over a path that never ran | **HIT** |
| **G-R5.9** the ablation matrix, static | each variant's removed layer absent from its objdump; every CRC recorded | `z3660_warp_step` absent from A3/A4/A5; `z3660_warp_selftest` absent from A1/A5; the run loop 2 188 → 2 104 exactly at the hook edge; A3/A4 identical in run-loop size and 7 910 bodies apart; all eight CRCs recorded | **HIT** |
| **G-R5.10** object equivalence, flags OFF | **0** changed function bodies | the lean ELF built with `-DZ3660_WARP_ABLATE=0` forced is **BYTE-IDENTICAL** to the shipping lean built with the flag absent; `warp-objdiff.py`: 25 102 common, **0 changed** | **HIT** |
| **G-R5.11** images | trio + five variants, CRCs, archive fires; `q2probe` untouched | eight built and archived CRC-stamped (§0.15.3); `BOOT-q2probe.BIN` untouched at **`FCC0CB3F`** | **HIT** |
| **G-R5.12** the rounds-1/2 re-derivation | both complete, sizes recorded whether or not they match | four completed; **three of the four doc claims move**, and the finding is bigger than the gate (§0.15.2) | **HIT — and it refuted §0.13.4** |

**Eleven of twelve gates hit. G-R5.1's count missed low for the fifth consecutive round** — a
pre-registered range that has been wrong every single time it has been written is a statement
about the estimator rather than about the rounds, and it is recorded as one. **G-R5.12 "hit"
in the sense that it ran and reported; what it reported is §0.15.2.**

### 0.15.5 The bail split, read in the harness — and what it does NOT yet say

`test_warp_bail_split()` produces both populations deliberately and both identities close
exactly. **The harness reading is a MECHANISM CHECK, not the measurement**: 22 bails on a
five-opcode corpus says the counters are wired to the right sites and says nothing about
dhrystone's mix. **§0.14.2's prediction — `BAIL_STUB/BAIL` in 0.80 – 0.97 — is a claim about
metal and stays unmeasured.**

What the harness does settle, and it is the part that could have been wrong:

- **`BAIL_MEM == BAIL_FENCE + BAIL_VERDICT` holds exactly**, which is a claim about the
  emitter rather than about arithmetic — one helper decline produces one bail, because the
  `CMP` / `e_bail_if` pair follows every `blx` and no other kind of site emits one. Had a bail
  existed without a decline in front of it, this is where it would have shown.
- **`BAIL_OTHER == 0`**, the tripwire, on a corpus built to reach every arm.
- **Both sub-causes are non-zero** (fence 3, verdict 7), so neither sub-counter is an identity
  asserted over a path that never ran — §0.5.5 G4's engagement rule, applied to a counter
  rather than to a corpus.

### 0.15.6 Five things worth recording that were not gates

- **FOUR OF THE SEVEN NEW MUTATION ARMS FIRST REPORTED `NOT CAUGHT`, AND ALL FOUR WERE
  DIAGNOSED RATHER THAN DROPPED.** §0.13.5 named the failure mode — *"a mutation arm that
  cannot be caught at all is worse than none: it reports CAUGHT-shaped silence"* — and round 5
  walked into four fresh instances of it in one afternoon:
  - **`X` could not be set from the arm table at all.** `warp_seed()` takes X from bit 27 of
    its key and every key the loop builds is below 2²⁷, so **X was zero in all eight seeds**
    and an arm that flips X had nothing to flip. The table grew an `x` column.
  - **A count and an operand in the same register cannot express "count 0 on a non-zero
    operand"**, which is what three of the four T4 arms turn on. The table grew a `d1` column
    and the opcodes moved to `<shift> D1,D0`.
  - **Two arms named the wrong opcode** — `0xE1A8` is `LSL`, which has no overflow test at
    all, where the ASL-clamp arm needed `0xE1A0`.
  - **`EXT.W`'s FLAG WIDTH IS NOT MUTATABLE, and that is a property rather than an
    oversight.** The value being tested is a *sign-extended byte*, so its bit 15 and its bit 31
    are the same bit and its low 16 bits are zero exactly when all 32 are: reading the flags at
    long width instead of word width gives the identical N and Z **for every operand that
    exists**. The arm was pointed at the destination width, which is observable. **A mutation
    that cannot be observed is not a weak test; it is a proof that two programs are the same
    program, and it is worth writing down as one.**
- **`docs/warp-corpus.py`'s gen-4 line was deliberately left unamended at 18 168.** Editing it
  to 17 016 would have made a scored gate pass retroactively. §0.13.6 fixed the direction of
  the repair and this round took it: **the emitter moved.**
- **`EXT` must be decoded before `LEA` or every `EXTB.L` is silently refused.** `EXTB.L Dn` is
  `0100 1001 1100 0rrr`, which matches `LEA`'s pattern with `rrr = 100` and an EA mode of
  `000` — an encoding the 68k reassigns precisely *because* `LEA Dn,An` is illegal. Both the
  emitter and the model carry the ordering and the comment; the accept-set discriminator would
  otherwise have reported a disagreement with neither side knowing which was right.
- **The five ablation `.bif`s had to be named one by one in `Z3660_system/.gitignore`**, whose
  own comment predicted this: *"An ignore rule that has to be defeated by a forced add is a
  rule that will eventually not be defeated."* The kit is an instrument rather than a shipping
  image, which makes it exactly the kind of thing that gets left untracked and then cannot be
  rebuilt by the session that needs to re-run the matrix.
- **The shipping lean carries §0.12.1's designed gate for the first time.** `7A5EC595`'s
  disarmed path is `ldr r3,[r4,#96]` / `cmp r3,#0` / `beq` — **three instructions, one load off
  a base the loop already holds, zero address materialisations, `m68k_run_mmu040` at 2 188 B**.
  Round 4 designed it, scored it MISSED against an image that did not contain it, and rolled
  the card back on a row that never tested it.

### 0.15.7 What round 5 leaves owed

1. **Every metal row.** Round 5 takes none. The ablation matrix, the bail split's shares, the
   coverage band, the armed band and the disarm penalty are all pre-registered in §0.14 and
   all unmeasured. **Taken 2026-08-24 → scored in §0.16.**
2. **The decision rule's own bar needs re-reading against A0's own row.** §0.14.1 rule 2 pins
   it at 2.128 pp = half of round 4's −4.256 %, and §0.15.2 has just established that
   −4.256 % was measured on an image round 4's source does not build. **The rule is applied to
   HALF OF A0's OWN SAME-SESSION PENALTY**, and §0.14.1's 2.128 pp is left byte-untouched
   beside it as what was written before the row.
3. **Whether A0's disarmed row is different at all now.** The gate it ships is one instruction
   and one load lighter than the one that measured −4.256 %. If the penalty moves on that
   alone, §0.12.1's mechanism is back in play — and it will have been resolved by a build
   discipline rather than by a theory.
4. **The memory-touching families on silicon.** §0.14.4 specifies the helper-substitution
   self-test arm; round 5 did not build it. **Re-owed, now with a design rather than a
   complaint.**
5. **The indexed `LEA` modes** — 6 of the 240 hot instructions, refused with a named reason
   (§0.14.3 T1) and worth a pre-registration of their own if a later tranche wants them.
6. **`z3660_warp_run`'s preamble**, unchanged, still the largest row in §0.9.3's table.
7. **Everything §0.8.8, §0.10.7, §0.11.7, §0.12.8, §0.13.6 and §0.14.9 said would not be
   established**, unchanged.

## 0.16 M1a round 5 — the metal row, scored against §0.14 (2026-08-24)

**The row §0.15.7 item 1 said round 5 takes none of.** The ablation matrix, the bail split's
shares, the coverage band, the armed band and the disarm penalty — every one pre-registered in
§0.14 and left unmeasured when §0.15 was written — were taken on **2026-08-24, 11:55 – 13:34
EEST**, A4000 + Z3660, piscsi-root installed AMIX, preset 7 (`amix_ram 128`, `service_cadence 4`,
`arm_frequency 1100` — see the caveat at §0.4.1, `UAE_040_MMU`). Eight flashes, eight guest boots, forty-five scored
dhrystone rows across nine arms, two `WARPTEST` gates, two `PROFZ`-fenced counter windows. Every
number below is read off the session's scoring sheets and verdicts (evidence held locally); the same numbers are reproduced a third time in
`docs/warp-gen2-prospectus.md` §§1.1–1.2 and 6.1, which prices them and does not score them —
its own words, and §0's authority stands over it. **Where the two disagree, this section follows
the evidence directory**, and the one place they diverge in substance is noted at §0.16.2.

### 0.16.1 The anchor, and the card decision it drives

`R` = **8185.0** (`F294E75A`, n = 7, spread 0.42 %, `s` = 0.70 %, threshold `R×(1+s)` =
**8242.3**, absolute floor 8242 quoted beside and never removed).

| | pre-registered (§0.14.8 step 3) | measured | verdict |
|---|---|---|---|
| **A0 disarmed** (the blessing gate) | ≥ 7 988 (same-session `R×0.97724` = 7 998.7; doc absolute 7 988) | **7 903**, penalty **−3.445 %** | **MISSED, third consecutive round** — 96 dhry short of 7 998.7, 85 short of 7 988 |
| A0 vs round 4's flown penalty | — | −3.445 % vs −4.256 % | **better by 0.811 pp** — but the class is absolute, not a delta |
| Q11 boot row, A0 | 45 … 50 s | **48 s** | **HIT** |

**The card does not ship `7A5EC595`.** The pre-declared rule (§0.14.1, carried from §0.12.7 step
3) is unconditional on the class, not on the trend: round 5 flew §0.12.1's designed gate for the
first time — one instruction, one load lighter than the image that measured −4.256 % — it is
worth 0.811 pp, and it is still not enough. `0:/Z3660.bin` returns to the clean interpreter
lineage, `F294E75A`, for a third consecutive round.

### 0.16.2 THE ABLATION MATRIX — every edge against its §0.14.1 prediction

| variant | removes | dhry (n=5) | penalty vs `R` | boot |
|---|---|---|---|---|
| A0 `7A5EC595` | nothing — the shipping lean | 7 903 | −3.445 % | 48 s |
| A1 `0BAEECE1` | `L-WTST` | 7 905 | −3.421 % | 48 s |
| A2 `161412F2` | `L-ATT` — **the floor** | 7 861 | −3.958 % | 49 s |
| A3 `5E2592EF` | `L-HOOK` | 8 064 | −1.478 % | 47 s |
| A4 `459E51A3` | `L-HOOK` + `L-REG` | 8 140 | −0.550 % | 46 s |
| A5 `BBED0509` | the engine entirely | 8 163 | −0.269 % | 46 s |

| edge | layer | predicted | measured | verdict |
|---|---|---|---|---|
| A0→A1 | `L-WTST` | 0.0 – 0.5 pp | **+0.024 pp** | **HIT** — below the floor, and that IS the prediction |
| A0→A2 | `L-ATT` = **THE FLOOR** | 0.0 – 0.3 pp | **−0.513 pp** | **MISSED, wrong-signed** — removing 64 B of cold attestation code made the disarmed image measurably *slower*, 0.213 pp past the band's own ceiling in magnitude |
| A0→A3 | `L-HOOK` | 0.6 – 1.4 pp | **+1.967 pp** | **MISSED, HIGH** by 0.567 pp — readable, 3.8× the floor |
| A3→A4 | `L-REG` — the pre-registered favourite | 1.8 – 3.4 pp | **+0.929 pp** | **MISSED, LOW** by 0.871 pp — readable, 1.8× the floor, but half its own band's bottom |
| A0→A5 | whole engine (closure) | 3.9 – 4.6 pp | **+3.177 pp** | **MISSED, LOW** by 0.723 pp |

**The decision rule (§0.14.1), read in its own order:**

```
1. THE FLOOR = |penalty(A0) - penalty(A2)| = 0.513 pp.
   No edge smaller than 0.513 pp may be read as a mechanism, in this round or any later one.

2. A single edge that recovers >= HALF the measured penalty NAMES the mechanism.
   Largest single-layer edge: A0->A3 (L-HOOK) = +1.967 pp = 57.1 % of A0's 3.445 pp.

   APPLIED bar (§0.15.7 item 2 -- half of A0's OWN same-session penalty) = 1.7227 pp
      1.967 >= 1.7227  ->  NAMES L-HOOK, the run-loop gate
   PRE-REGISTERED bar (§0.14.1 rule 2, byte-untouched -- half of round 4's -4.256 %) = 2.128 pp
      1.967 <  2.128, short by 0.161 pp  ->  rule 3 would read "distributed or emergent"

   BOTH BARS ARE REPORTED. The applied reading governs; the pre-registered figure stands
   beside it, unrewritten, because the naming verdict rests on a 0.161 pp margin against a
   0.513 pp floor, and saying so is the difference between an attribution and a claim.

4. A5's own penalty = -0.269 %; |0.269| <= floor 0.513  ->  WITHIN the floor of zero.
   The four named layers SPAN the cost. The "something outside all four carries it" branch
   does not fire.

5. ADDITIVITY: (A0-A1)+(A0-A2)+(A0-A3)+(A3-A4) = 0.024 - 0.513 + 1.967 + 0.929 = +2.407 pp
   (A0-A5) = +3.177 pp.  |difference| = 0.770 pp,  tolerance 2x floor = 1.026 pp  ->  HOLDS.
   The layers ADD; they do not interact.
```

> **The named mechanism is `L-HOOK`, the run loop's `if (z3660_warp_on) … z3660_warp_step()` gate
> and the restructured loop body around it — the first attribution this campaign has produced,
> after two disassembly-guessed mechanisms were refuted on metal (§0.10.6, §0.12.1).** `L-REG`,
> the pre-registered favourite, is refuted in public as §0.14.1 asked: it measured 0.929 pp
> against a predicted 1.8 – 3.4 pp — real, but under half its own floor.

*(One evidence-file note, not a doc-content issue: `MATRIX-SCORING.txt`'s own verdict line for
A0→A2 reads "MISSED, LOW by 0.513 pp", which is the measured magnitude rather than the miss's
excess over the 0.3 pp band ceiling; `MATRIX.md`'s prose gives the arithmetically consistent
0.213 pp and is what this section follows.)*

### 0.16.3 The armed row, and the coverage precondition

| row | pre-registered (§0.14.8 steps 1–2) | measured | verdict |
|---|---|---|---|
| **coverage `c`** | 0.20 – 0.28, point 0.229 | **0.20263** | **HIT** — below 60 %, the kill line's precondition is unmet for a **third consecutive wave** |
| armed, A0, n=5 | 0.910× – 0.976× of same-session `R` (7 448.4 – 7 988.6) | **7 563** = **0.92401×**, spread 0.42 % | **HIT** — 115 above the floor, 426 below the ceiling |
| return leg, n=3 | must return to the A0 disarmed class | **7 897** vs 7 903 = **−0.076 %**, spread 0.11 % | **HIT** |
| identity `(A0/R)×(armed/A0) = armed/R` | closes to ≥ 3 digits | 0.96555 × 0.95698 = **0.92401** vs **0.92401** | **HIT** — closes exactly, **5 digits** |
| kill comparison `armed ≤ R×(1+s)` | — | 7 563 ≤ 8 242.3, satisfied by 679 dhry | **precondition unmet — not a firing** (§0.2's guard: a miss at 20 % coverage is a bug report, not a verdict) |

**No verdict, in either direction, for the third wave running.** `WARPTEST` returns `PASS 17/17`
on both images, all counter identities close exactly, the dhrystone correctness gate is
byte-identical, `FAULTS(on)` equals `FAULTS(off)` exactly, the return leg returns to class, and
the whole-session anomaly scan is clean (§0.16.6). The engine is correct; it engages a fifth of
the workload; that is not a price.

### 0.16.4 The six counter bands — all HIT

| row | pre-registered band | point | measured | verdict |
|---|---|---|---|---|
| coverage `c` | 0.20 – 0.28 | 0.229 | **0.20263** | **HIT** |
| `WARP_INSNS`/`WARP_BLK` | 0.92 – 1.32 | 1.088 | **0.94765** | **HIT** |
| compiled length `L` | 2.30 – 3.10 | 2.651 | **2.4689** | **HIT** |
| decline rate | 51 – 62 % | 56.8 % | **58.778 %** | **HIT** |
| bail rate | 58 – 70 % | 64.3 % | **67.397 %** | **HIT** |
| `WARP_DISC_EV` per M retired (`I_true`) | 75 – 110 | 90.6 | **90.358** | **HIT** — 0.24 from the point estimate |

### 0.16.5 THE BAIL SPLIT — two clean misses, the falsifier that did not fire

```
WARP_BAIL 9 207 252 = STUB 9 205 768 + MEM 1 484 + OTHER 0        EXACT
WARP_BAIL_MEM 1 484  = FENCE 0 + VERDICT 1 484                     EXACT
WARP_BAIL_OTHER = 0                                                EXACT
```

| row | pre-registered (§0.14.2) | measured | verdict |
|---|---|---|---|
| `WARP_BAIL_STUB`/`WARP_BAIL` | 0.80 – 0.97, point 0.88 | **0.99984** | **MISSED, HIGH** by 0.030 |
| `WARP_BAIL_MEM`/`WARP_BAIL` | 0.03 – 0.20, point 0.12 | **0.000161** (1 484 / 9 207 252) | **MISSED, LOW** by 0.030 |
| `WARP_BAIL_FENCE`/`WARP_BAIL_MEM` | ≤ 0.10, point 0.02 | **0.000** | **HIT** |
| `WARP_BAIL_OTHER` | exactly 0 | **0** | **HIT** — the tripwire held |

> **THE FALSIFIER DID NOT FIRE.** §0.14.2's rule, quoted: *"if `WARP_BAIL_MEM`/`WARP_BAIL` ≥
> 0.30, the memory-helper candidate is CONFIRMED and this prediction is marked MISSED beside
> itself."* Measured **0.000161 — roughly 1 875× below the falsifier**. §0.13's named mechanism
> for round 4's bail-vs-decline gap — *"round 4's auto-adjust `(An)+`/`−(An)` sources compile
> successfully but bail at run time on the memory helper"* — is **REFUTED AT THREE DIGITS**: it
> accounts for 1 484 bails in 9.2 million. **The prediction's direction is right, emphatically —
> the gap IS the decline stub — and both magnitudes miss, in the same direction**: the stub
> share is more extreme than the band's own ceiling; essentially every bail is a cached decline
> stub.

**`WARP_BAIL_FENCE` never fired on metal.** It measured exactly 0, so the `MEM == FENCE +
VERDICT` identity above is asserted over a path that never ran on this workload; the identity is
exact, but **silicon has not exercised the fence arm**, which §0.14.9 item 4 already named as
M2's question rather than this round's. Flagged rather than absorbed.

**Two standing correctness gates, not new §0.14 predictions but scored on this exact tranche-2
image for the first time:**

- **`FAULTS(on)` vs `FAULTS(off)`**: **591 == 591**, delta 0 — the bail contract did not leak a
  fault (§0.5.5 G2).
- **Budget closure**: `COMPILE` 41 776 = first-compiles 134 + `DISC` 41 642 + `EVICT` 0 +
  `FLUSH` 0 → unattributed **0.000 %** against a ≤ 5 % gate (§0.5.6).

### 0.16.6 `WARPTEST`, boot windows, and R6's corroboration of §0.15.2

**`WARPTEST`, both images, each on its own boot before its own arm:**

| image | families | F1 alignment sweep | range misses | verdict |
|---|---|---|---|---|
| A0 lean `7A5EC595` | 17/17 | 64/64 | +0 | `PASS 17/17 families, 0 mismatches` |
| prof `6973E8B5` | 17/17 | 64/64 | +0 | `PASS 17/17 families, 0 mismatches` |

All four new families — F14 `LEA`, F15 `CLR`, F16 `EXT`/`EXTB`, F17 register-count shifts —
passed on silicon at the first attempt.

**Boot windows, all eight, against the pre-registered 45 … 50 s:** `R` 45 s · A0 **48 s** ·
A1 48 s · A2 49 s · A3 47 s · A4 46 s · A5 46 s · prof 78 s.

**R6 — the compile bracket, and an independent corroboration of §0.15.2:**

```
A0 lean 7A5EC595 : 902 gtick/blk = 1805 cyc/blk = 361 cyc/compiled-insn
prof    6973E8B5 : 912 gtick/blk = 1825 cyc/blk = 365 cyc/compiled-insn
-> the two builds agree to 1.1 %   (round 4: 7.7 %;  round 3: 4.7 %)
```

**Round 5's lean is 6.9 % faster per block than round 4's lean, with four more emitter families
in the decode chain — a direction that should not happen.** Round 4's *prof* (1 799 cyc/blk)
sits within 1.4 % of round 5's *prof* (1 825), while round 4's *lean* (1 938) is the outlier of
the four measurements. **The anomalous member of the set is the round-4 LEAN image — exactly the
image §0.15.2's byte search proved is not what its source builds.** Recorded as corroboration,
not proof; the byte search is the proof.


## 0.17 M1 round 6 — THE TAX FIX, pre-registered before the first line of round-6 code (2026-08-24)

**This section is written before any round-6 code and is scored as written.** §3.4's two rules
govern it exactly as they governed §0.6, §0.8, §0.10, §0.12 and §0.14: the pre-registration
commit precedes the implementation commits, provably, by commit timestamp in this repository;
and a pre-registration that gets rewritten is not one, so every number below is left
byte-untouched and scored beside itself.

**Round 6 is NOT routed by §0.5.2's decision table.** It is routed by the user's ruling on the
M1 fork, taken on **2026-08-24** against `docs/warp-gen2-prospectus.md` §8's five options:
**PARK + TAX FIX**. Generation one freezes at its measured scope and the prospectus's §6 is
landed, so that an engine which ships dormant also ships **free**. **No family is added, no
coverage is bought, no verdict is taken, and the kill line's precondition is not approached.**
Round 6 is the round that stops, and §0.19 is where it stops.

### 0.17.0 The two rows the fix is aimed at already exist, and they were flown

§0.16.2 measured the disarmed tax at **−3.445 %** and bisected it into four named layers.
`L-HOOK` — the run loop's `if (z3660_warp_on) … z3660_warp_step()` gate and the loop body
restructured around it — carries **+1.967 pp**. `L-REG` — `warp_on` sitting *inside*
`struct regstruct`, which moves the displacement of every member after it across **7 910**
function bodies (31.5 % of the image) — carries **+0.929 pp**. Together they are **2.896 pp of
the measured 3.445 pp**; the 0.549 pp left over is **A4's own penalty, to three digits, because
A4 is exactly that image**.

| the target row | CRC | dhry | penalty vs `R` = 8 185 | what it is |
|---|---|---|---|---|
| **A4** | `459E51A3` | **8 140** | **−0.550 %** | `L-HOOK` and `L-REG` both removed — **what a fixed image must reproduce** |
| **A5** | `BBED0509` | **8 163** | **−0.269 %** | the engine gone entirely — **the ceiling** |
| A0 (R5's shipping lean) | `7A5EC595` | 7 903 | −3.445 % | what the card refused, three rounds running |

**This is the rare estimate that is `measured` rather than `derived`, and §6.3 of the
prospectus says so in those words: the only thing the fix has to reproduce is a row that
already exists.** A4 and A5 flew in the same session, on the same guest, against the same
`R` — which also means **the A4 row is layout-controlled by construction**, and that is the
whole reason this fix can be verified by an A/B against it rather than by an argument.

### 0.17.1 THE FIX, MECHANISM FIRST — stated before it is built, in the form it is scored in

**`L-HOOK` — two run loops, and the disarmed one has no hook COMPILED IN.**

> The disarmed path must not run a *cheaper* gate. It must run a loop that **does not contain
> one**, byte-for-byte the shape A3/A4/A5 compile — because that shape is the one that measured
> 8 140 and 8 163, and anything else is a new measurement rather than a reproduction of an old
> one.

The mechanism, in four parts, each chosen against a named alternative:

1. **One body, two instantiations, by the PREPROCESSOR — not by the optimiser.** The loop body
   moves verbatim into `src/uae/newcpu_mmu040_body.h`, which `newcpu.cpp` `#include`s **twice**,
   once with `Z3660_RUNLOOP_WARP 0` and once with `1`. The existing
   `#if !Z3660_WARP_ABL_NO_HOOK` guards become `#if Z3660_RUNLOOP_WARP` and nothing else moves.
   *The alternative that is refused: one `always_inline` body taking a `const int warp` and
   trusting `-Os` to fold the dead arm.* That would make "no hook compiled in" a claim about
   gcc's constant propagation, and **§0.15.2 is this campaign's standing proof that a claim
   about what a build produces has to be read off the artifact**. A `#if` cannot be
   mis-optimised. (The file is a `.h` deliberately: `Z3660_emu/Makefile` collects sources with
   `find … -name '*.cpp'`, so a `.cpp` body would also be compiled standalone.)
2. **Selection happens ONCE, at the arm/disarm transition — never per pass and never per
   dispatch.** `m68k_go`'s `run_func` is already re-selected on every run-loop return; the
   68040 arm gains the variant choice, and `z3660_knobs_poll()`'s WARP block sets
   **`SPCFLAG_MODE_CHANGE`** on a transition so `do_specialties()` returns 1 in the same call,
   the loop returns, and `m68k_go` re-selects. That is UAE's own meaning for that flag — *the
   run function changed* — and the transition path (core0 writes the mailbox, core1's resync
   reads it) is the **only** way the arm state can move while a loop is executing.
3. **`m68k_run_mmu040` stays the exported name and becomes the SELECTOR**; the two bodies are
   `m68k_run_mmu040_nowarp` and `m68k_run_mmu040_warp`, both `noinline` so each keeps its own
   symbol for the gate to read. *The alternative that is refused: renaming the plain body to
   `m68k_run_mmu040` and teaching the twenty harness call sites which variant to drive.* That
   arrangement has exactly one failure mode and it is the one §0.13.5 named — **an armed test
   that silently runs the disarmed loop reports CAUGHT-shaped silence**. With one selector,
   firmware and harness enter through the same door and the arm state decides, once, in one
   place.
4. **No indirect branch is added to any hot path, and that is the A9-specific half of the
   choice.** §1.6's central mechanism is that this core has a **one-target BTB** against a
   1 867-target interpreter dispatch; a per-pass function-pointer call through a variable would
   put a *second* mispredicting indirect branch on the disarmed path to save a compare. The
   fix's dynamic cost on a disarmed pass is **zero instructions** — there is nothing there —
   and the selector's own load-test-branch is paid **once per run-loop entry**, which during an
   AMIX boot means on a CPU-prefs change or a halt and essentially never otherwise.

**`L-REG` — `warp_on` moves to the END of `struct regstruct`.**

`sizeof(struct regstruct)` is **464 B** (`nm -S`, both R5 builds), and `warp_on` currently sits
at **offset 96**, which is why removing it re-encodes 7 910 bodies. Appended after the last
member — after the `#ifdef JIT` block — **no existing member's displacement moves at all**, and
the field is still `ldr rX,[r4,#imm12]` off the base the loop already holds, which is §0.12.1's
entire reason for putting it in the struct. **The `imm12` range is the one thing that could
turn this into round 3's arrangement by accident**, so the fix carries a build-time
`static_assert` that the offset stays below 4 096 and the gate prints the displacement it
actually got.

> ### **WHAT THE FIX MUST NOT DO, and it is a gate rather than an intention: it must not change ARMED semantics.** The armed loop is the same body through the same `#include`, so `WARPTEST` must still return **17/17**, the lockstep must still be **0 divergences over ≥ 10⁸** retired guest instructions, and the accept set must still be **18 368** with symmetric difference **0**. The armed path is *restructured by the same change and is not what this round measures* (§6.4 risk 3) — so it is gated instead.

**The one cost the fix adds, priced before it is built.** A fixed image keeps the ability to
arm, so it carries **both** run loops' link footprint — roughly `+2 104 B` of `.text` against a
12.68 MB image. §6.3 names that class and measures it: it is an `L-WTST`-class cost, **0.024
pp**, inside the 0.513 pp floor. It is charged here rather than discovered later.

### 0.17.2 THE PRE-REGISTERED METAL BANDS — written now, whether or not a session is ever bought

**Round 6 flashes nothing.** The fix is built, gated and archived; the metal row is a separate
purchase and this section is what it would be scored against, written before the code exists so
that it cannot be written after the number.

| row | **pre-registered band** | point | derivation |
|---|---|---|---|
| **disarmed, the shipping lean** | **8 100 – 8 163**, i.e. penalty **−1.04 % … −0.27 %** | **8 140** | **the A4–A5 class, measured.** A4 = 8 140 is the floor's anchor and A5 = 8 163 the ceiling; the band's bottom is A4 less one 0.513 pp floor-width |
| **THE BLESSING GATE ≥ 7 988** | **MET** | — | **and this is the first round that predicts it.** The class was missed three consecutive rounds (−4.056 %, −4.256 %, −3.445 %); A4 clears it by **152** and A5 by **175** |
| **armed** | **7 448 – 7 700**, point **7 563** | **7 563** | **unchanged semantics ⇒ unchanged row.** R5 measured 7 563 = 0.92401×; `L-REG`'s 0.929 pp is a whole-image cost the armed row pays too, so the band is R5's row with the layout term as headroom at the top |
| **boot, disarmed** | **45 … 47 s** | 46 s | A4 and A5 both booted **46 s**; `R` booted 45 s. Narrowed from §0.14's 45 … 50 because the hook is gone from the booting path |
| **coverage `c`** | **0.200 – 0.206** | 0.2026 | no family moves, so this is a *control*: a miss means something changed that was not supposed to |

```
   THE DECISION RULE FOR THE DISARMED ROW, pre-registered:

   1. THE FLOOR is re-established in the same session, from the same A2 edge
      (|penalty(A0') - penalty(A2')|).  R5 measured it at 0.513 pp.  No edge and no
      claim smaller than the session's own floor is read as anything.

   2. THE FIX SUCCEEDS if the disarmed row lands in the A4-A5 class AND the >= 7 988
      class is met.  Both, not either.

   3. THE FIX FAILS if the disarmed row is below 7 988.  That is not a partial result
      and it is not re-banded afterwards: L-HOOK + L-REG were measured at 2.896 pp of
      3.445 pp on this exact image, so a miss says the two layers do not compose --
      which would be a FINDING, and it would say the additivity gate of S0.16.2 rule 5
      held on removals but not on repairs.

   4. A ROW BETWEEN 7 988 AND 8 100 is a HIT on the class and a MISS on the band, and
      it is reported as both.  The class is what the card turns on; the band is what
      this section claimed.

   5. ARMED SEMANTICS ARE NOT A BAND.  WARPTEST 17/17 or the round does not ship,
      independent of every number above.
```

> ### **THE PREDICTION THIS ROUND IS MOST LIKELY TO BE WRONG ABOUT, named before the row.** §0.14.9 item 5 stands unchanged and it cuts here: *"every edge above the floor is a property of THIS build of THIS toolchain at `-Os -flto`. A layout result is not a portable one."* The fix removes the two layers **on the same toolchain, in the same tree, at the same flags** — which is the only reason the A4 row is usable as a target at all — but the ambient churn §0.15.3 measured is **~460 re-decided function bodies for any change whatsoever**, and this change is not exempt from it. **The band's width, 63 dhrystones, is narrower than that churn is entitled to be**, and if the row lands outside it while still clearing 7 988, the honest reading is the churn and not the fix.

### 0.17.3 The round-6 gates, with their predictions

| gate | pre-registered |
|---|---|
| **G-R6.1** harness green, all three variants | `0 failed`; totals **within ±40** of 3 334 / 3 330 / 3 561, and any delta the **same** in default and lean |
| **G-R6.2** the accept-set discriminator | engine == `docs/warp-corpus.py` gen 5 **exactly**; **18 368** both sides; symmetric difference **0**. The fix touches no emitter, so this is a control |
| **G-R6.3** the sweep and the differential | **≥ 108 000** armed/disarmed pairs over the accepted opcodes, **0** divergences |
| **G-R6.4** lockstep, the armed path | `WARP_DIVERGE == 0` over **≥ 10⁸** retired guest instructions — **the unchanged-armed-semantics proof** |
| **G-R6.5** `WARPTEST` | **PASS 17/17 families, 0 mismatches**, F1's 64/64 alignment sweep still **+0** range misses |
| **G-R6.6** THE DISARMED LOOP HAS NO HOOK, statically | `m68k_run_mmu040_nowarp` contains **no gate** by `tools/warp-gate.sh`'s shape test, **and** names **no `z3660_warp_*` symbol at all**; `m68k_run_mmu040_warp` contains exactly **one** gate with **one** load. The script **exits non-zero** if either half fails |
| **G-R6.7** OBJECT EQUIVALENCE — the fixed disarmed loop **is** the row that measured 8 140 | `m68k_run_mmu040_nowarp` in the fixed lean, normalised by `warp-objdiff.py`, is **identical** to `m68k_run_mmu040` in R5's `459E51A3` (A4) build — **0 instruction differences**. Scored against A5 `BBED0509` as well |
| **G-R6.8** THE LAYOUT TERM IS GONE, statically | whole-image `warp-objdiff.py`, fixed lean vs R5's A5: **≤ 1 200 changed bodies, expected 400 – 700** — the ambient-churn class §0.15.3 measured at ~460 — against the **7 910** that the `regstruct` field cost when it sat at offset 96 |
| **G-R6.9** the `regstruct` displacement | `warp_on` is the **last** member; **no existing member's offset moves**; its own offset is **< 4 096** and is a plain `ldr rX,[rN,#imm]` in the armed loop, **zero address materialisations**. Build-time `static_assert` |
| **G-R6.10** the ablation kit still builds and its `L-REG` edge is now NULL | all five variants build from one tree; **A3→A4 is predicted to become a second negative control** — with the field at the tail, removing it moves nothing, so the edge that measured 7 910 bodies should now measure the ambient class |
| **G-R6.11** images | the shipping trio plus five ablation variants, CRCs recorded, archive fires, **ELF archived beside every BIN**; `q2probe` untouched at `FCC0CB3F` |
| **G-R6.12** the gate runs on every build | `tools/warp-gate.sh` is invoked by the `lean` / `bootbin` / `profile` / `abl%` targets and **fails the build** on a violation — an image whose disarmed loop carries a hook cannot be produced silently |

### 0.17.4 What round 6 explicitly does not establish

1. **Any metal row.** Round 6 flashes nothing. §0.17.2's bands are unmeasured and stay
   unmeasured until a session is bought; **the card's standing image is unchanged.**
2. **A verdict.** Coverage does not move, the kill line's precondition stays unmet for a fourth
   wave, and the kill line is neither fired nor cleared. **It never becomes entitled to fire**,
   and §0.19 records that as the campaign's outcome rather than as a gap.
3. **A mechanism for the residual.** `L-HOOK` names a **layer**; §0.14.9 item 2 stands. The fix
   removes a layer that was measured; it does not explain *why* four ARM instructions on the
   disarmed dispatch path cost 1.967 pp, and that question is closed unanswered.
4. **That the fix transfers.** Same toolchain, same flags, same tree. §0.14.9 item 5 unchanged.
5. **Anything about generation two.** No family, no contract, no chaining, no supervisor
   coverage. `docs/warp-gen2-prospectus.md` prices those and decides none of them.
6. **Everything §0.8.8, §0.10.7, §0.11.7, §0.12.8, §0.13.6, §0.14.9 and §0.15.7 said would not
   be established**, unchanged — including the memory-touching families on silicon (§0.15.7
   item 4), the indexed `LEA` modes, and `z3660_warp_run`'s preamble.


## 0.18 M1 round 6 — built, and its gates scored (2026-08-24)

**The pre-registration in §0.17 was committed at `bed3e41`, before the first line of round-6
code; the implementation commits follow it in this repository's log.** Every prediction in
§0.17 is left byte-untouched above and scored beside itself.

### 0.18.1 What was built

| piece | where |
|---|---|
| **A** the loop body, extracted verbatim and included twice | `src/uae/newcpu_mmu040_body.h` (new), `src/uae/newcpu.cpp` |
| **B** the selector, and `SPCFLAG_MODE_CHANGE` at the arm/disarm transition | `src/uae/newcpu.cpp` — `m68k_run_mmu040()`, `z3660_knobs_poll()` |
| **C** `warp_on` moved to the end of `struct regstruct`, with the `imm12` `static_assert` | `src/uae/include/newcpu.h` |
| **D** the ablation kit's post-fix meaning for levels 3 and 4 | `src/uae/z3660_warp_ablate.h` |
| **E** `tools/warp-gate.sh` turned into a **build gate** that exits non-zero, wired into every core1 target | `tools/warp-gate.sh`, `vitis_ide/Makefile` |
| — | **`z3660_blk_run` gets `always_inline`**, which was NOT in §0.17 and is there because §0.18.2 happened |
| — | `docs/warp-objdiff.py` gains `--alias` / `--only` / `--self`, so a **renamed** function can be compared with the body it replaced |

### 0.18.2 THE FINDING THE FIX PRODUCED ON ITS FIRST BUILD, AND IT IS AN INLINING ONE

**Splitting the loop in two gave `z3660_blk_run` a second caller, and `-Os` immediately
stopped inlining it.** Until round 6 that function had exactly ONE call site, so
`-finline-functions-called-once` inlined it at no size cost — and **every image whose disarmed
row this campaign has ever measured (`7A5EC595`, `5E2592EF`, `459E51A3`, `BBED0509`) carries it
inlined into the run loop.** The first build of the fix put a call and a return on rung 0's
recognizer path, in both loops:

```
   A4 459E51A3, the loop that measured 8 140 : calls uae_mmu040_get_iword x7, put_long x2,
                                               z3660_blk_probe x2, flush_cpu_caches_040,
                                               flush_mmu040, flush_icache, get_long ...
   the fix, first build                      : calls z3660_blk_run.constprop.0  -- ONE call,
                                               and every one of the above moved inside it
```

> ### **THIS IS §0.15.3's AMBIENT CHURN LANDING ON THE ONE PATH THE ROUND IS TRYING TO HOLD STILL.** *"Under `-Os -flto`, ANY change to this image re-decides inlining in about 460 function bodies"* — and a change that duplicates a large function re-decides them where it hurts. **The fix's entire verification is an A/B against A4's measured 8 140, and an A4 row is only a target if the image being compared to it has A4's loop.** So the inline is restored by `always_inline`, **by construction rather than by a heuristic whose input this round deliberately changed**, and the loop's call-target census then matches A4's exactly — same twelve targets, same multiplicities. It is recorded here rather than absorbed, because the next person to duplicate a hot function will meet it again.

### 0.18.3 The two loops, read off the ELF

```
   selector m68k_run_mmu040        :   28 B   ldr / ldr / cmp / beq / b warp / b nowarp
   disarmed m68k_run_mmu040_nowarp : 2100 B   NO GATE, and no z3660_warp symbol anywhere
   armed    m68k_run_mmu040_warp   : 2184 B   ldr r3,[r4,#464] / cmp r3,#0 / beq
                                              one load, off a live base, zero materialisations
```

**`sizeof(struct regstruct)` 464 → 472; `warp_on` is the last member, at displacement 464
against the `imm12` limit of 4 095**, asserted at build time and printed by the gate.

> ### **AND THE CLAIM THAT MATTERS IS EXACT: the shipping lean's disarmed run loop is normalised-IDENTICAL — 0 instruction differences — to the run loop in `BOOT-abl5`, an image built with the engine ENTIRELY ABSENT.** Not similar, not equivalent-modulo-a-gate: the same code, checked by `warp-objdiff.py --only` against all five ablation variants and coming back **0** on every one. The disarmed path of an image that can arm is, instruction for instruction, the path of an image that cannot.

### 0.18.4 The gates

| gate | pre-registered | measured | verdict |
|---|---|---|---|
| **G-R6.1** harness, three variants | `0 failed`; within ±40 of 3 334 / 3 330 / 3 561, same delta in default and lean | **3 334 / 3 330 / 3 561, `0 failed`** — delta **+0 / +0 / +0** | **HIT**, exactly. The first round whose count estimator was not wrong, because for once it predicted no change |
| **G-R6.2** accept-set discriminator | engine == model gen 5; **18 368**; symmetric difference **0** | engine **18 368**, model **18 368**, **agree on 65 536 of 65 536**, 0 in both directions | **HIT** — the control held; no emitter moved |
| **G-R6.3** sweep and differential | ≥ 108 000 pairs, **0** divergences | **110 208 pairs over 18 368 accepted opcodes, 0 divergences**; plus **1 080** shift-count pairs, 0 divergences | **HIT** |
| **G-R6.4** lockstep, armed | `WARP_DIVERGE == 0` over ≥ 10⁸ | **0 over 100 000 000 guest instructions, 12 500 000 block executions, 100.0 % retired inside blocks** | **HIT — the unchanged-armed-semantics proof** |
| **G-R6.5** `WARPTEST` | PASS 17/17, F1 64/64, **+0** range misses | **`PASS 17/17 families, 0 mismatches`**; F1 **64/64**, **+0** | **HIT** |
| **G-R6.6** disarmed loop has no hook | no gate by shape, **no `z3660_warp_*` symbol at all**; armed loop has exactly one gate with one load; script exits non-zero on a violation | both halves clean on all eight builds; **and the guard was fired on purpose**: pointed at the hooked loop it printed both failures and **exited 1** | **HIT**, and the guard is demonstrated rather than asserted |
| **G-R6.7** object equivalence with the row that measured 8 140 | `nowarp` **identical** to A4 `459E51A3`'s `m68k_run_mmu040`, **0** differences | **526 → 525 instructions, 2 104 → 2 100 B.** Difference in full: two spill slots swapped (`#44`↔`#52`, 6 of the 13 multiset entries, pure renaming), one basic-block reordering that folded a `b`+`bne` into a `beq` (**−1 instruction**), and one `mov r0,r3` → `ldr r0,[sp,#12]` on the `z3660_blk_probe` call. **Call-target census identical: same 12 targets, same multiplicities.** | **MISSED — and the miss is characterised to the instruction.** Three real differences, none on the per-dispatch path. See §0.18.6 |
| **G-R6.8** the layout term is gone | ≤ 1 200 changed bodies, expected 400 – 700, against 7 910 | fixed lean vs A5 `BBED0509`: **466**. The same census on **R5's** lean vs its A5: **7 912** | **HIT, and inside the expected sub-band.** 7 912 → 466 is a **17× collapse** onto the ambient-churn class (457 / 459 / 462 / 464) |
| **G-R6.9** the `regstruct` displacement | last member; no existing offset moves; **< 4 096**; plain `ldr`, zero materialisations | `warp_on` at **464**, `sizeof` 464 → 472, gate reads `ldr r3,[r4,#464]`; `static_assert` in `newcpu.h` | **HIT** |
| **G-R6.10** the kit's `L-REG` edge is now null | A0′→A3 and A3→A4 collapse to the noise class | **A3→A4 = 9 changed bodies** (was **7 910**), with `.text` **identical** at 6 342 604 both. A0′→A1 455, A0′→A2 455, A0′→A3 458, A0′→A5 466 | **HIT, emphatically.** The edge that carried the layout term carries nine functions |
| **G-R6.11** images | trio + five variants, CRCs, archive fires, ELF beside every BIN; `q2probe` untouched at `FCC0CB3F` | eight built from one tree after `clean-all`, all CRC-stamped, **ELF archived beside every one** (§0.18.5); `BOOT-q2probe.BIN` untouched at **`FCC0CB3F`** | **HIT** |
| **G-R6.12** the gate runs on every build | invoked by `lean`/`bootbin`/`profile`/`abl%`, fails the build | wired via `warp_gate` in `vitis_ide/Makefile`; it ran on all eight core1 links in this round's own logs | **HIT** |

**Eleven of twelve gates hit.** G-R6.7 is the miss, it is the one §0.17.2 named in advance as the
thing this round was most likely to be wrong about, and §0.18.6 is what it costs.

### 0.18.5 THE IMAGE MANIFEST — the matrix as rebuilt, before any metal row exists

**All eight images built from one tree after `make -C Z3660_emu clean-all`, CRC-stamped, and
archived with their core1 ELF beside them.**

| variant | CRC-32 | image B | `.text` | `nowarp` | `warp` | `sizeof(regs)` |
|---|---|---|---|---|---|---|
| `BOOT.BIN` (default) | **`15A7FF0C`** | 12 680 188 | — | 2 100 | 2 184 | 472 |
| **`BOOT-lean.BIN` — A0′, the shipping lean** | **`9AECF99D`** | 12 680 156 | 6 347 084 | **2 100** | **2 184** | 472 |
| `BOOT-prof.BIN` | **`9D07C731`** | 12 696 700 | 6 371 700 | 1 276 | 1 340 | 472 |
| `BOOT-abl1.BIN` − `L-WTST` | **`BFC9FC47`** | 12 680 108 | 6 342 228 | 2 100 | 2 184 | 472 |
| `BOOT-abl2.BIN` − `L-ATT` (the floor) | **`E2C1FBC5`** | 12 680 156 | 6 347 020 | 2 100 | 2 184 | 472 |
| `BOOT-abl3.BIN` − the hooked loop | **`82D6B35C`** | 12 680 116 | 6 342 604 | 2 100 | absent | 472 |
| `BOOT-abl4.BIN` − that and the field | **`C8AAB523`** | 12 680 116 | 6 342 604 | 2 100 | absent | **464** |
| `BOOT-abl5.BIN` − the engine entirely | **`BEC0D2AA`** | 12 663 644 | 6 326 472 | 2 100 | absent | 464 |
| `BOOT-q2probe.BIN` | `FCC0CB3F` | 12 795 260 | **untouched**, as gated | — | — | — |

**The second run loop's price, measured rather than estimated:** `.text` 6 344 904 (R5's A0) →
**6 347 084**, **+2 180 B** on a 12.68 MB image, against §0.17.1's pre-registered `+2 104`-ish
and §6.3's `L-WTST`-class **0.024 pp**. **Nothing was flashed.** The card's standing image is
unchanged.

> ### **AND THE ONE PROPERTY §0.15.2 PROVED ROUND 4 DID NOT HAVE IS CHECKED HERE RATHER THAN ASSUMED: `9AECF99D` IS WHAT ITS SOURCE BUILDS.** A `clean-all` and a rebuild from the committed tree produce a core1 ELF **byte-identical** to the archived `BOOT-lean-9AECF99D.elf`. Round 4 shipped an image whose source produces a different run loop, measured a penalty on it, rolled a card back on it, and needed a byte search on the artifact to find out. This round asked the question before booking anything, and `tools/warp-gate.sh` now asks it on every build.

### 0.18.6 What G-R6.7's miss costs, stated rather than absorbed

**The band in §0.17.2 is 63 dhrystones wide and the loop is not byte-identical to the one that
measured 8 140.** Three real instruction differences survive the fix:

1. **Two spill slots swapped.** `#44` ↔ `#52`. Six of the thirteen multiset entries, and it is
   a renaming — same stores, same loads, same frame size.
2. **One basic block reordered**, folding a `b` and a `bne` into a `beq`. The fixed loop is
   **one instruction shorter** for it. This is on the decoded-op-cache miss path.
3. **One `mov r0,r3` became `ldr r0,[sp,#12]`** — a register-allocation choice on the
   `z3660_blk_probe` call, i.e. rung 0's MOVE-idiom probe, not the per-dispatch path.

> ### **SO THE 8 140 ROW IS A TARGET, NOT A GUARANTEE, AND §0.17.2's DECISION RULE ALREADY SAYS WHAT TO DO ABOUT THAT.** Rule 4 is the live one: a row between 7 988 and 8 100 is a HIT on the class and a MISS on the band, reported as both. The static case for the fix does not rest on this gate — it rests on **G-R6.8's 7 912 → 466** and on the disarmed loop being **normalised-identical to the engine-free image's loop** (§0.18.3), both of which are exact. What G-R6.7 says is narrower and worth saying: *the fix reproduces A4's code, not A4's binary.*

### 0.18.7 What round 6 leaves owed

1. **Every metal row.** §0.17.2's bands are unmeasured. **Nothing was flashed and nothing is
   proposed**; the disarmed band, the blessing class, the armed row and the boot window are
   what a session would be scored against if the park is ever reopened for one.
2. **The floor must be re-established in that session.** §0.17.2 rule 1: `A0′` against
   `BOOT-abl2` `E2C1FBC5`, in the same session, before any edge is read.
3. **Everything §0.15.7 and §0.17.4 left owed**, unchanged — the memory-touching families on
   silicon, the indexed `LEA` modes, `z3660_warp_run`'s preamble, and the mechanism behind
   `L-HOOK`'s 1.967 pp, which this round **removed without explaining**.


## 0.19 M1 CLOSED — **PARKED AT GENERATION-ONE SCOPE**, by user decision 2026-08-24

**The kill line never became entitled to fire, and that is the outcome rather than a gap.**
§0.2 bound this plan to a pre-registered kill condition — *armed throughput at or below
`R×(1+s)` once coverage clears 60 %* — and its **precondition was never met**: coverage
measured **8.43 %**, **19.10 %** and **20.26 %** across three metal waves against a 60 % bar.
§0.5.2's decision table has therefore read *"not a verdict in either direction"* for three
consecutive waves, and it still does. **The plan is not being abandoned and it is not being
vindicated. It is being parked with its instruments intact.**

### 0.19.1 The final scorecard

| | |
|---|---|
| **armed trend, four rounds** | **5 301 → 7 185 → 7 536 → 7 563** dhrystones, ending at **0.92401×** of round 5's same-session `R` = **8 185**. *Each row is quoted against its own session's reference and the ratios are the comparable quantity:* round 2's 5 301 is the **null-block** row — the entry sequence's arrival, in full, with no code generated — then **0.8778× → 0.92432× → 0.92401×**. **Rising, and it never crossed 1.0×** |
| **coverage** | 8.43 % → 19.10 % → **20.26 %**. §0.14.8's honest reading stands: *generation one's frozen scope has a ceiling around a quarter of this workload, and the 60 % bar is a different engine, not three more families* |
| **correctness** | **five metal sessions, zero correctness failures.** `WARPTEST` **17/17** on every image it was run on; `WARP_DIVERGE` **0** over 10⁸ retired instructions every round; `FAULTS(on) == FAULTS(off)` exactly (591 == 591); budget closure unattributed **0.000 %** against a ≤ 5 % gate; the accept set **18 368 opcodes** agreeing with its independent model on **65 536 of 65 536** words |
| **the kill line** | **armed, never fired, never cleared.** Its precondition was unmet in every wave. §0.2's guard — *a miss at 20 % coverage is a bug report, not a verdict* — is what kept it holstered |
| **the dormant tax** | measured at −3.445 %, **bisected** (§0.16.2), and **removed** (§0.18). The engine ships dormant and, on every static measure available without metal, **free** |
| **what was refuted, in public** | three named mechanisms for the disarm residual — L1I footprint (§0.11.4), dependency depth (round 4), and the pre-registered favourite `L-REG` at 1.8 – 3.4 pp (§0.16.2, measured 0.929) — plus §0.13's memory-helper bail candidate, refuted at three digits |

### 0.19.2 What the park preserves, and it is deliberately everything

- **The engine ships**, compiled in, `WARP` unset = **OFF**, and the disarmed path is the
  engine-free run loop instruction for instruction (§0.18.3).
- **`WARPTEST` stays.** 17 families, the F1 alignment sweep, the mutation arms, the ARM word
  executor. It is the on-target proof that an image *could* be armed safely, and it costs a
  disarmed boot nothing.
- **The accept set stays** — 18 368 opcodes, with `docs/warp-corpus.py` as its independent
  model and the discriminator gate that compares them over all 65 536 words.
- **The ablation kit stays**, and the fix turned it into its own verifier: levels 1, 2 and 5
  are the control, the floor and closure that any re-measurement session needs, and levels 3
  and 4 now read ~0 by construction (§0.18.4 G-R6.10).
- **`tools/warp-gate.sh` stays, as a build gate.** The one thing a parked engine cannot
  tolerate is silently growing a cost again; this refuses the build instead.
- **The pre-registrations stay, unrewritten.** §§0.5 – 0.17 are the record of what was
  predicted before each round, including the four times it was wrong.

### 0.19.3 WHAT REOPENING REQUIRES — and the order is the finding

**`docs/warp-gen2-prospectus.md` prices the alternatives and recommends none of them. Its own
§9.1 is binding on any restart: this document's bands are not a pre-registration, and any
funded scenario gets its own §0.17-style pre-registration first.** Beyond that, the
prospectus's arithmetic says one thing loudly enough to record here:

> ### **AN `E`-ROUND COMES FIRST, AND FAMILIES COME SECOND.** The prospectus's §8 closing row is explicit that *"funding families before funding `E` is the expensive order"*: at the measured block-entry cost `E` = 112 cycles, full G2c reaches a ceiling of **1.131×** and §1.5's first-generation band of **1.25× – 1.55×** is **out of reach at any coverage the document prices** — whereas `E` = 40 alone reaches **1.209×**, and `E` = 0 reaches **1.257×**. The gap between every model band and its own Amdahl ceiling is **950 – 1 790 dhrystones**, and the block-entry term is **71 – 85 %** of it. **A restart that adds families without first attacking `z3660_warp_run`'s preamble buys the cost and not the payoff.**

And one nuance from §6.1 that a restart must carry rather than inherit silently: **the
`L-HOOK` naming verdict rests on two bars and only one of them fires.** Half of A0's own
same-session penalty is 1.7227 pp and `L-HOOK` measured 1.967 — it names the layer. Half of
the *pre-registered* figure (round 4's −4.256 %) is 2.128 pp, and 1.967 **misses it by
0.161 pp** against a floor of 0.513 pp. **The applied reading governs and the pre-registered
one stands beside it, unrewritten** (§0.16.2). The fix targeted both layers regardless, which
is why §6's pricing stands either way — but a restart that treats `L-HOOK` as a settled
mechanism rather than as a named layer is reading more out of this campaign than it put in.

### 0.19.4 The park's own status line

> **`M1 PARKED`.** No further rounds are scheduled. The card's standing image is unchanged and
> nothing from round 6 has been flashed. §0.17.2's metal bands are written, pre-registered and
> unmeasured; if a session is ever bought, that is what it is scored against, and §0.18.7 is
> the list of what it must re-establish first.

### 0.19.5 PARK ADDENDUM — the gate met its second compiler, and one row of §0.18.5 was wrong (2026-08-24)

**`make bootbin` through the Vitis docker failed G-R6.6 on the ARMED loop, and the loop was
fine.** The gate is there and calls `z3660_warp_step` two instructions later; gcc 12.2.0
hoists that call's argument load (`regs.instruction_pc`, displacement 88) between the flag
load and the compare, where gcc 14.2.1 sinks it below the branch. `tools/warp-gate.sh` matched
the gate on three FIXED lines after the handler's `blx r3`, so an instruction scheduled into
the middle of it moved the compare to line 3, the branch to line 4, and the script reported a
loop that plainly engages the engine as having no gate at all.

**It was wrong in the dangerous direction too, which is the part worth keeping.** Pointed at a
hooked loop through the DISARMED symbol, the old matcher on that same gcc-12.2.0 image printed
`no gate after the handler returns -- as designed`. The build still failed — but only because
the independent "names no `z3660_warp` symbol" test caught it. The shape half of G-R6.6 was a
false pass, on the one variant the property matters most for.

> ### **AND THE VARIABLE WAS NEVER RECORDED ANYWHERE: every ELF this campaign archived — all ten — was built by the HOST `arm-none-eabi-gcc` 14.2.1, while the project notes documented the Vitis docker (gcc 12.2.0) as the build path.** Not one measured row in this document is a docker build. The first one ever made was the one that failed the gate, and there was no way to see that from a manifest row, a CRC or a build log. Gate output now names the producer of every ELF it reads, and `make gate` re-reads every core1 build directory at once instead of checking a variant only at the instant it is linked.

**§0.18.5's `BOOT.BIN` row is wrong in two cells.** The default variant's loops are **3 204 /
3 284 B**, not 2 100 / 2 184 — those are lean's, transcribed. The CRC, the image size and
`sizeof(regs)` are right, and the image is sound: a clean rebuild at `43aa971` reproduces
`BOOT-15A7FF0C.elf` **byte-identically** and it passes the gate. **G-R6.12 is unaffected** —
the gate did run on that link and did pass; only the two cells copied into the table are wrong.

**Nothing about the engine, the loops or any measured row moves.** All three variants' core1
ELFs rebuild byte-identical to their round-6 archive entries (`15A7FF0C`, `9AECF99D`,
`9D07C731`), the harness is unchanged at **3 334 / 3 330 / 3 561, 0 failed**, and `M1 PARKED`
stands. What changed is the gate's shape test, restated as the property it always meant: the
flag must be reached in **one load off a live base**, with no address materialised first —
which is what round 3 actually violated, and what a surplus scheduled load never was.


---

# 1. Goal, non-goals, and honest bounds

## 1.1 The goal

**Execute AMIX-040 guest code as generated ARM code while the guest's 68040 MMU is on, with the
interpreter kept permanently as both the fallback path and the differential oracle, without
giving up a single correctness property the interpreter campaign established.**

Two words in that sentence carry the whole design.

**"Permanently."** The interpreter is not scaffolding to be removed. It is the definition of
correct behaviour for this emulator, it is the only path that has ever booted AMIX on this card,
and it is what every engaged/decline corpus in `test/host` measures against. The JIT's job is to
be *faster than* the interpreter on the instructions it chooses to take, and *absent* everywhere
else. Every design decision below that looks conservative is this decision applied.

**"Under the MMU."** This is the part nobody in the UAE family has done. WinUAE and Amiberry both
disable the JIT when a real MMU is modelled; Emu68 never needed one. QEMU's TCG with softmmu is
the existence proof that the combination is possible — it is not a source of portable code, and
we are not porting anything from it. The architecture gets built here.

## 1.2 The one thing this plan found before it was written

**The ARM32 code generator already exists in this tree, is compiled into every image today, and
is excluded from the MMU path by one deliberate decision with the reason written in the source.**

`Z3660_emu/src/uae/jit/` holds `codegen_arm.h` (102 KB of raw ARMv7 emitters),
`compemu_arm.cpp` (1.6 MB of generated per-opcode compilers), `compemu_midfunc_arm2.cpp`,
`compstbl_arm.cpp`, `compemu_support_arm.cpp` (the block manager, code cache and cache
maintenance) and `compemu_fpp_arm.cpp`. `#define JIT` is live at `include/sysconfig.h:22` and
`#define USE_JIT` at `jit/compemu_arm.h:46`. All but three files are compiled into every build
(the Makefile filters `codegen_arm.cpp` and the two `compemu_midfunc_arm*.cpp` because they are
textually `#include`d). `m68k_run_jit()` exists at `newcpu.cpp:4321` and runs today in the
`UAEJIT_030` / `UAEJIT_040` bootmodes with a 32 MB code cache. **(measured)**

It is kept out of the MMU path in two places. `uae_emulator.cpp:1039-1043` passes
`enable_jit = 0` for MMU bootmodes so `cachesize` stays 0, with this comment:

> `enable_mmu and JIT are mutually exclusive (the JIT inlines direct pointers and cannot restart
> on faults); callers pass enable_jit=0 for MMU mode, so cachesize stays 0 below.`

and `newcpu.cpp:6570-6578` puts `mmu_model == 68040 ? m68k_run_mmu040` **ahead of** the JIT arm
in the `run_func` selection, so even a non-zero `cachesize` could not reach it. **(measured)**

Both halves of that comment are true and both are exactly what this plan has to solve. But the
consequence for scope is large and it changes the shape of the project: **the register allocator,
the flag machinery, the block/`blockinfo`/`cache_tags` structures, the code-cache allocator, the
I-cache maintenance primitives and an ARM FPU compiler are all present, built for this exact
silicon.** The question is not "write a JIT". It is "which of these do we keep, and what replaces
the memory model". That fork is **Q1** and it is the first thing the grill has to settle.

> **Settled 2026-08-23 — §0.1 Q1: Route C.** The plumbing is kept, the `jnf_MEM_*` layer is
> replaced, and the per-opcode compilers are inherited family by family only where a family
> survives the bail contract. §0.3(a) put a number on the fork: **79.44 % of the 1 527
> per-opcode compilers in each compile table can reach guest memory**, so inheriting them
> wholesale would have meant inheriting the old memory model wholesale. One structure in the
> list above turned out less reusable than it reads — `cache_tags` keys on a host pointer that
> does not exist under the MMU (§2b, corrected at M0).

## 1.3 Non-goals

- **No 68060 work.** Track E3 brings up `UAE_060_MMU`; this engine touches only the 040 path. The
  one concession is a design preference, not a deliverable: keep the engine's 68k-model
  assumptions in one place so E3 *could* inherit it later (**Q13**).
- **No Amiberry port inside this plan.** Track G's lane is the *interpreter* rungs (0, 1/1c, 2)
  going to the bench fork and, gated, upstream. The JIT crosses to the bench at M-late or not at
  all — §6.1.
- **No guest cache emulation.** Track C's verdict stands: the reference silicon ladder's
  caches-off 68040@33 (5 367 dhrystones) sits right beside our own numbers, and modelling 68040
  caches would make us slower, not faster. The gap closes by cutting interpreter cost per
  instruction.
- **No re-opening the physical-index decoded-op-cache study.** It was struck by measurement
  (`f` = 0.0787 against a 0.35 gate). The JIT's code-cache keying is a *different* question about
  a *different* cache, and §2b says exactly how much of that strike transfers and how much does
  not — but nothing here re-funds the struck study.
- **Not cycle-exact.** The engine keeps the existing `do_cycles`/`adjust_cycles` accounting
  faithful enough that the service cadence and the A3000-SCSI pump keep their timebase (§2g),
  and no more.
- **Not default-on.** Whether a shipped image runs the JIT by default is the user's decision at
  M5, not this plan's.

## 1.4 The bound, quoted correctly

Row **J** of the C2 attack map is *"the residual only a JIT reaches"* — the buckets a JIT can
attack at all: `LOOP` (= `LOOPRES` + `DOPCFIND` + `DOPCFILL` + `BLKREC`) + `FETCHOP` + `FETCHEX` +
`TAILADV` + `TAILPOLL` + `TAILSPEC`. It has been priced twice on two capture bands:

| version | share | at 90 % coverage | **(measured share, derived ratio)** |
|---|---|---|---|
| map v3.3 §39.2 | — | **2.11 – 2.25×** | that capture's band |
| map v3.4 §46.2 | 63.90 – 60.45 % at `[40, 50]`, cadence 4 | **2.19 – 2.35×** | this capture's band |

> **Row J is an UPPER BOUND of 2.11 – 2.35×, and it is never a target.** It assumes 90 % coverage,
> it assumes a JIT recovers essentially all of the row's work, and the map names two directions in
> which it is soft: `TAILADV` is a price artefact that inflates it (it reads 10.57 % at one price
> and 6.17 % at another *within one capture*), and the fetch pair inside it is measured to be
> *partly latency-hidden*, which §30's law says returns nothing when removed. The map's own
> standing instruction — **"stop quoting a dhrystone figure for it"** — is honoured here: no
> dhrystone number is derived from row J anywhere in this document.

What row J does establish, and it is the reason this project exists: after the strike and the ATC
re-price, **everything else classified and attackable on the interpreter ladder compounds to
+0.9 % .. +3.2 %**, against row J's +119 % .. +135 % at the same band — a ratio between **37 : 1**
and **150 : 1**. *"It is not a better rung; it is a different machine."* **(measured, map §46.3)**

## 1.5 A realistic first-generation target band, and how it is derived

Row J is a ceiling on a machine that does not exist. This is the band a *first* generation should
be judged against, built bottom-up from the same capture, with every deflator named.

**Step 1 — the credibly removable base.** Of row J's buckets, take only those a generation-one
JIT actually removes work from, at the same band and window (`[40, 50]`, cadence 4, map §46.1):

```
   DOPCFIND        16.10 .. 17.96 %     the per-dispatch cache walk -- becomes one lookup per BLOCK
   LOOPRES         19.35 .. 16.89 %     run-loop residue -- most of it is per-instruction bookkeeping
   FETCHOP+FETCHEX  7.23 ..  8.60 %     jitted code fetches no guest instructions at all
   TAILPOLL         6.8 %   (c4)        the poll -- moves from per-cadence to per-block
   TAILSPEC         0.2 %               do_specialties -- likewise
   -------------------------------------------------------------------------------
   base            49.68 .. 50.45 %  ->  call it ~50 %                        (derived)
```

**Excluded on purpose, and each exclusion costs the band something:** `TAILADV` (retired as a
price artefact — including it is exactly how row J gets inflated); `DOPCFILL` (4.13 % of the
corrected profile at 0.218 fills/instruction — a JIT does not remove this cost, it *replaces* it
with compilation, which is dearer per event and rarer); `BLKREC` (rung 0 stays — §2a).

> **⚠ One comparability caveat inside the base, declared rather than smoothed over.** The first
> three rows are map v3.4's own shares on one window at one band. `TAILPOLL` (6.8 % at cadence 4)
> and `TAILSPEC` (0.2 %) come from the s19 capture session, not from that window — and *"a share
> swept at one band is not comparable with one swept at another"*, with `INSNS_SUPER` the cheapest
> discriminator between windows. `TAILSPEC` is below the noise either way; `TAILPOLL` is not, and
> map v3.3's own figure for it on a third window is 7.45 – 7.35 %, which brackets the 6.8 % used
> here rather than contradicting it. **The base is therefore ~50 % ± the width of one bucket's
> window disagreement, and the honest way to close it is to re-take all five on one capture at M0 —
> which costs one profiling boot and nothing else.**

> **⚠ CLOSED at M0 by the boot it asked for.** All six rows re-taken on one capture at one band
> (sweep `[40, 51]`): **base = 48.68 – 49.91 %**, which overlaps the stitched estimate at
> 49.68 – 49.91 %. `TAILPOLL` missed high (9.49 – 9.86 % against 6.8 %) and `LOOPRES` missed low
> by about the same amount, so **the two errors largely cancel and the stitched base was right for
> a reason it had not earned**. The caveat is closed with a measurement rather than a hedge, and
> the band below does not move. §0.4.2. **(measured)**

**Step 2 — two deflators, both `assumed`, both directly measurable from day one.**

- **`c` — coverage**, the share of *retired guest instructions* executed inside compiled blocks.
  Anchors that bracket it: the dopc reaches 85.32 % on dhrystone (rung 1c) and 96.36 % on the
  `f` session's pinned window; rung 2's data fast path covers 99.1 %; rung 3's fetch path covered
  91.10 %; the SPEC experiment's single opcode family covered 21.12 %. A first generation with a
  restricted family set and a block that ends at every control transfer: **c = 0.70 .. 0.90
  (assumed)**.
- **`r` — removal fraction** of the ~50 % base *inside* covered instructions. Generation one still
  writes back `instruction_pc` and snapshots flags at every fault-visible boundary, still calls
  helpers for the slow arms, still polls once per block: **r = 0.60 .. 0.80 (assumed)**.

**Step 3 — Amdahl, against the standing 8 185.**

```
   t   = 0.50 x c x r
   low : 0.50 x 0.70 x 0.60 = 0.210  ->  1/(1-0.210) = 1.27x
   high: 0.50 x 0.90 x 0.80 = 0.360  ->  1/(1-0.360) = 1.56x
```

> ### First-generation band: **1.25× – 1.55×**, i.e. **dhry ≈ 10 200 – 12 700** from the standing 8 185. (derived, on two assumed deflators)

Two things must be said beside it, or it is a target chosen after the result.

**This is the removal side only.** It charges nothing for the block-entry lookup, the block
chaining, the I-cache pressure of generated code, or compilation. §30's law is unambiguous about
the first of those: *"work added in front of the path arrives — nothing overlaps a dependency you
put in front"*, and rung 3 measured exactly that (+19 instructions cost 1.58 % of wall clock, in
full). **The cost side is unquantified and it is subtracted from this band, not added to it.**

**The downside case is a null, and it has a precedent on this rig.** Rung 3 worked — 91.10 %
coverage, fetch bucket 23 % cheaper per instruction, every premise at-or-better than predicted —
and moved the wall clock **−0.05 %**. A JIT whose block-entry cost and I-cache footprint eat its
removals lands in the same place, and M1's kill criterion exists to say so within one session.

For orientation only: on the community-relayed silicon ladder a Mercury 68040@33 with icache on
measures **11 539** and sits inside this band. That is a landmark, not a goal.

## 1.6 The mechanistic argument, stated so it can be attacked

The campaign's law is that a design's sign cannot be read off a bucket's size. So the case for a
JIT has to be made in terms of *which latency it removes*, not which bucket. Here it is, in the
map's own vocabulary, with the honest weakness attached:

**What the interpreter cannot escape is its loop-carried dependency chain:** dispatch → handler →
PC → next lookup → next dispatch. Every handler sits inside it, and the dispatch itself is an
**indirect branch with roughly 1 867 possible targets on a Cortex-A9 whose BTB holds one target
per site.** Its cost is a pipeline flush; an instruction count cannot see it, and *nothing
overlaps a flush, because the flush is the loss of the overlap*. **(measured/derived, handler
doc §30.3 step 2)**

A chained-block JIT removes that branch from the steady state entirely. That is a different
mechanism from every rung the campaign built, and it is the one thing the `HANDLER` discriminator
was supposed to classify and could not — it measured its own implementation instead, and
`HANDLER` remains **UNCLASSIFIED**, neither struck nor funded.

**The weakness, stated plainly:** "unclassified" is not "favourable". The discriminator's real
finding was that `gencpu`'s per-opcode handlers *are* already the specialisation — 15 instructions
of straight-line frameless leaf code for `MOVE.L Dn,Dn` — and that a runtime-parameterised inline
body is a *de*-specialisation costing 2.2–3.1× more. A JIT escapes that objection only because it
specialises at **compile time**, per shape, with the operands baked in, which is verbatim what
the handler doc says a future attempt would have to bring. **That is the bet, and M1 is where it
is settled.**

---

# 2. Architecture

Each chapter ends with what it can pre-register and what it cannot.

## 2a. Block discovery and shape

### What exists to build on

The decoded-op cache is already a virtual-keyed, generation-invalidated map from guest PC to a
decoded thing, with a measured hit rate and a measured fill price. Its geometry is fixed and
**cannot be reused as a block table**: a set is *exactly* 64 bytes — `tag[4]` 16 + `stamp` 4 +
`victim` 4 + `handler[4]` 16 + `opcode[4]` 8 + `ext0[4]` 8 + `meta[4]` 4 + `pad[4]` 4 — behind a
compile-time tripwire that refuses any build where it is not, because one set is one pair of
Cortex-A9 cache lines. **Four bytes are spare; a 4-way block pointer needs sixteen.** And the
geometry itself cannot be re-decided without a fresh PC distribution measured with rung 0 live,
which does not exist. **(measured, dopc doc)**

> **Decision (default, `assumed`, open as Q5's neighbour): a separate block-entry table, keyed the
> same way as the dopc and invalidated from the same choke point, with the dopc left byte-identical.**
> The dopc keeps serving the interpreter — which the JIT will still be running for every declined
> instruction — and its measured +23.31 % is not put at risk to save a table.

> **⚠ Corrected at M0 — the same decision, for a stronger reason.** The plan reached it from the
> dopc side (a 64-byte set has 4 spare bytes and a 4-way block pointer needs sixteen). The
> census found the harder half on the JIT side: the existing block-entry table **cannot** be
> reused under the MMU, whatever its geometry. `cache_tags[TAGSIZE]` is a 65 536-entry direct-
> mapped array of `union { cpuop_func* handler; blockinfo* bi; }` (`jit/compemu_arm.h:54`–`:57`,
> `:63`–`:66`; `jit/compemu_support_arm.cpp:162`) — 256 KB, two slots per block head — and its
> key is `cacheline(x) = ((uintptr)x) & 0xFFFF` **applied to `regs.pc_p`, a host pointer**
> (`jit/compemu_support_arm.cpp:1973`, `:2061`). **Under the MMU there is no `pc_p`.** The
> *structure* is reusable verbatim; the *keying* is not, and that is a fact about the JIT rather
> than a preference about the dopc. §0.3(c). **(measured)**

### Block boundaries

Generation one ends a block at, in this order:

| boundary | why |
|---|---|
| **any control transfer** (`Bcc`, `BSR`, `DBcc`, `JMP`, `JSR`, `RTS`, `RTE`, `RTR`, `TRAP*`) | a basic block is the smallest thing whose shape is statically known; superblocks are an M4 rung |
| **any instruction the engine does not compile** | it becomes the next interpreted instruction — see the bail contract in §2e |
| **any privileged or mode-changing instruction** (`MOVEC`, `MOVE` to/from `SR`/`USP`, `STOP`, `PFLUSH`, `CPUSHL`/`CINV`, `RESET`) | the block ends *before* it; the interpreter executes it, and every knob, table and cache it touches stays in exactly one implementation |
| **a guest page boundary** | load-bearing — see below |
| **a maximum block length** | bounds interrupt latency and compile cost — §2g |

**The page bound is not a safety margin, it is the same mechanism as rung 0's.** Rung 0's gate
reads the TTR registers and the already-populated page cache — a strictly non-faulting predicate —
and its verdict is valid for exactly one page; therefore the chunk is bounded by the page, and
*"the page-boundary rule falls out of the same mechanism instead of being a second one"*. A JIT
block inherits this verbatim: the translation the compiler used to read the guest instruction
bytes is valid for one page, so the block stops at the boundary and the next block re-validates.
**(measured pattern, block-idiom doc)**

### How rung 0's idiom work folds in

Rung 0 recognises three loop shapes (`bzero`, `mcpy`, and a `CPUSHL` maintenance loop) at the
dispatch site and runs many iterations per pass. It is worth **−43.6 % of an AMIX boot** in a
same-boot A/B, at a measured cost of −1 % on dhrystone, and it collapses the `CPUSHL`
invalidation storm 18×, which turned out to protect rung 1. **(measured)**

> **Decision (default): rung 0 stays exactly as it is through M3, and the JIT declines those PCs.**
> Subsuming it into block compilation is an M4 candidate with its own gate. The reason is the
> campaign's own rule, which this repository paid for once already: *"a cost measured with this
> recognizer switched off is not a cost the shipping firmware pays, and a change justified on one
> is justified on nothing."*

### Pre-registerable claims

1. **Mean block length in guest instructions**, on a pinned dhrystone window and on a boot window,
   predicted before it is measured. It is derivable in advance from the banked sampler rings.
2. **Block-cache hit rate ≥ the dopc's on the same workload** (85.32 % dhrystone, rung 1c),
   because block granularity is strictly coarser than instruction granularity.
3. **Coverage `c`** — retired guest instructions executed inside blocks — as a counter, read
   *before* any wall-clock number, per the dopc doc's verdict protocol.

### Open questions

- Basic block vs superblock/trace in generation one (§7 **Q7** covers the family set; the shape
  question is folded into **Q1**).
- Compile on first sight, or after a hot threshold? A threshold costs a counter per block head and
  saves compiling the long tail of once-executed blocks during boot.
- `DBcc` loops: a special case, or does rung 0 already own them?

## 2b. Code cache and keying

### Keying — virtual, with the supervisor bit, and the reason it is not free

The dopc tags **guest logical PC with `regs.s` in the otherwise-dead bit 0**
(`Z3660_DOPC_KEY(pc, super) = (pc & ~1) | (super ? 1 : 0)`), and this has already caught a real
bug: a naive fixed empty-marker of `1` means *"pc 0 in supervisor mode"*, and the harness's
user-mode `CPUSHL` case dispatched a NULL handler out of a way nothing had ever filled. The empty
marker is therefore set-relative. **A JIT block table inherits both the encoding and the trap.**
**(measured, dopc doc)**

The harder half is address-space identity. `URP`/`SRP` writes raise `IV_ROOT`, which is
**1.32 – 3.27 %** of invalidation requests — and the dopc declined to fund a per-address-space
generation because reading it *"cannot be read from a per-**set** stamp... so it means a per-entry
stamp and three more compares on the hit path"*, on a path that runs every dispatch. **For a JIT
the arithmetic reverses**: the lookup runs once per *block*, so three more compares are amortised
over the block's length. That is an argued reversal of a documented decision and it is **Q5**.

> ### As built at M1 round 1 (2026-08-24) — the keying, and what replaced the refuted table
>
> `Z3660_WARP_KEY(pc, super) = (pc & ~1) | (super ? 1 : 0)`, indexed
> `Z3660_WARP_INDEX(pc) = (pc >> 1) & (SETS-1)` over **1024 sets, direct-mapped**, one block
> per set, 16 bytes per entry on the target — tag, generation stamp, a 32-bit code pointer
> and the retired count, i.e. **half a Cortex-A9 line**, guarded by a compile-time tripwire
> on 32-bit pointers exactly as the dopc's set is. `cache_tags[65536]` and its
> `cacheline(regs.pc_p)` keying are **not used and not adapted**: §0.3(c) refuted the keying
> and there is no `pc_p` under the MMU.
>
> **Direct-mapped, and the reason is not thrift.** The dopc needs four ways because it is
> consulted once per *instruction*; a block table is consulted once per *block head*, and a
> way count is a decision to take against a measured head distribution rather than by
> analogy. Associativity is a round-2 question with `WARP_DISC` and a measured hit rate to
> answer it.
>
> **The set-relative empty marker is inherited whole** — `TAG_EMPTY(idx) = (idx ^ 1) << 1` —
> and the corpus asserts the property rather than the constant: for all 1024 sets, the
> marker indexes a *different* set, so no legitimate key can match it. Validity is the
> per-set generation stamp, so a discard stays O(1); the marker is what keeps the encoding
> honest when round 2 adds ways and supervisor blocks. Mutation **M-5** (the key drops the
> supervisor bit) is the arm that keeps it load-bearing, and it is caught. §0.7.

### What the physical-index strike does and does not transfer

`f` = **0.0787** against a pre-registered gate of 0.35, with the wall clock at **+0.00 %** against
+1.5 %, measured on metal 2026-08-23 by suppressing `CPUSHL`/`CINV`-driven dopc invalidation — a
strict upper bound by construction, cost side charged at zero. The physical-index dopc study is
**STRUCK**, and the reading rule forbids re-measuring at another band. **(measured, f doc)**

What transfers to this plan:

- **It supports virtual keying as the default**, because it says the prize on the other side of the
  namespace wall is small on this workload.
- **It does not transfer as a verdict.** `f` is a ratio of two hit rates and is immune to band and
  instrument, but *"it is not thereby immune to the mix"* — and the JIT's arithmetic is different
  in kind: the dopc throws away a decoded opcode worth **75.9 – 81.9 cycles** to refill; a JIT
  block costs one to three orders of magnitude more. **A whole-cache invalidation that is
  affordable for the dopc may not be affordable for the code cache, and the JIT must measure its
  own invalidation cost rather than inherit the dopc's verdict.**
- **It says nothing about the workload that matters most here.** The `f` session ran dhrystone
  only — small, resident — and the doc is explicit: *"The highest-risk guest path is demand
  paging... It says nothing about paging, and nothing about the disk."* Nothing bounds `f` on any
  other workload. M2 is where that is confronted.

### Size budget on the Zynq's DDR — a solved problem, and not the constraint

| fact | value | source |
|---|---|---|
| core1's DDR window | `ORIGIN 0x30000000, LENGTH 0x0FF00000` = **255 MB** | `Z3660_emu/src/lscript.ld:24` |
| core1 heap | `_HEAP_SIZE = 0x06000000` = **96 MB** | `lscript.ld:14` |
| that region's attributes | **not touched** by core1's section-table loop → BSP default: normal write-back cacheable, **executable** (no XN) | `main.cpp:906-970` |
| existing JIT cache size | `MAX_JIT_CACHE 32768` KB = **32 MB**, from `malloc()` | `include/newcpu.h:42`, `compemu_support_arm.cpp:2094` |
| current lean ELF / bin | 8 831 028 B / 6 499 012 B | on disk |

**(all measured)**

> **Added at M0, and it makes this whole subsection cheaper than it looks: the 32 MB is
> *already allocated*, in the shipping MMU firmware, on every boot.**
> `create_popalls()` calls `cache_alloc(POPALLSPACE_SIZE + MAX_JIT_CACHE * 1024)` =
> `malloc(33 556 480)` (`jit/compemu_support_arm.cpp:2094`; `cache_alloc` is a plain `malloc` at
> `:207`, its `mmap` arm commented out), and it is reached from `build_comp()` (`:2370`) which
> `build_cpufunctbl()` calls **unconditionally** under `#ifdef JIT` (`newcpu.cpp:1065`) — before
> and regardless of `alloc_cache()` returning early on `cache_size == 0` (`:1886`). The serial
> line is `Cache_Alloc of 33556480 at 0x........` (`:212`), on every boot of every image.
> **So the engine costs no new heap, and the allocation's success is proven rather than
> assumed.** §0.3(c). **(measured)**

> **DDR capacity is not the constraint; L1I is.** `m68k_run_mmu040` is 2 288 B in a 32 KB L1
> instruction cache — the loop's whole working set fits with room to spare, which is a large part
> of why the interpreter runs at IPC ≈ 1 on the dispatch walk. Generated blocks will not. This is
> the same unpriced second-order cost the campaign has now flagged three times: rung 2's 2 KB of
> always-hot `.bss` competing with a 64 KB dopc; the SPEC round's **−1.07 % standing cost of
> inlining anything into the run loop**, of which the larger half was a spilled byte and a bigger
> stack frame. **Code bytes per guest instruction is a first-class metric for this project, not a
> footnote.**

### Eviction

The existing JIT bump-allocates to `max_compile_start` and then calls `flush_icache_hard(3)` —
throw everything away, reset `current_compile_p`. With 32 MB that fires rarely.

> **Decision (default): keep exactly that for generation one.** It is simple, it is provably
> correct, and it composes with the generation counter (a full flush is one generation bump plus a
> pointer reset). **Measure the flush rate per boot before designing anything finer.**

### Self-modifying code

**The engine does not check guest stores.** The basis is architectural and it is measured: the
68040 requires a guest to maintain its own caches before executing code it has written or
remapped, and *"AMIX honours it: its `CPUSHL` loop alone is 7.79 % of a boot"*. The dopc pins this
as a documented contract in `test_dopc_coherence` rather than leaving it to be discovered. The
JIT inherits the contract and the test. **(measured, dopc doc)**

This is a genuine advantage of JIT-under-MMU over JIT-without: the existing UAE JIT carries
per-block **checksums** (`calc_checksum`, `compemu_raw_check_checksum`, `BI_NEED_RECOMP`, soft
flush) precisely *because* upstream has no MMU and therefore no architectural flush contract to
lean on. We have one. Which to use is a real choice with a cost on the block-entry path — folded
into **Q1**.

**The one hole the architecture does not cover is real and already has a fix site.** Core0 reads
disk sectors straight into guest DRAM (`scsi.c`'s `f_read`); the interpreter never sees the bytes
go past. An AMIX text pagein replaces instruction bytes underneath any cache that holds decodes of
them. The dopc hangs `IV_DMA` off core1's existing L1-invalidate-over-range point in `main.cpp`,
and the rejected alternative is the design lesson: a `SHARED` epoch word bumped by core0 *"would
put the knowledge on the producer, and a core1 running against a core0 built before that field
existed would read a permanent zero and never invalidate — **fail-DANGEROUS, silently, in the one
direction that corrupts a filesystem**"*. **The JIT joins the same site, on the consumer side,
using knowledge it already has.** **(measured, `main.cpp:349-364`)**

### Pre-registerable claims

1. Steady-state code-cache occupancy in bytes, on a boot and on a dhrystone run.
2. Full-flush rate per boot.
3. **The JIT's invalidation count equals the dopc's on the same boot**, because it hangs off the
   same causes at the same choke point. The guest's own invalidation demand is a guest property
   (91 k measured, cache-independent), so this is a checkable identity, not a hope.
4. Code bytes emitted per guest instruction, per family.

### Open questions

**Q5** (per-address-space generation), and the checksum-vs-architecture question inside **Q1**.

## 2c. Codegen target — ARMv7-A Cortex-A9, 32-bit

### Register mapping

The existing ARM32 allocator declares `N_REGS 11` — *"really 16, but 13 to 15 are SP, LR, PC; 11
holds memstart and 12 is scratch register"* (`jit/compemu_arm.h:124`).

> **⚠ Corrected at M0 — that comment is stale, and the plan believed it.** The code says
> `#define R_MEMSTART 10`, `#define R_REGSTRUCT 11` and
> `always_used[]={2,3,R_MEMSTART,R_REGSTRUCT,12,-1}` (`jit/codegen_arm.cpp:83`–`:85`), repeated
> by hand in `freescratch()` (`jit/compemu_support_arm.cpp:1571`). So **r10 is the guest-memory
> base, r11 is the `regs` pointer, r2/r3/r12 are the work registers, and exactly 8 host
> registers are allocatable** — r0, r1, r4–r9 — not eleven. §0.3(b)(c). **(measured)**
>
> **This changes the shape of the question in the engine's favour.** The plan asked what r11's
> replacement should be; r11 already *is* the `regs` pointer, which is one of the three things
> the plan wanted to point it at. The register that actually frees up is **r10**, and its
> candidates are the `z3660_dfv_r` verdict table or the page cache — decided by counting.
> **The budget is 8 allocatable registers and it does not grow**, which is a first-class input
> to the allocator question inside Q1, not a footnote to it.

Under the MMU there is no single base, so r10's meaning must change. But the replacement is
cheaper than it sounds, because of a fact that is easy to miss:

> **Guest physical address *is* host address, 1:1.** `drct_bank` carries
> `baseaddr_direct_r/_w = (uae_u8*)0x08000000` with `startaccessmask = 0x08000000`, and
> `direct_get_long()` / `direct_put_long()` (`include/memory.h:543-566`) are a bare
> `__builtin_bswap32` of a cast pointer. **(measured)**

So the *only* thing the JIT's memory path owes over the existing JIT's is the virtual→physical
step — and on this firmware even the base add is nominal: `regs.natmem_offset` is set to **0**
(`uae_emulator.cpp:1084`) and `R_MEMSTART` is loaded from it at block entry
(`jit/codegen_arm.cpp:534`–`:535`), so the existing JIT's whole memory model on this card is
"host address = guest address, plus a byteswap". **(measured, added at M0)**

r10's natural successors are the `z3660_dfv_r` verdict table or the page cache — decided by
counting. (`regs` is not a candidate: r11 already holds it.)

Two more gifts already in the struct: `regstruct` (`include/newcpu.h:176-187`) already carries
JIT-only tail fields — `scratchregs[]`, `scratchfregs[]`, `jit_exception`, `mem_banks`,
`cache_tags` — so the spill area exists. **(measured)**

### Flags — the largest single gift, and it is measured

`CPU_arm` is defined, so the live `flag_struct` is the 32-bit arm of `machdep/m68k.h:477`:

```c
struct flag_struct { uae_u32 nzcv; uae_u32 x; };
#define FLAGBIT_N 31   #define FLAGBIT_Z 30   #define FLAGBIT_C 29
#define FLAGBIT_V 28   #define FLAGBIT_X 0
```

**The guest's N/Z/C/V live in one word, in native ARM CPSR bit positions**, and the interpreter's
`optflag_*` macros (`m68k.h:555-769`, `ARMV6_ASSEMBLY` is defined by the build) are inline ARM
assembly that reads CPSR straight into it:

```c
__asm__ __volatile__ ("adds %[rv],%[rd],%[rs]\n\t" "mrs %[nzcv],cpsr\n\t" ...);
```

**(measured)**

So generated code owes only the **X** bit and the carry inversion on subtract/compare — and the
existing JIT already models exactly that, keeping 68k flags live in ARM flags where it can,
spilling to `regs.ccrflags.nzcv`, with `dont_care_flags`/`live_flags` liveness tracking:
*"ARM instructions create flags, which quite often are exactly what we want. Exception: carry is
often inverted."* **That machinery is reusable whichever route §7 Q1 picks.** **(measured,
`compemu_support_arm.cpp:236-260`)**

The saving this unlocks is specific: the interpreter snapshots `f = regs.ccrflags` on **every**
guest instruction, unconditionally, for restart. A JIT owes that snapshot only at boundaries where
a fault can actually be taken. §2e says where those are.

### Literal pools

The build defines `-DARMV6T2`, and the code-cache data-buffer machinery
(`DATA_BUFFER_SIZE 768`, `reset_data_buffer()`) is guarded by
`#if defined(CPU_arm) && !defined(ARMV6T2) && !defined(CPU_AARCH64)`
(`jit/compemu_support_arm.cpp:697`) — so on this target constants go through `movw`/`movt`
immediates and no literal pool is used. **(measured at M0 — upgraded from `derived`: the guard
and the `-DARMV6T2` in `Z3660_emu/Makefile`'s `CFLAGS` together settle it without a
disassembly.)**

> **One trap for whoever greps for the gate.** `USE_DATA_BUFFER` (`jit/compemu_arm.h:118`) is
> `#define`d for every `CPU_arm` build and is **tested nowhere in the tree** — it is dead, and
> it is not the switch. `!defined(ARMV6T2)` is. **(measured at M0)**

### The I/D-cache maintenance contract — the exact sequence, and the gap in it

The primitive is `flush_cpu_icache(start, stop)` at `jit/compemu_midfunc_arm.cpp:603`. The Xilinx
calls are `#if 0`'d out with the comment *"these are faster than xilinx's ones"*; the live path is
two hand-rolled CP15 loops at a 32-byte line stride (`compemu_midfunc_arm.cpp:566-600`):

| helper | CP15 op | meaning |
|---|---|---|
| `arm_flush_cache` | `mcr p15,0,Rn,c7,c14,1` per line, then `c7,c10,4` | **DCCIMVAC** — clean+invalidate D-line by MVA to PoC, then **DSB** |
| `arm_icache_invalidate` | `mcr p15,0,Rn,c7,c5,1` per line, then `c7,c10,4` | **ICIMVAU** — invalidate I-line by MVA to PoU, then **DSB** |

**Which core does it: core1, and only core1.** Core1 generates the code and core1 executes it, so
no cross-core maintenance is owed for the JIT's own output. That is *not* true of guest
instruction bytes, which core0 can write by DMA — §2b. **(measured)**

It is called at exactly four sites: after the popall trampolines
(`compemu_support_arm.cpp:2189`), in `prepare_block()` (`:2213`), after each finalised block
(`:2739`), and in `write_jmp_target()` — which patches a live branch word and flushes four bytes,
i.e. **self-modifying code in the steady state, not only at init**. **(measured)**

**The lesson attached to it is this repository's own, and it is written in the source:**

> `popallspace was just written above. With D-cache + I-cache enabled on Core1 and the firmware
> built -Os/-flto (the AMIX size change), the first pushall_call_handler() can fetch STALE I-cache
> lines for these trampolines -> ARM Undefined Instruction -> reboot (the "[Core1] Undefined
> Exception" seen entering any UAEJIT mode).`

That is commit `8be8949`, which restored a `flush_cpu_icache` call upstream had **commented out**
with the reasoning *"No need to flush. Initialized and not modified"*, and added
`#pragma GCC optimize("O3")` because the global build is `-Os`. `docs/port-ledger-beta21.md:213`
records that beta 21 lost it again and it had to be re-ported. **(measured)**

> **⚠ The gap, stated as a question and not as a defect (Q3).** The ARMv7-A sequence for
> self-modifying code is clean-D-to-PoU → **DSB** → invalidate-I-to-PoU → *branch-predictor
> invalidate* → **DSB** → **ISB**. The live sequence has **no ISB anywhere** and no
> branch-predictor invalidate; and `DCCIMVAC` cleans to PoC while the Zynq's L2C-310 is not
> maintained by CP15 operations at all — which is precisely why it is faster than the Xilinx call
> it replaced. The in-tree template that gets the full sequence right is `finish_Attributes()`
> (`main.cpp:773-783`): `Xil_DCacheFlush()` → invalidate-UTLB → invalidate-branch-array → `dsb()`
> → `isb()`. **It works today at the existing JIT's flush volume; a block-chaining JIT raises that
> volume by orders of magnitude, and the failure mode of getting it wrong is an intermittent
> reboot that looks like anything.** And the standing platform lesson from the `dsb-experiment`
> applies with full force: *adding barriers is not evidence of a fix; only a soak on metal is.*

### Pre-registerable claims

1. **The block-entry sequence's ARM instruction count**, counted from `objdump` *before* any metal
   time. This is §30.3 step 3 and the SPEC round's static gate, and the SPEC round is why it is
   mandatory: its served body turned out **2.2 – 3.1× longer** than the generated handlers it
   replaced, and the arithmetic said so before the metal session did.
2. Generated bytes per guest instruction, per compiled family.
3. `m68k_run_mmu040`'s own size must not grow (rung 2 and rung 3 both held it at 2 288 B; the SPEC
   round grew it to 3 516 B and paid −1.07 % for the privilege).

### Open questions

**Q1** (which route), **Q3** (the cache sequence), and r11's replacement.

## 2d. Memory access

### The measured shape of AMIX's address space

AMIX-040's own `pstart040.s` programs:

```
ITT0 = 0x003fc000   0-1 GB code, both privilege levels     (ITT1 disabled)
DTT0 = 0x003fc060   0-1 GB data, both privilege levels, cache-inhibited
DTT1 = 0x807fa060   0x80000000+, SUPERVISOR ONLY
```

**(measured, quoted in `accessor-fastpath.md` and `ifetch-fastpath.md`)**

Consequences, all measured:

- **Every supervisor data access is transparently translated** and never reaches the page cache.
  User data falls through both windows to it.
- The transparent share of served *data* accesses is **22.63 % (cadence 1) / 24.07 % (cadence 4)**
  — and *"the weighting is not a constant and must not be re-pinned"*; it is a property of the
  window's supervisor/user mix, and `DFAST_XPAR` makes reading it per capture free.
- **87 % of instruction fetches are ITT-matched** (53.3 M of 61.3 M).
- The data page cache runs at **99.19 – 99.53 %** hit; overall data fast-path coverage is
  **99.11 – 99.40 %**.

### What gets reused verbatim

| artefact | where | note |
|---|---|---|
| `z3660_dfv_r[2][256]`, `z3660_dfv_w[2][256]` | `z3660_dfast.h:196-197` | 2 KB, 16-bit cells: verdict in the low byte, `mmu_cache_state` in the high byte |
| the three verdicts `SLOW`/`XPAR`/`PAGE` | `z3660_dfast.h:157-162` | `SLOW` = *no claim is made*, run the unmodified accessor |
| `z3660_dfast_read()` / `z3660_dfast_write()` | `include/cpummu.h:744` / `:802` | `always_inline`, no `bl`, no `blx`, no indirect branch on the hit path |
| the two rebuild sites | `cpummu.cpp:272` (`mmu_tt_modified`), `:1772` (`mmu_set_tc`) | *the* complete set — every `MOVEC` to a TT register and CPU reset go through the first; the second is the only writer of `regs.mmu_enabled` |
| `atc_data_cache_read/write` with the existing index and tag arithmetic | `cpummu.h` | **sharing the arithmetic is how equivalence is guaranteed rather than hoped for** |

**Measured hit-path costs the JIT's inline sequences must beat, or at least not lose to:**

| path | before rung 2 | after |
|---|---|---|
| read, `PAGE` arm | 110 | **48** |
| read, `XPAR` arm | 76 | **33** |
| write, `PAGE` arm | 138 | **51** |
| write, `XPAR` arm | 97 | **33** |

Two structural fences must survive into generated code, and both are *structural* rather than
checked: **never fast-path across a page or bank boundary** (the caller tests `is_unaligned_bus()`
first, so every access reaching the resolver is naturally aligned and a naturally aligned 1/2/4-byte
access cannot cross a 4 KB page or a 64 KB bank window), and **never fast-path a device**. And one
asymmetry must not be copy-pasted away: on the *data* side `TTR_NO_WRITE` means read → `XPAR`,
write → `SLOW`. **(measured)**

### The instruction side, and a use for struck code

A JIT fetches no guest instruction words in steady state, so rung 3's table is not needed on the
*execution* path. It is needed on the **compile** path, where the compiler reads guest instruction
words to decode them — and there the design is intact in git at **`3087af9`**: `z3660_ifv[2][256]`,
1 KB, one direction (a fetch is always a read), built by the same `z3660_dfast_win()` replay at the
same two sites, with `TTR_NO_WRITE` → `XPAR` (a fetch is a read; the corpus pins it), and odd
fetches gated out structurally. `iword` 79 → 37 instructions on the 87 % arm. **(measured)**

Rung 3 was reverted because *"the 42 removed instructions are a call chain **around** the fetch...
the very latency the decoded-op cache exists to hide"*. **On the compile path nothing is hiding
that fetch** — there is no dopc hit in front of it, and the compiler reads several words back to
back. That is a genuinely different classification, and it is **Q6**: a hypothesis to pre-register,
not a claim to act on.

### The slow path

**Decline, do not reimplement.** Rung 2's rule is the rule: *"the fast path either answers exactly
what the original would have answered, or it does not answer"* — every non-hit arm returns 0 and
the caller runs `mmu_get_long()` / `mmu_put_long()` verbatim, so miss paths, fault paths, the
write-back contract, `mmu_bus_error`'s frame, the M-bit re-walk, `TTR_NO_WRITE` and the
`TCR_DWO` bus error are never duplicated and therefore cannot drift.

Under a JIT the slow path is a **helper call from generated code**, and that is where the
exception problem in §2e bites.

### Pre-registerable claims

1. Generated inline access sequence length per class, from `objdump`, before metal — against rung
   2's 33/48/33/51 as the bar.
2. `DFAST_HIT / (READ + WRITE)` stays at 99.1 %+, and the dpagecache hit rates are unmoved to
   within **0.04 pp** — rung 2's own guarantee, held because the tag arithmetic is shared.

   > **⚠ Qualified at M0, in §0.5.6: this claim is conditional on the counting emitter.** If
   > generated code does not increment the data counters in the profiling build, the surviving
   > ratio describes only the *interpreted remainder* — which is selection-biased toward the
   > accesses the engine declined, so it would fall for a reason that is not a regression. The
   > emitter therefore increments them at the same points the accessor does, in the profiling
   > build only, and the code-size metric is read off the lean emitter's `objdump`.

### Open questions

Inline the `PAGE` probe in generated code, or call a helper? The helper is a `bl`, and removing
calls was rung 2's entire win — but inlining a 48-instruction probe at every access is the L1I
risk of §2b in its most concentrated form. This is a code-size-vs-latency trade with a measurable
answer, and it belongs in M1's first A/B.

## 2e. Precise exceptions and interrupts

### What the 68040 gives us for free

The 68040 uses **full instruction restart**, so `m68k_run_mmu040`'s recovery is far simpler than
the 030 loop's sub-access replay. The whole live checkpoint at an instruction boundary is:

```c
f = regs.ccrflags;                 // 5851  the flags snapshot
mmu_restart = true;                // 5852
regs.instruction_pc = m68k_getpc();// 5853  the restart PC
```

and the recovery, on a `CATCH`:

```c
z3660_dopc_ext_pc = Z3660_DOPC_NO_PC;      // disarm the extension-word window
if (mmu_restart) { regs.ccrflags = f; m68k_setpci(regs.instruction_pc); }
cpu_restore_fixup();                        // undo predec/postinc An fixups
TRY(prb2) { Exception(prb); } CATCH(prb2) { halt = CPU_HALT_BUS_ERROR_DOUBLE_FAULT; } ENDTRY
```

`cpu_restore_fixup()` (`newcpu_mmu_glue.cpp:47-51`) restores `mmufixup[0..1]` — the address-register
fixups. The register file and SR are **not** snapshotted: handlers are written so partial effects
are confined to the flags and the fixups. **(measured, `newcpu.cpp:5841-6177`)**

**The format-$7 access-error frame is implemented** — `newcpu.cpp:2082-2118`, built from
`regs.mmu_ssw`, `regs.mmu_effective_addr` and the `wb2_*`/`wb3_*` slots set by `mmu_bus_error()`
(`cpummu.cpp:536`), pushing `0x7000 + vector_nr * 4`. The comment above the run loop calling it a
*"still-parked task"* is stale; ISSUE-52 closed and the 040 installs and boots. **(measured)**

### The constraint nobody had written down

`TRY` / `CATCH` / `THROW` in this tree are **C++ exceptions**, not `setjmp`/`longjmp`.
`include/mmu_common.h:11-24` takes the `__cplusplus` arm; the Makefile compiles everything with
`arm-none-eabi-g++ -std=c++11` and no `-fno-exceptions`; and `lscript.ld` keeps `.eh_frame`,
`.eh_framehdr`, `.gcc_except_table` and `.ARM.exidx`. **(measured)**

> **A fault raised inside a helper called from generated code would have to unwind an ARM frame
> that has no `.ARM.exidx` entry**, because generated code lives in `malloc`'d DDR and is in no
> linked object. The ARM EHABI unwinder's response to a PC it cannot find in the index is failure,
> which becomes `std::terminate`. **(derived, from the EHABI contract and the linker script — not
> measured. It is the single cheapest thing to settle before a line of codegen is written: Q2.)**

> **⚠ MEASURED 2026-08-23 — the derivation above is now a measurement, and the `derived` tag on it
> is left standing because that is what it was when it was written.** The probe booted once and
> printed `B: RESULT = TERMINATED`: the unwinder refused the generated frame and `std::terminate`
> was reached. Experiment A additionally showed that generated code *executes* with the shipped
> I-cache sequence alone, from the code cache's own arena. **The bail-before-fault contract below
> stands on metal, not on an argument.** §0.4.1.

### The bail-to-interpreter contract

Three ways out, and the plan pre-registers which:

1. **Contain every throw inside a C++ frame.** Each helper that can fault runs its own `TRY`/`CATCH`
   and returns a status word; generated code branches to a bail stub. Costs a status test per
   faulting access.
2. **Bail *before* the fault.** The access helper performs the translation and, on any non-hit,
   returns "bail" **without touching guest memory**; the interpreter then re-executes the whole
   instruction from its boundary and takes the fault where all the ISSUE-52 machinery already
   works. **Recommended for generation one.**
3. Register generated code with the unwinder. Real work, real risk, no benefit generation one
   needs.

> **Why 2.** It makes the JIT's fault semantics *identical to the interpreter's by construction*,
> which is exactly the argument that made rung 0 defensible: the block-idiom fast path keeps the
> real guest store and calls `put_long_mmu040()` with the same arguments in the same order, *"so
> TTR matching, the data page cache, the ATC, the M and U descriptor bits, protection checks, bank
> dispatch and device side effects are preserved **by construction** rather than by argument."*
> A fault under demand paging already costs thousands of cycles; re-executing one guest
> instruction to reach it is free at that scale.

**The contract itself:**

- A block is entered **only at a block-head PC**, and every block head is a guest-instruction
  boundary.
- At every guest-instruction boundary *inside* a block, the engine maintains exactly the state the
  interpreter maintains at the same boundary: the 16 registers, `regs.pc`, `regs.ccrflags`,
  `regs.instruction_pc`, `mmu_restart`, `mmufixup[0..1]`.
- **Bail = write that state back and return to `m68k_run_mmu040` with the PC at the boundary.**
  The interpreter executes that instruction and every following one until the next block head.
  A state at a bail boundary is a state the interpreted path itself passes through — rung 0's
  exact argument for interrupt safety, promoted to this engine's central invariant.

### The write-back families are excluded, by rule

Rung 0's own note is the model: `op_20c8_31_ff` is **not** instruction-restart — it increments the
destination register, sets the flags, advances the PC, clears `mmu_restart` and disarms `mmufixup`,
and *only then* stores. A fault there is a 68040 write-back fault on an instruction that has
already completed architecturally, which the guest kernel's `wb040.s` replays. **(measured)**

> **Generation-one rule: the engine never compiles an instruction whose fault semantics are
> write-back rather than restart. Those bail.** That keeps `mmu_restart`, `mmufixup` and the
> format-$7 write-back slots entirely inside the interpreter, and it is checkable **statically**,
> as a list of opcode families, before any metal time.

### Trace and interrupt windows

- `do_specialties()` stays per-instruction in the interpreter at **every** cadence — STOP, trace
  and mode-change. Interrupt *delivery* lives inside it (`newcpu.cpp:3714-3720`). **(measured)**
- **The engine refuses to enter a block while `regs.t0 || regs.t1`.** Trace on ⇒ interpret. This is
  one predicate at the block-entry site, and it removes an entire class of divergence.
- **Blocks poll at their boundaries**, and they must feed the cadence counter with the number of
  guest instructions they retired — §2g. This is not a performance choice; it is the difference
  between the A3000-SCSI pump keeping its timebase and silently changing it.

### Mid-block resume

**Generation one has none.** A fault inside a block is either prevented (bail before the access) or
taken by the interpreter after a bail. Revisit at M4, or never.

### Pre-registerable claims

1. **Zero `throw` sites reachable from generated code**, proven by symbol audit and `objdump`
   before metal. Static gate, mandatory, in the SPEC round's form: unmet ⇒ the build does not go to
   metal.
2. **`FAULTS` unchanged** between an interpreter-only arm and a JIT arm on the same workload — the
   SPEC round's own check read 3 630 vs 3 629 and that is the resolution to expect.
3. **`INSNS` (retired guest instructions) identical** between arms on a deterministic workload.
   Note the existing identity that makes this readable: `DOPC_HIT + DOPC_MISS == INSNS + FAULTS`
   — faulting instructions dispatch but never retire.

   > **⚠ Corrected at M0, in §0.5.2 and §0.5.6 — this claim is right in substance and wrong as
   > written.** `INSNS` is incremented once per *run-loop pass*, and a block is one pass, so it
   > **falls** with the engine on. The comparable quantity is the reconstructed guest stream
   > `(INSNS − WARP_ENTER) + WARP_INSNS` — rung 0's own correction with `WARP_ENTER` in
   > `BLK_HIT`'s place — and the identity becomes
   > `DOPC_HIT + DOPC_MISS == (INSNS − WARP_ENTER) + FAULTS`, which reduces to the form above
   > when the engine is off. **(measured topology, `z3660_prof.h`, `newcpu.cpp:4622`)**

## 2f. Invalidation contract

### The causes, and the choke point

`mmu_flush_cache()` (`cpummu.cpp:1828-1856`) is the deliberate choke point — *"a new upstream
caller inherits the whole-cache invalidation automatically, and has to be opted **out** of it
rather than into it."* The code cache joins `z3660_dopc_invalidate()`'s call sites; it does not
invent its own. **(measured)**

| cause | id | address namespace | measured share of requests | what the code cache must do |
|---|---|---|---|---|
| `CPUSHL`/`CPUSHP`/`CPUSHA`/`CINV` | `IV_FLUSH` (0) | **physical** — `flush_mmu040()` discards the argument | **46.70 – 63.89 %** | invalidate all |
| `PFLUSHA`, `MOVEC`→`TC`, MMU reset | `IV_PFLUSHA` (8) | names no page | 27.15 – 36.37 % | invalidate all |
| `PFLUSH` one page | `IV_PFLUSH` (7) | **logical** — the one narrowable cause | 6.15 – 9.14 % | invalidate all (narrowing is a later rung with its own evidence) |
| `MOVEC`→`URP`/`SRP` | `IV_ROOT` (1) | address-space change | 1.32 – 3.27 % | invalidate all — **unless Q5 buys a per-space generation** |
| core0 DMA into guest RAM | `IV_DMA` (2) | **physical** | 0.06 – 8.45 % | invalidate all, from core1's side |
| `MOVEC`→`CACR` | `IV_CACR` (9) | — | 0.00 % after narrowing | narrowed at the call site to changes of mask `0x80008000` |
| `build_cpufunctbl()` | `IV_TABLE` (3) | — | — | invalidate all |
| `map_banks()` | `IV_MAP` (6) | — | — | invalidate all |
| the switch moving | `IV_KNOB` (4) | — | — | *an off period is an unwatched period* |
| generation wrap | `IV_WRAP` (5) | — | — | **not a request** — subtracted from the identity |

**(all shares measured, dopc doc; `IV_FLUSH`'s range is quoted in the map's form as the doc
instructs)**

The live identity is `sum(IV_*) − IV_WRAP == DOPC_INVAL`. A JIT adds a *consumer*, not a cause;
its own counter must reconcile against the same sum.

### The namespace wall

`CPUSHL`/`CINV` and DMA name **physical** addresses; the cache tags **logical** ones. The host
corpus already proves this is not theoretical: *"`dopc_m_dma()` flushes `DOPC_PCODE` (0x001C0000)
for an entry tagged at `DOPC_VCODE` (0x00800000), so a naive range test refuses that flush and the
stale decode runs."* **A range test against a physical address ships corruption.** **(measured)**

### What `f` licenses, and what it does not

**Licenses:** the expectation that whole-cache invalidation is affordable *on the workload
measured*, because the response slope of misses to removed invalidations was **0.154** — the
entries a `CPUSHL` kills were mostly going to be re-missed anyway.

**Does not license, and each of these is quoted from the doc:**

- **Skipping any architecturally required invalidation.** *"Skipping an invalidation the 68040
  architecture requires is a correctness violation, not a tuning choice."* The failure taxonomy is
  written down: wrong computed results, **wild control flow** (the stale entry's handler is an
  arbitrary opcode's, so operand fields are read out of unrelated bytes), **wild writes** into the
  kernel's buffer cache, panics.
- **Any statement about paging.** *"It says nothing about paging, and nothing about the disk."*
  The `f` session ran dhrystone only, and the doc says the choice was deliberate: *"The
  highest-risk guest path is demand paging. AMIX must flush before executing a text page it has
  just paged in... Dhrystone is small and resident and is therefore the workload least likely to
  trip it."*
- **Any other workload.** *"Nothing here bounds `f` on any other workload."*
- **A re-measurement.** The reading rule forbids it, and the `IVSUP` machinery has been removed
  from the tree — re-running it means rebuilding it.

### Pre-registerable claims

1. The JIT's invalidation request count reconciles exactly against `sum(IV_*) − IV_WRAP` on the
   same boot.
2. **Blocks discarded per invalidation** (the block-level analogue of the dopc's misses-per-
   invalidation, measured at 274 – 292), read *before* any wall-clock number, per the dopc doc's
   verdict protocol.
3. Compilation events per boot, and the fraction attributable to invalidation rather than to
   first-sight.

### Open questions

Q5 again, and the standing invariant that must be re-checked at M2: is the guest's invalidation
demand *under paging load* still a guest property, or does it scale with the code cache in a way
dhrystone could not show?

## 2g. Service cadence and the compilation budget

### The two cadences, and they are not the same thing

**Core1's per-instruction poll.** `Z3660_RUNLOOP_TAIL()` (`newcpu.cpp:4921-4926`) counts guest
instructions and calls `check_uae_int_request()` every `z3660_service_cadence` of them. That
function runs `z3660_tasks()` (self-throttled 1-in-10 M), `ipl_main_read()` (IPL lines, ISR-armed
and poll-consumed) and, 1 poll in 256 under AMIX, `a3000_scsi_hsync()` — the WD33C93 interrupt-delay
pump. **One pump tick therefore costs `256 × service_cadence` emulated instructions.** The poll is
explicitly **not** instrumentation and survives in every build variant. **(measured)**

**Core0's cooperative loop.** No preemption, no work queue, **no watchdog on any callee**. *"Any
callee that blocks, blocks the piscsi service, the serial console and the ethernet nag together."*
A healthy iteration is **~20 µs**, ~48 261 iterations/s. **(measured, `core0-loop-blocking-audit.md`)**

### The deadline, measured

| cadence | status | what breaks first |
|---|---|---|
| 1 | default | nothing — max responsiveness, min throughput |
| **2 – 4** | **known good** | — |
| 8 | boots only *after* AMIX is up | the A3000-SCSI bootstrap: `sd` open steps through SELECT and SRV_REQ INT2s and the emulator notices them too late |
| 16 – 64 | ceiling ~1.3× vs cadence 1, knee ~8 | network RX under flood has **panicked AMIX** |

> *"The knob trades interrupt latency for CPU throughput, so the failure mode of a too-high cadence
> is never 'slower' — it is a missed or late interrupt somewhere."* **(measured, `docs/AMIX.md`)**

Standing posture: `service_cadence 4`, `amix_ram 128`. And cadence is not free in both directions:
at cadence 8 the guest CPU is +34 % and raw-device reads are **−16.2 %**.

### The rule this imposes on the engine

> **A block must feed the cadence counter with the number of guest instructions it retired, and
> must poll at its boundaries.** If it does not, one pump tick stops being `256 × cadence` *guest*
> instructions and every latency statement in `docs/AMIX.md` silently changes meaning — and the
> symptom is a missed interrupt, not a slow number. **This is the one item in the plan with
> cross-repo reach** (§6.2).

### The latency budget, with a precedent instead of an invented number

Rung 0 already answered "how long may the emulator go without polling" and answered it with a
knob: `SHARED.blk_chunk`, default **256 iterations ≈ 42 µs at 1.1 GHz**, chosen to sit inside the
**64 µs** of one 15.6 kHz hsync — *"the tightest cadence anything on this poll depends on"*.
**(measured)**

> **The engine adopts the same budget: no uninterruptible stretch — block execution *or*
> compilation — may exceed ~42 µs**, and it is exposed as one knob with the same `unset = default`
> polarity, for the same reason: *"a chunk size and not a yes/no because the chunk is what bounds
> interrupt latency, so one knob answers three questions a metal session has."*

### Where compilation runs

**Core1, inline, in bounded units — for M1 through M4.** Compile a block at its head; if it cannot
be compiled inside the budget, split it. The first execution of every block pays for it;
amortisation is the block's execution count, which is a counter from day one.

**Core0 offload is a named, deferred option and it is gated on its own evidence.** The case for it
is real — core0 free-runs at 99.7 % idle iterations, and C3 measured that turning its statistics
off bought only +0.1 %. The case against it is four measured facts:

1. **The AMP coherence lesson.** *"A parked Cortex-A9 still speculatively refills L1D."* Core0
   writing generated code into DDR for core1 to execute needs full cross-core maintenance that the
   JIT's single-core sequence does not perform — and the SMP bit and the maintenance-broadcast bit
   are **already set** in `boot.S`, and the coherence bug happened anyway, because the PL310 sits
   below the SCU and the SD DMA master is outside the coherency domain entirely.
2. **The producer/consumer rule this repo derived from it**: the *consumer* invalidates, using
   knowledge it already has; the alternative — a `SHARED` epoch word on the producer — was rejected
   as *"fail-DANGEROUS, silently"*.
3. **Core0 is not actually idle in the ways that matter.** The blocking-path audit lists twelve
   callees with no deadline, four of them unbounded; core0 and core1 share **one UART behind one
   semaphore** and core0 *spins* while it waits, so a long core1 dump starves the piscsi service
   for its duration; and `core0_hold` has no deadline in either direction.
4. **The measured gap that has never been explained**: 228 278 serviced piscsi requests with a mean
   inter-service gap of 18.4 ms and **one gap of 118 seconds**. Adding a new cross-core dependency
   before that is understood is adding a variable to an open investigation.

### Pre-registerable claims

1. **Worst-case uninterruptible stretch ≤ ~42 µs**, measured on target, block execution and
   compilation separately.
2. **The PSTAT gap fingerprint is unmoved** with the engine on: card-1/piscsi class is
   **1 177 – 1 402 µs** and the emulated-A3000 class is ~2 923 µs, and that difference is a
   controller fingerprint the campaign has already used as a behavioural proof.
3. Compile time for one block of the first family, measured on target **before** any integration.

---

# 3. Correctness strategy — the interpreter as differential oracle

## 3.1 The comparison set

Rung 2's corpus already defines what "the same" means on this machine, and the JIT adopts it
unchanged: **the returned value, the fault outcome, the fault address, the guest PC, the write-back
slots, `mmu_ssw`, `mmu_cache_state`, the destination memory, all sixteen data and address
registers, and the page-cache counters.** Add, for the JIT: `mmufixup[0..1]`, `mmu_restart`,
`regs.instruction_pc`, and the retired-instruction count. **(measured pattern)**

## 3.2 Lockstep modes

| mode | where | what it proves | cost |
|---|---|---|---|
| **per-instruction** | host harness | codegen semantics for one instruction, exhaustively | high; host only |
| **per-block** | host harness *and* metal | run a block jitted from a checkpoint, restore, run the same span interpreted, compare the whole set | affordable on metal in a gated build |
| **counter identity** | metal, every arm | `INSNS`, `FAULTS`, `DOPC_*`, `DFAST_*`, `IV_*` reconcile between arms | free |

**The honest limit, stated once:** the host harness runs on x86-64. It can prove **block discovery,
keying, invalidation, bail decisions and state write-back** against the real `m68k_run_mmu040` — it
cannot execute one instruction of ARM codegen. **Generated code is proven on target, and only on
target.** That asymmetry shapes the milestone gates: everything provable on the host is proven
before a metal session is booked.

## 3.3 The corpus, and the anti-vacuity device

The harness lives at `Z3660_emu/test/host/harness_main.cpp` (currently 3 186 / 3 182 / 3 390 tests
across the three build variants) and drives **the genuine run loop**, not a transcription — because
*"ISSUE-52 round-8 hid from a hand-copied CATCH for exactly that reason: a copy and the original
can drift, and the defect lived in the state the real CATCH consumes."* **(measured)**

> **Engagement pinning is mandatory and it is the point.** *"'The two paths agree' is trivially true
> when the fast path silently never fired — the failure a state-equivalence corpus is blindest to.
> Every case declares MUST serve or NEVER serve and a counter checks it. Half the corpus is decline
> cases; without the engagement check they would all pass against a fast path that was never
> compiled in."* For a JIT the decline surface is much larger than any rung's, so this matters more
> here than anywhere it has been applied so far.

**Mutation testing is a gate, not a nicety.** Every rung in this campaign injected defects and
required the corpus to catch them — rung 2's corpus caught two real double-count defects on its
first run; rung 3 pinned four mutations; SPEC swept 6 052 encodings × 3 arms. The JIT's mutation
set is pre-registered at M0 and grows with each milestone.

## 3.4 Pre-registration discipline

The port lane's `ACCEPTANCE.md` is the model and the campaign has been running it: platform classes,
identity before numbers, exact per-test pass lines, expectations registered **before** the run, and
a "what this does not establish" section. Two mechanical rules ride with it:

- **The pre-registration commit precedes the code commit**, provably. The SPEC round's did by 26
  minutes; the `f` round's by 29.
- **A pre-registration that gets rewritten is not one.** `ifetch-fastpath.md` scored itself
  **MISSED** and left the prediction byte-untouched, and `handler-discriminator.md` proved its own
  by md5. That is the standard.

And the reading rules the campaign learned the hard way, all of which bind here:

- **Read counters before wall clock.** *"`IV_*` before wall-clock is not a suggestion: it is what
  said, before any timing existed, that rung 1b's page filter was aimed at a 6–9 % cause."*
- **State the scorer's rule with the direction it scores**, before scoring. A bare ">2 % is a
  change" rule once flagged a count moving 6 → 4.
- **Check a pre-registered counter direction against the counter's position in the call topology.**
  A counter reached only *after* the branch being deleted cannot fall when it is deleted — a
  declared direction was once arithmetically impossible.
- **Every A/B same-boot, plus a cross-build reference row**, with the outgoing lean image archived
  first. Cross-session drift on this rig is ~9 %; within-arm dhrystone spread is 0.03 – 0.70 %.

## 3.5 The switch, and why the disarm is not free

`WARP` (console), `SHARED.warp_on` at the next free offset — `SHARED` closes at `dfast_on`
(`0xFFFF00EC`) today, so the next word is `0xFFFF00F0`, the slot `IFAST` and `SPEC` each briefly
held and gave back. Resolved by `z3660_warp_resolve()` in one header **and nowhere else**, the
single rule both cores compile — exactly like `BLK`, `DOPC` and `DFAST`. **(measured layout)**

> ⚠ **`SHARED` has a live ABI hazard.** Upstream beta 21 appended eight floppy fields at
> `0xFFFF009C..0xFFFF00B8` — *exactly* where `debug_emu` / `amix_mode` / `service_cadence` /
> `perf_report` sit. A mis-merge would alias `service_cadence` onto `write_floppy_data`. Any new
> `SHARED` field lands with that hazard checked, in both `main.h` copies, with the runtime size
> assertion.

**Polarity — and this one deliberately breaks the house rule.** Every shipping rung uses
`unset (0) = ON`, because *"a core0 built before the field existed publishes a zeroed struct, and
that must run the shipped behaviour rather than silently disarming the rung."* The engine is not
shipped behaviour until M5.

> **Decision: `unset = OFF` until the M5 ship gate, then the user decides whether to flip it.**
> (**Q9** — this is a deliberate inversion and the grill should challenge it.)

**The disarm costs real time, and the verdict number is never the switch.** Measured, three times:

| rung | shipping (cross-build) | disarm penalty | same-boot switch | identity |
|---|---|---|---|---|
| rung 2 `DFAST` | +25.17 % | **−3.74 %** | +30.03 % | 1.2517 × 1.0388 = 1.3003 ✔ |
| rung 3 `IFAST` | −0.05 % | **−1.58 %** | +1.55 % | 0.99947 × 1.01604 = 1.01550 ✔ |
| `SPEC` | −7.55 % | **−1.07 %** | −6.55 % | 0.98926 × 0.93452 = 0.92448 ✔ |

*"Rung 3's switch alone would have reported +1.55 % and looked like a small win."*

> **Standing rule for every WARP verdict: quote the cross-build row, take the same-boot switch
> beside it, and require the identity `cross-build × disarm-penalty = same-boot switch` to close.**
> It has closed to four or five digits three times; a verdict where it does not close is a
> measurement error, not a result.

## 3.6 Metal-session discipline

Inherited whole, because each item was bought with a failure: the **03-CARD-AS-FOUND CRC capture**
before anything; **work-copy root disks** for any arm that could corrupt (proven — installed root
CRC identical before and after, `30FE1733`); **CRC-gate every hop** and `FAILSAFE.bin` first;
**archive the outgoing lean image before rebuilding** (a rebuild once destroyed the only copy of
the reference arm and made a flash-revert impossible); read arm/disarm state from **a counter, not
a banner** (two cores share one unlocked UART and an arm banner arrives spliced); `PSTAT` off
during ring dumps; `/etc/haltsys`, not `shutdown` or `init 0`; and **screencap and analyse every
boot message** — on a first-of-a-kind boot, a new console line is a finding by default.

---

# 4. Milestones

Every milestone has a pre-registered gate and a **kill criterion**, and the kill criteria are the
point. Five candidate rungs died by pre-registered measurement in the interpreter campaign, and
that is why its numbers are trustworthy. Session counts are **assumed**.

| # | milestone | what it proves | measurement | **kill criterion** | est. |
|---|---|---|---|---|---|
| **M0** | **Design freeze** (post-grill) | the route is chosen, the unwind question is answered, the contracts are written and pre-registered | 1 static audit + 2 zero-build censuses off banked data + **1 target boot** (the unwind probe) + optionally 1 profiling boot (the §1.5 base re-take) | **Q2 says a throw cannot be contained AND bail-before-fault cannot cover the access classes** ⇒ stop here, for the price of one boot | 2–3 |
| **M1** | **Straight-line user-mode blocks** — dhrystone runs jitted | codegen, block cache, chaining, the poll and bail contracts | host corpus green + metal cross-build dhry + coverage counter + per-block lockstep | **cross-build dhry ≤ 8 185 with coverage in band and no named implementation defect** ⇒ the mechanism does not pay on this machine; revert, as rung 3 did the same day | 6–10 |
| **M2** | **Faults and demand paging survive** — miniroot boot | the bail contract on the path `f` explicitly did not test | miniroot boots to the installer prompt with the engine on; `FAULTS`/`INSNS` identities; full boot-message analysis | **a divergence class that cannot be reduced to a bail rule** ⇒ the fault contract is wrong, back to M0 | 4–8 |
| **M3** | **Full multiuser boot on metal** | the whole contract in the shipping posture | installed 040 system, piscsi/card-1 root, 128 MiB, cadence 4: multiuser + login + `zen0` 0 % loss + clean `/etc/haltsys`; dhry, boot time and raw-device read all same-boot A/B | **any correctness event at all** | 3–6 |
| **M4** | **Soak + the ladder inside the JIT** | durability, and where the next rungs are | long multiuser run under paging and network load + the 128 MiB memory toucher; then per-rung §30.3 pricing | per rung, not per milestone; a rung that misses its pre-registered band is reverted | 8–15 |
| **M5** | **Ship gate** | it is finishable | ACCEPTANCE record, changelog, docs, CRC-stamped image lineage, disarm-equivalence proof, "what this does not establish" | — the **default-on decision is the user's** | 2–3 |

### M0 — Design freeze `DONE`

**State on 2026-08-23: the grill has run, §0 is the freeze, the metal half has run, and the M1
pre-registration is committed. All seven items are `DONE`** — the unwind probe answered
`TERMINATED` on one boot (§0.4.1), the optional §1.5 base re-take was taken and the base survived
(§0.4.2), the four zero-build censuses are §0.3, and **§0.5 is the pre-registration, committed
before the first line of engine code.** The per-item ledger is §0.4. One census was added at the grill and is not in the list of five: the state-discipline
census (§0.3(b)), which is the input to Q1's pre-registered state-sync measurement.

Five cheap things, all before any codegen. Only the first needs the card.

1. **The unwind probe (Q2).** Emit a four-instruction stub into `malloc`'d memory, call it, have it
   `bl` a C++ function that throws, and catch outside. One target boot answers it, and a negative
   answer here ends the project for the price of that boot.
2. **The route audit (Q1), zero build.** Count how many of `compemu_arm.cpp`'s per-opcode compilers
   reach `readmem_real` / `writemem_real` / `get_n_addr_real`. If it is nearly all of them, Route A
   is a rewrite wearing a retrofit's clothes, and the grill should know that before choosing.
   *(Corrected at M0: those three are `STATIC_INLINE` and a per-opcode compiler never names them.
   The symbols to count are the ten public accessors they sit behind —
   `readbyte`/`readword`/`readlong`, `writebyte`/`writeword`/`writelong`, the two `_clobber`
   twins, and `get_n_addr`/`get_n_addr_jmp` — plus the one bypass, `jnf_MOVE16`. Answer: 79.44 %,
   §0.3(a).)*
3. **The family census (Q7), zero build.** Re-read the banked sampler rings for the time-weighted
   opcode histogram — it already exists. `MOVE`/`MOVEA` is 27.5 – 31.5 % of dispatch time with
   covered EA shapes at 22.9 – 26.4 %. Pick the ring whose `INSNS_SUPER` matches the target
   workload, not the most convenient one.
4. **The block-length census, zero build.** Derive mean block length from the same rings *before*
   predicting it, so the prediction in §2a is a prediction and not a readout.
5. **Optional: the §1.5 base re-take.** One profiling boot puts `DOPCFIND`, `LOOPRES`, the fetch
   pair, `TAILPOLL` and `TAILSPEC` on one capture at one band, closing the comparability caveat in
   §1.5. Worth taking if a metal session is being booked anyway; not worth booking one for.

**Deliverable:** this document, updated with the decisions, plus a committed pre-registration for
M1 — its band, its coverage bar, its counter directions checked against call topology, and its
revert line — **before** the first line of code.

### M1 — Straight-line user-mode blocks `IN PROGRESS — round 1 DONE`

> **Round 1, the plumbing slice, is `DONE` (2026-08-24).** Blocks are discovered, keyed,
> cached, invalidated, evicted, entered and bailed — and every block is a null block, so no
> code is generated and **no performance claim is made or is admissible**. Its
> pre-registration is §0.6 and its scored gates are §0.7: the harness is green in all three
> variants, the armed and disarmed arms are byte-identical over **10⁷** compared instruction
> boundaries with `WARP_DIVERGE = 0`, all five mutation arms are caught, and the three
> shipping images carry the switch (`9C7A2AD3` / `E80D04EE` / `423CB391`).
>
> **The one number it produced is a cost: 60 ARM instructions on the armed path against a
> pre-registered 24 – 40, and 5 on the disarmed one against 3 – 6.** The armed figure is
> `MISSED, high`, ~22 of it is named and none of it is fixed in the round it was measured
> in, and the re-derived metal expectation is a same-boot switch of **−13.6 % … −19.2 %**.
> That is §30's pricing law taking its first bite, exactly where R7 said it would.
>
> **Round 2 is the first per-opcode compiler**, and it re-registers its own band.

Compile only: register-to-register ALU, `MOVE`/`MOVEA` in the covered EA shapes, and the
conditional branches that end blocks. No FPU, no privileged instruction, no write-back family, no
trace, no supervisor-only path if it can be avoided.

**Gate (pre-registered at M0, illustrative here):** per-block lockstep clean over ≥ 10⁸ retired
guest instructions; coverage ≥ 60 % of retired instructions; `FAULTS` and `INSNS` identities exact;
the static gate on `throw` reachability met; **cross-build dhrystone above 8 185 by more than the
rig's own within-arm spread (0.04 – 0.70 %)**, with the first-generation band of §1.5 recorded
beside it for information, not as the bar.

> **§0.5 is the pre-registration this line promised, and it is binding where the sketch above is
> illustrative.** It narrows the shape set (`(An)+` / `-(An)` are excluded by §2e's own rule),
> corrects the `INSNS` identity to the reconstruction rung 0 already needed, derives **M1's own
> band 1.04× – 1.33×** from what generation one covers, operationalises the kill line against a
> **same-session** reference row, and pre-registers the coverage counter, the new counter ids and
> every declared direction's topology check. **Read §0.5 before writing a line of M1.**

**Kill:** see the table. Rung 3's precedent binds — a rung that engages, works, and does not pay
comes out the same day.

### M2 — Faults and demand paging `GATED(M1)`

The miniroot boot is the right first fault workload because it pages heavily, it is short, it is
already a routine harness target, and it does not risk an installed root. Work-copy discipline
applies regardless.

### M3 — Full multiuser boot on metal `GATED(M2)`

Shipping posture, no exceptions: preset 7, `amix_ram 128`, `service_cadence 4`, card-1/piscsi root.
Take the boot-window row as well as dhrystone — **the JIT's value is probably larger on the
supervisor-heavy workloads**, where the supervisor share is 48.77 % against dhrystone's 17.98 %,
and `INSNS_SUPER` is the cheapest discriminator between capture windows (**Q11**).

### M4 — Soak and the internal ladder `GATED(M3)`

Rung candidates, none funded in advance: block chaining in the steady state; superblocks/traces;
register allocation across block boundaries; inlining the `PAGE` probe; hoisting the poll;
subsuming rung 0; the compile-path `ifetch` table (**Q6**); per-address-space generations (**Q5**).
Each gets §30.3's four steps — name the latency, ask what is hiding it, count the added work
separately, and buy the answer with one lean A/B **plus** a cross-build reference row.

### M5 — Ship gate `GATED(M4)`

---

# 5. Risks

| # | risk | why it is real *here*, with evidence | mitigation |
|---|---|---|---|
| **R1** | **Divergence that surfaces hours later** | the campaign's own taxonomy for a missed invalidation: wrong results silently, **wild control flow** (a stale entry's handler is an arbitrary opcode's), **wild writes into the kernel's buffer cache**, panics. This repository has already had a page-crossing DMA clip an unrelated frame and stay invisible until it ate an ELF header | bail-before-fault (§2e); engagement-pinned corpus; per-block lockstep as a gated build mode; work-copy roots on metal; counter identities read before wall clock |
| **R2** | **A9 / L2 coherence on generated code** | `8be8949`: the first `pushall_call_handler()` fetched stale I-cache lines → **ARM Undefined Instruction → reboot**, in this tree, caused by a *compiler-flag change*. The live sequence has **no ISB** and no branch-predictor invalidate, and CP15 ops do not maintain the PL310. And `dsb-experiment` proved that **adding barriers is not evidence of a fix — only a soak on metal is** | settle the full sequence at M0 against the Cortex-A9 TRM and the in-tree `finish_Attributes()` template (**Q3**); core1-only generation; no core0 offload before its own gate; M4 soak is the acceptance, not the reasoning |
| **R3** | **Cadence starvation** | one pump tick = `256 × cadence` **guest** instructions; at cadence 8 the A3000-SCSI bootstrap already hangs, and a network flood at 16+ has **panicked AMIX**. The failure mode is a missed interrupt, not a slow number | blocks feed the cadence counter with retired instructions; poll at block boundaries; the ~42 µs budget with rung 0's precedent; PSTAT gap fingerprint as a behavioural check; boot-time cadence ≤ 4 stands |
| **R4** | **Code-cache thrash / L1I pressure** | DDR capacity is not the constraint (32 MB already allocated from a 96 MB heap); **L1I is**. `m68k_run_mmu040` is 2 288 B of a 32 KB L1I today. The SPEC round measured **−1.07 % for merely inlining a body into the loop**, of which the larger half was register pressure the footprint table priced at zero | code bytes per guest instruction as a first-class pre-registered metric; short generation-one blocks; helper calls where inlining explodes size; an L1I-pressure check in M1's A/B |
| **R5** | **The flags cost eats the saving** | the interpreter snapshots `regs.ccrflags` on **every** instruction for restart. If the JIT must too, a large part of the §1.5 base evaporates | snapshot only at fault-visible boundaries; the ARM-native CCR layout means the snapshot is one word (§2c); measure `r` directly rather than assuming it |
| **R6** | **FPU divergence** | `cpuemu_31` dispatches `fpuop_*` at 40 sites, so FP is ordinary handler work under the MMU; `fptype` is host `double`, not 80-bit extended; and **AMIX-040 sets `fpu_no_unimplemented`** so the emulator raises genuine vector-11 traps for the kernel's Motorola FPSP. A JIT that computed an FP result itself would silently bypass FPSP | **the FPU stays interpreter-only through M5, and this is a correctness decision, not a scheduling one.** FP opcodes are a bail family. `compemu_fpp_arm.cpp` stays unreachable under the MMU (**Q13**) |
| **R7** | **The pricing law turns on us** | rung 3: +19 prepended instructions arrived in full (−1.58 %), 42 removed ones returned nothing. A block-entry lookup is a prepended serial dependency by construction | chain blocks so the lookup leaves the steady state; count the entry sequence from `objdump` before metal; M1's kill criterion exists precisely for this outcome |
| **R8** | **Compilation latency on a cold guest** | every block on the boot path is compiled once, and boot is where the block-idiom rung found its 43.6 % | the ~42 µs unit budget; measure compile time per block at M0; a hot threshold is a cheap fallback |
| **R9** | **Build fragility** | `-Os -flto`, everything compiled as C++, a 1.6 MB generated compiler TU, header-dep tracking only added recently, `.DEFAULT_GOAL` pinned after `make bootbin` silently stopped rebuilding core1, `#pragma GCC optimize("O3")` already needed in the JIT TU, and the host harness must run on the host (the Vitis container's glibc is too old) | clean builds for every flashed image; the three build dirs stay separate; CRC-stamped archive on every build; byte-identity checks between rebuilds (the lean image has already rebuilt byte-identical across sessions, twice) |
| **R10** | **Core0's unexplained 118-second gap** | measured, unexplained, and still open. Any new cross-core dependency adds a variable to an open investigation | no core0 offload until that investigation closes; §2g |

---

# 6. Relation to the rest of the program

## 6.1 Track G — the UAE-family port

**Track G gets the interpreter rungs, not the JIT.** Rung 0, rung 1/1c and rung 2 are
hardware-independent, live in shared WinUAE-4.4.0-lineage files, and speed up every bench cycle
while improving bench/metal parity; the x86 host is also the portability proof, and the smaller
ratios expected there are themselves data.

The JIT is a different case in both directions. Amiberry *already has* an ARM and an x86 JIT for
its non-MMU modes, so the bench gains nothing from a port until there is something that works
under the MMU. And the value of a JIT-under-MMU to the UAE family is real but specific: the
beneficiaries are the full-MMU interpreter modes (AMIX, NetBSD, Linux-68k, A·UX), which is UAE's
never-optimised path.

> **Interplay to decide at M0 (folded into Q1):** if the engine is built as a new MMU-native layer
> over shared-lineage files rather than as a fork of `compemu_*`, it stays *portable and offerable*
> on the same terms as the rungs — behind the push-hold and the per-push permission gate. If it is
> built as a retrofit of this tree's `compemu_*`, it is ours alone. That is a licensing- and
> collaboration-shaped consequence of a technical choice, and it should be made deliberately.

## 6.2 The 68040 port lane and the silicon collaboration

**The JIT is our emulator's lane only. No guest kernel changes, no ISSUE numbers on the shared
branch, no acceptance rows on his repo.** The kernels that boot today boot unchanged.

What legitimately crosses: the C2 attack map already lives in that repo and already records row J.
A WARP verdict — hit or null — is worth a map version, as a **map entry**, not as a kernel change.
The map's own rule applies: the verdict is quoted with its band, its window and its
`INSNS_SUPER`, and a null is reported as fully as a hit.

**Cross-repo interface reach.** This project changes **no emulated interface**: not the 68040 PMMU
semantics the guest kernel sees, not the Zynq GEM frame path `amix-z3660net` consumes, not the
piscsi mailbox `amix-z3660scsi` speaks. **One exception, and it is §2g's rule:** if the cadence
contract moves — if one pump tick stops being `256 × service_cadence` guest instructions — that
reaches both driver repos through interrupt latency, and it is a cross-repo change requiring the
provider-before-consumer sequence. **The plan's position is that the contract does not move, and
M3's PSTAT fingerprint check is how that is demonstrated rather than asserted.**

## 6.3 Review gates

Commits here, never pushes. Changelog entries are proposed in the report and written post-review.
Verification is the standing checklist: whole-range identity and trailer scan, sweep, acceptance
bullets, then land.

**Public-fork hygiene applies to this document itself.** It is written for an outside reader: no
local paths, no internal process vocabulary, and the fork-boundary rule stands — keep
Amix-specific changes inside the fork boundary where possible so an upstream merge stays clean.
GPL-3.0, full credit to sHaNsHe.

## 6.4 The other tracks

- **Track B:** B1b's 128 MiB memory toucher becomes an M4 soak input rather than a separate errand.
- **Track D:** storage work is orthogonal but shares the cadence knob; a WARP session must not
  change `service_cadence` under a D measurement, and vice versa.
- **Track E3:** the 060 MMU mode inherits this engine only if the engine's 68k-model assumptions
  are isolated from the start (**Q13**). That is a cheap discipline at M0 and an expensive
  retrofit later.

---

# 7. Open questions for the grill  —  ALL RESOLVED 2026-08-23

Each has a recommendation and the cheapest experiment that would settle it. Nothing below is
decided.

> **All thirteen were resolved by the grill of 2026-08-23. Each carries a `RESOLVED` line
> pointing at the decision in §0.1, and the recommendation and cheapest-experiment text below it
> is left byte-untouched — including where the outcome differed from the recommendation, and
> including where a stated fact turned out wrong.** §3.4's rule is that a pre-registration which
> gets rewritten is not one, and a recommendation is a prediction like any other. Read §0.1 for
> what was decided; read this section for what was predicted before it was.

### Q1 — Route A (retrofit the in-tree UAE ARM JIT), Route B (a new MMU-native engine on its primitives), or Route C (hybrid)?

> **RESOLVED 2026-08-23 → §0.1 Q1: Route C, as recommended.** The cheapest experiment was run
> as census (a) (§0.3(a)) and answered **79.44 %** — nearly all of them, so Route A is a rewrite
> wearing a retrofit's clothes and the grill said so. The route carries one explicit
> pre-registration: the **state-sync overhead per faultable access is an M1 measurement**, and
> the allocator is redesigned toward Route B's shape only if that number says it is the ceiling.
> *One factual correction below: the memory model is `r10 = memstart`, not `r11` — §2c.*

**Recommendation: C.** Take Route B's block manager, keying and bail contract; reuse
`codegen_arm.h`'s raw emitters, the `blockinfo` / `cache_tags` structures, `flush_cpu_icache`, the
code-cache allocator and the flag machinery. Do **not** inherit `compemu_arm.cpp`'s per-opcode
compilers, whose entire memory model is `r11 = memstart` plus a bswap.

**Cheapest experiment:** count how many of `compemu_arm.cpp`'s per-opcode compilers reach
`readmem_real` / `writemem_real` / `get_n_addr_real`. One grep and one script, zero build. If it is
nearly all of them, Route A is a rewrite wearing a retrofit's clothes and the grill should say so
out loud.

### Q2 — Can a C++ `throw` cross a frame of generated code on this toolchain, or does it terminate?

> **RESOLVED 2026-08-23 → §0.1 Q2: the recommendation is adopted as the design posture.**
> Bail-before-fault everywhere; nothing in §2e is contingent on the probe. The probe itself is
> built (`make q2probe` → `BOOT-q2probe.BIN`, `Z3660_emu/src/warp_q2probe.cpp`) and awaits one
> boot; it confirms or kills the posture, it does not choose it.

**Recommendation: assume it terminates, and design so no throw ever crosses** — bail-before-fault
(§2e option 2). Everything else in the fault chapter follows from this answer.

**Cheapest experiment:** emit a four-instruction stub into `malloc`'d memory, call it, have it `bl`
a C++ function that throws, catch outside. One target boot. **This is the first thing M0 does,
because a negative answer that arrives after codegen exists is expensive and the same answer
arriving now is nearly free.**

### Q3 — Is the existing `flush_cpu_icache` sequence sufficient for ARMv7-A self-modifying code on this A9 + PL310?

> **RESOLVED 2026-08-23 → §0.1 Q3: as recommended.** `ISB` + branch-predictor invalidate are
> added to the sequence **before** the engine raises flush volume. The `UAEJIT`-bootmode A/B is
> the settling experiment, on metal, later. The probe image exercises the shipped sequence on
> one stub and the augmented sequence on the other — a data point, not the experiment.

**Recommendation: add `ISB` and a branch-predictor invalidate before trusting it at JIT volumes,
and price the addition** — it is per-block, not per-instruction, so it is almost certainly
affordable. Do not assume the L2 question away either: CP15 maintenance does not reach the PL310,
which is exactly why the hand-rolled path is faster than the Xilinx one it replaced.

**Cheapest experiment:** an A/B *in the existing `UAEJIT_040` bootmode*, which already runs
generated code today — a JIT-heavy AmigaOS workload with and without the extra barriers, watching
for Undefined Instruction events; plus the Cortex-A9 TRM and the PL310 documentation read against
`finish_Attributes()`. And remember the standing platform caveat: **a barrier change that stops a
symptom is not a proof; only a soak is.**

### Q4 — Where does compilation run: core1 inline, or core0 offload?

> **RESOLVED 2026-08-23 → §0.1 Q4: as recommended.** Core1 inline, units bounded by rung 0's
> ~42 µs budget, for M1–M4. Core0 offload stays gated, and its gate includes the unexplained
> 118-second core0 gap closing first.

**Recommendation: core1 inline for M1–M4**, in units bounded by rung 0's ~42 µs budget. Core0
offload stays a named option behind its own gate, and that gate includes the unexplained
118-second core0 gap being closed first.

**Cheapest experiment:** measure the compile time of one block of the first family on target,
before any integration. If it is far inside 42 µs, the question is settled for generation one.

### Q5 — Does the block cache need a per-address-space generation (`URP`/`SRP`), which the dopc declined to fund?

> **RESOLVED 2026-08-23 → §0.1 Q5: deferred to its pre-registered M1 counter** — the `IV_ROOT`
> blocks-discarded ratio. **Not built early.**

**Recommendation: probably yes** — and it is an argued *reversal* of a documented decision, so it
must be argued in the open. The dopc declined because a per-entry stamp costs three more compares
on a path that runs every dispatch. A JIT's lookup runs once per **block**, so the same three
compares are amortised over the block's length while the thing being thrown away is far dearer.

**Cheapest experiment:** in the M1 build, count blocks discarded by `IV_ROOT` against blocks built.
`IV_ROOT` is 1.32 – 3.27 % of *requests*; the ratio that matters is of *blocks*, and it is a
counter.

### Q6 — Should rung 3's struck `ifetch` verdict table (`3087af9`) be resurrected for the *compile* path?

> **RESOLVED 2026-08-23 → §0.1 Q6: deferred to its pre-registered M1 counter** — a compile-path
> fetch counter. **Not built early**, and not re-funded from a bucket share before that counter
> exists.

**Recommendation: yes as a hypothesis, no as a claim.** The law struck it on the execution path
because the dopc was hiding the fetch it sat beside. On the compile path nothing is hiding that
fetch, and the compiler reads several words back to back — which is a different classification
under the same law, not an exception to it.

**Cheapest experiment:** none until M1 compiles something; then it is a counter on the compile
path, not a build. **Do not resurrect it before that counter exists** — re-funding an
ifetch-side rung from a bucket share is the one thing §30 strikes outright.

### Q7 — Which opcode families does generation one compile?

> **RESOLVED 2026-08-23 → §0.1 Q7: as recommended, and pinned to the dhrystone window** per
> Q11. Generation one is `MOVE`/`MOVEA` in the covered EA shapes, register-register ALU, and the
> `Bcc`/`DBcc` that terminate blocks. The kernel-window `(xxx).L` shape is a **named M4 ladder
> candidate contingent on the boot rows**, not a generation-one family. §0.3(a) adds the
> inheritance detail the family choice needs: `Bcc` and `DBcc` are 45/45 and 16/16 pure and
> inheritable as-is, while `MOVE` is 272 of 281 memory-touching and is not.

**Recommendation:** start from the measurement that already exists rather than from taste.
Time-weighted sampler-ring histograms put `MOVE`/`MOVEA` at **27.5 – 31.5 %** of dispatch time —
twice the next family — with the covered EA shapes (`Dn`, `An`, `(An)`, `(An)+`, `-(An)`,
`d16(An)`) at **22.9 – 26.4 %**. Add register-to-register ALU and the `Bcc`/`DBcc` that terminate
blocks.

**Cheapest experiment:** none needed. The rings are banked; re-read them. One caution from the same
data: in a kernel-heavy window the `MOVE` family's covered share falls from ~83 % to **57.6 %** and
`(xxx).L → (xxx).L` alone becomes 37 % of the family — **so pick the family set against a window
whose `INSNS_SUPER` matches the workload the engine is meant to serve.**

### Q8 — Does rung 0's block-idiom fast path stay, get subsumed, or get retired?

> **RESOLVED 2026-08-23 → §0.1 Q8: as recommended.** Rung 0 stays untouched through M3 and the
> JIT declines its PCs.

**Recommendation: stays, untouched, through M3.** The JIT declines those PCs. It is worth −43.6 %
of a boot, measured, and it protects rung 1 by collapsing the `CPUSHL` invalidation storm 18×.

**Cheapest experiment:** at M3, one same-boot `BLK` on/off A/B with the engine on — and the
campaign's rule read out loud first: *a cost measured with another rung switched off is not a cost
the shipping firmware pays.*

### Q9 — What is the `WARP` switch's default polarity?

> **RESOLVED 2026-08-23 → §0.1 Q9: as recommended — `unset = OFF` through M4**, a deliberate
> inversion of the house rule with the reason recorded. The flip to `unset = ON` happens at M5
> on the user's word and not before. **Arm attestation rides every capture.**

**Recommendation: `unset = OFF` until M5**, deliberately inverting the house rule, then flip at the
ship gate if the user says so. The house rule exists so a zeroed `SHARED` struct from an older
core0 still runs *shipped behaviour*; an unshipped engine's shipped behaviour is "off".

**Cheapest experiment:** none — this is a judgement call, and it is here because the grill should
push on it rather than inherit it.

### Q10 — Does a block feed the cadence counter with its retired instruction count, or does the cadence contract change?

> **RESOLVED 2026-08-23 → §0.1 Q10: as recommended, and the timebase contract is FROZEN.**
> Blocks feed the cadence counter with retired guest instructions. This is the one item with
> cross-repo reach, and the **PSTAT gap fingerprint is a proof obligation on every metal
> verdict** rather than a check to run if there is time.

**Recommendation: it feeds it.** One pump tick must stay `256 × service_cadence` *guest*
instructions, or every latency statement in `docs/AMIX.md` silently changes meaning and the failure
mode is a missed interrupt. **This is the one item in the plan with cross-repo reach.**

**Cheapest experiment:** the PSTAT gap fingerprint. Card-1/piscsi class is 1 177 – 1 402 µs; the
emulated-A3000 class is ~2 923 µs. If the fingerprint is unmoved with the engine on, the timebase
is unmoved.

### Q11 — Is the perf gate dhrystone, or something bigger?

> **RESOLVED 2026-08-23 → §0.1 Q11: as recommended. Dhrystone stays THE gate.** Every verdict
> capture records a boot-window row and `INSNS_SUPER` beside it; the kernel-heavy story
> accumulates evidence and **gates nothing before M4**.

**Recommendation: dhrystone stays the scoring workload** — map v3.4 ratified the pinned-dhrystone
window as the right pricing basis, and it is what every number in this document is comparable to.
**But take a boot-window row beside it**, because the JIT's value is plausibly larger where the
supervisor share is 48.77 % than where it is 17.98 %, and because miss-driven buckets move 2–5×
with that share while per-dispatch buckets do not.

**Cheapest experiment:** it is a second row in the same capture, plus `INSNS_SUPER` recorded as the
window discriminator.

### Q12 — What kills the project, and do we agree to that now?

> **RESOLVED 2026-08-23 → §0.2, and the number is written down.** The kill line is agreed and
> **binding**, the rung-3 precedent binds by name, and **there is no re-scoping**. It is quoted
> verbatim in §0.2; it is not paraphrased anywhere in this document.

**Recommendation: pre-register M1's kill line in this document, before any code, and treat rung 3's
precedent as binding.** Rung 3 engaged at 91.10 % coverage, made its bucket 23 % cheaper per
instruction, hit or beat every premise it declared, moved the wall clock **−0.05 %**, and the code
came out the same day. A JIT that does the same is the same finding, and the project's credibility
rests on being able to say so.

**Cheapest experiment:** write the number down at M0.

### Q13 — Is the engine written 68k-model-agnostic, and does the FPU ever get compiled?

> **RESOLVED 2026-08-23 → §0.1 Q13: as recommended.** Model-agnostic yes — the code-organisation
> review happens when implementation starts. **FPU permanently interpreter-only**, on the
> vector-11 / FPSP contract, and that decision does not expire at M5. §0.3(a) notes the
> consequence: the five `R_MEMSTART` sites in `codegen_arm.cpp` are the FPU's own memory path,
> and Q13 is what keeps them unreachable.

**Recommendation: model-agnostic yes, FPU no.** Isolating the 68040-specific assumptions costs
almost nothing at M0 and is what lets Track E3's `UAE_060_MMU` inherit the engine instead of
forking it. The FPU stays interpreter-only permanently, and the reason is correctness rather than
scheduling: AMIX-040 sets `fpu_no_unimplemented` so the emulator raises real vector-11 traps for
the kernel's Motorola FPSP, `fptype` is host `double` rather than 80-bit extended, and a compiled
FP path that computed results itself would silently bypass FPSP — the exact divergence that
configuration line exists to prevent.

**Cheapest experiment:** for the FPU half, none — it is settled by the configuration comment and
the 40 `fpuop_*` dispatch sites. For the model-agnostic half, it is a code-organisation review at
M0, not an experiment.

---

# 8. What this plan does **not** establish

Written now, before any measurement, so it cannot be written to fit one.

1. **That a JIT pays on this machine.** Row J is an upper bound on a machine that does not exist,
   and §1.5's band rests on two `assumed` deflators with the cost side unquantified. The campaign's
   whole lesson is that a design's sign cannot be read off a bucket's size. **M1 is the experiment,
   not a formality.**
2. **That the `HANDLER` bucket is recoverable.** It is UNCLASSIFIED — neither struck nor funded —
   and the one discriminator built to classify it measured its own implementation instead. A JIT's
   claim on it rests on compile-time specialisation being categorically different from runtime
   parameterisation, which is an argument, not a measurement.
3. **That the invalidation story survives paging.** Everything known about invalidation cost on
   this guest was measured on dhrystone: small, resident, and explicitly chosen as *the workload
   least likely to trip the paging path*. M2 is the first honest test.
4. **That the existing cache-maintenance sequence is correct at JIT volumes.** It works today at
   the existing JIT's flush rate. That is evidence, not proof, and the one prior failure in this
   exact class was triggered by a compiler-flag change and looked like a reboot.
5. **That the compile budget holds.** ~42 µs is inherited from rung 0's measured chunk, not
   measured for compilation. If a block's compilation does not fit, the unit gets smaller, and the
   consequences of that for amortisation are unquantified.
6. **That any of the session estimates are right.** They are `assumed`, they are stated so they can
   be wrong in public, and they are not a commitment.

### 0.19.5 Addendum — the blessing row (2026-08-24 evening)

The park's closing metal session (evidence `2026-08-24-warp-r6-blessing`, 124 files) scored
§0.17.2: the tax-fixed lean `9AECF99D` measured **7 947** against the ≥ 7 988 gate — MISSED by
41; band 8 100–8 163 missed low by 153; **rule 3 fires and the row is not re-banded**. The same
session's floor variant (`BOOT-abl2` `E2C1FBC5`, the same tree minus 64 B of cold attestation
`.text`) measured **8 130 — inside the band, gate cleared by 142** — and a closing re-flight of
the reference read 8 174.0 against an opening 8 174.0, zero drift. The 183-dhrystone gap between
two near-identical binaries is a binary property: link layout, the ±0.5 pp ambient class §0.15
priced. **The fix's code is vindicated; the nominated binary drew a losing layout; no rescue
round was taken** — chasing a 41-dhrystone gate through the layout lottery is the sub-floor
guessing this campaign retired. Armed for the record: 7 100 with all six counter bands HIT and
the engine's work identical to five significant figures. `WARPTEST` PASS 17/17 on everything
flown; selector clean both directions; console boot-path exact.

**The park's physical posture, final:** the standing card image is the clean interpreter lineage
`F294E75A` (fourth consecutive rollback, CRC-verified); the engine is preserved in-repo —
buildable, gated by `tools/warp-gate.sh`, `WARPTEST`-proven — as `BOOT-lean.BIN` `9AECF99D`.
Shipping a WARP-carrying lean requires layout control first, which is the same class of work §8
of the prospectus already orders ahead of any reopening (the E-round). Project decision,
2026-08-24: the miss is accepted as scored.
