# Bringing up a new carrier board — checklist and findings (2026-08-06)

Two things learned the hard way on a new carrier revision. Both cost hours; both are
five-minute checks once you know them.

## 1. The 68060 must be fitted — even in pure-emulation boot modes

**Hard rule: never bring up a carrier with an empty 060 socket, in ANY boot mode.** The
emulation modes (UAE, UAE_030_MMU, …) never execute a single instruction on the 060, but the
chip still has to be physically present.

Symptom with the socket empty: the ARM side is perfect — SD mounts, boot menu works, HTTP
upload works, the boot-time memory test passes clean — but Kickstart dies 3–13 s after
emulator start, every boot. On serial you see the mouse-poll spin escalate (`[CLICK]` lines
from the auto-clicker, `cpu_emulator.cpp` ~1755 — its clicks acknowledge alert screens, so any
early-KS alert becomes an instant reboot), then a `RESET` executed at ROM `f80de8` (exec's
reboot site; the `CPU reset PC=` print in `newcpu.cpp` `m68k_reset`) — an endless reboot loop,
nothing ever drawn on native video.

A/B-proven: same Zynq module, same SD card, same firmware (crc `59FAE8CA`), same A4000 and
slot. Old carrier (060 fitted) boots Amix to multiuser; new carrier (socket empty) loops;
fitting the 060 in the new carrier fixed it immediately — Amix reached multiuser, zen0 up,
telnet confirmed.

Why the memory test doesn't catch it: the test only drives address/data/strobe cycles. It
never exercises the interrupt lines (IPL0–2, /INT2, /INT6), /BERR, or bus arbitration
(/BR /BG /BGACK) — exactly the signals an empty socket leaves floating (termination and
arbitration are routed through, or expect, the 060). So all memory tests pass while real 68k
operation dies as soon as interrupts and arbitration matter.

## 2. The boot-time bus memory test is the carrier-fault discriminator

Enable it in the config/preset: `enable_test YES` plus `test_range0`/`test_range1` … regions
(keys parsed in `Z3660/src/config_file.c`; values `YES`/`NO`/`MIN`; test loop in
`Z3660/src/mobotest.c`). It exercises real Amiga bus cycles against chip RAM, CIA space and
motherboard RAM, and its *failure pattern* localizes the fault. Two real cases from this
bring-up:

- **CPLD solder bridge:** 100 % of 32-bit chip-RAM cycles failed with garbage while 8-bit
  E-clock CIA cycles passed — 32-bit path broken, 8-bit path fine, i.e. the carrier's own
  bus logic, not the motherboard. Reflowing the bridge fixed it.
- **Single stuck data bit:** after the CPLD fix, every failure across the whole
  motherboard-RAM range was written-bit23-`0`, read-back-`1` (`+0x80` in byte 2) — a stuck-high
  D23. Not the carrier at all: the A4000 Fast-RAM SIMM had been unseated during the board
  swaps. Reseating fixed it.

### Operational gotchas

- Each non-empty test region **pauses** at a serial prompt: `Press 'C' to continue, 'R' to
  retry or ESC to exit` (`mobotest.c:1826`). These prompts come **before** the boot countdown
  and **drain any buffered `C` keys** — so the usual power-on C-spam route into the update
  menu fails with the test enabled.
- To reach the menu with the test on: answer each region prompt with `C`, **then** send `C`
  repeatedly during the `Continuing in N seconds` countdown. `ESC` skips the remaining
  regions.
- With the test enabled an unattended boot **stalls at the first prompt**. Re-disable it
  (restore the preset) for normal operation once the carrier is proven.
