#!/usr/bin/env python3
"""zscan.py -- host loop of the Z3660 timing scanner (P-27; PRIMITIVES.md section 3).

Drives a card running a ZT_SCAN firmware and ztest.rom through its debug console, one
timings row and CPU mode per run:

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

Outputs, in --out:
  raw-<card>.log          one ZT1 line per step (key=value)
  summary-<card>.csv      one line per row/mode/pass: the community tracker's columns plus the
                          dividers, bus MHz, CPU type/revision, ARM MHz and the window per path
  timings-<card>-<row>-<mode>.txt   the row as a timings block, windows as # comment lines

Transports:
  --dry-run               a simulated card (fake firmware + a timing-window model); no hardware
  --shim <cmd>            the bench: '<cmd> send <line>...' prints one 'ZT ...' reply per line,
                          '<cmd> cycle' power-cycles and waits for '060 starting now'
"""
import argparse
import csv
import datetime as dt
import os
import random
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
           chunk=0x44, cacr=0x48, cia=0x4C, flags=0x50)


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

    def cycle(self):
        p = subprocess.run(self.shim + ['cycle'], capture_output=True, text=True, timeout=600)
        self.log('shim cycle: ' + p.stdout.strip().splitlines()[-1] if p.stdout.strip() else 'shim cycle: no output')
        return p.returncode == 0

    def sleep(self, s):
        time.sleep(s)


