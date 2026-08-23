# The accessor fast path — CYBERSTORM C2 rung 2

**What it is.** The 68040 data accessors (`uae_mmu040_get_long` and its five siblings) resolve
a guest data access to a host address inline, with no calls at all, and perform the access
directly when it lands in a plain-RAM bank. Anything else falls through to the original,
unmodified accessor.

**Why.** C2 attack map v3.1 §25.1 measured a guest data access at **127.6 corrected ARM cycles
for a read and 142.1 for a write** — `READ` + `WRITE` = **19.59–20.35 %** of the interpreter at
cadence 1 and **23.12–24.13 %** in the standing posture, the largest attackable group on its
ladder — at a `dpagecache` hit rate of **99.21 % / 99.35 %**. Its conclusion:

> At a 99.2–99.4 % tier-0 page-cache hit rate, essentially every guest access is already being
> served without calling `mmu_translate`, and it still costs ~134 ARM cycles. **The cost is
> therefore not translation; it is the scaffolding around a hit.**

---

## The measurement that redirected the design

The map's model of this bucket was *a page-cache hit plus scaffolding*. Before writing anything,
the same capture was asked whether the page cache is even reached:

| | `19-STAGE-c1-profd.txt` |
|---|---|
| `READ` — data reads entering the accessors | **39 738 605** |
| `DPAGE_RHIT` + `DPAGE_RMISS` — accesses that reached the page cache | **27 760 860** |

**A misaligned read *splits* into two or four sub-accesses that each probe the page cache**, so
the page-cache count should *exceed* the read count. It is **30 % below** it. The only way out
of the accessor before the page cache is a TTR match — and the AMIX-040 kernel's own
`pstart040.s` says why:

```
ITT0 = 0x003fc000   0–1 GB code, both privilege levels
DTT0 = 0x003fc060   0–1 GB data, both privilege levels, cache-inhibited
DTT1 = 0x807fa060   0x80000000+, SUPERVISOR ONLY (user VAs live up there and must
                    translate through the URP tables)
```

So **every supervisor data access is transparently translated** — DTT0 below 1 GB, DTT1 above
2 GB — and never touches the page cache at all, while user accesses fall through both windows to
it. `mmu_ttr_enabled` is true for the whole run, so `mmu_get_long()`'s guard calls
**`mmu_match_ttr()` out of line on 100 % of data accesses**.

Counted from the generated ARM, that call is **~39 instructions** on the supervisor arm and
**~48** on the user arm (where it walks both windows and matches neither). For a **write** it is
wrapped in a second out-of-line call, `mmu_match_ttr_write()`, and costs **~55 / ~69**.

> **That is the largest single piece of the scaffolding, it is paid on every access, and it was
> not in the map's model of the bucket.** It is also the mechanism behind the map's own
> read-vs-write gap: 142.1 against 127.6 cycles, and the extra wrapper is on the write side only.

The full chain a hit paid, before:

```
uae_mmu040_get_long()          an out-of-line call (~920 call sites into it from cpuemu_31)
  mmu_match_ttr()              ANOTHER out-of-line call, on EVERY data access
    mmu_do_match_ttr(dtt0)     and another
    mmu_do_match_ttr(dtt1)     and another, when dtt0 does not match
  the dpagecache probe         reached only if no TTR matched
  x_phys_get_long()            an INDIRECT call through a function pointer
    phys_get_long()            a branch
      memory_get_long()        an out-of-line call: an indexed load out of the 65 536-entry
                               (256 KB) mem_banks[] table, a load of the addrbank's direct
                               pointer, and only then the guest load itself
```

---

## The design: one derived table, no new cache

A 68040 TTR match depends on the address **only through its top 8 bits** —
`MMU_TTR_LOGICAL_BASE` is `0xff000000` — plus the privilege level and the register's own
enable / S-field / write-protect / cache-mode bits. So the entire trichotomy is a pure function
of `(super, addr >> 24)` and four registers, and it fits in a **2 × 256 table of 16-bit
verdicts per direction**: 2 KB, one load, replacing the call chain.

