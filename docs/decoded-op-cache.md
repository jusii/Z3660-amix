# The decoded-op cache (CYBERSTORM C2, rungs 1 and 1b)

What it is, what it costs on the path that matters, how it stays coherent on a board where
the *other* CPU writes guest memory, and the pre-registered numbers the metal session is
supposed to judge it against — written down *before* the measurement, because a target
chosen after the result is not a target.

**Rung 1 shipped the cache and was judged on metal 2026-08-23**: dhrystone **+23.3 %**
(4405.3 → 5432.0) and `FETCHOP + FETCHEX` 28.65 % → 10.48–12.64 %, both clearing their bars —
against **both hit-rate bars missed** (dhrystone 77.48 % vs ≥ 88 %, raw disk 67.74 % vs ≥ 85 %),
with the misses diagnosed as invalidation re-warm. **Rung 1b is the narrowing that diagnosis
called for**; see "Rung 1b" below for what could be narrowed, what could not, and why.

The measurement that motivates it is in the 040 port's `docs/C2-ATTACK-MAP.md` §9.1 and
§11.2. The implementation is `Z3660_emu/src/uae/newcpu.cpp` (the table, the lookup and the
run-loop integration, with the long comment), `Z3660_emu/src/uae/z3660_dopc.h` (the
geometry, the switch rule and the extension-word window) and one hook inside
`Z3660_emu/src/uae/include/cpummu.h`. The evidence is
`Z3660_emu/test/host/harness_main.cpp` (`test_dopc_state_equivalence`,
`test_dopc_coherence`, `test_dopc_narrowing`, `test_dopc_counters`). Rung 1b adds the
page-presence filter and the narrowed entry points in the same two files, plus one call site
each in `Z3660_emu/src/uae/cpummu.cpp` and `newcpu_common.cpp`.

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
  cause. `sum(IV_*) − DOPC_INVAL` is exactly the number the narrowing skipped; the firmware
  prints that too. This is the attribution the 2026-08-23 verdict needed and could not get:
  it could establish that the misses were re-warm, but not *which* flush was causing them.

**With the cache off**, the contract is more interesting than the rung-1 text claimed, and it
is pinned in `test_dopc_counters`:

| counter | cache OFF | why |
|---|---|---|
| `DOPC_HIT` / `MISS` / `EXT` | exactly **0** | the lookup returns before it can count |
| `DOPC_INVAL` | exactly **0** | nothing is ever filled, so the empty-cache short-circuit fires on every request |
| `IV_*` | **they move** | they count what the *guest* asked for, and the guest issues `CPUSHL` and `PFLUSH` whatever this switch says |

The rung-1 sentence "**none of them move at all** … the property the whole equivalence corpus
rests on" was **false when written** — `z3660_dopc_invalidate()` had no on/off test at all
(only the *lookup* consults the switch), so `DOPC_INVAL` counted regardless, and metal read
164 311 and 2 951 874 in the two cache-off arms. Rung 1b makes `DOPC_INVAL` genuinely zero
there, and the `IV_*` block deliberately does not follow it: the OFF arm is now a free,
uncontaminated measurement of the guest's own invalidation rate, which is the denominator every
future narrowing decision needs.

## Rung 1b — the narrowing, and the namespace wall that decides what can be narrowed

Metal 2026-08-23 measured the conservative policy's price and it is the dominant term:
**274–292 misses per invalidation across three unrelated workloads.** Rung 1b narrows it. The
first thing it establishes is what *cannot* be narrowed, because that is what shapes the rest.

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

### What rung 1b actually does

1. **The empty-cache short-circuit** — applies to **every** cause, in either namespace. If
   nothing has been filled since the last invalidation, no entry is live, so the invalidation
   has nothing to do and does not move the generation. It cannot be wrong: "no entry exists" is
   not an approximation. This is the arm aimed at the measured storm — the verdict recorded
   **18× more invalidations with the block-idiom recognizer off** (2 936 789 / 2 951 874 against
   159 902), because AMIX's `CPUSHL` loop invalidates once per instruction and consecutive
   iterations dispatch nothing in between.

2. **`PFLUSH` of one page**, through a **page-presence filter**: 4096 bits keyed on a hash of
   the 4 KB logical granule, 512 bytes, set on every fill and cleared with the generation. The
   invariant is *every live entry's page bit is set*; the contrapositive — bit clear ⇒ no live
   entry in that page ⇒ the flush is provably a no-op — is what licenses the skip. A hash
   collision sets a bit for a page holding nothing, which produces a needless whole-cache
   invalidation, i.e. exactly the rung-1 behaviour. **Hash quality is a performance question and
   never a correctness one.**

