#!/usr/bin/env python3
"""zosswitch.py -- METAL-3: live CPU-frequency switches under a running AmigaOS on the real 060 (ZT_SCAN firmware).

Each switch is TROW <mhz> then TAPPLY GRANT: the ARM takes the bus from the running 060, re-locks the clock wizard at
the other row's table values and gives the bus back (~1 ms). After it: TSTAT (the wizard registers and the row),
then the OS liveness check (default: ICMP ping of the box, which does not go through amigactld, so it never collides
with a stress run polling amigactld). One CSV line per switch. The run ends back on the first row, by GRANT. It never
sends TREST, TAPPLY RESET or SCT: on the real 060 TREST applies by RESET, which is an Amiga reset under the OS.

  zosswitch.py --shim "bash zt-bench.sh" --card ddev --rows 80,70 -n 20 --alive "ping -c 3 -W 2 <box>" --out DIR
  zosswitch.py --dry-run --rows 80,70 -n 20 --out DIR       (fake card + fake OS)

Stops at the first switch the OS does not survive (no reply to the liveness check within --alive-tries), at a
nogrant, a lock timeout, or a TSTAT that does not show the row: the box is then left as it is, for the session's
own recovery (S:Bench-Halt if the OS still answers, otherwise the hung-OS rules)."""
import argparse
import csv
import datetime as dt
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zscan import ROWS, Reply, ServeConsole, StopRule  # noqa: E402


class FakeOS:
    """dry run: the card answers TROW/TAPPLY GRANT/TSTAT; the OS survives a switch unless told otherwise"""

    def __init__(self, seed, rows, nogrant=0.0, die=0.0):
        self.rnd = random.Random(seed)
        self.mhz = rows[0]
        self.applied = rows[0]
        self.nogrant, self.die = nogrant, die
        self.dead = False
        self.n = dict(grant=0, nogrant=0)

    def send(self, *lines):
        return [Reply(self.reply(l)) for l in lines]

    def reply(self, l):
        w = l.split()
        if w[0] == 'TROW':
            self.mhz = int(w[1])
            r = next(r for r in ROWS if r[0] == self.mhz)
            return 'ZT TROW idx=%d mhz=%d emu=0 m=%d d=%d' % (ROWS.index(r), r[0], r[1], r[2])
        if l == 'TAPPLY GRANT':
            if self.rnd.random() < self.nogrant:
                self.n['nogrant'] += 1
                return 'ZT TAPPLY nogrant'
            self.applied = self.mhz
            self.n['grant'] += 1
            self.dead = self.dead or self.rnd.random() < self.die
            return 'ZT TAPPLY ok how=GRANT lock_us=%d' % (26 + self.rnd.randint(0, 2))
        if l == 'TSTAT':
            r = next(r for r in ROWS if r[0] == self.applied)
            return ('ZT TSTAT st68k=0 emu=0 mhz=%d idx=%d armmhz=999 m=%d d=%d grant=%d nogrant=%d lock_us=27 '
                    'temp10=480 up=600' % (self.mhz, ROWS.index(r), r[1], r[2], self.n['grant'], self.n['nogrant']))
        return 'ZT TIMEOUT'

    def alive(self):
        return not self.dead

    def close(self):
        pass


