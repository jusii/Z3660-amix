# The block-idiom fast path (CYBERSTORM C2, rung 0)

What it is, what it is deliberately not, how to switch it, and the pre-registered numbers
the metal session is supposed to judge it against — written down *before* the measurement,
because a target chosen after the result is not a target.

The measurement that motivates it lives in the 040 port's `docs/C2-ATTACK-MAP.md` §9.7 and
§11.1. The implementation lives in `Z3660_emu/src/uae/newcpu.cpp` (the recognizer, with the
long comment) and `Z3660_emu/src/uae/z3660_blk.h` (the switch rule). The evidence lives in
`Z3660_emu/test/host/harness_main.cpp` (`test_blk_state_equivalence`).

## The measurement

A family of tight kernel block loops accounts for **33.6 % of an AMIX boot**, 25.1 % of a
raw disk read and 10.6 % of dhrystone. `bzero` alone is 22.1 % of boot, and it is two
instructions at one PC:

    0800032c   20c9   MOVE.L A1,(A0)+
    0800032e   51c9   DBF    D1,-4

Costed from the map's per-event table those two pay **850.5 ARM cycles per four bytes**, of
which the guest store is 162.7. Kernel zero-fill runs at 5.17 MB/s and **80.9 % of the price
is interpreter machinery around a four-byte write**.

## What the fast path does

A recognizer in `m68k_run_mmu040`, keyed on the **opcode pair** and never on a PC or a
symbol, spots the loop after the opcode fetch and before the dispatch, and runs many
iterations inside one pass of the run loop.

| stage | idiom | head opcodes |
|---|---|---|
| A | `bzero` | `MOVE.L An,(Ay)+` + `DBF Dn` whose displacement targets the head |
| B | `mcpy` | `MOVE.L (Ax)+,(Ay)+` + the same `DBF` shape |
| C | `Lcb_loop` | `CPUSHL <cache>,(An)` + `ADDA.W #n,An` + `DBF Dn` |

Stage C collapses to almost nothing, and the map's sentence about it is the right one: *it
is a cache-maintenance loop and there is no cache to maintain.* Everything `op_f428_31_ff`
does to this machine is a function of the opcode alone — `flush_mmu040()` discards its
address, `flush_cpu_caches_040()` acts only at scope 3 which this line-scope family never
is, `flush_icache()` is a no-op macro without the JIT — so all of it is idempotent and
doing it once for the whole run is the same operation, not an approximation of it.

## What it deliberately does not do

**It does not replace the guest store.** No `memset`, no host block move, no direct pointer
into guest memory. Every iteration calls `put_long_mmu040()` — the exact function the
generated handler calls, with the same arguments in the same order — so TTR matching, the
data page cache, the ATC, the M and U descriptor bits, protection checks, bank dispatch and
device side effects are preserved *by construction* rather than by argument.

Three reasons, in order of weight:

1. It is the difference between a change whose correctness is proved and one that is
   reasoned about. Fault semantics is risk 1 in the map's own ordering, and this repository
   has paid for the general lesson: a page-crossing DMA that clipped an unrelated frame
   stayed invisible until it ate an ELF header.
2. **The arithmetic does not need it.** The map pre-registers an 80 % reclaim, and 80 % is
   exactly what you get when the store stays and the machinery goes: 162.7 of 850.5 cycles
   is 19.1 %. The pre-registered gain *is* the gain of the safe design.
