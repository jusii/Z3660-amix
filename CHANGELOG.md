# Changelog

## 2026-08-27 — the reboot soak that never rebooted, and the count that goes with it (d40ce94, 0fe57b4)

- `tools/amix-stress/amix_soak.sh`: fixed two defects that made every reboot cycle a
  no-op scoring green. The shutdown ended in `&`, which the telnet runner's appended
  `; echo DONE_$?` turned into a Bourne syntax error (`&;`), so it never executed; and
  the post-reboot check was a bare TCP connect to port 23, which a box that never
  rebooted answers instantly. The background job is now parenthesised, and a reboot must
  clear two independent gates — telnet going DOWN within `DOWN_WAIT`, and the SVR4
  `who -b` boot stamp CHANGING across the cycle — with an unprovable reboot a hard
  failure (exit 3). `SOAK DONE` now reports `reboots_proven=N`; new env knobs
  `REBOOT_EVERY` / `DOWN_WAIT` / `UP_WAIT`.
- `docs/AMIX.md`: the "27-reboot soak with concurrent fork/exec load, all clean" claim is
  half retracted at all three occurrences. The workload half stands (scored from the
  firmware wild-PC guard over the serial UART, independent of the soak's telnet path);
  the reboot count is withdrawn as unestablished. Re-establishment procedure noted:
  5 cycles = 1 proven reboot, ~30 min on the box.

## 2026-08-25 — the ARM's own data abort, located in the binary (716d4bd)

- `docs/core0-async-data-abort-2026-08-25.md`: the 2026-08-25 `[Core0] DataAbortHandler()`
  (DFSR `0x00001c06`) decoded and symbolised against the exact running image
  (`BOOT-lean-9CE3F6A6`). The abort PC `0x0100b2c8` is the completion-poll branch of
  `Xil_L2CacheFlush()` — the PL310 whole-cache Clean-and-Invalidate by Way — reached from
  `scsi.c:1878` (`SETFSH`, "no filesystem handler"). Two defects named in passing:
  `Xil_L2CacheFlush()` masks no interrupts where its L1 sibling masks IRQ+FIQ, and the 60 Hz
  vblank ISR calls the same whole-cache flush in `bootmode CPU`, so a PL310 background
  operation can be re-programmed from interrupt context while it runs. Corrects the bench
  reading of the fault record: `WnR` is architecturally UNKNOWN on an asynchronous fault, so
  "write" was never a measurement either — only DFAR was previously known to be garbage.
  Every Amiga-side window (FPGA regs at `0x80000000`, PL, IOP peripherals) is excluded
  architecturally: all are `RESERVED` or `STRONG_ORDERED` in core 0's table, and those give
  *synchronous* aborts. Includes a six-candidate table with per-candidate discriminators, the
  electrical-vs-software family assessment, a survivability assessment of the reboot-on-abort
  handler, and seven registered logging discriminators for a second sighting. No firmware
  change.

## 2026-08-25 — an AMIX root is found on the real 060, and boot-menu `D` is unblocked (767af71..e1e98c4)

- **The only RDB scan that can find an AMIX root was unreachable on `bootmode CPU`, and no
  single line was wrong (`767af71`).** Three shipped lines formed a chain that closed:
  `config_file.c:1322` forces `cpu_ram=0` under `amix_mode`, `main.c:1450` disables SCSI boot
  without `cpu_ram`, and `scsi.c:621` gated the per-512-byte-sector scan on `amix_mode` — so on
  the real-CPU path `amix_mode` and `scsiboot` were mutually exclusive and an AMIX root got
  `No RDB found on disk, making up some CHS values.` Proven single-variable on metal
  (BLIZZARD F4-M2): duplicating the RDSK block to sector 0 turned boot 1's insert-disk into
  boot 2's `UNIX_Root (9)` / `UNIX_Swap (9)` / `UNIX_Boot (9)` with nothing else altered.
  The gate was protecting something real — a hybrid image (foreign container or MBR at sector
  0 plus a sub-64K RDB) gets geometry it was never used with on stock, and **this repo ships
  two**: `hdf/Amix_small.hdf` / `_su.hdf`, `conectix` cookie at sector 0, checksum-valid RDSK
  at sector 10. **Replacement key is the disk's own sector 0, not a config flag.** Three
  phases, in `scsi/rdb_scan.h`: sector 0 on magic alone (every AmigaOS disk decided on the
  first read, exactly as before, no checksum added where one could reject a disk that boots
  today); sectors 1..15 — the RDB spec's own location limit — **only** when sector 0 is
  entirely zero and only for a checksum-balancing candidate; then upstream's remaining 64K
  sample points in their old order. `piscsi_parse_rdb()` keeps no scan loop of its own on the
  image-file path, and `_Static_assert`s pin the header's constants to `PISCSI_MAX_BLOCK_SIZE`,
  `RDB_BLOCK_LIMIT` and `RDB_IDENTIFIER`. **Compatibility measured, not argued:** both policies
  replayed against all 41 disk images in the workspace — **15 identical, 26 differ, and every
  differing row is an AMIX root**; nine AmigaOS RDB disks, both `conectix` hybrids, three
  blanks and an ADF are unchanged. Host harness `rdb_scan_test.cpp`, **32 checks**, in a suite
  of **3587**. **Scored on metal the same day:** the lean image `9CE3F6A6` printed
  `[PISCSI] RDB at sector 2 (sector 0 empty).` and the three partitions on a real 68060 —
  `docs/rdb-location-policy.md` §8 predictions 1 and 2 HIT (that run ended in the unrelated
  core0 abort above). The raw-SD path is deliberately untouched and recorded as a known
  asymmetry: it already reads one 512-byte block per index for every disk, so it keeps the
  hybrid exposure phase 2's key removes on the image path.

- **Boot-menu `D` has never worked, and the cause was a truncated pbuf (`45a11a2`).**
  `genericRecvVersionCallback()` copied `p->len` — the **first** pbuf of a chain it then
  acknowledged as `p->tot_len` — and never NUL-terminated the buffer, so a version file that
  arrived split was parsed short. `jed_version.txt` is the longest of the three (67 bytes
  against 57 and 45) and the only one that crossed the split: `checksum32=A6AE44DF` was read as
  `0x00A6AE44`, so every CPLD download was refused as a bad checksum, while `U`/`F`/`T` and `O`
  worked. Measured on metal (BLIZZARD F4-M2:
  `Checksum-32 2796438751 (A6AE44DF) 10923588 (00A6AE44)`) — the *computed* value was right all
  along. Fixed by copying the whole chain with `pbuf_copy_partial()`, bounded and terminated;
  the field parse now fails closed (`version=`, `length=`, `checksum32=` and each `\r`
  terminator were dereferenced unchecked, so a server answering with anything else wrote
  through a NULL), the export copies are bounded to their real buffer size, and the pbuf is
  freed on the reject path, which leaked. **The bug was load-bearing protection**: `D`
  reflashes the CPLD, the one operation on this card that needs a jumper board and a hands-on
  recovery if it goes wrong, and it has been refusing to run since the update system shipped.
  Create `FAILSAFE.bin` (boot menu `J`) first and treat `D` as live from here.

- **Docs (`e1e98c4`).** `docs/rdb-location-policy.md` — the gate chain, what the `amix_mode`
  gate was really protecting, the 41-image census (26 AMIX roots: **all** with an empty sector
  0, RDSK at sector 2; every disk that must not change has *something* at sector 0), the
  old-vs-new differential, harness coverage, and a six-prediction metal pre-registration with
  falsifiers — including the warning that `f4m2-work-root.hdf` now carries RDSK at **both**
  sector 0 and sector 2, so as it stands it exercises phase 1 and not the fix.
  `docs/AMIX.md` + `docs/http-vs-tftp-upload.md` gain the **single-key hazard**: the boot menu
  reads one key at a time, several of them destructive, so a runtime console word typed there
  is executed letter by letter — `DOPC` is `D`, `O`, `P`, `C` (flash the CPLD, overwrite
  `z3660scsi.rom`, start the HTTP server, catch the countdown), and `PSTAT`, `PROF`, `DEMU`,
  `DSCSI`, `SERV` all begin with menu keys too. Console words belong to the running emulator's
  `C` console. Two adjacent traps recorded alongside: the boot-time memory-test prompts drain
  buffered `C` keys, so power-on `C`-spam can miss the menu, and a bare ESC on the serial
  console spins core0 forever.

## 2026-08-24 — the WARP run-loop gate met a second compiler, and it was the gate that was wrong (ca47eac, a474435)

- **`make bootbin` through the Vitis docker failed G-R6.6 on the ARMED loop, and the loop was
  fine (`ca47eac`, `docs/WARP-ENGINE.md` §0.19.5).** gcc 12.2.0 hoists the `z3660_warp_step`
  argument load (`regs.instruction_pc`, displacement 88) between the flag load and the compare,
  where gcc 14.2.1 sinks it below the branch; `tools/warp-gate.sh` matched the gate on three
  **fixed** lines after the handler's `blx r3`, so a scheduled-in instruction moved the compare
  to line 3 and the script reported a loop that plainly engages the engine as having no gate.
  **It was wrong in the dangerous direction too**: pointed at a hooked loop through the
  disarmed symbol on that same image, the old matcher printed `no gate after the handler
  returns -- as designed`; the build failed only because the independent "names no `z3660_warp`
  symbol" test caught it. And the variable was never recorded anywhere — **all ten ELFs the
  campaign archived were built by the host `arm-none-eabi-gcc` 14.2.1** while `CLAUDE.md`
  documents the Vitis docker (gcc 12.2.0) as the build path; not one measured row is a docker
  build, and no manifest row, CRC or build log could have shown it. §0.18.5's `BOOT.BIN` row is
  corrected in two cells — the default variant's loops are **3 204 / 3 284 B**, not 2 100 /
  2 184, which are lean's, transcribed; the CRC, image size and `sizeof(regs)` were right, a
  clean rebuild at `43aa971` reproduces `BOOT-15A7FF0C.elf` byte-identically and passes the
  gate, and G-R6.12 is unaffected. **Nothing about the engine, the loops or any measured row
  moves**: all three variants rebuild byte-identical to their round-6 archive entries
  (`15A7FF0C`, `9AECF99D`, `9D07C731`), the harness is unchanged at 3 334 / 3 330 / 3 561, and
  `M1 PARKED` stands.

- **The gate is now matched by its dataflow, not by one compiler's schedule (`a474435`).** A
  compare against zero whose register was loaded earlier in the window, immediately followed by
  a conditional branch — wherever the scheduler put them. The "exactly one load" rule becomes
  the property it always meant: `warp_on` reached in **one load off a live base**, with no
  address materialised first, so round 3's literal-pool arrangement (−4.056 %) still fails and
  a surplus scheduled load no longer masquerades as it. The disarmed loop is still refused if
  it carries the same dataflow, and still checked independently for naming any `z3660_warp`
  symbol. Every verdict line now names the **producer** that built the ELF (`readelf -p
  .comment`, all GCC strings, deduplicated), because the whole incident was one invisible
  variable. New `make gate` re-reads **every** core1 build directory at once without rebuilding
  anything — the per-variant hook checks an event, not the tree — plumbed through the top-level
  Makefile. `tools/warp-gate.sh --selftest` checks the matcher itself against five windows
  lifted verbatim off real builds: both compilers' schedules of the armed gate, round 3's
  materialised address, the disarmed loop, and a compare fed by no load. No assembler, no ELF,
  no build.

## 2026-08-24 — BLIZZARD F0 fallout: both ROM windows fail closed, and the AutoConfig ghost is not this card (a2ec4bc..43aa971)

- **What F0's two AutoConfig boards actually are (`a2ec4bc`, `43aa971`,
  `docs/real-cpu-autoconfig-and-ext-rom.md`).** The first `bootmode CPU` boot this project has
  ever run listed two boards, and neither was what it looked like.
  - **Board 2 (`Z3 / 5195 / 1 / $10000000 / 128 MB / OK`) is the extended ROM itself**, not the
    FPGA: every one of the four fields is a literal in
    `z3660-drivers/kick31_060/kick31_060.asm`, visible in the shipped 384-byte image at offset
    `0xC5`, hand-built into a `ConfigDev` and handed to `AddConfigDev()` by an `RTF_COLDSTART`
    resident. **Which also proves the extended ROM worked** — the RomTagScanner had to find its
    `$4AFC` matchword and call it — so A1's "the extended kickstart failed to load" premise is
    false in both halves.
  - **Board 2 is therefore NOT Fast RAM**: its `er_Type` is `$93`, `ERTF_MEMLIST` **clear**, and
    `cd_Flags` is `CDF_CONFIGME` with `ConfigBoard()` commented out. It publishes an address
    range — the FPGA's RTG aperture at `$10000000` — not usable memory. **BLIZZARD F0-M5's
    memory-contract question is re-opened**, which is the correction with the most downstream
    consequence.
  - **Board 1 (`Z2 / 15163 ($3B3B) / 200 / 4 MB / Bad`) is an identity this card cannot emit.**
    Manufacturer `0x144B` (5195) is assigned **unconditionally** in both FPGA chains and the
    product byte's high nibble likewise, so no stale or half-programmed chain state can produce
    15163/200; `0x3B3B` appears nowhere in the tree. `4 MB` is the **all-ones** Z2 size code
    (`000` is 8 MB), and the complete enumeration of what this card can advertise is 64 kB,
    128 MB and 256 MB — there is no 4 MB device and no combination of enable bits makes one.
    The ghost reproduced with the same signature in a second session, which **rules out
    randomness, not a no-responder read**: an undriven bus read is deterministic whenever the
    cycles before it are. Revised reading: a **Zorro III end-of-chain read**, which accounts for
    the type bits, the 4 MB, the `Bad` *and* the `$40000000` address the Zorro II reading could
    not; §4's inference from list order is withdrawn (board 2 is appended by a `RTF_COLDSTART`
    resident, after the chain walk). **Consequence: `scsiboot YES` is no longer expected to
    clear it**, and F4 gets a three-boot ladder keyed on the vendor field — anything the card
    emits reads 5195, so a ghost that still reads 15163 is off-card.
  - Also recorded: the real-CPU **`FPGA_RESET` latch-ordering defect** (feature bits are
    assigned only inside the FPGA's reset branch; the emulated path brackets its writes in an
    explicit pulse, the real-CPU path has none of its own and relies on the incidental
    `monitor_switch()` pulse ~160 lines before MAPROM/MAPROMEXT are set) — upstream's WIP note
    at `CHANGES.md:41`, **pre-registered in §6 and deliberately NOT shipped**, because it
    explains neither board and F0 is evidence the current timing worked. And the standing
    caveat on every RTL citation: **the FPGA bitstream is a vendored binary this repo cannot
    rebuild** — the newest `design_1_wrapper.bit` is beta 20 with an internal build stamp of
    2026/02/09, and beta 21 shipped no new bitstream, so `z3660.v` is a *description* of the
    FPGA on the card, not a guarantee.

- **Both ROM windows fail closed, and their tails are zeroed (`37b8a12`).** `load_romext()`
  validated **nothing** — not the size against the 512 KB window, not `f_read`'s byte count,
  not the image id — printed `File read OK` and returned success unconditionally, so
  `main.c:1723` enabled MAPROMEXT regardless. That matters because the window is not a buffer
  the guest reads on request: once the decode is on, **Kickstart scans it** for `$4AFC` resident
  matchwords and calls what it finds, so F0's 384-byte image published ~512 KB of
  uninitialised DDR to the guest's instruction fetch. Now: the size is bounded **before** the
  read (the read is sized by the file, not by the window, so an oversized image overran into
  whatever the ARM map holds above `$00F80000`), the whole window is zeroed before the read,
  and the byte count and the `$1111` extended-ROM id are checked after — a refusal re-zeroes
  the window and leaves the decode **disabled**. `load_rom()` gets the same bound and the same
  pre-zero; it is deliberately not magic-gated, because the kickstart slots legitimately hold
  whole-ROM images of several vintages plus DiagROM. Policy lives in the self-contained
  `Z3660/src/rom_window.h` (no BSP, no FatFs, no UAE types — the pattern of
  `boot_mode_is_emulated()` and `amix_ram.h`), guarded by
  `Z3660_emu/test/host/rom_window_test.cpp`, **31 checks**, including that all four shipped
  `kick060*.rom` sizes are admitted and that a truncated file is diagnosed as truncated rather
  than as a bad id.

- **The floppy hunk parse checks its open (`068706b`).** The open and the seek of
  `1:/z3660_scsi.rom` were both unchecked, so a missing file walked `process_hunks()` into an
  uninitialised stack `FIL` and printed `Unknown hunk type 00000000` — the tree's **only**
  `debug=1` hunk call site, 40 lines away in `main()` from the extended-kickstart load, which is
  exactly how it was misread as an ext-ROM failure during F0. Now it fails closed and names the
  file. `0x2600` is left as found: it is **not** `PISCSI_DRIVER_OFFSET` (`0x1000`) and holds
  zeros in the shipped 16 KB image, so `pifloppy_hreloc` has always been empty and
  `reloc_hunks()` has always relocated nothing, in every mode, on every boot — the right offset
  is an upstream question, not a guess to make here.

- **A real 68060 runs at 100 MHz only — measured, not the documented 50–100 (`2e1c4e5`,
  `KNOWN_ISSUES.md` `fork:amix` section).** Single-variable A/B on metal, one preset, only
  `cpufreq` changed: **100** boots KS 3.2.3 from reset to a full Intuition desktop with a live
  mouse; **50** gives a black HDMI output and zero serial past the ARM banner, held 13 minutes.
  That corroborates `CPLD/README.md`'s *"all firmware here is working only at 100 MHz … only
  some 'magic' frequencies work"* over the user-facing range. Noted alongside: the code does not
  implement the range the section states — `configure_clk()` clamps to `[50,120]` and then
  replaces the request with a table entry, so **the configured number is not the applied one**;
  read the `Clock index %d` line back from the serial. Says nothing about emulated modes, where
  the 060 is a bus-parked passenger.

## 2026-08-24 — Plan WARP ENGINE: M1 built over six rounds, scored, and PARKED at generation-one scope (ffda114..e32056a)

A JIT under the guest's 68040 MMU, taken from a plumbing slice to a working generation-one
engine in one day, six pre-registered rounds, every prediction scored beside itself and the
four wrong ones left unrewritten. **The kill line never became entitled to fire** — its
precondition is armed throughput measured at coverage ≥ 60 %, and coverage measured
**8.43 % → 19.10 % → 20.26 %** across three metal waves. The blessing row at the end flashed
and missed its gate by 41 (see below); the card's standing image remains `F294E75A`.

