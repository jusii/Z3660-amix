#!/usr/bin/env python3
"""ztest_sim.py -- run ztest.rom on a Musashi 68040 (machine68k) against a model A4000.

Offline check of the test program's logic: sizing, the per-path counters, the mailbox CRC and
mirror, the epoch reset, PARK, a restart, and that an injected fault lands on the right path
and data lines. Needs `pip install machine68k` (a Musashi binding; no 68060, so the program
detects "emulated" unless the parameter block forces the real-CPU paths).

The model: chip RAM 2 MB with the Gary overlay (CIA-A PRA bit 0), motherboard RAM blocks 0-3
at $07000000 (blocks 4-15 float), card RAM $08000000-$0FFFFFFF, the image at $F80000, both
8520s (timers, DDRs, PRA; a stopped timer's counter loads one CIA access after the TxHI write, as
measured on the A4000D in METAL-1), Gary's timeout register at $DE0000 (bit 7 set: a read of an empty
address faults; the fault is injected as a 68060 format-4 access-fault frame from the instruction
hook), custom chips as a write sink. machine68k maps only the low 256 MB,
so the Zorro III spaces ($FF000000, $40000000) read as empty: path 4 is NP, path 7 hashes a
constant.

    ztest_sim.py [--rom ztest.rom] [--quiet]     runs the scenario list, exit 0 when all pass
"""
import argparse
import struct
import sys
import zlib

import machine68k as m

PAGE = 0x10000
MB = 0x0FFF0000
ROMBASE = 0x00F80000
PATHS = ('chip', 'mobo', 'card', 'z3', 'rom', 'cia', 'acfg', 'self')


