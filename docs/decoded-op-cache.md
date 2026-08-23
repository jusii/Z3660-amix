# The decoded-op cache (CYBERSTORM C2, rungs 1, 1b and 1c)

What it is, what it costs on the path that matters, how it stays coherent on a board where
the *other* CPU writes guest memory, and the pre-registered numbers the metal session is
supposed to judge it against — written down *before* the measurement, because a target
chosen after the result is not a target.

**Rung 1 shipped the cache and was judged on metal 2026-08-23**: dhrystone **+23.3 %**
(4405.3 → 5432.0) and `FETCHOP + FETCHEX` 28.65 % → 10.48–12.64 %, both clearing their bars —
against **both hit-rate bars missed** (dhrystone 77.48 % vs ≥ 88 %, raw disk 67.74 % vs ≥ 85 %),
with the misses diagnosed as invalidation re-warm.

**Rung 1b was the narrowing that diagnosis called for. It was built, measured on metal the
same night, and mostly reverted.** It did exactly what it was designed to do — invalidations
−54.8 %, hit rates +4.8 to +7.1 pp — and it bought **−1.05 pp of speedup**, because the misses
it removed were re-warm misses still hot in the page cache: the cheap ones. The pre-registered
revert line (dhrystone below 5432) was crossed by 2.32 % and fired.

**Rung 1c is that revert, decomposed rather than blanket.** The three arms did not perform
alike and one of them was carrying the whole yield:

| arm | delivered | cost | 1c |
|---|---|---|---|
| **CACR change-only narrowing** | ~93 % of all invalidations removed (≈41 k of 44 k on dhrystone) | one compare at one call site | **KEPT** |
| empty-cache short-circuit | justified on a storm that BLK-on has already collapsed | a flag store on **every fill** | **REVERTED** |
| PFLUSH page-presence filter | 5.6–8.2 % of requests skipped, against a cause worth 6–9 % | two hashed marks on **every fill**, 512 B BSS | **REVERTED** |

Also kept, because they are what made the decomposition possible: the `IV_*` attribution
counters, the per-bucket transition spans, and the `DOPC_INVAL`-with-the-cache-off fix (which
now stands on an explicit switch test rather than riding on the short-circuit).

The measurement that motivates the cache is in the 040 port's `docs/C2-ATTACK-MAP.md` §9.1 and
§11.2. The implementation is `Z3660_emu/src/uae/newcpu.cpp` (the table, the lookup and the
run-loop integration, with the long comment), `Z3660_emu/src/uae/z3660_dopc.h` (the
geometry, the switch rule, the extension-word window and the rung-1b/1c record) and one hook
inside `Z3660_emu/src/uae/include/cpummu.h`. The surviving narrowing is one `if` in
`Z3660_emu/src/uae/newcpu_common.cpp`. The evidence is
`Z3660_emu/test/host/harness_main.cpp` (`test_dopc_state_equivalence`,
`test_dopc_coherence`, `test_dopc_counters`).

## The measurement

Instruction fetch costs **78.2 ARM cycles per opcode word and 85.2 per extension word**, at
1.5983 fetches per guest instruction — **28.65 % of the interpreter** (bracket 25.6–32.9 %,
split `FETCHOP` 17.34 % / `FETCHEX` 11.31 %). It is the largest rung on the map and the one
every correction makes bigger.

Sizing is a measurement and not a guess. From the measured guest-PC distribution over three
lossless rings (map §9.1):

| cache entries | BOOT (busy) | DHRY (busy) | IO (busy) |
|---|---|---|---|
| 1024 | 77.50 % | 74.57 % | 68.56 % |
| 2048 | 84.92 % | 85.99 % | 79.55 % |
| **4096** | **92.40 %** | (4052 PCs total) | **90.40 %** |

**4096 entries, 4-way, 64 KB.** The IO workload decides it: at 2048 it sits at 79.55 %, and
a cache that misses one access in five is a materially different proposition from one that
misses one in ten.

> **CAVEAT — this table and the measured hit rate are not the same quantity, and the gap is
> expected.** The PC distribution above was measured **before rung 0 existed**. Rung 0 now
> collapses each block-idiom loop into ONE dispatch — on the 2026-08-23 boot capture, 240 007
> chunks stood in for 106 662 312 guest instructions — and those are the tightest, most
> repetitive, most cacheable PCs in the workload. They no longer reach this cache as
> dispatches at all, which is precisely why they no longer inflate its hit rate. Metal
> measured **69.76 %** on boot against this table's modelled 92.40 %; counting the idiom
> stream back in reconstructs a BLK-off-equivalent **80.85 %**. So the table is **not
> falsified** by that capture and must not be read as if it were — but neither can it be
> used to size the cache again. **Deciding the geometry needs a fresh PC distribution
> measured with rung 0 live**, and until that exists 4096/4-way stands.

## The shape

| | |
|---|---|
| entries | 4096 = 1024 sets × 4 ways |
| set | 64 bytes — two Cortex-A9 lines, with **all four tags, the generation stamp and the replacement cursor inside the first** |
| index | `(pc >> 1) & 1023` — guest instructions are word-aligned, so bit 0 carries no information |
| tag | the **full guest pc**, with the **address space** (`regs.s`) in its otherwise-dead bit 0 |
| validity | a per-**set** generation stamp compared against one global counter |
| entry | opcode word, resolved `cpufunctbl[]` handler, **one extension word**, and the block-idiom verdict |
| replacement | round-robin, decided at fill time — **a hit performs no stores at all** beyond arming the extension-word window |

Validity is per set rather than per entry because every invalidation this cache performs is
whole-cache: four per-entry stamps could never disagree, and checking them would cost three
more compares on the path this rung cannot afford one on.

**The empty-way marker is set-relative**, `(index ^ 1) << 1`, and that is not fussiness. The
obvious sentinel is a fixed odd constant, on the argument that a key is an even pc so an odd
one is impossible. It is not impossible: the key's bit 0 is the supervisor bit, so key `1` is
"pc 0 in supervisor mode", which a guest reaches by taking an exception through an unplanted
vector. The harness's user-mode CPUSHL case did exactly that on the first run and dispatched a
NULL handler out of a way nothing had ever filled. Every key stored in set *i* has index bits
*i* by construction, so a marker with index bits *i*^1 cannot collide with one — which is a
property of the encoding rather than an argument about which addresses a guest is likely to
reach.

## The idiom verdict rides in the entry

This is the integration that makes rung 1 pay for rung 0's overhead rather than adding to it.

Metal 2026-08-22 measured the block-idiom fast path (rung 0) at **boot −43.6 %** and
**dhrystone −1 %**: the recognizer's three masked compares are paid on *every* dispatch and
repaid only inside the idiom stream, so a workload with no block idiom in its hot loop pays
and never collects.

So the entry carries the recognizer's **static shape verdict**, and on a hit that says "not an
idiom" the run loop does not call the recognizer at all. In the generated ARM that is a `tst`
and a `beq` — **2 instructions in place of 14**.

