#!/usr/bin/env python3
"""Plan WARP ENGINE, M1a -- the dhrystone opcode corpus behind docs/WARP-ENGINE.md Sec 0.12.

WHY THIS EXISTS. Round 3 measured a decline rate of 72.26 % and a block length of
1.581 compiled guest instructions, and it did NOT record WHICH opcodes the engine
refused -- there is no decline histogram in the R3 evidence directory. So round 4's
pre-registered decline-rate drop cannot be read off a counter; it has to be DERIVED
from the workload's own opcode mix, and this is the derivation, committed with the
pre-registration so it can be re-run by whoever doubts the number.

WHAT IT READS. The AMIX dhrystone binary that every WARP row has been measured on
(sum -r 52076, 8135 bytes), disassembled by an m68k objdump. It is the verdict
workload's user-mode text and nothing else.

THE HONEST LIMITS, stated here rather than in a footnote:

  1. STATIC, NOT DYNAMIC. This counts instructions in the image, not executions.
     Dhrystone's hot set is small and well known, so the script reports the hot
     functions separately from whole-.text -- but neither is an execution profile.
  2. LIBC IS ABSENT. The binary is dynamically linked against /usr/lib/libc.so.1,
     so strcpy/strcmp -- which Dhrystone leans on -- are not in this corpus at all.
  3. JUMP TABLES DISASSEMBLE AS INSTRUCTIONS. Proc_6 carries a switch table inside
     .text; objdump decodes its words as opcodes. --skip-data drops the ranges the
     script can identify by their `.short`/orphan-operand signature, and the count
     of dropped words is reported rather than hidden.

THE MODEL IS NOT TRUSTED. accept() below is a hand transcription of
z3660_warp.cpp:warp_emit_insn()'s accept predicate. The round-4 harness dumps the
ENGINE's own accept set over all 65536 opcodes and a gate requires the two to agree
opcode-for-opcode -- so a transcription error is a test failure, not a silent wrong
prediction. Run:

    python3 docs/warp-corpus.py --dis <objdump-output>
    python3 docs/warp-corpus.py --bitmap 3 > /tmp/model3.txt   (for the gate)
"""

import argparse
import collections
import re
import sys

# ---------------------------------------------------------------- EA modes ----
EA_DREG, EA_AREG, EA_IND, EA_PI, EA_PD, EA_D16 = 0, 1, 2, 3, 4, 5
W_B, W_W, W_L = 0, 1, 2


def _ea_ok(mode, sz):
    """warp_ea_ok(): the four modes generation one was built with."""
    if mode == EA_DREG:
        return True
    if mode == EA_AREG:
        return sz != W_B          # An is never a byte operand
    if mode == EA_IND:
        return True
    if mode == EA_D16:
        return True
    return False


