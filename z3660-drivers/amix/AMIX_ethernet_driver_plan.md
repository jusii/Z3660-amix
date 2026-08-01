# Plan: Z3660 Ethernet driver for AMIX (Amiga Unix)

> **Status: DEFERRED — blocked on prerequisite.** Implementation cannot start
> until the Z3660 emulator gains real 68030 PMMU translation (today it is faked:
> `mmu_op30fake_*` in
> [newcpu.cpp](../../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/src/uae/newcpu.cpp),
> TTR-match only, no page-table walk) and AMIX boots on the emulated 030. This
> document is saved for later reference. *Exception:* the FS-UAE prototyping
> phase (see "Prototyping strategy") could proceed independently if desired,
> since FS-UAE already emulates a real 030 MMU and runs AMIX.
>
> The prerequisite MMU work has its own plan:
> [../../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/UAE_030_MMU_plan.md](../../z3660-firmware/Z-TURN/vitis_ide/Z3660_emu/UAE_030_MMU_plan.md).

## Context

The Z3660 exposes its Ethernet through a physical Zynq GEM PHY that the ARM
firmware bridges to a tiny, OS-agnostic register + fixed-buffer interface in the
Amiga's Zorro III address space. Today the only consumer is `Z3660Net.device`,
an **AmigaOS SANA-II Exec device** ([../eth/device.c](../eth/device.c)).
That driver is useless under AMIX, which is SVR4 UNIX and needs a STREAMS-based
kernel network driver.

**Scope of this plan: the AMIX driver only.** We assume the real
blockers — a UAE-030 core with working PMMU translation (today the MMU is faked:
`mmu_op30fake_*` in newcpu.cpp,
TTR-match only, no page-table walk) and a booting AMIX on the emulated 030 — are
solved elsewhere. This plan covers writing a SVR4/AMIX kernel Ethernet driver
that speaks the **existing** Z3660 register contract. **No firmware or FPGA
changes are required**; the ARM↔68k seam is identical regardless of which OS runs
on the 68k.

The good news: the hardware contract is trivial (7 registers + fixed buffers +
one autovector interrupt), and both sides are big-endian m68k, so there is **zero
byte-swap work**.

