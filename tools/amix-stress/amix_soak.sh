#!/bin/bash
# amix_soak.sh -- laptop-side acceptance soak for the 030-MMU multi-fault-continuation fix.
#
# Each cycle = a heavy fault-path workload (deep-recursion user-stack growth via amixstress
# + fork/exec churn); every $REBOOT_EVERY'th cycle = a clean AMIX reboot that is PROVEN,
# not assumed. FAIL HARD if the wild-PC guard latches (the firmware's "user PC went wild"
# serial count rises), or if a reboot cycle cannot be proven.
#
#   Usage:  ./amix_soak.sh [cycles]            (default 50)
#   Prereq: amixstress installed on the AMIX root FS as /amixstress (survives reboots):
#             amixsync.py push amixstress /amixstress   (then chmod 755 on the box)
#   Needs:  amixsh.py (scripted-telnet runner) + ssh access to the serial-console host.
#   Exit:   0 = soak completed   2 = wild-PC guard latched   3 = a reboot could not be proven
#
# ---------------------------------------------------------------------------------------
# REBOOT PROOF -- why this is more than "the box answers again" (fixed 2026-08-27)
#
# Two independent defects, both present since this script's first commit, made every
# reboot cycle a no-op that still scored green:
#
#   1. THE SHUTDOWN NEVER RAN. amixsh.py sends each command as `<cmd>; echo DONE_$?` so it
#      can spot a completion marker. A command ending in `&` therefore arrives at the box as
#          /etc/shutdown -y -i6 -g0 >/dev/null 2>&1 &; echo DONE_$?
#      and `&;` is a SYNTAX ERROR in the Bourne shell. The cure is to parenthesise the
#      background job: `( ... & )` ends the command in `)`, which takes the trailing `;`
#      perfectly well, and the job is still detached. (Same cure, same day, in
#      amix-kerntools' tools/amixsoak.sh, which hit this on its own launch path.)
#
#   2. THE CHECK COULD NOT SEE THAT. The post-reboot test was a bare TCP connect to port 23.
#      A box that never rebooted answers that INSTANTLY -- so the script logged "boot OK"
#      for a reboot that had not happened. Reachability can never prove a reboot; it is
#      equally consistent with "came back" and "never left".
#
# So a reboot now has to clear two independent gates:
#   * telnet must go DOWN within $DOWN_WAIT s of the shutdown (the box really stopped), and
#   * where the box will produce one, the SVR4 `who -b` boot stamp must CHANGE across the
#     cycle. That second gate catches the case down-detection alone would misread -- telnet
#     bouncing for a reason other than a reboot, e.g. the inetd nowait throttle.
# Either gate failing is a hard FAIL (exit 3), not a warning. If the box cannot produce a
# boot stamp at all the run continues on down-detection alone, but says so in the log rather
# than silently claiming corroboration it does not have.
#
# The SOAK DONE line reports how many reboots were PROVEN. That count -- not a remembered
# one -- is what may be quoted in docs/AMIX.md.
# ---------------------------------------------------------------------------------------
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
REBOOT_EVERY="${REBOOT_EVERY:-5}"                                    # reboot every Nth cycle (0 = never reboot)
DOWN_WAIT="${DOWN_WAIT:-120}"                                        # s the box gets to STOP answering telnet after the shutdown
UP_WAIT="${UP_WAIT:-420}"                                            # s it then gets to come back to a listening telnet port
TARGET=${1:-50}

say(){ echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; }

wildcount(){ $KVM "grep -ac 'user PC went wild' $SERIAL" 2>/dev/null | tr -d '\r'; }
telup(){ timeout 4 bash -c "cat < /dev/null > /dev/tcp/$AMIX_HOST/$AMIX_PORT" 2>/dev/null; }

# Wait for telnet to STOP answering -- the box actually going down. 0 = it went down.
# Two consecutive misses are required so a single dropped probe is not read as a reboot.
wait_down(){
  local end=$(( $(date +%s) + DOWN_WAIT ))
  while [ "$(date +%s)" -lt "$end" ]; do
    if ! telup; then sleep 3; telup || return 0; fi
    sleep 3
  done
  return 1
}

