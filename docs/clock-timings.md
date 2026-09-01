# Clock timings: how the table is loaded, what card A changed, and what overwrites it

*Fork note (2026-08-28, extended 2026-09-01).* Source references are to beta 21 as it sits in this
tree. The numbers below were read off each SD over the firmware's own HTTP server and
byte-compared; the clock frequencies were read back from the board's own serial output, which
prints the **programmed** Clocking Wizard registers rather than the file it read.

**Two cards are referred to throughout, and they are not interchangeable:**

| | Card | `default_timings.txt` in force |
|---|---|---|
| **A** | the fork's original bench card (A4000) — every measurement dated 2026-08-28 or earlier is this one | 5 163 B, sha256 `fbe6cb7ac445754d…` |
| **B** | a second Z3660, measured 2026-09-01 (A4000D, socketed MC68LC060 rev 4, no FPU) | 5 163 B, sha256 `f6b5683640e8b1eb…` |

Same size, different content. §4.1 records what does and does not carry across.

The short version, if you read nothing else:

* **Card A's `default_timings.txt` is hand-tuned and this repo's template is not a copy of
  it.** Copying `z3660-zturn_SD_content/.../timings/default_timings.txt` onto the card, or
  letting the firmware regenerate the file, reverts the tuning. §4 records the exact delta so it
  can be put back. §4.2 says what that revert *looks like* on a real 060, because it does not
  look like a clock problem.
* **A tuned row is a property of one board.** Card A's 70/80/100 rows do not work on card B.
  §4.1.
* **`SCT` at the debug console rewrites that file** from whatever is in RAM, with no prompt and
  no backup. So does the ZTop timings-editor OK button, and so does the mobotest `G` key. §5.
* **`axi.divider = pclk.divider / 2`**, derived by the parser — so `AXICLK = 2 x PCLK` in
  real-CPU mode, and 160 MHz at an 80 MHz PCLK. §3.
* **`emu_extra_phase` does nothing in real-CPU mode.** §6.

---

## 1. Three tables, and only one of them is on your SD card

| # | Table | Where it lives | When it is in force |
|---|-------|----------------|---------------------|
| 1 | compile-time initialiser of `cd[]` | `config_clk.c:13-149` | from power-on, and it **stays** if `read_timings()` fails |
| 2 | `set_default_timings()` | `config_clk.c:534-733` | only when something calls it — and it then **writes itself to the SD** |
| 3 | the SD file | `1:/timings/<name>`, named by `1:/timings/timings.txt` | normal operation, via `read_timings()` at `main.c:1565` |

They are three *different* tables, not three copies of one. Table 1 still has the pre-beta-18
shape where `bclk` tracks `pclk` (index 6: VCO 960, `pclk.divider 12`, `bclk.divider 12`, so BCLK
= PCLK = 80 MHz); table 2 has the decoupled shape (index 6: VCO 1120, `pclk.divider 14`,
`bclk.divider 56`, so PCLK 80 MHz and BCLK 20 MHz).

The `default_timings.txt` shipped in this repo under
[`z3660-zturn_SD_content/DATA Second Partition (exFat)/timings/`](../z3660-zturn_SD_content/DATA%20Second%20Partition%20(exFat)/timings/)
is a **fourth thing**: a file written back by a 1.03 BETA 20 firmware under the name
`AA3000_timings.txt` — its own first two lines say so. Despite that name it matches table 2 at
every index except **14**, where it still carries the beta-20 values (`cpuclk`/`clk90` divider 48
and phase 200/290, `emu_extra_phase 60`) against table 2's 40 / 180 / 270 / 40. So it is close to
the current compiled defaults but not equal to them, and it is a snapshot of one machine's file
rather than a canonical default. Treat it as a starting point.

## 2. How the file is read

`read_timings()` (`config_clk.c:202-345`) opens `1:/timings/timings.txt`, takes its **first line**
as a filename, and parses `1:/timings/<that name>`.

* Recognised keys are exactly `index`, `global.multiplier`, `global.divider`, the `.phase` and
  `.divider` of `pclk` / `clken` / `bclk` / `cpuclk` / `clk90`, and `emu_extra_phase`
  (`enum TIMING_ITEMS` and `timing_item_names[]`, `config_clk.c:153-187`). Anything else —
  including any `axi.*` or `.dutycycle` line — is **ignored silently**.
* The parse writes into the live `cd[]` **in place**. There is no clear-first step, so a key a
  block omits keeps the compiled value from table 1. A partial file therefore yields a hybrid
  table, not a diagnosable error.
* If either `f_open` fails, `read_timings()` returns 0, **prints nothing**, and `main.c:1565`
  ignores the return value. A missing, renamed or misspelt timings file is thus completely
  silent, and table 1 is what gets programmed. The only way to notice is to read the clock
  frequencies back off the serial log (§7).