3. **`MOVEC` to CACR** narrowed to writes that *change* the register, with its own cause id. On
   a 68040 the retained bits are the two cache enables and nothing else (`cacr_mask` 0x80008000
   — the 68040 has no clear-cache bits in CACR; it uses `CINV`/`CPUSH`, which arrive at
   `mmu_flush_cache()`). If neither enable moved, the cache configuration is bit-identical.

4. **Everything else stays whole-cache**: root and table changes are genuinely global and
   genuinely rare, and narrowing there would buy a rounding error at real risk.

### Why a filter and not a tag walk

A 4 KB page spans `pc[11:1]` and the set index is `pc[10:1]`, so **one page touches every one of
the 1024 sets**. "Invalidate just this page" is a full 4096-tag walk over 32 KB of tag lines —
the whole Cortex-A9 L1 D-cache — evicting the interpreter's working set on every `PFLUSH` to
save a re-warm of ~285 entries. The filter answers the same question with one load and one test.

### The hit path is untouched, and that is measured

The pre-registered failure mode for this whole rung is *"a high hit rate with a low speedup"* —
what happens when the lookup costs as much as the fetch it replaces. So the narrowing was
designed to add **nothing** to the hit path, and the lean binary says it did:

| symbol | rung 1 (lean) | rung 1b (lean) |
|---|---|---|
| `uae_mmu040_get_iword` | 152 instructions | **152** |
| `m68k_run_mmu040` | 572 instructions | **572** |

Both byte-identical. The narrowing decides **whether** an invalidation happens, never how much
of the cache it covers — so every invalidation that does happen is still the whole cache, still
one generation increment, still read by one per-set compare. The cost lands on the fill path
(two filter marks and a flag store) and on the invalidation path, both of which were already
the expensive ones. The lean image grew **8 bytes**; the 512-byte filter is BSS.

### PRE-REGISTERED for the rung-1b verdict

Expected bands, written before the metal session, per narrowing. **`IV_*` attribution is the
first thing to read** — it is what says whether each band was even addressable.

| row | rung-1 measured | rung-1b expectation | reasoning |
|---|---|---|---|
| hit rate, **dhrystone** | 77.48 % | **80–85 %** | dhrystone's flush traffic is `CPUSHL`, which the namespace wall leaves whole-cache; the gain is the short-circuit collapsing back-to-back flushes only |
| hit rate, **boot** | 69.76 % | **75–85 %** | boot is the flush-heavy, `PFLUSH`-heavy workload — the one the page filter is aimed at, and the widest band because `IV_PFLUSH`'s share is unmeasured |
| hit rate, **raw disk** | 67.74 % | **72–82 %** | as boot, less the DMA arm, which is not narrowed |
| **misses per invalidation** | 274–292 | **unchanged, 260–300** | this is a property of the flush *interval*, not of the policy; if it moves much, the diagnosis was wrong |
| **`sum(IV_*) − DOPC_INVAL`** | — | **> 30 % of requests skipped** | the narrowing's own yield, and the row that decides whether rung 1b paid |
| dhrystone, cadence 1, lean | 5432.0 | **5550–5750** | see the arithmetic below |
| `FETCHOP + FETCHEX` | 10.48–12.64 % | **9–12 %** | fewer misses means fewer real fetches; a small move, and it must not go *up* |

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

