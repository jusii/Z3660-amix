# Plan WARP ENGINE — a JIT under the MMU

*Named for MacroSystem's WarpEngine, the 68040 accelerator an A4000 took in the same slot this
card sits in. The interpreter campaign took the emulated 68040 from 3 818 to 8 185 dhrystones
(2.14×) and closed with one row left on the board. This is the plan for that row.*

**Status:** `M0 IN PROGRESS` — the grill ran on **2026-08-23** and every question in §7 is
answered. §0 is the freeze: it is the authority for everything below it, and where a §2
claim disagrees with §0's censuses, §0 wins and the §2 text has been corrected in place
with the correction marked. **Still no engine code: M1 opens when M0's deliverable —
this document plus the committed M1 pre-registration — is complete.**

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

### Q2 — exceptions: **design as if a throw cannot cross generated code** `DECIDED`

The posture is assumed-terminates: **bail before the fault, everywhere** (§2e option 2), so no
`throw` is ever raised on a stack that has a generated frame on it. Every fault-visible
decision in §2e follows from this and none of them is contingent on the probe's answer.

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
| 1. the unwind probe (Q2) | `BUILT, AWAITING ONE BOOT` | `make q2probe` → `BOOT-q2probe.BIN`. Excluded from all three shipping images by two independent gates; their CRCs are unchanged with it in the tree. |
| 2. the route audit (Q1), zero build | `DONE` | §0.3(a) — 79.44 % |
| 3. the family census (Q7), zero build | `DONE` | §0.3(a), read against the dhrystone window per Q11 |
| 4. the block-length census, zero build | `DONE` | §0.3(a) — faultable-site density; block length is predicted, not read out, in §2a |
| 5. the §1.5 base re-take (optional) | `TODO` | one profiling boot; worth taking if a metal session is booked anyway |
| 6. the state-discipline census | `DONE` | §0.3(b) — added at the grill as Q1's contract input |
| 7. the M1 pre-registration commit | `TODO` | the remaining half of M0's deliverable; must precede the first code commit, provably |

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

> **M0 status: the probe is built and awaits one boot.** `make q2probe` →
> `BOOT-q2probe.BIN`, from `Z3660_emu/src/warp_q2probe.cpp`. It runs two experiments before
> emulation starts — a throw *contained* inside the helper (§2e option 1 in miniature) and a
> throw *crossing* the generated frame — and prints a sentinel before each, so the outcome that
> destroys its own reporting is still readable as silence-after-a-named-sentinel. **The design
> posture below does not depend on the answer** (§0.1 Q2): bail-before-fault is assumed either
> way, and a CROSSED result would only mean option 1 is available as well, not that option 2 is
> abandoned.

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

### M0 — Design freeze `IN PROGRESS`

**State on 2026-08-23: the grill has run and §0 is the freeze. Items 2, 3 and 4 are `DONE`
(§0.3), item 1 is `BUILT, AWAITING ONE BOOT`, item 5 is `TODO` and optional, and the M1
pre-registration — the other half of the deliverable below — is `TODO`. The per-item ledger is
§0.4.** One census was added at the grill and is not in the list of five: the state-discipline
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

### M1 — Straight-line user-mode blocks `GATED(M0)`

Compile only: register-to-register ALU, `MOVE`/`MOVEA` in the covered EA shapes, and the
conditional branches that end blocks. No FPU, no privileged instruction, no write-back family, no
trace, no supervisor-only path if it can be avoided.

**Gate (pre-registered at M0, illustrative here):** per-block lockstep clean over ≥ 10⁸ retired
guest instructions; coverage ≥ 60 % of retired instructions; `FAULTS` and `INSNS` identities exact;
the static gate on `throw` reachability met; **cross-build dhrystone above 8 185 by more than the
rig's own within-arm spread (0.04 – 0.70 %)**, with the first-generation band of §1.5 recorded
beside it for information, not as the bar.

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
workspace paths, no orchestrator-internal vocabulary, and the fork-boundary rule stands — keep
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