## 3. The derivation, AXI included

The Clocking Wizard's input reference is **200 MHz**, so

```
VCO         = 200 * global.multiplier / global.divider     (M / D)
output MHz  = VCO / that output's divider
```

`print_clkinfo()` computes the same thing from the live registers (`config_clk.c:421-425`), and
the register-write comment at `config_clk.c:494` states the reference explicitly.

**AXI is derived from the file, not carried by it.** The timings file has no `axi` key and
`write_timings()` never emits one. Instead, at the moment a `pclk.divider` line is parsed,
`read_timings()` sets

```c
/* config_clk.c:299 */
cd[index].axi.divider = cd[index].pclk.divider / 2;
```

so **`axi.divider = pclk.divider / 2`**, and therefore `AXICLK = 2 x PCLK`. On card A at index
6: `pclk.divider 10` gives `axi.divider 5`, and with VCO 800 that is **PCLK 80.00 MHz, AXICLK
160.00 MHz** — which is exactly what the board prints. Three indices measured on card A, two
VCOs, all three matching:

| index | VCO | `pclk.divider` | ⇒ `axi.divider` | PCLK printed | AXICLK printed |
|------:|----:|---------------:|----------------:|-------------:|---------------:|
| 0 | 800 | 16 | 8 | 050.00 | 100.00 |
| 4 | 1120 | 16 | 8 | 070.00 | 140.00 |
| 6 | 800 | 10 | 5 | 080.00 | 160.00 |

Two caveats:

* the division is integer, so an **odd** `pclk.divider` truncates;
* if an index block omits `pclk.divider`, `axi.divider` is never recomputed and keeps whatever
  table 1 compiled in. That is the one case where the relation above does not hold.

**In an emulated boot mode the relation is different.** `configure_clk()` multiplies
`pclk.divider` and `clken.divider` by `emu_div = 2` when `emu != 0` (`config_clk.c:486-501`) but
does **not** scale `axi.divider` (`config_clk.c:495`). PCLK and CLKEN halve; AXICLK does not
move. So under an emulator `AXICLK = 4 x PCLK`.

## 4. Card A's table is hand-tuned, and it is not in this repo

The file in force on **card A** is `1:/timings/default_timings.txt`, 5 163 bytes, sha256
`fbe6cb7ac445754d25428562119862723aaf0cc3bef532049559ea70fb352a7e`. Its header says it was
generated by 1.03 BETA 21 — i.e. it was written back by the firmware at some point, not
hand-edited on the SD. Everything in this section is card A's; **§4.1 is the part that matters if
you are holding a different board.**

Against the compiled defaults (`set_default_timings()`, table 2) it differs at exactly **two**
indices, and **only in dividers — every phase, and every `emu_extra_phase`, is identical**:

| index | field | compiled default | card A |
|-------|-------|-----------------:|-------:|
| **0** (50 MHz) | `bclk.divider` / `cpuclk.divider` / `clk90.divider` | 64 | **32** |
| **6** (80 MHz) | `global.multiplier` / `global.divider` | 28 / 5 | **4 / 1** |
| | `pclk.divider` | 14 | **10** |
| | `clken.divider` | 28 | **20** |
| | `bclk.divider` / `cpuclk.divider` / `clk90.divider` | 56 | **32** |

What that actually buys, once the arithmetic of §3 is applied:

| index | table | VCO | PCLK | AXICLK | CLKEN | BCLK / CPUCLK / CLK90 |
|-------|-------|----:|-----:|-------:|------:|----------------------:|
| 0 | compiled / shipped | 800 | 50.00 | 100.00 | 25.00 | 12.50 |
| 0 | **card A** | 800 | 50.00 | 100.00 | 25.00 | **25.00** |
| 6 | compiled / shipped | 1120 | 80.00 | 160.00 | 40.00 | 20.00 |
| 6 | **card A** | **800** | 80.00 | 160.00 | 40.00 | **25.00** |

So the index-6 rewrite changes the VCO and *every* divider and yet leaves PCLK, AXICLK and CLKEN
bit-for-bit where they were. Its whole net effect is to raise the 060 bus-side clocks from 20 to
**25 MHz**. It is a different **realisation** of the same clock tree, chosen for the bus side —
which is why it is easy to look at the file, see "80 MHz, phases match the defaults", and not
notice that anything has been tuned at all.

Index 14 (120 MHz) also differs from the *shipped template*, but there the card agrees with the
compiled defaults and it is the template that is stale (§1).

**Putting it back.** These are the two blocks as **card A** requires them (see §4.1 before
applying the index-6 one to any other board):

