#!/usr/bin/env python3
"""zscan.py -- host loop of the Z3660 timing scanner (P-27; PRIMITIVES.md section 3).

Drives a card running a ZT_SCAN firmware and ztest.rom through its debug console, one CPU
mode per run, over one or more timings rows (--rows): the rows are reached by a live frequency
change (TROW + apply at the row's own table values), outward from the boot row; a row whose own
values fail ends that direction. Per row:

  baseline   size the paths at the card's own values, freeze the present mask, require 0 errors
  warm-up    keep the program running at the baseline values (margins shrink warm)
  pass A     CPUCLK with CLK90 locked 90 degrees behind it, outward from the baseline
  pass B     BCLK around the pass-A centre
  options    --diag-clk90 (the CLK90 offset stepped either side of 90, one row), --pclk-pass
             (only meaningful where the card-RAM paths fail first)

Each pass runs coarse steps (10 deg) outward in both directions until two consecutive steps fail,
then refines each edge at 3 deg. The variable is logged as BCLK - CPUCLK in degrees of the bus
clock and in ns. A silent step gets one TAPPLY RESET, then one power cycle, then fails; restart
failures (a power-on that never gets the program running at the baseline) are logged apart.

--switch A,B changes frequency between two rows --switch-n times instead of sweeping (each change
measured like a step; switch-<card>.csv). --soft-first puts a soft reboot (RARM) before a power cycle,
--reset-to-safe ends a twice-silent real-060 step with a RESET at the pass start values instead of a
reboot, --max-cycles caps the power cycles of a run.

Outputs, in --out (the summary line and the timings file are written as each row ends):
  raw-<card>.log          one ZT1 line per step (key=value)
  summary-<card>.csv      one line per row/mode/pass: the community tracker's columns plus the
                          dividers, bus MHz, CPU type/revision, ARM MHz and the window per path
  timings-<card>-<row>-<mode>.txt   the row as a timings block, windows as # comment lines

Transports:
  --dry-run               a simulated card (fake firmware + a timing-window model); no hardware
  --shim <cmd>            the bench: '<cmd> send <line>...' prints one 'ZT ...' reply per line,
                          '<cmd> cycle' power-cycles and waits for the ready line, '<cmd> soft'
                          reboots without a power action
  --shim <cmd> --serve    one long-lived '<cmd> serve': a console line per stdin line, one reply each
"""
import argparse
import csv
import datetime as dt
import os
import random
import select
import shlex
import subprocess
import sys
import time

MB = 0x0FFF0000
PA = MB + 0x200
PATHS = ('chip', 'mobo', 'card', 'z3', 'rom', 'cia', 'acfg', 'self')
CARD_LOCAL = (2, 3, 7)          # 0-based: card, z3, self (PHASES.md section 1)
CMD_RUN, CMD_PARK, CMD_SIZE = 0, 1, 2
OFF = dict(magic=0x00, epoch=0x04, cmd=0x08, en=0x0C, pres=0x10, mode=0x14, chlo=0x18, chhi=0x1C,
           mobo=0x20, clo=0x24, chi=0x28, z3b=0x2C, z3o=0x30, z3l=0x34, romb=0x38, acfg=0x40,
           chunk=0x44, cacr=0x48, cia=0x4C, flags=0x50, bprobe=0x54)


def norm360(p):
    return int(p) % 360


def sdeg(p):
    """degrees into (-180, 180]"""
    p = p % 360
    return p - 360 if p > 180 else p


def kv(line):
    return dict(t.split('=', 1) for t in line.split() if '=' in t)


# --------------------------------------------------------------------------------------
# replies
class Reply:
    def __init__(self, line):
        self.line = line.strip()
        self.words = self.line.split()
        self.kv = kv(self.line)

    @property
    def verb(self):
        return self.words[1] if len(self.words) > 1 else ''

    def has(self, w):
        return w in self.words


def parse_tmb(r):
    """ZT TMB crc=... lwhere=... hb=... p1=err/ops/berr/fa/fx/pres ... sz=a/b/c/d/e"""
    k = r.kv
    out = dict(crc=k.get('crc', 'bad'), lwhere=int(k.get('lwhere', '0'), 16), hb=int(k.get('hb', 0)),
               where=int(k.get('where', '0'), 16), epoch=int(k.get('epoch', 0)), st=int(k.get('st', '0'), 16),
               rs=int(k.get('rs', 0)), id=int(k.get('id', '0'), 16), paths=[])
    for i in range(8):
        f = k.get('p%d' % (i + 1), '0/0/0/0/0/0').split('/')
        out['paths'].append(dict(err=int(f[0]), ops=int(f[1]), berr=int(f[2]), fa=int(f[3], 16),
                                 fx=int(f[4], 16), pres=int(f[5])))
    out['sz'] = [int(x, 16) for x in k.get('sz', '0/0/0/0/0').split('/')]
    out['temp'] = int(k['temp10']) / 10.0 if 'temp10' in k else None     # die temperature (newer firmware)
    out['up'] = int(k['up']) if 'up' in k else None
    return out


def cpu_ident(mb, mode):
    if mode == 'emu':
        return 'UAEJIT040', '-'
    pcr = mb['id']
    ident = pcr >> 16
    rev = (pcr >> 8) & 0xFF
    name = {0x0430: '68060', 0x0431: 'LC060/EC060'}.get(ident, 'PCR%04X' % ident)
    return name, str(rev)


# --------------------------------------------------------------------------------------
# transports
class StopRule(Exception):
    """a core0 abort or an unexpected boot banner in a reply window (METAL stop rule)"""


class ShimConsole:
    """The bench: an external shim does the serial and the power (bench-specific, not in this repo)."""

    def __init__(self, shim, log):
        self.shim = shlex.split(shim)
        self.log = log
        self.now = time.time

    def send(self, *lines):
        p = subprocess.run(self.shim + ['send'] + list(lines), capture_output=True, text=True, timeout=300)
        reps = [Reply(l) for l in p.stdout.splitlines() if l.startswith('ZT ')]
        while len(reps) < len(lines):
            reps.append(Reply('ZT TIMEOUT'))
        return reps

    def verb(self, v, timeout=600):
        p = subprocess.run(self.shim + [v], capture_output=True, text=True, timeout=timeout)
        out = p.stdout.strip().splitlines()
        self.log('shim %s: %s' % (v, out[-1] if out else 'no output'))
        return p.returncode == 0

    def cycle(self):
        return self.verb('cycle')

    def soft(self):
        return self.verb('soft')

    def sleep(self, s):
        time.sleep(s)