def accept(op, gen):
    """(accepted, tag) for one opcode word under generation `gen`.

    gen == 3 is the shipping round-3 engine. gen == 4 is round 4: generation
    one's own frozen scope completed (TST, the shift/rotate register forms) plus
    M1a tranche 1 (MOVEQ, the remaining register-destination ALU shapes, and
    (An)+ / -(An) SOURCES with a register destination).
    """
    # ------------------------------------------------------- MOVE / MOVEA ----
    if (op & 0xC000) == 0x0000 and (op & 0x3000) != 0x0000:
        szf = (op >> 12) & 3
        sz = W_B if szf == 1 else (W_W if szf == 3 else W_L)
        smode = (op >> 3) & 7
        dmode = (op >> 6) & 7
        if gen >= 4 and smode in (EA_PI, EA_PD):
            # ONE FAULTABLE SITE ONLY. A memory destination behind an
            # auto-adjusting source has TWO, with a register side effect
            # between them, and a bail at the second one cannot be restarted.
            if dmode not in (EA_DREG, EA_AREG):
                return (False, 'MOVE-auto-memdst')
            if not _ea_ok(dmode, sz):
                return (False, 'MOVE-ea')
            if sz == W_B and dmode == EA_AREG:
                return (False, 'MOVE-b-An')
            return (True, 'MOVE-auto')
        if not _ea_ok(smode, sz) or not _ea_ok(dmode, sz):
            return (False, 'MOVE-ea')
        if sz == W_B and (smode == EA_AREG or dmode == EA_AREG):
            return (False, 'MOVE-b-An')
        return (True, 'MOVE')

    # ------------------------------------------------------------- MOVEQ ----
    if (op & 0xF100) == 0x7000:
        return ((gen >= 4), 'MOVEQ')

    # --------------------------------------------------------- ADDQ/SUBQ ----
    if (op & 0xF000) == 0x5000 and ((op >> 6) & 3) != 3:
        mode = (op >> 3) & 7
        sz = (op >> 6) & 3
        if mode == EA_AREG:
            return ((sz != W_B), 'ADDQ-An')
        if mode == EA_DREG:
            return (True, 'ADDQ-Dn')
        return (False, 'ADDQ-mem')

    # -------------------------------------------------------------- DBcc ----
    if (op & 0xF0F8) == 0x50C8:
        return (True, 'DBcc')

    # ------------------------------------------------------- Scc / TRAPcc ----
    if (op & 0xF0C0) == 0x50C0:
        return (False, 'Scc')

    # ---------------------------------------------------------- Bcc / BRA ----
    if (op & 0xF000) == 0x6000:
        cond = (op >> 8) & 0xF
        d8 = op & 0xFF
        if cond == 1:
            return (False, 'BSR')
        if d8 == 0xFF:
            return (False, 'Bcc-32')
        if d8 != 0 and (d8 & 1):
            return (False, 'Bcc-odd')
        return (True, 'Bcc')

    # ----------------------------------------------------------- TST/TAS ----
    if (op & 0xFF00) == 0x4A00:
        sz = (op >> 6) & 3
        mode = (op >> 3) & 7
        if sz == 3:
            return (False, 'TAS')
        if gen >= 4 and mode == EA_DREG:
            return (True, 'TST')
        if gen >= 4 and mode == EA_AREG and sz != W_B:
            return (True, 'TST')
        return (False, 'TST-ea')

    # ------------------------------------------------- shifts and rotates ----
    if (op & 0xF000) == 0xE000:
        sz = (op >> 6) & 3
        if sz == 3:
            return (False, 'shift-mem')          # the memory word form
        typ = (op >> 3) & 3                      # 0 AS 1 LS 2 ROX 3 RO
        if typ == 2:
            return (False, 'ROXd')               # needs X in AND out: not built
        ir = (op >> 5) & 1
        if gen >= 4:
            return (True, 'shift-imm' if ir == 0 else 'shift-reg')
        return (False, 'shift')

    # ---------------------- immediate ALU: ADDI/SUBI/ANDI/ORI/EORI/CMPI ------
    if (op & 0xF000) == 0x0000:
        top = (op >> 9) & 7
        sz = (op >> 6) & 3
        mode = (op >> 3) & 7
        # BIT 8 SEPARATES TWO ENCODINGS THAT OTHERWISE ALIAS, and getting it
        # wrong silently accepts a family. `BTST Dn,<ea>` is 0000 rrr 1 tt eeeeee,
        # so its `rrr` decodes as an immediate-ALU selector and its `1tt` decodes
        # as a size of 0..2. Every immediate-ALU op has bit 8 clear; every
        # dynamic bit op has it set.
        if (op >> 8) & 1:
            return (False, 'bitop-dyn')
        if sz == 3:
            return (False, 'bit/CMP2/CAS')
        if top == 4:
            return (False, 'BTSTimm')
        if top == 7:
            return (False, 'MOVES')
        if mode != EA_DREG:
            return (False, 'IMM-ea')
        return ((gen >= 4), 'IMM')

    # ------------------------------------------------------- register ALU ----
    fam = (op >> 12) & 0xF
    opm = (op >> 6) & 7
    mode = (op >> 3) & 7
    if fam not in (0x8, 0x9, 0xB, 0xC, 0xD):
        return (False, 'other')

    if opm in (3, 7):                            # ADDA / SUBA / CMPA
        if fam in (0x8, 0xC):
            return (False, 'no-ANDA/ORA')
        if mode in (EA_DREG, EA_AREG):
            return (True, 'xxxA-reg')
        if gen >= 4 and mode in (EA_IND, EA_D16):
            return (True, 'xxxA-mem')
        if gen >= 4 and mode in (EA_PI, EA_PD):
            return (True, 'xxxA-auto')
        return (False, 'xxxA-ea')

    sz = opm & 3
    if sz == 3:
        return (False, 'alu-sz3')
    if opm < 4:                                  # <ea>,Dn
        if fam == 0x8 and mode == EA_AREG:
            return (False, 'OR-An')
        if fam == 0xC and mode == EA_AREG:
            return (False, 'AND-An')
        if mode == EA_DREG:
            return (True, 'ALU-reg')
        if mode == EA_AREG and fam in (0x9, 0xB, 0xD) and sz != W_B:
            return (True, 'ALU-reg')
        if gen >= 4 and mode in (EA_IND, EA_D16):
            return (True, 'ALU-mem')
        if gen >= 4 and mode in (EA_PI, EA_PD):
            return (True, 'ALU-auto')
        return (False, 'ALU-ea')

    # Dn,<ea>: modes 0 and 1 are ADDX/SUBX/CMPM, so only EOR Dn,Dn survives.
    if fam == 0xB and mode == EA_DREG:
        return (True, 'EOR')
    return (False, 'ALU-memdst')


# ------------------------------------------------------------ disassembly ----

LINE = re.compile(r'^\s*([0-9a-f]{6,8}):\s+((?:[0-9a-f]{4}\s+)+)\s*(\S.*)?$')
SYM = re.compile(r'^([0-9a-f]{6,8})\s+<([^>]+)>:')