# Wait for telnet to answer again. 0 = back up.
wait_login(){
  local end=$(( $(date +%s) + UP_WAIT ))
  while [ "$(date +%s)" -lt "$end" ]; do telup && return 0; sleep 10; done
  return 1
}

# SVR4 boot stamp -- `who -b` prints e.g. "   .       system boot  Aug 27 10:15".
# Empty output means the box would not give us one (command absent, login refused); the
# caller then degrades to down-detection only instead of inventing a verdict.
bootstamp(){
  timeout 40 python3 "$SH" 'who -b' 2>/dev/null | tr -d '\r' \
    | grep -i 'boot' | grep -v 'who -b' | head -1 | tr -s ' ' | sed 's/^ //;s/ *$//'
}

BASEW=$(wildcount)
REBOOTS=0
say "SOAK START base_wild=$BASEW target=$TARGET reboot_every=$REBOOT_EVERY"

cyc=0
while [ $cyc -lt $TARGET ]; do
  cyc=$((cyc+1))
  OUT=$(timeout 160 python3 "$SH" \
    "$STRESS 220 6" \
    'i=0; while [ $i -lt 120 ]; do /bin/true; i=`expr $i + 1`; done; echo churn-ok' \
    "$STRESS 140 10" \
    "$STRESS 60 40" 2>&1 | tr -d '\r' | grep -E 'amixstress|churn-ok' | tr '\n' '|')
  W=$(wildcount)
  say "cycle $cyc/$TARGET wild=$W :: ${OUT:-<no-output:telnet-throttled?>}"
  if [ "$W" != "$BASEW" ]; then say "*** FAIL: wild-PC guard latched ($BASEW->$W) ***"; exit 2; fi

  if [ "$REBOOT_EVERY" -gt 0 ] && [ $((cyc % REBOOT_EVERY)) -eq 0 ]; then
    say "-- reboot at cycle $cyc --"
    B1=$(bootstamp)
    [ -n "$B1" ] || say "NOTE pre-reboot boot stamp unavailable ('who -b' gave nothing) -- down-detection will be the only proof this cycle"

    # The PARENTHESES ARE LOAD-BEARING -- see REBOOT PROOF in the header. Without them the
    # runner's "; echo DONE_$?" turns this into `... & ; echo ...`, a Bourne syntax error,
    # and the box never shuts down.
    timeout 25 python3 "$SH" '(/etc/shutdown -y -i6 -g0 >/dev/null 2>&1 &)' >/dev/null 2>&1

    if ! wait_down; then
      say "*** FAIL: box still answering telnet ${DOWN_WAIT}s after the shutdown -- it never rebooted (cycle $cyc) ***"
      exit 3
    fi
    say "box went down (cycle $cyc)"

    if ! wait_login; then
      say "*** FAIL: no telnet ${UP_WAIT}s after the box went down -- boot-to-login failed (cycle $cyc) ***"
      exit 3
    fi
    sleep 15
    B2=$(bootstamp)

    if [ -n "$B1" ] && [ -n "$B2" ]; then
      if [ "$B1" = "$B2" ]; then
        say "*** FAIL: boot stamp unchanged across the cycle ('$B2') -- telnet bounced but the box never restarted (cycle $cyc) ***"
        exit 3
      fi
      say "reboot PROVEN (cycle $cyc): down+up, boot stamp '$B1' -> '$B2'"
    else
      say "reboot proven by down+up only (cycle $cyc) -- no boot stamp to corroborate with"
    fi
    REBOOTS=$((REBOOTS+1))

    W2=$(wildcount); if [ "$W2" != "$BASEW" ]; then say "*** FAIL on reboot: wild ($BASEW->$W2) ***"; exit 2; fi
  fi
done
say "SOAK DONE cycles=$cyc reboots_proven=$REBOOTS wild=$(wildcount) (base $BASEW)"