class FakeCard:
    """A simulated card for --dry-run: the zt_scan.c reply formats over a timing-window model.

    Window model (degrees of the bus clock, Delta = bclk - cpuclk): the forwarded paths pass
    inside their own window, error just outside it, and the 060 hangs further out. The CIA
    tolerates more than RAM, chip RAM less than Ramsey RAM. The card-local paths only see
    PCLK. Restarts fail now and then, as the real 060 does.
    """

    def __init__(self, seed, row, mode, arm_mhz, restart_fail=0.03):
        self.rnd = random.Random(seed)
        self.restart_fail = restart_fail
        self.idx = min(14, max(0, (row - 46) // 5))       # get_clock_index() for the rows in use
        self.t = 0.0
        self.mode = mode
        self.arm_mhz = arm_mhz
        self.row = row
        base = dict(m=4, d=1, div=[10, 20, 32, 32, 32], ph=[-40, 40, 50, 180, 270], extra=40)
        self.boot_row = dict(base)
        self.boot_row['ph'] = list(base['ph'])
        self.staged = list(base['ph'])
        self.applied = list(base['ph'])
        self.snap = None
        centre = sdeg(50 - 180) + self.rnd.uniform(-15, 15)        # where the forwarded window sits
        w = 70 + self.rnd.uniform(-10, 10)
        self.win = {0: (centre - w * 0.45, centre + w * 0.40), 1: (centre - w * 0.55, centre + w * 0.50),
                    4: (centre - w * 0.6, centre + w * 0.6), 5: (centre - w * 0.9, centre + w * 0.8),
                    6: (centre - w * 0.7, centre + w * 0.7)}
        self.hang = (centre - w * 0.75, centre + w * 0.70)
        self.pwin = (-90, 15)
        self.pres = 0xF7 if mode == 'cpu' else 0x23     # no Z3 board; emulated: chip, mobo, cia
        self.params = {}
        self.boot()

    # ---- the board ----
    def boot(self):
        self.applied = list(self.boot_row['ph'])
        self.staged = list(self.boot_row['ph'])
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
        return self.alive

    def delta(self, ph):
        return sdeg(ph[2] - ph[3])

    def hung(self, ph):
        d = self.delta(ph)
        if not (self.hang[0] <= d <= self.hang[1]):
            return True
        return not (self.pwin[0] - 25 <= sdeg(ph[0]) <= self.pwin[1] + 25)

    def path_ok(self, p, ph):
        if p in CARD_LOCAL:
            return self.pwin[0] <= sdeg(ph[0]) <= self.pwin[1]
        lo, hi = self.win.get(p, (-180, 180))
        return lo <= self.delta(ph) <= hi

    def advance(self):
        dtm = self.t - self.last_t
        self.last_t = self.t
        if not self.alive or self.parked:
            return
        cmd = self.params.get('cmd', CMD_SIZE if not self.sized else CMD_RUN)
        n = int(dtm * 90)
        if n <= 0:
            return
        if cmd == CMD_SIZE or not self.sized:
            self.sized = True
        self.hb += n
        pres = self.params.get('pres') or self.pres
        for p in range(8):
            if not (pres >> p) & 1 or (self.mode == 'emu' and p in (2, 3, 4, 6, 7)):
                continue
            ops = n * 6000
            self.cnt[p][1] += ops
            if not self.path_ok(p, self.applied):
                self.cnt[p][0] += max(1, int(ops * self.rnd.uniform(1e-5, 1e-3)))

    # ---- the console ----
    def reply(self, line):
        self.advance()
        w = line.upper().split()
        v = w[0]
        if v == 'TPH':
            if self.snap is None:
                self.snap = list(self.applied)
            self.staged = [int(x) for x in w[1:6]]
            return 'ZT TPH idx=%d mhz=%d emu=%d m=4 d=1 div=10/20/32/32/32 req=%s real=%s' % (
                self.idx, self.row, self.mode == 'emu', '/'.join(map(str, self.staged)),
                '/'.join('%d.00' % x for x in self.staged))
        if v == 'TAPPLY':
            how = w[1]
            if self.mode == 'emu' and how != 'LIVE':
                return 'ZT TAPPLY refused emu'
            if how == 'PARK':
                if not self.parked:
                    return 'ZT TAPPLY notparked where=%08X' % (self.hb and 0x00060000)
                self.applied = list(self.staged)
                self.alive = not self.hung(self.applied) and self.rnd.random() > 0.005
            elif how == 'RESET':
                self.applied = list(self.staged)
                self.parked = False
                self.rs += 1
                self.alive = not self.hung(self.applied) and self.rnd.random() > 0.05
            else:
                self.applied = list(self.staged)
                self.alive = not self.hung(self.applied)
            return 'ZT TAPPLY ok how=%s lock_us=%d' % (how, 60 + self.rnd.randint(0, 30))
        if v == 'WM':
            a = int(w[1], 16)
            for i, x in enumerate(w[2:]):
                off = a + 4 * i - PA
                name = [k for k, o in OFF.items() if o == off]
                if name:
                    self.params[name[0]] = int(x, 16)
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
            pcr = 0x04310400 if self.mode == 'cpu' else 0
            return ('ZT TMB crc=%s lwhere=%08X hb=%d where=%08X epoch=%d st=%08X rs=%d id=%08X %s sz=%08X/%08X/FFFFFFFF/0000A0F4/7BD94DEF'
                    % ('ok' if self.rnd.random() > 0.05 else 'mirror', where, self.hb, where, self.epoch,
                       0x45 if self.mode == 'cpu' else 0x4D, self.rs, pcr, ' '.join(ps), self.pres, 0x000F))
        if v == 'TSTAT':
            ph = self.applied
            xp = self.boot_row['extra'] if self.mode == 'emu' else 0
            return ('ZT TSTAT st68k=0 rr=0 rsti=1 rstc=1 nbr=1 mio15=1 emu=%d mhz=%d idx=%d armmhz=%d m=4 d=1 '
                    'div=5/%s ph=0/%d/%d/%d/%d/%d rowdiv=10/20/32/32/32 rowph=%s xp=%d snap=%d live=%d park=0 '
                    'nogrant=0 notparked=0 lockto=0 reset=0 applylive=0 refused=0 lock_us=70'
                    % (self.mode == 'emu', self.row, self.idx, self.arm_mhz,
                       '20/40/32/32/32' if self.mode == 'emu' else '10/20/32/32/32',
                       ph[0], ph[1], ph[2], ph[3] + xp, ph[4] + xp, '/'.join(map(str, ph)), self.boot_row['extra'],
                       self.snap is not None, self.applied != self.boot_row['ph']))
        if v == 'TREST':
            self.staged = list(self.boot_row['ph'])
            return self.reply('TAPPLY ' + ('LIVE' if self.mode == 'emu' else 'RESET'))
        return 'ZT ? %s' % line


class FakeConsole:
    def __init__(self, card, log):
        self.card = card
        self.log = log
        self.now = lambda: card.t

    def send(self, *lines):
        out = []
        for l in lines:
            self.card.t += 2.0                      # what one console round trip costs on the bench
            out.append(Reply(self.card.reply(l)))
        return out

    def cycle(self):
        self.card.t += 60.0
        ok = self.card.boot()
        self.log('fake cycle: %s' % ('060 running' if ok else 'restart failure'))
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
                return parse_tmb(r)
        return parse_tmb(r) if r.verb == 'TMB' else None

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
        if row['mhz'] != a.row:
            raise SystemExit('card runs row %d, asked for %d (boot the right preset)' % (row['mhz'], a.row))
        if row['emu'] != (a.mode == 'emu'):
            raise SystemExit('card mode emu=%d, asked for %s' % (row['emu'], a.mode))
        if a.arm_mhz and abs(row['armmhz'] - a.arm_mhz) > 15:
            raise SystemExit('ARM at %d MHz, asked for %d (z3660cfg arm_frequency)' % (row['armmhz'], a.arm_mhz))

    # ---- baseline ----
    def baseline(self):
        self.epoch += 1
        mode = 2 if self.a.mode == 'emu' else 1
        self.wm(magic=0x5A545041, epoch=self.epoch, cmd=CMD_SIZE, pres=0, mode=mode)
        self.con.sleep(3)
        mb = self.tmb()
        if not mb or mb['hb'] == 0:
            return None
        self.pres = mb['sz'][0]
        self.cpu = cpu_ident(mb, self.a.mode)
        self.epoch += 1
        self.wm(epoch=self.epoch, cmd=CMD_RUN, pres=self.pres, acfg=mb['sz'][4])
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
    def measure(self, dwell):
        self.con.sleep(dwell)
        a = self.tmb()
        self.con.sleep(1.5)
        b = self.tmb()
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
    def apply(self, ph, how):
        r = self.con.send('TPH %s' % ' '.join(str(norm360(x)) for x in ph))[0]
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
        if not r.has('ok'):
            return 'apply-failed', r.line
        self.epoch += 1
        self.wm(epoch=self.epoch, cmd=CMD_RUN)
        return how, real

    def restart_at_baseline(self):
        """Power cycle until the program runs at the card's own values (<= 3), re-baseline."""
        for n in range(3):
            self.con.cycle()
            res = self.baseline()
            if res and not res['silent']:
                return True
            self.rfail += 1
            self.log('restart failure %d (no heartbeat at the baseline values after a power cycle)' % self.rfail)
        raise SystemExit('three restart failures in a row: stopping (power left as the shim left it)')

    def step(self, pas, off, ph):
        how0 = 'live' if self.a.mode == 'emu' else 'park'
        tries = []
        how, real = self.apply(ph, how0)
        res = self.measure(self.a.dwell) if how not in ('tph-refused', 'apply-failed') else dict(silent=True, fails=[], mb=None, where=0, summary='silent')
        tries.append(how)
        if res['silent'] and self.a.mode == 'cpu':
            # D13: one RESET first
            how, real = self.apply(ph, 'reset')
            res = self.measure(self.a.dwell)
            tries.append('reset')
            if res['silent']:
                # then one power cycle, the same step again
                self.restart_at_baseline()
                how, real = self.apply(ph, how0)
                res = self.measure(self.a.dwell)
                tries.append('cycle+' + how)
        elif res['silent']:
            self.restart_at_baseline()
            how, real = self.apply(ph, how0)
            res = self.measure(self.a.dwell)
            tries.append('cycle+' + how)
        v = 'fail-silent' if res['silent'] else res['summary']
        self.rawline(pas, off, ph, real, '+'.join(tries), len(tries), res, v)
        return dict(off=off, ph=list(ph), silent=res['silent'], fails=res['fails'], v=v,
                    pathok=self.path_ok(res))

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
             'c90off=%d' % sdeg(ph[4] - ph[3]), 'how=%s' % how, 'try=%d' % tries,
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
        f += ['berr=%d' % berr, 'fx=%08X' % fx, 'where=%08X' % res.get('where', 0), 'v=%s' % v]
        self.raw.write(' '.join(f) + '\n')
        self.raw.flush()

    # ---- a sweep: coarse outward, then edges refined ----
    def sweep(self, pas, base, vary, coarse, fine, maxsteps):
        results = {0: None}
        r0 = self.step(pas, 0, base)
        results[0] = r0
        if r0['silent'] or r0['fails']:
            self.log('%s: the starting point fails (%s) -- no sweep' % (pas, r0['v']))
            return results
        for sign in (+1, -1):
            bad = 0
            k = 1
            # +180 and -180 are one phase: the negative side stops short of it
            while (k * coarse <= 180 if sign > 0 else k * coarse < 180) and len(results) < maxsteps + 1:
                off = sign * k * coarse
                r = self.step(pas, off, vary(base, off))
                results[off] = r
                self.log('%s %+4d: %s' % (pas, off, r['v']))
                bad = bad + 1 if (r['silent'] or r['fails']) else 0
                if bad >= 2:
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
                            results[o2] = self.step(pas + '-fine', o2, vary(base, o2))
                            self.log('%s-fine %+4d: %s' % (pas, o2, results[o2]['v']))
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
               ['%s_%s_%s' % (q, p, x) for q in PASSES for p in PATHS for x in ('lo', 'hi')]


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--card', required=True, help='card name in the logs (ddev, tdev)')
    ap.add_argument('--box', required=True, help='box name in the logs (a4000d, a4000t)')
    ap.add_argument('--row', type=int, required=True, help='timings row (cpufreq of the booted preset)')
    ap.add_argument('--mode', choices=('cpu', 'emu'), required=True)
    ap.add_argument('--arm-mhz', type=int, default=1000, help='expected ARM clock; checked against TSTAT')
    ap.add_argument('--fw', default='-', help='firmware CRC (scanner build) for the log')
    ap.add_argument('--prog', default='-', help='ztest.rom sha256 prefix for the log')
    ap.add_argument('--machine', default='', help='summary: Machine column')
    ap.add_argument('--kickstart', default='', help='summary: Kickstart column')
    ap.add_argument('--hwrev', default='', help='summary: HW Rev column (card revision)')
    ap.add_argument('--cpld', default='', help='summary: CPLD column')
    ap.add_argument('--out', required=True)
    ap.add_argument('--dwell', type=float, default=3.0, help='seconds the program runs per step')
    ap.add_argument('--warmup', type=int, default=600, help='seconds at the baseline before the passes')
    ap.add_argument('--coarse', type=int, default=10)
    ap.add_argument('--fine', type=int, default=3)
    ap.add_argument('--max-steps', type=int, default=36)
    ap.add_argument('--passes', default='cpuclk,bclk', help='comma list of cpuclk,bclk,clk90off,pclk')
    ap.add_argument('--dry-run', action='store_true', help='simulated card, no hardware')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--fake-restart-fail', type=float, default=0.03, help='dry run: chance a power-on never starts the 060')
    ap.add_argument('--shim', help="bench shim command ('<shim> send <line>...', '<shim> cycle')")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    events = []
    if a.dry_run:
        card = FakeCard(a.seed, a.row, a.mode, a.arm_mhz, a.fake_restart_fail)
        con = FakeConsole(card, lambda m: sc.log(m))
    elif a.shim:
        con = ShimConsole(a.shim, lambda m: sc.log(m))
    else:
        raise SystemExit('need --dry-run or --shim')
    sc = Scanner(a, con, a.out)
    sc.log('zscan %s: card=%s box=%s row=%d mode=%s arm=%d passes=%s' % (
        'DRY RUN' if a.dry_run else 'BENCH', a.card, a.box, a.row, a.mode, a.arm_mhz, a.passes))

    row = sc.read_row()
    sc.check_identity(row)
    sc.row = row
    base = list(row['ph'])
    sc.log('row %d idx %d: M/D %d/%d div %s phases %s ARM %d MHz' % (
        row['mhz'], row['idx'], row['m'], row['d'], row['div'], base, row['armmhz']))

    res = sc.baseline()
    if not res or res['silent']:
        sc.restart_at_baseline()
        res = sc.baseline()
    if res['silent'] or res['fails']:
        raise SystemExit('baseline fails (%s): instrument or board fault, no sweep' % res['summary'])
    if a.warmup and not sc.warmup(a.warmup):
        raise SystemExit('errors during the warm-up at the baseline: stop')

    centre = list(base)
    wins, var_bases = {}, {}
    for pas in [p.strip() for p in a.passes.split(',') if p.strip()]:
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
                local = [p for p in results[o]['fails'] if PATHS.index(p) in CARD_LOCAL]
                if local:
                    sc.log('note: %s fails first at %+d on card-local path(s) %s: a pclk pass applies here (D16)'
                           % (pas, o, local))

    # back to the card's own values
    r = con.send('TREST')[0]
    sc.log('TREST: %s' % r.line)

    d = summary_row(a, sc, base, centre, wins, var_bases)
    sp = os.path.join(a.out, 'summary-%s.csv' % a.card)
    new = not os.path.exists(sp)
    with open(sp, 'a', newline='') as fh:
        w = csv.DictWriter(fh, SUMMARY_COLS)
        if new:
            w.writeheader()
        w.writerow(d)

    tp = os.path.join(a.out, 'timings-%s-%d-%s.txt' % (a.card, row['mhz'], a.mode))
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
    sc.log('raw log %s, summary %s, timings %s; restart failures %d' % (
        sc.raw.name, sp, tp, sc.rfail))


if __name__ == '__main__':
    main()
