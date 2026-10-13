#!/bin/bash
# Launch scda_recomp.exe fresh and timestamp every stderr line as it arrives,
# with zero cdb/debugger interference during boot (avoids the hang observed
# when polling too aggressively during early boot I/O).
cd "/c/VScode/XboxRecomp/build/scda_upgrade/bin/RelWithDebInfo" || exit 1
OUTDIR="/c/VScode/XboxRecomp/tools/debug"
date +%s.%N > "$OUTDIR/spatial_run2_meta.txt"
./scda_recomp.exe 2>&1 | while IFS= read -r line; do
    printf '%s %s\n' "$(date +%s.%N)" "$line"
done > "$OUTDIR/spatial_run2_stderr_ts.log"
