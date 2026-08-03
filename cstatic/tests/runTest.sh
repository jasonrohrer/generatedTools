#!/bin/bash
#
# Runs cstatic against tests/buggy and checks the output two ways:
#   1. emacs really can parse every finding line, and nothing else
#   2. the findings are printed next to the answer key for eyeballing
#
# Usage:  tests/runTest.sh [extra cstatic options...]

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/cstatic-test-$$.txt"

echo "running cstatic on $HERE/buggy ..."
"$HERE/../cstatic.sh" "$@" "$HERE/buggy" > "$OUT" 2>&1
echo "output in $OUT"
echo

cat "$OUT"

echo
echo "=================== emacs compilation-mode check ==================="
echo

if command -v emacs >/dev/null 2>&1; then
    emacs -Q --batch -l "$HERE/checkEmacsParse.el" -f cstatic-check "$OUT"
else
    echo "emacs not installed, skipping the parse check"
fi

echo
echo "=================== answer key ==================="
echo
echo "compare the findings above against $HERE/ANSWERS.md"
echo "roughly twenty bugs are planted; a good run finds most of them and"
echo "reports none of the deliberate non-bugs listed at the end of the key."