class Board:
    def __init__(self, rom):
        self.rom = rom
        self.pages = {}
        self.cia = {'A': [0] * 16, 'B': [0] * 16}
        self.latch = {'A': [0, 0], 'B': [0, 0]}      # timer A, B latch
        self.count = {'A': [0, 0], 'B': [0, 0]}
        self.pend = {'A': [None, None], 'B': [None, None]}   # counter loads not landed yet
        self.flip = []                                # (lo, hi, xor, every)
        self.reads = 0
        self.gary_to = 0                              # $DE0000 bit 7
        self.gary_toen = 0                            # $DE0001 bit 7
        self.gary_berr = True                         # False: Gary ignores the BERR mode
        self.gary_needs_toen = False                  # True: BERR only with TOENB set as well (the D?)
        self.fault = None                             # address of a faulted read, for the hook

    # ---- storage ----
    def page(self, a):
        p = a & ~(PAGE - 1)
        b = self.pages.get(p)
        if b is None:
            b = self.pages[p] = bytearray(PAGE)
        return b, a & (PAGE - 1)

    def ovl(self):
        ddra, pra = self.cia['A'][2], self.cia['A'][0]
        return not (ddra & 1) or (pra & 1)

    def present(self, a):
        if a < 0x00200000:
            return True
        if 0x07000000 <= a < 0x07400000:
            return True
        if 0x08000000 <= a < 0x10000000:
            return True
        return False

    # ---- byte access ----
    def r8(self, a):
        a &= 0xFFFFFFFF
        if ROMBASE <= a < ROMBASE + len(self.rom):
            return self.rom[a - ROMBASE]
        if a < 0x00080000 and self.ovl():
            return self.rom[a]
        if 0x00BF0000 <= a < 0x00C00000:
            return self.cia_r(a)
        if 0x00DF0000 <= a < 0x00E00000:
            return 0
        if not self.present(a):
            if self.gary_to & 0x80 and self.gary_berr and 0x00F00000 <= a < 0x00F80000 and \
                    (self.gary_toen & 0x80 or not self.gary_needs_toen):
                self.fault = a
            return 0xFF
        b, o = self.page(a)
        return b[o]

    def w8(self, a, v):
        a &= 0xFFFFFFFF
        if ROMBASE <= a < ROMBASE + len(self.rom):
            return
        if 0x00BF0000 <= a < 0x00C00000:
            return self.cia_w(a, v & 0xFF)
        if 0x00DF0000 <= a < 0x00E00000:
            return
        if a == 0x00DE0000:
            self.gary_to = v & 0xFF
            return
        if a == 0x00DE0001:
            self.gary_toen = v & 0xFF
            return
        if not self.present(a):
            return
        b, o = self.page(a)
        b[o] = v & 0xFF

    def r(self, a, n):
        v = 0
        for i in range(n):
            v = (v << 8) | self.r8(a + i)
        self.reads += 1
        for lo, hi, x, every, *ovl in self.flip:
            if ovl and not self.ovl():
                continue
            if lo <= a < hi and self.reads % every == 0:
                v ^= x & ((1 << (8 * n)) - 1)
        return v

    def w(self, a, v, n):
        for i in range(n):
            self.w8(a + i, (v >> (8 * (n - 1 - i))) & 0xFF)

    # ---- 8520s: CIA-A on odd bytes at $BFExxx, CIA-B on even bytes at $BFDxxx ----
    def cia_which(self, a):
        if (a & 0xF000) == 0xE000 and (a & 1):
            return 'A', (a >> 8) & 0xF
        if (a & 0xF000) == 0xD000 and not (a & 1):
            return 'B', (a >> 8) & 0xF
        return None, 0

    def cia_land(self, c):
        for t in (0, 1):
            if self.pend[c][t] is not None:
                self.count[c][t] = self.pend[c][t]
                self.pend[c][t] = None

    def cia_r(self, a):
        c, r = self.cia_which(a)
        if c is None:
            return 0xFF
        v = self.cia_r_now(c, r)
        self.cia_land(c)
        return v

    def cia_r_now(self, c, r):
        if r in (4, 5, 6, 7):
            t = (r - 4) // 2
            v = self.count[c][t]
            return v & 0xFF if r % 2 == 0 else v >> 8
        if r == 0 and c == 'A':
            ddr = self.cia[c][2]
            return (self.cia[c][0] & ddr) | (~ddr & 0xFF)
        return self.cia[c][r]

    def cia_w(self, a, v):
        c, r = self.cia_which(a)
        if c is None:
            return
        self.cia_land(c)
        if r in (4, 5, 6, 7):
            t = (r - 4) // 2
            if r % 2 == 0:
                self.latch[c][t] = (self.latch[c][t] & 0xFF00) | v
            else:
                self.latch[c][t] = (self.latch[c][t] & 0x00FF) | (v << 8)
                cr = self.cia[c][0xE + t]
                if not (cr & 1):                     # stopped: the high byte loads the counter,
                    self.pend[c][t] = self.latch[c][t]   # one CIA access later
            return
        self.cia[c][r] = v

    # ---- host side (the ARM) ----
    def mb32(self, off):
        b, o = self.page(MB + off)
        return struct.unpack('>I', b[o:o + 4])[0]

    def wm(self, off, v):
        b, o = self.page(MB + off)
        b[o:o + 4] = struct.pack('>I', v & 0xFFFFFFFF)

    def mailbox(self, base=0):
        b, o = self.page(MB + base)
        rec = bytes(b[o:o + 0x200])
        ok = rec[:4] == b'ZTMB' and zlib.crc32(rec[:0x1FC]) == struct.unpack('>I', rec[0x1FC:])[0]
        g = lambda x: struct.unpack('>I', rec[x:x + 4])[0]
        paths = []
        for p in range(8):
            r = 0x20 + p * 0x20
            paths.append(dict(err=g(r), ops=g(r + 4), berr=g(r + 8), fa=g(r + 12), fx=g(r + 16), pres=g(r + 24)))
        return dict(crc=ok, hb=g(8), where=g(12), epoch=g(16), st=g(20), rs=g(24), id=g(28),
                    paths=paths, sz=[g(0x120 + 4 * i) for i in range(5)], xcnt=g(0x13C), raw=rec)