The bit is **conservative-true**: `z3660_blk_run()` may only clear it when it has positively
established, from the instruction words alone, that this pc is not an idiom head. Every
decline that depends on anything else — the trace bits, the chunk cap, alignment, the device
gate, privilege — leaves it set. A cached true costs one wasted recognizer test; a cached
false would lose the fast path at that pc until the next invalidation, and the fast path is
worth 43.6 % of a boot. The cheap error is the one the design is biased towards, and the
expensive one is unreachable — the words the verdict is computed from are the same words the
entry caches, and anything that changes them invalidates the entry.

It is also **self-correcting**: an entry filled while the machine could not decide says "may
be an idiom", and the first pass that *can* decide, and decides no, writes the cheaper answer
back.

## One extension word, and why only one

The map asks the entry to hold "the instruction's extension words". The generated handlers
fetch their own — `cpuemu_31` calls `get_iword_mmu040(N)` about 920 times — so the only place
to answer them is inside `uae_mmu040_get_iword()`, through an **address-keyed window** the run
loop publishes on a hit.

Address-keyed is what makes it safe. The window holds the word at one specific guest address;
a handler that has already moved the pc computes a different address and falls straight
through to a real fetch, and so does the next instruction's opcode. It is armed only by a
validated hit, and disarmed by the next miss, by any invalidation, and by the run loop's fault
`CATCH`. Its content therefore has exactly the coherence of the entry it came from.

At 16 bytes per entry there is room for one word, and the measurement says one is most of the
prize: 1.5983 fetches per instruction is one opcode plus **0.5983** extension words. What is
*not* reached, stated plainly: second and later extension words, and the 32-bit displacements
and immediates that go through `get_ilong`. `DOPC_EXT` is the counter that says how much was.

The slot is filled by a **real fetch on the miss path**, bounded by the same argument rung 0's
recognizer uses for its own lookahead: the page holding `pc` was just fetched successfully, so
every word inside it is reachable. With the MMU off the granule is the 64 KB bank window
instead — a speculative word from a bank the opcode did not come from could carry a device
side effect, and this never issues one.

## The hit path, counted

The map's caveat is the whole reason to count it: *"a high hit rate with a low speedup is the
expected failure mode"*, because at a 96.6–98.5 % tier-0 `ipagecache` hit rate what this has
to beat is an already-fast path. Counted out of the generated ARM
(`arm-none-eabi-objdump -d` of the **lean** `Z3660_emu.elf`), not estimated:

| path | ARM instructions | plus |
|---|---|---|
| **hit → dispatch** (way 0, extension word served, verdict "not an idiom") | **50** | nothing: no call, no guest-memory read |
| hit → dispatch, no extension word in the entry | 44 | — |
| hit → dispatch, ways 1/2/3 | 52 / 55 / 57 | — |
| **uncached fetch → dispatch** (cache off; MMU on, `ipagecache` hit) | **104** | the `x_phys_get_iword` callee **and a guest-DRAM read** |
| extension word **served from the entry** | **10** | nothing |
| extension word fetched (same conditions) | 47 | the `x_phys_get_iword` callee and a guest-DRAM read |

The 104 splits as 58 in the run loop and 46 inside the single out-of-line
`uae_mmu040_get_iword`, and it does **not** include `x_phys_get_iword` or the bus traffic —
which is where most of the map's 78.2 cycles actually live. So the ratio understates the
change, deliberately: the instruction counts are the part that can be counted exactly.

Three further things the hit removes that do not appear as instructions: the indirect call
through `x_prefetch`, the indexed load out of the 65 536-entry (256 KB) `cpufunctbl` — the
cached handler comes out of the **same cache line as the tag** — and the guest-memory read
itself.

**The miss path pays about 11 extra run-loop instructions** (the switch test, the window
disarm, the fill call) plus the extension-word lookahead. At the pre-registered hit rates
that is ~1 instruction per dispatch amortised against ~54 saved on 88–92 % of them.

Function sizes, for the record — lean / default / profiling `m68k_run_mmu040`: 396 → **572**,
664 → **860**, 529 → **715**; `uae_mmu040_get_iword` (lean) 136 → **152**. **Rung 1b left both
of the lean figures exactly where rung 1 put them** (572 and 152), which is how "the narrowing
costs nothing on the hit path" is checked rather than claimed.

### The way histogram — four counters, and a verdict written before them

The table above is why C2 v3.1 declined to *optimise* this path and asked for one measurement
instead. `DOPCFIND` measures at **56.7–58.8 corrected ARM cycles per dispatch** against the
**50–57 counted instructions** in that table: **about one instruction per cycle**, so there is
no stall pool to reclaim and the reclaim is bounded by *instructions removed*. The way walk is
the whole of what a way hint could remove and it is **7 instructions end to end** (50 → 57) —
against a hint check of **≥ 2 instructions on every dispatch, hit or miss**.

So the band turns entirely on one number nobody had measured: **how hits distribute across the
four ways.** `DOPC_WAY0..WAY3` (ids 39–42) are that number, incremented on the hit arm where
the way is already in a register. The miss arm gains nothing — a miss has no way.

```
DOPC_WAY0 + DOPC_WAY1 + DOPC_WAY2 + DOPC_WAY3  ==  DOPC_HIT
```

**The decision rule, pre-registered from the map before the counters existed** — and printed
by `PROFD` as a `[PROF] r dopc way VERDICT:` line so it is applied rather than left in a
document:

| way-0 share | verdict |
|---|---|
| **≥ 85 %** | the walk is already short; way prediction is worth < 0.5 % and **the rung is STRUCK from the ladder** |
| **≤ 60 %** | the walk is real, the 6.5 % ceiling is live, and the rung **earns a design round** with its cost site (the hint check, on every dispatch) pre-registered |
| in between | it **stays at ladder #5**, behind three rungs worth 4–8 % each |

The 6.5 % figure is the ceiling *if hits were uniform across ways* — mean 53.5 instructions
falling to 50 — **before** charging the hint check, which gives most of it back. That is why
85 % strikes the rung rather than merely deferring it.

**Cost to take it: one counter block on one prof-image boot.** The same capture that scores
the accessor rung carries it; there is no reason to spend a session on it.

## Coherence — the whole risk surface

It is asymmetric. A cache that invalidates too eagerly is merely slower; one that invalidates
too little executes instructions the guest has already overwritten, and that is a working
system that goes wrong hours later with no symptom at the point of the bug. So this one
follows the map's instruction literally: **invalidate everything on any event that could
plausibly matter, measure the hit rate, and only then narrow it.** Whole-cache invalidation is
affordable as the *default* policy because it is O(1) — one generation increment — rather than
a walk.

**Rung 1b changed no row of this table.** Every event below still arrives and still results in
a whole-cache invalidation whenever it has anything to invalidate; what rung 1b added is the
right to answer two of them with "provably nothing to do". The `cause` column is the counter
each one now lands in.

