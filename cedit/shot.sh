#!/bin/bash
# shot.sh -- run cedit under a private Xvfb display and grab screenshots,
# optionally typing keys in between (so nothing appears on your screen).
#
#   ./shot.sh out.png [cedit args...]
#
# Env:
#   KEYS="ctrl+x ctrl+f ..."   xdotool key sequence sent before the shot
#   TYPE="some text"           xdotool type text sent before the shot
#   DISP=:98                   display number (default :98)
OUT="${1:-shot.png}"; shift
DISP="${DISP:-:98}"
LOG="${TMPDIR:-/tmp}/cedit-shot.log"
Xvfb "$DISP" -screen 0 1280x900x24 >/dev/null 2>&1 &
XPID=$!
sleep 0.7
DISPLAY="$DISP" ./cedit "$@" >"$LOG" 2>&1 &
CPID=$!
sleep 1.2
WID=$(DISPLAY="$DISP" xdotool search --name '^cedit' | head -1)
if [ -n "$WID" ]; then
    DISPLAY="$DISP" xdotool windowmove "$WID" 0 0
    DISPLAY="$DISP" xdotool windowfocus "$WID" 2>/dev/null
fi
sleep 0.3
if [ -n "$TYPE" ]; then DISPLAY="$DISP" xdotool type --delay 20 "$TYPE"; fi
if [ -n "$KEYS" ]; then DISPLAY="$DISP" xdotool key --delay 60 $KEYS; fi
sleep 0.6
DISPLAY="$DISP" scrot -o "$OUT"
kill $CPID 2>/dev/null; wait $CPID 2>/dev/null
kill $XPID 2>/dev/null; wait $XPID 2>/dev/null
echo "screenshot -> $OUT"; cat "$LOG"
