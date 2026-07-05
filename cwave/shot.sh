#!/bin/bash
# shot.sh -- run cwave headlessly under Xvfb and grab a screenshot.
#   ./shot.sh out.png [args to cwave...]
# Waits a moment for the window to render, then scrots the virtual display.
OUT="${1:-shot.png}"; shift
DISP=":99"
Xvfb "$DISP" -screen 0 1200x720x24 >/tmp/xvfb.log 2>&1 &
XPID=$!
sleep 0.7
DISPLAY="$DISP" ./cwave "$@" >/tmp/cwave.log 2>&1 &
CPID=$!
sleep 2.0
DISPLAY="$DISP" scrot "$OUT" 2>/tmp/scrot.log
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
kill $XPID 2>/dev/null; wait $XPID 2>/dev/null
echo "screenshot -> $OUT (cwave log below)"; cat /tmp/cwave.log