- **Round 1 — the plumbing slice (`d55db48`, `ffda114`, `072e4f0`, `b16967d`).** Blocks are
  discovered, cached, entered and bailed with **no code generated**: every block is a null
  block that syncs nothing and returns at the pc it was entered at, so the switch, the block
  cache, the invalidation wiring, the cadence feed and the bail accounting are proven before a
  compiler exists. Coverage is 0 by construction and no performance claim is made. `WARP`
  console knob at `0xFFFF00F0`, unset = OFF, exact-magic arm, republished every boot; counters
  47..59, id 46 stays retired. Corpus: armed and disarmed run the same 10⁷ guest instructions
  and are compared after every one of them, `WARP_DIVERGE` 0; five mutations (M-5, M-7,
  M-ICACHE, M-ARM, M-REPUB) each have an arm shown failing. `docs/warp-objdiff.py` compares two
  builds at function granularity with relocation normalised away — whole-image byte identity is
  the wrong test after an add. **Scored:** five gates hit, 12 functions changed in the disarmed
  lean image; the entry cost missed high — 60 ARM instructions on the armed null-block path
  against a pre-registered 24–40, ~22 of it named and none of it fixed in the round that
  measured it.

- **Round 2 — generation one compiles, and the lookup thrash (`219fd48`, `c129842`, `0ec5c13`,
  `a392fbe`).** Blocks are discovered at control-transfer targets only, so the entry amortises;
  the table becomes 2048×4 with the high pc bits in the index — round 1 folded every head into a
  2 KiB window and recompiled **3.07 M times**. Emitters for MOVE/MOVEA, register ALU, Bcc and
  DBcc on a memory-resident register file, so a bail materialises the pc and nothing else;
  memory goes through one helper holding the verdict-table fast path. `WARP_EVICT` appended as
  id 60. The harness gains an **ARM-subset executor**, so the armed arm executes the same bytes
  the target will: every opcode the compiler accepts (7 616) run both ways from six seeded
  states — 45 696 pairs — which found the write-back MOVE's restart bookkeeping, a refused
  opcode leaking its post-state, and a wrong per-site bail pc. **Scored:** G-R2.3 clean over 10⁸
  boundaries at 100 % coverage; G-R2.10 compiles once per head where round 1 compiled once per
  execution; G-R2.8 missed high again at **108 ARM instructions per block** against 26–38, and
  the disarmed gate did not move off round 1's five.

- **Round 3 — the publish range, the fidelity gap, and a self-test on the card (`5180db1`,
  `052a40c`, `c23c399`, `8cf3fc8`, `1adb64c`).** Root cause of the armed engine dying on
  silicon, named before it was fixed: **the CP15 publish sequence rounds its LENGTH up and never
  rounds its ADDRESS down**, so a block whose bytes reach past the last line the loop touches is
  executed from stale memory. Round 1's 8-byte blocks could not reach it; round 2's hit it
  **43.75 %** of the time — and the harness never saw it because `warp_publish()` is compiled out
  of the harness. That is the fidelity gap the round is named for. Fixed by rounding the range
  out at WARP's call site, with a bump keeping every block on its own line; the coverage
  predicate reads **28 of 64** offset/length pairs uncovered on round 2's geometry and **64 of
  64** after. **`WARPTEST`** compiles real blocks on the card, publishes them, executes them and
  compares against the same words interpreted; it refuses a block that would call a memory
  helper, because the self-test hands generated code NULL for the two helper registers and a
  `blx` would branch to zero on a live guest's card. `m68k_run_mmu040` 2 548 → 2 196 B, disarmed
  gate 5 → 4; object equivalence missed high at 208 functions, **205 of them from taking one hot
  function out of line under `-flto`**. Images `C8224D10` / `C89953E2` / `D9BD4449`.

- **The build-integrity finding — a flown image that is not what its source builds (`64d66dd`,
  `6d75520`, `69c37ca`).** `CFLAGS` has carried `-MMD` since C1 and **nothing ever read the
  output**, so a header edit left every object stale: round 3 shipped a run loop its own source
  does not build — 2 196 B as flown, 2 380 B rebuilt clean. Fixed by `-include`ing the `.d`
  files; like for like the round-4 gate is 2 372 B. The re-derivation later moved the finding
  onto the right round, proven on the archived artifact: **round 3's source reproduces its image
  exactly, round 4's does not** — so the three-instruction gate designed for round 4 was scored
  MISSED and never actually flown. Consequence adopted for good: `archive_image` now copies the
  **core1 emulator ELF** to the same CRC-stamped basename beside every `.BIN`, because two
  build-integrity incidents hit a flown image whose source could no longer be objdumped once its
  build dir's ELF was overwritten.

- **M1a round 4 — families, and the churn attributed (`6afab03`, `1019b15`, `42a7831`).** 87 % of
  the discard-all events are CPUSHL/CINV and PFLUSHA, at **342 executions per compile**, so
  `DISC ≈ COMPILE` is an identity and not churn; split into `DISC_EV` / `DISC_ATC` /
  `DISC_CPUSH`. Tranche 1 completes generation one's own frozen scope — MOVEQ, TST, the
  shift/rotate register forms, immediate and memory-source ALU, and `(An)+` sources, the
  auto-adjust SOURCE compiled **only** with a register destination because the write-back is
  after the access, so one faultable site restarts cleanly. The disarmed gate moves into `regs`:
  one load off a base the loop already holds, not two dependent loads. Eight new mutation arms,
  all caught — the arm overrides had to widen to 64-bit and gain an A7 field or three of them
  could not bite at all — and the harness now writes its accept set for `docs/warp-corpus.py
  --compare`.

- **M1a round 5 — the ablation kit, the bail split, tranche 2 (`d152a50`, `370e76e`, `e30f0fd`,
  `cf62579`, `5c75a27`, `335698f`, `ae566c8`, `6d75520`).** Objdump-guess-then-measure is retired
  after two refutations on metal: instead, **five lean variants from one tree, each missing one
  layer, plus a negative control whose layer is provably off the disarmed path**, and
  `tools/warp-gate.sh` reads the gate off the built ELF **by shape rather than by position** —
  a level that removes the hook has no gate, and printing whatever follows the handler's `blx`
  under that heading is the plausible wrong answer the script exists to stop. `WARP_BAIL` splits
  by cause, ids 64..68, **dump version stays 2**; `BAIL_OTHER` is pre-registered at zero as a
  tripwire, not a statistic. Tranche 2 adds LEA, CLR, EXT/EXTB and the **1 152** register-count
  shift forms (ARM's register-specified shift already implements the 68k rule for every count
  ≥ 1, so each shape is its immediate body plus a branch-free zero-count tail; ASL gets a clamp
  at 31; indexed LEA modes are refused, a full-format extension can reach memory). Sweep:
  **18 368** accepted opcodes agree with the independent model exactly, **110 208** differential
  pairs, **0** divergences; the register-count boundaries get their own arm because the sweep's
  seeds cannot reach a count of 0, 32 or 63 on purpose. Eleven of twelve gates hit.

- **The round-5 metal row — the first named mechanism, and two refutations (`17000f9`).**
  2026-08-24 11:55–13:34 EEST on the A4000 + Z3660, preset 7 (`amix_ram 128`,
  `service_cadence 4`, `arm_frequency 1100`): eight flashes, eight guest boots, 45 scored
  dhrystone rows across nine arms, two `WARPTEST` gates, two `PROFZ`-fenced counter windows.
  Anchor `R` = **8 185.0**. A0 disarmed **7 903**, penalty **−3.445 %** — the ≥ 7 988 blessing
  class **MISSED for the third consecutive round**, so the card did not ship it and
  `0:/Z3660.bin` stayed on the clean interpreter lineage `F294E75A`.
  - Ablation floor `|A0 − A2|` = **0.513 pp**; no edge below it may be read as a mechanism.
  - Largest single-layer edge is **`L-HOOK`**, the run-loop gate and the loop body around it, at
    **+1.967 pp = 57.1 %** of the penalty. It clears the applied bar (1.7227 pp) and misses the
    pre-registered one (2.128 pp) by **0.161 pp**; **both are reported**, because a naming
    verdict resting on a 0.161 pp margin against a 0.513 pp floor is an attribution and not a
    claim. `L-REG`, the pre-registered favourite, measured **0.929 pp** against a predicted
    1.8–3.4 pp — **refuted in public**, as the pre-registration asked. The layers add
    (additivity holds inside 2× the floor).
  - Coverage **0.20263** against a pre-registered 0.20–0.28 (HIT, and the kill line's
    precondition unmet a third time); armed **7 563 = 0.92401×** of `R`; the identity
    `(A0/R)×(armed/A0) = armed/R` closes to **five digits**. All six counter bands HIT.
  - Bail split: `WARP_BAIL` 9 207 252 = STUB 9 205 768 + MEM 1 484 + OTHER 0, exact. The
    memory-helper candidate for round 4's bail-vs-decline gap is **refuted at three digits** —
    1 484 bails in 9.2 million, roughly 1 875× below its own falsifier; essentially every bail
    is a cached decline stub. `WARP_BAIL_FENCE` never fired on metal, so that identity is exact
    over a path silicon has not exercised — flagged rather than absorbed.
  - `WARPTEST` **PASS 17/17** on both images, all four new families passing on silicon at the
    first attempt; `FAULTS(on) == FAULTS(off)` exactly (591 == 591); budget closure
    unattributed **0.000 %** against a ≤ 5 % gate.

- **The generation-two prospectus — priced, and recommending nothing (`847ab22`, `ffa8430`,
  `e32056a`).** `docs/warp-corpus.py --prospectus` reports the gen-5 exclusion histogram, a
  candidate family ladder, chain run lengths between indirect transfers and a block model
  calibrated against three metal rows; the candidates live in `accept_gen2()`, outside
  `accept()`, so no gate, bitmap or `--compare` can reach them and the scored gen-3/4/5 outputs
  are byte-identical. `docs/warp-gen2-prospectus.md` prices park, park + tax fix, the write-back
  contract, chaining and supervisor coverage against the R5 rows and ends in a decision table
  with **no recommendation**. Its arithmetic says one thing loudly: at the measured block-entry
  cost `E` = 112 cycles, full G2c ceilings at **1.131×** and the first-generation band of
  1.25×–1.55× is out of reach at any coverage priced, while `E` = 40 alone reaches **1.209×**
  and `E` = 0 reaches **1.257×** — **an E-round comes first and families come second**.

- **Round 6 — WARP ships free (`bed3e41`, `ae7322c`, `b906d95`, `99adc71`).** The dormant tax is
  removed rather than explained: the loop body moves to `newcpu_mmu040_body.h` and is
  `#include`d twice, the loop selected **once** at the arm/disarm transition via
  `SPCFLAG_MODE_CHANGE` and `m68k_go`'s own `run_func` choice, and `warp_on` moves to the **end**
  of `struct regstruct` so no existing member's displacement moves. Read off the ELF: selector
  28 B; disarmed `m68k_run_mmu040_nowarp` **2 100 B with no gate and no `z3660_warp` symbol
  anywhere**; armed `m68k_run_mmu040_warp` 2 184 B with one load off a live base and zero
  materialisations; `sizeof(struct regstruct)` 464 → 472, `warp_on` at displacement 464 against
  the `imm12` limit of 4 095, asserted at build time.
  - **The disarmed run loop is normalised-IDENTICAL — 0 instruction differences — to the run loop
    of an image built with the engine entirely absent**, checked against all five ablation
    variants. The layout term collapses **7 912 → 466** changed bodies, a 17× fall onto the
    toolchain's ambient-churn class, and the ablation edge that carried it now moves nine
    functions with `.text` identical on both sides.
  - Eleven of twelve gates hit. **G-R6.7 missed and is characterised to the instruction**: two
    spill slots swapped (a renaming), one basic-block reorder that folds a `b`+`bne` into a
    `beq` and is one instruction shorter, and one register-allocation choice on the
    `z3660_blk_probe` call — call-target census identical, nothing on the per-dispatch path.
    *The fix reproduces A4's code, not A4's binary.*
  - One finding the fix produced on its own first build, and it is an inlining one: splitting the
    loop in two gave `z3660_blk_run` a **second caller**, and `-Os` immediately stopped inlining
    it — putting a call and a return on rung 0's recognizer path, in both loops. Restored with
    `always_inline`, by construction rather than by a heuristic whose input the round
    deliberately changed.
  - `tools/warp-gate.sh` becomes a **build gate**: it reads both loops off the linked ELF and
    exits non-zero if the disarmed one carries a gate or names `z3660_warp` at all, and every
    core1 target calls it, so a violating image cannot be built. It was fired on purpose against
    the hooked loop and exited 1. `warp-objdiff.py` gains `--alias` / `--only` / `--self` so a
    renamed body can be compared with the body it replaced.

- **The blessing row — the gate missed by 41, and the code exonerated (evidence
  `2026-08-24-warp-r6-blessing`).** The tax-fixed lean measured **7 947** against the ≥ 7 988
  gate (band 8 100–8 163 missed low by 153) — §0.17.2 rule 3 fires by its own words and the row
  is not re-banded. The same session's floor variant — the **same tree** minus 64 B of cold
  attestation `.text` — measured **8 130, inside the band, clearing the gate by 142**; and a
  closing re-flight of the reference read 8 174.0 against an opening 8 174.0, **zero drift**
  across 55 minutes and four flashes. The 183-dhrystone gap between two near-identical binaries
  is therefore a property of the binaries — link layout, the ±0.5 pp class this span already
  named — so the fix's **code** is vindicated and the nominated **binary** drew a losing layout.
  No rescue round was taken. Armed for the record: 7 100, with all six counter bands HIT and the
  engine's work identical to five significant figures — the miss is wall clock, not the engine.
  **The card's standing image remains the clean interpreter lineage `F294E75A`** (fourth
  consecutive rollback, on-card CRC verified); `WARPTEST` PASS 17/17 on everything flown.