class ServeConsole(ShimConsole):
    """One long-lived '<shim> serve': a console line per stdin line, one 'ZT ...' reply per line.
    The shim keeps its own guard on every send; this only saves the per-call start-up."""

    def __init__(self, shim, log):
        ShimConsole.__init__(self, shim, log)
        self.p = None
        self.buf = b''
        self.open()

    def open(self):
        self.p = subprocess.Popen(self.shim + ['serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.buf = b''
        first = self.readline(120)
        if first != 'ZT SERVE ready':
            self.log('serve did not start: %r' % first)
            self.close()

    def readline(self, timeout):
        end = time.time() + timeout
        while b'\n' not in self.buf:
            left = end - time.time()
            if left <= 0 or self.p is None:
                return None
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r:
                return None
            chunk = os.read(self.p.stdout.fileno(), 4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode('ascii', 'replace').strip()

    def send(self, *lines):
        reps = []
        for l in lines:
            if self.p is not None and self.p.poll() is not None:
                self.log('serve exited (rc %s): the shim guard refused or the console went away' % self.p.returncode)
                self.p = None
            if self.p is None:
                reps.append(Reply('ZT NOCONSOLE'))
                continue
            try:
                self.p.stdin.write((l + '\n').encode('ascii'))
                self.p.stdin.flush()
            except BrokenPipeError:
                self.p = None
                reps.append(Reply('ZT NOCONSOLE'))
                continue
            r = self.readline(60) or ('ZT TIMEOUT' if self.p.poll() is None else 'ZT NOCONSOLE')
            reps.append(Reply(r))
            if r in ('ZT ABORT', 'ZT BANNER'):
                raise StopRule('%s after "%s"' % (r, l))
        return reps

    def close(self):
        if self.p is not None:
            try:
                if self.p.poll() is None:
                    self.p.stdin.write(b'QUIT\n')
                    self.p.stdin.flush()
                    self.p.wait(timeout=10)
            except Exception:
                self.p.kill()
            try:
                self.p.stdin.close()
            except Exception:
                pass
        self.p = None

    def cycle(self):
        self.close()
        ok = ShimConsole.cycle(self)
        self.open()
        return ok

    def soft(self):
        self.close()
        ok = ShimConsole.soft(self)
        self.open()
        return ok


# the stock timings table (default_timings.txt 206c251a): mhz, M, D, dividers pclk/clken/bclk/cpuclk/clk90,
# phases, emu_extra_phase -- the dry run's rows
ROWS = [
    (50, 4, 1, [16, 32, 64, 64, 64], [-50, 50, -40, 190, 280], 60),
    (55, 22, 5, [16, 32, 64, 64, 64], [-50, 50, -20, 220, 310], 20),
    (60, 24, 5, [16, 32, 64, 64, 64], [-50, 50, -20, 180, 270], 60),
    (65, 26, 5, [16, 32, 64, 64, 64], [-45, 45, -50, 180, 270], 60),
    (70, 56, 10, [16, 32, 64, 64, 64], [-45, 45, -40, 180, 270], 60),
    (75, 30, 5, [16, 32, 64, 64, 64], [-40, 40, 50, 180, 270], 40),
    (80, 28, 5, [14, 28, 56, 56, 56], [-40, 40, 50, 180, 270], 40),
    (85, 21, 5, [10, 20, 40, 40, 40], [-45, 45, 70, 180, 270], 60),
    (90, 27, 5, [12, 24, 48, 48, 48], [-45, 45, 80, 180, 270], 60),
    (95, 19, 4, [10, 20, 40, 40, 40], [-45, 45, 90, 180, 270], 60),
    (100, 25, 5, [10, 20, 40, 40, 40], [-20, 30, 40, 220, 310], 0),
    (105, 21, 5, [8, 16, 32, 32, 32], [-20, 40, 60, 200, 290], 30),
    (110, 22, 5, [8, 16, 32, 32, 32], [-10, 50, 60, 240, 330], 0),
    (115, 23, 5, [8, 16, 32, 32, 32], [-10, 50, 60, 250, 340], 0),
    (120, 30, 5, [10, 20, 40, 48, 48], [-40, 40, 60, 200, 290], 60),
]


class FakeCard:
    """A simulated card for --dry-run: the zt_scan.c reply formats over a timing-window model.

    Window model (degrees of the bus clock, Delta = bclk - cpuclk): the forwarded paths pass
    inside their own window, error just outside it, and the 060 hangs further out. The CIA
    tolerates more than RAM, chip RAM less than Ramsey RAM. The card-local paths only see
    PCLK. The window drifts with the row; a real 060 hangs at any phase above its ceiling row.
    Restarts fail now and then, as the real 060 does.
    """

    def __init__(self, seed, row, mode, arm_mhz, restart_fail=0.03, ceiling=95, shift=None):
        self.rnd = random.Random(seed)
        self.restart_fail = restart_fail
        self.t = 0.0
        self.mode = mode
        self.arm_mhz = arm_mhz
        self.boot_mhz = row
        self.ceiling = ceiling if mode == 'cpu' else 999
        self.table = {r[0]: dict(idx=i, m=r[1], d=r[2], div=list(r[3]), ph=list(r[4]), xp=r[5])
                      for i, r in enumerate(ROWS)}
        self.off = self.rnd.uniform(-15, 15)               # where the windows sit against the table
        self.shift = shift or {}                           # {mhz: deg}: a row whose table misses its window
        self.pshift = {}                                   # {mhz: deg}: the card-local (PCLK) window off its table
        self.contam_p = 0.0                                # chance a failing mobo step leaves the board faulty
        self.pagelost_p = 0.0                              # chance a RESET comes back cold, page lost
        self.flaky_p = 0.0                                 # chance an apply gives a one-off chip error
        self.flaky_now = False
        self.need_magic = False
        self.contam = False
        self.w = 70 + self.rnd.uniform(-10, 10)
        self.pwin = (-90, 15)
        self.pres = 0xF7 if mode == 'cpu' else 0x23     # no Z3 board; emulated: chip, mobo, cia
        self.params = {}
        self.boot()

    # ---- the board ----
    def boot(self):
        self.rows = {k: dict(v, ph=list(v['ph'])) for k, v in self.table.items()}   # timings file reloaded
        self.cur = self.boot_mhz
        self.applied = (self.cur, list(self.rows[self.cur]['ph']))
        self.snap = None
        self.alive = self.rnd.random() > self.restart_fail   # restart failure now and then
        self.hb = 0
        self.rs = 1
        self.epoch = 0
        self.parked = False
        self.sized = False
        self.cnt = [[0, 0, 0] for _ in range(8)]
        self.last_t = self.t
        self.params = {}
        self.contam = False
        self.flaky_now = False
        return self.alive

    def win(self, mhz):
        t = self.table[mhz]['ph']
        c, w = sdeg(t[2] - t[3]) + self.off + (mhz - 80) * 0.3 + self.shift.get(mhz, 0), \
            self.w * (1.0 - (mhz - 80) * 0.004)
        return {0: (c - w * 0.45, c + w * 0.40), 1: (c - w * 0.55, c + w * 0.50), 4: (c - w * 0.6, c + w * 0.6),
                5: (c - w * 0.9, c + w * 0.8), 6: (c - w * 0.7, c + w * 0.7), 'hang': (c - w * 0.75, c + w * 0.70)}

    def delta(self, ph):
        return sdeg(ph[2] - ph[3])

    def hung(self, mhz, ph):
        if mhz > self.ceiling:
            return True
        lo, hi = self.win(mhz)['hang']
        if not (lo <= self.delta(ph) <= hi):
            return True
        q = self.pshift.get(mhz, 0)
        return not (self.pwin[0] + q - 25 <= sdeg(ph[0]) <= self.pwin[1] + q + 25)

    def path_ok(self, p, mhz, ph):
        if p in CARD_LOCAL:
            q = self.pshift.get(mhz, 0)
            return self.pwin[0] + q <= sdeg(ph[0]) <= self.pwin[1] + q
        lo, hi = self.win(mhz).get(p, (-180, 180))
        return lo <= self.delta(ph) <= hi

    def advance(self):
        dtm = self.t - self.last_t
        self.last_t = self.t
        if not self.alive or self.parked:
            return
        cmd = self.params.get('cmd', CMD_SIZE if not self.sized else CMD_RUN)
        n = int(dtm * 60)
        if n <= 0:
            return
        if cmd == CMD_SIZE or not self.sized:
            self.sized = True
        self.hb += n
        pres = self.params.get('pres') or self.pres
        mhz, ph = self.applied
        for p in range(8):
            if not (pres >> p) & 1 or (self.mode == 'emu' and p in (2, 3, 4, 6, 7)):
                continue
            ops = n * 6000
            self.cnt[p][1] += ops
            bad = not self.path_ok(p, mhz, ph) or (p == 0 and self.flaky_now)
            if bad and p == 1 and self.rnd.random() < self.contam_p:
                self.contam = True
            if bad or (p == 1 and self.contam):
                self.cnt[p][0] += max(1, int(ops * self.rnd.uniform(1e-5, 1e-3)))

    def rowline(self, tag):
        r = self.rows[self.cur]
        ed = 2 if self.mode == 'emu' else 1
        return 'ZT %s idx=%d mhz=%d emu=%d m=%d d=%d div=%d/%d/%d/%d/%d req=%s real=%s' % (
            tag, r['idx'], self.cur, self.mode == 'emu', r['m'], r['d'], r['div'][0] * ed, r['div'][1] * ed,
            r['div'][2], r['div'][3], r['div'][4], '/'.join(map(str, r['ph'])),
            '/'.join('%d.00' % norm360(x) for x in r['ph']))

    # ---- the console ----
    def reply(self, line):
        self.advance()
        w = line.upper().split()
        v = w[0]
        if v == 'TROW':
            mhz = int(w[1])
            if mhz not in self.rows:
                return 'ZT TROW bad'
            if self.snap is None:
                self.snap = {k: list(x['ph']) for k, x in self.rows.items()}
            self.cur = mhz
            return self.rowline('TROW')
        if v == 'TPH':
            if self.snap is None:
                self.snap = {k: list(x['ph']) for k, x in self.rows.items()}
            v5 = [int(x) for x in w[1:6]]
            if any(not -360 <= x <= 360 for x in v5):
                return 'ZT TPH bad'
            self.rows[self.cur]['ph'] = v5
            return self.rowline('TPH')
        if v == 'TAPPLY':
            how = w[1]
            if self.mode == 'emu' and how != 'LIVE':
                return 'ZT TAPPLY refused emu'
            r = self.rows[self.cur]
            xp = r['xp'] if self.mode == 'emu' else 0
            if any(not -360 <= x <= 360 for x in r['ph'][:3] + [r['ph'][3] + xp, r['ph'][4] + xp]):
                return 'ZT TAPPLY refused phase'
            new = (self.cur, list(self.rows[self.cur]['ph']))
            if how == 'PARK':
                if not self.parked:
                    return 'ZT TAPPLY notparked where=%08X' % (self.hb and 0x00060000)
                self.applied = new
                self.alive = not self.hung(*new) and self.rnd.random() > 0.005
            elif how == 'RESET':
                self.applied = new
                self.parked = False
                self.contam = False
                if self.rnd.random() < self.pagelost_p:
                    self.params, self.epoch, self.rs, self.need_magic, self.hb = {}, 0, 0, True, 0
                self.rs += 1
                self.alive = not self.hung(*new) and self.rnd.random() > 0.01
            else:
                self.applied = new
                self.alive = not self.hung(*new)
            self.flaky_now = self.rnd.random() < self.flaky_p
            return 'ZT TAPPLY ok how=%s lock_us=%d' % (how, 27 + self.rnd.randint(0, 1))
        if v == 'WM':
            a = int(w[1], 16)
            for i, x in enumerate(w[2:]):
                off = a + 4 * i - PA
                name = [k for k, o in OFF.items() if o == off]
                if name:
                    self.params[name[0]] = int(x, 16)
            names = [k for i in range(len(w) - 2) for k, o in OFF.items() if o == a + 4 * i - PA]
            if self.need_magic:
                if 'magic' not in names:
                    return 'ZT WM ok n=%d' % (len(w) - 2)       # a cold page: an epoch alone is not adopted
                self.need_magic = False
            if self.alive:
                cmd = self.params.get('cmd', CMD_RUN)
                if self.params.get('epoch', 0) != self.epoch and cmd != CMD_PARK:
                    self.epoch = self.params.get('epoch', 0)
                    self.cnt = [[0, 0, 0] for _ in range(8)]
                self.parked = cmd == CMD_PARK
            return 'ZT WM ok n=%d' % (len(w) - 2)
        if v == 'TMB':
            where = 0xFFFF0000 if self.parked else 0x00070000
            ps = []
            pres = self.params.get('pres') or self.pres
            for p in range(8):
                pr = 1 if (pres >> p) & 1 else 0
                if self.mode == 'emu' and p in (2, 3, 4, 6, 7):
                    pr = 2
                ps.append('p%d=%d/%d/%d/00000000/%08X/%d' % (p + 1, self.cnt[p][0], self.cnt[p][1], 0,
                                                            0x80 if self.cnt[p][0] else 0, pr))
            pcr = 0x04310402 if self.mode == 'cpu' else 0
            return ('ZT TMB crc=%s lwhere=%08X hb=%d where=%08X epoch=%d st=%08X rs=%d id=%08X %s sz=%08X/%08X/FFFFFFFF/0000A48B/DD9D6123'
                    % ('mirror' if self.rnd.random() > 0.1 else 'ok', where, self.hb, where, self.epoch,
                       0x75 if self.mode == 'cpu' else 0x4004D, self.rs, pcr, ' '.join(ps), self.pres, 0xFF00)
                    + ' temp10=%d up=%d' % (450 + min(200, int(self.t / 30)), int(self.t)))
        if v == 'TSTAT':
            r = self.rows[self.cur]
            amhz, aph = self.applied
            a = self.rows[amhz]
            ed = 2 if self.mode == 'emu' else 1
            xp = r['xp'] if self.mode == 'emu' else 0
            return ('ZT TSTAT st68k=0 rr=0 rsti=1 rstc=1 nbr=1 mio15=1 emu=%d mhz=%d idx=%d armmhz=%d m=%d d=%d '
                    'div=%d/%d/%d/%d/%d/%d ph=0/%d/%d/%d/%d/%d rowdiv=%s rowph=%s xp=%d snap=%d live=%d park=0 '
                    'nogrant=0 notparked=0 lockto=0 reset=0 applylive=0 refused=0 lock_us=27'
                    % (self.mode == 'emu', self.cur, r['idx'], self.arm_mhz, r['m'], r['d'],
                       a['div'][0] // 2, a['div'][0] * ed, a['div'][1] * ed, a['div'][2], a['div'][3], a['div'][4],
                       aph[0], aph[1], aph[2], aph[3] + xp, aph[4] + xp, '/'.join(map(str, r['div'])),
                       '/'.join(map(str, r['ph'])), r['xp'], self.snap is not None,
                       self.snap is not None and (self.cur != self.boot_mhz or
                                                  any(self.rows[k]['ph'] != v for k, v in self.snap.items()))))
        if v == 'TREST':
            if self.snap:
                for k, ph in self.snap.items():
                    self.rows[k]['ph'] = list(ph)
            self.cur = self.boot_mhz
            return self.reply('TAPPLY ' + ('LIVE' if self.mode == 'emu' else 'RESET'))
        return 'ZT ? %s' % line


class FakeConsole:
    def __init__(self, card, log, rt=2.0):
        self.card = card
        self.log = log
        self.rt = rt
        self.now = lambda: card.t

    def send(self, *lines):
        out = []
        for l in lines:
            self.card.t += self.rt                  # what one console round trip costs on the bench
            out.append(Reply(self.card.reply(l)))
        return out

    def cycle(self):
        self.card.t += 60.0
        ok = self.card.boot()
        self.log('fake cycle: %s' % ('060 running' if ok else 'restart failure'))
        return True

    def soft(self):
        self.card.t += 25.0
        ok = self.card.boot()
        self.log('fake soft reboot: %s' % ('running' if ok else 'restart failure'))
        return True

    def sleep(self, s):
        self.card.t += s


# --------------------------------------------------------------------------------------
class Scanner:
    def __init__(self, a, con, out):
        self.a = a
        self.con = con
        self.out = out
        self.epoch = 100
        self.rfail = 0
        self.raw = open(os.path.join(out, 'raw-%s.log' % a.card), 'a')
        self.events = open(os.path.join(out, 'events-%s.log' % a.card), 'a')
        self.pres = 0
        self.cpu = ('?', '?')
        self.row = None
        self.lock = '-'         # lock_us of the last TAPPLY
        self.temp = None        # die temperature (C) and firmware uptime (s) of the last TMB
        self.up = None
        self.reboot = 'cycle'   # how the last recovery rebooted (soft / cycle)
        self.contaminated = 0   # failing steps that left the board faulty until a reset
        self.pagelost = 0       # cold restarts with the mailbox page lost, re-armed
        self.flaky = 0          # failing steps that passed on the confirm (--confirm-fail)
        fp = os.path.join(out, 'flaky-%s.csv' % a.card)
        newf = not os.path.exists(fp)
        self.flaky_fh = open(fp, 'a', newline='')
        self.flaky_w = csv.writer(self.flaky_fh)
        if newf:
            self.flaky_w.writerow(['t', 'card', 'mode', 'row', 'pass', 'step', 'phases', 'first_fails',
                                   'first_counts_fx', 'temp_c'])
            self.flaky_fh.flush()
        self.acfg = 0
        rp = os.path.join(out, 'resets-%s.csv' % a.card)
        new = not os.path.exists(rp)
        self.resets_fh = open(rp, 'a', newline='')
        self.resets = csv.writer(self.resets_fh)
        if new:
            self.resets.writerow(['t', 'card', 'cpu', 'rev', 'row', 'pass', 'kind', 'phases', 'lock_us', 'heartbeat_s'])
            self.resets_fh.flush()
        self.nresets = {}
        self.safe = None        # the current pass's start values (known good)
        self.boot = None        # the row the preset boots (where a reboot lands)
        self.cycles = 0         # power cycles this run
        self.softs = 0          # soft reboots (RARM) this run

    def log(self, msg):
        line = '%s %s' % (self.stamp(), msg)
        print(line)
        self.events.write(line + '\n')
        self.events.flush()

    def stamp(self):
        if isinstance(self.con, FakeConsole):
            return 'T+%07.1fs' % self.con.now()
        return dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

    # ---- primitives over the console ----
    def wm(self, **fields):
        words = []
        for k, v in sorted(fields.items(), key=lambda kvp: OFF[kvp[0]]):
            words.append((PA + OFF[k], v))
        # contiguous runs go in one WM line (at most 8 words, the console's 90-character line)
        lines, run = [], []
        for addr, v in words:
            if run and (addr != run[-1][0] + 4 or len(run) == 8):
                lines.append(run)
                run = []
            run.append((addr, v))
        if run:
            lines.append(run)
        reps = self.con.send(*['WM %08X %s' % (r[0][0], ' '.join('%08X' % (x & 0xFFFFFFFF) for _, x in r))
                               for r in lines])
        return all(r.verb == 'WM' and r.has('ok') for r in reps)

    def tmb(self):
        for _ in range(3):
            r = self.con.send('TMB')[0]
            if r.verb == 'TMB' and r.kv.get('crc') in ('ok', 'mirror'):
                return self.note(parse_tmb(r))
        return self.note(parse_tmb(r)) if r.verb == 'TMB' else None

    def note(self, mb):
        if mb.get('temp') is not None:
            self.temp, self.up = mb['temp'], mb['up']
        return mb

    def since_on(self):
        """seconds since the session's power-on (--power-on, UTC ISO time), or '-'"""
        if not self.a.power_on:
            return '-'
        t0 = dt.datetime.fromisoformat(self.a.power_on.replace('Z', '+00:00'))
        return '%d' % (dt.datetime.now(dt.timezone.utc) - t0).total_seconds()

    def tstat(self):
        r = self.con.send('TSTAT')[0]
        return r.kv if r.verb == 'TSTAT' else None

    # ---- the row as the card holds it ----
    def read_row(self):
        k = self.tstat()
        if not k:
            raise SystemExit('TSTAT gave no reply: is this a ZT_SCAN firmware at its debug console?')
        # rowdiv/rowph: cd[] in timings-file units (what TPH stages); div: the wizard registers
        div = [int(x) for x in k['rowdiv'].split('/')]       # pclk/clken/bclk/cpuclk/clk90
        ph = [int(x) for x in k['rowph'].split('/')]
        reg = [int(x) for x in k['div'].split('/')]          # axi/pclk/clken/bclk/cpuclk/clk90
        row = dict(mhz=int(k['mhz']), idx=int(k['idx']), m=int(k['m']), d=int(k['d']), emu=int(k['emu']),
                   armmhz=int(k['armmhz']), div=div, regdiv=reg[1:], ph=ph, xp=int(k['xp']))
        return row

    def check_identity(self, row):
        a = self.a
        if a.row and row['mhz'] != a.row:
            raise SystemExit('card runs row %d, asked for %d (boot the right preset)' % (row['mhz'], a.row))
        if row['emu'] != (a.mode == 'emu'):
            raise SystemExit('card mode emu=%d, asked for %s' % (row['emu'], a.mode))
        if a.arm_mhz and abs(row['armmhz'] - a.arm_mhz) > 15:
            raise SystemExit('ARM at %d MHz, asked for %d (z3660cfg arm_frequency)' % (row['armmhz'], a.arm_mhz))

    def ensure_mode(self):
        """ztest takes its CPU mode at start, from the parameter block when that is valid, and card RAM keeps the
        block over a soft reboot (METAL-2: mode=2 left by an emulated session started ztest emulated on the 060).
        A running program in the other mode gets the mode written and a restart."""
        mb = self.tmb()
        if not mb:
            return
        want = 1 if self.a.mode == 'emu' else 0
        if (mb['st'] >> 3) & 1 == want:
            return
        self.log('ztest runs in the other mode (st %08X): writing mode=%d and restarting it' % (
            mb['st'], 2 if want else 1))
        self.wm(magic=0x5A545041, mode=2 if want else 1)
        if self.a.mode == 'cpu':
            self.apply(self.row['ph'], 'reset')
        else:
            self.softs += 1
            self.con.soft()
        mb = self.tmb()
        if not mb or (mb['st'] >> 3) & 1 != want:
            raise SystemExit('ztest still runs in the other mode after the restart (st %s)' % (
                '%08X' % mb['st'] if mb else 'unread'))

    # ---- baseline ----
    def baseline(self):
        self.epoch += 1
        mode = 2 if self.a.mode == 'emu' else 1
        self.wm(magic=0x5A545041, epoch=self.epoch, cmd=CMD_SIZE, pres=0, mode=mode, en=self.a.en,
                flags=self.a.flags)
        self.con.sleep(3)
        mb = self.tmb()
        if not mb or mb['hb'] == 0:
            return None
        self.pres = mb['sz'][0]
        self.acfg = mb['sz'][4]
        self.cpu = cpu_ident(mb, self.a.mode)
        self.epoch += 1
        self.wm(epoch=self.epoch, cmd=CMD_RUN, pres=self.pres, acfg=self.acfg)
        res = self.measure(self.a.dwell)
        self.log('baseline: present=%02X cpu=%s rev=%s %s' % (self.pres, self.cpu[0], self.cpu[1], res['summary']))
        return res

    def warmup(self, secs):
        end = self.con.now() + secs
        worst = 0
        while self.con.now() < end:
            self.con.sleep(min(30, max(1, end - self.con.now())))
            mb = self.tmb()
            if mb:
                worst = max(worst, sum(p['err'] + p['berr'] for p in mb['paths'] if p['pres'] == 1))
        self.log('warm-up %d s at the baseline: worst error sum %d' % (secs, worst))
        return worst == 0

    # ---- one measurement at whatever is applied ----
    @staticmethod
    def cold(a, b):
        """running (heartbeat rising) at epoch 0: ztest restarted with its page and parameter block lost"""
        return a is not None and b is not None and b['hb'] > a['hb'] and b['epoch'] == 0

    def rearm(self, why):
        """a cold-restarted ztest (page lost, e.g. a runaway 060 scribbled the mailbox before a RESET): the whole
        parameter block again -- magic, mode, SIZE, then RUN with the frozen present mask -- not just the epoch"""
        self.pagelost += 1
        self.log('page-lost restart (%s): re-arming the parameter block' % why)
        self.epoch += 1
        self.wm(magic=0x5A545041, epoch=self.epoch, cmd=CMD_SIZE, pres=0, mode=2 if self.a.mode == 'emu' else 1,
                en=self.a.en, flags=self.a.flags)
        self.con.sleep(3)
        self.tmb()
        self.epoch += 1
        self.wm(epoch=self.epoch, cmd=CMD_RUN, pres=self.pres, acfg=self.acfg)

    def measure(self, dwell, rearmed=False):
        self.con.sleep(dwell)
        a = self.tmb()
        self.con.sleep(1.5)
        b = self.tmb()
        if self.epoch and not rearmed and self.cold(a, b):
            self.rearm('measure')
            return self.measure(dwell, rearmed=True)
        silent = (a is None or b is None or b['hb'] <= a['hb'] or b['epoch'] != self.epoch)
        res = dict(silent=silent, mb=b or a, where=(b or a or {}).get('lwhere', 0))
        fails = []
        if not silent:
            for i, p in enumerate(b['paths']):
                if p['pres'] == 1 and (p['err'] or p['berr']):
                    fails.append(PATHS[i])
        res['fails'] = fails
        res['summary'] = 'silent' if silent else ('pass' if not fails else 'fail-err:' + ','.join(fails))
        return res

    # ---- apply a phase set and measure, with the D7/D13 retry rule ----
    def tph(self, ph):
        """TPH values as 0..359: the console's line input drops '-' (debug_console.c:345)"""
        return 'TPH %s' % ' '.join(str(norm360(x)) for x in ph)

    def unreachable(self, ph):
        """emulated: cpuclk/clk90 + emu_extra_phase above 360 is refused by zt_row_check, and the console cannot
        send the -360 form (its line input drops '-': debug_console.c:345), so those values cannot be applied"""
        if self.a.mode != 'emu' or not self.row:
            return False
        return any(norm360(ph[i]) + self.row['xp'] > 360 for i in (3, 4))

    def apply(self, ph, how):
        if self.unreachable(ph):
            return 'tph-refused', 'unreachable over the console (emulated phase + extra above 360)'
        r = self.con.send(self.tph(ph))[0]
        if r.verb != 'TPH' or r.has('bad'):
            return 'tph-refused', r.line
        real = r.kv.get('real', '')
        if how == 'park':
            self.wm(cmd=CMD_PARK)
            for _ in range(3):
                mb = self.tmb()
                if mb and mb['lwhere'] == 0xFFFF0000:
                    break
                self.con.sleep(0.5)
            r = self.con.send('TAPPLY PARK')[0]
            if not r.has('ok'):
                self.log('TAPPLY PARK: %s -> RESET' % r.line)
                r = self.con.send('TAPPLY RESET')[0]
                how = 'reset'
        elif how == 'reset':
            r = self.con.send('TAPPLY RESET')[0]
        else:
            r = self.con.send('TAPPLY LIVE')[0]
        self.lock = r.kv.get('lock_us', '-')
        if not r.has('ok'):
            return 'apply-failed', r.line
        self.epoch += 1
        self.wm(epoch=self.epoch, cmd=CMD_RUN)
        return how, real

    def restage(self):
        """a reboot lands on the boot row: stage the row being scanned again (TPH/TAPPLY follow)"""
        if self.row and self.boot and self.row['mhz'] != self.boot['mhz']:
            r = self.con.send('TROW %d' % self.row['mhz'])[0]
            self.log('restage after the reboot: %s' % r.line)

    def recover(self):
        """--soft-first: one RARM (PS software reset, no power action) before the power cycles"""
        if self.a.soft_first:
            self.softs += 1
            self.reboot = 'soft'
            self.con.soft()
            res = self.baseline()
            if res and not res['silent']:
                self.restage()
                return True
            self.log('soft reboot did not bring the program back: power cycle next')
        return self.restart_at_baseline()

    def restart_at_baseline(self):
        """Power cycle until the program runs at the card's own values (<= 3), re-baseline."""
        for n in range(3):
            if self.a.max_cycles is not None and self.cycles >= self.a.max_cycles:
                raise SystemExit('power-cycle budget of this run (%d) spent: stopping' % self.a.max_cycles)
            self.cycles += 1
            self.reboot = 'cycle'
            self.con.cycle()
            res = self.baseline()
            if res and not res['silent']:
                self.restage()
                return True
            self.rfail += 1
            self.log('restart failure %d (no heartbeat at the baseline values after a power cycle)' % self.rfail)
        raise SystemExit('three restart failures in a row: stopping (power left as the shim left it)')

    def step(self, pas, off, ph, how0=None):
        how0 = how0 or ('live' if self.a.mode == 'emu' else 'park')
        tries = []
        how, real = self.apply(ph, how0)
        if how in ('tph-refused', 'apply-failed'):
            # the firmware refused the values (range check): nothing was applied, nothing to recover
            v = 'refused'
            self.rawline(pas, off, ph, real, how, 1, dict(silent=False, mb=None, where=0), v)
            self.log('%s %+d: apply refused (%s)' % (pas, off, real))
            return dict(off=off, ph=list(ph), silent=False, fails=['refused'], v=v, pathok=[False] * 8)
        res = self.measure(self.a.dwell)
        tries.append(how)
        if res['silent'] and self.a.mode == 'cpu':
            # D13: one RESET first
            self.timed_reset('d13-same', pas, ph)
            res = self.measure(self.a.dwell)
            tries.append('reset')
            if res['silent'] and self.a.reset_to_safe and self.safe and list(ph) != self.safe:
                # silent twice: RESET back to the pass's start values (known good), then ONE more try of the
                # same step; silent again = the step fails, no power action (a timing failure repeats, a
                # restart failure does not)
                self.reset_safe(pas, off, ph, real, tries, res, kind='d13-safe')
                how, real = self.apply(ph, how0)
                res = self.measure(self.a.dwell) if how not in ('tph-refused', 'apply-failed') else \
                    dict(silent=True, fails=[], mb=None, where=0, summary='silent')
                tries.append('retry+' + how)
                if res['silent']:
                    self.reset_safe(pas, off, ph, real, tries, res, kind='d13-safe')
                    v = 'fail-silent'
                    self.rawline(pas, off, ph, real, '+'.join(tries), len(tries), res, v)
                    return dict(off=off, ph=list(ph), silent=True, fails=[], v=v, pathok=self.path_ok(res))
            if res['silent']:
                # then one power cycle (--soft-first: a soft reboot before it), the same step again
                self.recover()
                how, real = self.apply(ph, how0)
                res = self.measure(self.a.dwell)
                tries.append(self.reboot + '+' + how)
        elif res['silent']:
            self.recover()
            how, real = self.apply(ph, how0)
            res = self.measure(self.a.dwell)
            tries.append(self.reboot + '+' + how)
        v = 'fail-silent' if res['silent'] else res['summary']
        self.rawline(pas, off, ph, real, '+'.join(tries), len(tries), res, v)
        return dict(off=off, ph=list(ph), silent=res['silent'], fails=res['fails'], v=v,
                    pathok=self.path_ok(res), mb=res.get('mb'))

    def clean_check(self, pas, r):
        """after a step that failed with errors: back at the pass start values the program must run clean.
        A failing step can leave the board faulty until an Amiga reset (METAL-2: motherboard RAM erring at the
        boot row's own values after row 90's table step; clean again after a reset), and every later step would
        be measured on that board. Not clean: reset (060: TAPPLY RESET, emulated: soft reboot), re-check."""
        if not self.a.clean_check or not r['fails'] or r['silent'] or not self.safe:
            return
        how0 = 'live' if self.a.mode == 'emu' else 'park'
        real = None
        if not self.a.reset_after_fail:
            how, real = self.apply(self.safe, how0)
            back = self.measure(self.a.dwell) if how not in ('tph-refused', 'apply-failed') else \
                dict(silent=True, fails=[])
            if not back['silent'] and not back['fails']:
                return
            self.contaminated += 1
            self.log('%s: CONTAMINATED -- the start values fail (%s) after a failing step: reset' % (
                pas, 'silent' if back['silent'] else ','.join(back['fails'])))
        else:
            # METAL-2: a board faulted by a failing step can still pass at the start values while failing at
            # weaker phases, so the check cannot see it: reset after every failing step
            back = dict(silent=False, fails=[])
        if self.a.mode == 'cpu':
            self.reset_safe(pas, 0, self.safe, real, [], back, kind='after-fail')
        else:
            self.recover()
            self.apply(self.safe, how0)
            back = self.measure(self.a.dwell)
            if back['fails'] and not back['silent'] and self.a.confirm_fail:
                self.log('the start values fail once after the reboot (%s): one confirm' % ','.join(back['fails']))
                self.apply(self.safe, how0)
                back = self.measure(self.a.dwell)
            if back['silent'] or back['fails']:
                raise SystemExit('STOP: the start values still fail after a reboot (%s)' % (
                    'silent' if back['silent'] else ','.join(back['fails'])))

    def confirm(self, pas, off, ph, r):
        """--confirm-fail: a step that failed with errors is measured once more on a reset board and counts as
        failed only if it fails again ("a timing failure repeats"); a one-off is logged as flaky with the first
        try's counts and fx, so a marginal step (few errors, partial-bit fx) and a faulted board (whole-word fx)
        stay apart"""
        if not self.a.confirm_fail or r['silent'] or not r['fails'] or 'refused' in r['fails']:
            return r
        how0 = None
        if not (self.a.reset_after_fail and self.safe):     # no reset happened yet: make one
            if self.a.mode == 'cpu':
                how0 = 'reset'
            else:
                self.recover()
        r2 = self.step(pas + '-confirm', off, ph, how0=how0)
        if not r2['silent'] and not r2['fails']:
            mb = r.get('mb') or {}
            first = []
            for i, p in enumerate((mb.get('paths') or [])):
                if PATHS[i] in r['fails']:
                    first.append('%s=%d/%d fx=%08X' % (PATHS[i], p['err'], p['ops'], p['fx']))
            self.flaky += 1
            self.log('FLAKY %s %+d at row %d: failed once (%s), passed on the confirm' % (
                pas, off, self.row['mhz'], ' '.join(first) or ','.join(r['fails'])))
            self.flaky_w.writerow([self.stamp(), self.a.card, self.a.mode, self.row['mhz'], pas, off,
                                   '/'.join(str(norm360(x)) for x in ph), ','.join(r['fails']), ' '.join(first),
                                   '%.1f' % self.temp if self.temp is not None else ''])
            self.flaky_fh.flush()
            return r2
        if r2['fails']:
            self.clean_check(pas, r2)
        return r2

    def reset_board(self):
        """a clean board at the boot row: TREST (RESET on the 060) or a soft reboot (emulated)"""
        if self.a.mode == 'cpu':
            r = self.con.send('TREST')[0]
            self.log('reset_board: %s' % r.line)
            self.row = dict(self.boot)
            self.epoch += 1
            self.wm(epoch=self.epoch, cmd=CMD_RUN)
            res = self.measure(self.a.dwell)
            if res['silent'] or res['fails']:
                raise SystemExit('STOP RULE: TREST (RESET at the boot row) did not bring the program back clean (%s)'
                                 % res['summary'])
        else:
            self.row = dict(self.boot)
            self.recover()

    def timed_reset(self, kind, pas, ph):
        """TAPPLY RESET at ph, timed to the first heartbeat of the restarted program (rs + 1, this epoch);
        one line per reset in resets-<card>.csv: the restart-path sample"""
        mb0 = self.tmb()
        rs0 = mb0['rs'] if mb0 else None
        t0 = self.con.now()
        how, real = self.apply(ph, 'reset')
        t_hb = None
        if how not in ('tph-refused', 'apply-failed'):
            prev = None
            while self.con.now() - t0 < 30:
                mb = self.tmb()
                if mb and (rs0 is None or mb['rs'] > rs0) and mb['epoch'] == self.epoch and mb['hb'] > 0:
                    t_hb = self.con.now() - t0
                    break
                if self.cold(prev, mb):
                    t_hb = self.con.now() - t0
                    kind += '+page-lost'
                    self.rearm('after a RESET')
                    break
                prev = mb
                self.con.sleep(0.5)
        self.resets.writerow([self.stamp(), self.a.card, self.cpu[0], self.cpu[1], self.row['mhz'], pas, kind,
                              '/'.join(str(norm360(x)) for x in ph), self.lock,
                              '%.1f' % t_hb if t_hb is not None else 'none'])
        self.resets_fh.flush()
        self.nresets[kind] = self.nresets.get(kind, 0) + 1
        return t_hb

    def reset_safe(self, pas, off, ph, real, tries, res, kind='safe'):
        """RESET at the pass start values; a silent one (30 s) is the METAL stop rule"""
        self.timed_reset(kind, pas, self.safe)
        back = self.measure(self.a.dwell)
        if back['silent']:                      # the METAL-1 rule allows the heartbeat 30 s
            self.con.sleep(20)
            back = self.measure(self.a.dwell)
        tries.append('reset-safe:' + ('ok' if not back['silent'] else 'silent'))
        if not back['silent'] and back['fails']:
            self.log('RESET at the start values runs but fails %s' % back['fails'])
        if back['silent']:
            self.rawline(pas, off, ph, real, '+'.join(tries), len(tries), res, 'fail-silent')
            raise SystemExit('STOP RULE: a RESET at the pass start values %s did not bring the program back'
                             % [norm360(x) for x in self.safe])

    def goto_row(self, mhz):
        """stage a frequency row and apply it at its own table values: a live frequency change"""
        r = self.con.send('TROW %d' % mhz)[0]
        if r.verb != 'TROW' or r.has('bad') or r.has('refused'):
            self.log('TROW %d: %s' % (mhz, r.line))
            return None, None
        row = self.read_row()
        # TSTAT's m/d are the wizard registers (the row applied so far); the staged row's own M/D are in the
        # TROW reply (cd[], as TAPPLY will write them)
        row['m'], row['d'] = int(r.kv['m']), int(r.kv['d'])
        self.row = row
        self.safe = None                    # no known-good values at a new row until its start passes
        res = self.step('row', 0, row['ph'])
        k = self.tstat()                    # the wizard registers only change with the apply
        if k:
            row['regdiv'] = [int(x) for x in k['div'].split('/')][1:]
        return res, row

    def path_ok(self, res):
        ok = []
        mb = res.get('mb')
        for i in range(8):
            if not (self.pres >> i) & 1:
                ok.append(None)
            elif res['silent'] or not mb:
                ok.append(False)
            else:
                p = mb['paths'][i]
                ok.append(p['err'] == 0 and p['berr'] == 0)
        return ok

    def rawline(self, pas, off, ph, real, how, tries, res, v):
        row, a = self.row, self.a
        busmhz = 200.0 * row['m'] / row['d'] / row['div'][2]
        dbc = sdeg(ph[2] - ph[3])
        mb = res.get('mb')
        f = ['ZT1', 't=%s' % self.stamp(), 'card=%s' % a.card, 'box=%s' % a.box, 'fw=%s' % a.fw,
             'prog=%s' % a.prog, 'cpu=%s' % self.cpu[0], 'rev=%s' % self.cpu[1], 'armmhz=%d' % row['armmhz'],
             'row=%d' % row['mhz'], 'idx=%d' % row['idx'], 'mode=%s' % a.mode, 'pass=%s' % pas,
             'step=%+d' % off, 'm=%d' % row['m'], 'd=%d' % row['d'],
             'div=%s' % '/'.join(map(str, row['div'])), 'regdiv=%s' % '/'.join(map(str, row['regdiv'])),
             'xp=%d' % (row['xp'] if a.mode == 'emu' else 0), 'busmhz=%.2f' % busmhz,
             'req=%s' % '/'.join(str(norm360(x)) for x in ph), 'real=%s' % (real or '-'),
             'dbc=%.1f' % dbc, 'dbc_ns=%.2f' % (dbc / 360.0 * 1000.0 / busmhz),
             'c90off=%d' % sdeg(ph[4] - ph[3]), 'how=%s' % how, 'try=%d' % tries, 'lock=%s' % self.lock,
             'sil=%d' % int(res['silent']), 'rfail=%d' % self.rfail, 'epoch=%d' % self.epoch,
             'hb=%d' % (mb['hb'] if mb else -1), 'dwell=%.1f' % a.dwell]
        berr = 0
        fx = 0
        for i, name in enumerate(PATHS):
            if not (self.pres >> i) & 1:
                f.append('%s=%s' % (name, 'NX' if (mb and mb['paths'][i]['pres'] == 2) or
                                    (a.mode == 'emu' and i in (2, 3, 4, 6, 7)) else 'NP'))
                continue
            if not mb or res['silent']:
                f.append('%s=-' % name)
                continue
            p = mb['paths'][i]
            f.append('%s=%d/%d/%d' % (name, p['err'], p['ops'], p['berr']))
            berr += p['berr']
            fx |= p['fx']
        f += ['berr=%d' % berr, 'fx=%08X' % fx, 'where=%08X' % res.get('where', 0),
              'temp=%s' % ('%.1f' % self.temp if self.temp is not None else '-'),
              'up=%s' % (self.up if self.up is not None else '-'), 'since_on=%s' % self.since_on(), 'v=%s' % v]
        self.raw.write(' '.join(f) + '\n')
        self.raw.flush()

    # ---- a sweep: coarse outward, then edges refined ----
    def sweep(self, pas, base, vary, coarse, fine, maxsteps):
        self.safe = list(base)
        results = {0: None}
        r0 = self.step(pas, 0, base)
        results[0] = r0
        if r0['silent'] or r0['fails']:
            self.log('%s: the starting point fails (%s) -- no sweep' % (pas, r0['v']))
            return results
        for sign in (+1, -1):
            bad = 0
            k = 1
            # phase is circular: the upward side keeps going past +180 (across 0/360) while it passes, up to one
            # step short of a full turn; the downward side stops short of what the upward side already covered
            top = max(o for o in results) if sign < 0 else 0
            limit = 360 - coarse if sign > 0 else 360 - top
            while (k * coarse <= limit if sign > 0 else k * coarse < limit) and len(results) < maxsteps + 1:
                off = sign * k * coarse
                r = self.step(pas, off, vary(base, off))
                self.log('%s %+4d: %s' % (pas, off, r['v']))
                self.clean_check(pas, r)
                r = self.confirm(pas, off, vary(base, off), r)
                results[off] = r
                bad = bad + 1 if (r['silent'] or r['fails']) else 0
                if bad >= 2 or (r['silent'] and not self.a.two_silent):
                    break
                k += 1
        # refine: between the last passing and the first failing coarse step on each side
        for sign in (+1, -1):
            offs = sorted((o for o in results if o * sign > 0), key=abs)
            last_ok = 0
            for o in offs:
                r = results[o]
                if r['silent'] or r['fails']:
                    o2 = last_ok + sign * fine
                    while abs(o2) < abs(o):
                        if o2 not in results:
                            r2 = self.step(pas + '-fine', o2, vary(base, o2))
                            self.log('%s-fine %+4d: %s' % (pas, o2, r2['v']))
                            self.clean_check(pas, r2)
                            results[o2] = self.confirm(pas + '-fine', o2, vary(base, o2), r2)
                        o2 += sign * fine
                    break
                last_ok = o
        return results

    # ---- windows from the step results ----
    @staticmethod
    def window(results, pred):
        offs = sorted(results)
        if not pred(results[0]):
            return None
        lo = hi = 0
        lo_fail = hi_fail = None
        for o in sorted((o for o in offs if o > 0)):
            if pred(results[o]):
                hi = o
            else:
                hi_fail = o
                break
        for o in sorted((o for o in offs if o < 0), reverse=True):
            if pred(results[o]):
                lo = o
            else:
                lo_fail = o
                break
        return dict(lo=lo, hi=hi, lo_fail=lo_fail, hi_fail=hi_fail, centre=(lo + hi) / 2.0, margin=(hi - lo) / 2.0)

    def windows(self, results):
        out = {'all': self.window(results, lambda r: not r['silent'] and not r['fails'])}
        for i, name in enumerate(PATHS):
            if not (self.pres >> i) & 1:
                out[name] = 'NX' if self.a.mode == 'emu' and i in (2, 3, 4, 6, 7) else 'NP'
                continue
            out[name] = self.window(results, lambda r, i=i: bool(r['pathok'][i]))
        return out


# --------------------------------------------------------------------------------------
def fmt_win(w):
    if w is None:
        return 'none'
    if isinstance(w, str):
        return w
    e = lambda x: 'open' if x is None else '%+d' % x
    return 'lo=%+d hi=%+d centre=%+.1f margin=%.1f fail_below=%s fail_above=%s' % (
        w['lo'], w['hi'], w['centre'], w['margin'], e(w['lo_fail']), e(w['hi_fail']))


PASSES = ('cpuclk', 'bclk', 'clk90off', 'pclk')
SUMMARY_COLS = ['Machine', 'Kickstart', 'HW Rev', 'CPLD', 'Boot.bin', 'SCSI', 'Auto RAM', 'Emu/CPU', 'CPU Type',
                'MHz', 'global multi', 'global divider', 'clock base', 'pclk phase', 'clken phase', 'bclk phase',
                'cpuclk phase', 'clk90 phase', 'emu extra', 'Remarks',
                'card', 'cpu_rev', 'arm_mhz', 'pclk_div', 'clken_div', 'bclk_div', 'cpuclk_div', 'clk90_div',
                'bus_mhz', 'start_phases', 'dbc_centre', 'dbc_centre_ns'] + \
               ['%s_%s' % (q, x) for q in PASSES for x in ('lo', 'hi', 'centre', 'margin')] + \
               ['%s_%s_%s' % (q, p, x) for q in PASSES for p in PATHS for x in ('lo', 'hi')] + \
               ['temp_start_c', 'temp_end_c']


def keep_form(v):
    """a phase in the row's own convention (180/270 rather than -180/-90)"""
    v = int(round(v))
    while v > 360:
        v -= 360
    while v < -360:
        v += 360
    return v


def summary_row(a, sc, start, final, wins, var_bases):
    row = sc.row
    busmhz = 200.0 * row['m'] / row['d'] / row['div'][2]
    d = dict.fromkeys(SUMMARY_COLS, '')
    dbc = sdeg(final[2] - final[3])
    d.update({'Machine': a.machine, 'Kickstart': a.kickstart, 'HW Rev': a.hwrev, 'CPLD': a.cpld,
              'Boot.bin': a.fw, 'Auto RAM': 'Y', 'Emu/CPU': 'CPU' if a.mode == 'cpu' else 'UAEJIT40',
              'CPU Type': sc.cpu[0], 'MHz': row['mhz'], 'global multi': row['m'], 'global divider': row['d'],
              'clock base': 200 * row['m'] // row['d'],
              'pclk phase': keep_form(final[0]), 'clken phase': keep_form(final[1]), 'bclk phase': keep_form(final[2]),
              'cpuclk phase': keep_form(final[3]), 'clk90 phase': keep_form(final[4]),
              'emu extra': row['xp'] if a.mode == 'emu' else '',
              'Remarks': 'measured window, P-27 scanner (centre of the all-paths window; edges per path)',
              'card': a.card, 'cpu_rev': sc.cpu[1], 'arm_mhz': row['armmhz'],
              'pclk_div': row['div'][0], 'clken_div': row['div'][1], 'bclk_div': row['div'][2],
              'cpuclk_div': row['div'][3], 'clk90_div': row['div'][4], 'bus_mhz': '%.2f' % busmhz,
              'start_phases': '/'.join(str(keep_form(x)) for x in start),
              'dbc_centre': '%.1f' % dbc, 'dbc_centre_ns': '%.2f' % (dbc / 360.0 * 1000.0 / busmhz)})
    for q, win in wins.items():
        vb = var_bases[q]
        w = win['all']
        if isinstance(w, dict):
            d.update({'%s_lo' % q: keep_form(vb + w['lo']), '%s_hi' % q: keep_form(vb + w['hi']),
                      '%s_centre' % q: '%.1f' % (vb + w['centre']), '%s_margin' % q: '%.1f' % w['margin']})
        else:
            d['%s_lo' % q] = d['%s_hi' % q] = 'none'
        for p in PATHS:
            pw = win[p]
            if isinstance(pw, dict):
                d['%s_%s_lo' % (q, p)] = keep_form(vb + pw['lo'])
                d['%s_%s_hi' % (q, p)] = keep_form(vb + pw['hi'])
            else:
                d['%s_%s_lo' % (q, p)] = d['%s_%s_hi' % (q, p)] = pw if pw else 'none'
    return d


def parse_rows(spec):
    """'50-100' (5 MHz steps), '80,50', or a mix"""
    out = []
    for part in spec.split(','):
        part = part.strip()
        if '-' in part:
            lo, hi = part.split('-')
            out += list(range(int(lo), int(hi) + 1, 5))
        elif part:
            out.append(int(part))
    return out


def write_row(a, sc, base, centre, wins, var_bases, remark=None):
    """the summary CSV line and the timings file of one row, written as soon as the row ends"""
    d = summary_row(a, sc, base, centre, wins, var_bases)
    if remark:
        d['Remarks'] = remark
    d['temp_start_c'] = '%.1f' % sc.row_temp0 if getattr(sc, 'row_temp0', None) is not None else ''
    d['temp_end_c'] = '%.1f' % sc.temp if sc.temp is not None else ''
    sp = os.path.join(a.out, 'summary-%s.csv' % a.card)
    new = not os.path.exists(sp)
    if not new:
        with open(sp) as fh:
            old = list(csv.DictReader(fh))
            hdr = old[0].keys() if old else []
        if list(hdr) != SUMMARY_COLS:       # an older file: the new columns are appended, the old lines kept
            with open(sp, 'w', newline='') as fh:
                w = csv.DictWriter(fh, SUMMARY_COLS)
                w.writeheader()
                for x in old:
                    w.writerow(dict((c, x.get(c, '')) for c in SUMMARY_COLS))
    with open(sp, 'a', newline='') as fh:
        w = csv.DictWriter(fh, SUMMARY_COLS)
        if new:
            w.writeheader()
        w.writerow(d)
    row = sc.row
    tp = os.path.join(a.out, 'timings-%s-%d-%s.txt' % (a.card, row['mhz'], a.mode))
    if wins:
        with open(tp, 'w') as fh:
            fh.write('# Z3660 timings row measured by the P-27 scanner -- not for a card until the OS campaign passes (D11)\n')
            fh.write('# card %s box %s cpu %s rev %s ARM %d MHz mode %s; BCLK-CPUCLK at the centre %s deg = %s ns\n' % (
                a.card, a.box, sc.cpu[0], sc.cpu[1], row['armmhz'], a.mode, d['dbc_centre'], d['dbc_centre_ns']))
            for q in wins:
                fh.write('# %s pass: window %s..%s centre %s margin +-%s deg\n' % (
                    q, d['%s_lo' % q], d['%s_hi' % q], d['%s_centre' % q], d['%s_margin' % q]))
                for p in PATHS:
                    fh.write('#    %-5s %s..%s\n' % (p, d['%s_%s_lo' % (q, p)], d['%s_%s_hi' % (q, p)]))
            fh.write('index %d\n# %d MHz\n' % (row['idx'], row['mhz']))
            fh.write('    global.multiplier = %3d\n    global.divider    = %3d\n' % (row['m'], row['d']))
            names = ('pclk', 'clken', 'bclk', 'cpuclk', 'clk90')
            for n, v in zip(names, centre):
                fh.write('    %-12s = %3d\n' % (n + '.phase', keep_form(v)))
            for n, v in zip(names, row['div']):
                fh.write('    %-14s = %3d\n' % (n + '.divider', v))
            if a.mode == 'emu':
                fh.write('    emu_extra_phase = %3d\n' % row['xp'])
    sc.log('row %d written: summary %s%s' % (row['mhz'], sp, (', timings ' + tp) if wins else ' (no window)'))


def scan_row(a, sc, mhz):
    """one frequency row: get there at its table values, then the passes. False = its own values fail."""
    if sc.row and mhz == sc.row['mhz'] and sc.row['ph'] == sc.boot['ph'] and mhz == sc.boot['mhz']:
        row = sc.row
        r0 = sc.step('row', 0, row['ph'])
    else:
        r0, row = sc.goto_row(mhz)
        if r0 is None:
            return None
    base = list(row['ph'])
    if not r0['silent'] and r0['fails']:
        r0 = sc.confirm('row', 0, base, r0)
    sc.row_temp0 = sc.temp
    passes = [p.strip() for p in a.passes.split(',') if p.strip()]
    sc.log('row %d idx %d: M/D %d/%d div %s phases %s ARM %d MHz -> %s' % (
        row['mhz'], row['idx'], row['m'], row['d'], row['div'], base, row['armmhz'], r0['v']))
    if (r0['silent'] or r0['fails']) and a.find_start:
        # the table values are outside this card's window: look along BCLK for a passing point to sweep from
        sc.safe = None
        found = None
        dead = set()                        # a side that went silent: further out only hangs harder
        # only the card-local paths failing = the PCLK/CLKEN pair's business (D16), else BCLK's
        fp = [p for p in r0['fails'] if p in PATHS]
        local = bool(fp) and all(PATHS.index(p) in CARD_LOCAL for p in fp)
        axis = 'pclk' if local else 'bclk'
        for k in range(1, 19):
            for sign in (+1, -1):
                off = sign * 10 * k
                if off == -180 or sign in dead:
                    continue
                if axis == 'pclk':
                    ph = [base[0] + off, base[1] - off, base[2], base[3], base[4]]
                else:
                    ph = [base[0], base[1], base[2] + off, base[3], base[4]]
                r = sc.step('start', off, ph, how0='reset' if a.mode == 'cpu' else None)
                sc.log('row %d start search %s %+d: %s' % (row['mhz'], axis, off, r['v']))
                r = sc.confirm('start', off, ph, r)
                if not r['silent'] and not r['fails']:
                    found = ph
                    break
                if r['silent']:
                    dead.add(sign)
                elif r['fails'] and a.clean_check and a.mode == 'emu':
                    # no known-good values to check against here: the next search step starts from a reset
                    # board (the 060 applies every search step by RESET)
                    sc.recover()
            if found or len(dead) == 2:
                break
        if found:
            k = 0 if axis == 'pclk' else 2
            sc.log('row %d: the table values fail (%s); sweeping from %s %+d instead' % (
                row['mhz'], r0['v'], axis, found[k] - base[k]))
            start_remark = 'table values fail (%s); swept from %s %d' % (r0['v'], axis, keep_form(found[k]))
            r0 = dict(r0, silent=False, fails=[])
            base = found
            if axis == 'pclk' and 'pclk' not in passes:
                passes.append('pclk')       # D16: a pclk pass where the card-local paths fail first
        else:
            start_remark = None
    else:
        start_remark = None
    if r0['silent'] or r0['fails']:
        write_row(a, sc, base, base, {}, {}, remark='P-27 scanner: the table values fail at this row (%s)%s; no sweep' % (
            r0['v'], ' and no BCLK step from -170 to +180 passes' if a.find_start else ''))
        if a.reset_after_fail and not r0['silent']:
            sc.reset_board()
        return False
    centre = list(base)
    wins, var_bases = {}, {}
    for pas in passes:
        if pas == 'cpuclk':
            vary = lambda b, o: [b[0], b[1], b[2], b[3] + o, b[3] + o + 90]
            var_base = centre[3]
        elif pas == 'bclk':
            vary = lambda b, o: [b[0], b[1], b[2] + o, b[3], b[4]]
            var_base = centre[2]
        elif pas == 'clk90off':
            vary = lambda b, o: [b[0], b[1], b[2], b[3], b[3] + 90 + o]
            var_base = 90
        elif pas == 'pclk':
            vary = lambda b, o: [b[0] + o, b[1] - o, b[2], b[3], b[4]]
            var_base = centre[0]
        else:
            raise SystemExit('unknown pass %s' % pas)
        results = sc.sweep(pas, centre, vary, a.coarse, a.fine, a.max_steps)
        win = sc.windows(results)
        sc.log('%s window (offsets from %s): %s' % (pas, var_base, fmt_win(win['all'])))
        for p in PATHS:
            sc.log('   %-5s %s' % (p, fmt_win(win[p])))
        wins[pas] = win
        var_bases[pas] = var_base
        if isinstance(win['all'], dict):
            c = int(round(win['all']['centre']))
            centre = vary(centre, c)
            sc.log('%s centre: offset %+d -> phases %s' % (pas, c, [norm360(x) for x in centre]))
        # D16: the PCLK/CLKEN pass is only worth it where the card-RAM paths fail first
        if pas in ('cpuclk', 'bclk') and isinstance(win['all'], dict):
            for edge in ('lo_fail', 'hi_fail'):
                o = win['all'][edge]
                if o is None or results[o]['silent']:
                    continue
                local = [p for p in results[o]['fails'] if p in PATHS and PATHS.index(p) in CARD_LOCAL]
                if local:
                    sc.log('note: %s fails first at %+d on card-local path(s) %s: a pclk pass applies here (D16)'
                           % (pas, o, local))
    write_row(a, sc, base, centre, wins, var_bases,
              remark=('measured window, P-27 scanner; ' + start_remark) if start_remark else None)
    # leave the row's table values staged (a later TREST or revisit starts from the table)
    sc.con.send(sc.tph(row['ph']))
    return True


def run_switch(a, sc):
    """owner item 5: a live frequency change between two rows, each at its own table values"""
    ra, rb = [int(x) for x in a.switch.split(',')]
    sp = os.path.join(a.out, 'switch-%s.csv' % a.card)
    new = not os.path.exists(sp)
    fh = open(sp, 'a', newline='')
    w = csv.writer(fh)
    if new:
        w.writerow(['t', 'card', 'mode', 'cpu', 'rev', 'arm_mhz', 'n', 'from', 'to', 'how', 'lock_us', 'verdict'])
    if sc.row['mhz'] != ra:
        r0, _ = sc.goto_row(ra)
        if r0 is None or r0['silent'] or r0['fails']:
            raise SystemExit('switch: row %d does not run at its own values (%s)' % (ra, r0 and r0['v']))
    ok = 0
    for i in range(1, a.switch_n + 1):
        frm = sc.row['mhz']
        to = rb if frm == ra else ra
        r, row = sc.goto_row(to)
        v = 'refused' if r is None else r['v']
        ok += v == 'pass'
        w.writerow([sc.stamp(), a.card, a.mode, sc.cpu[0], sc.cpu[1], sc.row['armmhz'], i, frm, to,
                    'live' if a.mode == 'emu' else 'park', sc.lock, v])
        fh.flush()
        sc.log('switch %d: %d -> %d MHz: %s (lock %s us)' % (i, frm, to, v, sc.lock))
    fh.close()
    sc.log('SWITCH %d/%d ok between rows %d and %d (%s)' % (ok, a.switch_n, ra, rb, sp))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--card', required=True, help='card name in the logs (ddev, tdev)')
    ap.add_argument('--box', required=True, help='box name in the logs (a4000d, a4000t)')
    ap.add_argument('--row', type=int, help='the boot row, checked against TSTAT (default: whatever it boots)')
    ap.add_argument('--rows', help="rows to scan: '50-100' (5 MHz steps), '80,50', ... (default: the boot row)")
    ap.add_argument('--keep-going', action='store_true',
                    help='go on past a row whose table values fail (default: that ends the direction)')
    ap.add_argument('--confirm-fail', action='store_true',
                    help='a step failing with errors is measured once more after its reset; failed only if it fails '
                         'again, else logged as flaky (flaky-<card>.csv)')
    ap.add_argument('--reset-after-fail', action='store_true',
                    help='reset after every failing step (060: RESET at the pass start values; emulated: soft reboot)')
    ap.add_argument('--no-clean-check', dest='clean_check', action='store_false',
                    help='skip the return check at the pass start values after a failing step')
    ap.add_argument('--find-start', action='store_true',
                    help="a row whose table values fail: step BCLK outward (10 deg) for a passing point to sweep from")
    ap.add_argument('--power-on', help="the session's power-on time, UTC ISO (from POWER-LOG): raw lines get since_on=")
    ap.add_argument('--switch', help='A,B: no sweep; change frequency between rows A and B --switch-n times')
    ap.add_argument('--switch-n', type=int, default=20)
    ap.add_argument('--mode', choices=('cpu', 'emu'), required=True)
    ap.add_argument('--arm-mhz', type=int, default=1000, help='expected ARM clock; checked against TSTAT')
    ap.add_argument('--fw', default='-', help='firmware CRC (scanner build) for the log')
    ap.add_argument('--prog', default='-', help='ztest.rom CRC for the log')
    ap.add_argument('--machine', default='', help='summary: Machine column')
    ap.add_argument('--kickstart', default='', help='summary: Kickstart column')
    ap.add_argument('--hwrev', default='', help='summary: HW Rev column (card revision)')
    ap.add_argument('--cpld', default='', help='summary: CPLD column')
    ap.add_argument('--out', required=True)
    ap.add_argument('--dwell', type=float, default=5.0, help='seconds the program runs per step')
    ap.add_argument('--warmup', type=int, default=600, help='seconds at the baseline before the passes')
    ap.add_argument('--coarse', type=int, default=10)
    ap.add_argument('--fine', type=int, default=3)
    ap.add_argument('--max-steps', type=int, default=72)
    ap.add_argument('--passes', default='cpuclk,bclk', help='comma list of cpuclk,bclk,clk90off,pclk')
    ap.add_argument('--en', type=lambda x: int(x, 0), default=0, help='ztest path-enable mask (0 = all)')
    ap.add_argument('--flags', type=lambda x: int(x, 0), default=0, help='ztest flags (bit1 = Gary BERR probe)')
    ap.add_argument('--soft-first', action='store_true',
                    help='a silent step that RESET does not cure gets a soft reboot (RARM) before a power cycle')
    ap.add_argument('--reset-to-safe', action='store_true',
                    help='real 060: a step silent after its RESET retry gets a RESET at the pass start values and one '
                         'more try; silent again = the step fails, no reboot (Amix ruling on D13, 2026-10-22)')
    ap.add_argument('--two-silent', action='store_true',
                    help='keep stepping outward after a silent step (default: a silent step ends that side)')
    ap.add_argument('--max-cycles', type=int, help='power cycles this run may spend; past it the run stops')
    ap.add_argument('--dry-run', action='store_true', help='simulated card, no hardware')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--fake-restart-fail', type=float, default=0.03, help='dry run: chance a power-on never starts the 060')
    ap.add_argument('--fake-rt', type=float, default=2.3, help='dry run: seconds per console round trip')
    ap.add_argument('--fake-ceiling', type=int, default=95, help='dry run: the real 060 hangs above this row')
    ap.add_argument('--fake-contam', type=float, default=0.0,
                    help='dry run: chance per pass that a failing mobo step leaves mobo faulty until a reset')
    ap.add_argument('--fake-flaky', type=float, default=0.0, help='dry run: chance an apply gives a one-off error')
    ap.add_argument('--fake-pagelost', type=float, default=0.0, help='dry run: chance a RESET restarts cold, page lost')
    ap.add_argument('--fake-width', type=float, default=0, help='dry run: forwarded window width in degrees')
    ap.add_argument('--fake-pshift', default='', help="dry run: 'mhz:deg,...' rows whose PCLK window sits off their table")
    ap.add_argument('--fake-shift', default='', help="dry run: 'mhz:deg,...' rows whose window sits off their table")
    ap.add_argument('--shim', help="bench shim command ('<shim> send|serve|cycle|soft')")
    ap.add_argument('--serve', action='store_true', help="one persistent '<shim> serve' instead of a call per line")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    if a.dry_run:
        shift = dict((int(k), int(v)) for k, v in (x.split(':') for x in a.fake_shift.split(',') if x))
        card = FakeCard(a.seed, a.row or 80, a.mode, a.arm_mhz, a.fake_restart_fail, a.fake_ceiling, shift)
        card.contam_p = a.fake_contam
        card.pagelost_p = a.fake_pagelost
        card.flaky_p = a.fake_flaky
        if a.fake_width:
            card.w = a.fake_width
        card.pshift = dict((int(k), int(v)) for k, v in (x.split(':') for x in a.fake_pshift.split(',') if x))
        con = FakeConsole(card, lambda m: sc.log(m), a.fake_rt)
    elif a.shim:
        con = (ServeConsole if a.serve else ShimConsole)(a.shim, lambda m: sc.log(m))
    else:
        raise SystemExit('need --dry-run or --shim')
    sc = Scanner(a, con, a.out)
    t0 = con.now()
    sc.log('zscan %s: card=%s box=%s rows=%s switch=%s mode=%s arm=%d passes=%s dwell=%.1f' % (
        'DRY RUN' if a.dry_run else 'BENCH', a.card, a.box, a.rows or a.row, a.switch, a.mode, a.arm_mhz,
        a.passes, a.dwell))
    try:
        row = sc.read_row()
        sc.check_identity(row)
        sc.row = sc.boot = row
        sc.log('boot row %d idx %d: M/D %d/%d div %s phases %s ARM %d MHz' % (
            row['mhz'], row['idx'], row['m'], row['d'], row['div'], row['ph'], row['armmhz']))
        sc.ensure_mode()
        res = sc.baseline()
        if not res or res['silent']:
            sc.restart_at_baseline()
            res = sc.baseline()
        if res['silent'] or res['fails']:
            raise SystemExit('baseline fails (%s): instrument or board fault, no sweep' % res['summary'])
        if a.warmup and not sc.warmup(a.warmup):
            raise SystemExit('errors during the warm-up at the baseline: stop')

        if a.switch:
            run_switch(a, sc)
        else:
            rows = parse_rows(a.rows) if a.rows else [row['mhz']]
            boot = row['mhz']
            for direction in ([r for r in sorted(rows, reverse=True) if r <= boot],
                              [r for r in sorted(rows) if r > boot]):
                for mhz in direction:
                    ok = scan_row(a, sc, mhz)
                    if ok is None:
                        sc.log('row %d: TROW refused, skipped' % mhz)
                    elif not ok and not a.keep_going:
                        sc.log('row %d fails at its table values: the rest of this direction is skipped' % mhz)
                        break
        r = con.send('TREST')[0]
        sc.log('TREST: %s' % r.line)
    except StopRule as e:
        sc.log('STOP RULE: %s -- nothing more is sent; the box is left as it is' % e)
        raise SystemExit(3)
    finally:
        sc.log('run time %.0f s; power cycles %d, soft reboots %d, restart failures %d, contaminated %d, resets %s, '
               'page-lost restarts %d, flaky %d; raw log %s' % (con.now() - t0, sc.cycles, sc.softs, sc.rfail,
                                                               sc.contaminated, sc.nresets or 0, sc.pagelost, sc.flaky,
                                                               sc.raw.name))
        if isinstance(con, ServeConsole):
            con.close()


if __name__ == '__main__':
    main()