```
index 0
# 50 MHz
    global.multiplier =   4
    global.divider    =   1
    pclk.phase   = -50
    clken.phase  =  50
    bclk.phase   = -40
    cpuclk.phase = 190
    clk90.phase  = 280
    pclk.divider   =  16
    clken.divider  =  32
    bclk.divider   =  32
    cpuclk.divider =  32
    clk90.divider  =  32
    emu_extra_phase =  60
```

```
index 6
# 80 MHz
    global.multiplier =  4
    global.divider    =   1
    pclk.phase   = -40
    clken.phase  =  40
    bclk.phase   =  50
    cpuclk.phase = 180
    clk90.phase  = 270
    pclk.divider   =  10
    clken.divider  =  20
    bclk.divider   =  32
    cpuclk.divider =  32
    clk90.divider  =  32
    emu_extra_phase =  40
```

**What reverts the tuning.** All three are ordinary maintenance actions:

1. **Refreshing the SD from this repo.** `z3660-zturn_SD_content/` is a template tree; copying its
   `timings/default_timings.txt` over the card's is a straight revert (and, at index 14, a
   regression to beta-20 values).
2. **An upstream merge** that updates that template. It does not touch the card by itself, but it
   makes the next SD refresh worse, not better.
3. **Any `write_timings()` caller** — §5.

The tuning exists in exactly one place: that 5 KB file on the card. **Keep a copy off the card**,
and re-check it after any of the three.

### 4.1 What transfers to another card — the index-0 row, and nothing above it

Measured 2026-09-01 on **card B**. Its own `default_timings.txt` is the same 5 163 bytes as card
A's but a different file (sha256 `f6b5683640e8b1eb…`), and the difference is not cosmetic:

* **Index 0 (50 MHz) transferred.** Card B boots AMIX 2.1 to a multiuser login at `cpufreq 50`,
  twice from cold power-on, with the tuned index-0 block above in place. With the *stock* table it
  does not boot at all (§4.2).
* **Index 6 (80 MHz) did not.** Card A's index-6 block was applied to card B **verbatim** and the
  programmed values confirmed on card B's serial — and 80 MHz still fails, as do 70 and 100. The
  phases are right; the board is not.

