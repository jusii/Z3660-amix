#!/bin/sh
# runbench.sh -- portable CPU + I/O benchmark for 68030 UNIX systems.
#
# Purpose: compare a REAL 68030 UNIX machine against the Z3660 *emulated* 68030
# (the Zynq-based accelerator running under AmigaOS / Amiga Unix "AMIX").  Run it
# on any 68030 UNIX that has a C compiler and send back the results file.
#
#     sh runbench.sh
#
# Tunables (environment):
#     HZ        clock-tick rate for Dhrystone timing (auto-detected; set it
#               yourself if detection is wrong -- usually 60 or 100)
#     RUNS      Dhrystone iterations  (default 50000; the RATE is what's comparable,
#               so any value giving a >=2 s run is fine -- raise it on fast machines)
#     IOMB      disk I/O test size in MB   (default 2)
#     BENCHDIR  directory for the I/O test file, must be on a REAL disk (default .)
#
# Needs: /bin/sh, a C compiler (cc or gcc), dd; compress is optional.
# Portable Bourne sh, tested on AMIX 2.1 (SVR4.0 m68k).  Uses egrep (not grep '\|'),
# wraps timed commands as "{ time ...; } 2>&1", and times pipelines via "time sh -c".

here=`dirname "$0"`
cd "$here" || exit 1

RUNS=${RUNS:-50000}
IOMB=${IOMB:-2}
BENCHDIR=${BENCHDIR:-.}
HOST=`uname -n 2>/dev/null || hostname 2>/dev/null || echo unknown`
OUT="bench-results.$HOST.txt"
IOBLKS=`expr $IOMB \* 128`        # 128 blocks of 8192 bytes = 1 MB

# ---- Clock-tick rate (HZ) for Dhrystone's times()-based timing (-DHZ=) ----
# Order: caller-supplied $HZ  >  getconf  >  compiled sysconf helper  >  100.
if [ -z "$HZ" ]; then HZ=`( getconf CLK_TCK ) 2>/dev/null`; fi
if [ -z "$HZ" ]; then
  if cc -O hz.c -o hz 2>/dev/null || gcc -O hz.c -o hz 2>/dev/null; then HZ=`./hz 2>/dev/null`
  elif [ -x ./hz.amix ]; then HZ=`./hz.amix 2>/dev/null`; fi    # precompiled AMIX helper
fi
[ -z "$HZ" ] && HZ=100

# ---- Build Dhrystone.  dhry.c is netlib 2.1 with the redundant K&R
#      "extern int times()" removed (clashes with <sys/times.h> on ANSI cc).
#      Some vintage gcc reject -O2 -- use -O. ----
CCNAME=
if   cc  -O -DHZ="$HZ" dhry.c -o dhry 2>/dev/null && [ -f dhry ]; then CCNAME="cc -O -DHZ=$HZ"
elif gcc -O -DHZ="$HZ" dhry.c -o dhry 2>/dev/null && [ -f dhry ]; then CCNAME="gcc -O -DHZ=$HZ"
elif [ -x ./dhry.amix ]; then cp dhry.amix dhry; CCNAME="precompiled dhry.amix (AMIX/SVR4-m68k, HZ=60)"; HZ=60
else echo "ERROR: no compiler (cc/gcc) and no usable precompiled dhry.amix" >&2; exit 1
fi

log() { echo "$*"; echo "$*" >> "$OUT"; }

: > "$OUT"
log "==================================================================="
log " 68030 UNIX benchmark -- $HOST -- `date`"
log " `uname -a 2>/dev/null`"
log " build: $CCNAME -DHZ=$HZ    Dhrystone runs: $RUNS    I/O: ${IOMB}MB"
log "==================================================================="

# ---- Dhrystone (integer CPU); DMIPS = Dhrystones-per-sec / 1757 (VAX-11/780) ----
log "--- Dhrystone 2.1 (integer CPU) ---"
echo "$RUNS" | ./dhry 2>&1 | egrep -i 'per second|microsecond|too small' | while read l
do log "  $l"; done

# ---- disk write / read.  "{ time CMD; } 2>&1" captures real/user/sys reliably
#      (a bare "time cmd | other" does NOT on SVR4 sh). ----
log "--- disk WRITE ${IOMB}MB -> $BENCHDIR/bench.dat ---"
{ time dd if=/dev/zero of="$BENCHDIR/bench.dat" bs=8192 count=$IOBLKS ; } 2>&1 | while read l
do log "  $l"; done
sync

log "--- disk READ ${IOMB}MB ---"
{ time dd if="$BENCHDIR/bench.dat" of=/dev/null bs=8192 ; } 2>&1 | while read l
do log "  $l"; done

# ---- mixed CPU + I/O (optional, needs compress).  Time the pipeline via
#      "time sh -c '...'" -- AMIX sh rejects "time ( subshell )". ----
log "--- mixed CPU+I/O: dd | compress > file (needs 'compress') ---"
{ time sh -c "dd if=/dev/zero bs=65535 count=10 2>/dev/null | compress > '$BENCHDIR/bench.Z'" ; } 2>&1 | while read l
do log "  $l"; done

rm -f "$BENCHDIR/bench.dat" "$BENCHDIR/bench.Z"
log ""
log "Done.  Please send '$OUT' back for the real-vs-emulated-030 comparison."
