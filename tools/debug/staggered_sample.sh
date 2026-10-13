#!/bin/bash
# Staggered single-attach sampling (Update 205 methodology fix): each fresh
# scda_recomp.exe process is touched by cdb -pv EXACTLY ONCE, at a specified
# target time after launch, then killed. No continuous polling, no repeated
# attaches per process (repeated attaches were shown in Update 205 to stall
# the target or make it permanently unattachable).
#
# Object identity is NOT assumed fixed across processes: we search the known
# heap arena (0x010CC000-0x011CC000, per Update 204) for the vtable constant
# 0x4F6940 as the primary discovery method, and also probe the historically
# observed fixed address 0x1135928 as a cross-check (not as ground truth).
#
# Usage: staggered_sample.sh <TARGET_SECONDS> <SAMPLE_TAG>
TARGET=$1
TAG=$2
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
EXE_DIR="/c/VScode/XboxRecomp/build/scda_upgrade/bin/RelWithDebInfo"
OUT="/c/VScode/XboxRecomp/tools/debug/stagger_${TAG}.log"

cd "$EXE_DIR" || exit 1
LAUNCH_TS=$(date +%s.%N)
./scda_recomp.exe > "/c/VScode/XboxRecomp/tools/debug/stagger_${TAG}_stdout.log" 2> "/c/VScode/XboxRecomp/tools/debug/stagger_${TAG}_stderr.log" &
BASHPID=$!
sleep 0.6
PID=$(tasklist //FI "IMAGENAME eq scda_recomp.exe" //FO CSV 2>/dev/null | tail -n +2 | grep -v "27552" | awk -F',' '{gsub(/"/,"",$2); print $2}' | sort -n | tail -1)

echo "=== SAMPLE $TAG target=${TARGET}s launch_ts=$LAUNCH_TS pid=$PID ===" > "$OUT"

# sleep until target offset relative to launch
now=$(date +%s.%N)
remaining=$(awk -v n="$now" -v l="$LAUNCH_TS" -v t="$TARGET" 'BEGIN{print t-(n-l)}')
needs_sleep=$(awk -v r="$remaining" 'BEGIN{print (r>0)?1:0}')
if [ "$needs_sleep" = "1" ]; then
    sleep "$remaining"
fi
actual_ts=$(date +%s.%N)
actual_elapsed=$(awk -v a="$actual_ts" -v l="$LAUNCH_TS" 'BEGIN{print a-l}')
echo "actual_elapsed=${actual_elapsed}s" >> "$OUT"

mem=$(tasklist //FI "PID eq $PID" //FO CSV 2>/dev/null | tail -1 | cut -d',' -f5)
echo "mem_at_sample=$mem" >> "$OUT"

CMD='s -d 0x10cc000 L0x100000 4f6940; ?? *(unsigned long*)0x1135928; ?? *(unsigned long*)0x1135920; ?? *(unsigned long*)0x1135964; ?? *(unsigned long*)0x1139980; ?? *(unsigned long*)0x1139984; ?? *(unsigned long*)0x1136928; ?? *(unsigned long*)0x1139990; ?? *(unsigned long*)0x113b990; ?? *(unsigned long*)0x113d928; ?? *(unsigned long*)0x113d990; k 3; q'

"$CDB" -pv -p $PID -c "$CMD" >> "$OUT" 2>&1

kill_ts=$(date +%s.%N)
kill_rel=$(awk -v k="$kill_ts" -v l="$LAUNCH_TS" 'BEGIN{print k-l}')
echo "=== cdb call finished at rel=${kill_rel}s ===" >> "$OUT"

taskkill //F //PID $PID > /dev/null 2>&1
echo "DONE $TAG" >> "$OUT"
echo "$OUT"