So §4's index-6 block is **card A's realisation of 80 MHz, not a fix for 80 MHz**. The
bus-side phase margin it encodes belongs to one CPLD/carrier. Re-tuning 80 MHz for a different
board is a fresh phase campaign, not a file copy — and see
[`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) for why the ceiling is a per-board property in the
first place.

### 4.2 What a stock table looks like when it fails

A **fresh SD card carries the stock table** — the shipped template of §1, or whatever
`set_default_timings()` last wrote. On a real 68060 that is not a subtle regression, and it does
not present as a clock problem:

* **No PISCSI activity at all** after the serial prints `060 starting now`. Not a slow disk, not a
  failed mount — the CPU never issues a single SCSI read, because it never gets that far.
* **An Amiga diagnostic colour screen** on the video output: **green = chip RAM error**,
  **blue = custom chip error**.

The CPU is failing **Kickstart's early chipset/RAM diagnostics**, before any disk is touched. With
an A3000 Kickstart the same condition on card B presented instead as a **reset loop** (174 resets
counted). That these colour screens are a Kickstart-init failure and not a CPU or bus failure is
the separate DiagROM datum: on card B, DiagROM — which never runs those diagnostics — boots
cleanly on the real 060 at 80 MHz, the very clock at which both AmigaOS and AMIX die.

**If you see that, restore the card's tuned `default_timings.txt` before debugging anything
else.** The whole failure is upstream of the disk, so time spent on SCSI is wasted.

## 5. `write_timings()` — the file is rewritten from RAM, silently

`write_timings()` (`config_clk.c:347-417`) opens the file named by `timings.txt` with
`FA_CREATE_ALWAYS` and rewrites **all 15 index blocks** from the in-RAM `cd[]`. There is no
prompt, no confirmation and no backup, and any comment or hand formatting the file carried is
gone. If `timings.txt` itself is missing it is recreated pointing at `default_timings.txt`
(`config_clk.c:361-367`), so a card that never had a timings file gets one.

Three callers reach it:

| Caller | Trigger | What lands in the file |
|--------|---------|------------------------|
| `debug_console.c:1038-1041` | **`SCT`** (or `SAVE_CURRENT_TIMINGS`) typed at the running serial console | the live `cd[]` |
| `ARM_ztop/button.c:281` | the ZTop **timings-editor OK button** | the live `cd[]` plus that dialog's edits |
| `config_clk.c:733`, via `mobotest.c:2640` | the mobotest **`G`** key ("Saving default timings") | `set_default_timings()`, i.e. table 2 — **always destructive to a tuned file** |

`SCT` is the sharp one, because whether it is harmless or destructive depends on something the
operator cannot see:

* if `read_timings()` **succeeded** at boot, `cd[]` holds the card's own tuned values and `SCT`
  writes them straight back — close to a no-op;
* if `read_timings()` **failed** — which is silent (§2) — `cd[]` holds the compile-time table 1,
  and `SCT` overwrites the tuned file with it. Table 1 is not even table 2: at index 6 it would
  leave BCLK at 80 MHz.

None of the three is on the boot path, and a failing `read_timings()` never writes anything by
itself. But `SCT` is four characters at a console where nearby words are typed routinely, and the
boot menu reads **one key at a time** — see *Never send a console word to the boot menu* in
[`AMIX.md`](AMIX.md#runtime-debug-toggles-not-config-file-options). Treat `SCT` as a write to the
SD, because that is what it is.

## 6. `emu_extra_phase` is emulator-only

`configure_clk(clk, verbose, emu)` reads `emu_extra_phase` **only when `emu != 0`**:

```c
/* config_clk.c:486-492 */
int emu_div=1, extra_phase=0;
if(emu)
{
   emu_div=2;
   extra_phase=cd[ind].emu_extra_phase;
}
```

and `extra_phase` is then added to exactly two outputs — `clk90.phase` (`config_clk.c:509`) and
`cpuclk.phase` (`config_clk.c:512`). It never touches `pclk`, `clken` or `bclk`.

**With a real 68060 it has no effect on the clocks the CPU runs on.** The final programming
before the 060 is released is `configure_clk(config.cpufreq, 1, 0)` at `main.c:1877` — `emu = 0`,
so `extra_phase` is 0. (An earlier, silent pass at `main.c:1567` does pass `emu = 1`, on every
boot including the real-CPU one, but it is superseded by `main.c:1877` before the 060 starts.)
The emulated boot modes take `configure_clk(config.cpufreq, 1, 1)` at `main.c:1790`, and there
the value is applied.

This was checked on metal. At four clock indices in real-CPU mode — `cpufreq` 80, 70, 60 and 50,
whose `emu_extra_phase` values are 40, 60, 60 and 60 — every printed `CPUCLK` and `CLK90` phase
equalled the file's `cpuclk.phase` / `clk90.phase` exactly, with nothing added. At index 6 that
is `CPUCLK … PHASE 180.00` and `CLK90 … PHASE 270.00` against the file's 180 and 270; had the
emulator path been in force they would have read 220 and 310.

So: tuning `emu_extra_phase` to fix a real-CPU problem changes nothing, and a real-CPU
measurement says nothing about whether the value is right for the emulators.

## 7. Reading back what is actually in force

Don't trust the file, and don't trust `cpufreq` — read the serial log. `configure_clk()` with
`verbose` prints the index it chose and then six lines straight out of the Clocking Wizard
registers (`config_clk.c:523-531`):

```
Clock index 6
AXICLK 160.00 MHz, PHASE 000.00 degrees, DC 050.00%
  PCLK 080.00 MHz, PHASE -40.00 degrees, DC 050.00%
_CLKEN 040.00 MHz, PHASE 040.00 degrees, DC 050.00%
  BCLK 025.00 MHz, PHASE 050.00 degrees, DC 050.00%
CPUCLK 025.00 MHz, PHASE 180.00 degrees, DC 050.00%
 CLK90 025.00 MHz, PHASE 270.00 degrees, DC 050.00%
```

(That is card A at `cpufreq 80` as captured, with only the log's own line timestamps stripped.
It is the whole of §4's index-6 block read back off the silicon: VCO 800 with `pclk.divider 10`
giving PCLK 80.00 and AXICLK 160.00, and the tuned `bclk`/`cpuclk`/`clk90` divider 32 giving
25.00 MHz where the compiled default would give 20.00.)

Those are the **programmed** values, so they are ground truth for both the dividers and the
phases — including whether `emu_extra_phase` was applied. One further gotcha: the index is chosen
by `get_clock_index()` with strict `>` comparisons (`config_clk.c:436-456`), and `configure_clk()`
then replaces the requested frequency with `cd[ind].clk` (`config_clk.c:470`). `cpufreq 80` lands
on index **6**; the configured number is not necessarily the applied one. See also the real-CPU
frequency note in [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md).

## See also

- [`../z3660-zturn_SD_content/DATA Second Partition (exFat)/timings/README.md`](../z3660-zturn_SD_content/DATA%20Second%20Partition%20(exFat)/timings/README.md)
  — upstream's description of the timings files, plus the fork's card-specific warning
- [`AMIX.md`](AMIX.md) — the fork's AMIX reference, including the debug-console hazards
- [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) — clock-configuration limitations
