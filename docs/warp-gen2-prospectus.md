# WARP ENGINE — generation-two prospectus

*A pricing instrument for one decision: park generation one where it stands, or fund a generation
two. It is paper only. No engine code was written for it, no image was built, no metal row was
taken; every number below is either read off an existing measurement or derived from one, and the
derivation is shown.*

**This document carries no recommendation.** It prices five options and stops. The decision, and
the reasons for it, belong outside this file.

**Its authority.** `docs/WARP-ENGINE.md` §0 is the freeze and remains the authority for everything
it covers. This document adds nothing to it, scores nothing in it, and moves no number in it.
Where the two disagree, WARP-ENGINE.md wins.

**Confidence convention**, the same three tags WARP-ENGINE.md uses, and the tag is part of the
claim:

| tag | means |
|---|---|
| **measured** | off metal, off a capture, off a disassembly — with the file it came from |
| **derived** | arithmetic over measured numbers, with the arithmetic shown |
| **assumed** | neither. Every `assumed` below names what would settle it |

## The evidence base

| source | what it supplies |
|---|---|
| the R5 metal session (2026-08-24, evidence held locally) | `R`, the armed and disarmed rows, the six-variant ablation matrix, six boot windows, the armed profiler dump with all 69 counters |
| `docs/WARP-ENGINE.md` §0.12.3, §0.12.7, §0.14.8 | the corpus method and the band arithmetic, both validated against rows they were not fitted to |
| `docs/WARP-ENGINE.md` §0.14.3, §0.14.8 | tranche 2's accept set, and the closing statement that names the write-back exclusion as the wall |
| `docs/WARP-ENGINE.md` §1.5, §0.4.2 | the ~50 % removable base (48.68 – 49.91 %, one capture) and the Amdahl form built on it |
| `docs/WARP-ENGINE.md` §2a, §2e, §2f, §2g | block boundaries, the bail contract and the write-back exclusion, the invalidation choke point, the cadence contract |
| `docs/warp-corpus.py --prospectus` | the exclusion histogram, the candidate ladder, the chain measurement, the block-model calibration |
| `Z3660_emu/src/uae/jit/compemu_arm.h`, `compemu_support_arm.cpp` | the in-tree classic JIT's `dependency`/`deplist` block-linking machinery |
| the R4 corpus disassembly (evidence held locally) | the corpus disassembly — the AMIX dhrystone binary every WARP row has been measured on (`sum -r 52076`, 8 135 bytes) |

---

# 1. G1 — the baseline, and it is not modelled

**Every row in this section is `measured`, off one session, on one card, against a
same-session clean-interpreter reference.** Nothing here is derived. It is the anchor the four
priced scenarios are quoted against, and it is presented first so that a projection can never be
mistaken for it.

**Session:** 2026-08-24 (evidence held locally), preset 7, `amix_ram 128`,
`service_cadence 4`, installed 040 root, card as found at `F294E75A` and returned to it.
Benchmark identity verified on the box before the first row: `sum -r 52076`, 8 135 bytes — the
same binary as every prior WARP row.

## 1.1 The wall-clock rows

| row | image | n | median | vs `R` |
|---|---|---|---|---|
| **`R`** clean-interpreter reference | `F294E75A` | 7 | **8 185** (min 8 174, max 8 208, spread **0.42 %**) | — |
| **A0 disarmed** | `7A5EC595` | 5 | **7 903** | **0.96555× = −3.445 %** |
| **A0 armed** | `7A5EC595` | 5 | **7 563** | **0.92401× = −7.599 %** |
| A0 disarmed again, same boot | `7A5EC595` | 3 | 7 897 | 0.96482× — the switch is repeatable in-boot |

`R` = 8 185 exactly, so this session's kill threshold `R × (1 + s)` with `s` = 0.70 % is **8 242**
— numerically identical to §0.5.2's user-agreed absolute floor. Both are quoted; neither was
approached from above.

**Boot windows, MMU-enable → first guest TCP accept:** `R` 45 s, A0 48 s, A1 48 s, A2 49 s,
A3 47 s, A4 46 s, A5 46 s. All inside §0.14.8's pre-registered 45 … 50 s. **The boot rows order
themselves the same way the dhrystone rows do**, which matters in §6: the dormant tax reproduces
on a second, supervisor-heavy workload.

## 1.2 The counters

From `R5-armed-profd.txt` (profiling build `6973E8B5`, `PROFZ`-fenced, 100 000 pinned runs — the
standing rule is that counters come from the profiling build and wall clock from the lean build).

```
   INSNS 55 398 698   WARP_ENTER 4 454 413   WARP_INSNS 12 945 947   WARP_BLK 13 661 120
   WARP_COMPILE 41 776   WARP_DECLINE 24 555   WARP_CINSNS 42 517   WARP_BAIL 9 207 252
   WARP_DISC_EV 5 773   WARP_BAIL_STUB 9 205 768   WARP_BAIL_MEM 1 484   BAIL_FENCE 0
   I_true = (INSNS - WARP_ENTER) + WARP_INSNS = 63 890 232
```

| quantity | measured | §0.14.8 pre-registered | |
|---|---|---|---|
| **coverage `c`** | **20.263 %** | 0.20 – 0.28, point 0.229 | in band, low |
| **`WARP_INSNS`/`WARP_BLK`** | **0.9476** | 0.92 – 1.32, point 1.088 | in band, low |
| **compiled length `L`** = `CINSNS`/(`COMPILE`−`DECLINE`) | **2.4689** | 2.30 – 3.10, point 2.651 | in band |
| **decline rate** | **58.778 %** | 51 – 62 %, point 56.8 % | in band |
| **bail rate** `BAIL`/`BLK` | **67.397 %** | 58 – 70 %, point 64.3 % | in band |
| **`DISC_EV` per M retired** | **90.36** — one every **11 067** retired instructions | 75 – 110, point 90.6 | in band, 0.3 % off the point |
| **`BAIL_STUB`/`BAIL`** | **0.99984** | 0.80 – 0.97, point 0.88 | **missed, high** |
| **`BAIL_MEM`/`BAIL`** | **0.000161** | 0.03 – 0.20, point 0.12 | **missed, low** |
| **`BAIL_FENCE`/`BAIL_MEM`** | **0.000** | ≤ 0.10, point 0.02 | in band |

> ### The one row a generation-two decision must not skip: **the entry cost is measured, and it is the largest single term in every projection below.** `E ≈ 112` ARM instructions per block entry (§0.13.2 G-R4.8, read off the lean objdump), against a compiled block length of **2.4689** guest instructions. That is **45.4 ARM instructions of entry per compiled guest instruction**, and §0.5.3's arithmetic says amortisation only starts paying above `L` ≈ 8.

> **The bail split's two missed rows are a finding, and they belong in the pricing.** 99.984 % of
> bails are **cached decline stubs** — the engine re-meeting a head it already refused — and only
> 1 484 of 9.2 million are real blocks bailing on a memory helper. **Round 4's named suspect is
> refuted at three digits.** For generation two this says the bail path is not where the cost is;
> the *decline* path is, and a decline is exactly what a wider accept set removes.

## 1.3 What G1 establishes, and what it does not

**Establishes:** the engine is correct (0 divergences over 10⁸ retired guest instructions,
110 208 differential pairs over 18 368 accepted opcodes, 22 of 22 mutation arms caught,
`WARPTEST` 17/17 on target); it engages (20.263 % coverage); and it costs 7.6 % of dhrystone
armed and 3.4 % disarmed.