def make_machine(board):
    mach = m.Machine(m.CPUType.M68040, 64)
    mem = mach.mem
    mem.set_invalid_func(lambda mode, width, addr: None)   # above 256 MB: empty Zorro III space
    rd = [lambda a: board.r(a, 1), lambda a: board.r(a, 2), lambda a: board.r(a, 4)]
    wr = [lambda a, v: board.w(a, v, 1), lambda a, v: board.w(a, v, 2), lambda a, v: board.w(a, v, 4)]
    for page in range(4096):
        for width in range(3):
            mem.set_special_range_read_func(page << 16, width, rd[width])
            mem.set_special_range_write_func(page << 16, width, wr[width])
    return mach


class Sim:
    def __init__(self, rom, berr=False):
        self.board = Board(rom)
        self.mach = make_machine(self.board)
        if berr:
            self.mach.cpu.set_instr_hook_callback(self.hook)
        self.reset()

    def hook(self, pc):
        """a read that faulted in the previous instruction: take the access fault now"""
        a = self.board.fault
        if a is None:
            return
        self.board.fault = None
        cpu, b = self.mach.cpu, self.board
        sr = cpu.r_sr()
        sp = cpu.r_sp() - 16
        b.w(sp, sr, 2)
        b.w(sp + 2, pc, 4)
        b.w(sp + 6, 0x4000 | (2 << 2), 2)           # format 4, vector 2 (access fault)
        b.w(sp + 8, a, 4)
        b.w(sp + 12, 0x01050020, 4)                 # FSLW as the A4000T read it in T5
        cpu.w_sp(sp)
        cpu.w_sr(sr | 0x2700)
        cpu.w_pc(b.r(MB + 0x400 + 8, 4))            # VBR = MB+$400 (ztest's own table)

    def reset(self):
        # what a CPLD reset does to the board: CIAs reset (overlay back on), RAM kept
        self.board.cia = {'A': [0] * 16, 'B': [0] * 16}
        self.mach.cpu.pulse_reset()

    def run_passes(self, n, limit=4000):
        hb0 = self.board.mailbox()['hb'] if self.board.mailbox()['crc'] else 0
        for _ in range(limit):
            self.mach.execute(200000)
            mb = self.board.mailbox()
            if mb['crc'] and mb['hb'] >= hb0 + n:
                return mb
        return self.board.mailbox()

    def run_cycles(self, c):
        for _ in range(c // 200000):
            self.mach.execute(200000)
        return self.board.mailbox()


def fmt(mb):
    s = 'crc=%s hb=%d where=%08X epoch=%d rs=%d st=%08X xcnt=%d sz=%s' % (
        mb['crc'], mb['hb'], mb['where'], mb['epoch'], mb['rs'], mb['st'], mb['xcnt'],
        '/'.join('%08X' % x for x in mb['sz']))
    for i, p in enumerate(mb['paths']):
        s += '\n   p%d %-5s pres=%d err=%d ops=%d berr=%d fa=%08X fx=%08X' % (
            i + 1, PATHS[i], p['pres'], p['err'], p['ops'], p['berr'], p['fa'], p['fx'])
    return s


def params(board, **kw):
    lay = dict(magic=0x200, epoch=0x204, cmd=0x208, en=0x20C, pres=0x210, mode=0x214, chlo=0x218, chhi=0x21C,
               mobo=0x220, clo=0x224, chi=0x228, z3b=0x22C, z3o=0x230, z3l=0x234, romb=0x238, acfg=0x240,
               chunk=0x244, cacr=0x248, cia=0x24C, flags=0x250)
    for k, v in kw.items():
        board.wm(lay[k], v)
    board.wm(0x200, 0x5A545041)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default='ztest.rom')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    results = []

    def check(name, cond, mb):
        results.append((name, cond))
        print('%s %s' % ('PASS' if cond else 'FAIL', name))
        if not a.quiet or not cond:
            print('   ' + fmt(mb))

    # 1. cold start, no parameter block: sizes, then runs (emulated 040: 3,4,5,7,8 NX)
    s = Sim(rom)
    mb = s.run_passes(4)
    pres = [p['pres'] for p in mb['paths']]
    check('cold start: sized and running, NX map, zero errors',
          mb['crc'] and mb['hb'] >= 4 and pres == [1, 1, 2, 2, 2, 1, 2, 2]
          and all(p['err'] == 0 and p['berr'] == 0 for p in mb['paths'])
          and mb['paths'][0]['ops'] > 0 and mb['paths'][5]['ops'] > 0
          and mb['sz'][1] == 0x000F and (mb['st'] >> 3) & 1 == 1, mb)
    # a host read can tear against the program's copy loop: valid in most samples, never required in all
    good = 0
    for _ in range(40):
        s.mach.execute(37000)
        good += s.board.mailbox(0x800)['crc']
    mir = s.board.mailbox(0x800)
    check('mirror holds a valid published record (%d/40 samples)' % good, good >= 30, mir)

    # 2. parameter block: epoch resets the counters, present mask frozen from sizing
    params(s.board, epoch=7, cmd=0, pres=mb['sz'][0])
    mb = s.run_passes(3)
    check('epoch 7 adopted, counters restarted',
          mb['epoch'] == 7 and mb['paths'][0]['err'] == 0 and mb['paths'][0]['ops'] > 0, mb)

    # 3. a fault on chip RAM reads: path 1 only, bit 7 only
    s.board.flip = [(0x00002000, 0x00200000, 0x00000080, 97)]
    params(s.board, epoch=8)
    mb = s.run_passes(3)
    s.board.flip = []
    ok = mb['paths'][0]['err'] > 0 and mb['paths'][0]['fx'] == 0x80 and \
        all(p['err'] == 0 for i, p in enumerate(mb['paths']) if i != 0)
    check('chip fault lands on path 1, fx=00000080', ok, mb)

    # 4. a fault on the CIA (byte lane): path 6 only
    s.board.flip = [(0x00BF0000, 0x00C00000, 0x04, 5)]
    params(s.board, epoch=9)
    mb = s.run_passes(3)
    s.board.flip = []
    ok = mb['paths'][5]['err'] > 0 and all(p['err'] == 0 for i, p in enumerate(mb['paths']) if i != 5)
    check('CIA fault lands on path 6', ok, mb)

    # 5. PARK: heartbeat stops, where=PARKED; RUN with a new epoch resumes
    params(s.board, cmd=1)
    mb = s.run_cycles(4000000)
    hb = mb['hb']
    mb2 = s.run_cycles(4000000)
    check('PARK: where=FFFF0000, heartbeat frozen', mb['where'] == 0xFFFF0000 and mb2['hb'] == hb, mb2)
    params(s.board, cmd=0, epoch=10)
    mb = s.run_passes(2)
    check('RUN after PARK resumes with epoch 10', mb['hb'] >= hb + 2 and mb['epoch'] == 10, mb)

    # 6. a restart (what TAPPLY RESET does): restarts counted, page kept, params kept
    rs = mb['rs']
    s.reset()
    mb = s.run_passes(2)
    check('restart: rs+1, running again on the same params', mb['rs'] == rs + 1 and mb['epoch'] == 10 and mb['crc'], mb)

    # 7. forced real-CPU paths: card RAM (with MOVE16), socket ROM via the overlay, self image
    s = Sim(rom)
    params(s.board, mode=1, cmd=2, epoch=1)
    mb = s.run_passes(2)
    params(s.board, mode=1, cmd=0, epoch=2, pres=mb['sz'][0])
    mb = s.run_passes(3)
    pres = [p['pres'] for p in mb['paths']]
    ok = pres == [1, 1, 1, 0, 1, 1, 1, 1] and all(p['err'] == 0 and p['berr'] == 0 for p in mb['paths']) \
        and mb['paths'][2]['ops'] > 0 and mb['paths'][4]['ops'] > 0 and mb['paths'][7]['ops'] > 0
    check('real-CPU paths: card/rom/self run clean, Z3 NP without a board', ok, mb)
    check('socket ROM whole sum = FFFFFFFF (ztest is its own socket ROM here)', mb['sz'][2] == 0xFFFFFFFF, mb)

    # 8. faults on card RAM and on the overlay ROM land on paths 3 and 5
    s.board.flip = [(0x08100000, 0x08300000, 0x00010000, 53)]
    params(s.board, epoch=3)
    mb = s.run_passes(3)
    s.board.flip = []
    check('card fault lands on path 3, fx=00010000',
          mb['paths'][2]['err'] > 0 and mb['paths'][2]['fx'] == 0x10000 and
          all(p['err'] == 0 for i, p in enumerate(mb['paths']) if i != 2), mb)
    s.board.flip = [(0x00000000, 0x00080000, 0x01000000, 3, 'ovl')]
    params(s.board, epoch=4)
    mb = s.run_passes(3)
    s.board.flip = []
    check('overlay ROM fault lands on path 5', mb['paths'][4]['err'] > 0 and mb['paths'][0]['err'] == 0, mb)

    # 9. the 8520 counter load lands one access after TxHI: the CIA path must stay clean
    s = Sim(rom)
    mb = s.run_passes(4)
    check('CIA timers with the late 8520 load: path 6 clean', mb['paths'][5]['ops'] > 0 and
          mb['paths'][5]['err'] == 0, mb)

    # 10. Gary BERR probe (flags bit 1, real-CPU mode): one access fault per pass on path 6
    s = Sim(rom, berr=True)
    params(s.board, mode=1, cmd=2, epoch=1)
    mb = s.run_passes(2)
    params(s.board, mode=1, cmd=0, epoch=2, pres=mb['sz'][0], flags=2)
    mb = s.run_passes(2)
    hb, x0 = mb['hb'], mb['xcnt']
    mb = s.run_passes(3)
    p6 = mb['paths'][5]
    check('BERR probe: path 6 berr per pass, no errors, vector 2, running on, Gary back to 0',
          p6['berr'] >= 3 and p6['err'] == 0 and mb['xcnt'] >= x0 + 3 and (mb['st'] >> 16) & 0xFF == 2
          and mb['hb'] >= hb + 3 and s.board.gary_to == 0 and
          all(p['err'] == 0 and p['berr'] == 0 for i, p in enumerate(mb['paths']) if i != 5), mb)

    # 11. the probe when no BERR comes (Gary ignores the mode): counted as an error on path 6
    s.board.gary_berr = False
    params(s.board, epoch=3)
    mb = s.run_passes(3)
    p6 = mb['paths'][5]
    check('BERR probe without a fault: path 6 err at the probe address, berr 0',
          p6['err'] > 0 and p6['berr'] == 0 and p6['fa'] == 0x00F00000, mb)

    # 12. a Gary that needs TOENB too: flags 2 completes the read (error), flags 6 (TIMEOUT + TOENB) faults
    s.board.gary_berr = True
    s.board.gary_needs_toen = True
    params(s.board, epoch=4, flags=2)
    mb = s.run_passes(3)
    p6 = mb['paths'][5]
    ok2 = p6['err'] > 0 and p6['berr'] == 0
    params(s.board, epoch=5, flags=6)
    mb = s.run_passes(3)
    p6 = mb['paths'][5]
    check('TOENB probe: flags 2 no fault on a TOENB-gated Gary, flags 6 faults, both registers back to 0',
          ok2 and p6['berr'] >= 3 and p6['err'] == 0 and s.board.gary_to == 0 and s.board.gary_toen == 0, mb)

    bad = [n for n, c in results if not c]
    print('%d/%d scenarios pass' % (len(results) - len(bad), len(results)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