| event | where | cause | why |
|---|---|---|---|
| `CPUSHL` / `CPUSHP` / `CPUSHA` / `CINV` | `mmu_flush_cache()` via `flush_mmu040()` | `IV_FLUSH` | the 68040's own instruction-cache maintenance |
| `PFLUSH` (one page) | `mmu_flush_atc()` | `IV_PFLUSH` | a mapping change — **the one narrowed cause** |
| `PFLUSHA` | `mmu_flush_atc_all()` | `IV_PFLUSHA` | as above, naming no page |
| MOVEC to `TC`, MMU reset | the same, via `mmu_set_tc()` / `mmu_reset()` | `IV_PFLUSHA` | translation turned on, off or resized |
| MOVEC to `URP` / `SRP` | `newcpu_common.cpp` | `IV_ROOT` | an address-space change; tags are virtual |
| MOVEC to `CACR` | `newcpu_common.cpp` | `IV_CACR` | the register that enables the caches this stands in for |
| `build_cpufunctbl()` | `newcpu.cpp` | `IV_TABLE` | entries hold **resolved handler pointers**; a mode change makes every one wrong |
| `map_banks()` | `memory.cpp` | `IV_MAP` | the bank map an opcode was read through is being replaced |
| **DMA into guest RAM** | `Z3660_emu/src/main.cpp` | `IV_DMA` | see below |
| the switch moving | `z3660_dopc_set()` | `IV_KNOB` | an off period is an unwatched period |

**`mmu_flush_cache()` is the choke point, and picking it is the design decision.** Rung 1b gave
the two ATC callers their own causes and their own policy, so what still arrives *there* is
`CPUSHL`/`CINV` — **plus anything upstream adds later, which is the property worth keeping**: a
new upstream caller inherits the whole-cache invalidation automatically, and has to be opted
*out* of it rather than into it. That is the safe default for the one class of bug this cache
can produce. It is
already where upstream flushes its own (compiled-out) instruction cache, so this cache is
invalidated by exactly the operations the *architecture* requires a guest to perform before
executing code it has written or remapped. That requirement is what makes it safe not to check
every guest store — and AMIX honours it: its `CPUSHL` loop alone is 7.79 % of a boot. Note the
rung 0 interaction the map flags: the block-idiom fast path collapses that whole loop into
**one** `flush_mmu040()`, and one is all that has to arrive, because the flush is idempotent
and the invalidation is whole-cache.

### The DMA arm is specific to this board, and it is not hypothetical

core0 reads disk sectors **straight into guest DRAM** (`scsi.c`'s `f_read` into the mapped
address), and the interpreter never sees those bytes go past. An AMIX text pagein, or an
AmigaOS loader filling a buffer it is about to jump into, replaces instruction bytes
underneath this cache.

core1's own side of that mailbox already knows when it happened: it spins until core0 is done
and then invalidates its L1 over the exact range. That line is the precise moment the write
becomes observable to this core, so the decoded-op cache is invalidated beside it — for the
piscsi read triggers and for the floppy ones.

**The alternative is worth naming because it looked right first.** A `SHARED` epoch word that
core0 bumps after every DMA puts the knowledge on the producer, and a core1 running against a
core0 built before that field existed would read a permanent zero and never invalidate:
**fail-dangerous, silently, in the one direction that corrupts a filesystem.** Here the
consumer decides, using knowledge it already has, and needs nothing from the image on the
other side.

### What is deliberately *not* in the list

An ordinary guest store. Checking every write against the cache would cost more than the cache
saves, and the 68040 does not require it. `test_dopc_coherence` pins that as a **documented
contract** rather than leaving it to be discovered: with no maintenance the stale decode runs,
and every other case in the suite changes the answer.

## The switch

`SHARED.dopc_on` at `0xFFFF00E8`, resolved by `z3660_dopc_resolve()` in `z3660_dopc.h` and
**nowhere else**:

| mailbox word | meaning |
|---|---|
| `0` (unset) | the built-in default, **ON** |
| `0xFFFFFFFF` | OFF — the lookup returns before it has read the table, nothing is filled, the window stays disarmed |
| anything else | ON |

Unset means *on*, not off, for the same reason `overhead_switches.h` and `z3660_blk.h` give: a
core0 built before the field existed must still run the shipped behaviour.

It is a toggle and not a size, unlike `BLK`: the geometry is fixed by the map's measured PC
distribution, and a runtime way-count knob would put a variable bound on the hit path, which is
the one place this rung cannot afford one.

Serial console: **`DOPC`** (or `DECODED OP CACHE`) toggles it. Core1 picks it up at its next
knob resync and **invalidates the whole cache when it does** — an off period is an unwatched
period, and invalidating on every set removes that question rather than answering it.

## The counters

Profiling build only, appended counter ids 24–27 (rung 1) and 28–37 (rung 1b), reported by
`PROFD`.

- **`DOPC_HIT / (DOPC_HIT + DOPC_MISS)`** — the hit rate. `HIT + MISS == INSNS` identically:
  every dispatch consults the cache exactly once, which is what makes it a fraction of
  anything.
- **`DOPC_EXT`** — extension words served out of the entry rather than fetched. The two halves
  of this rung are separable and worth separating: `FETCHOP` goes by construction on every hit,
  `FETCHEX` only for the instructions whose first extension word fits the slot. **The
  pre-registered `FETCHOP + FETCHEX < 18 %` is not reachable on the first half alone.**
- **`DOPC_INVAL`** — invalidations **actually performed**. The price of the conservative policy,
  and the counter that decides whether narrowing is worth doing.

  **Do not compare it against `DOPC_MISS`.** The rung-1 text here said "large relative to
  `DOPC_MISS` means the misses are self-inflicted", and that test is wrong: one whole-cache
  invalidation can cost up to 4096 refills, so the two counts are not commensurable and their
  ratio means nothing. On metal `DOPC_INVAL` ran 0.34–0.37 % of `DOPC_MISS` on every workload,
  which by that test says "not self-inflicted" — and the misses were self-inflicted.

  **The right quantity is misses per invalidation, read against the working set.** Metal
  2026-08-23 measured **291.5 (boot), 274.3 (dhrystone), 285.7 (raw disk)** — a near-constant
  across three workloads with entirely different code footprints, which is the signature of
  re-warm rather than capacity; a capacity-bound cache's miss count tracks its workload's
  footprint. And ~285 is far below the 4096 entries held, so capacity was never the binding
  constraint at all. The firmware now prints this row itself (`[PROF] r dopc … misses … per
  invalidation`) so the next reader does not have to form it by hand.

- **`IV_FLUSH … IV_CACR`** (ids 28–37, rung 1b) — invalidation **requests**, one counter per
  cause, counted where they reach `z3660_dopc_invalidate()`. This is the attribution the
  2026-08-23 verdict needed and could not get: it could establish that the misses were
  re-warm, but not *which* flush was causing them. It is what decided rung 1c, before any
  timing was consulted.

  With the cache on, **`sum(IV_*) == DOPC_INVAL`** exactly — every request that reaches the
  invalidator performs one. (Rung 1b's short-circuit made this an inequality; the revert
  restored the identity, and the harness asserts equality again.)