**Does not establish:** a verdict. Coverage has been below the 60 % bar for three consecutive
waves, so the kill line's precondition is unmet, the kill line is unfired, and no row taken so far
prices the engine. That is §0.14.9 item 1, unchanged, and it is why this document exists.

---

# 2. The method, and its one calibration

## 2.1 The band arithmetic

Every projection uses §0.12.7's Step-2 form, re-calibrated here on R5's own row exactly as
§0.14.8 re-calibrated it on R4's:

```
   T  = 214.2 cy per guest instruction at R                      (measured, S0.9.3)
   T' = T - removal + entry + gate
   entry   = c x (E / L) x 2.17          E = 112 ARM insns/block entry (measured, G-R4.8)
   gate    = (1 - c) x G_eff             the armed non-target gate, per dispatch
   removal = rho x c x T                 rho = 19.18 % .. 40.34 %   (S0.5.3 step C)
   speedup = T / T'
```

**The calibration, done once, on the row that exists:**

```
   R5 measured 0.92401x  ->  T' = 214.2/0.92401 = 231.82 cy   (net +17.62 cy per guest insn)
   entry(R5) = 0.20263 x (112/2.4689) x 2.17 = 19.95 cy
   removal(R5) = 19.18%..40.34% x 0.20263 x 214.2 = 8.32 .. 17.51 cy
   -> the gate term must be 17.62 - 19.95 + (8.32..17.51) = 5.99 .. 15.18 cy
   -> G_eff = 10.59 cy    (RE-CALIBRATED on R5; R4's own re-calibration read 10.4)
```

> **The re-calibration moved the gate term by 1.8 %.** That is the strongest single reason to
> keep using this arithmetic: calibrated on R3 it produced a band containing R4's row, calibrated
> on R4 it produced a band containing R5's row, and the calibration constant itself has now
> barely moved across two rounds. **(derived)**

> **One convention is inherited rather than re-derived, and it is declared.** §0.14.8 took the
> midpoint of the gate-term *range* as `G_eff` and then charged `(1−c) × G_eff`, which
> double-counts the `(1−c)` weighting once. It is kept identical here so the projections are
> comparable with the pre-registered bands; the effect is a small optimism in the gate term
> (≤ 2 cy at these coverages), and this sentence is the record of it.

## 2.2 The block model, and the one thing that makes it usable

§0.12.3's corpus model counts **maximal runs of accepted instructions** and ignores block
boundaries. It reproduced R3's block length to three digits and then drifted: 1.583 against a
measured 1.581, then 2.763 against 2.4208, then 3.026 against 2.4689 — **+0.1 %, +14.1 %,
+22.6 %.** A model whose error grows monotonically is not the one to extrapolate with.

`--prospectus` applies §2a's **actual** block rule instead — a block ends *after* any control
transfer, *before* any declined instruction, and at a guest page boundary:

| gen | model `L_blk` (callees) | metal `CINSNS`/(`COMPILE`−`DECLINE`) | metal / model |
|---|---|---|---|
| 3 | 1.434 | 1.5810 | **1.1025** |
| 4 | 2.188 | 2.4208 | **1.1067** |
| 5 | 2.255 | 2.4689 | **1.0949** |

> ### **The block-boundary model reads 8.7 – 9.6 % low against three consecutive metal rows, and the offset is stable to 1.1 %.** A calibration factor of **1.10** turns it into a predictor. That is the instrument every block length below is derived with, and it is a strictly better one than the model §0.12.3 used — which was exact once and has been drifting since. **(measured / derived)**

**Why it reads low, stated rather than papered over:** the corpus is dhrystone's user text only.
`libc` is absent (the binary is dynamically linked, so `strcpy`/`strcmp` — which dhrystone leans
on — are in no tier here), and the executed mix weights the longer callee blocks more heavily than
a static count does. Both push the same way. **The direction is known; the magnitude is what the
1.10 absorbs.** `assumed` that it stays 1.10 across a much wider accept set — the widest single
assumption in this document, and §3.5 says what it would cost if it is wrong.

## 2.3 The pricing law, applied throughout

§30's law, quoted so it governs rather than decorates: *work added in front of the path arrives —
nothing overlaps a dependency you put in front.* So every removed cost below is classified
**chained** (its result is what the next step waits on; removal returns in full) or **hidden**
(it overlaps something else; removal returns a fraction), and `ρ` = 19.18 – 40.34 % is precisely
the campaign's own per-bucket return-fraction envelope carried forward.

**Entry amortisation is on the chained side.** The block-entry sequence is a dependency in front
of the block's first instruction: nothing in the block can start until the lookup resolves. That
is why `E/L` enters at the full 2.17 cy per ARM instruction, and it is why chaining — which
removes the dependency rather than shortening it — is the one lever in this document with a
mechanism behind it rather than a coefficient.

---

# 3. G2a — the write-back contract

**What it is:** lift §2e's write-back exclusion. Compile instructions whose fault semantics are
write-back rather than restart — memory-destination `MOVE` forms, and the `LINK`/`UNLK`/`RTS`/`JSR`
frame quartet.

## 3.1 The family list is read off the histogram, not chosen

`docs/warp-corpus.py --prospectus`, gen-5 exclusion histogram. **The two tiers disagree
violently, and the disagreement is the finding:**

| refused family | whole `.text` (632) | **callees only (240)** |
|---|---|---|
| `PEA <abs.L>` | **102 — 16.14 %, the #1 blocker** | **3 — 1.25 %** |
| `JSR` | 97 — 15.35 % (**70 of them `jsr (A2)`, indirect**) | **7 — 2.92 % (6 absolute, 1 PC-relative, 0 indirect)** |
| `MOVE.L` with a refused EA | 76 — 12.03 % | **22 — 9.17 %** (15 of them memory-destination) |
| `MOVE.L (An)+,(An)+` | 22 — 3.48 % | **22 — 9.17 %** |
| `LINK.W` | 12 | **11 — 4.58 %** |
| `UNLK` | 12 | **11 — 4.58 %** |
| `RTS` | 13 | **11 — 4.58 %** |
| `MOVEM` | 6 | **4 — 1.67 %** |
| `LEA` indexed | 7 | 6 — 2.50 % |

> ### **`PEA` is not a generation-two family, and the data is what says so.** It is the largest single refusal in the image and **three instructions** on the tier that executes: **99 of its 102 occurrences are in `main`'s printf banner**, which runs once per benchmark. **Choosing families off whole-`.text` would have funded `PEA` first and bought almost nothing.** §0.12.3's tier split exists for exactly this, and this is the first time it has changed an answer.

> ### **`JSR` sits sixth on the executed tier's refusal histogram, and on that tier every one of its targets is statically known** — six absolute, one PC-relative, zero register-indirect. The 70 indirect `jsr (A2)` calls are `main`'s banner again. **That fact is worth more to §4 than to this section:** a statically-targeted call is a chainable one.

**`MOVEM` is in the top ten and is priced separately** (§3.6) rather than folded in: it is *N*
faultable sites in one instruction, which is a different contract from the rest of the list.

**So G2a's family list, derived:**

| | family | callees-tier gain | the contract it needs |
|---|---|---|---|
| **W1a** | `MOVE`/`MOVEA` with a **memory destination** — `(An)`, `(An)+`, `−(An)`, `d16(An)` — from any source generation one already reads | **+15** | restartable write-back |
| **W1b** | `MOVE.L (An)+,(An)+` — the struct-copy shape | **+22** | restartable write-back **and** per-shape aliasing |
| **W2** | `LINK` / `UNLK` / `RTS` / `JSR` (+ `BSR`, the same contract, 0 occurrences here) | **+40** (11/11/11/7) | a stack write inside a control transfer |

