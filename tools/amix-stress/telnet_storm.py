#!/usr/bin/env python3
# Reproduction driver: storm telnet connections to AMIX inetd so it fork/exec's
# in.telnetd over and over. Each exec demand-pages telnetd's text -- the suspected
# trigger for the wild-PC BUS ERROR. We can't see the guest's console BUS ERROR
# from here, so this just drives the load; count failures as a weak proxy and
# KVM-capture the console afterward for the ground truth.
import socket, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.39"
PORT = 23
N    = int(sys.argv[2]) if len(sys.argv) > 2 else 30
HOLD = float(sys.argv[3]) if len(sys.argv) > 3 else 0.4   # let telnetd start+negotiate

ok = conn_fail = early = 0
for i in range(N):
    try:
        s = socket.create_connection((HOST, PORT), timeout=8)
    except OSError as e:
        conn_fail += 1
        print(f"[{i:03d}] CONNECT-FAIL {e}")
        time.sleep(0.3)
        continue
    s.settimeout(HOLD)
    got = b""
    try:
        end = time.time() + HOLD
        while time.time() < end:
            chunk = s.recv(256)
            if not chunk:
                break
            got += chunk
    except socket.timeout:
        pass
    except OSError:
        early += 1
    s.close()
    if got:
        ok += 1
    else:
        early += 1
    if (i+1) % 10 == 0:
        print(f"  ...{i+1}/{N}  ok={ok} early/empty={early} connfail={conn_fail}")
    time.sleep(0.15)

print(f"DONE storm N={N}: ok={ok} early/empty={early} connfail={conn_fail}")