| verdict | meaning |
|---|---|
| `SLOW` (0) | run the unmodified accessor — no claim is made |
| `XPAR` (1) | a TTR matched: the physical address *is* the logical one; the cache state is in the entry's high byte |
| `PAGE` (2) | no TTR matched and the MMU is on: consult the existing page cache |

**This is not a new cache.** It holds no translation, no physical address and no guest data; it
is a memoized truth table over four control registers, in the same class as `mmu_ttr_enabled`
itself — which is derived from the same four registers in the same function. It cannot go stale
in any way `mmu_ttr_enabled` cannot, because it is rebuilt at exactly the sites that recompute
those flags:

| site | why it is the complete set |
|---|---|
| `mmu_tt_modified()` | every `MOVEC` to ITT0/ITT1/DTT0/DTT1 calls it, and so does CPU reset |
| `mmu_set_tc()` | the **only** writer of `regs.mmu_enabled`, and of the page geometry |

The page-cache arm then uses the **existing** `atc_data_cache_read/write` with the **existing**
index and tag arithmetic — byte for byte the expression in `cpummu.h` — so `DPAGE_RHIT` /
`DPAGE_WHIT` count exactly what they counted before. The map's pre-registration requires the hit
rates not to move; **sharing the tag arithmetic is how that is guaranteed rather than hoped for**,
and the harness asserts it on every corpus case.

> **One pre-existing hole was found and closed on the way.** `restore_cpu()` writes all four TT
> registers and never called `mmu_tt_modified()`, so a savestate restore left `mmu_ttr_enabled` /
> `_ins` / `_data` describing the *previous* machine. That is a defect of the accessors' own TTR
> guard, not of this rung; savestates are not used on the board, which is why nothing noticed.

### What is deliberately *not* cached

`baseaddr_direct_r` / `_w` is consulted **live** out of `mem_banks[]` on every access, exactly as
`memory_get_long()` consults it. Caching it per page-cache entry would save one dependent load
and would owe `map_banks()` an invalidation hook. **It is not worth a coherence obligation**, and
the consequence is that a device bank declines and the original path issues the access with its
side effects intact.

---

## The fallback is the original code, unmodified

Every arm that is not *"transparent, or a page-cache hit, into a direct RAM bank, from an aligned
address"* returns 0 and the caller runs `mmu_get_long()` / `mmu_put_long()` verbatim. Miss paths,
fault paths, the write-back contract, `mmu_bus_error`'s frame, the M-bit re-walk, `TTR_NO_WRITE`,
the `TCR_DWO` bus error, device banks: **none of them are reimplemented, so none of them can
drift.** The fast path either answers exactly what the original would have answered, or it does
not answer.

Two properties worth stating outright:

* **It never fast-paths across a page boundary.** The caller tests `is_unaligned_bus()` *first*,
  so every access that reaches the resolver is naturally aligned — and a naturally aligned 1-, 2-
  or 4-byte access cannot cross a 4 KB or 8 KB page, nor a 64 KB bank window. The map's hard rule
  is satisfied **structurally**, not by a check that could be wrong. (This repository has paid for
  the general lesson once: the cdfs page-crossing DMA that clipped an unrelated frame and stayed
  invisible until it ate an ELF header.)
* **It never fast-paths a device.** See above.

### The switch is the reference arm — and it is *not* free

`z3660_dfast_arm(0)` rebuilds the tables all-`SLOW`, so every access falls through to the original
path, and it gives the host harness a genuine reference to run the equivalence corpus against.