```
   callees tier, 240 instructions
   gen 5   115 accepted  47.92 %
   +W1     152 accepted  63.33 %      (+37 = 15 + 22)
   +W2     155 accepted  64.58 %      (+40)
   +W1+W2  192 accepted  80.00 %      (+77, exactly additive)
```

**77 of the 125 remaining refusals — 61.6 % of everything the engine still declines on the
executed tier — are in these two families.** Mode 7 as a *destination* stays out: that is
§0.5.1 item 4's `(xxx).L` `MOVE`, parked for M4, and admitting it here would quietly price a
family the freeze parked.

## 3.2 The fault-contract engineering, stated per family class

**This is the section that decides whether G2a is a tranche or a project.** §2e's exclusion is not
a scoping preference; it is the reason the bail contract is checkable statically.

**Class 1 — a restartable memory-destination store.** Rung 0's own note is the model:
`op_20c8_31_ff` increments the destination register, sets the flags, advances the PC, clears
`mmu_restart`, disarms `mmufixup` — **and only then stores.** A fault at that store is a 68040
write-back fault on an instruction that is *architecturally complete*, and the guest kernel's
`wb040.s` replays it. Today the engine's whole bail contract is *"a state at a bail boundary is a
state the interpreted path itself passes through"*, and the pre-store state of a write-back
instruction is not one of those states — the interpreter is already past it.

*What it needs:* the block's exit sequence gains a **per-site write-back shape** rather than a
per-site PC. A block bailing at a memory destination must reproduce the interpreter's *post*-state
— register advanced, flags set, PC advanced, `mmu_restart` clear — and then leave, so the
interpreter's own fault machinery sees exactly what it would have seen. Every existing exit site
carries a PC; these carry a PC **and a shape**, and the mutation arm that proves it is
`M-AUTO-ORDER`'s existing form pointed at the destination instead of the source.

**Class 2 — `(An)+` destination aliasing the source.** `MOVE.L (A0)+,(A0)` computes its
destination EA from the **incremented** `A0`. §0.12.2 refused the undo-on-bail shortcut for
exactly this reason, and W1b's 22 instructions are the shape that makes it live: source and
destination are both auto-adjusting, both faultable, and the register between them has already
moved. *What it needs:* a per-shape aliasing analysis at **compile** time — same base register on
both sides is a distinct emitted shape, not a runtime test — plus a differential arm that seeds
the aliasing case, because `warp_seed()` will not produce it by accident. §0.13.5's rule applies
in full: an arm that cannot be caught reports CAUGHT-shaped silence.

**Class 3 — a stack write inside a control transfer.** `JSR`/`BSR` push a return address *and*
transfer; `RTS` pops *and* transfers. That is **two faultable sites and a block boundary in one
instruction**, and the block boundary is on the wrong side of the store. `LINK` is a push plus two
register updates; `UNLK` is a load plus two. *What it needs:* the exit sequence must be able to
leave from *between* the two halves with the interpreter's own intermediate state — which is a
strictly stronger requirement than class 1, because the PC that the bail writes back is not the
PC the instruction started at.

> ### **These are three different contracts, not one, and the honest reading is that G2a is three pre-registrations rather than a family list.** §0.14.8 said so before this document existed: *"None of it is scoped here, and none of it is promised."*

## 3.3 The projection

**Coverage.** Two independent derivations, and the band is their union.

```
   METHOD A -- S0.12.3's own, productive entries x retired per entry
     productive-entry fraction:  R3 0.8633, R4 0.7204, R5 0.6804  of static accept
        the ratio is FALLING as the accept set widens (-16.6 %, then -5.6 %)
        G2a: 0.60 .. 0.70   (assumed, direction: down)      static accept 0.8000
     retired per productive entry: R5 measured 2.9063
        scaled by the calibrated block-length ratio 3.35/2.48 = 1.35   (see below)
        G2a: 2.9063 x (1.20 .. 1.35) = 3.49 .. 3.92         (assumed band on the scaling)
     WARP_BLK held at R5's 13 661 120 -- R3->R4 +3.3 %, R4->R5 +1.6 %; charged 1.00 (assumed)
     WARP_INSNS = 13 661 120 x (0.480 .. 0.560) x (3.49 .. 3.92) = 22.9 .. 30.0 M
     c = / I_true 63 890 232  =  35.8 % .. 47.0 %

   METHOD B -- the measured static-to-coverage ratio, cross-check
     R3 0.3167 static -> 0.0843 measured   beta_eff 0.266
     R4 0.4375 static -> 0.1910 measured   beta_eff 0.437
     R5 0.4792 static -> 0.2026 measured   beta_eff 0.423
     G2a 0.8000 static x beta_eff (0.42 .. 0.50)  =  33.6 % .. 40.0 %
```

> **`coverage c` = 0.32 – 0.47, point 0.38. (derived, on three assumed factors)** The band is the
> union of the two methods, widened at the bottom because method B's `β_eff` has been flat for
> exactly two rounds and method A's ratio is still falling.

**Block length.** Calibrated model, callees tier: `L_blk` 3.048 × 1.10 = **3.35**. Banded
**2.9 – 3.6** for the tier disagreement — the whole-`.text` tier reads 1.937 against the callees'
3.048, a 57 % gap, because W2 chops `main`'s long banner runs into fragments while lengthening the
callees'. §0.14.3's rule is followed: *the gap between the tiers is the measure of how far to
trust it*, and the band widens rather than the point moving.

**The band, at the MEASURED `E` = 112, with no chaining:**

```
   G2a LOW    c = 0.32, L = 2.90, E = 112, G_eff = 10.59
      entry   0.32 x (112/2.90) x 2.17     = 26.82 cy
      gate    0.68 x 10.59                 =  7.20 cy
      removal 19.18 % x 0.32 x 214.2       = 13.15 cy
      T' = 214.2 - 13.15 + 34.02 = 235.07  ->  0.9112x
   G2a HIGH   c = 0.47, L = 3.60
      entry   0.47 x (112/3.60) x 2.17     = 31.73 cy
      gate    0.53 x 10.59                 =  5.61 cy
      removal 40.34 % x 0.47 x 214.2       = 40.61 cy
      T' = 214.2 - 40.61 + 37.34 = 210.93  ->  1.0155x
```

> ### **G2a armed band: 0.911× – 1.016× of `R`. Against `R` = 8 185 that is dhry ≈ 7 458 – 8 312.** The floor is **below** G1's measured 7 563; the ceiling crosses parity by 1.6 %. **G2a does not reliably beat the interpreter it replaces**, and the reason is arithmetic rather than pessimism: the entry cost *rises* with coverage faster than the removals do until `L` gets much larger than 3.6.

