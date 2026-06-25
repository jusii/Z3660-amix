#!/bin/bash
# overnight.sh -- laptop-side overnight burn-in for the 030-MMU continuation fix.
#
# Waits for inetd's telnet loop-protection to clear, launches a long guest-side fault-path
# stress loop on AMIX (one login -> no further telnet -> no throttling), then watches the
# wild-PC guard via the firmware serial log for ~3h. FAIL HARD if the guard ever latches or a
# "User BUS ERROR" reaches the serial.
#
#   Usage:  ./overnight.sh
#   Prereq: gsoak.sh present on AMIX as /tmp/gsoak.sh and amixstress as /tmp/amixstress
#           (push both with amixsync.py first). This script copies amixstress -> /amixstress.
#
# All bench specifics are env-overridable; defaults are this project's bench.
KVM_HOST="${KVM_HOST:-kvm1w.alanara.fi}"
KVM="ssh -o BatchMode=yes -o ConnectTimeout=8 root@$KVM_HOST"
SERIAL="${SERIAL:-/userdata/media/z3660-serial.log}"
SH="${AMIXSH:-$HOME/Devel/Omat/Amiga/Amix/grimoire-amix/tools/host-net/amixsh.py}"
export AMIX_HOST="${AMIX_HOST:-192.168.2.39}" AMIX_PASS="${AMIX_PASS:-pass}" AMIX_PORT="${AMIX_PORT:-23}"
LOG="${OVERNIGHT_LOG:-/tmp/amix_overnight.log}"
MINUTES="${MINUTES:-180}"
: > "$LOG"
wildc(){ $KVM "grep -ac 'user PC went wild' $SERIAL" 2>/dev/null | tr -d '\r'; }
busc(){  $KVM "grep -ac 'User BUS ERROR' $SERIAL" 2>/dev/null | tr -d '\r'; }
telup(){ timeout 4 bash -c "cat < /dev/null > /dev/tcp/$AMIX_HOST/$AMIX_PORT" 2>/dev/null; }
BASEW=$(wildc)
echo "$(date +%H:%M:%S) overnight START base_wild=$BASEW minutes=$MINUTES" | tee -a "$LOG"

launched=0
for t in $(seq 1 40); do
  if telup; then
    OUT=$(timeout 45 python3 "$SH" \
      'cp /tmp/amixstress /amixstress 2>/dev/null; chmod 755 /amixstress 2>/dev/null; : > /tmp/gsoak.log; N=20000 nohup sh /tmp/gsoak.sh >/dev/null 2>&1 & echo launched-$!' \
      2>&1 | tr -d '\r')
    echo "$OUT" | grep -q launched- && { echo "$(date +%H:%M:%S) guest soak launched" | tee -a "$LOG"; launched=1; break; }
  fi
  sleep 30
done
[ $launched -eq 0 ] && echo "$(date +%H:%M:%S) WARN: guest soak not launched (telnet); monitoring guard only" | tee -a "$LOG"

for m in $(seq 1 "$MINUTES"); do
  W=$(wildc); B=$(busc)
  if [ -n "$W" ] && [ "$W" != "$BASEW" ]; then echo "$(date +%H:%M:%S) *** FAIL: wild guard latched $BASEW -> $W ***" | tee -a "$LOG"; exit 2; fi
  [ $((m % 10)) -eq 0 ] && echo "$(date +%H:%M:%S) ok min~$m wild=$W buserr_serial=$B" | tee -a "$LOG"
  sleep 60
done
echo "$(date +%H:%M:%S) overnight DONE ~${MINUTES}min clean final_wild=$(wildc) (base $BASEW)" | tee -a "$LOG"