- **`IV_CACR_SKIP`** (id 38, rung 1c) — **not a cause.** `MOVEC` to CACR that changes no
  retained bit is proved inert *at its call site*, so it never reaches the invalidator and
  no `IV_*` counter can see it.

  > **This is a defect the verdict found in its own pre-registered instrument, and the rule
  > that came out of it.** Row E was defined as `sum(IV_*) − DOPC_INVAL` = "the number the
  > narrowing skipped". For the CACR arm that is **false**: an inert write is not a request
  > that was skipped, it is a request that was **never counted**. The session measured a
  > 54.8 % fall in invalidations while the row could account for only 7.3 % of it, and had to
  > reconstruct the other ≈41 000 by hand across two sessions' captures.
  >
  > **A narrowing that decides before `z3660_dopc_invalidate()` must bring its own request
  > counter, or it is invisible to the row that scores it.** A second call-site narrowing
  > brings a second counter rather than sharing this one — a counter aggregating two causes
  > can attribute neither.

  So the guest's own request rate is **`sum(IV_*) + IV_CACR_SKIP`**, and the narrowed share is
  `IV_CACR_SKIP` out of that. The firmware prints exactly that row.

**With the cache off**, the contract is more interesting than the rung-1 text claimed, and it
is pinned in `test_dopc_counters`:

| counter | cache OFF | why |
|---|---|---|
| `DOPC_HIT` / `MISS` / `EXT` | exactly **0** | the lookup returns before it can count |
| `DOPC_INVAL` | exactly **0** | `z3660_dopc_invalidate()` consults the switch too, not only the lookup |
| `IV_*` | **they move** | they count what the *guest* asked for, and the guest issues `CPUSHL` and `PFLUSH` whatever this switch says |

The rung-1 sentence "**none of them move at all** … the property the whole equivalence corpus
rests on" was **false when written** — `z3660_dopc_invalidate()` had no on/off test at all
(only the *lookup* consults the switch), so `DOPC_INVAL` counted regardless, and metal read
164 311 and 2 951 874 in the two cache-off arms. `DOPC_INVAL` is genuinely zero there now, and
the `IV_*` block deliberately does not follow it: the OFF arm is a free, uncontaminated
measurement of the guest's own invalidation rate, which is the denominator every future
narrowing decision needs.

> Rung 1b delivered that property as a *side effect* of the empty-cache short-circuit
> ("nothing is ever filled, so the skip fires on every request"). Rung 1c reverted the
> short-circuit and **kept the property on its own footing**: an explicit switch test at the
> top of the invalidation path. **The safety argument is the ON transition, not the skip** —
> `z3660_dopc_set()` and the knob resync both store the switch *before* they invalidate, so
> coming back on always performs the invalidation that kills whatever went stale during the
> off window. The harness drives that transition rather than asserting it, and reordering the
> two lines fails it.

## Rungs 1b and 1c — the narrowing, the namespace wall, and what the metal took back

Metal 2026-08-23 measured the conservative policy's price and it is the dominant term:
**274–292 misses per invalidation across three unrelated workloads.** Rung 1b narrowed it and
was measured the same night; rung 1c is the decomposed revert. The first thing established is
what *cannot* be narrowed, because that is what shapes the rest — and it is the part of this
section that survives both rungs intact.

### The namespace wall

**This cache is tagged by guest LOGICAL address. The two largest invalidation causes name
PHYSICAL ones.**

| cause | its address | narrowable against a logical tag? |
|---|---|---|
| `CPUSHL` / `CINV` | **physical** — the 68040's caches are physically tagged, so `CPUSHL <cache>,(An)` takes a physical address (upstream's `flush_mmu040()` discards the argument for exactly this reason) | **no** |
| DMA (piscsi / floppy) | **physical** — it is a DMA descriptor, an address a device could execute | **no** |
| **`PFLUSH` of one page** | **logical** — `PFLUSH (An)` takes a logical address | **yes** |
| `PFLUSHA`, `TC`, `URP`/`SRP`, `build_cpufunctbl()`, `map_banks()` | names no page at all | n/a — genuinely global |

Scoping either physical cause against a logical tag compares two different numbers and skips an
invalidation that was required. **The host corpus already demonstrates it**: `dopc_m_dma()`
flushes `DOPC_PCODE` (0x001C0000) for an entry tagged at `DOPC_VCODE` (0x00800000), so a naive
range test refuses that flush and the stale decode runs. Narrowing those two needs a *physical*
index this cache does not maintain, and building one costs a translation on the fill path —
a separate rung with its own evidence.

The arithmetic says that is the right order anyway. DMA is a **minority cause**: the raw-disk
capture recorded 84 219 invalidations against roughly 7 500 piscsi reads for that workload, so
even eliminating the DMA arm entirely addresses under a tenth of the flushes on the workload
that uses it most, and essentially none on dhrystone. The storm is in the flush path.

### ★ The attribution, measured — and it is what decided rung 1c

Read before any wall-clock number, which is the ordering the whole protocol exists for. `IV_*`
counts requests; shares are of `sum(IV_*)`. Metal 2026-08-23, cadence 1, `DOPC` on:

| cause | boot | dhrystone | raw disk | narrowable? |
|---|---|---|---|---|
| `IV_FLUSH` (`CPUSHL`/`CINV`) | **63.83 %** | **51.16 %** | **46.63 %** | **NO** — physical |
| `IV_PFLUSHA` | 27.15 % | 36.37 % | 35.66 % | no — names no page |
| `IV_PFLUSH` | 6.15 % | 9.14 % | 6.85 % | **yes — the only one** |
| `IV_DMA` | 1.55 % | 0.06 % | 8.45 % | **NO** — physical |
| `IV_ROOT` | 1.32 % | 3.27 % | 2.41 % | no — global |
| `IV_CACR` | 0.00 % | — | — | narrowed at the call site |

**`IV_FLUSH` + `IV_PFLUSHA` is 82–91 % of every workload and neither can be narrowed against a
logical tag. The one cause the page filter was built for is 6–9 %.** The attribution said that
before a single timing was taken, and it is the reason the revert is a decomposition rather
than a coin-toss.

> **★ The figure to quote for `IV_FLUSH` is its measured range: 46.70 – 63.89 %** — raw disk at
> the bottom, boot at the top, dhrystone at 51.21 % in between. That is the range C2 map v3
> §17.3 carries and the one a decision should cite, because `IV_FLUSH` is **the only large
> cause that carries a namespace a physical index could match against**, and it is therefore
> the number that would fund building one. The other 27–36 % (`IV_PFLUSHA`) names no page and
> is not reachable by this or any tag-based narrowing.
>
> The table above and the map's are two independent derivations of the same session and agree
> to within 0.07 pp on every row (63.83/51.16/46.63 here against 63.89/51.21/46.70 there); the
> map's are the ones quoted downstream, so they are the ones named here. Nothing in either set
> changes a conclusion, and the discrepancy is recorded rather than harmonised away because a
> silently unified number is how two captures become one imaginary one.

### What rung 1b did, and what happened to each arm

1. **The empty-cache short-circuit** — applies to **every** cause, in either namespace. If
   nothing has been filled since the last invalidation, no entry is live, so the invalidation
   has nothing to do. It cannot be wrong: "no entry exists" is not an approximation.

   **REVERTED (1c), and not because it was wrong.** It was justified on the rung-1 observation
   of **18× more invalidations with the block-idiom recognizer off** (2 936 789 / 2 951 874
   against 159 902) — AMIX's `CPUSHL` loop invalidating once per instruction with nothing
   dispatched in between. **That measurement was taken with BLK OFF.** With BLK **on**, which
   is the shipping posture and the posture rung 1b was measured in, rung 0's fast path already
   collapses that whole loop into one `flush_mmu040()`. **The storm the short-circuit exists to
   absorb has already been eliminated before it is reached** — it arrives to find the work
   done, having charged a flag store on every fill to get there.

   > The general lesson, which is bigger than this arm: **a justification measured with another
   > rung switched off is not a justification for the shipping configuration.** Re-take it in
   > the posture the change will actually run in.

