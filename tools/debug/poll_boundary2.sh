#!/bin/bash
# Reproduction of the proven Update 193 dual-address poll (boundary tag +
# vtable), single cdb -pv call per iteration, ~1.2s cadence. Deliberately
# simple/lightweight -- this exact pattern never hung a target across many
# prior updates, unlike the heavier 13-address variant tried earlier today.
PID=$1
LOG=$2
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
i=0
while true; do
    i=$((i+1))
    ts=$(date +%s.%N)
    out=$("$CDB" -pv -p $PID -c "?? *(unsigned long*)0x1135920; ?? *(unsigned long*)0x1135928; q" 2>&1)
    tag=$(echo "$out" | grep "^unsigned long" | sed -n '1p' | awk '{print $NF}')
    vt=$(echo "$out" | grep "^unsigned long" | sed -n '2p' | awk '{print $NF}')
    running=$(tasklist //FI "PID eq $PID" 2>/dev/null | grep -c scda_recomp)
    echo "$ts poll#$i boundarytag=$tag vtable=$vt running=$running" >> "$LOG"
    if [ "$running" -eq 0 ]; then
        echo "process gone, stopping" >> "$LOG"
        break
    fi
done