**★ The verdict session's FIRST act is the same-boot boot-window A/B** — `DOPC` on/off at the
console during boot, sending the command the moment `Emulation mode 4` appears (~45 s before
MMU-enable, since core0's debug console is live from core1 start). Rung 1's 52 s against
rung 0's 43 s is a **cross-session** comparison and §5b of that verdict showed this rig drifts
~9 % across sessions on at least one row. **That possible 21 % boot regression must be settled
before any other number is taken**, because boot is the workload rung 0 was justified on and a
boot regression would be a real trade rather than a free win.

**Bench note (not this repo):** the 2026-08-23 session lost ~7 minutes and 6 telnet logins to
`amixrun`'s `AMIX_CMD_TIMEOUT` defaulting to 60 s — too short for the both-accelerations-off arm,
where the *interactive* login path (fork/exec/page-fault/bzero) is exactly the work rung 0
accelerates. Run the sweep with `AMIX_CMD_TIMEOUT=300`, and raising the fieldkit default is an
`amix-kerntools` change.

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
re-warm, which is what rung 1b addresses. Two further results the pre-registration did not
anticipate, both worth carrying forward:

- **The switch is not a free fallback.** The lean OFF arm ran 3837.0 against rung 0's lean
  reference of 4405.3 — **12.90 % slower** — because part of rung 1 is paid on every fetch
  whether the cache is on or not (`uae_mmu040_get_iword` grew 136 → 152 to carry the
  extension-word window, on a path that runs ~1.6× per guest instruction). So **+23.31 %**
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
   the moment `Emulation mode 4` appears — ~45 s before MMU-enable, and core0's debug console is
   live from core1 start — so one power-up yields a cache-on and a cache-off boot window. Rung 1
   left a possible **52 s vs 43 s** boot regression standing on a cross-session comparison, on a
   rig shown to drift ~9 %. Settle it before any other number is taken.
1. Flash `BOOT-prof.BIN`. `PROF 1000`, `PROFB`, run the workload, `PROFD`. Read
   `DOPC_HIT`/`DOPC_MISS` **before** any wall-clock number: if the cache is not hitting, the
   timings are measuring something else. Read the **`IV_*` attribution** beside them — which
   cause dominates decides which narrowing could possibly have helped — and the firmware's own
   `misses per invalidation` and `narrowed away` rows.
2. `DOPC` to OFF and repeat. `DOPC_HIT`/`MISS`/`EXT` **and** `DOPC_INVAL` must go to exactly
   zero; the `IV_*` counters are **expected to move** (see "The counters"), and that arm is a
   free measurement of the guest's own invalidation rate. This is the A/B that separates the
   change from the flash.
3. Sweep the four states — `DOPC` × `BLK` — on the same boot. The **cache-on/BLK-on vs
   cache-off/BLK-on** pair is the rung-1 verdict; **cache-on/BLK-on vs cache-on/BLK-off** is
   what says whether the idiom verdict paid off the recognizer. Note that the switch is **not a
   free fallback**: rung 1's lean OFF arm ran 12.90 % slower than the pre-rung-1 build, because
   part of the rung is paid on every fetch whether the cache is on or not.
4. Flash `BOOT-lean.BIN` for the timings — boot to login, dhrystone, `dd` from `ced0s1`. Every
   A/B **same-boot**: rung 1's session caught a false 9 % disk "regression" that way.
   `AMIX_CMD_TIMEOUT=300` for the both-off arm.
5. Re-take the stage profile last and check `FETCHOP + FETCHEX`. The `s` lines now carry
   per-bucket spans, so `LOOP`'s corrected share is a number rather than the interval rung 1 had
   to report — sweep it across the boot line's `[marginal, in-bucket]` price bracket.

## What is not covered, stated plainly

- **Second and later extension words**, and everything that goes through `get_ilong`. One slot
  per entry is what 16 bytes buys; the honest remainder is visible as `DOPC_EXT` being smaller
  than `FETCH` was.
- **Invalidation is still whole-cache when it happens.** Rung 1b narrowed *whether* it happens,
  not its extent (see above). A `URP` write on every context switch still costs the whole
  supervisor working set as well as the user one.

  The obvious next narrowings, in the order the evidence supports:
  - **A physical index**, which is what the DMA arm and `CPUSHL` would need. It is the only way
    past the namespace wall, and it costs a translation on the fill path — measure `IV_FLUSH`
    and `IV_DMA` first, because if they are small the whole idea is unfunded.
  - **A separate generation per address space**, which the tag already distinguishes. Rung 1
    named this as the obvious first move; rung 1b did not take it, because a per-address-space
    generation cannot be read from a per-**set** stamp — the four ways of one set can hold both
    address spaces — so it means a per-entry stamp and three more compares on the hit path,
    which is the exact cost this rung's pre-registered failure mode is about. `IV_ROOT` is the
    counter that would justify paying it.
- **No config-file key.** The console covers A/B on a live guest, which is what the verdict
  needs.
- **The `MMU_ICACHE` path** (`cpummu.h`, compiled out at `MMU_ICACHE 0`) is untouched. If it is
  ever switched on it becomes a second, independent instruction cache with its own coherence
  story, and `mmu_flush_cache()` already covers both.
