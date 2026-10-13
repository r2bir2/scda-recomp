#!/bin/bash
# Same dual-address poll as poll_boundary2.sh, but with an explicit gap
# between cdb attach cycles. Rationale: two prior attempts this session
# (PID 3660 after ~400 tight-loop attaches, PID 47872 after ~200) both
# ended up permanently unattachable (cdb: "Win32 error 0n156") even though
# the target process kept running -- repeated non-invasive cdb -pv attach
# cycles appear to have a cumulative cost on attachability itself. Adding
# a deliberate gap trades poll resolution for not breaking the target.
PID=$1
LOG=$2
GAP=${3:-3}
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
i=0
while true; do
    i=$((i+1))
    ts=$(date +%s.%N)
    out=$("$CDB" -pv -p $PID -c "?? *(unsigned long*)0x1135920; ?? *(unsigned long*)0x1135928; q" 2>&1)
    tag=$(echo "$out" | grep "^unsigned long" | sed -n '1p' | awk '{print $NF}')
    vt=$(echo "$out" | grep "^unsigned long" | sed -n '2p' | awk '{print $NF}')
    attach_err=$(echo "$out" | grep -c "Unable to examine")
    running=$(tasklist //FI "PID eq $PID" 2>/dev/null | grep -c scda_recomp)
    echo "$ts poll#$i boundarytag=$tag vtable=$vt attach_err=$attach_err running=$running" >> "$LOG"
    if [ "$running" -eq 0 ]; then
        echo "process gone, stopping" >> "$LOG"
        break
    fi
    sleep $GAP
done