- **M1 CLOSED — PARKED at generation-one scope, by user decision.** Final scorecard: armed trend
  over four rounds **5 301 → 7 185 → 7 536 → 7 563** dhrystones (round 2's 5 301 is the
  null-block row — the entry sequence's arrival in full, with no code generated), ending at
  **0.92401×** of its own session's reference — rising, and never crossing 1.0×. Coverage
  8.43 % → 19.10 % → **20.26 %**: generation one's frozen scope has a ceiling around a quarter of
  this workload, and the 60 % bar is a different engine, not three more families.
  **Correctness: five metal sessions, zero correctness failures** — `WARPTEST` 17/17 on every
  image it ran on, `WARP_DIVERGE` 0 over 10⁸ retired instructions every round, `FAULTS(on) ==
  FAULTS(off)` exactly, budget closure 0.000 % unattributed, and an accept set of 18 368 opcodes
  agreeing with its independent model on 65 536 of 65 536 words. The dormant tax was measured at
  −3.445 %, **bisected** and **removed**. Refuted in public along the way: three named mechanisms
  for the disarm residual (L1I footprint, dependency depth, and the pre-registered favourite
  `L-REG`) plus the memory-helper bail candidate. The park preserves everything — the engine
  compiled in and OFF, `WARPTEST`, the accept set, the ablation kit, the build gate, and every
  pre-registration unrewritten including the four times it was wrong.

- **Image lineage at the park.** Eight images built from one tree after `clean-all`, all
  CRC-stamped and each with its core1 ELF archived beside it: `BOOT.BIN` `15A7FF0C`,
  **`BOOT-lean.BIN` `9AECF99D` (12 680 156 B, `.text` 6 347 084) — the shipping lean**,
  `BOOT-prof.BIN` `9D07C731`, ablation `BFC9FC47` / `E2C1FBC5` / `82D6B35C` / `C8AAB523` /
  `BEC0D2AA`, and `BOOT-q2probe.BIN` untouched at `FCC0CB3F`. The second run loop's price,
  measured rather than estimated: `.text` +2 180 B on a 12.68 MB image. **`9AECF99D` is verified
  to be what its source builds** — a `clean-all` and rebuild produce a core1 ELF byte-identical
  to the archived one — which is the property round 4's flown image did not have. After the
  blessing row missed its gate (above), the standing card image is `F294E75A`; `9AECF99D`
  remains the buildable shipping lean in-repo.

- **Build robustness (`b117ffd`).** `BOOTGEN_EXE_HOST` only checked PATH and a `$HOME/git`
  checkout, both literal strings picked regardless of whether the file existed, so a host with
  neither gave `Error 127`. Each candidate is existence-checked now, and a third fallback wraps
  the `z3660-build:latest` image's `mkbootimage` in a one-shot `docker run` for just that step,
  bind-mounting `CURDIR` so the existing relative bif/output paths resolve unchanged.

## 2026-08-23 — C2 rungs 1–3, the HANDLER experiment, the f measurement, and Plan WARP ENGINE opens (8e68da1..d55db48)

The interpreter-optimisation campaign's decisive day: one rung shipped at **+25.17 %**, one
narrowing decomposed into a keep and two reverts, one rung built and **reverted at −0.05 %**, one
experiment reverted **OFF-SCALE**, and one study **struck on its own measurement** — then the JIT
plan that the ladder's remainder justifies.

- **Rung 1 — the decoded-op cache (`8e68da1`).** A PC-indexed cache consulted at the top of
  `m68k_run_mmu040`, before `x_prefetch`, removing the fetch and the decode for any pc it holds.
  Instruction fetch was 78.2 ARM cycles per opcode word and 85.2 per extension word at 1.5983
  fetches per guest instruction — 28.65 % of the interpreter. **Sized from the measurement**: the
  PC distribution over three lossless rings puts 2048 entries at 79.55 % of the IO workload's
  busy time and 4096 at 90.40 %, so 4096 entries, 4-way, 16 B/entry, 64 KB, as a compile-time
  assertion. A hit reaches its dispatch in **50 instructions with no call and no guest-memory
  read**, against 104 for the uncached fetch, and comes out of the same cache line as the tag.
  The **idiom verdict rides in the entry**, so rung 0's recognizer is not called at all on a hit
  that says "not an idiom" — a `tst` and a `beq` in place of 14 instructions — which is what
  makes this rung pay for rung 0's dhrystone tax instead of adding to it. One extension word is
  served through an address-keyed window the run loop publishes (10 ARM instructions against 47
  plus a callee and a DRAM read). Coherence is deliberately blunt and asymmetric: one generation
  increment on every event that could plausibly matter, hung on `mmu_flush_cache()` because that
  is exactly where the **architecture** already requires a guest to act before executing code it
  wrote or remapped — plus MOVEC to URP/SRP/CACR, `build_cpufunctbl`, `map_banks`, the knob, and
  **the board-specific DMA arm** (core0 reads disk sectors straight into guest DRAM and the
  interpreter never sees those bytes go past; the consumer decides, from what it already knows,
  because a producer-side epoch word would fail *dangerously* against an older core0). The
  empty-way marker is set-relative and that cost a crash to learn — key 1 is "pc 0 in supervisor
  mode", which a guest reaches through an unplanted vector. Counters `DOPC_HIT`/`MISS`/`EXT`/
  `INVAL`, ids 24–27; harness 1594 → 1767 / 1763 / 1872 (default / lean / prof).
  **Metal:** the ship-vs-ship number is **+23.31 %**, not the +41.57 % the ON/OFF switch reads —
  the lean OFF arm is 12.90 % below the pre-rung-1 build, so the switch is not a free fallback.

- **Rung 1b — the narrowing, and the wall that shapes it (`738e9b4`, `fcf0701`).** The misses are
  invalidation **re-warm**, not capacity: 274–292 misses per invalidation across boot, dhrystone
  and raw disk — a near-constant across three workloads with entirely different code footprints,
  and far below the 4096 entries held. Attribution first: one counter per cause, ids 28–37,
  locked to the cause enum by a compile-time tripwire, with `FLUSH` narrowed to CPUSHL/CINV alone
  and `ROOT` to URP/SRP alone. **The first finding is what cannot be narrowed**: this cache is
  tagged by guest *logical* address and the two largest causes name *physical* ones — CPUSHL's
  operand (the 68040's caches are physically tagged, which is why upstream's `flush_mmu040()`
  discards it) and the DMA arm's range. Narrowed anyway: the empty-cache short-circuit, a PFLUSH
  page-presence filter (4096 bits over a hashed 4 KB granule — a tag walk was the obvious
  alternative and is the wrong one, since a 4 KB page spans all 1024 sets, i.e. 32 KB of tag
  lines, the whole L1 D-cache, to save re-warming ~285 entries), and MOVEC-to-CACR restricted to
  writes that change the register. The hit path is **byte-identical** to rung 1 and the image
  grew 8 bytes.

- **Rung 1c — one arm kept, two reverted (`237033a`, `a1a360a`, `590c2d0`).** Rung 1b's own
  pre-registered revert line fired — dhrystone lean cadence 1 came back **5306.0** against the
  5432.0 it had to beat — and the attribution counters are what turned a blanket revert into a
  decomposition. **KEPT:** the CACR narrowing, one compare at one call site, which removed ~93 %
  of every invalidation 1b removed (~41 000 of 44 247). **REVERTED:** the short-circuit, not
  because it was wrong but because its justification was — the 18× invalidation storm it absorbed
  was measured with rung 0's recognizer OFF, and with BLK ON the fast path already collapses
  AMIX's CPUSHL loop into one `flush_mmu040()`. **REVERTED:** the PFLUSH filter, sound and
  mutation-tested, aimed at a cause worth 6–9 % of requests, paying two hashed marks and a flag
  store on every fill. Why a correct narrowing still lost: **+7.08 pp of hit rate bought
  −1.05 pp of speedup** — the misses it removed were re-warm misses, still hot in ipagecache
  (91.61 %) and in the ARM's caches, i.e. the cheap ones. The re-warm diagnosis itself is
  **confirmed**; the falsifier was mis-specified (the ratio has the invalidation count in its
  denominator, and the predicted ×1.506 move matched the measured 413.2 to three figures).
  **Scored:** dhrystone cadence-1 lean **5584.0** (bar ≥ 5432, **+2.80 %**), `DOPC_INVAL` 39 858
  with `IV_CACR_SKIP` 39 353 and `sum(IV_*) == DOPC_INVAL` exactly, boot window 52 s, hit rates
  85.32 / 72.83 / 75.53 — but the hit-rate bands are **refuted rather than narrowly missed**, and
  the law they bought is recorded: **the hit rate is not what pays, the fill-path cost is.**
  Across three measured configurations hit rate and speed do not track each other at all.

- **Rung 2 — the accessor fast path, and the call nobody had counted (`f4f14dd`, `153c4d3`,
  `d5c3d6a`, `9c11ee0`).** READ + WRITE is 19.59–20.35 % of the interpreter at cadence 1 at a
  dpagecache hit rate of 99.21 / 99.35 %, and a guest data access still costs 127.6 corrected ARM
  cycles (read) / 142.1 (write) — so the cost is the scaffolding around a hit, not translation.
  **The measurement that redirected the design, taken before anything was written:** READ counts
  39 738 605 data reads while `DPAGE_RHIT + DPAGE_RMISS` is 27 760 860 — 30 % *below* a figure
  that should exceed it. The only exit before the page cache is a **transparent-translation
  window match**, and the AMIX-040 kernel's own `pstart040.s` says why — ITT0 covers 0–1 GB of
  code at both privilege levels, DTT0 the same for data, DTT1 `0x80000000+` supervisor-only — so
  **every supervisor data access is transparently translated and never touches the page cache**,
  while user accesses fall through both windows to it, and `mmu_ttr_enabled` being true for the
  whole run means `mmu_match_ttr()` is called **out of line on 100 % of data accesses** (~39
  instructions supervisor, ~48 user; ~55/69 for a write, wrapped in a second out-of-line call —
  which is also the mechanism behind the map's unexplained read-vs-write gap).
  - **The design is one derived table, not a new cache.** A 68040 TTR match depends on the
    address only through its top 8 bits plus the privilege level and four registers, so the whole
    trichotomy (`SLOW` / `XPAR` / `PAGE`) fits in a 2×256 table of 16-bit verdicts per direction —
    2 KB, **one load**, replacing the call chain. It holds no translation, no physical address and
    no guest data; it is rebuilt at exactly the sites that recompute those flags, so it cannot be
    stale in any way `mmu_ttr_enabled` is not. The PAGE arm uses the *existing* page-cache index
    and tag arithmetic byte for byte, which is how "the hit rates must not move" is guaranteed
    rather than hoped for. Every other arm returns 0 and the caller runs the original accessor
    verbatim, so no fault path, write-back contract or device-bank side effect is reimplemented.
    It never fast-paths across a page boundary — `is_unaligned_bus()` is tested first, so
    everything reaching the resolver is naturally aligned, structurally rather than by a check.
  - **Counted out of the lean objdump along the actual path:** read PAGE 110 → 48, read XPAR
    76 → 33, write PAGE 138 → 51, write XPAR 97 → 33. **All calls removed** — no `bl`, no `blx`,
    no indirect branch remains — and `m68k_run_mmu040` is unchanged at 572, so the I-cache
    regression the map warned about did not happen. The `always_inline` **is** the rung: at `-Os`
    gcc first declined it and emitted the resolver as an out-of-line call, found by reading the
    generated ARM.
  - **Evidence:** the verdict table replayed **exhaustively** — 12 register configurations ×
    MMU on/off × both privilege levels × all 256 top bytes = 12 288 points against the live
    matcher — plus a state-equivalence corpus run disarmed then armed with **engagement pinned on
    every case**, and a case that drives seven real 68k instructions through the run loop's own
    dispatch (measuring the *second*, warm pass, because a running guest is warm and a cold pass
    would have pinned engagement at 1). The corpus earned its keep on its first run, failing
    twice on the page-cache counters, both real defects. One pre-existing hole closed on the way:
    `restore_cpu()` writes all four TT registers and never called `mmu_tt_modified()`. Harness
    1800 → 3147 / 3143 / 3345, later 3186 / 3182 / 3384.
  - **Metal, and three claims corrected (`9c11ee0`).** "The disarm costs nothing on the hot path"
    is **refuted at about 3.7 %** — with every verdict SLOW an access still pays the table load
    and the branch and *then* the unmodified accessor. The consequence is exact:
    1.2517 × 1.0388 = 1.3003, the same-boot A/B ratio to four digits, so **the shipping number is
    +25.17 %, not +30.03 %** — two honest numbers answering different questions, and the doc had
    quoted neither. The 30/70 XPAR weighting is not a constant (`DFAST_XPAR` measures 22.63 % /
    24.07 %). Finding **F6** recorded as open: `DOPCFIND` cost +15.5 % more per dispatch after
    this rung against a ±3 % pre-registration, with two candidates the session cannot separate.
    The misalignment asymmetry is **closed** as a denominator defect — `MISALIGN_R` is
    incremented at three sites including the instruction-stream fetch, and the instruction stream
    misaligns by construction — new counter `MISALIGN_I` (43) settles it by subtraction, leaving
    1.43× read:write.

- **Rung 3 — the instruction fetch resolves inline too, and it does not pay (`3087af9`,
  `7f38e7b`, `95e6171`, `905d177`).** Same table aimed the other way: after rung 2,
  FETCHOP+FETCHEX was the largest attackable group left, and the capture priced the population —
  **53.3 M of 61.3 M instruction fetches (87 %) are ITT-matched**. One more 2×256 table (1 KB,
  one direction), built by the same replay at the same two rebuild sites, with three differences
  that are all consequences of what a fetch is: `likely()` points at XPAR not PAGE; `TTR_NO_WRITE`
  is XPAR not SLOW (a fetch is a read, and the corpus asserts that on a live fetch so a future
  copy-paste from the data table fails a test rather than a boot); and the PAGE arm is the
  one-entry instruction page cache, reading `atc_last_ins_laddr` directly so invalidation needs
  no hook. The fast path gates *itself* on `!(addr & 1)` so "a served access cannot cross a page
  or a bank window" stays structural — a word at page_end−1, served, would read its second byte
  out of an unrelated frame, which is the cdfs page-crossing DMA in a hotter loop. Counted: iword
  XPAR 79 → 37, iword PAGE 96 → 55, ilong XPAR 73 → 30, ilong PAGE 90 → 48, ~42 instructions per
  served fetch on every arm; `.text` +944 B, `.bss` +1 KB, run loop unchanged.
  **Pre-registered +7.8 .. +8.7 %. Metal measured 91.10 % coverage, the fetch bucket 23 % cheaper
  per guest instruction — and cross-build dhrystone cadence 4 at −0.05 %. REVERTED (`7f38e7b`).**
  Every input premise came in at or better than assumed: **the miss is the counts-to-wall-clock
  conversion, not the counts.** The rung removed latency the decoded-op cache was already hiding,
  and the dependency it added is prepended and never overlaps. `docs/ifetch-fastpath.md` is kept
  and the design survives in the reverted commit; F6's D-cache candidate is demoted by the third
  table. A companion pricing correction lands with it: the price bracket moved [57,68] → [54,66]
  → [39,50] in five hours, so the "in-bucket end is always impossible" streak was the bracket's
  range and not a law — **a capture is priced from its own boot line, never from the document**,
  and cross-session cyc/dispatch at a price only one capture measured charges them different
  instruments, which is half of what F6 rested on.

- **The HANDLER discriminator — pre-registered, built, measured OFF-SCALE, reverted (`d7f5470`,
  `87d3f49`, `40fe568`, `2fb073c`, `363a508`).** An experiment, not a rung: covered MOVE/MOVEA
  shapes dispatch through an inline body instead of the generated handler, on a verdict bit in
  the decoded-op entry. The pre-registration was committed **before the code**, and the as-built
  charge was recorded beside it when register pressure from inlining the body spilled the meta
  byte and the disarm penalty came out **+5, not +2**. **Metal: cross-build lean cadence-4
  dhrystone 8197 → 7578, −7.55 %, at 21.12 % coverage — the rung fired and lost**, because the
  specialised body is 2–3× the generated handler path it replaces. The bands are refuted **as
  built**, so HANDLER remains unclassified; the machinery goes, the gate arithmetic is recorded
  (zero fetch-accessor calls, body shorter than the handler — unreachable inside the decoded-op
  entry's fields), and the reverted tree rebuilds `DEC21748` byte for byte.

- **The instruments the verdicts needed (`562c721`, `c82b275`, `55ee6c2`, `47bd901`, `6afc8f9`,
  `004aa6d`, `88e88b4`).** The dispatch loop is decomposed — buckets 14/15/16 = `DOPCFIND` (the
  lookup, closed on both arms *before* the fetch, so it measures the lookup and not what a miss
  costs), `DOPCFILL` (the miss path only) and `BLKREC` (rung 0's recognizer) — with id 0 renamed
  to print as `LOOPRES`, because the name moves with the meaning rather than the version being
  bumped. That closed the interval the map was owed: **LOOP 24.80 % @39 .. 25.11 % @50**, a
  0.31 pp bracket in place of 33.90–39.71 % of *upper bounds*, both ends of which were wrong and
  biased rather than conservative. Four way counters (39–42) measure where hits land, with the
  decision rule pre-registered and **printed by the firmware as its own verdict line**. Three
  statements the map found wrong in the instrument were fixed rather than left to keep firing on
  correct builds: the `DOPCFILL` identity needs a fault term (a miss whose opcode fetch faults is
  counted and never reaches the fill — the shortfall measured 1349 against FAULTS 3632 and 1348
  against 3627, reproducing to within one count), `tcnt[BLKREC] ≤ tcnt[DOPCFIND]` is a claim about
  *calls* compared over *spans*, and the price bracket now carries a **per-capture ceiling**
  `min_b(acc/spans)` that the dump warns about rather than silently clamping. And the way
  histogram printed the number that inverts its own verdict (`47bd901`): five values through a
  four-slot rotating format ring, so the fifth overwrote the first and the line printed
  `DOPC_HIT` in way0's place — a reader taking it at face value computes way0 = 100 %, crosses the
  pre-registered 85 % threshold and **strikes the rung the firmware's own verdict line kept**.
  The real way0 is 62.50 %. Fixed with a caller-owned buffer, and guarded by a test that re-reads
  the source and fails if any `z3660_printf()` argument list uses more ring-form calls than the
  ring has slots — no compiler can see this, since the broken form is legal C with correct types.

- **The f measurement — bought as a removal, and it struck its own study (`a3af6b1`, `58d9acb`,
  `5b3e32e`, `7ace17d`, `a967977`, `f5c80dc`, `88e88b4`).** The one number left unmeasured before
  a physically-indexed decoded-op cache could be funded was `f`, and it was bought as a
  **deliberately unsound** switch: armed, `IVSUP` counts CPUSHL/CINV and does not perform them,
  so the cache keeps entries the guest told it to drop — a no-op invalidates a superset of what a
  physical index would, making one boot's hit-rate response a **strict upper bound** on `f`.
  Default OFF, magic-word arm, console only, no config key, core0 republishing the OFF sentinel
  every boot because the mailbox survives a warm reset, a banner on arm and on every dump, and
  `IV_SUPPRESSED` (id 46) as the capture's confession. **Metal verdict:
  f = (0.03638844 − 0.03352485)/0.03638844 = 0.0787 against a gate of 0.35**, with the wall clock
  moving +0.00 % against +1.5 % — two instruments, one null. `T*` re-derived at the measured `f`
  is 14.2–15.3 cyc against a 93–99 bar, and it is an upper bound. Direction table 15 PASS /
  2 NOTE / 0 MISS, identities exact on both arms, correctness gate byte-identical to the
  reference block. **The physical-index round is struck**, so the unsound machinery is removed
  entirely (console command, mailbox word, suppression, banners, counter and corpus — SHARED back
  to `0xF0`, 46 counters again); `docs/f-measurement.md` keeps the record. One fix stays: the
  generation wrap now raises `IV_WRAP` at the wrap arm itself — `do_invalidate()`'s memset was
  silent, so the counter only ever moved when a caller typed the id out — and a wrap adds to
  `sum(IV_*)` without adding to `DOPC_INVAL`, so both identities subtract it. The identity's
  `IV_WRAP` term is then made load-bearing: dropping it left the check passing because the window
  carried no wrap, so one wrap is now driven into it through a real call site.

- **Every image archived, CRC-stamped (`76fa64b`, `621b5fd`).** Each variant is written to a
  fixed filename, so building the next one overwrites the last — and on 2026-08-23 a rebuild
  overwrote `BOOT-lean.BIN` and crc `DEC21748`, the reference arm that day's A/B was judged
  against and still live on the card, which existed nowhere on the host afterwards. One `cp` per
  build into `sd_card/archive/`, CRC-stamped with the same zlib crc32 the boot menu's `CRC`
  command prints, untracked and never pruned. The reverted rung-3 build then reproduces
  `DEC21748` byte-identically, which is the check that closes the incident.

- **Documents scored against their own metal (`a216b7d`, `a1a360a`, `590c2d0`, `10a52ab`,
  `0f087b4`, `fead1c7`, `ca7ce30`, `004aa6d`).** Corrections, not softenings. The decoded-op
  doc named the **wrong test** (`DOPC_INVAL` against `DOPC_MISS` is not commensurable — one
  whole-cache invalidation can cost up to 4096 refills — and it read 0.34–0.37 % on every workload
  while the misses *were* self-inflicted) and claimed a property it never had (the invalidator
  took no on/off test at all). The block-idiom doc carried a bench guard **no correct
  implementation could ever have passed**: 22.08 % × (1 − 0.80) = 4.42 % against a "below 3 %"
  threshold, i.e. an implied 86.4 % reclaim; metal read 4.46 % of *busy* boot time — a 79.8 %
  reclaim, the model confirmed to within 0.2 points — and was correctly scored a MISS. Corrected
  to below 5 % of busy boot time, with the threshold → implied-reclaim table beside it, and with
  "busy" made explicit because a share over the whole ring is diluted by however much idle the
  boot happened to contain. The same doc's utilisation ratio was wrong in a way that put the
  answer outside the judgeable range (`BLK_INSNS` is not a subset of `INSNS`; the guest stream is
  `(INSNS − BLK_HIT) + BLK_INSNS`, 37.25 % against a literal 59.28 % and a prediction of 31.80 %).
  `probe_cyc` 32 is **not** below a floor — 45.5 was v1's inlined whole-cost figure halved and 32
  is a marginal out-of-line one, so the calibration now times a third, call-free pass and reports
  marginal / scaffolding / in-bucket separately (32.8 / ~9.8 / ~42.6 on metal), with the reader's
  rule reduced to one line. The dispatch identity is restated as `DOPC_HIT + DOPC_MISS == INSNS +
  FAULTS` — twelve captures across three sessions and three builds put the sum above INSNS by
  exactly FAULTS, to the unit, and a faulting instruction is a dispatch that never retires. And
  three long-owed profiler notes land: a share swept at one band is not comparable with one swept
  at another; a bucket's share is an artefact in **both** directions (TAILADV reads 0.00 % at its
  own ceiling and 9.16 % at a cheaper price, and a rung was once funded off that); and the
  correction being linear means two price columns of one capture recover every bucket's spans/INSN
  and its zero-crossing price, for no counter at all. The footprint tables are marked as pricing
  **no second-order cost at all**, and F6 is one.

- **Plan WARP ENGINE — the plan, and M0 (`e473ce3`, `22dd4b3`, `cea8a46`, `e58b414`, `3f33a6f`,
  `d55db48`).** Row J is the interpreter ladder's last row and the only step change left: an
  upper bound of **2.11–2.35×** against a classified remainder of **+0.9–3.2 %**. The plan is a
  hybrid engine with the interpreter kept as fallback *and* as differential oracle, bailing to it
  **before** any fault rather than unwinding through generated frames; six milestones, each with
  a pre-registered gate and a kill criterion, and thirteen open questions carrying the design
  decisions.
  - **The Q2 probe (`22dd4b3`, `e58b414`).** A fourth image, `BOOT-q2probe.BIN`, emits a
    four-instruction ARM stub into the heap, calls it and provokes an unwind across it, printing
    a sentinel before each risky call so the terminated outcome reads as named silence; it is
    filtered out of the other three variants' source lists, whose CRCs are unchanged. One boot
    answered it: **TERMINATED** — the EHABI unwinder refuses a generated frame, so
    bail-before-fault is **measured, not derived**. The same boot showed generated code running
    with the shipped icache sequence from the code cache's own arena. The §1.5 base re-taken on
    one capture reads 48.68–49.91 % and the stitched ~50 % survives; lean `F294E75A` validated at
    median 8151, with the profiling build's +7.22 % owed a mechanism.
  - **The design freeze (`cea8a46`).** All 13 open questions resolved, each keeping its original
    recommendation byte-untouched and the kill line quoted verbatim. Zero-build censuses:
    **79.44 % of the 1 527 per-opcode compilers per table can reach guest memory**, so Route C;
    `instruction_pc` / `mmu_restart` / `mmufixup` appear nowhere in `jit/`, and `regs.pc` only at
    block terminators. Two claims in the plan's own Section 2 refuted and corrected in place
    (r10 is memstart, not r11; `cache_tags` keys on a host pointer).
  - **The M1 contract (`3f33a6f`, `d55db48`).** Scope enumerated, the kill line put in
    same-session operational form, M1's own band derived bottom-up at **1.04×–1.33×** from what
    generation one covers, the state-sync measurement given its Route-B trigger, and every
    counter direction checked against call topology — which corrected the INSNS identity and
    added ids 47–59, id 46 staying retired.

## 2026-08-22 — the RAM window knob, the overhead switches, the LEAN and PROFILING variants, and C2 rung 0 (8aba549..7e7e1c1)

- **`amix_ram <MiB>` — the AMIX memory contract's size becomes a config key (`8aba549`).** The
  window was one compile-time literal, `RANGE_MAP(0x0800, 0x0900, drct_bank)`, 16 MiB at guest
  `$08000000` with the next line dummying out everything above — our own choice, not a kernel
  limit, and the comment justifying it cited a `QUICK_KLUDGE` path that applies to a high-loaded
  kernel this one is not. Default 16, so an absent key reproduces the shipped map bit for bit on
  both the 030 and the 040 line; the knob is shared by both boot modes because the mechanism is
  the address map. Bounds 1..128 MiB, for two independent reasons that agree on the number:
  `reset_autoconfig()` hard-maps the RTG/ethernet combo board at guest `$10000000` and runs
  *after* the `RANGE_MAP`s (so an oversized window would not lose loudly — the board would punch
  a quiet hole through memory AMIX had been told was RAM, and that window is where
  `amix-z3660net` binds), and core1's ARM page table marks host `0x10000000`–`0x17FFFFFF`
  RESERVED. **Out-of-range values are REFUSED and fall back to the default, never truncated to
  the bound** — the failure mode at the top of this range is a guest emitting no console output
  at all, and a wrong-but-plausible size is the most expensive possible lie. Above 64 MiB the
  firmware warns and still obeys, because the knob exists so the ladder can find the real line.
  The `a3000mem` landmine is disarmed with it: past 16 MiB its bank, its startup memset and its
  `get_real_address()` alias must all go together, so the alias is re-gated on a new
  `a3000mem_alias` flag armed at exactly the program point that sets `a3000_amix_mode` — leaving
  the 16 MiB default byte-identical, and mapping guest `$07000000` to `dmmy_bank` rather than
  exposing this A4000's motherboard SIMMs as a second probeable region adjacent-below ours.
  Bounds and bank arithmetic live in one self-contained header that core0's parser, core1's map
  builder and the host harness all include. New host guard `amix_ram_test` proves the invariant
  by sweeping the input domain rather than sampling it; harness 256 + 26 + 34 + 81 = **397**.
  What only metal can answer: whether the guest's RAM sizer scales with the mapped extent at all.

- **The run-loop instrumentation becomes switchable, then compiled out (`c413780`, `dceab1b`,
  `c357414`).** The firmware measured and traced itself on every emulated instruction and every
  cooperative-loop iteration, and none of it could be turned off without a rebuild, so its cost
  had only ever been estimated. Three YES/NO keys with matching serial toggles — `perf_stats`,
  `emu_probes`, `core0_stats` — all defaulting ON, removing measurement and never emulation, with
  the costs read out of the generated ARM rather than estimated (the default path grows by 11 ARM
  instructions per emulated instruction). The ISSUE-52 probes are **gated, not deleted**.
  - **And making them switchable was not free.** The 2026-08-22 metal calibration priced every
    switch on a live 68040 AMIX at 128 MiB, all rungs inside one boot: `emu_probes` off **+7.9 %**
    (the dominant one — `[USTEP]`'s first term is `!regs.s`, true for every user-mode
    instruction), `perf_stats` off **+2.0 %**, `core0_stats` off **+0.1 %** (nil), all three off
    **+9.1 %** — but defaults-ON measured **−3.6 % against the old hard-wired build**,
    reproducibly, against a 0.7 % baseline spread. Switching an instrument off cannot recover
    that: what the hard-wired build never paid is the **predicate**. So `make lean` builds core1
    with the instruments *and their gates* compiled out — `m68k_run_mmu040` 376 → **101**
    instructions, and `nm` finds none of the perf symbols in the lean binary at all. The default
    build is unchanged, proven rather than argued: a clean rebuild is bit-reproducible and the
    post-change objdump differs by exactly **one instruction**, the `__LINE__` operand of UAE's
    own `abort()` macro. `service_cadence` is deliberately not compiled out in either variant —
    it is not an instrument.
  - Why `core0_stats` is nil despite reporting `ovh` = 57–59 %: that is a share of *core0* loop
    time, and core0 free-runs at ~900 k iterations/s with ITERATION 99.7 % of uptime. The guest is
    core1-bound; `ovh=` is an honest number about the wrong core.

- **`service_cadence` DOES cost disk throughput — the mechanism, and two wrong claims
  (`1cd42d4`).** `AMIX.md` said disk throughput was measured flat across the 1→64 range and
  offered that as confirmation that piscsi is not cadence-coupled;
  `piscsi-service-path.md` said the same thing citing `AMIX.md`, which cited it back. The claim
  was **circular**, and the calibration refuted it: raw-device reads of `/dev/rdsk` fell
  **16.2 %** from cadence 1 to cadence 8 (665.4 → 557.6 KB/s, reproducible, non-overlapping sample
  sets) while guest CPU rose 34 %. Two separate errors, kept separate. **(1) The benchmark was
  invalid** — the row believed to measure disk read a file under `/tmp`, and at the standing
  128 MiB that file is entirely buffer-cache resident; the firmware's own C0STAT counters prove
  it (`EMU>SCSIR 0c` across a window containing a full run — zero device reads). A file-backed
  benchmark at ≥ 128 MiB of guest RAM is not a disk benchmark. **(2) It was the wrong device** —
  "piscsi is not cadence-coupled" is still true (the mailbox has no interrupt), but the AMIX root
  here is controller 0 / target 6 of the **emulated A3000 SCSI**, served by AMIX's own sd/a3091
  driver, with piscsi only the backend the emulation reads through. The mechanism:
  `a3000_scsi_hsync()` is the only thing that advances the WD33C93 interrupt countdown and it is
  called from inside the function `service_cadence` gates, so one pump tick costs
  256 × `service_cadence` emulated instructions, and every SELECT / SRV_REQ / phase / completion
  interrupt of every block command waits on it. Guidance until it is decoupled: cadence 1 for
  I/O-bound work on an A3000-SCSI root; 4/8 remains worth it for CPU-bound work.

- **`make profile` — a third image, and where the interpreter's time actually goes (`0f54560`,
  `f47444d`).** The lean image reached dhry **5427** on 68040 metal; what is left in the
  per-guest-instruction budget is the interpreter itself, and nothing here had ever measured
  where inside it that time goes — so a ranking of candidate attacks would have been a guess with
  a table around it. `BOOT-prof.BIN` is built from the **lean** core1 plus `-DZ3660_EMU_PROF=1`
  (a profile of the diagnostic loop would be a map of a binary nobody runs). Two instruments,
  separate on purpose: a **sampler** answering "where in the guest" (wall-clock tick capturing pc,
  opcode and four mode bits into a 65536-entry ring in `.bss`) and **buckets** answering "where in
  the interpreter" (a phase machine charging every ARM cycle to exactly one exclusive stage, so
  `sum(buckets) == the measured span` by construction). Counters are a third thing and always run,
  with three tiers of translation separately visible. Probes sit at the ~10 out-of-line
  `uae_mmu040_*` copies rather than the ~3000 inlined call sites, which is what keeps the image
  16 KB over the default instead of unrecognisable — and, more importantly, keeps 3000 inlined
  probe sites from wrecking the icache behaviour of the loop being measured. Bias is **stated
  rather than discovered later**: the sampler polls in the run-loop tail so a deadline expiring
  during a long instruction is attributed to it, off-loop time accrues no samples and the next
  sample carries a WEIGHT field, the buckets charge their own probe cost and the firmware measures
  an empty enter/exit pair at arm time so it is subtractable, and PMCCNTR's 32 bits are
  cross-checked against an independent global-timer span.
  - **The bug the generated ARM caught.** `Z3660_PROF_RET` was first written the obvious way,
    sequencing the bucket close *before* the accessor's work — every read and fetch would have
    read near zero and HANDLER absorbed the lot, a map that is confidently, silently wrong.
    Counters right, stack balanced, emulation correct, 354 assertions green; nothing detected it.
    Found by reading the generated ARM, and now pinned by a test that fails against the old
    spelling. The same review caught an **unbounded spin** in the clock calibration, which on a
    board whose global timer was not running would have been a card booting to nothing — in the
    one image a person flashes precisely because they are already chasing something.
  - `system_cli_prof.bif` was left untracked at first (`f47444d`), so `make profile` worked here
    and would have failed on any fresh clone; the ignore rule now names all three descriptors
    explicitly.

- **Profiler wire format v2 — three numbers that did not mean what they said (`977c5bd`,
  `2c4e16c`, `b43c71c`, `a62e2c5`, `7bd6db1`, `9e3a71a`).** The first metal session with the
  instrument produced four captures, and three of the values they printed were **mislabelled** —
  not wrong measurements, which is the worse failure, because a mislabelled number still looks
  like a number and arithmetic built on it still balances. **(1)** `probe_cyc` is per
  *transition*, not per pair: TRANSITIONS increments in both `enter()` and `exit()`, so the
  reported probe overhead was exactly 2× too big, demanding 43.8 G cycles from a bucket holding
  28.0 G while the host tool clamped rather than complained; the honest figure is 41.33 %, and
  correcting it moves LOOP from first place in the ranking to fifth. **(2)** `cpu_hz` / `wall_hz`
  are the runtime clock: v1 scaled by a compile-time BSP constant frozen at 666.67 MHz while the
  rig runs at 1100 MHz, so every Hz, duration and actual sample rate was low by exactly **1.65** —
  and nothing caught it because **a wrap check is not a clock check** (CPU : global-timer is a
  fixed 2 : 1 in silicon, so the erroneous constant cancels out of the ratio and the check passes
  to three decimals while the clock is 65 % wrong). **(3)** Counter 12 was never an ATC hit —
  `mmu_translate()` reaches its success return both by tag match and via `goto atc_retry`, so it
  counted successful translates and made `ATC_HIT + ATC_MISS` a sum of a set with one of its own
  subsets; renamed `XLATE_OK`, with a genuine `ATC_HIT` appended at id 19. **(4)** TAIL is split
  into TAILADV / TAILSAMP / TAILPOLL / TAILSPEC, the map's highest-value instrument ask, with the
  profiler's own hook bucketed separately so it can be subtracted rather than ranked. Ids were
  never renumbered — two renamed to say what they always counted, four appended — and the sample
  record is unchanged, so a v1 decoder for S lines is still correct. The same defect had been
  living in `[PERF]` (`a62e2c5`): the kIPS line spent the BSP constant twice, as both the length
  of its "~1 Hz" window and the numerator of the rate it printed, and its `(uncalibrated, use as
  relative)` caveat was true and useless; it now walks the same road as the profiler and reports
  `clk=cfg` with the rate it used.
  - **`PROF OFF` and `PROF <hz>` (`2c4e16c`)** — a way to stop that does not destroy the capture.
    Every rung of the rate ladder arms, and arming zeroes the ring, the buckets and the counters;
    the session armed at `prof_hz 200` from the config, and the first `PROF` press mapped 200 into
    the `< 1000` arm and wiped a multi-minute boot capture that could only be retaken by
    cold-booting the board. `PROF <hz>` also prints the wrap-free window in seconds, because that
    is the number that decides whether a capture will contain its workload — two of three
    workload captures had wrapped and profiled the idle machine *after* the benchmark.
  - **`IFETCH_CALLS` (id 20) (`7bd6db1`)** — appended to settle a landing-model hypothesis, and
    the source settles it in the same commit: there are exactly five `ENTER_IFETCH` sites and
    every one is followed immediately by a single `C_FETCH`, so `IFETCH_CALLS == FETCH`
    identically. It ships anyway, because it makes that an identity a capture *asserts* and it
    fails on the day a sixth fetch site is added to one path and not the other.
  - **`probe_cyc` is the MARGINAL cost of a transition (`9e3a71a`)** — the v2 calibration's 59
    cyc/transition was arithmetically impossible (LOOP exhausts at 50.59, TAILADV at 54.11),
    because it timed the armed loop alone and charged the `bl`, the callee prologue, the
    predicate, the epilogue and the loop's own compare-and-branch to the probe — all of which
    survive in the machine the subtraction leaves behind. Now it times the same loop twice
    differing in one bit, and both spans are printed on the boot line. The correction was
    **pre-registered so it could be wrong**.
  - **What the first profiling session taught, including about the profiler (`b43c71c`)** — a v1
    → v2 correction table so four existing captures stay usable rather than being thrown away,
    plus three operating laws all learned the hard way: **a ring must contain its workload**;
    **one UART, two cores — PSTAT off before any long dump** (a full ring dump is ~65 500
    semaphore acquisitions over two minutes and core0 spins while it waits, starving its piscsi
    service loop); and **a console echo is not evidence that an instrument is armed**, since core0
    cannot tell which core1 image is running and on lean and profiling images the instruments are
    compiled out. The four-point overhead A/B is measured rather than owed: lean 4500 dhry/s,
    profiling idle 3457 (−23.2 %), +sampler 3409 (a further −1.4 %), +buckets 2106 (a further
    −38.2 %) — and the middle gap being 1.4 % is what says the guest-PC map can be read as wall
    time. The legacy **"keep `BOOT.BIN` under ~12 MB"** rule is formally **RETIRED** here; `-Os`
    and `--gc-sections` stay for link footprint and build time, but size is not a deploy gate —
    CRC-gate every hop instead, and create `FAILSAFE.bin` first.

- **`make bootbin` had stopped rebuilding core1, and said nothing (`0a9012a`).** Found by
  disassembling the lean image after adding a mailbox word and noticing the shipped binary did
  not read it — three successive full builds, all green, all packing a core1 ELF that was hours
  old. GNU make takes `.DEFAULT_GOAL` from the first target of the first rule it *reads*, and an
  include counts: `Z3660_emu/Makefile` does `-include $(OBJS:.o=.d)` four lines above `all:`, so
  as soon as the generated dependency files existed the default goal became whichever object file
  sorts first. The parent invokes it with no goal, so from the second build onward core1 built
  exactly one object and exited 0, and bootgen packed the stale ELF into a fresh `BOOT.BIN`. The
  failure is silent by construction: green log, new timestamps, a CRC that changes whenever core0
  changes, and nothing anywhere mentioning core1. **Every incremental image built since the `.d`
  files first appeared is suspect.** `.DEFAULT_GOAL := all` pinned, with the reasoning beside it,
  and the host harness's identical shape pinned too. `lean` and `profile` are also forwarded from
  the repository-root Makefile, where both had been documented all along and both failed.

- **C2 rung 0 — the block-idiom fast path (`7e7e1c1`).** A family of tight kernel block loops is
  **33.6 % of an AMIX boot**, 25.1 % of a raw disk read and 10.6 % of dhrystone; `bzero` alone is
  22.1 % of boot and it is two instructions at one PC. Costed from the map's per-event table those
  two pay **850.5 ARM cycles per four bytes, of which the guest store is 162.7** — kernel
  zero-fill runs at 5.17 MB/s and 80.9 % of the price is interpreter machinery around a four-byte
  write. A recognizer in `m68k_run_mmu040`, after the opcode fetch and before the dispatch, keyed
  on the **opcode pair** and never on a PC or a symbol, services many iterations per pass. Three
  stages: `bzero`, `mcpy`, and the CPUSHL cache-flush loop — stage C collapses to almost nothing,
  and correctly so: everything the handler does is a function of the opcode alone, so doing it
  once for the whole run is the same operation and not an approximation.
  - **It does not replace the guest store.** No memset, no host block move, no direct pointer into
    guest memory: every iteration calls `put_long_mmu040()`, the exact function the generated
    handler calls, so TTR matching, the page caches, the M and U bits, protection, bank dispatch
    and device side effects are preserved by construction. The arithmetic does not need more —
    162.7 of 850.5 cycles is 19.1 %, so the pre-registered 80 % reclaim **is** the reclaim of the
    safe design. The device gate is strictly non-faulting and declines on a cold page, costing one
    interpreted instruction per page (0.1 % of a 1024-longword fill) and making the page bound
    fall out of the same mechanism rather than being a second rule. Yield granularity is one
    chunk, default 256 iterations ≈ 42 µs at 1.1 GHz — inside one 15.6 kHz hsync period, the
    tightest cadence anything on this poll depends on. Switch `SHARED.blk_chunk` at `0xFFFF00E4`,
    console `BLK` cycling OFF → 64 → 256 → 1024, compiled into **every** variant because this is
    an optimisation and the lean production image is the one that most wants it. Counters
    `BLK_HIT` / `BLK_INSNS` / `BLK_BYTES`, ids 21–23.
  - **Evidence.** Every case runs twice from an identical fixture, once with the recognizer OFF as
    a pure passthrough, and the outcomes are compared as whole objects — all sixteen registers,
    pc, `instruction_pc`, SR, USP, ISP, and a **one-megabyte memcmp** of guest physical memory
    covering the page tables, the code, the vectors, the supervisor stack and all four data pages
    — at chunk caps 1, 256 and 1024, because a fast path whose answer depends on its chunk size
    has a boundary bug. Two fixture properties were found by mutation testing rather than by
    design: the register file and CCR are **poisoned** before every run (without it, a fast path
    that forgot to set the flags agreed perfectly with the interpreter that set them), and key
    cases run pre-warmed as well as cold. Seventeen deliberate defects injected, sixteen caught;
    the two that are not are **dead because of today's gate**, and they are written correctly
    anyway with the code saying why — widen the gate or hoist the store and they become the
    difference between a page fault and a corrupted file system.
  - **Metal 2026-08-22:** boot **−43.6 %**, dhrystone **−1 %** — the recognizer's three masked
    compares are paid on every dispatch and repaid only inside the idiom stream, which is the tax
    rung 1's cached idiom verdict was then built to remove.

---

### Cross-cutting, across the whole span

- **Correctness never regressed.** Five metal sessions in the WARP arc with zero correctness
  failures; on the C2 side every rung shipped behind a state-equivalence corpus compared as whole
  objects against its own disarmed reference, and every revert rebuilt its predecessor's image
  byte for byte (`DEC21748`, twice).
- **The WARP kill line was never entitled to fire.** Its precondition is coverage ≥ 60 %;
  coverage measured 8.43 %, 19.10 % and 20.26 %. "Not a verdict in either direction" is the
  recorded outcome, three waves running.
- **Every build now archives its image CRC-stamped, with the core1 ELF beside it**
  (`sd_card/archive/`), after a rebuild destroyed a live reference arm and a flown image turned
  out not to be what its source builds.
- **`mkbootimage` falls back to the docker image's copy**, so a host with neither a PATH nor a
  `$HOME/git` copy can still assemble a boot image.
- **Both Makefiles read their `.d` files**, and `.DEFAULT_GOAL` is pinned — the two halves of a
  build hazard that silently shipped stale objects in both directions.
- **The profiler's counter block grew 46 → 69 ids over the span, every one an append (id 46
  retired), and the dump wire version stayed at 2 throughout** — so every capture taken since
  2026-08-22 still reads correctly in every later symbolizer.