**Decisive enabler — fork [hydra-amix](https://github.com/isoriano1968/hydra-amix)
instead of starting from scratch.** That repo is a modern, working AMIX 2.1p2
STREAMS DLPI Ethernet driver for the Hydra Systems Zorro II NIC (DP8390/NE2000).
It already solves every framework problem that was a "risk" in the from-scratch
version of this plan:
- **Driver ABI confirmed**: STREAMS DLPI provider (`struct streamtab hydrainfo`,
  `cdevsw` slot 47, major `hya`, `hydrawput()` handling `DL_INFO_REQ`,
  `DL_BIND_REQ`, `DL_UNITDATA_REQ`). DLPI is the right target — no guesswork.
- **Toolchain documented**: cross-build with `m68k-amix-gcc` (GCC 2.7.2.3, SVR4
  target), `make CC=m68k-amix-gcc CFLAGS="-O -D_KERNEL -DSVR40 -DSVR4"`, ELF→COFF
  via `elf2coff`, bootable image via `stand/ make oldboot KERNEL=…`. The
  toolchain archaeology is **already done**.
- **Autoconfig pattern**: `autocon(BOARD_ID,…)` bootinfo lookup + direct Zorro
  probe + a userspace `zorro_probe.c`; lazy init at `ifconfig` time.
- **Interrupt registration**: `hydraintr()` registered in `int2_tbl[]` on **INT2**
  — which is exactly the interrupt the Z3660 firmware can drive (the AmigaOS
  driver already selects INT2 via `ENV:ZZ9K_INT2`,
  [../eth/device.c:237](../eth/device.c#L237)).
- **Bonus**: it ships the A2065 (`aen`) reference driver in-tree and an
  FS-UAE fallback mode — see the prototyping strategy below.

So the work collapses to: **fork hydra-amix, gut its device layer, and graft the
Z3660 register/buffer contract underneath the intact DLPI/STREAMS scaffolding.**
The Z3660 device layer is *simpler* than Hydra's: there is no DP8390 chip to
drive, no card ring buffer to manage, no MAC PROM to read — the ARM firmware does
all of that. The Z3660 driver just pokes 7 registers and memcpys two buffers.

## The hardware/firmware contract (stable, reuse as-is)

Register offsets from the board base ([../common/z3660_regs.h:40](../common/z3660_regs.h#L40)):

| Register | Offset | Dir | Meaning |
|---|---|---|---|
| `REG_ZZ_ETH_TX` | 0x190 | W/R | Write = TX frame byte count; read back = send result/error |
| `REG_ZZ_ETH_RX` | 0x194 | W | Write 1 = pop current RX backlog slot |
| `REG_ZZ_ETH_MAC_HI` | 0x198 | R | MAC bytes 0–1 |
| `REG_ZZ_ETH_MAC_LO` | 0x19C | R | MAC bytes 2–5 |
| `REG_ZZ_ETH_RX_ADDRESS` | 0x1A4 | R | Offset of current unread RX slot |
| `REG_ZZ_INT_STATUS` | 0x1A8 | R | bit0 = Ethernet IRQ pending |
| `REG_ZZ_CONFIG` | 0x104 | W | bit0 enable IRQ; write `8\|16` to ack/clear |

Buffers (offsets into the board's RTG fast-RAM window):
- **TX**: single buffer at `TX_FRAME_ADDRESS` (0x07EE0000). Layout: 6B dst MAC,
  6B src MAC, 2B ethertype, payload. Then write `14 + payload_len` to `REG_ZZ_ETH_TX`.
- **RX**: 32-slot ring at `RX_BACKLOG_ADDRESS` (0x07ED0000), `FRAME_SIZE`=2048.
  Each slot: 2B size (BE), 2B serial (BE), then frame at `+RX_FRAME_PAD` (4).
  Firmware fills slots and bumps a 16-bit `frame_serial`
  ([ethernet.c:534](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/ethernet.c#L534));
  `REG_ZZ_ETH_RX_ADDRESS` returns the current unread slot
  ([rtg.c:806](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/rtg/rtg.c#L806)); writing
  `REG_ZZ_ETH_RX=1` advances `frames_received_from_backlog` and resets the ring
  when drained ([ethernet.c:604](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/ethernet.c#L604)).

The AmigaOS driver ([../eth/device.c:749](../eth/device.c#L749)) is the
**reference implementation** of this protocol — read its `frame_proc`,
`read_frame`, `write_frame`, and `dev_isr` to mirror the exact sequencing in C.

## What to build (AMIX side) — as deltas against hydra-amix

Fork hydra-amix and keep its `usr/sys/...`, `master.d`, `stand/`, and `streamtab`
scaffolding intact. Create `usr/sys/amiga/driver/z3660/` modeled on
`driver/hydra/`. The changes:

1. **Driver config / kernel integration** — *mostly reuse*
   - Copy the `hydra` `master.d` entry / `cdevsw` slot / `streamtab` wiring,
     rename to a `z36`/`z3660` major. Build with the documented
     `make CC=m68k-amix-gcc …` flow; boot image via `stand/ make oldboot`.

2. **Board autoconfig / probe** — *adapt IDs + Zorro III*
   - Reuse hydra's `autocon()` + Zorro-probe pattern, but match Z3660: vendor
     `0x144B`, product `0x1` (same IDs the AmigaOS driver matches at
     [../eth/device.c:166](../eth/device.c#L166)). Z3660 is **Zorro III**, so
     confirm AMIX autoconfig hands back the Z3 base address correctly (Hydra is
     Zorro II). Adapt `zorro_probe.c` for the Z3660 ID to validate before kernel
     work.

3. **Device layer — replace wholesale (the big simplification)**
   - **Delete** Hydra's `ne2000.h` / DP8390 ring + PROM logic. The Z3660 has no
     chip to drive. Replace with the [../common/z3660_regs.h](../common/z3660_regs.h)
     register pokes and buffer memcpys described above, porting the proven
     sequencing from the AmigaOS `frame_proc`/`write_frame`/`dev_isr`
     ([../eth/device.c:749](../eth/device.c#L749)) into C kernel code.
   - MAC: read `REG_ZZ_ETH_MAC_HI/LO` instead of reading a PROM.

4. **Register/buffer mapping — cache-inhibited**
   - Map the board register page and the RTG buffer window into kernel virtual
     space **cache-inhibited** (CI bit in the 030 page descriptor, or a TT
     register). This is mandatory: the RX serial watermark and frame bytes are
     written by the ARM behind the 68k's back, so a cached 030 would read stale
     data. (Note the firmware's own commented-out `Xil_SetTlbAttributes` lines at
     [ethernet.c:307](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/ethernet.c#L307)
     — coherency is a known sensitivity on this path.)

5. **Interrupt handler** — *reuse hydra's `int2_tbl[]` registration*
   - Register the handler in `int2_tbl[]` exactly as hydra's `hydraintr()` does and
     drive the Z3660 firmware on **INT2** (the AmigaOS driver already supports INT2
     via `ENV:ZZ9K_INT2`, [../eth/device.c:237](../eth/device.c#L237)).
     Handler body: read `REG_ZZ_INT_STATUS` bit0, ack via `REG_ZZ_CONFIG = 8|16`,
     then drain RX at the right `spl`.

6. **RX path — drain the whole backlog**
   - Loop: read `REG_ZZ_ETH_RX_ADDRESS`, read the slot's serial; while serial
     differs from last seen, copy the frame into an `mblk`/`streams` buffer, send
     it upstream (DLPI `DL_UNITDATA_IND`, demultiplexed by ethertype/SAP), then
     write `REG_ZZ_ETH_RX = 1` to pop and advance. Mirror the firmware ring
     semantics above; do **not** process just one frame per IRQ (the AmigaOS
     driver gets away with one-per-signal because it re-Waits — a UNIX ISR should
     drain to avoid 32-slot overrun under load).

7. **TX path**
   - On downstream `DL_UNITDATA_REQ`/`M_DATA`: build dst MAC + src MAC + ethertype
     header at `TX_FRAME_ADDRESS`, copy payload, write `14 + len` to `REG_ZZ_ETH_TX`,
     read back for error. Single TX buffer ⇒ serialize transmits with a per-device
     lock/queue; gate the next frame on the firmware accepting the prior one.

8. **DLPI / link layer plumbing** — *reuse hydra's `hydrawput()` verbatim*
   - hydra-amix already proves the DLPI contract against AMIX 2.1p2's TCP/IP:
     keep its `streamtab` and `wput` handling of `DL_INFO_REQ`,
     `DL_BIND_REQ`/`DL_UNBIND_REQ`, `DL_UNITDATA_REQ/IND`, `DL_PHYS_ADDR_REQ`.
     Only the leaf actions change — bind/SAP demux stays, TX/RX leaves call the
     Z3660 register layer, and `DL_PHYS_ADDR_REQ` returns the MAC from
     `REG_ZZ_ETH_MAC_HI/LO`. The "is it DLPI or legacy if-glue?" question is now
     **settled: DLPI.**

## Prototyping strategy — decouple from the MMU blocker

hydra-amix was developed with an **FS-UAE fallback (A2065) test mode**. FS-UAE
emulates a real 68030 PMMU and **runs AMIX today**. This lets the project split
cleanly:

- **Phase A (now, no Z3660 MMU needed):** Fork hydra-amix and bring up the Z3660
  DLPI/STREAMS driver under FS-UAE against either the A2065 fallback or a
  stub/bridge that mimics the Z3660 register+backlog contract. This validates all
  the AMIX-side framework code — config, autoconfig, interrupt wiring, DLPI
  primitives, RX-drain/TX sequencing — entirely independent of the Z3660 emulator
  work.
- **Phase B (when the Z3660 can boot AMIX):** Swap the FS-UAE stub for real
  Z3660 register access and validate on hardware. By then only the
  ~7-register/2-buffer leaf layer is unverified.

This means the Ethernet driver can be built and ~90% tested **in parallel** with
the UAE-030-MMU emulation effort, rather than waiting on it.

## Reference / files

- **Driver skeleton to fork**: [hydra-amix](https://github.com/isoriano1968/hydra-amix)
  — `usr/sys/amiga/driver/hydra/` (`hydra.c/.h`), `driver/aen/` (A2065 ref),
  `master.d/`, `stand/`, `zorro_probe.c`.
- Z3660 protocol reference (read, don't modify): [../eth/device.c](../eth/device.c),
  [../common/z3660_regs.h](../common/z3660_regs.h).
- Firmware behavior (read, don't modify): [ethernet.c](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/ethernet.c),
  [rtg.c register handlers](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/rtg/rtg.c#L806),
  [memorymap.h](../../z3660-firmware/Z-TURN/vitis_ide/Z3660/src/memorymap.h).
- New driver code lives in an AMIX kernel source tree (the hydra-amix fork), not
  this repo; the sources can be vendored into this `z3660-drivers/amix/` directory
  for distribution.

## Risks / open research (ethernet-only)

1. ~~**AMIX network-driver ABI**~~ — **resolved**: DLPI, per hydra-amix.
2. ~~**Toolchain / DDK**~~ — **resolved**: `m68k-amix-gcc` 2.7.2.3 + `elf2coff` +
   `stand/ make oldboot`, documented in hydra-amix. (Still need to obtain/build
   that cross-toolchain and hold a licensed AMIX 2.1p2 — no AMIX binaries are
   redistributable.)
3. **Zorro III base address** — hydra is Zorro II; confirm AMIX autoconfig returns
   the Z3660's Zorro III base correctly for register/buffer mapping.
4. **Cache coherency** — CI mapping of registers + RX/TX buffers is mandatory and
   interacts with how faithfully the (future) MMU/cache emulation honors CI.
5. **Multicast / promiscuous** — verify firmware filtering; for AMIX-era IPv4
   (unicast + broadcast + ARP) the current path is sufficient, but DLPI
   `DL_ENABMULTI_REQ` may have no hardware backing.
6. **Single TX buffer** — no concurrent transmits; needs driver-side serialization.

## Verification (end to end)

1. Driver loads, board probed, MAC reads back correctly (`ifconfig` shows it).
2. `arp` resolves a LAN host (TX header + RX demux working).
3. `ping` to gateway (full TX/RX round trip; watch firmware `DEBUG_ETHERNET`
   serial/backlog counters to confirm pop sequencing).
4. TCP: `telnet`/`ftp` to another host (flow under load; confirm no backlog
   overrun by stressing RX with a flood ping).
5. Cross-check against the AmigaOS `Z3660Net.device` on the same hardware to
   isolate driver bugs from firmware/network issues.
