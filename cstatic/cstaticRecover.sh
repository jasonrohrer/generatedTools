#!/bin/bash
#
# cstaticRecover.sh -- rebuild an emacs-ready report from a cstatic work
#                      directory that was left behind by "cstatic.sh -k".
#
# A big project takes a long time.  If you get tired of waiting and kill the
# run, the answers Claude already produced are sitting in the work directory
# doing nothing.  This turns whatever is there into the same GCC-style output
# that cstatic.sh prints, so "M-x compile" can page through it.
#
#     cstaticRecover.sh /tmp/cstatic.XkMsiC
#     cstaticRecover.sh                        # the most recent work dir
#
# It only ever READS the work directory, so it is safe to run against a job
# that is still going -- you get a snapshot of the findings so far.
#
# Findings that never made it through the skeptical verification pass are
# marked "[unverified]".  Those are raw first-pass output: expect a noticeably
# higher false-alarm rate than a completed run.

set -u
set -o pipefail

QUIET=0
WORKDIR=""

usage() {
    cat <<'EOF'
cstaticRecover.sh -- rebuild an emacs-ready report from a cstatic work dir.

  cstaticRecover.sh [-q] [workdir]

  workdir   a directory left behind by "cstatic.sh -k", e.g. /tmp/cstatic.XkMsiC
            With no argument, the most recently modified /tmp/cstatic.* is used.

  -q        print findings only, no progress or coverage lines
  -h        this help

The work directory is only read, never written, so this is safe to run against
a job that is still in progress.

Findings marked [unverified] come from the first pass only: the skeptical
verification pass had not reached them yet.  They are noisier than the findings
of a completed run.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -q) QUIET=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "cstaticRecover: unknown option $1" >&2; usage >&2; exit 2 ;;
        *) WORKDIR="$1"; shift ;;
    esac
done

note() { [ "$QUIET" -eq 1 ] || echo "$*"; }

# ------------------------------------------------------- find the work dir

if [ -z "$WORKDIR" ]; then
    WORKDIR="$(ls -1dt "${TMPDIR:-/tmp}"/cstatic.* 2>/dev/null | head -1)"
    if [ -z "$WORKDIR" ]; then
        echo "cstaticRecover: no cstatic work directory found in ${TMPDIR:-/tmp}"
        echo "cstaticRecover: pass one explicitly, e.g. /tmp/cstatic.XkMsiC"
        exit 1
    fi
fi

WORKDIR="${WORKDIR%/}"

if [ ! -d "$WORKDIR/raw" ]; then
    echo "cstaticRecover: $WORKDIR does not look like a cstatic work directory"
    echo "cstaticRecover: (expected a raw/ subdirectory inside it)"
    exit 1
fi

# ------------------------------------------------- recover the project root

# The prompts record it, either directly or via the full path of the file
# under review in isolation mode.
ROOT="$(grep -h -m1 '^PROJECT ROOT: ' "$WORKDIR"/prompts/*.txt 2>/dev/null \
        | head -1 | sed 's/^PROJECT ROOT: //')"
if [ -z "$ROOT" ]; then
    FULLPATH="$(grep -h -m1 '^FULL PATH: ' "$WORKDIR"/prompts/*.txt 2>/dev/null \
                | head -1 | sed 's/^FULL PATH:[[:space:]]*//')"
    REL="$(grep -h -m1 '^FILE UNDER REVIEW: ' "$WORKDIR"/prompts/*.txt 2>/dev/null \
           | head -1 | sed 's/^FILE UNDER REVIEW:[[:space:]]*//')"
    if [ -n "$FULLPATH" ] && [ -n "$REL" ]; then
        ROOT="${FULLPATH%/"$REL"}"
    fi
fi
[ -z "$ROOT" ] && ROOT="$PWD"

MY="$(mktemp -d "${TMPDIR:-/tmp}/cstaticRecover.XXXXXX")"

TEE_PID=""
flush_log() {
    [ -z "${TEE_PID:-}" ] && return
    exec 1>&- 2>&-
    wait "$TEE_PID" 2>/dev/null
    TEE_PID=""
}
trap 'rm -rf "$MY"; flush_log' EXIT

# ------------------------------------------------------------------- log

if [ -n "${CSTATIC_LOG_DIR:-}" ]; then
    LOG_DIR="$CSTATIC_LOG_DIR"
elif [ -d "$ROOT/.claude" ]; then
    LOG_DIR="$ROOT/.claude/cstatic"
else
    LOG_DIR="$HOME/.claude/cstatic"