def stamp():
    return dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--shim', help='bench shim (serve verb), e.g. "bash zt-bench.sh"')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--card', default='ddev')
    ap.add_argument('--rows', default='80,70', help='A,B: start row (the booted one) and the other row')
    ap.add_argument('-n', type=int, default=20, help='switches (an even number ends on row A)')
    ap.add_argument('--dwell', type=float, default=30.0, help='seconds on each row before the next switch')
    ap.add_argument('--alive', help='liveness command, rc 0 = the OS answers (default in a dry run: the fake OS)')
    ap.add_argument('--alive-tries', type=int, default=3, help='tries of --alive, 5 s apart, before a switch is lost')
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--fake-nogrant', type=float, default=0.0)
    ap.add_argument('--fake-die', type=float, default=0.0)
    a = ap.parse_args()
    ra, rb = [int(x) for x in a.rows.split(',')]
    if a.n % 2:
        raise SystemExit('-n must be even, so the run ends on row %d' % ra)
    os.makedirs(a.out, exist_ok=True)
    logf = open(os.path.join(a.out, 'zosswitch-%s.log' % a.card), 'a')

    def log(m):
        line = '%s %s' % (stamp(), m)
        print(line, flush=True)
        logf.write(line + '\n')
        logf.flush()

    if a.dry_run:
        con = FakeOS(a.seed, [ra, rb], a.fake_nogrant, a.fake_die)
        alive = con.alive
        sleep = lambda s: None
    else:
        if not a.shim or not a.alive:
            raise SystemExit('--shim and --alive are required on metal')
        con = ServeConsole(a.shim, log)
        alive = lambda: subprocess.run(a.alive, shell=True, capture_output=True, timeout=60).returncode == 0
        sleep = time.sleep

    def tstat():
        r = con.send('TSTAT')[0]
        return r.kv if r.verb == 'TSTAT' else None

    def alive_check():
        for i in range(a.alive_tries):
            if alive():
                return i + 1
            sleep(5)
        return 0

    sp = os.path.join(a.out, 'switch-os-%s.csv' % a.card)
    new = not os.path.exists(sp)
    fh = open(sp, 'a', newline='')
    w = csv.writer(fh)
    if new:
        w.writerow(['t', 'card', 'n', 'from', 'to', 'reply', 'lock_us', 'tstat_mhz', 'tstat_md', 'alive_try',
                    'temp10', 'verdict'])
    ok, verdict = 0, 'pass'
    try:
        k = tstat()
        if not k or k.get('emu') != '0' or int(k.get('mhz', 0)) != ra:
            raise SystemExit('start: TSTAT %s, expected the real 060 on row %d' % (k, ra))
        if not alive_check():
            raise SystemExit('start: the OS does not answer --alive before any switch')
        log('start: row %d, OS answers; %d switches %d<->%d, dwell %.0f s' % (ra, a.n, ra, rb, a.dwell))
        cur = ra
        for i in range(1, a.n + 1):
            to = rb if cur == ra else ra
            r1 = con.send('TROW %d' % to)[0]
            if r1.verb != 'TROW' or int(r1.kv.get('mhz', 0)) != to:
                verdict = 'trow:%s' % r1.line
                break
            r = con.send('TAPPLY GRANT')[0]
            lock = r.kv.get('lock_us', '-')
            if not r.line.startswith('ZT TAPPLY ok'):
                verdict = r.line.replace('ZT TAPPLY ', '')
                if r.line == 'ZT TAPPLY nogrant':
                    con.send('TROW %d' % cur)      # nothing was applied: stage the running row again
            k = tstat() or {}
            row = next(x for x in ROWS if x[0] == to)
            md_ok = k.get('m') == str(row[1]) and k.get('d') == str(row[2])
            if verdict == 'pass' and not md_ok:
                verdict = 'tstat'
            tries = alive_check() if verdict == 'pass' else 0
            if verdict == 'pass' and not tries:
                verdict = 'os-lost'
            w.writerow([stamp(), a.card, i, cur, to, r.line, lock, k.get('mhz', '-'),
                        '%s/%s' % (k.get('m', '-'), k.get('d', '-')), tries, k.get('temp10', '-'), verdict])
            fh.flush()
            log('switch %d: %d -> %d MHz: %s (lock %s us, OS on try %d)' % (i, cur, to, verdict, lock, tries))
            if verdict != 'pass':
                break
            ok += 1
            cur = to
            sleep(a.dwell)
    except StopRule as e:
        verdict = 'stop-rule: %s' % e
    finally:
        fh.close()
        con.close()
    log('SWITCH-OS %d/%d ok between rows %d and %d, end: %s (%s)' % (ok, a.n, ra, rb, verdict, sp))
    return 0 if ok == a.n else 1


if __name__ == '__main__':
    sys.exit(main())