3. A host-pointer fast path could not be tested. `board_stubs.cpp` leaves every bank's
   `baseaddr` NULL on purpose (harness decision #6), so a `memset` over a direct pointer
   would ship having never executed in the harness — and the harness is the whole evidence
   base for this change.

## The three risks, and what answers each

**1. Fault semantics.** `op_20c8_31_ff` is *not* instruction-restart: it increments the
destination register, sets the flags, advances the pc to the `DBcc`, clears `mmu_restart`
and disarms `mmufixup` — and only then stores. A fault there is a 68040 write-back fault on
an instruction that has already completed architecturally, which wb040.s replays. The fast
path reproduces that state exactly, by hoisting the loop-invariant half (flags, pc,
`instruction_pc`, `mmu_restart`) out of the loop rather than re-deriving it.

**2. Interrupt latency.** The fast path performs **at most one chunk and then returns to the
run loop**, leaving the pc on the head instruction if iterations remain. The tail therefore
runs — cycle accounting, sampler, `check_uae_int_request()` at its configured cadence,
`do_specialties()` — **once per chunk**, and the state at every chunk boundary is a state the
interpreted path itself passes through, so an interrupt taken there is indistinguishable
from one taken between two guest instructions. **Yield granularity is one chunk.**

**3. Coherence / device access.** The destination bank is checked once per chunk and
anything that is not plain RAM declines to the interpreter. With the store unchanged this is
not a correctness question but a *latency* one, and a sharp one: a burst of 256 writes to an
emulated device with no poll between them is how a core0 mailbox round trip would deadlock.

The gate is **strictly non-faulting** and never computes an address that is used for a
store. It reads the TTR registers (a pure predicate) and the already-populated data page
cache; on a cold entry it declines, the interpreter runs one ordinary instruction, and the
entry that instruction populates lets the next pass through. One interpreted instruction per
page — 0.1 % of a 1024-longword fill — buys a gate that cannot raise, cannot set a
descriptor bit the interpreted path would not have set, and cannot move a fault.

It also makes the page-boundary rule fall out of the same mechanism instead of being a
second one: the gate's verdict is valid for one page, so the chunk is bounded by the page,
so the fill stops at every boundary and re-validates.

## The switch

`SHARED.blk_chunk` at `0xFFFF00E4`, resolved by `z3660_blk_resolve_chunk()` in
`z3660_blk.h` and **nowhere else**:

| mailbox word | meaning |
|---|---|
| `0` (unset) | the built-in default, **256 iterations per chunk** |
| `0xFFFFFFFF` | OFF — the recognizer returns before examining anything |
| anything else | that many iterations per chunk, clamped to 1024 |

Unset means *default*, not off, for the same reason `overhead_switches.h` gives: a core0
built before the field existed must still run the shipped behaviour.

It is a chunk size and not a yes/no because the chunk is what bounds interrupt latency, so
one knob answers three questions a metal session has — off, on, and how long a stall this
guest will tolerate. **256 iterations is a ~42 µs window at 1.1 GHz**, inside the 64 µs of
one 15.6 kHz hsync, which is the tightest cadence anything on this poll depends on
(`docs/AMIX.md`'s A3000-SCSI interrupt-delay pump).

Serial console: **`BLK`** (or `BLOCK IDIOM`) cycles OFF → 64 → 256 → 1024 → OFF. Core1 picks
it up at its next knob resync, well under a second.

There is deliberately **no config-file key yet**. The console covers A/B on a live guest,
which is what the verdict needs; a key would be needed only to boot a whole run with the
recognizer off on the same flash, and the OFF boot baseline (163.8 s) is already measured on
the pre-change image.

## Utilisation: the counters

Profiling build only, appended counter ids 21–23, reported by `PROFD`.

- **`BLK_INSNS / INSNS`** — the share of the guest instruction stream the recognizer took.
  The map predicts 22.07 % of a boot for stage A alone, 31.80 % for A+B+C. *A capture that
  shows 2 % is a recognizer not firing where the profile says the time is, and the first
  thing to check is the trigger, not the host operation.*
- **`BLK_INSNS / BLK_HIT`** — the mean chunk length, which separates "firing" from "firing
  usefully". A mean near 2 means the recognizer's test is being paid per iteration and the
  whole saving is gone. Healthy is the high hundreds; never below ~8.
- **`BLK_BYTES`** — comparable across workloads and across future stages in a way an
  instruction count is not, and what a "kernel zero-fill runs at N MB/s" figure comes from.

They are counters and not buckets on purpose: a bucket needs a phase transition around the
fast path, and the fast path's whole value is that it does not pay per-instruction
machinery. The fast path's own overhead lands in `LOOP`, which after probe subtraction is
0.51 % today — so a post-change capture in which `LOOP` has grown is unambiguously this and
nothing else.

## PRE-REGISTERED — judge the change against these, not against what it produces

From the map §11.1, at its stated 80 % reclaim:

| | measured before | stage A only | A + B + C |
|---|---|---|---|
| boot window | 163.8 s | **~135 s** | **~122 s** |
| dhrystone | 4486 | **~4543** | **~4862** |
| raw disk read | ~797–829 kB/s | **~844 kB/s** | **~925 kB/s** |

Plus the bench guard, which is the stronger check because it says the time went away *from
the place the map said it was*: re-take the boot profile and **`bzero`'s share must fall
below 3 %**.

**A result below the stage-A boot figure means the recognizer is not firing where the
profile says the time is.** Check `BLK_INSNS / INSNS` first, and then the trigger.

Extending the recognizer to the whole §9.7 family — the `MOVES` cross-space copies, 33.63 %
of boot and 25.11 % of the IO workload — would take boot to ~120 s and the disk read to
~1037 kB/s. That is the size of the prize, not a pre-registration.

## Verdict protocol

1. Flash `BOOT-prof.BIN`. `PROF 1000`, `PROFB`, boot, `PROFD`. Read `BLK_INSNS / INSNS` and
   the mean chunk length **before** looking at any wall-clock number: if the recognizer is
   not firing, the timings are measuring something else.
2. `BLK` to OFF and repeat the workload; the counters must go to zero and the rates must
   return to the pre-change figures. This is the A/B that separates "the change" from "the
   flash".
3. Flash `BOOT-lean.BIN` for the timings — boot to login, dhrystone, `dd` from `ced0s1` —
   and compare against the table above.
4. The bench guard last: re-take the boot profile and check `bzero`'s share.

## What is not covered, stated plainly

Two lines in the recognizer are currently **dead**, and mutation testing says so rather than
reasoning saying so — inverting either leaves the whole corpus green while sixteen other
deliberate defects do not:

- `mmu_restart = false` is overwritten by the run loop at the top of the next pass.
- the post-increment happening before the store rather than after is invisible, because
  nothing can observe the machine between the two.

Both are dead *because of today's gate*: it only fast-paths a page whose page-cache entry a
successful access already established, and a chunk never leaves that page, so a store inside
a chunk cannot fault. They are written correctly anyway and they stay. Widen the gate to
translate cold pages itself, or hoist the store out of `put_long_mmu040()`, and a mid-chunk
fault becomes reachable in the same edit — at which point those two lines are the difference
between a page fault and a corrupted file system, and nobody will be looking at them.