fi
if mkdir -p "$LOG_DIR" 2>/dev/null && [ -w "$LOG_DIR" ]; then
    LOGBASE="$LOG_DIR/cstatic-$(basename "$ROOT")-recovered-$(date +%Y%m%d-%H%M%S)"
else
    LOGBASE="${TMPDIR:-/tmp}/cstatic-$(basename "$ROOT")-recovered-$(date +%Y%m%d-%H%M%S)"
fi
# recovery gets re-run in quick succession; a one-second timestamp is not
# unique enough to stop those runs from overwriting each other
LOG="$LOGBASE.log"
logn=2
while [ -e "$LOG" ]; do
    LOG="$LOGBASE-$logn.log"
    logn=$((logn + 1))
done

# Mirror to the log through a tee whose PID we keep, so we can wait for it.
# "exec > >(tee ...)" is never waited for: emacs closes the pipe as soon as
# this script exits, tee takes a SIGPIPE mid-write, and the report is cut off
# at a 4096-byte boundary in a different place every run.  An interactive
# shell usually lets it finish, which is what makes the bug look intermittent.
if : > "$LOG" 2>/dev/null && mkfifo "$MY/logpipe" 2>/dev/null; then
    tee -a "$LOG" < "$MY/logpipe" &
    TEE_PID=$!
    exec > "$MY/logpipe" 2>&1
else
    LOG=""
fi

log_location_note() {
    [ -n "$LOG" ] && echo "cstaticRecover: this report is saved at $LOG"
    return 0
}

# --------------------------------------------------------------- the parser

