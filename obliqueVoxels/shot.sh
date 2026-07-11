#!/bin/bash
# shot.sh -- run obliqueVoxels headlessly under Xvfb and grab a screenshot.
#   ./shot.sh out.png [args to obliqueVoxels...]
OUT="${1:-shot.png}"; shift
DISP=":99"
Xvfb "$DISP" -screen 0 1200x720x24 >/tmp/ov_xvfb.log 2>&1 &
XPID=$!
sleep 0.7
DISPLAY="$DISP" ./obliqueVoxels "$@" >/tmp/ov.log 2>&1 &
CPID=$!
sleep 2.0
DISPLAY="$DISP" scrot "$OUT" 2>/tmp/ov_scrot.log
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
kill $XPID 2>/dev/null; wait $XPID 2>/dev/null
echo "screenshot -> $OUT (app log below)"; cat /tmp/ov.log
