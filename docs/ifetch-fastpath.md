# The instruction-fetch fast path — CYBERSTORM C2 rung 3

**What it is.** The 68040 instruction accessors (`uae_mmu040_get_iword` and
`uae_mmu040_get_ilong`) resolve a guest fetch to a host address inline, with no calls at
all, and perform the fetch directly when it lands in a plain-RAM bank. Anything else falls
through to the original, unmodified accessor.

**Why now.** Rung 2 removed the out-of-line `mmu_match_ttr()` call from the *data*
accessors and measured **+25.17 %** on dhrystone cadence 4. The instruction accessors carry
the same call — `mmu_match_ttr_ins()` — and after rung 2 landed, the fetch pair is the
largest attackable group left on the C2 ladder:

| group | c4 share, swept `[54, 59]` (2026-08-23 capture) |
|---|---|
| **`FETCHOP` + `FETCHEX`** | **18.06 – 19.98 %** ← this rung |
| `HANDLER` | 19.19 – 19.80 % (not attackable in the same sense) |
| `DOPCFIND` | 15.06 – 16.15 % |
| `READ` + `WRITE` | 14.58 – 15.87 % (was 23.12 – 24.13 % before rung 2) |

and the s19 capture priced the population: **53.3 M of 61.3 M instruction fetches — 87 % —
are ITT-matched** and pay the call, because AMIX-040's `pstart040.s` sets

```
ITT0 = 0x003fc000   0–1 GB code, BOTH privilege levels
ITT1 = disabled
```

and the kernel's text lives under 1 GB.

---

## The design: rung 2's table, aimed the other way

One more `2 × 256` table of 16-bit verdicts — `z3660_ifv`, **1 KB**, one direction because a
fetch is always a read — built by the *same* `z3660_dfast_win()` replay, from
`regs.itt0` / `regs.itt1`, at the *same* two rebuild sites (`mmu_tt_modified()`,
`mmu_set_tc()`). Sharing the replay is the point: two copies of "what does a TTR match
mean" is the divergence this fork has paid for elsewhere.

Three differences from rung 2, all consequences of what a fetch *is*:

**1. `likely()` points at `XPAR`, not `PAGE`.** The data side falls through to the page
cache about three quarters of the time; the fetch side takes the transparent arm ~87 % of
the time. That is the only branch hint in the rung and it is measured, not assumed.

**2. `TTR_NO_WRITE` is `XPAR`, not `SLOW`.** On the data side a write-protected window means
"reads transparent, writes take the original path". A fetch is a read, and
`mmu_get_iword()`'s guard tests `== TTR_NO_MATCH` — so a write-protected ITT is simply
transparent to it, with the window's own cache state installed. `W` in an ITTR is
architecturally meaningless anyway; the table replays what this emulator does, not what the
manual implies. The corpus asserts it on a live fetch, so a future copy-paste from the data
table fails a test rather than a boot.

**3. The `PAGE` arm is one entry, not 256.** `MMU_IPAGECACHE` is a single
`atc_last_ins_laddr` / `_paddr` / `_cache` triple. The tag expression is copied from
`mmu_get_iword()` character for character — including `| regs.s` unnormalised — so
`IPAGE_HIT` / `IPAGE_MISS` keep counting the same events. **Invalidation needs no hook** for
the same reason: `flush_shortcut_cache()` writes an impossible tag into
`atc_last_ins_laddr`, and the fast path *reads that variable* rather than shadowing it, so
it goes stale in the same instant the original does.

### The alignment gate, and why it is not a reroute

`uae_mmu040_get_ilong()` already tests `is_unaligned_bus(addr, 4)` above the fast path
(ISSUE-52), so everything reaching the resolver from there is aligned.
`uae_mmu040_get_iword()` has **never** had such a test — an odd instruction address is an
address error the 68040 raises elsewhere — so the fast path gates itself on `!(addr & 1)`
there. **Not to re-route the access**: the original path for an odd fetch is exactly what it
always was, and the corpus asserts that byte for byte. The gate exists so that *"an access
served by the fast path cannot cross a page or a 64 KB bank window"* stays **structural**.
A word at `page_end − 1`, served, would read its second byte out of an unrelated physical
frame — the cdfs page-crossing DMA, in a hotter loop.