## 2026-08-21 — 040: ISSUE-52 round-8 RESOLVED — the cold `mmufixup[1]` slot zeroed copyout's source register (7429a58)

- Metal proved round 7's fix was not the corruptor (symptom byte-identical), and the
  `[IDCMP]` probe exonerated fetch/translation: I and D agree on the same physical long
  and both read the poison — the page CONTENT was wrong.
- Root cause: `m68k_reset_newcpu()` cleared only `mmufixup[0].reg`; upstream (amiberry
  `src/newcpu.cpp:3703-3704`) clears both. `cpu_restore_fixup()` reads `reg >= 0` as ARMED,
  so the BSS-zero slot 1 means "A0 armed, restore 0". `cpuemu_31` never arms slot 1, so on
  the 040 path nothing cleared it and the FIRST bus fault of the session set A0 = 0. On the
  030 path `m68k_run_mmu030`'s CATCH clears both slots on its first fault — invisible there.
- That first fault is the icode copyout's first-touch write: `lcolloop`'s
  `moves.l %d1,(%a1)+` (DFC=1) at kernel `0x080005C2`, frame PC `0x080005C6`, `ssw=0401`.
  A0 is the copy's SOURCE, so the loop resumed reading from guest address 0 — the ISSUE-10
  signature and ISSUE-52's garbage user PC, one bug.
