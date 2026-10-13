#!/bin/bash
# SCDA-DIAG-219 orchestration: fresh process per attempt, max ~3 min wait for
# the one-shot capture marker, one short cdb snapshot if it hasn't fired yet,
# immediate kill+retry if the snapshot shows the known unrelated
# sub_0003FCA5/jmp $ freeze on a different stream. No long silent waits.
set -u
BIN_DIR="/c/VScode/XboxRecomp/build/scda_upgrade/bin/RelWithDebInfo"
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
MARKER="$BIN_DIR/scda_diag220_success.marker"
LOGDIR="/c/VScode/XboxRecomp/tools/debug"
MAX_ATTEMPTS=8
WAIT_SLICE=180

taskkill //IM scda_recomp.exe //F 2>/dev/null
rm -f "$MARKER"

SUCCESS=0
for attempt in $(seq 1 $MAX_ATTEMPTS); do
    echo "=== ATTEMPT $attempt: launching fresh process ==="
    rm -f "$MARKER"
    LOG="$LOGDIR/diag219_attempt${attempt}_stderr.log"
    : > "$LOG"
    cd "$BIN_DIR" || exit 1
    ./scda_recomp.exe > /dev/null 2> "$LOG" &
    disown
    sleep 2
    PID=$(powershell -NoProfile -Command "(Get-Process scda_recomp -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Id)" | tr -d '\r')
    if [ -z "$PID" ]; then
        echo "ATTEMPT $attempt: process did not start, retrying"
        continue
    fi
    echo "ATTEMPT $attempt: pid=$PID"
    START=$(date +%s)
    while true; do
        if [ -f "$MARKER" ]; then
            echo "ATTEMPT $attempt: MARKER FOUND after $(( $(date +%s) - START ))s"
            SUCCESS=1
            break
        fi
        NOW=$(date +%s)
        ELAPSED=$((NOW-START))
        if [ $ELAPSED -ge $WAIT_SLICE ]; then
            echo "ATTEMPT $attempt: ${ELAPSED}s elapsed, no marker yet -- taking one cdb snapshot"
            SNAP=$("$CDB" -pv -p $PID -c "r eip; kb 2; q" 2>&1)
            echo "$SNAP" > "$LOGDIR/diag219_attempt${attempt}_snapshot.txt"
            if [ -f "$MARKER" ]; then
                echo "ATTEMPT $attempt: MARKER FOUND (appeared during snapshot)"
                SUCCESS=1
                break
            fi
            if echo "$SNAP" | grep -q "sub_0003FCA5"; then
                echo "ATTEMPT $attempt: frozen in sub_0003FCA5 on a different stream -- killing, moving to next attempt"
                taskkill //PID $PID //F 2>/dev/null
                break
            else
                echo "ATTEMPT $attempt: still progressing elsewhere, giving it one more ${WAIT_SLICE}s slice"
                echo "$SNAP" | grep -E "^scda_recomp!|^[0-9a-f]+\`" | head -5
                START=$NOW
            fi
        fi
        sleep 3
    done
    taskkill //PID $PID //F 2>/dev/null
    if [ $SUCCESS -eq 1 ]; then
        break
    fi
done

taskkill //IM scda_recomp.exe //F 2>/dev/null

if [ $SUCCESS -eq 1 ]; then
    echo "FINAL: SUCCESS"
else
    echo "FINAL: FAILED after $MAX_ATTEMPTS attempts"
fi
