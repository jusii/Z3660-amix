/* CYBERSTORM C3 -- which run-loop instruments this BINARY was compiled with.
 *
 * The single copy of the build-variant rule, in a self-contained header for the same
 * reason as amix_ram.h and overhead_switches.h: the contract that matters is "a build
 * that passes no -D behaves exactly as this firmware always has", and a contract worth
 * stating is worth testing. The host harness compiles this header directly and asserts
 * both the default and the lean shape.
 *
 * TWO VARIANTS, ONE TREE
 *
 *   DEFAULT (no flags)   the diagnostic firmware. The ISSUE-52 probes and the
 *                        per-instruction perf accumulator are compiled IN and gated by
 *                        the runtime switches (PROBES / PERFS, config emu_probes /
 *                        perf_stats). Switched off, each still costs its predicate on
 *                        the hot path -- an address load, a value load, a compare and a
 *                        perfectly-predicted branch, per emulated instruction.
 *
 *   LEAN (-DZ3660_EMU_PROBES=0 -DZ3660_EMU_PERF=0)
 *                        the production firmware. The instruments are compiled OUT, so
 *                        the predicates vanish with them: no probe tests, no accumulator,
 *                        no switch predicates on the hot path at all. `make lean`.
 *
 * WHY BOTH EXIST. The 2026-08-22 metal calibration measured the runtime switches on real
 * hardware: all three off (rung F) bought +9.1 % guest throughput, but defaults-ON was
 * -3.6 % against the OLD hard-wired build -- i.e. making the instrumentation switchable
 * cost real time that the hard-wired build never paid, and only a switch actually thrown
 * earns it back. Compiling the instruments out removes the switch cost as well, which
 * neither the old hard-wired build nor rung F could do. The diagnostics stay one flash
 * away: the default build is the one to put on the card when something needs looking at.
 *
 * THE HAZARD THIS HEADER DOES NOT COVER. The variant is selected by -D flags, and no
 * header dependency tracks a -D. The two variants therefore build into SEPARATE build
 * directories (Z3660_emu/build vs Z3660_emu/build-lean, see Z3660_emu/Makefile), so a
 * variant switch can never leave half the objects from the other one behind.
 */
#ifndef SRC_UAE_RUNLOOP_BUILD_H_
#define SRC_UAE_RUNLOOP_BUILD_H_

/* 1 = compiled in (and runtime-switchable). 0 = compiled out entirely.
 * Defaults are load-bearing: an unflagged build must not change a single instruction. */
#ifndef Z3660_EMU_PROBES
#define Z3660_EMU_PROBES 1
#endif
#ifndef Z3660_EMU_PERF
#define Z3660_EMU_PERF   1
#endif

/* Bit 0 = the run-loop probes, bit 1 = the perf accumulator.
 * 3 = the default (diagnostic) build; 0 = the lean (production) build. */
#define Z3660_RUNLOOP_BUILD_PROBES  1
#define Z3660_RUNLOOP_BUILD_PERF    2
#define Z3660_RUNLOOP_BUILD_FLAGS \
   ((Z3660_EMU_PROBES ? Z3660_RUNLOOP_BUILD_PROBES : 0) | \
    (Z3660_EMU_PERF   ? Z3660_RUNLOOP_BUILD_PERF   : 0))

/* "Lean" means EVERY run-loop instrument is out, so the hot path carries no predicate at
 * all. A half-lean build (one flag only) is a legitimate thing to compile, but it is not
 * the production variant and must not be reported as one. */
#define Z3660_RUNLOOP_BUILD_IS_LEAN  (Z3660_RUNLOOP_BUILD_FLAGS == 0)

#endif /* SRC_UAE_RUNLOOP_BUILD_H_ */