- Fix: restore the lost reset line + static `{ -1, 0 }` initialiser (cold state safe by
  construction). `m68k_run_mmu040` de-`static`'d so the harness drives the real loop.
- Harness 244 → 256: `test_mmu040_copyout_fault_keeps_source_reg` reproduces metal byte
  for byte pre-fix (A0 = 0x34, dest long 1 = 0x4AFC0000, 13/14 longs wrong, long 0 right).
- `BOOT.BIN` 12,762,380 B, zlib crc32 `D5CBFAF4` — flashed and metal-validated same day:
  3/3 boots to the interactive installer prompt; the first 68040 AMIX install ran on it.
  Detail in `UAE_040_MMU_gate.md` §10.14.

## 2026-08-21 — 040: ISSUE-52 round-7, the format-$7 write-back slot was never consumed (60ce49f)

- The [USTEP] "translated user-page instruction fetch returns garbage" reading is
  REFUTED, arithmetically: 68040 translation composes by OR (`l->phys | (addr &
  mmu_pagemask)`, and the MMU_IPAGECACHE shortcut likewise), so the page offset is
  invariant — a fetch at page offset 4 can never return the content of physical 0,
  whatever frame it resolves to. No pc_p-relative fetch is reachable from the 040
  table either (cpuemu_31: 1182 m68k_getpci, zero m68k_getpc/m68k_incpc/next_d*).
  set_x_funcs' 040 arm, the *_mmu040 fetch inlines (byte-identical to upstream),
  x_get_disp_ea_020 and m68k_run_mmu040 all check out. The address was right; the
  bytes on the page were wrong.
- FIXED (`newcpu_common.cpp`, Exception_build_stack_frame case 0x7): the frame
  stacked regs.wb2_status/regs.wb3_status without clearing them — two upstream
  lines lost in the port. Handing the slot to the guest consumes it: AMIX's
  wb040.s replays on WB3S bit 7 alone. mmu_bus_error() recomputes both words on
  every fault it handles, hiding this for six rounds; the faults that reach
  Exception(2) WITHOUT it (hardware_exception2 / exception2_read /
  exception2_fetch*, which THROW straight to the run loop's CATCH under this
  non-cpu_compatible config) then stacked the PREVIOUS write fault's valid WB3S
  and stale WB3D against the CURRENT fault's address — the guest performs that
  store. A stray write, of a stale datum, into the page that just faulted: the
  ISSUE-10 "write-back-fabricated value" signature, landing in pages being
  demand-paged in (proc 1's text among them). 68030 path untouched.
- Harness 221 → 244. test_mmu040_user_icode_stream drives the metal case end to
  end through the real run loop and passes before and after — the proof the fetch
  is faithful. test_mmu040_writeback_slot_consumed fails 3 assertions before the
  fix and passes after.
- Build: BOOT.BIN 12,762,380 B, zlib crc32 `B29BF9EB` (superseded same day by
  round 8's `D5CBFAF4`). Detail incl. two latent divergences found and deliberately
  NOT changed: `UAE_040_MMU_gate.md` §10.13.

## 2026-08-21 — 040: [USTEP] probe; LEA path proven correct (ISSUE-52 round-5) (a3c6a0b)

Hypothesis was family #7 — the 040 core mis-executing the icode's full-format PC-relative
`lea` (`4FFB 0170 0000 0028`). **The emulator's lea path is correct** by every check:
handler anchors `base = pc+2`, `x_get_disp_ea_020` full-format branch reads the 32-bit bd
via the translating `x_next_ilong`, length is 8; all reads aligned; `MMU_ICACHE == 0`.
New `test_mmu040_lea_pcrel_fullformat` runs the exact instruction on a table-walked mmu040
page and asserts `A7=(pc+2)+0x28`, 8-byte length, clean next-fetch — **passes**. (A first
draft failed on stale `x_next_*` from a prior `cpu_bringup(68030)`; correctly wired, it
passes — recorded as the trap to avoid.) Harness 217 → **221**.

- No named emulator defect ⇒ **no speculative fix**; ships the `[USTEP]` probe: pre-state
  of the first ≤8 user-mode instructions (`pc/op/a7/d0`). Step-1 `a7` must be `0x8080002A`,
  step-1 `pc` `0x80800008`.
- Prediction: `[USTEP1]` shows the lea faithful, so `0xCB7C0002` was a timing-fragile
  kernel-ring mis-read or later corruption. A wrong value = a new metal-only class.
- `BOOT.BIN` 12,762,388 B, zlib crc32 `368993CA`. Warning-free.

## 2026-08-21 — 040: [URTE] syscall-return probe (ISSUE-52 round-4) (b89af61)

The bus-misalign fix moved the fault past the launch RTE — PID 1 launches correctly
(fault PC 0x80000012 → 0x80800012) and runs. The remaining fault is at the syscall
RETURN from icode's `trap #0`, with a corrupt A7 (metal `fa=0x40001FC0` = kernel SSP;
kernel ring saw `usp=0xCB7C0002`).

- **Statically exonerated:** 040 `move An,USP`/`move USP,An` handlers are byte-identical
  to the proven 030 ones, and `MakeFromSR` installs `regs.usp` into A7 correctly on the
  S 1→0 transition. New harness `test_mmu040_rte_syscall_return_usp` proves the install
  lands A7 on USP, not the leftover SSP, given a valid `regs.usp`.
- **Ships the `[URTE]` probe:** fires on any user-transition RTE (frame SR S-bit clear),
  printing `pc/sr/ssp/usp/fmt`; `usp` (read pre-`MakeFromSR`) is the value RTE installs.
  Firing #1 = launch (clean), #2 = syscall return (decisive). A corrupt #2 `usp` with a
  well-formed frame ⇒ `regs.usp` was corrupt before the RTE ⇒ kernel-side; a wrong frame
  ⇒ kernel-side.
- Diagnostic; becomes a fix only if a read implicates the emulator's USP handling (harness
  currently says it's faithful). Harness 212 → **217**.
- `BOOT.BIN` 12,762,388 B, zlib crc32 `309F04F1`. Warning-free.

## 2026-08-21 — 040: split bus-misaligned MMU accesses (ISSUE-52, family #6) (4bf1f00)

Round-3 metal overturned the prior "kernel-side" call: the frame holds `f_pc=0x80800000`
(read back correct) yet PID 1 surfaces at `0x80000012` — bit 23 dropped. RTE reads right,
delivers wrong.

- `_start`'s format-0 frame puts the PC longword at `a7+2 = 0x40001FBA` — bus-misaligned,
  same page. The 040 accessors split only on **page-crossing** (`is_unaligned_page`), so the
  read handed a misaligned physical address to `memory_get_long`'s `swap32(*(uae_u32*)addr)`,
  which the board's memory layer can't do — dropping bit 23 (`0x80800000 → 0x80000000`).
- **Fix:** 040 data/instruction accessors split on **bus misalignment** (`is_unaligned_bus`),
  so the physical layer only ever sees aligned pieces. `is_unaligned_page ⊂ is_unaligned_bus`
  (nothing that split before stops); 68060 and 68030 untouched.
- Host can't value-reproduce (flat buffer handles any alignment), so the guard pins the
  routing invariant via a harness-only split counter; firmware binary unchanged. Non-vacuous
  (reverting to the page gate fails all three routing checks). Harness 201 → **212**.
- Corrects this file's earlier §10.8 "exception-return path clean" — that call rested on a
  host value-assertion that structurally cannot see this bug; the metal round-3 capture broke
  the tie.
- **Predicts PID 1 executing** at `0x80800000` → init/getty/login. Last emulator gate; ninth
  emulator fix, sixth in the never-exercised-040-path family.
- `BOOT.BIN` 12,762,388 B, zlib crc32 `BDBA0BDE`. Warning-free.

## 2026-08-21 — 040: exception-return path clean; ISSUE-52 frame is pre-corrupted (f4d787b)

Hypothesis was family member #6 — RTE reading the frame through the wrong accessor
family. **Refuted by inspection and by test.**

- `op_4e73_31_ff` (RTE) and `op_4e77_31_ff` (RTR) both use `get_word_mmu040`/
  `get_long_mmu040`; format `$7` goes to `m68k_do_rte_mmu040()` (`cpummu.cpp:1595`). No
  `mmu030_state` residue in the 040 table's code.
- New `test_mmu040_rte_delivers_pc`: a real format-0 frame on a **table-walked** stack
  delivers its exact PC (`0x80800000`), drops to user mode, and switches `a7` to the USP —
  table-walked and TTR-covered. Harness 193 → **201**.
- Therefore "fed the right PC, delivered the wrong one" is impossible in-core: the frame
  already holds `0x80000012` when RTE reads it. *Main's computed entry* being correct is a
  different moment from *what's on the stack at RTE*.
- Ships a bounded `[RTE]` probe (u-block-window `a7` only, 8 lines) printing the frame RTE
  is about to consume — two-way discriminator between "built wrong" and "corrupted after".
- **No emulator defect found and none invented.** Second consecutive "not us" round; the
  sweep has now covered supervisor access, MMU control, exception entry, user mode, and
  exception return, with permanent guards at each — the 040 core looks converged.
- `BOOT.BIN` 12,762,404 B, crc32 `AA1E8B4F`. Warning-free.

## 2026-08-21 — ISSUE-49 fix FLASHED + METAL-VALIDATED: THE FIRST 68040 AMIX BANNER (034901C0 live)

`034901C0` (the bitfield-accessor fix) flashed through both deploy gates, 030 canary
green, and the verdict boot delivered the campaign's goal: **the 68040 AMIX kernel
runs on the metal card** — no panic, no reset, six minutes observed, 65,536
interrupts serviced at steady rates — and the screen shows the full banner:
`UNIX(R) System V Release 4.0 AT&T Amiga (Unlimited) 68040-260821-10 Version 2.1c`,
memory sized correctly under the contract (16,773,120 total / 14,487,552 available).

The kernel exec'd **PID 1**; init took a user-space bus error (`User BUS ERROR at
40001FC0, PC:80000012 FAULT:6 PID:1`) which the kernel handled gracefully as a
NOTICE and survived. The frontier is now USERLAND bring-up (init's exec at a
near-null user PC) — a new case, not a kernel or MMU one.

Night's tally on this line: SEVEN emulator defects fixed and harness-guarded (mode
predicate, pre-init printf, STOP pc, MOVEC→TC, PFLUSH/PTEST accessors, reset MMU
state, bitfield 030-accessors) + one latent stock kernel bug (unzeroed page DB,
port ISSUE-48, confirmed ×4) + the panic-path guard (ISSUE-46) metal-proven.
Harness 90 → **182**. Box parked OFF, selector restored to golden.
## 2026-08-21 — 040: bitfield ops used the 68030 accessors (ISSUE-49) (6e8e33a)

The port lane isolated `bfins %d0,%a2@{2:1}` (d0=0) failing to write against table-walked
kernel VAs while neighbouring byte ops landed — 149/149 page structs reading `0x22`.

- **Cause:** `cpuemu_32`'s bitfield fix (call `x_get_bitfield`/`x_put_bitfield` instead of
  the physical-accessor bare versions) was copied to `cpuemu_31` **without swapping the
  accessor family** — those helpers use `get_byte_mmu030_state`/`put_byte_mmu030_state`, so
  every 040 bitfield op translated through 68030 PMMU state this core never sets up.
  Invisible under ITT0/DTT0 identity mapping, fatal in the table-walked window.
- **Fix:** `x_get_bitfield_040`/`x_put_bitfield_040` (`*_mmu040` accessors) + a two-line
  bridge in `cpuemu_31.cpp`, keeping the generated body byte-identical across all 68 sites.
- **Guard = the reproduction.** Pre-fix: six table-mapped arms fail (including the
  byte-exact `0x20`→unchanged and `0x22`), TTR-covered / MMU-off / register-destination all
  pass. Localises to translation, not decode. **BFEXTU failed too** — the read half was
  broken as well. Harness 167 → **182**.
- **Closes the metal signature:** `0x20 → 0x00`, then `ori.b #2` → `0x02` not `0x22`; no
  stale `p_free`, so `segmap_unlock` should not fire. Kernel needs no change — port
  ISSUE-49 becomes a symptom record.
- **Fifth instance of one family** — STOP, MOVEC→TC, mmu_op, reset state, bitfield
  accessors: every one a 68040 path present in the tree but never exercised until this
  kernel ran. The standing sweep: find the 040 equivalent of anything the 030 path was
  hand-patched for.
- `BOOT.BIN` 12,762,388 B, zlib crc32 `034901C0`. Warning-free.

## 2026-08-21 — bus: recover the stuck pipe (678b5ce)

The watchdog fired on `FF261896` (030 canary green) and the decode named the wedge:
`REG4=0x15` is a **pending byte WRITE** (bit1=0 write, bits2-3=01 byte, bit4 pending),
not the `R32` in the line — and `REG5=0x80000000` is exactly what `wait_write_ack()`
spins on, with `nbg=0` (bus granted). A byte write entered the pipe and never acked.

- **Ordering deadlock:** `arm_write_amiga_byte()` writes fire-and-forget (never calls
  `wait_write_ack()`); the *next* access absorbs the latency in its pipe-empty wait. So
  the stuck-write recovery poke lives in the write path and is unreachable from the read
  that's stuck. Fixed by applying the same poke from the pipe-empty wait, retried every
  `BUS_WD_N`, report still one-shot. Cannot fire on a healthy boot (seconds-scale
  threshold, 100–200× the worst legitimate lockout).
- **Cache modes eliminated:** `DTT0`'s CM=3 (cache-inhibited/nonserialized) only sets
  `mmu_cache_state`; this core models no cache, so no access-pattern change.
- **MOVE16 named as the 040-only rhythm delta:** four `move16` in the kernel, issued as
  four back-to-back long bus cycles; the 030 cannot produce that burst.
- **Workaround, not a cure** — the un-acked write is below the firmware. Recurring
  `[BUSWD]` with the boot continuing = a real FPGA/CPLD ack fault for that lane.
- `BOOT.BIN` 12,762,356 B, zlib crc32 `0BF7BAAE`. Harness 167/167. Warning-free.

## 2026-08-21 — bus watchdog on the pipe-empty wait (1e6178f)

Front 1's wedge (core1 executing no guest instructions, `ps_count` frozen, last access
chip RAM `0x001D6BC0`) needed naming without touching timing. Reading the three bus
waits narrowed the target to one: `wait_read_ack()` and `wait_write_ack()` are both
already bounded with recovery paths, so the bare `while(read_reg(REG4)&(1<<4));`
pipe-empty wait — six sites, no bound at all — is the only place a permanent stall can
park core1.

- All six sites now use `WAIT_PIPE_EMPTY(tag, address)`: spins as before, counts, and on
  crossing N prints **one** line (`[BUSWD] STALL <tag> addr= REG4= REG5= nbg= ps_count=
  ps_last=`) then keeps spinning. `REG4`/`REG5`/`nbg` discriminate bus-never-granted vs
  pipe-never-drained vs cycle-never-acked.
- **N = 20,000,000** ≈ 2–6 s (the loop body is one ~100–300 ns GP-AXI register read).
  The worst *legitimate* lockout — a blitter-nasty blit, 10–20 ms — is ~100k–200k
  iterations, so N is 100–200× above it. A healthy boot cannot reach it.
- Zero behaviour change in practice: the compiler hoists the counting path out of line,
  leaving the fast path `ldr`/`ands`/`bne` as before plus one `mov`. Global one-shot,
  not `mmu_model`-gated.
- `BOOT.BIN` 12,762,372 B, zlib crc32 `FF261896` — carries all six emulator fixes, the
  reset-state fix and the watchdog. Harness 167/167. Warning-free.

## 2026-08-21 — 040: guest reset now clears the MMU state (27664e2, 87310e9)

**ISSUE-48 resolution recorded:** the port lane confirmed the page database was
unzeroed DRAM (`calls=1 npages=3688 dirty=3620 held=150`); `page_free` is gone
(`syncg calls=0`) and the kernel runs for minutes. That vindicates §9.6/§9.7 — the
geometry was correctly derived and never the problem — and explains the byte-for-byte
determinism (unzeroed DRAM after a cold boot is deterministic).

