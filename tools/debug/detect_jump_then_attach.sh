#!/bin/bash
# Detect the natural boot-completion memory jump (~56K -> ~170K+) using ONLY
# tasklist polling (confirmed safe, non-invasive, does not touch the debug
# API at all), then perform the single permitted cdb -pv attach immediately
# after the jump is detected -- avoiding any attach before the jump (which
# was shown this round to prevent the jump from ever happening).
TAG=$1
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
EXE_DIR="/c/VScode/XboxRecomp/build/scda_upgrade/bin/RelWithDebInfo"
OUT="/c/VScode/XboxRecomp/tools/debug/jump_${TAG}.log"

cd "$EXE_DIR" || exit 1
LAUNCH_TS=$(date +%s.%N)
./scda_recomp.exe > "/c/VScode/XboxRecomp/tools/debug/jump_${TAG}_stdout.log" 2> "/c/VScode/XboxRecomp/tools/debug/jump_${TAG}_stderr.log" &
sleep 0.6
PID=$(tasklist //FI "IMAGENAME eq scda_recomp.exe" //FO CSV 2>/dev/null | tail -n +2 | grep -v "27552" | awk -F',' '{gsub(/"/,"",$2); print $2}' | sort -n | tail -1)
echo "=== JUMP-DETECT $TAG launch_ts=$LAUNCH_TS pid=$PID ===" > "$OUT"

prev_kb=0
jumped=0
i=0
while [ $i -lt 200 ]; do
    i=$((i+1))
    memline=$(tasklist //FI "PID eq $PID" //FO CSV 2>/dev/null | tail -1)
    kb=$(echo "$memline" | awk -F',' '{gsub(/"/,"",$5); gsub(/[^0-9]/,"",$5); print $5}')
    ts=$(date +%s.%N)
    rel=$(awk -v t="$ts" -v l="$LAUNCH_TS" 'BEGIN{print t-l}')
    if [ -z "$kb" ]; then
        echo "$rel process-gone" >> "$OUT"
        break
    fi
    if [ "$kb" -gt 150000 ] && [ "$jumped" -eq 0 ]; then
        jumped=1
        echo "$rel JUMP-DETECTED kb=$kb (prev_kb=$prev_kb)" >> "$OUT"
        break
    fi
    prev_kb=$kb
    sleep 0.02
done

if [ "$jumped" -eq 1 ]; then
    attach_ts=$(date +%s.%N)
    attach_rel=$(awk -v a="$attach_ts" -v l="$LAUNCH_TS" 'BEGIN{print a-l}')
    echo "=== ATTACHING at rel=${attach_rel}s ===" >> "$OUT"
    CMD='k 10; s -d 0x10cc000 L0x100000 4f6940; ?? *(unsigned long*)0x1135928; ?? *(unsigned long*)0x1135920; q'
    "$CDB" -pv -p $PID -c "$CMD" >> "$OUT" 2>&1
else
    echo "no jump detected within poll budget" >> "$OUT"
fi

taskkill //F //PID $PID > /dev/null 2>&1
echo "DONE $TAG" >> "$OUT"
echo "$OUT"