2. **`PFLUSH` of one page**, through a **page-presence filter**: 4096 bits keyed on a hash of
   the 4 KB logical granule, 512 bytes, set on every fill and cleared with the generation. The
   invariant was *every live entry's page bit is set*; the contrapositive — bit clear ⇒ no live
   entry in that page ⇒ the flush is provably a no-op — licensed the skip. Sound, and
   mutation-tested against five separate defects.

   **REVERTED (1c).** It skipped 5.6–8.2 % of requests against a cause worth 6–9 %, for two
   hashed marks and a flag store on **every fill**. See "The hit path was untouched" above for
   why that trade lost.

3. **`MOVEC` to CACR** narrowed to writes that *change* the register, with its own cause id. On
   a 68040 the retained bits are the two cache enables and nothing else (`cacr_mask` 0x80008000
   — the 68040 has no clear-cache bits in CACR; it uses `CINV`/`CPUSH`, which arrive at
   `mmu_flush_cache()`). If neither enable moved, the cache configuration is bit-identical.

   **KEPT (1c) — this is the whole surviving narrowing.** One compare at one call site, nothing
   on the hit path and nothing on the fill path, and it removed **~93 % of every invalidation
   rung 1b removed** (≈41 000 of the 44 247 that went away on the dhrystone capture). Its yield
   is now counted by `IV_CACR_SKIP`, because a call-site narrowing is invisible to the `IV_*`
   block by construction.

4. **Everything else stays whole-cache**: root and table changes are genuinely global and
   genuinely rare, and narrowing there would buy a rounding error at real risk.

### Why the filter, and not a tag walk — kept, because the next attempt will meet it again

A 4 KB page spans `pc[11:1]` and the set index is `pc[10:1]`, so **one page touches every one of
the 1024 sets**. "Invalidate just this page" is a full 4096-tag walk over 32 KB of tag lines —
the whole Cortex-A9 L1 D-cache — evicting the interpreter's working set on every `PFLUSH` to
save a re-warm of ~285 entries. The filter answered the same question with one load and one
test, and it still lost. **Whatever narrows `PFLUSH` next has to beat a 6–9 % cause with a cost
that is not paid per fill** — which is a much harder brief than either of these two met.

### The hit path was untouched — and the fill path was not, which is what decided it

The pre-registered failure mode for this whole rung is *"a high hit rate with a low speedup"* —
what happens when the lookup costs as much as the fetch it replaces. Rung 1b was designed to
add **nothing** to the hit path, and it succeeded: `uae_mmu040_get_iword` and
`m68k_run_mmu040` were byte-identical across rungs 1 and 1b, and they still are in 1c.

**The failure mode arrived anyway, one path over.** The narrowing was free on the hit path and
charged on the **fill** path — two hashed filter marks and a flag store on *every* fill — and
the fill path runs on every miss. Against that it removed re-warm misses whose bytes were
still in `ipagecache` (91.61 %) and in the ARM's caches: the cheap ones. **The cost was paid
per miss and the saving was worth a fraction of a miss.** That is the shape to look for in the
next narrowing, and it is why a physical index (which costs a *translation* on the fill path)
needs its own pre-registration rather than an argument by analogy.

> **★ The fill path now has a bucket (C2 rung 1d): `DOPCFILL`, profiler id 15.** Every
> statement above about fill-path cost was reasoned from ARM instruction counts and from what
> happened to dhrystone; none of it was ever measured *as a share*, because there was nowhere
> for it to land — the fill fell into `LOOP` along with everything else the run loop does. It
> is bracketed now, entered once per miss, with `tcnt[DOPCFILL] == DOPC_MISS` as an exact
> identity the dump checks. **The physical-index study's pre-registration should be written
> against that measurement rather than against this paragraph**, which is what map §17.3's bar
> asks for in as many words: *"this study must pre-register its cost site and measure the
> fill-path price, not assume it."* The lookup half is bracketed too (`DOPCFIND`, id 14), so
> the same capture prices the hit path a physical index must not slow down. See
> `docs/profiler.md`, "The LOOP split".

Current sizes, lean build, **counted in ARM instructions** from `objdump -d`:

| symbol | lean | |
|---|---|---|
| `uae_mmu040_get_iword` | **76** (0x130 = 304 bytes) | the hit path — unchanged across 1, 1b, 1c |
| `m68k_run_mmu040` | **572** (0x8f0 = 2288 bytes) | unchanged across 1, 1b, 1c — **and 1d**, proven byte-for-byte |
| `z3660_dopc_fill` | **65** (0x104 = 260 bytes) | the marks are gone |

> **A unit note on the rows above, since this table has been bitten by one before.** These
> are the symbols' sizes in 32-bit *words* (`bytes ÷ 4`), which on a fixed-width target is
> instructions **plus each symbol's literal pool**. Counting instructions alone —
> `objdump -d` minus the `.word` lines — gives `m68k_run_mmu040` **552** and
> `uae_mmu040_get_iword` **64** on the same lean build. Neither measure is wrong; they are
> not the same measure, and the difference is the constant pool. `docs/profiler.md`'s
> prof-versus-lean table counts instructions.

> **Unit correction.** The rung-1 table above read "`uae_mmu040_get_iword` 136 → 152
> instructions" beside "`m68k_run_mmu040` 572 instructions". The two rows were **not in the
> same unit**: 572 is ARM instructions, but 152 is halfwords — the symbol is 304 bytes, i.e.
> **76** ARM instructions on this fixed-width target. The *relative* claim the row was making
> (it grew by 16 halfwords = 8 instructions to carry the extension-word window) is unaffected,
> and so is the −12.90 % cache-off finding derived from it. Sizes here are ARM instructions
> with the byte count beside them, so the unit cannot drift again.

### PRE-REGISTERED for the rung-1b verdict — and how it scored

Expected bands, written before the metal session, per narrowing. **`IV_*` attribution is the
first thing to read** — it is what says whether each band was even addressable. The `measured`
and `verdict` columns were added after; the pre-registered ones were not touched.

| row | rung-1 | rung-1b expectation | **measured** | verdict |
|---|---|---|---|---|
| hit rate, **dhrystone** | 77.48 % | **80–85 %** | **84.56 %** | **HIT** |
| hit rate, **boot** | 69.76 % | **75–85 %** | **74.60 %** | MISS by 0.40 pp |
| hit rate, **raw disk** | 67.74 % | **72–82 %** | **73.85 %** | **HIT** |
| **misses per invalidation** | 274–292 | **unchanged, 260–300** | 393.7 / 413.2 / 405.7 | outside — **row mis-specified**, see below |
| **`sum(IV_*) − DOPC_INVAL`** | — | **> 30 % of requests skipped** | 5.58 / 8.19 / 6.12 % | MISS **as defined** — the instrument could not see the real ~55 %, see below |
| dhrystone, cadence 1, lean | 5432.0 | **5550–5750** | **5306.0** | **REVERT** (5306 < 5432) |
| `FETCHOP + FETCHEX` | 10.48–12.64 % | **9–12 %** | **11.04 %** raw | **HIT**, and down |