### The dopc interplay is untouched

The decoded-op cache's extension-word window sits **above** the fetch bracket in
`uae_mmu040_get_iword()` and is not moved: a word served from it costs no fetch at all and
must not be charged to one. Rung 3 serves what the dopc does *not* — dopc misses, and the
extension words of a hit whose window has already been consumed.
`Z3660_PROF_ENTER_IFETCH()` and the `FETCH` counter stay exactly where they are, **above**
the fast path, so `FETCHOP`/`FETCHEX` still bracket every fetch and the profile stays
comparable across the rung. **The bucket gets cheaper; it does not get emptier.**

---

## The hit path, counted

`arm-none-eabi-objdump -d` of the **lean** `Z3660_emu.elf`, before (`d5c3d6a`) and after,
counted along the actual path — every instruction issued, predicated-not-taken ones
included.

| path | before | after | saved | ratio |
|---|---|---|---|---|
| **`iword`, `XPAR`** (ITT match, direct bank) — the 87 % arm | **79** | **37** | 42 | **2.14×** |
| **`iword`, `PAGE`** (ipagecache hit, direct bank) | **96** | **55** | 41 | **1.75×** |
| **`ilong`, `XPAR`** (aligned) | **73** | **30** | 43 | **2.43×** |
| **`ilong`, `PAGE`** (aligned) | **90** | **48** | 42 | **1.88×** |

The saving is ~42 instructions per served fetch on every arm, which is what one expects
when what is removed is the same three things each time.

**Calls removed from the hit path: all of them.** Before, a fetch paid an out-of-line
`mmu_match_ttr_ins` (itself calling `mmu_do_match_ttr` once or twice), an indirect call
through `x_phys_get_iword`, and an out-of-line `memory_get_word`. After, the served path
contains **no `bl`, no `blx`, and no indirect branch**.

> **A note on `x_phys_get_iword`.** The shipping configuration installs `phys_get_word`
> (`mmu_set_funcs`, the non-`cpu_compatible` arm), which tail-branches to
> `memory_get_word`; the glue's default is `memory_get_wordi`. **On a direct bank the two
> are the same three instructions** — both collapse to `direct_get_word(addr)` — so the
> fast path returns the same bytes whichever pointer is installed. They diverge only on
> the `wget`/`wgeti` arm, which is exactly the arm the bank test declines.

### Footprint

| symbol (lean) | before | after |
|---|---|---|
| `m68k_run_mmu040` | 2 288 B | **2 288 B — unchanged** |
| `uae_mmu040_get_iword` (×2 copies) | 304 B | 488 B |
| `uae_mmu040_get_ilong` (×1 copy) | 280 B | 448 B |
| core1 `.text` | 6 455 628 B | **6 456 572 B** (+944 B = +236 ARM instructions, +0.015 %) |
| new `.bss` | — | **1 KB** (`z3660_ifv`) |
| `BOOT-lean.BIN` | 12 795 204 B | 12 795 204 B (unchanged; `crc32 DEC21748 → 86C0BC18`) |

**The run loop did not grow at all**, which is the specific I-cache regression the map warns
about. The growth is confined to three out-of-line accessor copies, because the resolver is
inlined *there* and not at the ~3000 handler call sites. Rung 2 grew `.text` by 4 792 B for
six accessors across ~10 translation units; rung 3 costs a fifth of that for two.

---

## Pre-registered — judge against these

### The projection, and the arithmetic

The s19 capture gives the fetch population directly:

```
IFETCH_CALLS                                     61 288 169
MISALIGN_I (misaligned longword fetches)          8 231 131
  => longword fetches      = 2 x MISALIGN_I  =   16 462 262   (26.9 %)
  => word fetches                            =   44 825 907   (73.1 %)
  => fast-pathABLE shape (words + ALIGNED longwords)
                           = 44.83 M + 8.23 M =  53.06 M      (86.6 % of fetches)
```

Weighted by the 87/13 `XPAR`/`PAGE` split, over that fast-pathable population:

```
mean instructions per served-shape fetch
  before  = (44.83 x [.87x79 + .13x96] + 8.23 x [.87x73 + .13x90]) / 53.06 = 80.3
  after   = (44.83 x [.87x37 + .13x55] + 8.23 x [.87x30 + .13x48]) / 53.06 = 38.3
                                                                    -> -52.3 %
fetch bucket, by instruction count
  served-shape   53.06 M x 80.3  = 4 260 M       (76.6 % of the bucket)
  splitter       8.23 M x 158    = 1 300 M       (23.4 %, untouched)
  total                          = 5 560 M  ->  3 330 M      -> -40.1 %
```

The splitter's 158 is counted, not guessed: `mmu_get_ilong_unaligned` is 18 instructions of
frame and two calls to a `constprop` copy of `mmu_get_iword` that costs **73** on the `XPAR`
arm — the ordinary fetch path minus the accessor wrapper — and the C++ exception handling in it
is landing-pad based, so the non-throwing path pays nothing for the `TRY`.

**A 40.1 % cut of a bucket that is 18.06–19.98 % of cadence 4 removes 7.2–8.0 pp of the
profile**, which at constant IPC is

> ### **+7.8 % to +8.7 % on dhrystone cadence 4, lean, cross-build.**
> Against the shipping 8185, that is **8824 – 8898**.

### What this projection does NOT include

Rung 2's projection under-called by 1.8×–3.4×, and the verdict's finding F3 named the
mechanism. So, explicitly:

1. **Stall recovery — the big one, and it is not modelled at all.** These are *instruction*
   counts, not cycles. What rung 3 removes is two out-of-line calls plus an indirect call
   per fetch, and rung 2's measured over-delivery came from exactly that: removing a call
   chain removes stalls and speculation fences an instruction count cannot see. **If the
   same 1.68× factor applies (rung 2's ~+15 % model against its measured +25.17 %), rung 3
   lands at +13.1 % to +14.6 %.** That is stated as a *possibility with a named mechanism*,
   not as the prediction; the pre-registered band above is the instruction-count one.
2. **The splitter.** 13.4 % of fetches are misaligned longwords routed to
   `mmu_get_ilong_unaligned()`, which calls `mmu_get_iword()` — the *inline*, not the
   accessor — twice. They are **not served** and are individually the most expensive fetches
   in the profile. Priced below as a not-taken item.
3. **The 76.6/23.4 bucket split is derived, not measured.** Its ingredients are counted, but
   the profiler has no counter that separates splitter fetches from ordinary ones, so the
   ratio is arithmetic over this document's instruction counts rather than a bucket reading.
   **It is the softest number in the chain**; if the splitter is a larger share than 23.4 %,
   the projection over-states. `MISALIGN_I / FETCH` is the closest check the next capture can
   offer without a new counter.
4. **Second-order D-cache cost — zero of it is priced.** `z3660_ifv` is 1 KB more
   always-hot `.bss` beside rung 2's 2 KB, competing with a 64 KB decoded-op cache for
   L1D. The 2026-08-23 verdict measured `DOPCFIND` costing **+15.5 % per dispatch** after
   rung 2, with exactly this as one of two candidate causes (finding F6). Rung 3 can only
   push that the same way.
5. **The mix is capture-specific.** The 87 % ITT share and the 26.9 % longword-fetch share
   are s19 properties, not constants — verdict finding F5 is the standing warning against
   re-pinning such a ratio. A window with a different supervisor/user mix moves both.
6. **The dopc population.** Fetches served from the decoded-op cache's extension-word
   window never reach this path. A change in dopc hit rate changes the fetch population and
   therefore this arithmetic.

### The other pre-registrations

* **`IPAGE_HIT` / `IPAGE_MISS` must not move.** If the fetch hit rates change, the change
  altered *what* is being cached and the comparison is not like-for-like. The corpus
  asserts it per case; metal confirms it against the s19 rates.
* **`IFAST_HIT / FETCH` coverage: 80–90 %.** The fast-pathable shape is 86.6 % of fetches
  and essentially all of it should be served on a warm guest. **Coverage is the first thing
  to read if the wall clock moves**: a big win with low coverage moved something else.
* **`IFAST_XPAR / IFAST_HIT`: 85–90 %**, from ITT0's 0–1 GB both-privilege window. This is
  the rung's premise measured directly. Read it per capture; do not re-pin it (F5).
* **`DFAST_HIT` and the dpagecache rates must not move**, and `IFAST` off must not change
  them either — the two rungs are independently switchable and the harness pins that in
  both directions.

### The pre-registered failure mode

*The fetch accessors get faster and dhrystone does not move, because the removed
scaffolding was already overlapping with the guest-DRAM latency of the fetch itself.* If
that happens, read `IFAST_HIT` coverage first, and `DOPC_HIT` second — a decoded-op cache
hit rate that rose would mean fewer fetches reached this path at all.

### And the same-boot switch will overstate it — by a *predicted* amount

`IFAST` is a console switch, so the A/B is same-boot. But rung 2's finding F2 applies here
too and the disassembly prices it in advance: **with `IFAST` off, an `iword` fetch costs 98
instructions against the pre-rung-3 build's 79** (+19), because a disarmed table is still
loaded, still compared, and only then falls through to the original accessor.

```
+19 instr x 53.06 M served-shape fetches = +1 008 M instructions
  on a 5 560 M fetch bucket               = +18.1 % of the bucket
  = +3.3 .. +3.6 pp of c4                 -> the IFAST-OFF arm should sit
                                             ~3.2 .. 3.5 % BELOW the pre-rung-3 build
```

which is the same shape and nearly the same size as DFAST's **measured** −3.74 %. So:
**quote the cross-build number as the shipping gain**, take a pre-rung-3 reference row in
the same session, and expect the same-boot switch to read roughly 3 pp high. If the
IFAST-OFF arm does *not* come in ~3 % low, this model is wrong somewhere and the shipping
number is the one to trust.

---

## The switch: `IFAST`, and why it is not folded into `DFAST`

Serial console `IFAST` (or `IFETCH FAST PATH`), mailbox `SHARED.ifast_on` at `0xFFFF00F0`,
resolved by `z3660_ifast_resolve()` — the single rule both cores compile, exactly like
`BLK`, `DOPC` and `DFAST`. **Unset means ON.**

Folding it into `DFAST` would have been one field, one command and one thing to explain.
It is separate for three reasons that outrank that:

1. **A shared switch cannot price this rung.** It would A/B *(rung 2 + rung 3)* against
   neither, and rung 2's number is already banked. The question rung 3 has to answer is
   what the fetch side buys **on top of** the shipped data side.
2. **It puts the 2×2 inside one boot.** `DFAST` × `IFAST`, four arms, one `PROFZ` fence
   each — the only cheap way to see whether the two rungs *interact*, which finding F6 says
   is a live question and not a theoretical one.
3. **Bisection.** If the fetch path is ever suspected on metal, one knob turns off exactly
   the suspect and leaves the machine otherwise shipped.

---

## The guard: state equivalence, engagement, and four mutations

Host harness, `Z3660_emu/test/host`. Two tests, and the reference arm is the *disarmed*
fetch path — the original accessor text, not a second implementation.

**`test_ifast_verdict_table`** replays the table exhaustively: 11 register configurations ×
MMU on/off × both privilege levels × all 256 top bytes = **11 264 points**, each compared
against the live `mmu_match_ttr_ins()` for verdict *and* installed `mmu_cache_state`. It
also pins the two switches as independent **in both directions** — `IFAST` off must leave
`DFAST` armed and vice versa — because the rebuild builds all three tables in one pass, and
that pass is precisely where one disarm could silently take the other rung out of the
reference arm of its own A/B.

**`test_ifast_state_equivalence`** runs a corpus of fetches twice — disarmed, then armed —
and demands the returned value, fault outcome, fault address, guest PC, `mmu_ssw`,
`mmu_cache_state` **and both instruction-page-cache counters** be identical. Cases:
ordinary hits at both ends of a page and across into the next; user text and the privilege
bit in the tag; odd addresses and `2 (mod 4)` longwords (must **never** serve); unmapped
pages; a supervisor-only text page from user state; a device bank (declines, *and* the
declined fetch is asserted to have reached the bank's own get function); ITT-transparent
fetches including the write-protected window; a supervisor-only ITT from both privilege
levels; remap+flush and single-page `PFLUSH`; MMU off; the disarm itself; and **the whole
machine** — seven real 68k instructions through the run loop's own dispatch, with their
text in the identity window, run armed and disarmed and compared on all sixteen registers,
PC, SR, fault outcome and memory, with `DFAST` held ON in both arms so the data rung's own
serve count can be asserted *unchanged* across the fetch switch.

**Engagement is pinned on every case**, because *"the two paths agree"* is trivially true
when the fast path never fired.

> **Four mutations, each caught by the case that exists for it:**
>
> | mutation | caught by |
> |---|---|
> | the resolver never serves | 14 engagement failures across the corpus |
> | `TTR_NO_WRITE` mapped to `SLOW` (the data side's rule) | the verdict table (8 mismatched points) **and** the live write-protected-ITT fetch |
> | the odd-address gate removed | the three ODD-address cases, as *serves* |
> | the `IPAGE_HIT` counted above the bank decline (rung 2's own historical defect) | the device-bank case: `2` hits where the original counts `1` |

### Totals

| variant | before | after |
|---|---|---|
| default | 3 186 | **3 477** |
| lean | 3 182 | **3 473** |
| profiling | 3 384 | **3 699** |

(The other three harness binaries — SCSI CDB, boot-mode and AMIX-RAM — are unaffected at
34 / 75 / 81.)

---

## What it does *not* take, and what each is worth

* **The misaligned-longword splitter.** 13.4 % of instruction fetches, each costing roughly
  twice an ordinary one — call it a quarter of the fetch bucket, and the single largest
  thing left in it. Serving them would mean either putting the resolver inside
  `mmu_get_iword()` (which is *the fallback*, and the contract that makes this rung safe is
  that the fallback is unmodified original text), or re-implementing the splitter's fault
  semantics — `SAVE_EXCEPTION` / `THROW_AGAIN` / `misalignednotfirst` — in the fast path.
  **Not taken: it trades the rung's whole safety argument for its second-largest win.** The
  honest way to take it is the one rung 2's doc already named — serving the second word
  from a wider decoded-op entry so it is never fetched at all — which is a decoded-op-cache
  change, not an accessor one.
* **A shared `dfv`/`ifv` table.** The four TT registers are independent; ITT0 and DTT0 hold
  different values on the machine this exists for. Nothing to share but the *build*, which
  is shared.
* **Caching the bank decision.** Same answer as rung 2: it buys one dependent load and owes
  `map_banks()` an invalidation hook. Not worth a coherence obligation.
* **`uae_mmu040_get_ibyte`.** It routes through `mmu_get_byte(addr, false, …)` — the data
  accessor with `data=false` — and is not on any hot path in this workload. Left alone
  rather than given a fourth arm nothing measures.