- **Fronts 2 and 3 share one firmware root cause**, the third instance of
  "register stored, side-effect function never called": `m68k_reset_newcpu()` zeroed
  the MMU registers but never called `mmu_set_tc()`/`mmu_tt_modified()`, so a warm
  reset carried stale `mmu_enabled` + `mmu_ttr_enabled*` + ATC into the next ROM run.
  Explains the ATC ifetch fault on ITT0-covered kernel text (`vec=2 fa=0800ABEA
  ssw=0546`, address-specific via stale ATC) and the cycle-2/3 early-KS wedges.
  Fixed, `mmu_model`-gated (030 untouched). Also clears `amix_mmu_on` so each cycle
  starts in cycle 1's proven state — **wants the 030 regression before trusting**.
- Confirmed the mmu040 ifetch path consults ITT0 correctly, and that the kernel's
  only TTR-clear is the **haltsys** path (`.text+0xda012`), not post-`sysseginit`.
- **Front 1 measured:** mailbox class eliminated (all nine SHARED flags zero),
  reset-line spin eliminated (`read_reset == 1`), and `ps_count` frozen across 9 s
  with `ps_lastaddr = 0x001D6BC0` — core1 wedged in a chip-RAM bus cycle. Not fixed;
  behaviour-preserving watchdog proposed.
- Harness 160 → **167**, new guard non-vacuous. `BOOT.BIN` 12,762,372 B, crc32
  `C3191831`. Warning-free.

## 2026-08-20 — 040: candidate C exonerated; QUICK_KLUDGE landmine found (d6d0df8)

Static three-way diff of kernel expectation vs card vs bench.

- **Maps differ as suspected:** bench main RAM at `0x07000000` (Amiberry
  `a3000lmem_bank.start = 0x08000000 - size`), card at `0x08000000`. But `config()`
  (`.text+0x18f5c`) derives `MAINSTORE`/`VSIZOFMEM` by a fixpoint over
  `bootinfo.memory[]` and gets **both right** (16 MB at the correct base each side);
  `maxclick` covers all real RAM in both. **C is not the cause.**
- **Landmine:** `config()`'s fallback (runs only if `VSIZOFMEM == 0`) hardcodes
  `0x07000000` and masks `end` with `0xF7C00000`, which **clears bit 27**. At the card's
  load base that yields `MAINSTORE = 0` and `VSIZOFMEM = 128 MB` — excluding all real
  RAM and failing the `pages`/`epages` assert. Skipped today; fires on any boot where
  `bootinfo.memory[]` is empty/unusable. Port-lane fix, independent of this bug.
- **`PANIC: page_free` is an `assfail`**, one of three asserts: `pp ∈ [pages, epages)`
  (line 622), a `p_flags` bit (623), `pp@(56) == 0` (624). Which one is not in the text.
- **B needs**: the line, the `pp`, the caller — all delivered by one boot once candidate
  A (the `sync()` NULL guard) stops the double fault.
- Firmware contract variant **withdrawn**; no firmware change proposed.

## 2026-08-20 — 040: page_free is the bug; the ROM chain is the double panic (078f256)

`8C51370A` captured the transfer in one line and closed the case, with the framing
inverted.

- `from=0x0805D24E` = **`sync+0x34`**; `xpanic` (`.text+0x3e668`) relocates to `sync`
  at `0x3e6a2` → the path is **`panic()` → `xpanic()` → `sync()`**.
- `sync()` loops `i = 1..nfstype-1` over `vfssw` (`.data+0x913c`) calling
  `vfssw[i].vsw_vfsops->vfs_sync`. Relocations prove **only entry 0 has a static
  `vsw_vfsops`** (`vfs_strayops`); entries 1+ are filled by `vfsinit()`. The loop
  **starts at 1**, so panicking before `vfsinit()` dereferences NULL.
- `NULL+16` = absolute `0x10` = **CPU vector 4**, holding exec's stub `0x00F80A62` —
  which **self-identifies as vector 4** (`(0xF80A64−0xF80A5C)>>1 = 4`). Two independent
  derivations agree.
- exec's tail walks AMIX's pseudo-ExecBase (`@4 = 0x080007F0`) → `tc_TrapCode` =
  `0xFFFFFFFE` → `rts` to −2 → Address Error → **DOUBLE PANIC**.
- **The cascade is environment-independent; the bench boots green because `page_free`
  never panics there.** The ROM/ExecBase chain is a dead end for root-causing.
- A software panic is not a CPU exception — which is why no exception trace could ever
  see the primary event.
- `UAE_040_MMU_gate.md` §9 is the self-contained port-lane handover (candidates:
  A sync() NULL guard, B the real page_free condition, C memory geometry). Firmware
  cleared; memory-contract change proposed, not implemented.

## 2026-08-20 — 040: fault named, case handed to the port lane (37cb2aa)

`F87F6FA7` returned outcome 2: **zero interrupts** (storm dead, IPL7 edge question moot)
and `a7=080f49e8` **at MMU-on** — `pstack` already 6936/8000 deep before the MMU was
enabled, only 296 bytes more to the fault. The deep stack is ambient; the exhaustion
panic was the cascade.

- `0x00F80B4C` disassembled from the card's own KS 2.04 r37.175: the **`rts` at the tail
  of AmigaOS exec's CPU-exception dispatcher**, which jumps to
  `ExecBase->ThisTask->tc_TrapCode` read from **absolute address 4**. AMIX owns low
  memory by then, so the walk returns garbage — `0xFFFFFFFE`.
- **Not reached by an exception:** the kernel's VBR is `M68Kvec` (`.text+0x1378` == the
  observed `0x08001378`) and every entry relocates to a kernel symbol — no ROM address.
  Control was transferred through a pointer.
- Emulator exonerated: one exception, correct frame, kernel's own handler, no storm, no
  stale ATC. **Kernel/handover question → port lane.**
- Corrections: the TC write sits after `pstart` (nearest label `Lkroot`), and the
  missing `[SERMIRROR]` is expected — `btrace_mark` gates on `btrace_on` (DBG-only).
- Ships `[ROMENT]` (one compare/instruction, AMIX-040, 4 hits max) naming the source pc,
  the pushed return address and `execbase@4` — the one fact the binary can't supply.
- Memory-contract change proposed, **not** implemented (needs a decision + 030 regression).
- `BOOT.BIN` 12,762,372 B, zlib crc32 `8C51370A`. Warning-free.

## 2026-08-20 — 040: extended exception trace (c086aa7)

`F4782D71` returned a one-line trace and falsified the prediction:
`[XT01] vec=3 ipc=00F80B4C pc=00F80B4C fa=00000000 ssw=0000 sr=2700 a7=080F48C0`.
No `vec=2` chain. The first non-interrupt exception after MMU-on *is* the Address
Error, and `pstack` was already ~92% consumed (7232/8000) before it — so the consumer
is ordinary execution or the interrupts the tracer skipped. That filter was the mistake.

- **`vec=3` at an even pc explained from the code:** `Exception_normal()` raises
  exception 3 via `exception3_notinstruction()` when the longword at `vbr + 4*nr` is
  **odd**. An odd vector-table entry for an earlier (possibly interrupt) exception
  presents exactly this way.
- **Noted, not changed:** `check_uae_int_request()` treats IPL7 as level-triggered
  (`|| eff_irq == 7`) where silicon is edge-triggered — a stuck IPL7 would be taken
  repeatedly regardless of `intmask`, stacking a frame each time. Shared with the proven
  030 path; the trace decides before anything is touched.
- Ships: per-level interrupt counts + stack high-water, first 12 logged in full with
  `vbr` and the fetched vector, periodic capped summary; `fault=`/`vec3@` on vec 2/3;
  `a7`+`vbr` on the MMU-enable line as the "before" stack sample.
- Instrumentation only. `BOOT.BIN` 12,762,372 B, zlib crc32 `F87F6FA7`. Warning-free.

## 2026-08-20 — 040: page_free is boot-stack exhaustion (9143d5a)

`095356E9` panicked byte-identically to `D910C41A`, so PFLUSH/PTEST were real bugs but
not this panic's cause. Static read-out of the kernel binary, no boot needed:

- Load base proven at `0x08000000` (`movec %d0,%tc` at `.text+0xd7550` == serial's
  `PC=080d7550`); enclosing symbol `pstart`.
- **The panic is `pstack` exhaustion**: an 8000-byte boot stack (`pstackt` `.data+0x4`
  → `pstack` `.data+0x1f44`, VA `0x080F45C0`..`0x080F6500`), ~92% consumed at both
  panics. `page_sub`/`page_free` are page-allocator neighbours — where the runaway was
  standing, not the bug. The first panic's odd `Backtrace` (`0x080F485B`) explains the
  Address-Error double fault.
- `pstart`'s TTRs (`itt0=0x003FC000`, `dtt0=0x003FC060`, `dtt1=0x807FA060`) make the
  kernel image **and Kickstart ROM** transparent, so `pc=0xF80B4C` is legitimately
  executable — not a wild jump. The page-table-walked window is `0x40000000+`, where
  the first panic's `pc=0x40080000` sat.
- **TTR decoding verified correct** against those exact values — class eliminated.
- Ships `amix_xtrace()`: first 64 non-interrupt exceptions after the 040 MMU enable
  (`vec/ipc/pc/fa/ssw/sr/a7`), self-arming and self-closing. Instrumentation, not a fix.
- `BOOT.BIN` 12,762,364 B, zlib crc32 `F4782D71`. Build warning-free.

## 2026-08-20 — 040: PFLUSH was a no-op, PTEST was fabricated (06f45ce, 95bf4af)

`D910C41A` proved the MMU-enable fix and collapsed the preload to seconds; the kernel
then reached one VM-init stage further and panicked again (`page_sub` → `page_free`).

- Same disease as `MOVEC`→TC, one layer along. `cpuemu_31`'s six F5xx handlers call
  `mmu_op(opcode, 0)`; `cpummu.h` doesn't declare `mmu_op`, so it bound to
  `newcpu.cpp`'s copy — which was missing upstream's
  `if (currprefs.mmu_model) { mmu_op_real(opcode, extra); return; }`. Every 040 MMU
  instruction fell into the **fake** path: `PFLUSH` a complete no-op, `PTEST`
  fabricated from the address. `mmu_op_real()` had **no caller in the tree**.
- Localised by diffing the bench-proven `../amiberry`: its `cpuemu_31` handler is
  byte-identical; its `newcpu.cpp` has the guard. The divergence was ours alone.
- Both halves are on AMIX's VM-init path — page-table edits plus `getfault040.s`'s
  PTEST fault classifier — which is why the panic moved but stayed in `page_*`.
- Restored guard is model-gated: `UAE_040`/`UAEJIT_040` and all non-MMU tables keep
  the fake path verbatim; `UAE_030_MMU` uses `mmu_op30()` and is untouched.
- **Hypothesis 4 closed as correct as-is:** a 68040 ATC entry has no ASID or
  root-pointer tag on silicon either, so `PFLUSH` is the sole coherence point by
  architecture. Contract documented at the code site; URP/SRP deliberately unchanged.
- Callerless-entry-point sweep of cpummu.cpp: mmu_set_tc and mmu_op_real were the
  only two; every other entry point has live callers now.
- Harness 152 → **160**, new stale-ATC/PFLUSHA/PTEST guard proven non-vacuous.
  `BOOT.BIN` 12,762,364 B, zlib crc32 `095356E9`.

## 2026-08-20 — 040: the MMU was never enabled (2f78695, 84c5269, 12ca0c2)

`6FDBDA40` ran the whole 040 chain (strap → boot1 → boot2-040 → 31 MB preload →
warm reset → kernel) and panicked in VM init. Not a VM bug: `m68k_move2c()` handled
`MOVEC → TC` as `regs.tcr = *regp & 0xc000` and **`mmu_set_tc()` had no caller in the
tree**. Every `*_mmu040` accessor gates translation on `regs.mmu_enabled`, which only
`mmu_set_tc()` sets — so the kernel enabled its MMU, believed it was in virtual space,
and every access stayed transparent. Kernel VAs at `0x40000000+` fall in the Z3
expansion window (no RAM) → `page_sub`, wild pc `0x40080000`, F-line double panic.
Cases 4–7 (TTRs) were inert the same way (`mmu_tt_modified()` never called).
The 68030 uses `PMOVE`, not `MOVEC`, which is why the golden path never showed it.

- `fmt=0x0` on the F-line is **correct** (format `$2` is only the FPU-unimplemented
  path) — evidence of garbage execution, not a frame-format defect.
- MMU-enable print added: `68040 MMU enabled. TC=… Page size = … amix_mmu_on=…`.
- Console mirror: no gating bug (the gate was engaged; the kernel died before console
  init). Hardened to byte/word/long and announces itself once as `[SERMIRROR]`.
- Auto-click: added `amix_mmu_on` disarm + a hard session budget of 4 episodes; the
  old LBA heuristic can't trip on an 85 MB disk (hence `[CLICK] e48`). 030 unchanged.
- 040 run loop now honours `service_cadence` (identical at cadence 1). Preload
  round-trip cost: documented no-go, whole-L1D-flush candidate deferred to its own
  dispatch (same class as the cdfs page-crossing incident — needs bench proof).
- Ranked follow-up hypotheses if page_sub survives: write-back fabrication
  (ISSUE-10 class, now reachable), 8K page-size first run, URP/SRP without ATC flush
  (deliberately unfixed, wants a decision), 030-shaped memory contract vs the 040
  kernel.
- New guard `test_mmu040_movec_tc_enables_mmu` (real `movec d0,tc` through the 040
  table: translation on, page size derived, VA walks to the right frame; TTR arm) —
  proven non-vacuous. Harness 142 → **152**.
- `BOOT.BIN` 12,762,356 B, zlib crc32 `D910C41A`. Build warning-free.

## 2026-08-20 — 040: STOP never advanced the pc (54838a6, a05da20)

`806C76AE` brought the 040 core up for the first time — banner, `Emulation mode 4`,
`CPU=68040 FPU=68040`, SCSI boot ROM autoconfigured, 030 canary green — and AmigaOS
then wedged in the Kickstart strap: grey screen, SCSI silent after enumeration, pc
pinned at `0x00F81420`.

- `DM` of the live guest ROM shows `0x00F81420 = 4E72 2000` (`STOP #$2000`) with
  `60E6` (`BRA` back) at `0x00F81424`: the pc was pinned **on** the STOP, where a
  correct run sits at `$F81424`.
- `cpuemu_31.cpp` is the only table generated by the newer WinUAE `gencpu`, whose
  68040 STOP is **re-entrant** — it never advances the pc, relying on an upstream run
  loop that does. This tree is advance-and-stop; every other table ends STOP with
  `m68k_incpc(4)`/`m68k_incpci(4)`. Every `rte` therefore re-stopped and the
  instruction after STOP never ran, so exec's idle loop never reached its `bra` and
  the scheduler never re-checked `TaskReady`.
- **Interrupts were never the problem** — they were delivered throughout. The piscsi
  axis was a coincidence: any 040-MMU AmigaOS boot would have wedged identically.
- Fixed in `cpuemu_31.cpp`'s bridge block (used exactly once, the STOP handler), so
  the generated body stays byte-identical.
- Host guard `test_stop_advances_pc_all_tables` covers all four selectable tables;
  proven non-vacuous. Harness 126 → **142**.
- Sweep of all 1836 shared opcodes found only one other mismatch, `BKPT`, left alone
  (unreachable; the lineages disagree on the stacked pc) and documented.
- `BOOT.BIN` 12,762,340 B, zlib crc32 `6FDBDA40`. Build warning-free.

## 2026-08-20 — core1 wedged by a pre-init z3660_printf (d7e996f, af93d9a)

`BBD5C14F` proved the core0 fix — core0 reached `Starting CPU emulator on Core1`
and `060 starting now...` was gone — but core1 then died on **every** boot mode,
including the 030 path that was multiuser-green on `455A7D12` hours earlier.

- **The breadcrumb was the bug.** `z3660_printf()` takes a UART semaphore living
  in the SHARED struct; `shared` is assigned in `init_shared()` at `main.cpp:886`,
  and the `[Core1] alive` breadcrumb sat at `main.cpp:852`. Core1's first
  statement was therefore `while(((SHARED*)0)->uart_semaphore != 0);` — a spin on
  DDR `0x0000001C`. Nothing faults (`MMUTable[0]=0` runs at line 882, *after* the
  breadcrumb), so core1 spun silently forever and core0 hung on the ack.
  Mode-independent by construction, which is why golden 030 broke too.
- **Fix:** `z3660_printf()` is NULL-safe — semaphore once `shared` exists, bare
  `vprintf` before that, matching core1's long-proven pre-init prints. Breadcrumb
  keeps its earliest placement; the pre-init contract is documented at the
  definition and the call site.
- **Static-initialiser suspect refuted by measurement:** no `_GLOBAL__sub_I_` in
  any object touched that night; `.init_array` unchanged at 4 entries.
- Not host-checkable — the harness compiles no `main.cpp` and `board_stubs.cpp`
  fakes the very seam that broke. Stated rather than worked around. The
  static-ctor check IS mechanical and is recorded in the gate doc.
- `BOOT.BIN` 12,762,340 B, zlib crc32 `806C76AE`. Build warning-free.
- Still true: neither the 68040 access-error frame nor the `amix_mmu_on` latch has
  executed on hardware yet.

## 2026-08-20 — UAE_040_MMU: core0 never started core1 (9e1c776, adf8ea1)

First AMIX-040 metal boot (firmware `2CFA9216`) produced no core1 output and the
Amiga reset line cycled every ~3 s forever. Core1 did not crash — it was never
started, and the emulator was not involved.

- `Z3660/src/main.c` classified the boot mode with a copy-pasted list of the
  *emulator* modes that omitted `UAE_040_MMU`, so `emu` was 0 (the real-68060
  setting). `emu==1` gates the only site that starts core1, so core0 skipped it,
  took its real-CPU path and relinquished the bus to a 68060 that is not fitted.
  Log proof: `060 starting now...` — 1 occurrence in the entire serial history,
  this boot — instead of `Starting CPU emulator on Core1` (648 historically).
- The predicate was replicated **8×** (`main.c`, `debug_console.c` ×6, `rtg.c`),
  all omitting the mode. Now one `boot_mode_is_emulated()` in `config_file.h`,
  written as a **closed list of the real-CPU modes** so an appended mode defaults
  to emulated — which fails safe (core1 halts loudly) instead of handing the bus
  to absent silicon.
- New host guard `test/host/bootmode_test.cpp` (26 checks) asserts the invariant
  over every `BOOTMODE` index; proven non-vacuous against the pre-fix predicate.
  The rest of core0 is not host-compilable, so the symptom itself stays
  metal-only — stated explicitly rather than papered over.
- Permanent core1 breadcrumbs: `[Core1] alive` (first statement of `main()`) and
  `[Core1] dispatch: cfg_emu=N amix_mode=N`.
- **Nothing in the 68040 access-error frame or the `amix_mmu_on` latch was
  reached**, so neither is implicated and neither is yet metal-tested.
