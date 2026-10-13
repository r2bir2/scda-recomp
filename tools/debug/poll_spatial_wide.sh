#!/bin/bash
# Wide spatial poll across the full 0x8070-byte region (Update 193 template,
# extended to 13 markers spanning start..end plus two unaffected controls).
# Usage: poll_spatial_wide.sh <PID> <LOG> [MAX_ITERS]
PID=$1
LOG=$2
MAX=${3:-200}
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"

# Addresses relative to known object user-pointer this=0x1135928
A1=0x1135920   # this-8      allocator boundary tag
A2=0x1135928   # this+0x0    vtable / object start
A3=0x1136928   # this+0x1000
A4=0x1137928   # this+0x2000
A5=0x1138928   # this+0x3000
A6=0x1139990   # this+0x4068 (0x4070/0x4000 internal boundary)
A7=0x113A990   # this+0x5068
A8=0x113B990   # this+0x6068
A9=0x113C990   # this+0x7068
A10=0x113D928  # this+0x8000
A11=0x113D990  # this+0x8068 (last known-zero byte, upper edge)
A12=0x1145928  # this+0x10000 (control, expected unaffected)
A13=0x114D928  # this+0x18000 (control, expected unaffected)

i=0
while [ $i -lt $MAX ]; do
    i=$((i+1))
    ts=$(date +%s.%N)
    out=$("$CDB" -pv -p $PID -c "?? *(unsigned long*)$A1; ?? *(unsigned long*)$A2; ?? *(unsigned long*)$A3; ?? *(unsigned long*)$A4; ?? *(unsigned long*)$A5; ?? *(unsigned long*)$A6; ?? *(unsigned long*)$A7; ?? *(unsigned long*)$A8; ?? *(unsigned long*)$A9; ?? *(unsigned long*)$A10; ?? *(unsigned long*)$A11; ?? *(unsigned long*)$A12; ?? *(unsigned long*)$A13; q" 2>&1)
    vals=$(echo "$out" | grep "unsigned long" | awk '{print $NF}' | tr '\n' ' ')
    running=$(tasklist //FI "PID eq $PID" 2>/dev/null | grep -c scda_recomp)
    echo "$ts poll#$i vals=[ $vals] running=$running" >> "$LOG"
    if [ "$running" -eq 0 ]; then
        echo "process gone, stopping" >> "$LOG"
        break
    fi
done
