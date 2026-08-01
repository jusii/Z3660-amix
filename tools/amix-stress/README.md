# amix-stress

Stress / reproduction tools for the **68030 MMU fault-path** under AMIX (UNIX SVR4 on the
emulated 030). Built to reproduce and validate the fix for the intermittent
`User BUS ERROR at <pc>, PC:<pc> FAULT:6` crashes that hit AMIX system tools (cron,
`in.telnetd`, …) under fork/exec load.

These are a sibling to [`../amix-bench`](../amix-bench) (CPU/IO benchmark): same idea —
portable source + a precompiled AMIX m68k binary + scripts.

## The bug these target

A 68030 instruction that bus-faults under demand paging is resumed mid-flight from its 030
bus-error frame (`m68k_do_rte_mmu030` → the run loop's inner retry loop). The instruction-start
PC was only snapshotted at the **outer** run-loop top, never when the **inner** loop re-ran a
continuation-resumed instruction. So if the resumed instruction faulted **again** — e.g. a
`MOVEM.L <regs>,-(SP)` function prologue growing the **user stack** across the next
not-yet-resident page — the new bus-error frame was rebuilt from the stale outer-loop PC (the
kernel's `RTE` trap-return epilogue). The following `RTE` then returned the user process to that
**kernel PC in user mode** → next fetch landed in unmapped user space → `User BUS ERROR`.

Fixed in `z3660_emu/.../newcpu.cpp` (`m68k_run_mmu030`): re-snapshot the instruction-start PC at
the inner-loop continuation point. The stressors below force exactly this path (deep user-stack
growth + heavy fork/exec).

## Files

| File | Runs on | Purpose |
|------|---------|---------|
| `amixstress.c` | AMIX guest | Deep recursion with big per-frame buffers → forces `MOVEM -(SP)` to grow the user stack across many demand-paged pages (the exact trigger). K&R `main()` for the vintage AMIX `cc`. |
| `amixstress` | AMIX guest | Precompiled for **AMIX 2.1c** (ELF32-BE m68k, dyn-linked `libc.so.1`). Run directly or recompile from source. |
| `telnet_storm.py` | host | Storms telnet connects so inetd fork/exec's `in.telnetd` repeatedly — the ~100% reproducer of the crash. |
| `gsoak.sh` | AMIX guest | Sustained loop: `amixstress` (varied depth) + fork/exec churn. One login → no inetd throttling. The reliable soak driver. |
| `amix_soak.sh` | host | Acceptance soak: N workload cycles + periodic clean reboots, polling the firmware's wild-PC guard; hard-fails on a latch. |
| `overnight.sh` | host | Overnight burn-in: launch `gsoak.sh` on the guest, then watch the wild-PC guard via the serial log for ~3h. |

## Build / run

### On the AMIX guest

```sh
# build (or use the shipped binary)
cc -O amixstress.c -o amixstress

# one shot:  amixstress [depth] [iters]   (depth = recursion frames, ~depth*2KB of stack growth)
./amixstress 220 6

# sustained soak (install amixstress on the root FS first so it survives reboots):
cp amixstress /amixstress
N=20000 sh gsoak.sh &        # loops amixstress + fork/exec churn; logs every 25 iters to /tmp/gsoak.log
```

### From the host

```sh
# reproduce: storm in.telnetd  ->  <host> <count> <hold-seconds>
python3 telnet_storm.py 192.168.2.39 30 0.4
# (before the fix this crashes ~100% of in.telnetd at a constant wild PC; after, the AMIX
#  console stays clean. "empty" responses under a storm are inetd loop-protection, not crashes —
#  the ground truth is the firmware's serial wild-PC count.)

# acceptance soak (env-overridable bench config; see headers):
./amix_soak.sh 50

# overnight burn-in:
./overnight.sh
```

The host scripts need `amixsh.py` / `amixsync.py` (scripted telnet / FTP runners — from
`grimoire-amix/tools/host-net/`, or any equivalent) and ssh to the serial-console host. Bench
specifics are env vars with this project's defaults: `KVM_HOST`, `SERIAL`, `AMIX_HOST`,
`AMIX_PASS`, `AMIX_PORT`, `AMIXSH`.

## How the guard works

Instrumented firmware (AMIX-gated, one-shot) prints a `[WILD]` dump to the **serial UART** the
instant a user process's PC goes wild — *before* AMIX even prints its console `User BUS ERROR`.
The soak scripts treat any rise in the serial `"user PC went wild"` count (or any
`"User BUS ERROR"` on the serial) as a hard failure. With the fix, the count never moves.

## Validation result (2026-06-25, real A4000+Z3660)

| Test | Before fix | After fix |
|------|-----------|-----------|
| `telnet_storm.py … 30` | ~21 `in.telnetd` BUS ERROR | 0 — console clean |
| fork/exec 200× + `amixstress` deep recursion | crashes | clean |
| Continuous `gsoak.sh`, ~100 min+ | crashed within minutes | stable, wild-guard never latches |
| Host MMU harness (`z3660_emu/test/host`) | 60/60 | 60/60 |