- Known issue, deferred: AmigaOS-side `z3660-drivers/ZTop/Ztop.c` carries the
  same class of bug — its private `BOOTMODE` enum and label table stop at
  `UAE_030_MMU`, so ZTop opened while the card runs mode 8 reads its label
  array out of bounds and cannot select the mode. Not on the AMIX boot path
  (no AmigaOS/ZTop in the chain); needs a two-line driver-side fix later.
- `BOOT.BIN` 12,762,340 B, zlib crc32 `BBD5C14F`. Build warning-free.

## 2026-08-20 — UAE_040_MMU: the 68040 fault path (10587eb, d25b073)

The two blockers the 040-readiness audit found, closed.

- **Access-error frame.** `Exception_normal()` tested `currprefs.mmu_model && nr == 2`
  — truthy for `mmu_model == 68040` — so every 68040 access error was handed the
  68030 PMMU's frame `$A`/`$B` builder and never reached the `cpu_model >= 68040`
  arm. Guarded to `== 68030`; the 040 arm now builds a real **format `$7`** frame.
  The contents were the actual defect: `case 0x7:` had the right field order but
  stacked **zeros** for every write-back slot, and the 68040 does not re-run a
  faulted write on `rte` — the store travels to the OS in WB3 for the kernel to
  complete, so zeroed slots dropped it. Now stacked from the real fault record:
  SSW, FA, EA, WB3S/WB3A/WB3D, WB2S/WB2A, PD0–PD3. `mmu_bus_error()` also now
  initialises `regs.mmu_effective_addr` (only MOVE16/MOVEM ever assigned it, so EA
  and `wb2_address` carried stale values). Field order and encodings cross-checked
  read-only against the port repo's `wb040.s` consumer offsets — exact agreement,
  including its metal-captured `WB3S=0x0081` for a user-data long write.
- **`amix_mmu_on` under 040.** Latched only in `cpummu030.cpp` on a 2 KB page size
  the 68040 does not have, so the emulated A3000 SCSI stayed in ROM-load timing for
  the whole boot. `mmu_set_tc()` now latches on `amix_mode` + the TC E-bit.
  `amix_mmu_on` = *when* (timing, both cores); new `amix_kimage_030` = *which guest*,
  gating every hard-coded `0x07xxxxxx` 68030-image read, derived from the CPU model
  in `a3000_scsi_init()` so the 040 path can never arm them.
- Host harness **126/126** (was 96) — `test_mmu040_access_error_frame` (24 checks,
  write + read fault: WB3D carries the real store value, write faults continue,
  read faults restart) and `test_amix_mmu040_latch` (6). `scsi_cd_test` 34/34.
- **Consequence worth naming: the emulator's 040 fault path is now behaviourally
  equivalent to silicon for `wb040.s`'s purposes** — deferred-write-back work
  (ISSUE-10/-22 class) should reproduce under `UAE_040_MMU`, not only on metal.
- Flagged, deliberately untouched: `a3000_scsi_init()` does not reset
  `amix_mmu_on`, so after an AMIX warm reboot the next Kickstart ROM load runs
  with runtime pacing instead of the proven immediate-completion timing — fixing
  it changes proven 030 behaviour, so it needs its own decision + metal 030
  regression. And `mmu_model == 68060` now lands on the synthesized MMU-off
  frame (inert today; a future 060 lane needs its own arm).
- `BOOT.BIN` 12,762,340 B, zlib crc32 `2CFA9216`. Build warning-free. Metal
  still untried for AMIX-040 — bench/harness-proven only.

## 2026-08-20 — UAE_040_MMU carries the AMIX contract (f7b2167, 970b33a)

`bootmode UAE_040_MMU` can now run the native AMIX 040 boot chain. Two defects
closed and the mode's gate doc brought up to date.

- **`main.cpp`**: the `UAE_040_MMU` branch passes `shared->amix_mode` into
  `uae_emulator()` (both the `MUSASHI_EMULATOR` and the plain dispatch block),
  exactly as `UAE_030_MMU` does. The whole existing AMIX contract then applies
  model-independently: single ≤16 MB window at `$08000000`, `a3000mem` at
  `$07000000`, emulated A3000 SCSI at `$00DD0000`, `a3000_scsi_init()`, and the
  `UAE_040_MMU_AMIX` banner. `UAE_040`/`UAEJIT_040` deliberately keep it off —
  no MMU / JIT-excludes-MMU, the same rule the 030 modes follow.
- **`uae_emulator.cpp`**: `fpu_no_unimplemented` is set for the 68040 iff
  `amix_mode`. The pref reads backwards — set means "the emulator does not
  implement the whole opcode set", so `fpp.cpp` raises the real 040 traps. The
  AMIX 040 kernel links Motorola FPSP and only executes it if the
  unimplemented-instruction trap (vector 11, frame format `$2`) and the
  unimplemented-data-type trap arrive. 68881/68882 and non-AMIX 040 unchanged.
- Host harness **96/96** (was 90/90): new `test_fpu040_unimplemented_trap` pins
  both directions — pref set → `FSIN` traps to vector 11 with a format `$2` /
  `$02C` frame; pref clear → same instruction executes in-emulator.
- **Operator note: `amix_mode` is NOT implied by `bootmode UAE_040_MMU`** — the
  config default resolves YES only for UAE_030_MMU. An AMIX-040 preset must
  carry `amix_mode YES` explicitly; the serial banner confirming it took is
  `UAE_040_MMU_AMIX`.
- `BOOT.BIN` 12,762,340 B, zlib crc32 `76FBA34C` (warning-free build). NOT
  flashed — see the audit below.
- **Audit (documented in `UAE_040_MMU_gate.md` §8, deliberately not fixed
  here):** two emulator-side blockers still gate an AMIX-040 boot —
  `Exception_normal()`'s `if (currprefs.mmu_model && nr == 2)` branch is truthy
  for `mmu_model == 68040` and builds a 68030 frame `$A`/`$B` for a 68040 fault
  (shadowing the `cpu_model >= 68040` arm — the first demand-paging fault gets
  a wrong-format frame), and `amix_mmu_on` — the A3000-SCSI "AMIX runtime
  timing" latch — can only be set by the 030 TC write (2 KB page gate), so the
  controller would stay in Kickstart-load timing for an entire 040 boot. The
  latch also arms hard-coded 030-kernel-image address pokes that must never
  fire against a 040 image.

## 2026-08-20 — httpd work FLASHED + METAL-VALIDATED (455A7D12 live)

`BOOT.BIN` crc32 `455A7D12` (from `7952863`) flashed to `0:/Z3660.bin` through
both deploy-kvm.sh CRC gates (staged + on-card both `455A7D12`, `COPY OK:
12762340 bytes (read == write, verified)`; `0:/FAILSAFE.bin` `7E03DA03`
untouched) and validated on the A4000+Z3660:

- New-build proof at `P`: `[httpd] FATFS mounted on 0: (read only)` — a string
  that exists only in this image.
- **`GET /download?path=0:/Z3660.bin`: 12,762,340 bytes byte-exact in 7.0 s —
  1.81 MB/s**, ~4× the measured upload rate (TCP streaming with a real window;
  upload remains ~457 kB/s).
- 1 MB round-trip on `1:` byte-identical; 80 KB asset through the static
  `1:/web` path byte-identical (the old engine truncated at 8 KB).
- `PUT ?path=0:/` with a small body: clean `404`, graceful close (curl rc=0).
  With a 1 MB body the 404 status still arrives but curl reports rc=56 — the
  server closes without draining the remaining request body, so TCP resets the
  in-flight tail. Inherent to answer-then-close; the status code is still a
  usable gate, and deliberately NOT "fixed" (draining a huge misdirected PUT
  before answering would stall the refusal for minutes).
- Traversal (`1:/../0:/…`) and unknown volume → 404.
- **40×-404 hammer then a full download on connection #41** — the CLOSE_WAIT
  pcb-strand class (server death after ~32 client-closed connections) is gone.
- Guest regression: production 030 UFS box boots to multiuser (run-level 2,
  `z3660amix`, `zen0` up at `.39`, 214 s from `R` including the normal AMIX
  bootstrap warm-reset double-cycle); boot log clean (`Emulation mode 4`,
  `68030 MMU enabled`, no SD FAIL, no panic). Clean `shutdown -i0`, box parked
  OFF.

## 2026-08-20 — httpd: single connection-teardown path (state + pcb lifetime) (7952863)

Follow-up to the streaming/download change: every way a connection could end
had its own cleanup and most were wrong. Consolidated into `http_conn_close()`,
the only place a connection ends and the only place the per-connection state is
freed. It detaches all five callbacks, then closes, then frees.

The rule the old code broke, read out of the BSP's lwIP 2.1.3: `tcp_close()`
may free the pcb before returning — `tcp_close_shutdown()` takes an RST path
that calls `tcp_free()` outright when the receive window is still short (the
request not yet `tcp_recved()`) — and an `ERR_MEM` is returned as `ERR_OK` with
a deferred close from `tcp_tmr`. After `tcp_close()` the pcb must not be used
again, and there is nothing to retry.

- **`http_send_404()` closed inline and freed nothing** — every 404 leaked a
  connection state, and because it closed before the request was `tcp_recved()`
  it hit the RST-and-free path, so the caller then used a freed pcb and the
  client got a reset instead of the 404 body. It now queues the body and sets
  `close_pending`; the close happens from `http_sent()`/`http_poll()` after the
  request is finished, which reaches the graceful FIN path.
- **`http_sent()`/`http_poll()` called `tcp_arg(pcb, NULL)` after
  `tcp_close()`** — the same use-after-close on every served response.
- **Peer FIN freed the state but never closed**, parking the pcb in CLOSE_WAIT
  permanently: a `tcp_pcb` leak (`MEMP_NUM_TCP_PCB` = 32) per client-closed
  connection, so the server eventually stops accepting.
- **`http_send_json_system()` early-returned** on snprintf/tcp_write failure
  without `close_pending` — state and pcb both stranded.
- **`http_accept()` closed the pcb and returned `ERR_MEM`** — lwIP aborts the
  pcb itself on a non-OK accept return, so that was a close plus an abort of
  the same pcb. Refusal is left to lwIP.
- `http_recv()` drops a connection handed a NULL state, establishing the
  invariant instead of each handler re-checking it.
- Host-harness negative control: the identical 14-scenario lifetime suite
  against the pre-fix tree fails 12 (leaks, use-after-free, CLOSE_WAIT
  strands); fixed tree passes all, ASan+UBSan clean. Send-engine and
  path-policy suites re-run unchanged, all green.
- Visible behavior change: a 404 is now a real HTTP response with a graceful
  FIN instead of a connection reset.
- Build: `BOOT.BIN` 12,762,340 bytes, crc32 `455A7D12`. Not flashed yet.

## 2026-08-20 — httpd: whole-file streaming + native GET /download (both volumes) (311694d..366a53a)

The boot-menu HTTP server could not send a response much larger than
TCP_SND_BUF (8192). `http_send_file()` looped `f_read(1024)` → `tcp_write()` →
`tcp_output()` and broke out on the first `tcp_write()` error; since nothing
pumps lwIP inside a callback, no ACK could arrive and no send buffer could
free up mid-loop, so every response was silently truncated at the send-buffer
boundary. Replaced with a `tcp_sent`-driven continuation (`http_send_pump()`),
with the open `FIL`, its read offset and the not-yet-accepted bytes carried in
the per-connection state; a `tcp_poll` net covers a pass that queues nothing.
Proven in a host harness on the extracted functions: the doc's 12,428,476-byte
benchmark payload delivered byte-exact through 1517 forced `ERR_MEM` refusals
(the old loop delivered 8,192 bytes of it).

- **New `GET /download?path=<vol>:/<file>`** — reads any file from `0:` or
  `1:`, same `?path=` convention (and percent-decoding) as `PUT /upload`.
  `0:` is mounted read-only in the httpd init (`f_clk_mount`, the sd_fileops
  pattern). Paths must name `0:`/`1:` and must not contain `..`; else 404.
- **`Content-Length` on all file responses**, so a short transfer is
  detectable host-side instead of silent. Downloads also get
  `Content-Disposition`. `Connection: close` unchanged.
