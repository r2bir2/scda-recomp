#!/bin/bash
# Detect when spatial_run2_stderr_ts.log stops growing (process reached the
# silent fatal spin-loop) purely via file size polling -- zero cdb involvement.
LOG="/c/VScode/XboxRecomp/tools/debug/spatial_run2_stderr_ts.log"
STABLE_NEEDED=6   # consecutive unchanged checks
INTERVAL=2
prev=-1
stable=0
i=0
while [ $i -lt 300 ]; do
    i=$((i+1))
    sleep $INTERVAL
    cur=$(wc -l < "$LOG" 2>/dev/null || echo 0)
    ts=$(date +%s.%N)
    if [ "$cur" = "$prev" ] && [ "$cur" -gt 100 ]; then
        stable=$((stable+1))
    else
        stable=0
    fi
    echo "$ts check#$i lines=$cur stable=$stable"
    if [ $stable -ge $STABLE_NEEDED ]; then
        echo "STALL-DETECTED ts=$ts lines=$cur"
        break
    fi
    prev=$cur
done
