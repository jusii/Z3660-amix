// SPDX-License-Identifier: MIT
/* CYBERSTORM C1 -- profiler config keys: the single copy of the resolution rule.
 *
 * WHAT THESE ARE
 *
 * Two keys in z3660cfg.txt that arm the interpreter profiler from POWER-ON, before
 * anything could be typed on the serial console:
 *
 *   prof_hz <N>          sampler rate in Hz. 0 (and an absent key) = OFF.
 *   prof_buckets YES|NO  the interpreter stage buckets. Absent = NO.
 *
 * The serial console can arm both at any time (PROF / PROFB). The config keys exist for
 * the one profile the console cannot take: the boot itself. By the time a human can type
 * PROF, the kernel is up and the interesting part is over.
 *
 * WHY THE DEFAULTS ARE OFF, AND WHY THAT IS THE OPPOSITE OF overhead_switches.h
 *
 * The C3 overhead switches default to ON because ON is what this firmware has always
 * done, and their contract is "an absent key changes nothing". These default to OFF for
 * exactly the same reason: the profiler has never existed, so an absent key must produce
 * a board that does not profile. Both rules are the same rule -- absent means unchanged --
 * and they land on opposite values because the histories differ.
 *
 * Even in the profiling BUILD, an unarmed profiler is close to free: the sampler's poll is
 * a predicate on a global that is false, and the buckets' probes return on their own
 * predicate. It is not free enough to arm by default, and nothing about a diagnostic
 * should be on when nobody asked.
 *
 * ON A NON-PROFILING BUILD these keys parse, publish to the mailbox, and are then ignored
 * by a core1 that has no profiler in it. That is deliberate: a config file is shared
 * across a flash of any of the three variants, and a key that made the default firmware
 * refuse to boot would be a much worse failure than one that quietly does nothing. The
 * serial console says so explicitly (PROF on a non-profiling build prints that the
 * firmware is not the profiling variant) so the silence is only in the config path.
 *
 * WHY THE RULE LIVES IN A HEADER
 *
 * Same reason as amix_ram.h and overhead_switches.h: config_file.c needs the Xilinx BSP
 * and cannot be compiled on the host, so the bounds and the defaults live here,
 * self-contained, and the host guard (Z3660_emu/test/host/bootmode_test.cpp) compiles
 * this header directly and asserts them. A default that drifts fails a test rather than
 * quietly changing what a metal session measures.
 */
#ifndef SRC_PROF_CONFIG_H_
#define SRC_PROF_CONFIG_H_

/* Sentinel and defaults. -1 means "no key in the file", the same convention the other
 * two resolution headers use, so all three read alike. */
#define Z3660_PROF_CFG_UNSET        (-1)
#define Z3660_PROF_CFG_HZ_DEFAULT   0     /* off */
#define Z3660_PROF_CFG_BUCKETS_DEF  0     /* off */

/* Must agree with Z3660_PROF_HZ_MIN/MAX in Z3660_emu/src/uae/z3660_prof.h. They cannot
 * share a header -- this one is core0 C, that one is core1 C++ inside the UAE tree -- so
 * the host guard asserts the two agree instead. The floor keeps the tick period well
 * inside the ARM cycle counter's 2^32 wrap; the ceiling is where the poll starts
 * distorting the thing it measures. */
#define Z3660_PROF_CFG_HZ_MIN       1
#define Z3660_PROF_CFG_HZ_MAX       20000

/* Ring capacity, mirrored from Z3660_PROF_RING_ENTRIES for the same reason and with the
 * same guard. core0 needs it to answer the only question that matters when arming at an
 * explicit rate: how many seconds of history fits before the ring wraps. Two of the three
 * C1 workload captures wrapped and retained a tail that was recorded AFTER the benchmark
 * ended, so they profiled the scheduler idle loop and had to be thrown away -- a number
 * the console prints at arm time is the cheapest possible defence against repeating it. */
#define Z3660_PROF_CFG_RING_ENTRIES 65536

/* The cross-core command word core0 writes into SHARED.prof_ctl. These MIRROR
 * Z3660_PROF_CTL_* in Z3660_emu/src/uae/z3660_prof.h, which core0 cannot include (core1
 * C++, inside the UAE include tree, and pulling that in would drag the whole emulator's
 * type world into the config parser). Mirroring is a real hazard, so the host guard
 * compiles BOTH headers and asserts the two sets agree value for value -- the same
 * treatment the SHARED mailbox itself gets, and for the same reason: two hand-maintained
 * copies that only ever meet at runtime, on metal, silently.
 *
 * Layout: command in bits 15..0, argument in bits 31..16. */
#define Z3660_PROF_CFG_CTL_NONE        0u
#define Z3660_PROF_CFG_CTL_ARM         1u   /* arg: 1 = arm the stage buckets as well */
#define Z3660_PROF_CFG_CTL_STOP        2u
#define Z3660_PROF_CFG_CTL_ZERO        3u
#define Z3660_PROF_CFG_CTL_DUMPSTATS   4u
#define Z3660_PROF_CFG_CTL_DUMPRING    5u   /* arg: sample count, 0 = the default slice */
#define Z3660_PROF_CFG_CTL_BUCKETS_ON  6u
#define Z3660_PROF_CFG_CTL_BUCKETS_OFF 7u
#define Z3660_PROF_CFG_CTL_MAKE(c,a)   ((((unsigned)(a)) << 16) | ((c) & 0xFFFFu))