**Three hits, one near-miss, two defective rows, one revert.** The narrowing did what the
attribution predicted it could — invalidations −54.8 %, hit rates +4.8 to +7.1 pp — and bought
no speedup, because the misses it removed were the cheap ones. Boot was unaffected either way
(see the boot A/B below), which was the question the session was told to settle first.

#### The two rows that were defective, stated as defects rather than as caveats

**Row D — "misses per invalidation, unchanged 260–300" was pre-registered as a falsifier of the
re-warm diagnosis. It is not one; it is an arithmetic identity.** Misses per invalidation is
misses ÷ invalidations, and the narrowing's entire job is to cut the denominator. On dhrystone:

```
misses         22 141 773 -> 15 070 608   = -31.9 %
invalidations      80 719 ->     36 472   = -54.8 %
ratio           x (0.681 / 0.452) = x1.506
274.3 x 1.506 = 413.6 predicted           413.2 measured
```

**The row moved *because* the narrowing worked, to three significant figures.** And the
diagnosis it was supposed to test is **confirmed**, not refuted: misses **fell** when
invalidations fell, which is the signature of re-warm — a capacity-bound cache's miss count
would have stayed pinned to its workload's footprint. **The correct falsifier is "misses do NOT
fall when invalidations fall"**, and this row must not be re-used in its pre-registered form.

**Row E — `sum(IV_*) − DOPC_INVAL` cannot see a narrowing that decides at a call site.** The
CACR narrowing sits in `newcpu_common.cpp`, ahead of `z3660_dopc_invalidate()`, so an inert
write never increments `IV_CACR` at all: it is not a skipped request, it is an uncounted one.
The row read 5.58/8.19/6.12 % while the true yield was **~55 %**, of which ~93 % was the CACR
arm. Rung 1c fixed the instrument (`IV_CACR_SKIP`, id 38) rather than restating the row, and
the general rule is in "The counters" above.

**The dhrystone arithmetic, shown rather than asserted.** At 77.48 % hit, 22.52 % of dispatches
miss. A miss costs the opcode fetch (78.2 cyc measured) plus the extension lookahead and the
fill; a hit costs the tag compare. Take the marginal miss cost as ~180 ARM cycles and dhrystone
at 5432 with `FETCHOP + FETCHEX` ≈ 11 % of the corrected total. If the hit rate reaches 82 %,
the miss count falls by (22.52 − 18.00)/22.52 = **20.1 %**, and the fetch-attributable share
falls with it: 11 % × 0.201 ≈ **2.2 % of total time removed**, giving 5432 × 1/(1 − 0.022) ≈
**5554**. At 85 % the same arithmetic gives (22.52 − 15.00)/22.52 = 33.4 %, 11 % × 0.334 ≈
3.7 %, ≈ **5640**. Hence the 5550–5750 band, with the top end allowing for the invalidation
work itself disappearing on skipped requests. **A result below 5432 means the narrowing cost
more than it saved and the filter should be reverted, not tuned.**

> **That arithmetic is where the rung failed, and it is worth keeping visible.** It priced
> *every* avoided miss at the marginal ~180 cycles. The metal says the misses a narrowing
> actually avoids are **re-warm** misses — refills of a page the machine just flushed, still
> hot in `ipagecache` and in the ARM's caches — and they are worth a fraction of that. The
> same-boot ratio is what exposed it: **+7.08 pp of hit rate, −1.05 pp of speedup** (ON/OFF
> +41.57 % → +40.52 %). Any future miss-reduction estimate on this cache must price avoided
> misses by *which* misses they are, not by the average.

**★ The boot A/B — settled, and the answer is "not the cache".** Rung 1's 52 s against rung 0's
43 s was a cross-session comparison on a rig shown to drift ~9 %. Six guest boots, one session,
one card, window = `68040 MMU enabled` → first TCP accept:

| image | DOPC ON | DOPC OFF | ON repeat |
|---|---|---|---|
| lean | 53 s | **52 s** | 53 s |
| prof | 69 s | 72 s | 73 s |

**Switching the cache off does not recover the 9 s** (lean OFF is 52 s, not 43 s), and rung 1's
52 s reproduces at 53/53 with zero spread, so the rig did not drift on this row. **The cache's
own boot cost is ≤1 s (1.9 %).** The residue belongs to rung 1's switch-independent overhead
(`uae_mmu040_get_iword` on a path that runs ~1.6× per guest instruction) and/or rung-0-session
drift. Three lessons the session paid for:

- **Take a repeat per arm.** The prof pair alone (69 vs 72) would have read as a 4 % cache win;
  the third boot showed the ON arm's own spread is 4 s. The prof image resolves nothing on this
  row — `prof_hz` arms the sampler from power-on — so only the lean rows carry the verdict.
- **The `Emulation mode 4` → MMU-enable window is as short as 18 s on the lean image**, not the
  ~45 s this document previously estimated. Measured from rung 1's own span before any code was
  written (02:10:56 → 02:11:14). A host-side poll pays an ssh round trip per probe, so the arm
  must do the detect **and** the write inside **one** ssh at ~0.2 s. Sending earlier is not
  safer: at the boot menu `D`, `O`, `P`, `C` are menu keys and `C` catches the autoboot
  countdown.
- **The echo is the ground truth.** Both OFF arms carry `DECODED OP CACHE OFF` in the captured
  span, before `68040 MMU enabled`. An arm that cannot be seen in the log did not happen.

**Bench note (not this repo):** the 2026-08-23 session lost ~7 minutes and 6 telnet logins to
`amixrun`'s `AMIX_CMD_TIMEOUT` defaulting to 60 s — too short for the both-accelerations-off arm,
where the *interactive* login path (fork/exec/page-fault/bzero) is exactly the work rung 0
accelerates. Run the sweep with `AMIX_CMD_TIMEOUT=300`, and raising the fieldkit default is an
`amix-kerntools` change.

### PRE-REGISTERED for the rung-1c verdict — and how it scored

Rung 1c removes two of rung 1b's three arms and keeps the third, so it is scored **against rung
1b's measured figures and against rung 1's**, not against a fresh model. Written before the
session that will judge it; the **measured** and **verdict** columns were added after and the
pre-registered ones were not touched.