# Identical to the PARSER in cstatic.sh.  If you change one, change both.
PARSER='
function fixpath(p,   i, r, b, best) {
  if (substr(p, 1, length(ROOTQ) + 1) == ROOTQ "/") p = substr(p, length(ROOTQ) + 2)
  sub(/^\.\//, "", p)
  if (p in known) return p
  best = ""
  for (i = 1; i <= nk; i++) {
    r = knownlist[i]
    if (length(p) > length(r) && substr(p, length(p) - length(r)) == "/" r) {
      if (length(r) > length(best)) best = r
    }
  }
  if (best != "") return best
  b = p
  sub(/^.*\//, "", b)
  if ((b in basemap) && !(b in basedup)) return basemap[b]
  return p
}
function flush(   key) {
  if (!inblock) return
  inblock = 0
  if (file == "" || line !~ /^[0-9]+$/) return
  if (msg == "") return
  file = fixpath(file)
  if (col !~ /^[0-9]+$/ || col+0 < 1) col = 1
  if (sev != "error" && sev != "warning") sev = "warning"
  printf "%s\t%s\t%s\t%s\t%s\t%s\n", file, line+0, col+0, sev, msg, detail
}
BEGIN {
  inblock = 0
  nk = 0
  if (KNOWNFILE != "") {
    while ((getline kline < KNOWNFILE) > 0) {
      if (kline == "") continue
      known[kline] = 1
      knownlist[++nk] = kline
      kb = kline
      sub(/^.*\//, "", kb)
      if (kb in basemap) basedup[kb] = 1; else basemap[kb] = kline
    }
    close(KNOWNFILE)
  }
}
/^[ \t]*<<<FINDING>>>[ \t]*$/ {
  flush(); inblock = 1; file=""; line=""; col=""; sev=""; msg=""; detail=""; next
}
/^[ \t]*<<<END>>>[ \t]*$/ { flush(); next }
inblock {
  s = $0
  gsub(/\t/, " ", s)
  if (s ~ /^[ \t]*FILE:/)        { sub(/^[ \t]*FILE:[ \t]*/, "", s);   file = s }
  else if (s ~ /^[ \t]*LINE:/)   { sub(/^[ \t]*LINE:[ \t]*/, "", s);   sub(/[^0-9].*$/, "", s); line = s }
  else if (s ~ /^[ \t]*COL:/)    { sub(/^[ \t]*COL:[ \t]*/, "", s);    sub(/[^0-9].*$/, "", s); col = s }
  else if (s ~ /^[ \t]*SEV:/)    { sub(/^[ \t]*SEV:[ \t]*/, "", s);    gsub(/[ \t]/, "", s); sev = tolower(s) }
  else if (s ~ /^[ \t]*MSG:/)    { sub(/^[ \t]*MSG:[ \t]*/, "", s);    msg = s }
  else if (s ~ /^[ \t]*DETAIL:/) { sub(/^[ \t]*DETAIL:[ \t]*/, "", s)
                                   if (s != "") detail = (detail == "" ? s : detail "\001" s) }
}
END { flush() }
'

DEDUPE='
{
  key = $1 "\t" $2
  if (!(key in best) || length($0) > length(best[key])) best[key] = $0
  if (!(key in ord)) { ord[key] = ++n; keys[n] = key }
}
END { for (i = 1; i <= n; i++) print best[keys[i]] }
'

# ------------------------------------------------------------ known paths

if [ -d "$ROOT" ]; then
    while IFS= read -r f; do
        printf '%s\n' "${f#"$ROOT"/}"
    done < <(find "$ROOT" -type f \( -name '*.c' -o -name '*.h' \) \
                  -not -path '*/.git/*' 2>/dev/null | LC_ALL=C sort) \
        > "$MY/knownpaths.txt"
else
    : > "$MY/knownpaths.txt"
fi

# ------------------------------------------------------- gather what exists

NA_TOTAL=0
[ -f "$WORKDIR/joblist.txt" ] && NA_TOTAL=$(wc -l < "$WORKDIR/joblist.txt" | tr -d ' ')
NA_DONE=$(find "$WORKDIR/raw" -maxdepth 1 -name 'a*.out' -size +0 2>/dev/null | wc -l)
NV_TOTAL=0
[ -f "$WORKDIR/vjoblist.txt" ] && NV_TOTAL=$(wc -l < "$WORKDIR/vjoblist.txt" | tr -d ' ')
NV_DONE=$(find "$WORKDIR/raw" -maxdepth 1 -name 'v*.out' -size +0 2>/dev/null | wc -l)

note "cstaticRecover: rebuilding from $WORKDIR"
note "cstaticRecover: project root $ROOT"
if [ -n "$LOG" ]; then
    note "cstaticRecover: logging this report to $LOG"
else
    note "cstaticRecover: could not open a log file, so this report is not saved"
fi
if [ "$NA_TOTAL" -gt 0 ]; then
    note "cstaticRecover: $NA_DONE of $NA_TOTAL analysis job(s) had finished"
else
    note "cstaticRecover: $NA_DONE analysis job(s) produced output"
fi
if [ "$NV_TOTAL" -gt 0 ]; then
    note "cstaticRecover: $NV_DONE of $NV_TOTAL verification job(s) had finished"
else
    note "cstaticRecover: verification had not started"
fi
note ""

if [ "$NA_DONE" -eq 0 ]; then
    echo "cstaticRecover: nothing to recover -- no analysis job had produced"
    echo "cstaticRecover: any output yet in $WORKDIR/raw"
    log_location_note
    exit 0
fi

# raw first-pass candidates
cat "$WORKDIR"/raw/a*.out 2>/dev/null \
    | awk -v ROOTQ="$ROOT" -v KNOWNFILE="$MY/knownpaths.txt" "$PARSER" \
    > "$MY/candidates.raw.tsv"
sort -t$'\t' -k1,1 -k2,2n "$MY/candidates.raw.tsv" \
    | awk -F'\t' "$DEDUPE" > "$MY/candidates.tsv"

NRAW=$(wc -l < "$MY/candidates.raw.tsv" | tr -d ' ')
NDISTINCT=$(wc -l < "$MY/candidates.tsv" | tr -d ' ')
note "cstaticRecover: $NRAW raw report(s) -> $NDISTINCT distinct file+line finding(s)"
if [ "$NRAW" -gt "$NDISTINCT" ]; then
    note "cstaticRecover: (a big header gets re-read by every file that includes" \
         "it, so its bugs get reported many times over; same file and line means" \
         "the same defect)"
fi

# verified findings, if the verify pass got anywhere
: > "$MY/verified.tsv"
if [ "$NV_DONE" -gt 0 ]; then
    cat "$WORKDIR"/raw/v*.out 2>/dev/null \
        | awk -v ROOTQ="$ROOT" -v KNOWNFILE="$MY/knownpaths.txt" "$PARSER" \
        > "$MY/verified.tsv"
fi

# Which target files did a verifier actually finish?  Those files are taken
# from the verified output only -- a verifier that rejected everything for a
# file means that file has no findings, not that we should fall back to the
# raw guesses it rejected.
: > "$MY/settled.txt"
if [ -f "$WORKDIR/vjoblist.txt" ]; then
    while IFS=$'\t' read -r vid tgt; do
        [ -z "${vid:-}" ] && continue
        if [ -s "$WORKDIR/raw/$vid.out" ]; then
            printf '%s\n' "$tgt" >> "$MY/settled.txt"
        fi
    done < "$WORKDIR/vjoblist.txt"
elif [ "$NV_DONE" -gt 0 ]; then
    # no job list to consult, so trust whatever the verifiers named
    cut -f1 "$MY/verified.tsv" | LC_ALL=C sort -u > "$MY/settled.txt"
fi

# unverified = candidates for files no verifier settled, tagged as such.
# settled.txt is read in BEGIN rather than as a first input file: when it is
# empty, the usual NR==FNR idiom stays true for the whole second file and
# silently eats every candidate.
awk -F'\t' -v OFS='\t' -v SETTLED="$MY/settled.txt" '
  BEGIN { while ((getline s < SETTLED) > 0) if (s != "") settled[s] = 1; close(SETTLED) }
  !($1 in settled) { $5 = "[unverified] " $5; print }
' "$MY/candidates.tsv" > "$MY/unverified.tsv"

cat "$MY/verified.tsv" "$MY/unverified.tsv" \
    | sort -t$'\t' -k1,1 -k2,2n | awk -F'\t' "$DEDUPE" > "$MY/final.tsv"

NFINAL=$(wc -l < "$MY/final.tsv" | tr -d ' ')
[ -z "$NFINAL" ] && NFINAL=0
NUNV=$(grep -c '\[unverified\]' "$MY/final.tsv" 2>/dev/null)
[ -z "$NUNV" ] && NUNV=0

note "cstaticRecover: ---------------------------------------------------------"
note ""

# ------------------------------------------------------------------ print

# Identical to the print stage in cstatic.sh.  If you change one, change both.
sort -t$'\t' -k1,1 -k2,2n "$MY/final.tsv" | awk -F'\t' '
function sanitize(s,    out, m, num, name) {
  out = ""
  while (match(s, /[A-Za-z0-9_][A-Za-z0-9_.\/-]*\.[chCH](:|,?[ ]+lines?[ ]+)[0-9]+/)) {
    out = out substr(s, 1, RSTART - 1)
    m = substr(s, RSTART, RLENGTH)
    s = substr(s, RSTART + RLENGTH)
    if (match(m, /[0-9]+$/)) {
      num = substr(m, RSTART)
      name = substr(m, 1, RSTART - 1)
      sub(/(:|,?[ ]+lines?[ ]+)$/, "", name)
      out = out name "@" num
      if (match(s, /^:[0-9]/)) { s = "." substr(s, 2) }
    }
    else { out = out m }
  }
  out = out s
  while (match(out, /:[0-9]/)) {
    out = substr(out, 1, RSTART - 1) " " substr(out, RSTART + 1)
  }
  return out
}
function wrap(text,    words, i, n, cur) {
  n = split(text, words, " ")
  cur = ""
  for (i = 1; i <= n; i++) {
    if (cur == "") { cur = words[i] }
    else if (length(cur) + 1 + length(words[i]) <= 74) { cur = cur " " words[i] }
    else { lines[++nl] = cur; cur = words[i] }
  }
  if (cur != "") lines[++nl] = cur
}
{
  printf "%s:%s:%s: %s: %s\n", $1, $2, $3, $4, sanitize($5)
  nl = 0
  delete lines
  n = split($6, d, "\001")
  for (i = 1; i <= n; i++) if (d[i] != "") wrap(sanitize(d[i]))
  for (i = 1; i <= nl && i <= 4; i++) {
    if (i == 4 && nl > 4) print "    " lines[i] " ..."
    else print "    " lines[i]
  }
  print ""
}
'

NERR=$(awk -F'\t' '$4 == "error"' "$MY/final.tsv" | wc -l | tr -d ' ')
NWARN=$(awk -F'\t' '$4 != "error"' "$MY/final.tsv" | wc -l | tr -d ' ')

if [ "$NFINAL" -eq 0 ]; then
    echo "cstaticRecover: no findings in what had been produced so far."
else
    echo "cstaticRecover: $NFINAL finding(s): $NERR error(s), $NWARN warning(s)."
fi
if [ "$NUNV" -gt 0 ]; then
    echo "cstaticRecover: $NUNV of them are [unverified] -- first-pass output that" \
         "never faced the skeptical second reader, so expect false alarms."
fi
if [ "$NA_TOTAL" -gt "$NA_DONE" ]; then
    echo "cstaticRecover: this is PARTIAL -- $(( NA_TOTAL - NA_DONE ))" \
         "of $NA_TOTAL analysis job(s) had not finished, so parts of the code" \
         "were never read."
fi
echo "cstaticRecover: duplicate root causes are not collapsed here; the same bug" \
     "may appear at both its definition and a call site."
log_location_note

exit 0