**§1.5 Amdahl cross-check.** `t = 0.50 × c × r`, with `r = 2ρ` (§0.5.3's `ρ` is already
half of §1.5's `r`):

```
   low  t = 0.50 x 0.32 x 0.384 = 0.0614  ->  1.065x   dhry 8 720
   high t = 0.50 x 0.47 x 0.807 = 0.1896  ->  1.234x   dhry 10 100
```

> **The Amdahl form is a removal-only ceiling and the gap to the model band IS the cost side.**
> §1.5 said it in advance: *"The cost side is unquantified and it is subtracted from this band, not
> added to it."* At G2a the gap is **1 262 – 1 788 dhrystones**, and **78.8 – 85.0 % of it is the
> block-entry term** (the gap is `entry + gate` exactly — the removal term is identical on both
> sides — so the split is arithmetic, not attribution). **This is the single most important
> cross-check in the document**: it says the engine's problem is not what it fails to remove, it is
> what it adds in front.

## 3.4 Engineering estimate

Yardstick, measured off this repository's own log and evidence directories: a round is one
pre-registration commit, one build round, and zero-or-one metal session. Round 4 pre-registered at
07:30 and scored at 09:33; round 5 pre-registered at 11:05 and scored at 11:46; the R5 metal
session flew **eight** images between 11:55 and 13:39. **One tranche ≈ one build round + one metal
session.**

| round | what it is | metal? |
|---|---|---|
| **R6** | the restartable write-back contract itself — exit sequence gains a per-site shape, new mutation arms, a fault-injection corpus that bails *at* a destination | no |
| **R7** | W1a — memory-destination `MOVE` from register and `(An)`/`d16(An)` sources | yes |
| **R8** | W1b — the `(An)+`→`(An)+` aliasing shape, with its own compile-time discrimination | yes |
| **R9** | W2a — `LINK`/`UNLK` | no |
| **R10** | W2b — `RTS`/`JSR`/`BSR`, the stack-write-inside-a-transfer contract | yes |

> ### **G2a ≈ 5 rounds, 3 metal sessions.** Plus the dormant-tax fix (§6), which every continue scenario needs to ship.

## 3.5 Top three risks

1. **The write-back contract is where a silent wrong answer lives.** §5 R1's taxonomy is explicit:
   a missed invalidation gives wrong results silently, wild control flow, wild writes into the
   kernel's buffer cache. A write-back bail that reproduces the *pre*-state instead of the
   post-state is exactly that class of defect, it will not show on dhrystone, and M2 (demand
   paging) is the first workload that exercises it at all. **Mitigation:** the differential and
   the mutation arms are the instrument, and `M-AUTO-ORDER` already exists in the shape that
   catches the ordering half.
2. **The calibration factor 1.10 is assumed to hold across a much wider accept set than it was
   measured on.** Its three rows sit at static accept rates of **31.7 %, 43.8 % and 47.9 %**; G2a
   is **80.0 %**, well outside that span, and the two effects the offset absorbs — absent `libc`
   and execution weighting — need not scale together. If block length lands at 2.5 instead of
   3.35, the entry term rises 34 % and the band's ceiling drops below parity. **This is the
   softest input in G2a.**
3. **`WARP_BLK` held constant.** Block *entries* are driven by the workload's control-transfer
   arrivals, which the engine does not change — but W2 compiles the control transfers themselves,
   and a compiled `RTS` changes what a "block head arrival" is. Charged 1.00 with no evidence for
   the direction.

## 3.6 What G2a does not establish

- **That the memory-destination families are correct on silicon.** They emit a helper call, and
  `z3660_warp_selftest()` refuses any block containing a `blx` (§0.11.1). They join round 4's
  memory-source ALU and auto-adjust sources as harness-proven only. §0.14.4 specifies the
  helper-substitution arm that would change this; it is still unbuilt.
- **Anything about the supervisor share.** Generation one is user-mode only and G2a does not
  change that.
- **`PEA` and `MOVEM`, priced and not included.** On the callees tier they are worth +1.25 pp and
  +1.67 pp of accept and take `L_blk` from 3.048 to 3.431 — about **+0.4 in block length for two
  more contracts**, one of which (`MOVEM`) is *N* faultable sites in one instruction. They are
  named here so that adding them later cannot look like a discovery.
- **A verdict.** G2a's projected coverage of 0.32 – 0.47 is still under the 60 % bar.

---

# 4. G2b — G2a plus block chaining

**What it is:** a taken branch whose target is a compiled block jumps straight into it, without
returning to the dispatch loop. The entry cost is then paid once per *chain*, not once per block.

## 4.1 The chain length is measured, not assumed

A chain ends at exactly three things: an **uncompiled opcode**, an **indirect or computed
branch** (its target is a runtime value the compiler never saw), or a **chain-invalidation
boundary**. `--prospectus` measures the first two on the corpus; the third is measured on metal
and turns out not to bind.

**The control-transfer census, callees tier — this is why chains are short:**

```
   40 control transfers in 240 instructions (16.67 %)
     indirect : RTS 11, JMP d8(PC,Xn) 1                       <- 12 chain TERMINATORS
     static   : JSR 7 (6 absolute + 1 PC-relative), Bcc 15, BRA 6   <- 28 chain LINKS
```

**Not one register-indirect `JSR` on this tier.** Every call dhrystone's hot code makes has a
target the compiler can see. The 70 `jsr (A2)` calls that dominate the whole-`.text` census are
`main`'s banner, and they are the shape that would *not* chain.

**Run lengths between chain boundaries, under the G2a accept set:**

| tier | `L_blk` | `L_chain` | **blocks per chain** |
|---|---|---|---|
| whole `.text` | 1.937 | 2.377 | **1.23** |
| hot set | 1.964 | 2.352 | **1.20** |
| **callees only** | **3.048** | **4.571** | **1.50** |
| callees, with a return-address prediction stack (`RTS` links) | 3.048 | 6.000 | **1.97** |

> ### **Chaining buys 1.50 – 1.97 blocks per chain on this workload, not 5 and not 10.** The ceiling is set by the *accept rate*, not by the chaining machinery: a chain breaks at every declined instruction, and at G2a's 80 % accept the mean distance between declines is short. Dhrystone's callees are small functions that end in `RTS`, and `RTS` is the second terminator. **(derived, from a linear-text run-length measurement over the corpus)**

**The honest limit of the measurement, stated rather than footnoted.** This is a *linear-text*
run length. A chain follows control flow; the model follows addresses. For a taken forward branch
into nearby code the two agree; for a backward branch closing a loop the model under-counts (the
loop re-enters a chain it has already traversed); for a call to a distant callee it over-counts
(the model continues into the caller's next instruction rather than into the callee). §0.12.3's
own model was a linear-text run length and reproduced a metal block length to three digits, which
is why the method is used again — but that validation was for *blocks*, not for chains, and
**there is no metal row anywhere that validates a chain length.** `derived`, one tag weaker than
everything in §3.

**The invalidation boundary does not bind, and this is measured.** `WARP_DISC_EV` = 90.36 per
million retired instructions — **one discard event every 11 067 retired guest instructions**
against a chain of ~5. A chain is essentially never torn down mid-flight. What the discard rate
*does* cost is re-linking: `WARP_DISC` 41 642 blocks discarded against `WARP_COMPILE` 41 776
compiles means **the code cache is thrown away and rebuilt at almost exactly 1:1**, so every
chain link built is a chain link discarded. **The link-building work scales with compiles, not
with executions**, which is the opposite of what chaining's payoff scales with.

## 4.2 The engineering, named

**The machinery exists in the tree and is the right shape.** `Z3660_emu/src/uae/jit/compemu_arm.h`
already carries, per block:

```c
   dependency  dep[2];     /* the two successors of a conditional branch */
   dependency* deplist;    /* the BACK-links: everything chained INTO this block */
```

and `compemu_support_arm.cpp` carries the three operations that make them safe: `create_jmpdep()`
threads a source block's patch site onto the target's `deplist`; `set_dhtu()` walks a block's
`deplist` and rewrites **every inbound chained jump** when its handler changes; `invalidate_block()`
calls `set_dhtu(bi, bi->direct_pen)` to repoint every inbound link at a pending stub and then
`remove_deps()` to drop its own outbound ones.

> ### **That is a complete answer to "a discarded block that others chain into", already written, already debugged, and it is about sixty lines.** What does **not** transfer is the same thing §0.3(c) found for `cache_tags`: `create_jmpdep()` resolves its target through `get_blockinfo_addr((void*)target)`, keyed on `pc_p` — **a host pointer, and under the MMU there is no `pc_p`.** The *structure* is reusable verbatim; the *keying* is rebuilt on the guest logical PC with the supervisor bit, exactly as §2a rebuilt the block table.

**The cadence contract is the hard part, and Q10 is frozen.** §2g's rule is not a performance
choice: *a block must feed the cadence counter with the number of guest instructions it retired,
and must poll at its boundaries.* One A3000-SCSI pump tick is `256 × service_cadence` **guest**
instructions; a chain that runs five blocks without polling has silently changed the emulator's
timebase, and the symptom is a missed interrupt, not a slow number.

*How it holds:* the poll moves from a block boundary to a **chain-internal accumulator**. Each
chained block adds its retired count to a register-resident total and tests it against the
remaining budget; the test is the chain link's own condition, so the only added work on the taken
path is an add and a compare — and the chain exits to the dispatch loop when the budget is spent,
which is the *same* exit the un-chained engine takes every block. The ~42 µs uninterruptible
budget (§2g, rung 0's own `blk_chunk` = 256 iterations ≈ 42 µs at 1.1 GHz, chosen to sit inside
one 15.6 kHz hsync) becomes the chain's bound rather than the block's. **`WARP_ENTER`'s topology
check must survive this unchanged**: §0.5.2 pre-registered that `WARP_ENTER` increments **only** at
the run-loop entry and **never** on a chained transfer, or coverage reads high. That check was
written before chaining existed and is exactly what makes chaining measurable.

## 4.3 The projection

`L_eff` = `L_blk` × blocks-per-chain: low 2.90 × 1.50 = **4.35**, high 3.60 × 2.00 = **7.20**.
Coverage is unchanged — chaining does not change *which* instructions compile.

```
   G2b LOW    c = 0.32, L_eff = 4.35
      entry   0.32 x (112/4.35) x 2.17     = 17.88 cy
      gate    0.68 x 10.59                 =  7.20 cy
      removal 19.18 % x 0.32 x 214.2       = 13.15 cy
      T' = 214.2 - 13.15 + 25.08 = 226.13  ->  0.9472x
   G2b HIGH   c = 0.47, L_eff = 7.20
      entry   0.47 x (112/7.20) x 2.17     = 15.87 cy
      gate    0.53 x 10.59                 =  5.61 cy
      removal 40.34 % x 0.47 x 214.2       = 40.61 cy
      T' = 214.2 - 40.61 + 21.48 = 195.06  ->  1.0981x
```

> ### **G2b armed band: 0.947× – 1.098× of `R`, i.e. dhry ≈ 7 753 – 8 988.** Chaining moves the floor up 295 dhrystones and the ceiling up 676. **It is the largest single lever in this document per unit of engineering, and it still leaves the floor below parity.**

**§1.5 Amdahl cross-check:** unchanged from G2a — `t` = 0.0614 – 0.1896 → **1.065× – 1.234×**,
dhry 8 720 – 10 100. **Chaining closes 23 – 38 % of the gap between the model band and the Amdahl
ceiling and closes none of the ceiling itself**, which is correct: chaining removes cost, it does
not remove more interpreter work.

> **One upside the model deliberately does not charge, and it is the plan's own central
> mechanism.** §1.6: the interpreter's irreducible cost is a loop-carried dependency chain ending
> in *an indirect branch with roughly 1 867 possible targets on a Cortex-A9 whose BTB holds one
> target per site*, and *nothing overlaps a flush, because the flush is the loss of the overlap.*
> A chained block-to-block jump is a **direct** branch — the mispredict is gone, not shortened. The
> arithmetic above prices only the 112-instruction entry sequence and charges **zero** for the
> mispredict. **The band's ceiling is therefore an underestimate by an unknown amount, and this is
> the one place in the document where the error is knowably one-sided in our favour.** It is left
> uncharged because §30's law forbids converting a named latency into wall clock without measuring
> what was hiding it — and nothing has measured it.

## 4.4 Engineering estimate

| round | what it is | metal? |
|---|---|---|
| **R11** | the link machinery — patchable exit sites, `dep[2]`/`deplist` re-keyed on the guest logical PC + supervisor bit, `set_dhtu` on the discard path | no |
| **R12** | the invalidation interaction — every inbound link unlinked at discard, and the identity that proves it (`links_live == 0` after `warp_discard_all()`) | yes |
| **R13** | the cadence contract inside chains — the chain-internal accumulator, the ~42 µs bound measured on target, `WARP_ENTER`'s topology re-proven | yes |

> ### **G2b ≈ 3 rounds beyond G2a, 2 metal sessions. Cumulative G2a+b ≈ 8 rounds, 5 metal sessions.**

## 4.5 Top three risks

1. **The cadence contract is the one item in the plan with cross-repo reach (§6.2).** Get it
   wrong and the failure mode is *never* "slower" — it is a missed or late interrupt in the
   piscsi service or the A3000-SCSI pump, i.e. a driver-visible fault in `amix-z3660scsi` or
   `amix-z3660net`, surfacing hours later. It is also the one risk here that the dhrystone gate
   cannot see at all.
2. **Chain length is the only load-bearing number in this document with no metal validation.**
   1.50 – 1.97 blocks per chain is a linear-text derivation. If the true figure is 1.2, G2b's
   floor collapses back onto G2a's.
3. **Re-link cost at a 1:1 discard-to-compile ratio.** Every compiled block is discarded roughly
   once per window; every discard walks a `deplist`. The work is proportional to compiles, which
   is the wrong side of the ledger, and it is charged **zero** above.

## 4.6 What G2b does not establish

- **Anything about a superblock or a trace.** Chaining links basic blocks; it does not merge them,
  does not allocate registers across them, and does not remove the per-block flag snapshot.
- **That the ~42 µs bound holds.** It is a pre-registerable claim (§2g), measured on target,
  separately for block execution and compilation — and chaining is precisely what could break it.
- **The mispredict upside.** Named, mechanism given, charged zero, unmeasured.

---

# 5. G2c — G2b plus supervisor coverage

**What it is:** generation one is **user-mode only** — *"a block is not entered with `regs.s`
set"* (§0.5.1). G2c admits supervisor blocks. **This section is modelled coarsely and says so:
it is the weakest-evidenced scenario in the document by a wide margin.**

## 5.1 What the supervisor share actually is

| window | supervisor share of retired instructions | source |
|---|---|---|
| dhrystone, R5 armed | **17.91 %** | `R5-armed-profd.txt` |
| dhrystone, R5 disarmed | **15.80 %** | `R5-disarmed-profd.txt` |
| dhrystone, M0 counters window | 16.09 % | §0.4.2 |
| multiuser boot (M3's own figure) | **48.77 %** | §4 M3 |
| **MMU-enable → first TCP accept** | **70.88 %** | §0.4.2 |
| banked sampler rings | 43.31 % / 54.10 % / 87.91 % | §0.5.3 |

> **The 49 – 70 % figure is a *boot* number and dhrystone is an 18 % number.** Both are true and
> they are not comparable — §0.4.2's own discriminator lesson: *a share swept at one band is not
> comparable with one swept at another.* **The verdict gate is dhrystone.** G2c's dhrystone value
> is therefore bounded by 17.91 %, and its boot value is bounded by 48.77 – 70.88 %.

## 5.2 The one piece of real evidence, and it cuts against G2c

Rung 0's block-idiom fast path recognises three supervisor loop shapes — `bzero`, `mcpy`, and a
`CPUSHL` maintenance loop — and is worth **−43.6 % of an AMIX boot** in a same-boot A/B, at a
measured cost of −1 % on dhrystone, and it collapses the `CPUSHL` invalidation storm 18×.
**(measured, `docs/block-idiom-fastpath.md`)**

> ### **The largest compilable supervisor loops in this workload are ALREADY covered — by rung 0, which the JIT declines by rule (Q8: *"the JIT declines those PCs and does not modify rung 0"*).** G2c's marginal supervisor gain is what rung 0 does not already take, and the only measurement anybody has of how much that is says rung 0 is worth **43.6 % of the boot's wall clock** — which is a *time* share, not an instruction count, and converting one into the other is precisely what §30's law forbids doing without asking what was hiding the work. **This is the single most important input to G2c, it is the wrong unit, and that is what makes the band wide.**

## 5.3 The projection, banded wide and tagged

*What fraction of supervisor execution is loop or straight-line kernel code a block engine could
cover?* There is **no capture that answers this**. What exists: kernel text is compiled C with the
same instruction mix as user text (so the family-level accept rate should be similar, ~80 % under
G2a), minus three deductions with no measurement behind their sizes — traps and system-call entry
(short, frequent, block-ending), interrupt windows, and the MMU-maintenance instructions
themselves (`PFLUSH`, `CPUSHL`, `MOVEC`, `MOVE` to/from `SR`), which §2a already makes block
boundaries — minus rung 0's share, which is measured only as a boot-time speedup and not as an
instruction count.

```
   phi_covered = the fraction of SUPERVISOR retired instructions that end up inside blocks
   band 0.20 .. 0.50, point 0.35     (ASSUMED -- the widest band in this document)
   dhrystone window: supervisor share 0.1791
   delta_c = 0.1791 x (0.20 .. 0.50) = 0.036 .. 0.090
   c(G2c) = 0.32+0.036 .. 0.47+0.090 = 0.356 .. 0.560
```

```
   G2c LOW    c = 0.356, L_eff = 4.35
      entry   0.356 x (112/4.35) x 2.17    = 19.89 cy
      gate    0.644 x 10.59                =  6.82 cy
      removal 19.18 % x 0.356 x 214.2      = 14.63 cy
      T' = 214.2 - 14.63 + 26.71 = 226.28  ->  0.9466x
   G2c HIGH   c = 0.560, L_eff = 7.20
      entry   0.560 x (112/7.20) x 2.17    = 18.90 cy
      gate    0.440 x 10.59                =  4.66 cy
      removal 40.34 % x 0.560 x 214.2      = 48.39 cy
      T' = 214.2 - 48.39 + 23.56 = 189.37  ->  1.1311x
```

> ### **G2c armed band on dhrystone: 0.947× – 1.131×, i.e. dhry ≈ 7 748 – 9 258.** The floor does not move at all from G2b — added coverage costs entry at almost exactly the rate it buys removal at the low `ρ` — and the ceiling gains 270 dhrystones.

**§1.5 Amdahl cross-check:** `t` = 0.50 × (0.356 … 0.560) × (0.384 … 0.807) = 0.0683 – 0.2259 →
**1.073× – 1.292×**, dhry 8 785 – 10 574.

## 5.4 The question the brief asks, answered

*Do user + supervisor together put the original 1.25× – 1.55× band in reach?*

> ### **No — and the arithmetic says why, in one line.** Set the entry cost to **zero** at G2c's own ceiling — a perfect engine that enters blocks for free:
>
> ```
>    T' = 214.2 - 48.39 + 4.66 = 170.47  ->  1.2565x   dhry 10 285
> ```
>
> **§1.5's 1.25× floor requires `ρ` at its ceiling, coverage ≥ 0.56, AND free block entry.** It is
> not reachable with `E` = 112, at any coverage this document prices. §1.5's band was derived at
> `c` = 0.70 – 0.90 and charged **nothing** for entry, and it said so at the time: *"The cost side
> is unquantified and it is subtracted from this band, not added to it."* Three metal rounds have
> now quantified it. **1.25× – 1.55× is a statement about a different engine — one with a
> single-digit entry cost — and the fastest route to it is not more families, it is `E`.**

**The `E` lever, priced here because the arithmetic keeps pointing at it and nothing else does.**
`z3660_warp_run`'s 33-instruction preamble has been re-owed since §0.9.3 and is *still* the largest
row in its table; the open-coded memory fast path (§0.9.6 item 3) is unbuilt; the four-way hashed
lookup is 18 instructions. **`E` has never had a round of its own.** At G2c-high with `E` = 40
instead of 112, `T'` = 214.2 − 48.39 + 6.75 + 4.66 = 177.22 → **1.2087×, dhry 9 893** — more than
chaining and supervisor coverage bought together.

## 5.5 Engineering estimate

| round | what it is | metal? |
|---|---|---|
| **R14** | supervisor admission — the block key's supervisor bit audited under real kernel PCs (`M-5` is the existing arm), trap and interrupt windows inside kernel blocks, the privileged-instruction block boundary re-proven against the kernel's actual mix | yes (boot row) |
| **R15** | the kernel's own family gaps — `MOVEM` is the kernel's prologue idiom, not dhrystone's | yes |
| **R16** | the fault path under demand paging — **this is M2, and M2 is `GATED(M1)`** | yes |

> ### **G2c ≈ 3 – 4 rounds beyond G2b, 3 metal sessions. Cumulative full G2c ≈ 11 – 12 rounds, 8 metal sessions — and it is gated on M2, which is itself unscoped.**

## 5.6 Top three risks

1. **Supervisor blocks are where a wrong answer becomes a corrupted root filesystem.** §5 R1's
   *"wild writes into the kernel's buffer cache"* is not hypothetical in this repository — a
   page-crossing DMA once clipped an unrelated frame and stayed invisible until it ate an ELF
   header. Every supervisor round runs on a work-copy root, and that is a discipline, not a
   mitigation.
2. **`φ_covered` = 0.20 – 0.50 is the widest assumed band in this document and nothing here
   narrows it.** What would: one profiling boot with a supervisor-PC histogram — cheap, and not
   taken.
3. **Rung 0 already owns the best supervisor loops.** If the JIT is ever allowed to subsume them
   (an M4 candidate with its own gate), the comparison becomes *"a cost measured with this
   recognizer switched off is not a cost the shipping firmware pays"* — the campaign's own rule,
   already paid for once.

## 5.7 What G2c does not establish

- **The boot-window value**, which is where the supervisor share is 48.77 – 70.88 % and where the
  JIT's value is probably largest. The dhrystone band above says nothing about it, and Q11's boot
  row is the instrument that would.
- **That M2 is passable.** Faults and demand paging are `GATED(M1)`, unscoped, and every
  supervisor round runs through them.
- **Anything at all with better than `assumed` confidence on the coverage side.**

---

# 6. The dormant-tax fix — priced as its own line item

**Every continue scenario needs it to ship. The park scenario is improved by it too: a parked
engine that costs nothing can stay in the tree.**

## 6.1 The tax is measured, and for the first time it is attributed

R5's ablation matrix, all six images built from one tree by `-D` flags after `make clean-all`,
flown in the pre-registered order A0, A2, A3, A4, A5, A1:

| variant | CRC | dhry | penalty vs `R` | edge | §0.14.1 predicted |
|---|---|---|---|---|---|
| **A0** shipping lean | `7A5EC595` | 7 903 | **−3.4453 %** | — | the control |
| **A1** − `L-WTST` | `0BAEECE1` | 7 905 | −3.4209 % | **+0.024 pp** | 0.0 – 0.5 → **HIT** |
| **A2** − `L-ATT` | `161412F2` | 7 861 | −3.9585 % | **−0.513 pp** | 0.0 – 0.3 → **MISSED** |
| **A3** − `L-HOOK` | `5E2592EF` | 8 064 | −1.4783 % | **+1.967 pp** | 0.6 – 1.4 → **MISSED, high** |
| **A4** − `L-HOOK` − `L-REG` | `459E51A3` | 8 140 | −0.5498 % | **+0.929 pp** (A3→A4) | 1.8 – 3.4 → **MISSED, low** |
| **A5** − the engine | `BBED0509` | 8 163 | −0.2688 % | **+3.177 pp** | 3.9 – 4.6 → missed low |

**Reading the pre-registered decision rule, in its own order:**

1. **THE FLOOR** = |penalty(A0) − penalty(A2)| = **0.513 pp**, and it came out *negative* —
   removing 64 bytes of cold code made the image measurably **slower**. Nothing smaller than
   0.513 pp may ever be read as a mechanism.
2. **A single edge recovering ≥ half the measured penalty names the mechanism — and there are
   TWO BARS, so both are reported** (WARP-ENGINE.md §0.16.2, whose reading governs).
   `L-HOOK`'s edge is **1.967 pp**, 57.1 % of A0's own 3.4453 pp.
   - **APPLIED bar** — half of A0's **own same-session** penalty = **1.7227 pp**.
     1.967 ≥ 1.7227 → **`L-HOOK` NAMES THE MECHANISM.**
   - **PRE-REGISTERED bar** — §0.14.1 rule 2, byte-untouched, half of round 4's −4.256 % =
     **2.128 pp**. 1.967 < 2.128, **short by 0.161 pp** → the pre-registered rule would have
     read rule 3, *"distributed or emergent"*.

   **The applied reading governs; the pre-registered figure stands beside it, unrewritten,
   because the naming verdict rests on a 0.161 pp margin against a 0.513 pp floor** — and
   saying so is the difference between an attribution and a claim. **§6.2's pricing is
   unaffected either way**: the fix targets `L-HOOK` and `L-REG` as *layers*, both measured
   above the floor, and it does not depend on either one being the mechanism.
3. Not applicable — an edge cleared the bar.
4. **A5's own penalty is −0.269 %, inside the 0.513 pp floor of zero.** → **the four named layers
   SPAN the cost.** Nothing outside them carries it.
5. **Additivity:** (A0−A1) + (A0−A2) + (A0−A3) + (A3−A4) = **2.407 pp** against the closure
   (A0−A5) = **3.177 pp**. |difference| = 0.770 pp against a tolerance of 2 × floor = 1.026 pp →
   **inside tolerance. The layers do not measurably interact.**

> ### **THE PRE-REGISTERED FAVOURITE IS REFUTED, IN PUBLIC, AS INTENDED.** §0.14.1 named `L-REG` — the `struct regstruct` displacement, 7 910 changed function bodies, 31.5 % of the image — and predicted 1.8 – 3.4 pp. It measured **0.929 pp**, below its own band, and **`L-HOOK` — four ARM instructions on the disarmed dispatch path, predicted at 0.6 – 1.4 pp — measured 1.967 pp instead.** The hot path won and the layout lost. Third mechanism named for this residual; the first one an instrument found rather than a disassembly suggested.

**The static noise floor agrees with the dynamic one.** `warp-objdiff.py` over four completely
different edges — a cold-code removal, a hot-path removal, two unrelated single layers, and an
entire round of engine work — reports 457, 459, 462 and 457 changed function bodies. **Under
`-Os -flto`, any change to this image re-decides inlining in about 460 bodies.** The layout edge
(A3 vs A4) stands at 7 910, **17× above that ambient churn**; nothing else does. **The two floors
are independent and they say the same thing: ~0.5 pp dynamic, ~460 bodies static.**

## 6.2 The fix, and what it must be designed against

| layer | carries | fix |
|---|---|---|
| **`L-HOOK`** | **1.967 pp (57.1 %)** | **Two run loops, selected once at arm/disarm rather than tested per dispatch.** `m68k_run_mmu040` and `m68k_run_mmu040_warp` are built as separate functions; the disarmed image runs a loop with **no gate at all**, not a cheaper one. The arm/disarm verb switches which one the dispatcher calls, which it already does once per verb rather than once per instruction |
| **`L-REG`** | 0.929 pp (27.0 %) | Move `warp_on` to the **end** of `struct regstruct`, or out of it entirely, so no existing member's displacement moves. Recovers the layout term for **both** the disarmed and the armed rows — it is a whole-image cost that arming does not remove |
| `L-WTST` | 0.024 pp | nothing to do. Inside the floor |
| `L-ATT` | inside the floor (measured negative) | nothing to do, and **nothing may be claimed** |
| residual | 0.269 pp | inside the floor of zero |

**Target: `L-HOOK` + `L-REG` = 1.967 + 0.929 = 2.896 pp of the measured 3.445 pp**, leaving
**0.549 pp — which is A4's own measured penalty, to three digits, because A4 is exactly that
image.** A4 sits 0.281 pp from A5, i.e. **inside the 0.513 pp floor of a dormant engine that costs
nothing at all.**

> ### **THE METHOD NOTE, AND IT IS BINDING: this fix cannot be designed one layer at a time.** The floor is **0.513 pp**. `L-REG` alone is 0.929 pp — measurable, but only 1.8× the floor, which is the resolution at which this campaign has already been wrong twice. **The fix must target the full ~2.9 pp**, and its verification needs either a whole-percentage-point effect or explicit layout control. The kit that measures it **already exists** — A0′(fixed) against A5 must land inside the floor, with A2's edge re-measured in the same session to re-establish what the floor is for that build.

## 6.3 What it is worth, per scenario

- **Park:** the disarmed image is the shipping image, and **the ablation matrix has already
  measured what the fixed one scores.** `A4` *is* an image with `L-HOOK` and `L-REG` both removed:
  **8 140, −0.550 %**, with `A5`'s **8 163, −0.269 %** as the ceiling. **237 – 260 dhrystones
  returned, and the ≥ 7 988 blessing class cleared by 152 – 175 dhrystones** after three
  consecutive rounds of missing it. This is the rare estimate that is `measured` rather than
  `derived` — the only thing the fix has to reproduce is a row that already exists.
  *(The one thing `A4` does not model: a fixed `A0′` keeps the ability to arm, so it carries the
  second run loop's link footprint — an `L-WTST`-class cost, measured at 0.024 pp.)*
- **Every continue scenario:** `L-REG`'s 0.929 pp is a layout cost the **armed** row pays too. Every
  band in §§3–5 may be read **+0.9 pp** with the fix landed. It is not folded into the bands
  above, which keeps them conservative.

## 6.4 Estimate, risks, and what it does not establish

> ### **The dormant-tax fix ≈ 1 round, 1 metal session** (three variants: A0′, A5, and A2 for the floor). The instrument is already built, tracked and CRC-archived.

**Risks:** (1) two run loops is code duplication in the one function this campaign has already
mis-built once — §0.15.2's finding was a stale object producing an image its own source does not
build, and `tools/warp-gate.sh` exists because of it; the fix must be gate-verified off the ELF,
not off the image. (2) `L-HOOK`'s 1.967 pp is one edge on one build of one toolchain at
`-Os -flto`; §0.14.9 item 5 already says a layout result is not a portable one. (3) The armed path
is restructured by the same change and is not what the round measures.

**Does not establish:** anything about the armed row, which is a different image and a different
measurement; and nothing about whether the residual mechanism *generalises* — `L-HOOK` names a
layer, and §0.14.9 item 2 is explicit that a layer is not yet a mechanism.

---

# 7. All scenarios, and the Amdahl cross-check beside each

| scenario | coverage `c` | `L_blk` | blocks/chain | `L_eff` | **armed band** | **dhry vs `R` = 8 185** | §1.5 Amdahl ceiling | the gap = the cost side |
|---|---|---|---|---|---|---|---|---|
| **G1** measured | **0.2026** | **2.469** | 1.00 | 2.469 | **0.9240×** | **7 563** | 1.040× – 1.089× (8 516 – 8 914) | −953 … −1 351 |
| **G2a** | 0.32 – 0.47 | 2.9 – 3.6 | 1.00 | 2.9 – 3.6 | **0.911× – 1.016×** | **7 458 – 8 312** | 1.065× – 1.234× (8 720 – 10 100) | −1 262 … −1 788 |
| **G2a+b** | 0.32 – 0.47 | 2.9 – 3.6 | 1.50 – 2.00 | 4.35 – 7.20 | **0.947× – 1.098×** | **7 753 – 8 988** | 1.065× – 1.234× (8 720 – 10 100) | −967 … −1 112 |
| **G2c** | 0.356 – 0.560 | 2.9 – 3.6 | 1.50 – 2.00 | 4.35 – 7.20 | **0.947× – 1.131×** | **7 748 – 9 258** | 1.073× – 1.292× (8 785 – 10 574) | −1 037 … −1 316 |
| *G2c with `E` = 40* | 0.560 | 3.6 | 2.00 | 7.20 | *1.209×* | *9 893* | 1.292× (10 574) | *−681* |
| *G2c with `E` = 0* | 0.560 | 3.6 | 2.00 | 7.20 | *1.257×* | *10 285* | 1.292× (10 574) | *−289* |

**Reference lines, quoted beside every row and never removed:** `R` = **8 185** (same session,
n = 7, spread 0.42 %); §0.5.2's absolute floor **8 242**; the disarmed blessing class
**≥ 7 988**; §1.5's first-generation band **1.25× – 1.55×** = dhry 10 231 – 12 687.

> ### **The cross-check's verdict, in one sentence: the gap between every model band and its own Amdahl ceiling is 950 – 1 790 dhrystones, it never closes, and the block-entry term is 71 – 85 % of it.** (The gap *is* `entry + gate` by construction — the removal term is identical on both sides — so the split is exact rather than attributed: G2a 78.8 – 85.0 %, G2b 71.3 – 73.9 %, G2c 74.5 – 80.2 %.) §1.5 predicted this shape before any engine existed — *"the cost side is unquantified and it is subtracted from this band, not added to it"* — and three metal rounds have now put a number on the subtraction.

---

# 8. THE DECISION TABLE

*Five options. One row each. No recommendation — that is carried outside this document.*

**Ruled 2026-08-24: PARK + TAX FIX** (user decision; carried in `WARP-ENGINE.md` §0.19, the
fix built in round 6). The table below is kept as priced, for the day the fork reopens.

| option | dhry band vs `R` = 8 185 | cost | risk grade | what it forecloses |
|---|---|---|---|---|
| **PARK** — freeze at G1, `WARP` stays `unset = OFF`, no further rounds | **disarmed 7 903 (−3.445 %)**; armed 7 563 available on request | **0 rounds** | **LOW.** Nothing changes; the correctness machinery, the 18 368-opcode accept set and the ablation kit all stay in the tree as evidence | Forecloses nothing technically — every family list and every contract is written down. Ships a card carrying a **permanent −3.445 % tax on a feature nobody uses**, which has now missed the ≥ 7 988 blessing class **three consecutive rounds**; the standing card stays `F294E75A` |
| **PARK + TAX FIX** — land §6, ship the engine dormant and free | **disarmed A4-class: 8 140 (−0.550 %) measured, A5 ceiling 8 163 (−0.269 %)**; armed unchanged at 7 563 | **1 round, 1 metal session** | **LOW–MEDIUM.** One restructure of `m68k_run_mmu040`, in the function §0.15.2 proved can be mis-built; `tools/warp-gate.sh` is the guard | Forecloses nothing. Clears the ≥ 7 988 class by 152 – 175 dhrystones, so the R5 lean can ship and the engine stays in the tree as a switchable, zero-cost instrument. **This is the only option that improves the card without deciding anything** |
| **G2a** — the write-back contract | **7 458 – 8 312** (+0.9 pp with the tax fix) | **5 rounds, 3 metal sessions** + the tax fix | **HIGH.** Three distinct fault contracts, each able to produce a silent wrong answer that dhrystone cannot see; first honest test is M2 | Forecloses nothing, but **buys a band whose floor is below G1's measured row.** Its own value is the coverage it unlocks for chaining, not its wall clock. Committing here without §4 is buying the cost and not the payoff |
| **G2a + G2b** — write-back contract **plus** block chaining | **7 753 – 8 988** (+0.9 pp with the tax fix) | **8 rounds, 5 metal sessions** + the tax fix | **HIGH.** G2a's risks, plus the cadence contract — the one item in the plan with **cross-repo reach** (`amix-z3660scsi`, `amix-z3660net`), whose failure mode is a missed interrupt rather than a slow number | Forecloses nothing, and it is the first option whose ceiling is meaningfully above `R`. Still under §0.5.2's 60 % coverage bar at its own midpoint, so **it still does not produce a verdict** — the kill line stays armed and unfired for a fourth wave |
| **FULL G2c** — plus supervisor coverage | **7 748 – 9 258** (+0.9 pp with the tax fix) | **11 – 12 rounds, 8 metal sessions** + the tax fix, **and it is gated on M2** | **VERY HIGH.** Supervisor bugs corrupt roots; `φ_covered` is a 0.20 – 0.50 assumption with no capture behind it; rung 0 already owns the best supervisor loops and the JIT declines them by rule | Forecloses the **cheap** answer: at `E` = 112 the ceiling is 1.131× and §1.5's 1.25 × – 1.55 × band is **out of reach at any coverage this document prices**, whereas `E` = 40 alone reaches 1.209×. **Funding families before funding `E` is the expensive order** |

---

# 9. What this document is not

1. **It is not a pre-registration.** Nothing here is a prediction an engine will be scored
   against. WARP-ENGINE.md §3.4's discipline requires a pre-registration commit to precede the
   implementation commits, provably, by timestamp; if any scenario here is funded, it gets its own
   §0.16-style pre-registration first, and **this document's bands are not it.**
2. **It does not move a scored number.** No existing line in `docs/WARP-ENGINE.md` is altered — it
   gains a three-line reference in its status header and nothing else.
   `docs/warp-corpus.py`'s `accept()` predicate is byte-untouched: the change is a **pure
   addition**, 295 inserted lines and zero deleted, and the gen-3/4/5 accept bitmaps and the full
   `--dis` report are identical before and after (`--bitmap` MD5s and the report diffed clean).
   The generation-two candidate families live in a **separate** predicate, `accept_gen2()`, which
   no gate, no bitmap and no `--compare` exit status can reach.
3. **It does not price a verdict.** Coverage is under the 60 % bar in every scenario's lower half,
   so §0.5.2's decision table still reads *"not a verdict in either direction"* — the same reading
   it has had for three consecutive waves.
4. **It has one number with no metal validation anywhere**, and it is load-bearing: blocks per
   chain (§4.1). Everything else traces to a measured row or to a model calibrated against three.
5. **It does not model the mispredict.** §1.6's central mechanism — the 1 867-target indirect
   dispatch branch on a one-target BTB — is the reason a chained JIT should beat an interpreter at
   all, and it is charged **zero** in every band above. The bands are therefore biased **low** by
   an unmeasured amount, and the honest response to that is a measurement, not a coefficient.