/* requested: -1 (unset), 0 (explicitly off), or a rate.
 *
 * An OUT-OF-RANGE rate is CLAMPED, not refused, and this is a considered choice rather
 * than laziness. Every other config value here refuses and falls back, because a wrong
 * amix_ram or a wrong boot mode produces a board that does not work. A wrong prof_hz
 * produces a profile at a different rate than asked for -- and the rate that was actually
 * used is recorded in the dump header, so the reader is told. Refusing (i.e. profiling
 * nothing) would be the more destructive answer to a typo. */
static inline int z3660_prof_cfg_hz(int requested)
{
   if(requested == Z3660_PROF_CFG_UNSET) return Z3660_PROF_CFG_HZ_DEFAULT;
   if(requested <= 0)                    return 0;                       /* explicitly off */
   if(requested <  Z3660_PROF_CFG_HZ_MIN)  return Z3660_PROF_CFG_HZ_MIN;
   if(requested >  Z3660_PROF_CFG_HZ_MAX)  return Z3660_PROF_CFG_HZ_MAX;
   return requested;
}

static inline int z3660_prof_cfg_buckets(int requested)
{
   if(requested == Z3660_PROF_CFG_UNSET) return Z3660_PROF_CFG_BUCKETS_DEF;
   return requested ? 1 : 0;
}

/* "This resolved pair is the historical behaviour" -- i.e. no profiler activity at all --
 * used by the config printer to stay silent unless a profile was actually requested. */
static inline int z3660_prof_cfg_is_default(int hz, int buckets)
{
   return hz == Z3660_PROF_CFG_HZ_DEFAULT && buckets == Z3660_PROF_CFG_BUCKETS_DEF;
}

/* ---------------------------------------------------------------------------
 * What a bare `PROF` press does next -- the rate-cycling ladder, and its one rule that
 * is not a ladder step at all.
 *
 * THE LADDER: off -> 250 -> 1000 -> 4000 -> off. Every rung ARMS, and arming zeroes the
 * ring, the buckets and the counters -- which is harmless while the ladder is the only
 * way in, because then you always know what the next press will destroy.
 *
 * THE RULE: an armed rate that is NOT one of the ladder's own values did not come from
 * the ladder. It came from `prof_hz` in z3660cfg.txt (the boot profile, the one capture
 * the console structurally cannot take) or from an explicit `PROF <hz>`. Stepping over
 * such a rate arms on top of it and destroys it, and for the boot profile that means
 * another cold boot to retake something that took minutes to collect. So the press STOPS
 * instead, which is lossless and is what somebody who armed deliberately would want.
 *
 * This cost a real capture in the 2026-08-22 C1 metal session: armed at 200 Hz from the
 * config file, the first PROF press mapped 200 into the `< 1000` arm and re-armed at
 * 1000 Hz. There was no press that meant "stop" -- which is also why `PROF OFF` now
 * exists as a command in its own right rather than as an emergent property of a ladder.
 *
 * It lives here, next to the bounds, because it is a RULE rather than a keystroke: the
 * host guard compiles this header and asserts every transition, where the console code
 * it drives needs the Xilinx BSP and cannot be compiled on the host at all.
 * --------------------------------------------------------------------------- */
#define Z3660_PROF_CFG_LADDER_A     250
#define Z3660_PROF_CFG_LADDER_B     1000
#define Z3660_PROF_CFG_LADDER_C     4000

static inline int z3660_prof_cfg_is_ladder_rate(int hz)
{
   return hz == 0 || hz == Z3660_PROF_CFG_LADDER_A
                  || hz == Z3660_PROF_CFG_LADDER_B
                  || hz == Z3660_PROF_CFG_LADDER_C;
}

/* Returns the rate the next `PROF` press should arm at; 0 means "stop, keeping the data".
 * Callers must ALSO consult z3660_prof_cfg_is_ladder_rate() to tell an ordinary
 * ladder-wrap stop (4000 -> off) from an off-ladder refusal, because the two want
 * different words on the console even though they take the same action. */
static inline int z3660_prof_cfg_next_rate(int armed_hz)
{
   if(!z3660_prof_cfg_is_ladder_rate(armed_hz)) return 0;   /* deliberate: stop, do not arm */
   if(armed_hz == 0)                       return Z3660_PROF_CFG_LADDER_A;
   if(armed_hz <  Z3660_PROF_CFG_LADDER_B) return Z3660_PROF_CFG_LADDER_B;
   if(armed_hz <  Z3660_PROF_CFG_LADDER_C) return Z3660_PROF_CFG_LADDER_C;
   return 0;
}

#endif /* SRC_PROF_CONFIG_H_ */