| row | rung 1 | rung 1b | **rung-1c expectation** | **measured** | verdict | reasoning |
|---|---|---|---|---|---|---|
| **dhrystone, cadence 1, lean** | 5432.0 | 5306.0 | **≥ 5432** | **5584.0** | **HIT**, +2.80 % | the revert removes the per-fill marks that cost 1b its 2.32 %; the kept CACR arm costs one compare, so ~neutral-to-slightly-positive against rung 1 is the honest expectation. **This is the row the rung stands or falls on.** |
| hit rate, **dhrystone** | 77.48 % | 84.56 % | **78–82 %** | **85.32 %** | **above band** — and the band was wrong, see below | gives back most of 1b's +7.08 pp: the CACR arm is kept and it removed ~93 % of the invalidations, but re-warm from the restored PFLUSH/short-circuit traffic returns |
| hit rate, **boot** | 69.76 % | 74.60 % | **70–74 %** | **72.83 %** | in band | as dhrystone; boot's `IV_PFLUSH` share (6.15 %) is the part coming back |
| hit rate, **raw disk** | 67.74 % | 73.85 % | **69–73 %** | **75.53 %** | **above band** | as boot |
| **`DOPC_INVAL`, dhrystone** | 80 719 | 36 472 | **well below 80 719, near 40 000** | **39 858** | **HIT** | the CACR arm alone accounted for ≈41 k of the 44 k rung 1b removed, so most of 1b's invalidation win must survive the revert. **If it lands near 80 000 the CACR arm is not working** and the keep was mis-attributed. |
| **`IV_CACR_SKIP`** | — | (uncounted) | **≈ 40 000 on a dhrystone run**, and `sum(IV_*) == DOPC_INVAL` exactly | **39 353**, and the identity held **exactly** | **HIT** | the new counter should make visible, in one dump, the quantity the verdict reconstructed across two sessions |
| **boot window, lean** | 52 s | 53 s | **52–54 s, unchanged** | **52 s** | **HIT** | nothing on the boot path changed; a move here means something else did |
| `FETCHOP + FETCHEX` | 12.64 % | 11.04 % | **10–13 %** | **not taken** | **owed** | tracks the hit rate back down; it must not exceed rung 1's 12.64 % |

**Six hits, two hit-rate rows above band, one row untaken.** The rung is taken: the row it was
said to stand or fall on cleared its bar by 2.80 % on a three-run arm with 0.125 % spread, the
invalidation attribution came in where the keep predicted, and the new counter made in one dump
what the previous verdict reconstructed across two sessions.

#### The hit-rate bands were refuted, and the lesson is bigger than the rows

**The three hit-rate rows assumed 1b's +7.08 pp would be *given back* when its two arms were
reverted. It was not, because it was never theirs**: the gain belonged to the kept CACR arm all
along, and reverting the page filter and the short-circuit did not touch it. Two of the three
rows therefore came in *above* their bands, and the bands should be read as **refuted, not
narrowly missed** — an expectation built on a mis-attribution does not become right by landing
close to it.

> **★ And the rung's own completed lesson, which is why those rows should not have been
> load-bearing in the first place: the hit rate is not what pays. The fill-path cost is.**
>
> Rung 1b had the **higher** hit rate (84.56 % against rung 1's 77.48 %) and was **slower**
> (5306.0 against 5432.0). Rung 1c has a hit rate higher again (85.32 %) and is faster than
> both (5584.0). Across three measured configurations the hit rate and the speed do not track
> each other at all; what tracks the speed is **what the fill path costs**, because the fill
> path runs on every miss and its price is paid whether or not the miss it serves was
> expensive. 1b's two hashed filter marks and flag store were paid on every fill to remove
> misses that were **re-warm** — still hot in `ipagecache` (91.61 %) and in the ARM's own
> caches — and re-warm misses are the cheap ones.
>
> So: **do not pre-register a hit-rate band as a proxy for a speed prediction on this cache.**
> Pre-register the fill path's cost, and price avoided misses by *which* misses they are. That
> is also the bar the next rung has to clear — see the physical index below, whose whole cost
> lands exactly there.

**Stop conditions.** (a) An OFF arm whose `DOPC_HIT`/`MISS`/`EXT`/`INVAL` are not all exactly
zero **in a `PROFZ`-zeroed window** — the counters accumulate from power-on and the console
cannot fire before core1 starts, so an un-zeroed OFF arm legitimately carries a warm-up window
(2.49 M dispatches, 1.5 % of a boot) and is *not* a stop. (b) `sum(IV_*) != DOPC_INVAL` on a
cache-on capture. (c) `STACK_OVF` non-zero. (d) any new panic, bus error or filesystem error in
the span. **None fired.**

**What is still owed on this rung**, carried here so it is not lost between documents: the
`FETCHOP + FETCHEX` row above (C2 map §19 item 1, the cheapest thing on that list); a same-boot
`DOPC` ON/OFF A/B defending the +2.80 % against the rig's proven ~9 % cross-session drift (§19
item 3 — 2.80 % sits inside that band, and no same-boot ratio was taken in the 1c session); and
the cache's worth **on disk** for 1c, which has no arm at all.

## PRE-REGISTERED (rung 1) — and how it scored

From map §11.2, and from the #1 metal verdict this change was built on top of. **Scored on
metal 2026-08-23**; the measured column was added after, the pre-registered one was not touched.

| | measured before | pre-registered | **metal 2026-08-23** | |
|---|---|---|---|---|
| hit rate, dhrystone | — | **≥ 88 %** | 77.48 % | **MISS** |
| hit rate, raw disk read | — | **≥ 85 %** | 67.74 % | **MISS** |
| dhrystone, cadence 1, lean | 4442 (BLK off) / **4405 (BLK on)** | **≥ 5000** | **5432.0** (+23.31 %) | **HIT** |
| `FETCHOP + FETCHEX`, next stage profile | 28.65 % | **< 18 %** | 10.48–12.64 % | **HIT** |
| the BLK tax | −0.83 % lean, −1.9 % prof | **recovered** | **reversed**: BLK worth +1.35 % with the cache on, −2.17 % without | **HIT** |

Three hits, two misses, and **the two misses pointed at the same fixable thing** — invalidation
re-warm, which rung 1b addressed and rung 1c mostly gave back. The diagnosis was right; the
misses were re-warm, and removing them did not pay. Two further results the pre-registration
did not anticipate, both worth carrying forward:

- **The switch is not a free fallback.** The lean OFF arm ran 3837.0 against rung 0's lean
  reference of 4405.3 — **12.90 % slower** — because part of rung 1 is paid on every fetch
  whether the cache is on or not (`uae_mmu040_get_iword` grew 136 → 152 *halfwords* — 68 → 76
  ARM instructions — to carry the extension-word window, on a path that runs ~1.6× per guest
  instruction; see the unit correction above). So **+23.31 %**
  (ship-this-build vs ship-the-last-build) is the number that decides the rung; the +41.57 %
  same-boot figure overstates it, because its baseline already carries that overhead. "Switch
  it off if it misbehaves" costs ~13 %: it is an A/B instrument, not a production fallback.
- **The same-boot A/B earned its pre-registration twice** — it refuted a false 9 % disk
  "regression" (cross-session drift; the cache actually helps disk by +2.74 %) and exposed the
  finding above. Take every A/B same-boot.

The dhrystone bar is a 33 % reclaim of rung 1's 28.65 % — below the map's 50 % estimate and
above the noise (within-arm spread on this rig is 0.03 %).

**The BLK-tax row is the one this design adds to the map's three**, and it is the reason the
idiom verdict is in the entry at all. The A/B is: cache ON + BLK ON against 7e7e1c1's measured
BLK-on figure of **4405.3**, same rig, same cadence, lean image. If dhrystone clears 4442 (the
BLK-**off** figure) then the recognizer's per-dispatch cost has been paid off by the cache, and
rung 0 is finally free on workloads that do not use it.

