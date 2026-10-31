#!/bin/bash
#
# Regression tests for cedit's editing behavior (no display needed).
#
#   tests/run.sh            run all tests
#   tests/run.sh --update   regenerate the .expected files (check them!)
#
# Each NAME.keys is a keystroke script (see "cedit --script" in cedit.c).
# If NAME.input exists, it names a file in tests/ to start from;
# otherwise the test starts from a new empty file.  The resulting buffer
# is compared to NAME.expected.
#
# formatSample.c is run through --format and compared to
# formatSample.expected, and cedit's own sources must survive
# --check-indent unchanged (dogfooding the indenter).

cd "$(dirname "$0")"
CEDIT=../cedit
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
update=0
[ "$1" == "--update" ] && update=1

for keys in *.keys; do
    name=${keys%.keys}
    work="$TMP/$name.c"
    if [ -f "$name.input" ]; then
        cp "$(cat "$name.input")" "$work"
    fi
    out=$(cd "$TMP" && "$OLDPWD/$CEDIT" --script "$OLDPWD/$keys" "$name.c" 2>/dev/null)
    if [ $update == 1 ]; then
        printf '%s\n' "$out" > "$name.expected"
    elif ! diff <(printf '%s\n' "$out") "$name.expected" > "$TMP/diff.txt"; then
        echo "FAIL: $name"
        cat "$TMP/diff.txt"
        fail=1
    else
        echo "ok:   $name"
    fi
done

out=$($CEDIT --format formatSample.c)
if [ $update == 1 ]; then
    printf '%s\n' "$out" > formatSample.expected
elif ! diff <(printf '%s\n' "$out") formatSample.expected; then
    echo "FAIL: formatSample"
    fail=1
else
    echo "ok:   formatSample"
fi

if (cd .. && ./cedit --check-indent cedit.c editor.c textBuffer.c syntax.c \
        indent.c format.c config.c compile.c *.h > "$TMP/indent.txt"); then
    echo "ok:   own sources indent-clean"
else
    echo "FAIL: own sources not indent-clean:"
    cat "$TMP/indent.txt"
    fail=1
fi

exit $fail
