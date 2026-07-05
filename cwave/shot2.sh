#!/bin/bash
# shot2.sh OUTPREFIX DELAY1 DELAY2 [cwave args...]
# grabs two screenshots at two delays during one cwave run.
PRE="$1"; D1="$2"; D2="$3"; shift 3
DISP=":99"
Xvfb "$DISP" -screen 0 1200x720x24 >/tmp/xvfb.log 2>&1 &
XPID=$!
sleep 0.7
DISPLAY="$DISP" ./cwave "$@" >/tmp/cwave.log 2>&1 &
CPID=$!
sleep "$D1"
DISPLAY="$DISP" scrot "${PRE}_a.png" 2>/dev/null
sleep "$D2"
DISPLAY="$DISP" scrot "${PRE}_b.png" 2>/dev/null
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
kill $XPID 2>/dev/null; wait $XPID 2>/dev/null
echo "done; cwave log:"; cat /tmp/cwave.log