**A high hit rate with a low speedup is the pre-registered failure mode.** If the hit rates
clear their bars and dhrystone does not clear 5000, the next thing to read is the hit path's
instruction count above, not the hit rate — the cache is working and the hit path is too
expensive, and the fix is in the lookup, not the geometry.

## Verdict protocol

0. **★ FIRST, before anything else: the same-boot boot-window A/B.** Send `DOPC` on the serial
   the moment `Emulation mode 4` appears, so one power-up yields a cache-on and a cache-off boot
   window. **The window is as short as 18 s on the lean image** — measured, not estimated (this
   document previously said ~45 s, which is the *prof* image's figure and is not a floor). So
   the arm must do the banner detect **and** the console write inside **one** ssh at ~0.2 s; a
   host-side poll pays a round trip per probe and will miss it. Sending earlier is not safer:
   at the boot menu `D`, `O`, `P`, `C` are menu keys and `C` catches the autoboot countdown.
   **Take a repeat per arm** — the 2026-08-23 prof pair alone would have read as a 4 % cache
   win, and the third boot showed the ON arm's own spread was 4 s. Confirm the arm in the log
   (`DECODED OP CACHE OFF` before `68040 MMU enabled`); the echo is the ground truth, never the
   intent. *(This row is settled for rung 1b — see the boot A/B above — but the method stands
   for every rung that touches the boot path.)*
1. Flash `BOOT-prof.BIN`. `PROF 1000`, `PROFB`, run the workload, `PROFD`. Read
   `DOPC_HIT`/`DOPC_MISS` **before** any wall-clock number: if the cache is not hitting, the
   timings are measuring something else. Read the **`IV_*` attribution** beside them — which
   cause dominates decides which narrowing could possibly have helped — and the firmware's own
   `misses per invalidation` and `narrowed at the call site` rows. **`IV_*` before wall-clock is
   not a suggestion**: it is what said, before any timing existed, that rung 1b's page filter
   was aimed at a 6–9 % cause.
2. `DOPC` to OFF and repeat, **with `PROFZ` first**. `DOPC_HIT`/`MISS`/`EXT` **and**
   `DOPC_INVAL` must be exactly zero **in the zeroed window**; the `IV_*` counters are
   **expected to move** (see "The counters"), and that arm is a free measurement of the guest's
   own invalidation rate. An OFF arm read without `PROFZ` legitimately carries the pre-arm
   warm-up (the counters accumulate from power-on and the console cannot fire before core1
   starts — 2.49 M dispatches, 1.5 % of a boot, on the 2026-08-23 session). **That is not the
   switch failing to switch; it is the window being wrong.** Zero it and re-take.
3. Sweep the four states — `DOPC` × `BLK` — on the same boot. The **cache-on/BLK-on vs
   cache-off/BLK-on** pair is the rung-1 verdict; **cache-on/BLK-on vs cache-on/BLK-off** is
   what says whether the idiom verdict paid off the recognizer. Note that the switch is **not a
   free fallback**: rung 1's lean OFF arm ran 12.90 % slower than the pre-rung-1 build, because
   part of the rung is paid on every fetch whether the cache is on or not. **And any figure
   quoted to justify a change must be measured in the posture that change will ship in** —
   rung 1b's short-circuit was justified on a BLK-**off** flush storm that BLK-on had already
   eliminated.
4. Flash `BOOT-lean.BIN` for the timings — boot to login, dhrystone, `dd` from `ced0s1`. Every
   A/B **same-boot**: rung 1's session caught a false 9 % disk "regression" that way, and rung
   1b's same-boot ratio is what showed +7.08 pp of hit rate buying −1.05 pp of speedup.
   `AMIX_CMD_TIMEOUT=300` for the both-off arm.

   **Within-arm re-run thresholds, corrected by measurement:**

   | row | threshold | observed |
   |---|---|---|
   | dhrystone | **0.5 %** | 0.15 % (1b), 0.03 % (rung 1) — the threshold is right |
   | **raw disk kB/s** | **3 %** | 2.87 % (1b), 3.67 % (rung 1) — **0.5 % was mis-specified** |

   Raw-disk throughput on this rig simply does not reproduce to 0.5 %; a 0.5 % threshold demands
   a re-run on every single disk arm and then reports the survivor, which is worse than not
   having one. **Disk conclusions rest on the same-boot ratio, not on absolutes** — 1b's
   absolutes fell (1002.7 → 946.6) while the cache's worth on disk *doubled* (+2.74 % → +5.40 %).
5. Re-take the stage profile last and check `FETCHOP + FETCHEX`. **Compare raw against raw** —
   it is the only like-for-like measure across rungs. The `s` lines carry per-bucket spans, so
   corrected shares are numbers rather than intervals; sweep them across the boot line's
   `[marginal, in-bucket]` price bracket and report the range. See `profiler.md` — rung 1's
   `2 × IFETCH_CALLS` bracket method is **refuted** and must not be re-used.

## What is not covered, stated plainly

- **Second and later extension words**, and everything that goes through `get_ilong`. One slot
  per entry is what 16 bytes buys; the honest remainder is visible as `DOPC_EXT` being smaller
  than `FETCH` was.
- **Invalidation is whole-cache, and after rung 1c it is also unconditional again** for every
  cause. The only narrowing left in the firmware is the CACR compare, which happens before this
  cache is reached at all. A `URP` write on every context switch still costs the whole
  supervisor working set as well as the user one.

  The next narrowings, in the order the evidence supports — **and every one of them now has to
  clear a bar the last attempt did not**:

  > **The rung-1c bar.** A correct narrowing is not enough; rung 1b's were correct. The misses
  > this cache re-warms after a flush are *cheap* misses — still hot in `ipagecache` (91.61 %)
  > and in the ARM's caches. **So a narrowing must not charge the fill path or the hit path**,
  > because those run per miss and per dispatch while the saving is worth a fraction of a miss.
  > Rung 1b's arms charged the fill path and lost 2.32 %. The CACR arm charged one call site
  > and won. Pre-register the *cost site*, not just the yield.

  - **A physical index**, which is what the DMA arm and `CPUSHL` would need — and `IV_FLUSH` at
    **46.63–63.83 %** plus `IV_DMA` at **8.45 %** on raw disk say it is the only funded target
    left. It is the only way past the namespace wall. **But it costs a translation on the fill
    path**, which is precisely the cost class that just failed, so it is its own design round
    with its own pre-registration and the fill-path price measured rather than assumed.
  - **A separate generation per address space**, which the tag already distinguishes. Rung 1
    named this as the obvious first move; rung 1b did not take it, because a per-address-space
    generation cannot be read from a per-**set** stamp — the four ways of one set can hold both
    address spaces — so it means a per-entry stamp and three more compares on the hit path,
    which is the exact cost this rung's pre-registered failure mode is about. `IV_ROOT` at
    1.32–3.27 % does not fund that.
- **No config-file key.** The console covers A/B on a live guest, which is what the verdict
  needs.
- **The `MMU_ICACHE` path** (`cpummu.h`, compiled out at `MMU_ICACHE 0`) is untouched. If it is
  ever switched on it becomes a second, independent instruction cache with its own coherence
  story, and `mmu_flush_cache()` already covers both.
