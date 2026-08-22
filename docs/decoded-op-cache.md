# The decoded-op cache (CYBERSTORM C2, rung 1)

What it is, what it costs on the path that matters, how it stays coherent on a board where
the *other* CPU writes guest memory, and the pre-registered numbers the metal session is
supposed to judge it against — written down *before* the measurement, because a target
chosen after the result is not a target.

The measurement that motivates it is in the 040 port's `docs/C2-ATTACK-MAP.md` §9.1 and
§11.2. The implementation is `Z3660_emu/src/uae/newcpu.cpp` (the table, the lookup and the
run-loop integration, with the long comment), `Z3660_emu/src/uae/z3660_dopc.h` (the
geometry, the switch rule and the extension-word window) and one hook inside
`Z3660_emu/src/uae/include/cpummu.h`. The evidence is
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
664 → **860**, 529 → **715**; `uae_mmu040_get_iword` (lean) 136 → **152**.

## Coherence — the whole risk surface

It is asymmetric. A cache that invalidates too eagerly is merely slower; one that invalidates
too little executes instructions the guest has already overwritten, and that is a working
system that goes wrong hours later with no symptom at the point of the bug. So this one
follows the map's instruction literally: **invalidate everything on any event that could
plausibly matter, measure the hit rate, and only then narrow it.** Whole-cache invalidation is
affordable as the *default* policy because it is O(1) — one generation increment — rather than
a walk.

| event | where | why |
|---|---|---|
| `CPUSHL` / `CPUSHP` / `CPUSHA` / `CINV` | `mmu_flush_cache()` via `flush_mmu040()` | the 68040's own instruction-cache maintenance |
| `PFLUSH` (page) / `PFLUSHA` | `mmu_flush_cache()` via `mmu_flush_atc()` / `_all()` | a mapping change |
| MOVEC to `TC`, MMU reset | the same, via `mmu_set_tc()` / `mmu_reset()` | translation turned on, off or resized |
| MOVEC to `URP` / `SRP` | `newcpu_common.cpp` | an address-space change; tags are virtual |
| MOVEC to `CACR` | `newcpu_common.cpp` | the register that enables the caches this stands in for |
| `build_cpufunctbl()` | `newcpu.cpp` | entries hold **resolved handler pointers**; a mode change makes every one wrong |
| `map_banks()` | `memory.cpp` | the bank map an opcode was read through is being replaced |
| **DMA into guest RAM** | `Z3660_emu/src/main.cpp` | see below |
| the switch moving | `z3660_dopc_set()` | an off period is an unwatched period |

**`mmu_flush_cache()` is the choke point, and picking it is the design decision.** It is
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

Profiling build only, appended counter ids 24–27, reported by `PROFD`.

- **`DOPC_HIT / (DOPC_HIT + DOPC_MISS)`** — the hit rate. `HIT + MISS == INSNS` identically:
  every dispatch consults the cache exactly once, which is what makes it a fraction of
  anything.
- **`DOPC_EXT`** — extension words served out of the entry rather than fetched. The two halves
  of this rung are separable and worth separating: `FETCHOP` goes by construction on every hit,
  `FETCHEX` only for the instructions whose first extension word fits the slot. **The
  pre-registered `FETCHOP + FETCHEX < 18 %` is not reachable on the first half alone.**
- **`DOPC_INVAL`** — the price of the conservative policy, and the counter that decides whether
  narrowing is worth doing. Large relative to `DOPC_MISS` means the misses are self-inflicted
  rather than capacity, and the fix is a narrower invalidation (per-page, or a separate
  generation per address space) and not a bigger cache.

With the cache off, **none of them move at all** — the lookup returns before it can even count
a miss, which is the property the whole equivalence corpus rests on.

## PRE-REGISTERED — judge the change against these, not against what it produces

From map §11.2, and from the #1 metal verdict this change is built on top of:

| | measured before | pre-registered |
|---|---|---|
| hit rate, dhrystone | — | **≥ 88 %** |
| hit rate, raw disk read | — | **≥ 85 %** |
| dhrystone, cadence 1, lean | 4442 (BLK off) / **4405 (BLK on)** | **≥ 5000** |
| `FETCHOP + FETCHEX`, next stage profile | 28.65 % | **< 18 %** |
| the BLK tax | −0.83 % lean, −1.9 % prof | **recovered** — dhry with cache on + BLK on ≥ dhry with BLK on today |

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

1. Flash `BOOT-prof.BIN`. `PROF 1000`, `PROFB`, run the workload, `PROFD`. Read
   `DOPC_HIT`/`DOPC_MISS` **before** any wall-clock number: if the cache is not hitting, the
   timings are measuring something else. Read `DOPC_INVAL` beside them.
2. `DOPC` to OFF and repeat. All four counters must go to **exactly zero** and the rates must
   return to the pre-change figures. This is the A/B that separates the change from the flash.
3. Sweep the four states — `DOPC` × `BLK` — on the same boot. The **cache-on/BLK-on vs
   cache-off/BLK-on** pair is the rung-1 verdict; **cache-on/BLK-on vs cache-on/BLK-off** is
   what says whether the idiom verdict paid off the recognizer.
4. Flash `BOOT-lean.BIN` for the timings — boot to login, dhrystone, `dd` from `ced0s1`.
5. Re-take the stage profile last and check `FETCHOP + FETCHEX`.

## What is not covered, stated plainly

- **Second and later extension words**, and everything that goes through `get_ilong`. One slot
  per entry is what 16 bytes buys; the honest remainder is visible as `DOPC_EXT` being smaller
  than `FETCH` was.
- **Invalidation is whole-cache.** A `URP` write on every context switch costs the whole
  supervisor working set as well as the user one. That is the conservative choice the map asked
  for, and `DOPC_INVAL` is the evidence that would justify narrowing it — the obvious first
  narrowing is a separate generation per address space, which the tag already distinguishes.
- **No config-file key.** The console covers A/B on a live guest, which is what the verdict
  needs.
- **The `MMU_ICACHE` path** (`cpummu.h`, compiled out at `MMU_ICACHE 0`) is untouched. If it is
  ever switched on it becomes a second, independent instruction cache with its own coherence
  story, and `mmu_flush_cache()` already covers both.
