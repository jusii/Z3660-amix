#!/bin/bash
# amix_soak.sh -- laptop-side acceptance soak for the 030-MMU multi-fault-continuation fix.
#
# Each cycle = a heavy fault-path workload (deep-recursion user-stack growth via amixstress
# + fork/exec churn); every 5th cycle = a clean AMIX reboot + boot-to-login. FAIL HARD if the
# wild-PC guard latches (the firmware's "user PC went wild" serial count rises) or a
# "User BUS ERROR" reaches the serial.
#
#   Usage:  ./amix_soak.sh [cycles]            (default 50)
#   Prereq: amixstress installed on the AMIX root FS as /amixstress (survives reboots):
#             amixsync.py push amixstress /amixstress   (then chmod 755 on the box)
#   Needs:  amixsh.py (scripted-telnet runner) + ssh access to the serial-console host.
#
# All bench specifics are env-overridable; defaults are this project's bench.
set -u
LOG="${SOAK_LOG:-/tmp/amix_soak.log}"
KVM_HOST="${KVM_HOST:-kvm1w.alanara.fi}"                              # host whose serial console reads the firmware UART
KVM="ssh -o BatchMode=yes -o ConnectTimeout=8 root@$KVM_HOST"
SERIAL="${SERIAL:-/userdata/media/z3660-serial.log}"                 # firmware UART log on $KVM_HOST (carries [WILD] dumps)
SH="${AMIXSH:-$HOME/Devel/Omat/Amiga/Amix/grimoire-amix/tools/host-net/amixsh.py}"  # scripted-telnet runner
export AMIX_HOST="${AMIX_HOST:-192.168.2.39}" AMIX_PASS="${AMIX_PASS:-pass}" AMIX_PORT="${AMIX_PORT:-23}"
STRESS="${STRESS:-/amixstress}"                                      # persistent path on the AMIX root FS
TARGET=${1:-50}

wildcount(){ $KVM "grep -ac 'user PC went wild' $SERIAL" 2>/dev/null | tr -d '\r'; }
telup(){ timeout 4 bash -c "cat < /dev/null > /dev/tcp/$AMIX_HOST/$AMIX_PORT" 2>/dev/null; }
wait_login(){ local i; for i in $(seq 1 36); do telup && return 0; sleep 10; done; return 1; }

BASEW=$(wildcount)
echo "$(date +%H:%M:%S) SOAK START base_wild=$BASEW target=$TARGET" | tee -a "$LOG"

cyc=0
while [ $cyc -lt $TARGET ]; do
  cyc=$((cyc+1))
  OUT=$(timeout 160 python3 "$SH" \
    "$STRESS 220 6" \
    'i=0; while [ $i -lt 120 ]; do /bin/true; i=`expr $i + 1`; done; echo churn-ok' \
    "$STRESS 140 10" \
    "$STRESS 60 40" 2>&1 | tr -d '\r' | grep -E 'amixstress|churn-ok' | tr '\n' '|')
  W=$(wildcount)
  echo "$(date +%H:%M:%S) cycle $cyc/$TARGET wild=$W :: ${OUT:-<no-output:telnet-throttled?>}" | tee -a "$LOG"
  if [ "$W" != "$BASEW" ]; then echo "$(date +%H:%M:%S) *** FAIL: wild-PC guard latched ($BASEW->$W) ***" | tee -a "$LOG"; exit 2; fi
  if [ $((cyc % 5)) -eq 0 ]; then
    echo "$(date +%H:%M:%S) -- reboot at cycle $cyc --" | tee -a "$LOG"
    timeout 25 python3 "$SH" '/etc/shutdown -y -i6 -g0 >/dev/null 2>&1 &' >/dev/null 2>&1
    sleep 75
    if wait_login; then echo "$(date +%H:%M:%S) boot OK (cycle $cyc)" | tee -a "$LOG"; sleep 15
    else echo "$(date +%H:%M:%S) WARN boot-to-login slow/failed at cycle $cyc" | tee -a "$LOG"; fi
    W2=$(wildcount); if [ "$W2" != "$BASEW" ]; then echo "$(date +%H:%M:%S) *** FAIL on reboot: wild ($BASEW->$W2) ***" | tee -a "$LOG"; exit 2; fi
  fi
done
echo "$(date +%H:%M:%S) SOAK DONE cycles=$cyc wild=$(wildcount) (base $BASEW)" | tee -a "$LOG"