- **`1:/web` assets share the engine** — they had the same truncation bound.
- **Uploads stay `1:`-only, now by explicit policy.** The `0:` refusal
  previously held only because that volume was unmounted; mounting it for
  reads would have made the boot volume HTTP-writable. Flashing stays on the
  boot-menu `E` → `COPY` → `CRC` gate with FAILSAFE behind it. `/api/dirs`
  (the shipped UI's upload destination selector) drops its `0:` candidate for
  the same reason — no web-UI JS change needed.
- Bounded `extract_filename()` (unbounded target length → caller-buffer
  overrun); closed files leaked by aborted transfers (`tcp_err`, recv-NULL);
  removed per-chunk console logging from the send path (`printf2` also paints
  HDMI — per-ACK logging would dominate a multi-MB transfer).
- Docs: `http-vs-tftp-upload.md` practical guidance rewritten by direction;
  the stale "deploy-kvm.sh broken on beta 21" warning retired (the script was
  rewritten to the HTTP → COPY → dual-CRC route and works).
- Build: `BOOT.BIN` 12,762,340 bytes, crc32 `C05D5F5C`. Not flashed yet;
  metal validation pending (queued: >8 KB web asset, `GET ?path=0:/Z3660.bin`
  vs on-card `CRC`, `PUT` to `0:` still 404).

## 2026-08-19 — Beta-21 consolidation FLASHED + METAL-VALIDATED (CEEC4984 live)

Same-day follow-up to the consolidation entry: `BOOT.BIN` crc32 `CEEC4984`
(from `1e47b77`; clean containerized build with the FSBL rebuilt — it reproduces
byte-identical to the committed `fsbl.elf`) was flashed to the card's
`0:/Z3660.bin` through both deploy CRC gates and validated on the A4000+Z3660:

- Cold boot verified: banner `1.03 (BETA 21)`, boot-menu `P` = HTTP server,
  file-version report `Z3660.bin v1.03 BETA 21`.
- Guest validation: the production golden (030 kernel r3 `sum -r` 55482) boots
  to multiuser with **working ethernet** (.39 answers) — piscsi, 030 MMU, eth,
  C0STAT/PSTAT all live. The B1 boot2-040 test image also booted to login
  (its zen0 stayed down — an image-lineage delta vs the live golden, not a
  firmware issue: the same firmware brought the live golden's network up).
- **Warm-reset restoration proven on metal**: guest `shutdown -i6` →
  `custom_reset` → Core0 reset → full reboot to login (the pre-restoration
  post-fsck hang class).
- The legacy "keep BOOT.BIN under ~12 MB" SD-write warning is retired: the
  12,762,340-byte image staged (TFTP), copied (`read == write, verified`) and
  CRC-matched on-card.
- cdfs/CD checks were NOT exercised (current card cfg has no `isoN` unit);
  DMA canary sums unchanged across the attempts. CD validation queued for a
  session with the golden cfg.
- Rollback set left on 1:: `Z3660-b21-CEEC4984.bin`, `Z3660-bak-BF1805DB.bin`
  (the instrumented crawl build this flash replaced), `Z3660-prod-59FAE8CA.bin`;
  `0:/FAILSAFE.bin` (`7E03DA03`) untouched.

## 2026-08-19 — Beta-21 consolidation: amix-main-2 is the single firmware line (203f108..cc54084)

`amix-main-2` is now the going-forward firmware line; `piscsi-crawl` is frozen
as reference. Content-level audit of both lines (they share **no** git ancestor
— the beta-21 resync was a re-port, so commit identity proves nothing;
`docs/port-audit-beta21-consolidation.md` holds the full table) ported the six
post-resync fixes: CLMT uninstall on `f_lseek(CREATE_LINKMAP)` failure
(d182e41), 256-item fast-seek link map (aea93bf), zero-block-size divide guard
(c0510a7), request-path `f_lseek`/`f_write` error handling (dc81392), PSTAT
per-span latency counters (9b56883), C0STAT per-task accounting (b4c2789,
extended to beta 21's floppy mailbox arms in cc54084), plus their service-path
and blocking-audit docs.

- **The audit caught a re-port regression the changelogs had hidden:** the
  core0 half of the warm-reset chain — `piscsi_init()` re-run on guest reset
  and the stale-`FPGA_INT6` clear — had been dropped while the CORE1 half
  survived, so a soft reboot (notably fsck's post-repair reboot) would hang at
  `0x00F81212`. Restored as 1f3521f + dc38a02.
- Everything else spot-checked PRESENT (030-MMU fix set, CD-ROM units,
  direct-DMA + SD-DMA + AMP core-1 coherence, RDB 512-byte scan, JIT -Os fix);
  two legitimate N/A (superseded warm-reset variant; `cdrom_units` key replaced
  by upstream's `isoN`).
- Containerized build verified twice from clean, byte-identical: `BOOT.BIN`
  12 680 420 bytes, CRC32 `E64FA66D`. Note: this exceeds the legacy ~12 MB
  SD-write warning (already exceeded pre-port) — flash procedure CRC-gates
  every hop and creates FAILSAFE.bin first.
- Trap for future line switches: `build/` survives branch switches and the
  Makefile has no header deps — wipe `Z3660/build` and `Z3660_emu/build` when
  moving between lines, or stale objects produce phantom link errors.

## 2026-08-12 — Investigation: how `amix_mode` presents RAM to AMIX, and what more than 16 MB would cost (07cecf8, docs-only)

`docs/amix-ram-presentation.md`, doc-only — no code or config touched. The 16 MB
is a **compile-time emulator memory-map decision**, not a config knob and not a
kernel limit: `amix_mode` is a boolean the whole way through (config key →
`SHARED` `0xFFFF00C0` → `uae_emulator()`'s fourth argument), and the size is the
literal in `RANGE_MAP(0x0800, 0x0900, drct_bank)` (`uae_emulator.cpp:1141`)
ended by the `dmmy_bank` line after it. Under `amix_mode` the firmware never
publishes a memory-list entry at all — it makes a range behave like RAM and
everything above it behave like nothing, and the guest discovers the extent.

- **Two independent 128 MB ceilings, and they agree**: the RTG board at guest
  `$10000000` (where `z3660eth`'s board window lives) and core 1's `RESERVED`
  (fault) page-table entry for host `0x10000000`–`0x17FFFFFF` — both inherited
  from upstream unchanged. Host DDR is free `0x08000000`–`0x18000000`, so DDR is
  not the constraint. The change surface is two `RANGE_MAP` lines plus retiring
  the vestigial `a3000mem` window whose host backing sits at `0x09000000`; the
  sharpest hazard is `get_real_address()` (`memory.h:801-810`), which aliases
  guest `$07xxxxxx` → host `$09xxxxxx` and must be dropped with the bank. Both
  page tables already cover 128 MB, the piscsi direct-DMA gate already spans
  `[0x08000000, 0x10000000)`, and `z3660eth` never DMAs into guest RAM.
- **`amix_mode` forces `cpu_ram` off but not `autoconfig_ram`**, while the
  shipped defaults and all four shipped presets set `autoconfig_ram YES` — a
  second, 256 MB pool-linked Zorro III region. Source-level observation, not a
  reproduced failure; reading the metal card's active preset resolves it.
- Risk register puts the safe cap at **64 MB** (2× margin under the ~128 MB
  describable-RAM ceiling, never bisected, and it moves as the kernel grows);
  the staged plan opens with one cheap flash whose single serial line decides
  whether the approach works at all.

## 2026-08-09 — P3 040-MMU lane, build-ahead (3.1–3.3 done; 3.4 parked)

Scoped build-ahead of a 68040-MMU emulation mode, ahead of the external 68040 AMIX
kernel that will eventually consume it (no 040 kernel exists to boot yet — every step
here is validated only as far as is possible without one). Four commits, unpushed
(fork, local-only):

- **`704b549` → `6ec08f6` — the 12 MiB `BOOT.BIN` cap is a ghost, and the 68040-MMU
  table is generated + linked.** The size gate first read NO-GO (a sibling opcode table
  overshoots the conventional ~12 MiB line), but that line proved to be a dead TFTP-era
  rule of thumb: the real cpuemu_31 (id 31 = 68040 MMU) + cpustbl_mmu040 tables were
  generated via the WinUAE-4.4.0 gencpu flow, linked in (+291 KB), and the resulting
  **12.156 MiB image booted on real metal** (cap test 2026-08-09: `[SD Init] Done
  (Z3660.BIN)`, not FAILSAFE; production `59FAE8CA` restored after). The ~12 MiB gate in
  CLAUDE.md/port-ledger is advisory, not a boot-load wall.
- **`6848894` — `UAE_040_MMU` is runnable.** `m68k_run_mmu040()` run loop, the
  `set_x_funcs()` 040 accessor arm (mmu_get/put, distinct from the 030 bank), `main.cpp`
  dispatch, and the six link-only compile bridges made faithful (the non-obvious ones —
  `do_cycles_stop`, `MakeFromSR_STOP`, `op_unimpl` — reasoned in the gate doc). Append-only
  bootmode enum (index 8, both copies). Host smoke 69/69 (runs moveq/add/move.l through the
  id-31 table, MMU off). Metal MMU-off AmigaOS boot smoke QUEUED in next-metal-session
  (crc `92E1473F`).
- **`5ebae16` — 68040 MMU address translation validated on the host, 90/90.** The 040
  analog of the 030 MMU vectors, driving the real `cpummu.cpp` engine (which turned out
  already complete — no core file needed touching): root→pointer→page table walks (4K/8K),
  TTR transparent windows, protection (invalid/write-protect/supervisor), U+M bit updates,
  ATC hit; a single-translation smoke asserts VA 0x00408678 → PA 0x0A000678 against the
  hand-computed frame.

`UAE_040_MMU_gate.md` carries the measured GO verdict (§ gate), the coverage table (§6),
and — most usefully — a written **gap list (§7)** of 68040 MMU behaviors not yet modelled,
ordered by when a booting 040 OS would hit them: #1 the format-$7 bus-error stack frame
(the parked 3.4; its frame *builder* is host-assertable ahead of a kernel, its *integration*
is not), #2 no cache model (CINV/CPUSH/MOVE16, cache-inhibit coherency), then PFLUSH/PTEST,
indirect descriptors, page-crossing faults, ATC eviction. **3.4 stays parked pending an
external 68040 kernel** (user decision); the lane is otherwise as far advanced as build-ahead
allows.

## 2026-08-06 — new carrier revision: bring-up findings and the 060 rule

- **`docs/carrier-bringup-checklist.md` (`849a4bd`).** Two findings from the new carrier
  revision's bring-up, A/B-proven against the old carrier with the same Zynq module, SD card,
  firmware (`59FAE8CA`) and A4000: **the 68060 must be physically fitted even in pure-emulation
  boot modes** — an empty socket leaves the interrupt//BERR/arbitration lines floating, which
  the boot-time memory test never drives, so the test passes while Kickstart dies 3–13 s in
  (the auto-clicker acknowledges the alert → RESET at ROM `f80de8`, endless reboot loop); and
  **`enable_test`/`test_range` as the carrier-fault discriminator** — its failure pattern
  isolated a CPLD solder bridge (32-bit chip-RAM cycles all garbage while 8-bit CIA cycles
  passed) and a stuck-high D23 (the A4000 Fast-RAM SIMM unseated during the board swaps), plus
  the operational gotchas (region prompts drain buffered `C` keys before the countdown;
  re-disable the test for unattended boots). The new carrier was validated on metal 2026-08-06:
  full stress battery — CD/DMA canaries, disk, CPU, network, warm reboot — PASS.

## 2026-08-02/03 — the web UI, the httpd defects, and the metal validation

- **`httpd` sends a well-formed response header now (`2f4e58a`).** Beta 21's 200-response ended
  every header line with a bare `\r` instead of `\r\n`, so the whole block arrived as one run-on
  status line with no blank line before the body: `HTTP/1.1 200 OKContent-Type: text/htmlConnection:
  close`. The file was transmitted intact — a raw socket read got every byte — but a conforming
  client discarded the body, which read as an empty page. `http_send_404()` two functions later
  already spelled `\r\n`, which is what identified it as a typo. `.json` was also added to
  `mime_type()`; the shipped UI fetches `web/locales/*.json` and they were going out as
  `application/octet-stream`.
- **The web UI's own files are now shipped (`2f4e58a`, `7b9b874`).** `httpd` serves from `1:/web`,
  but beta 21 ships those files only under `Z3660/src/web/` — nothing installs them, so an updated
  board answers every request with `f_open failed … 5` and a 404. Vendored into
  `z3660-zturn_SD_content/…/web/` so they travel with the card image. The markup's fallback text is
  also translated: the loader hardcodes `lang='en'` and fetches only `/locales/en.json`, but every
  literal in the HTML was Spanish — so whenever that fetch failed (i.e. always, given the header
  defect) the page silently rendered in a language the code never selected.
- **Both fixes flashed and verified on hardware 2026-08-03** (firmware crc32 `59FAE8CA`, flashed
  through the rewritten `deploy-kvm.sh`): a strict client now receives the three header lines
  separately and the full 7,410-byte body — it received **zero** before — and `locales/en.json`
  arrives as `application/json` and parses. Post-flash gates all green: `BETA 21`,
  `Emulation mode 4`, `68030 MMU enabled`, A3000 SCSI initialised, Amix to multiuser on the LAN,
  and `validate-metal.sh --cycles 5` PASS on the CD stack.
- **`docs/http-vs-tftp-upload.md` (new)** — measurements behind the workspace's move off TFTP,
  including the correction that HTTP uploads do *not* run while the guest does.
- **Defect catalogue in `docs/AMIX.md` now lists seven beta-21 defects**, none reported upstream:
  the L1-only cache maintenance, the `else`-after-`else`, the `machdep/m68k.h` include guard, the
  `isoN` slot/target trap **and its out-of-bounds write**, the malformed HTTP header, the
  uninstalled web UI, and the UI's fake file rows (`core.bin`/`config.ini`/`logo.png`) with dead
  action buttons.

## 2026-08-01 — beta-21 resync complete (branch `amix-main-2`)

Rebased the Amix fork onto upstream firmware v1.03 beta 21 (`upstream/main` `1dddf79`) on a
fresh branch — upstream rewrote history, so there is no merge base and the resync is a
guided replay, not a merge. Plan of record: `docs/port-ledger-beta21.md`.

**Steps 1–4 (`90cf432..06645d6`).** Build plumbing, `-Os` + `--gc-sections`, the
`SHARED`/`BOOTMODE`/`CONFITEM` ABI merges, and the 68030 PMMU engine + host MMU harness.

**Steps 5–10 (`ebe5381..2fea74c`).** `main.cc` is gone upstream, so its six-hunk delta moved
into `main.cpp` (the `z3660_dbg_emu` accessor, the `a3000_scsi_irq` INT2 injection,
`PISCSI_CMD_PDT`, the AMP read-coherence stash+invalidate from `0a4c064`, the PDT read case,
the `UAE_030_MMU` dispatch). The emulated A3000 WD33C93+SuperDMAC lands whole with the AMIX
memory contract and the CD-ROM CDB test; `scsi.c` regains the 512-byte RDB scan and the
`amix_mode` direct-DMA gate; `config_file` regains `amix_mode`/`service_cadence`; the console
regains DEMU/PERF/SERV alongside beta 21's DENET.

- **§4g — the flash-safety gate (`ddb7caf`).** Beta 21 had weakened our direct-DMA cache
  maintenance to **L1-only** on both the piscsi READ and WRITE paths while keeping our
  "L1+L2" comments. The full-hierarchy `Xil_DCacheInvalidateRange`/`Xil_DCacheFlushRange`
  are restored. On real hardware the weakened form can send stale DRAM to disk and let an L2
  writeback land on freshly DMA-ed bytes; amiberry models no cache hierarchy, so the bench
  cannot test this — **the first flash is the test**.
- **`isoN` adopted (`71f9240`), `cdrom_units` dropped** (grep-absent tree-wide). An ISO is an
  `hdf[]` slot flagged as a CD, so **the slot number must equal the SCSI target**: `iso4` +
  `scsi4 4`. Documented in `z3660cfg.txt`, `config_file.h` and `docs/AMIX.md`.
- **`iso7`..`iso19` are now refused (`8dad55a`).** Upstream's parser computes `index` 0..19
  but `cd_target[]` is 7 deep, so those keys wrote past it into `adf_num[]` and the head of
  `hdf[0]` — silent config corruption in shipping upstream code. Guarded, with a console
  line naming the 0–6 limit. The shipped example moves to the correct `iso4`/`scsi4 4` form
  (upstream's `iso6` + `scsi4 6` pairing produced no working CD).

**Gates at final state:** docker build green with the full engine linked; `BOOT.BIN`
12,428,476 B = 11.853 MiB, **154,436 B under the 12 MiB threshold** (+16 KiB vs the
metal-proven golden `fee33b02`); host MMU harness **64/64**; CD-CDB test **34/34**; the two
`SHARED` copies byte-identical at 51 fields; `CONFITEM`/`config_item_names[]` **129/129**
(`CONFITEM_ISO0..19` inserted at 25–44); `COMMANDS`/`command_names[]` 83/83.

**Four beta-21 defects** are recorded in `docs/AMIX.md` as upstream-report candidates: the
L1-only cache maintenance, an `else`-after-`else` in `main.cpp`'s dead preprocessor arm, the
lost two-phase include guard in `machdep/m68k.h` (blocks any x86 build — fixed here in the
x86 arm), and the `isoN` out-of-bounds write.

## 2026-07-25

- **beta-21 resync port ledger (analysis only).** `docs/port-ledger-beta21.md` (`939936b`): the
  full commit-by-commit plan for rebuilding this fork on upstream firmware v1.03 beta 21.
  Derives the true fork set (90 commits, `main..amix-main`) despite upstream's history rewrite
  leaving no merge base — the resync must be a replay, not a merge — and classifies every commit
  ABSORBED / RE-PORT / DROP with file-level evidence (2/64/18 + 4 split). Contract verdicts:
  ethernet and piscsi-mailbox deltas both **none** (`z3660_scsi_enums.h` byte-identical), so the
  Amix drivers and the amiberry emulation need no changes. Notable hazards recorded: beta 21
  transcribed our direct-DMA coherency comments but weakened the calls to L1-only (§4g — real
  stale-L2 corruption risk on any non-bounce piscsi transfer; re-port + upstream-report
  candidate); a silent cross-core `SHARED`-struct ABI collision at `0xFFFF009C` (§5); an
  invalid-C++ `else`-after-`else` in beta 21 `main.cpp` hidden by preprocessing; BOOT.BIN size
  pressure (`-O3`, gc-sections off) sequenced to be re-proven before the MMU engine in P2.
  `docs/AMIX.md` gains a pointer.

- **piscsi READ: invalidate core1's L1 after the DMA (AMP coherence).** The last T2.P3
  metal bug, and the one that made direct-DMA CD reads unusable. Core0 owns the SD
  transfer and (since e37bb43) invalidates L1+L2 for the target range *before* it — but
  **core1**, the 68k emulator, is parked in `while(shared->write_scsi==1){NOP;}` for the
  whole transfer, and a parked Cortex-A9 is still live: it speculatively refills its own
  L1D over that same range while the DMA is in flight. Nothing ever invalidated core1's
  L1 *after* the transfer, so the guest read a mix of fresh DRAM and stale cache lines.
  `write_scsi_register()` now stashes the READ descriptor as the guest programs it
  (ADDR2 = byte length, ADDR3 = DMA target) and, after the completion spin, calls
  `Xil_L1DCacheInvalidateRange(addr,len)` for READ/READ64/READBYTES only — the WRITE path
  never reads its buffer back, and its trigger-time flush is unchanged. The BSP
  clean+invalidates only the unaligned edge lines, so neighbours of odd driver buffers
  survive. Covers disk **and** CD reads, so it plausibly also closes the rare disk-read
  corruption seen earlier on this platform (the cdfs one-sector cap had been masking it
  by shortening the speculation window).
  Metal-diagnosed on the real A4000+Z3660: with the forced bounce gone, a *quiescent*
  `mount -F cdfs` read the CD byte-correctly, but under combined CD+disk load the guest
  kernel's kmem daemon panicked on a wild freelist pointer (a stale line landing on a
  kmem pool page), and a second run silently corrupted the root filesystem — two failures
  out of two. With the fix: zero panics, zero corruption, and the full R4 read suite
  byte-identical to the ISO (all 7 files `cmp`-verified host-side, including a UTF-8
  name, a three-level directory and a symlink deref), clean umount + remount.

- **De-instrument the piscsi transfer path for production.** Revert the two TEMPORARY
  diagnostic commits that cornered the (now fixed) cdfs-mount fault: the A/B discriminator
  (af29745 — it force-bounced every non-root unit, masking the direct-DMA read path
  entirely) and the per-transfer crumb instrumentation (09520e0). The SD-controller
  coherence fix those crumbs uncovered (e37bb43) is retained verbatim: the WRITE-DIRECT
  revert conflict was resolved by keeping `Xil_DCacheFlushRange` and dropping only the
  crumb call. Net `scsi.c` state = d1da9f8 + bcc8227 + e37bb43. BOOT.BIN 12,412,164 B
  (< 12 MiB gate).

## 2026-07-11

- **firmware boot menu: SD-card file manager (key 'E').** New self-contained
  module `sd_fileops.c/.h` adds an interactive `SD>` command loop over the serial
  console, reachable from `show_options()`/`main_thread()` in mobotest.c. Mounts
  both SD volumes (0: FAT boot, 1: exFAT data) and offers DIR, COPY, REN, DEL,
  MKDIR, CRC, FREE, HELP and EXIT, with explicit `vol:/path` arguments. COPY
  generalizes the fixed BOOT.bin/FAILSAFE.bin/Z3660.bin copy shortcuts: streamed
  through a 4 MB→64 KB fallback buffer (so multi-GB hdf images work), progress
  every ~10%, read-total vs write-total verify, overwrite confirm and ESC-abort.
  CRC is a zlib-compatible CRC-32. Upstream-clean (mobotest.c + the new pair
  only); +8 KB core0 text, no BOOT.BIN size change. Verified on the board
  2026-07-11: CRC matches host zlib byte-exact (incl. the freshly TFTP'd
  Z3660.bin), 943 MB image copy in 101 s with verify green. Also on branch
  `pr-sd-fileops` (same two commits on upstream 5e216af) for an eventual
  upstream PR.

## 2026-07-10

- **scsi: DMA AMIX DDR CPU-RAM buffers directly instead of bouncing.** After the DDR move
  (22ce6f2 put AMIX RAM at $08000000) the firmware's direct/bounce boundary was gated on
  `cpu_ram` — force-disabled for AMIX — while both drivers key their bounce protocol on
  `BOUNCE_THRESH = 0x08000000`. The two sides disagreed about who copies. Gate on
  `cpu_ram || amix_mode` so a buffer inside the drct_bank window
  `[0x08000000,0x10000000)` is DMAed in place, honouring the driver contract (d1da9f8).

- **scsi: make direct-DMA piscsi transfers coherent with the SD controller DMA.** The SD
  controller DMAs to DRAM *below* the PL310 L2, while the guest's freshest bytes live in
  the cache hierarchy. Reads now `Xil_DCacheInvalidateRange` before the transfer, so a
  stale dirty L2 line cannot be written back on top of freshly DMAed bytes (observed on
  real HW: a UFS superblock read returning old guest memory). Writes now
  `Xil_DCacheFlushRange` before the transfer — the write path had **no** cache maintenance
  at all and so deterministically wrote stale DRAM to disk, which physically corrupted the
  SD image's UFS superblock (e37bb43).

- **emu: stop logging the per-command piscsi PDT register poll** — serial noise on every
  command; cosmetic only (bcc8227).

## 2026-07-07

- **piscsi: read-only 2048-byte CD-ROM units.** Expose a SCSI target as a
  read-only CD-ROM backed by a raw ISO/UDF image, alongside the existing RDB
  hardfiles. `struct piscsi_dev_` gains a `pdt` (peripheral device type) field;
  a new read-only mailbox register `PISCSI_CMD_PDT` (0xA0) returns the current
  drive's `pdt` and is the single source of truth for device type. A CD-flagged
  slot in `piscsi_map_drive()` bypasses the RDB scan (a bare optical image has no
  RDB), opens its backing image read-only, reports a 2048-byte block size
  (`BLOCKS = file_size / 2048`), sets `pdt = 0x05`, and uses benign non-zero CHS
  geometry; the normal disk path is unchanged. New config key
  `cdrom_units <id[,id...]>` flags SCSI target IDs as CD-ROM units, threaded
  through `piscsi_init()` into `piscsi_map_drive()`. Removability (start/stop,
  prevent/allow, media-change, unit attention) is deferred — the medium is
  assumed always present in this phase.
- **a3000_scsi (emulated A3000 WD33C93): present a read-only CD-ROM to the stock
  Amix kernel.** A backend unit flagged as a CD-ROM (via `PISCSI_CMD_PDT`, `pdt =
  0x05`) is shown as a read-only optical drive — INQUIRY device-type 0x05 + RMB +
  `"AMIX CD-ROM"`, READ CAPACITY at 2048-byte blocks, MODE SENSE(6)/(10)
  write-protect bit with the rigid-disk geometry pages dropped, WRITE(6)/(10)
  rejected with CHECK CONDITION / DATA PROTECT, and PREVENT/ALLOW MEDIUM REMOVAL as
  a GOOD no-op. The disk path (`pdt = 0x00`) is unchanged. New host unit test
  `test/host/scsi_cd_test.cpp` asserts the exact CDB response bytes (34/34) — the
  byte-level reference the native z3660scsi driver (Path A) must match. READ TOC
  and removability/media-change are deferred to a later increment.
