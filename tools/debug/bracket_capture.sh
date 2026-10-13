#!/bin/bash
# Sparse bracket capture around an estimated transition time T0 (seconds,
# process-relative). Alternates a 13-marker spatial address read with a
# ~*kb all-thread stack dump, spaced ~2s apart, to minimize total cdb
# attach count (rapid/continuous polling was observed to hang the target
# in an earlier attempt this session).
#
# Usage: bracket_capture.sh <PID> <LAUNCH_TS> <T0_SECONDS_BEFORE> <T0_SECONDS_AFTER> <OUTLOG>
PID=$1
LAUNCH_TS=$2
BEFORE=$3
AFTER=$4
LOG=$5
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"

A1=0x1135920; A2=0x1135928; A3=0x1136928; A4=0x1137928; A5=0x1138928
A6=0x1139990; A7=0x113A990; A8=0x113B990; A9=0x113C990; A10=0x113D928
A11=0x113D990; A12=0x1145928; A13=0x114D928

spatial() {
    ts=$(date +%s.%N)
    rel=$(echo "$ts - $LAUNCH_TS" | bc)
    out=$("$CDB" -pv -p $PID -c "?? *(unsigned long*)$A1; ?? *(unsigned long*)$A2; ?? *(unsigned long*)$A3; ?? *(unsigned long*)$A4; ?? *(unsigned long*)$A5; ?? *(unsigned long*)$A6; ?? *(unsigned long*)$A7; ?? *(unsigned long*)$A8; ?? *(unsigned long*)$A9; ?? *(unsigned long*)$A10; ?? *(unsigned long*)$A11; ?? *(unsigned long*)$A12; ?? *(unsigned long*)$A13; q" 2>&1)
    vals=$(echo "$out" | grep "^unsigned long" | awk '{print $NF}' | tr '\n' ' ')
    echo "=== SPATIAL ts=$ts rel=${rel}s ===" >> "$LOG"
    echo "vals=[ $vals]" >> "$LOG"
}

stacks() {
    ts=$(date +%s.%N)
    rel=$(echo "$ts - $LAUNCH_TS" | bc)
    out=$("$CDB" -pv -p $PID -c "~*kb; q" 2>&1)
    echo "=== STACKS ts=$ts rel=${rel}s ===" >> "$LOG"
    echo "$out" | grep -A 30 "^ *#" >> "$LOG"
    echo "$out" | grep -E "^[0-9]+  Id:|^\." >> "$LOG"
}

echo "BRACKET-START launch_ts=$LAUNCH_TS before=$BEFORE after=$AFTER" >> "$LOG"
n=$((BEFORE + AFTER))
n=$((n / 2))
for i in $(seq 1 $n); do
    spatial
    sleep 1
    stacks
    sleep 1
done
echo "BRACKET-END" >> "$LOG"