# Dhrystone's own hot set: the loop body plus every Proc_/Func_ it calls.
#
# THE TWO TIERS ARE NOT DECORATION. `main` holds both the measured loop AND a long
# printf banner that runs exactly once; on a 632-instruction corpus that banner is
# 102 `pea <abs.L>` argument pushes, which is 16 % of the image and ~0 % of the
# executions. Reporting main separately from the Proc_/Func_ callees is the closest
# a static count can honestly get to the executed mix, and the gap between the two
# tiers is itself the measure of how much the static reading can be trusted.
HOT = ('main', 'Proc_1', 'Proc_2', 'Proc_3', 'Proc_4', 'Proc_5', 'Proc_6',
       'Proc_7', 'Proc_8', 'Func_1', 'Func_2', 'Func_3')
CALLEES = tuple(f for f in HOT if f != 'main')


def parse(path):
    """-> [(addr, opword, func, text)], plus the count of words judged to be data."""
    out = []
    data = 0
    func = '?'
    for raw in open(path):
        m = SYM.match(raw)
        if m:
            func = m.group(2)
            continue
        m = LINE.match(raw)
        if not m:
            continue
        text = (m.group(3) or '').strip()
        # A line with no mnemonic is objdump's continuation of the PREVIOUS
        # instruction's extension words -- not an opcode. So is `.short`.
        if not text or text.startswith('.short'):
            data += 1
            continue
        words = m.group(2).split()
        out.append((int(m.group(1), 16), int(words[0], 16), func, text))
    return out, data


def mnemonic(text):
    return text.split()[0] if text else '?'


def report(path):
    insns, data = parse(path)
    print("corpus            : %s" % path)
    print("decoded lines     : %d instruction, %d data/continuation" % (len(insns), data))
    print()
    for label, sel in (("WHOLE .text", lambda f: True),
                       ("DHRYSTONE HOT SET (main + callees)", lambda f: f in HOT),
                       ("Proc_/Func_ CALLEES ONLY", lambda f: f in CALLEES)):
        rows = [r for r in insns if sel(r[2])]
        if not rows:
            continue
        n = len(rows)
        a3 = sum(1 for r in rows if accept(r[1], 3)[0])
        a4 = sum(1 for r in rows if accept(r[1], 4)[0])
        print("=== %s : %d instructions ===" % (label, n))
        print("  accepted by R3 : %5d  %6.2f %%   declined %6.2f %%"
              % (a3, 100.0 * a3 / n, 100.0 * (n - a3) / n))
        print("  accepted by R4 : %5d  %6.2f %%   declined %6.2f %%"
              % (a4, 100.0 * a4 / n, 100.0 * (n - a4) / n))
        print("  newly accepted : %5d  %6.2f pp of the corpus"
              % (a4 - a3, 100.0 * (a4 - a3) / n))
        gained = collections.Counter()
        refused = collections.Counter()
        for _, op, _f, text in rows:
            ok3, _ = accept(op, 3)
            ok4, tag4 = accept(op, 4)
            if ok4 and not ok3:
                gained[(tag4, mnemonic(text))] += 1
            elif not ok4:
                refused[(tag4, mnemonic(text))] += 1
        print("  --- what R4 gains ---")
        for (tag, mn), c in gained.most_common(14):
            print("      %-16s %-10s %5d" % (tag, mn, c))
        print("  --- what R4 still refuses ---")
        for (tag, mn), c in refused.most_common(14):
            print("      %-16s %-10s %5d" % (tag, mn, c))
        print()

    # Run length between refusals: the block-length lever, on this corpus.
    for label, sel in (("WHOLE .text", lambda f: True),
                       ("DHRYSTONE HOT SET (main + callees)", lambda f: f in HOT),
                       ("Proc_/Func_ CALLEES ONLY", lambda f: f in CALLEES)):
        rows = [r for r in insns if sel(r[2])]
        if not rows:
            continue
        print("=== %s : mean accepted RUN length (a proxy for block length) ===" % label)
        for gen in (3, 4):
            runs, cur = [], 0
            for _, op, _f, _t in rows:
                if accept(op, gen)[0]:
                    cur += 1
                else:
                    if cur:
                        runs.append(cur)
                    cur = 0
            if cur:
                runs.append(cur)
            mean = (sum(runs) / float(len(runs))) if runs else 0.0
            print("  gen %d : %d runs, mean %.3f, max %d" %
                  (gen, len(runs), mean, max(runs) if runs else 0))
        print()


def bitmap(gen):
    for op in range(65536):
        sys.stdout.write("1" if accept(op, gen)[0] else "0")
        if (op & 63) == 63:
            sys.stdout.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dis', help='objdump -d output of the corpus')
    ap.add_argument('--bitmap', type=int, help='dump the model accept bitmap for a generation')
    a = ap.parse_args()
    if a.bitmap:
        bitmap(a.bitmap)
        return
    n3 = sum(1 for o in range(65536) if accept(o, 3)[0])
    n4 = sum(1 for o in range(65536) if accept(o, 4)[0])
    print("model accept over all 65536 opcode words: R3 %d, R4 %d (+%d)"
          % (n3, n4, n4 - n3))
    print()
    if a.dis:
        report(a.dis)


if __name__ == '__main__':
    main()