> **CORRECTION (2026-08-23 metal, finding F2).** This paragraph used to say the disarm *"costs
> nothing on the hot path — the disarm lives in the table the hot path was going to read
> anyway"*. **That is true of the ARMED table and false of the disarmed one**, and metal
> measured the difference: with every verdict `SLOW`, an access still pays the table load and
> the branch and *then* runs the unmodified accessor.
>
> ```
> lean c4, DFAST OFF (this build)        6290 / 6299   mean 6294.5
> lean c4, pre-rung-2 build (562c721)    6539 / 6539   mean 6539.0   ->  -3.74 %
> ```
>
> Three earlier sessions bracket the reference at 6415–6539, so 6294.5 is below all six rows
> and the sign is not in doubt. **The disarmed arm is about 3.7 % slower than the machine
> before this rung existed** — which is what makes the same-boot A/B overstate the shipping
> gain, below.
>
> What this does *not* weaken is the switch's purpose. Map §23.1's point was that a *build*
> difference cannot be defended by a console switch; the switch here defends **engagement and
> reversibility** (return leg 4022 against arm A's 4012 mean, 0.25 %), which it does exactly.
> It is the *magnitude* that needs a cross-build reference — and the session took one.

On metal it is the serial console's **`DFAST`** (or `ACCESSOR FAST PATH`), carried across the
cores in `SHARED.dfast_on` at `0xFFFF00EC`, resolved by `z3660_dfast_resolve()` — the single rule
both cores compile, exactly like `BLK` and `DOPC`. **Unset means ON**, so a core0 built before the
field existed still runs the shipped behaviour rather than silently disarming the rung. Nothing is
invalidated on the way through: the fast path holds no state of its own.

> **That makes a `PROFZ`-fenced same-boot A/B of this rung possible without a second flash** —
> which matters because map §23.1 established that a *build* difference cannot be defended by a
> console switch and needs a two-flash A/B. This rung is not a build difference; it is a switch,
> deliberately, so its own scoring does not inherit that problem.

---

## The hit path, counted

`arm-none-eabi-objdump -d` of the **lean** `Z3660_emu.elf`, counted along the actual path, not
estimated. The two arms are separated because the AMIX-040 register set puts supervisor accesses
through `XPAR` and user accesses through `PAGE`.

| path | before | after | ratio |
|---|---|---|---|
| **read, `PAGE` arm** (no TTR match, page-cache hit, direct bank) | **110** | **48** | **2.29×** |
| **read, `XPAR` arm** (TTR match, direct bank) | **76** | **33** | **2.30×** |
| **write, `PAGE` arm** | **138** | **51** | **2.71×** |
| **write, `XPAR` arm** | **97** | **33** | **2.94×** |
| read, weighted 24 % `XPAR` / 76 % `PAGE` | **101.8** | **44.4** | **2.29×** |
| write, same weighting | **128.1** | **46.7** | **2.74×** |

> **CORRECTION (2026-08-23 metal, finding F5).** The two weighted rows were originally computed
> at **30 % `XPAR` / 70 % `PAGE`** (read 99.8 → 43.5, write 125.7 → 45.6), inferred from how far
> short of `READ` the page-cache counters fell in the s19 capture. `DFAST_XPAR` now measures the
> split directly, and on the 2026-08-23 capture it is **22.63 % (c1) / 24.07 % (c4)** — so the
> rows above are re-derived at that session's **24/76**.
>
> Both numbers go **up**, and the ratio barely moves, because the shift is *away* from the
> cheaper arm: `PAGE` costs more than `XPAR` both before and after, so a smaller `XPAR` share
> means more of the expensive arm on both sides of the comparison.
>
> **The weighting is not a constant and must not be re-pinned to 24/76 either.** It is a
> property of the window's supervisor/user mix: supervisor data goes through DTT0/DTT1 (`XPAR`)
> and user data falls to the page cache (`PAGE`), and the supervisor *instruction* share moved
> from 61.63 % (s19) to 52.02 / 53.24 % between captures — which is the whole of the change.
> The counter makes reading it per capture free; the honest statement is *"read `DFAST_XPAR`
> off the capture you are interpreting"*.
>
> The instrument validated on its own data, which is what makes the counter trustworthy: the
> original *inference* method, re-run on the 2026-08-23 capture, gives 21.82 % (c1) / 23.27 %
> (c4) against the directly measured 22.43 / 23.86 % — **the two methods agree to 0.6 pp within
> a capture** and disagree only between captures.

**Calls removed from the hit path: all of them.** Before, a read paid an out-of-line
`mmu_match_ttr` (itself calling `mmu_do_match_ttr` once or twice), an indirect call through
`x_phys_get_long`, a branch through `phys_get_long`, and an out-of-line `memory_get_long`. After,
the hit path contains **no `bl`, no `blx` and no indirect branch**.

### Against the measured cycles

The map measured **127.6 cyc/read** against the ~99.8 instructions counted here — **IPC ≈ 0.78**,
and unlike the decoded-op hit path (which measures at IPC ≈ 1) this one has real stall sources: a
call chain, an indirect call, and two dependent loads out of a 256 KB table. **That is the
headroom the rung is claiming**, and it is why the map's pre-registered failure mode —
*"the accessors get faster and dhrystone does not move, because the removed scaffolding was
already overlapping with guest-DRAM latency"* — is the one to watch.

### Footprint

| symbol (lean) | before | after |
|---|---|---|
| `m68k_run_mmu040` | 572 instr | **572 instr — unchanged** |
| `uae_mmu040_get_long` | 77 instr | 124 instr |
| `uae_mmu040_get_word` | 77 | 122 |
| `uae_mmu040_get_byte` | 71 | 115 |
| `uae_mmu040_put_long` | 85 | 133 |
| `uae_mmu040_put_word` | 83 | 131 |
| `uae_mmu040_put_byte` | 79 | 126 |
| new `.bss` | — | 2 KB (`z3660_dfv_r` + `z3660_dfv_w`) |

**The run loop did not grow at all**, which is the specific I-cache regression the map warned
about. The growth is confined to the ~10 out-of-line accessor copies (one set per translation
unit), because the resolver is inlined *there* and not at the ~3000 handler call sites.

Whole-image, lean, measured by building the previous commit and this one:

| | before (`55ee6c2`) | after |
|---|---|---|
| core1 `Z3660_emu.elf` `.text` | 6 450 836 B | **6 455 628 B** (+4 792 B = **+1 198 ARM instructions**) |
| `BOOT-lean.BIN` | 12 778 804 B | 12 795 204 B |

`+1 198` instructions over a ~1.6 M-instruction image is **+0.07 %**. (The boot image's +16 400 B
is almost entirely core0's `DFAST` console command and the `mkbootimage` alignment around it, not
core1.) The retired "keep BOOT.BIN under ~12 MB" rule is not in play; the number that mattered was
the run loop's, and it is unmoved.

> **The `always_inline` is the rung, not decoration.** `ALWAYS_INLINE` in this tree is bare
> `__inline` — a hint — and at the `-Os` the firmware builds with, **gcc declined it**: the first
> build of this rung emitted the resolver as an out-of-line `bl` with both results returned
> through the stack, i.e. it *added* a call to the path whose entire purpose is removing calls.
> Measured, not assumed; `z3660_dopc_find()` carries the same note for the same reason.

---

## The guard: state equivalence, not spot checks

Host harness, `Z3660_emu/test/host`. Two tests, and the reference arm is the *disarmed* fast path
— which is the original accessor text, not a second implementation.

**`test_dfast_verdict_table`** replays the table exhaustively. For twelve register
configurations × MMU on/off × both privilege levels × all 256 top bytes — **12 288 points** — the
table's verdict and installed `mmu_cache_state` must equal what the live `mmu_match_ttr()` /
`mmu_do_match_ttr()` produce. 512 points per configuration is small enough to be exhaustive, and
that is the whole argument for indexing the table by the top byte in the first place. The
configurations include the AMIX-040 registers by name, both S-field polarities, write-protect,
both cache modes, overlapping windows (so *dtt0 first* is exercised rather than assumed), and
dtt0-off-dtt1-on.

**`test_dfast_state_equivalence`** runs a corpus of accesses twice — disarmed, then armed — and
demands that **the returned value, the fault outcome, the fault address, the guest PC, the
write-back slots, `mmu_ssw`, `mmu_cache_state`, the destination memory, all sixteen data and
address registers, and the four page-cache counters** are identical. Cases:

| | |
|---|---|
| ordinary hits | every size, both arms, supervisor and user; first and last aligned access **in** a page; the same VA in both privilege states (the tag carries `regs.s`) |
| misalignment | odd, odd-word, word and long, read and write — the split path must never be fast-pathed |
| **page crossing** | a misaligned access straddling into an **unmapped** page, read and write: same fault address, same guest PC, because the 68040 restarts the instruction |
| faults | unmapped page; write-protected page (**read serves, write faults**); supervisor-only page from user state |
| not plain RAM | the same page tables and the same cache entries, only the bank changes — and the declined access is asserted to have reached the device's own put function |
| transparent | DTT0 read/write/byte; misaligned inside a TTR window; write-protected TTR; supervisor-only TTR from both privilege levels |
| invalidation | a remap + flush, and a single-page `PFLUSH`, must both be honoured — the fast path must not answer from a retired entry |
| MMU off | declines, both directions |
| **the whole machine** | seven real 68k instructions through the run loop's own dispatch, with their data in the identity window — a write, a read-back of that write, and a second write — run armed and disarmed and compared on all sixteen registers, PC, SR, fault outcome and memory. This is what the direct calls above cannot cover: the accessors are now much larger and inline a register-allocating resolver, and they are reached from ~3000 generated handler sites, so a clobber the harness survives could still eat a handler's operand |
| the reference itself | disarmed ⇒ every verdict in both tables is `SLOW`, and a rebuild while disarmed stays `SLOW` |

> The interpreted case runs its sequence **twice and measures the second pass**. The first is a
> cold cache — a write to a page whose *write*-cache entry does not exist yet is a miss, the fast
> path declines, and the original path fills it, which is exactly **one** serve out of three
> accesses. A running guest is warm; measuring the cold pass would understate engagement and hide
> a warm-path regression behind it. Warm, all three accesses are served, and that is the
> assertion.

**Engagement is pinned on every case**, because *"the two paths agree"* is trivially true when
the fast path silently never fired — the failure a state-equivalence corpus is blindest to. Every
case declares `MUST` serve or `NEVER` serve and a counter checks it. Half the corpus is decline
cases; without the engagement check they would all pass against a fast path that was never
compiled in.

> **The corpus earned its keep on its first run.** It failed twice, both times on the
> page-cache counters, both times a real defect: (1) a decline at the bank test had already
> counted a `DPAGE_RHIT`, and the original path then counted a second one for the same access —
> so a device access double-counted the hit rate the map's pre-registration says must not move;
> (2) the `XPAR` arm counted a page-cache hit that the original path never performs at all. Both
> are fixed by counting **below every decline** and **only on the `PAGE` arm**. Neither would
> have been caught by comparing returned values.

### The identity window

The harness deliberately kept every bank non-direct (decision #6: a 64-bit host cannot honour
"the guest's physical address *is* the host address" for a malloc'd buffer). That would have made
the fast path **decline in every test**. So one window now gets the board's model for real: 1 MB
`mmap`'d at a fixed host address equal to the guest address it serves, `MAP_FIXED_NOREPLACE` so
a collision fails loudly rather than clobbering, and a bank over it with `baseaddr_direct_*` set —
plus a non-direct twin over the same storage for the device case. If the mapping cannot be made,
`harness_direct_ok()` is 0 and the corpus **fails** rather than silently dropping its coverage.

`memory_get_long()` and friends now call the same `direct_get_*` / `direct_put_*` inlines the
fast path uses (`memory.h`), so there is exactly one copy of the fork's identity-mapping
assumption in the tree.

### Totals

| variant | before | after |
|---|---|---|
| default | 1796 | **3181** |
| lean | 1792 | **3177** |
| profiling | 1943 | **3379** |

(The other three harness binaries — SCSI CDB, boot-mode and AMIX-RAM — are unaffected at
34 / 75 / 81.)

---

## What it does *not* take, and what each is worth

Priced, so the next round does not have to re-derive them:

* **A per-bank "is direct" bitmap.** The `mem_banks[]` probe is two dependent loads out of a
  256 KB table. An 8 KB bitmap indexed by `addr >> 16` would be one load out of a structure 32×
  denser. Worth ~1 instruction and a better D-cache profile — **but it caches a bank decision and
  owes `map_banks()` a hook**, and the kernel's live bank working set (~2 048 entries ≈ 8 KB)
  already fits in L1D. Not taken: a coherence obligation for a small win.
* **16-byte `struct mmufastcache` entries.** The index is `idx2 * 12`, which gcc emits as
  `mov #12` + `mul`. A 16-byte entry makes it a shift: −1 instruction and ~2 cycles of latency off
  the critical path, at 3 KB → 4 KB per direction. Cheap and safe; not taken because it changes a
  shared structure for a measurable-but-small gain and this rung is already large.
* **Gathering `mmu_pagemaski` / `mmu_pageshift1m` / `mmu_pagemask` into one struct.** Each global
  costs a pc-literal load plus a load; one base pointer would serve all three. Worth ~4
  instructions on the `PAGE` arm. Not taken: it duplicates state that the fallback path also
  reads, for less than 10 %.
* **The instruction side. TAKEN — see `docs/ifetch-fastpath.md` (rung 3).**
  `mmu_get_iword()` / `mmu_get_ilong()` have the *same* out-of-line TTR call, and ITT0 covers
  0–1 GB, so **87 % of instruction fetches (53.3 M of 61.3 M) are TTR-matched and pay it**. That
  was the next rung on the map's ladder and this finding was the largest single input to it.
  **Deliberately not taken *here***: moving the fetch buckets in the same build would have
  destroyed the like-for-like check this rung's pre-registration depends on. That constraint is
  discharged — the accessor comparison above is banked — and rung 3 took it in the next commit,
  behind its own switch so the two can still be scored apart.

---

## Pre-registered, from the map — judge against these

* **dhry cadence 4 ≥ 6900** (a ~22 % reclaim, below the 40 % estimate and far above the rig's
  0.00–0.32 % within-arm spread);
* the re-taken stage profile must show **`READ` + `WRITE` below 17 %** at cadence 4 — the check
  that the time went away from where the profile said it was, not merely that a wall clock moved;
* **`DOPCFIND` and the `dpagecache` hit rates must not move** — if the hit rates change, the
  change altered *what* is being cached and the comparison is not like-for-like.

**The pre-registered failure mode:** *the accessors get faster and dhrystone does not move,
because the removed scaffolding was already overlapping with guest-DRAM latency.* If that
happens, the next thing to read is `DFAST_HIT` — the fast path's coverage of `READ` + `WRITE`. A
wall-clock win with low coverage moved something else.

---

## Measured — 2026-08-23, A4000 + Z3660 metal

Every row pre-registered before power-on. Full scorecard and findings in the session's
`MATRIX.md` / `NOTES.txt`; what belongs here is the judgment on this document's own claims.

| pre-registered | measured | |
|---|---|---|
| dhry c4 lean **≥ 6900** | **8185** (8185 / 8184 / 8185, within-arm spread 0.01 %) | **HIT** |
| `READ`+`WRITE` **< 17 %** at c4 | **14.58 – 15.87 %** (from 23.12 – 24.13 %) | **HIT** |
| dpagecache hit rates **unmoved** | read 99.19 % (was 99.20), write 99.33 / 99.34 (was 99.35 / 99.30) | **HIT** — ≤ 0.04 pp |
| `DFAST` coverage 98.0 – 99.5 % | **99.11 %** (c1), **99.14 %** (c4) | **HIT** |
| `DFAST_XPAR` share 30 – 33 % | **22.63 / 24.07 %** | **MISS** — see the F5 correction above |
| `DOPCFIND` **absolute** cost unmoved (±3 %) | **+15.5 %** per dispatch | **MISS** — open item below |
| the named failure mode | did not occur, and could not have hidden: dhry +25 %, `READ`+`WRITE` −8.4 pp and coverage 99.1 % all moved together | **clear** |

> ### **The shipping number is +25.17 %. It is not +30.03 %.**
>
> ```
> cross-build   lean c4   8184.7 vs the pre-rung-2 build's 6539.0   = +25.17 %   <- SHIPPING
> same-boot     lean c4   8184.7 vs DFAST OFF's        6294.5       = +30.03 %
> cross-build   lean c1                                            = +19.43 %
> same-boot     prof c1                                            = +15.77 %
> ```
>
> Both are honest and they answer different questions — *"what does the switch do on this
> boot"* versus *"what did shipping this rung buy"* — and the gap is **exactly** the disarm
> penalty from finding F2:
>
> ```
> 1.2517 (shipping)  x  1.0388 (disarm penalty)  =  1.3003  =  the same-boot ratio, to 4 digits
> ```
>
> **Quote +25.17 %.** The same-boot switch overstates the shipping gain by ~4.9 pp, and any
> future rung with a console switch inherits the same arithmetic — rung 3 pre-registers its own
> version of it from the disassembly rather than discovering it afterwards.

The pre-registered band was **+8.5 – 8.9 %**; every arm overshot it. Even this document's own
instruction counts predicted more than the band did (~13 pp of the profile removed, ≈ +15 % at
constant IPC) — **and the measured 25 % is above even that.** The rung did better than an
instruction count predicts, which means the removed scaffolding was *not* overlapping with DRAM
latency: it was on the critical path, and removing an indirect call, two dependent loads out of
a 256 KB table and a call chain that fences the ARM's own speculation also removed stalls the
instruction count never saw. The map measured IPC 0.78 on this bucket and called that "the
headroom the rung is claiming"; the outcome says the headroom was larger than IPC 0.78 implied,
because the *surviving* code got faster too.

**That is the mirror image of the pre-registered failure mode, and it is the lesson to carry
forward:** the failure mode assumed overlap; the metal says the opposite. Rung 3 is projected
with that stated explicitly rather than folded into its number.

### Open item — `DOPCFIND` got ~15.5 % more expensive per dispatch (finding F6)

At the same price (57 cyc/span), cadence 4:

```
s19   4 325 817 638 / 71 116 727 spans = 60.83 cyc/dispatch
now   5 494 477 787 / 78 203 698 spans = 70.26 cyc/dispatch      -> +15.5 %
```

> **Both halves of this comparison are now weaker than they read, and the rung-3 session
> found each.** *First*, **57 is a price neither capture measured**: it was s19's own
> marginal figure, this capture's was 54, and the next session's bracket came in at
> `[39, 50]` — a 32 % move in five hours. `cyc/dispatch` has the price as a first-order
> term, so a pinned price charges two captures different amounts of instrument (see
> `profiler.md`, bias note 2c: *never pin a price a capture did not measure*). *Second*,
> **candidate (b) is DEMOTED**: rung 3 added a third always-hot table to the same L1D and
> `DOPCFIND` moved +0.39 %, where a mechanism that charged 15.5 % for the first 2 KB should
> have charged for the third. F6 stays **open** with neither candidate confirmed —
> `docs/ifetch-fastpath.md`, *Measured — 2026-08-23*, has the settlement and the same-boot
> triple (spread 0.63 %) that replaces this cross-session row.

Pre-registered at ±3 % precisely so a move could not be waved through. It moved. Two candidates,
and the 2026-08-23 session cannot separate them:

* **(a) workload mix.** The dopc hit rate moved 82.91 → 79.05 % at c1 but 78.17 → 79.88 % at c4
  — *opposite signs*, which is what a mix change looks like. A miss walks all four ways before
  giving up, so a lower hit rate raises mean cyc/dispatch. The supervisor share also moved 9 pp.
* **(b) D-cache pressure from this rung.** It adds 2 KB of `.bss` (`z3660_dfv_r` + `z3660_dfv_w`)
  that is touched on **every** data access, i.e. maximally hot, competing with a 64 KB
  decoded-op cache for L1D. The footprint table above reports `.text` growth and the run loop's
  unchanged size; **it prices no second-order cost at all**, and this is one.

The machine got 25 % faster while this bucket got 15 % slower per dispatch, so if it is (b) it
is a **paid-for trade, not a regression to reverse** — but it is exactly the sort of cost the
ladder's later rungs need to know about, and rung 3 adds 1 KB more of the same kind of table.

**How to settle it cheaply — it is a same-boot question.** `PROFB` on; `PROFZ`, dhry with
`DFAST` ON, `PROFD`; `PROFZ`, dhry with `DFAST` OFF, `PROFD`; compare `DOPCFIND` cyc/dispatch
across the switch inside one boot with the mix held as close as the window allows. Four console
commands and two dhrystone runs. With rung 3 landed, the same sequence over the `DFAST` × `IFAST`
2×2 answers it for both rungs at once — which is one of the three reasons `IFAST` is a separate
knob.

## New counters

| id | name | what |
|---|---|---|
| 43 | `MISALIGN_I` | of `MISALIGN_R`, the instruction-stream half — see below |
| 44 | `DFAST_HIT` | data accesses served by the fast path |
| 45 | `DFAST_XPAR` | ... of those, via a TTR match rather than the page cache |

---

## The misalignment asymmetry — answered

Map §25.1: *"one guest read in five is misaligned (20.71 %), against one write in 800 (0.12 %).
That asymmetry has never been explained."*

**It is a denominator defect, and the write side is the control that proves it.**

`MISALIGN_R` is incremented at **three** sites — `uae_mmu040_get_long`, `uae_mmu040_get_word`,
and **`uae_mmu040_get_ilong`, which is an instruction-stream fetch**. `READ` is incremented at
the two data sites (plus `get_byte`) only. So `MISALIGN_R / READ` divides *(data + instruction)*
misalignments by *data reads alone*.

Two things follow, and together they close it:

1. **There is no instruction-stream analogue on the write side.** `MISALIGN_W` is incremented at
   `uae_mmu040_put_long` and `put_word` and nowhere else. That is the entire reason writes read
   170× lower — not a property of the workload.
2. **The instruction stream misaligns by construction.** Guest instructions are word-aligned, so
   a 32-bit fetch at `pc+2` sits at an address ≡ 2 (mod 4) **half the time**, and
   `is_unaligned_bus(addr, 4)` calls that misaligned. `2 × 8 231 131 = 16 462 262` — 26.9 % of the
   capture's 61 288 169 instruction-stream fetch calls being longword fetches is entirely
   ordinary for SVR4 m68k code, which is full of 32-bit absolute addresses and displacements.

**The genuine misaligned-data-read rate is therefore expected to be the same order as
`MISALIGN_W`'s 0.12 %, not 20.71 %.** `MISALIGN_I` (id 43) measures the instruction-stream half
directly, so the next capture settles it by subtraction rather than by this argument:

```
genuine misaligned DATA reads  ==  MISALIGN_R − MISALIGN_I
```

`MISALIGN_R` itself is untouched, so every existing capture still reads exactly as it did.

### Measured — and map §25.1 is closed

```
                             c1          c4
  MISALIGN_R            4 653 832   5 143 469
  MISALIGN_I            4 590 681   5 080 454      (98.64 % / 98.77 % of it)
  genuine misaligned DATA reads
      = R − I              63 151      63 015
      / READ              0.1886 %    0.1828 %
  control MISALIGN_W / WRITE
                         0.1317 %    0.1275 %
  read : write asymmetry   1.43x       1.43x       (was 106x / 117x in-capture)
```

**It was a denominator defect, exactly as argued.** Subtract the instruction half and the two
sides land within 1.43× of each other. Pre-registered band for `(R − I)/READ` was 0.05 – 0.40 %;
measured 0.1886 / 0.1828 %.

The construction argument cross-checks too, with the same caveat as everything else derived from
one window: `2 × MISALIGN_I` is **17.77 % (c1) / 19.29 % (c4)** of `IFETCH_CALLS` against the
26.9 % predicted from the s19 capture — the same mix-specificity as the `XPAR` split (F5), not a
defect in the reasoning. Ordinary for SVR4 m68k code either way.

**Does a cheap fix fall out?** No, and that is the useful half of the answer. The
misaligned-longword *instruction* fetch is not a slow route taken needlessly — a 68040 instruction
stream genuinely is half-misaligned at 4-byte granularity, and the splitter it goes through
(`mmu_get_ilong_unaligned` → two `mmu_get_iword`, *not* `mmu_get_word`: it is an instruction
fetch on both halves) is what ISSUE-52 requires on metal: the board's memory layer cannot do a
misaligned physical transfer and drops bytes. **The split is load-bearing and must not be
optimised away.** Rung 3 leaves it alone for exactly that reason and prices it as a not-taken
item. What is available is serving these fetches from a wider decoded-op entry so the second
word never needs a fetch at all — which is a decoded-op-cache change, not an accessor one.
