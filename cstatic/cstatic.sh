#!/bin/bash
#
# cstatic.sh -- Claude-powered static analysis for C89 projects,
#               with GCC-style output that "M-x compile" can page through.
#
# Usage (from inside the project folder):
#
#     M-x compile RET  /path/to/cstatic.sh RET
#
# The script never compiles or runs your code.  It reads every .c and .h
# file, asks Claude to hunt for serious, real bugs, then asks a second,
# independent Claude to try to REFUTE each candidate.  Only findings that
# survive refutation are printed, and a last pass collapses the ones that
# turn out to share a single root cause.
#
# Output lines look like:
#
#     game.c:199:8: error: short one-line summary
#         explanation line 1
#         explanation line 2
#
# which is exactly what compilation-mode's GNU parser expects.  "error:"
# means the bug is reachable today; "warning:" means it is real but nothing
# currently drives the program into it.
#
# Run "cstatic.sh -h" for options.

set -u
set -o pipefail

VERSION="1.0"

# ---------------------------------------------------------------- defaults

CLAUDE="${CSTATIC_CLAUDE:-$HOME/.local/bin/claude}"
MODEL="${CSTATIC_MODEL:-opus}"
EFFORT="${CSTATIC_EFFORT:-high}"
JOBS="${CSTATIC_JOBS:-4}"
CHUNK_LINES="${CSTATIC_CHUNK_LINES:-600}"
CHUNK_OVERLAP="${CSTATIC_CHUNK_OVERLAP:-80}"
MAX_FINDINGS="${CSTATIC_MAX_FINDINGS:-10}"
JOB_TIMEOUT="${CSTATIC_TIMEOUT:-1200}"
DO_VERIFY=1
DO_GLOBAL=1
DO_MERGE=1
RECURSE=0
KEEP_TMP=0
QUIET=0
declare -a EXCLUDES=()
declare -a ARGFILES=()

usage() {
    cat <<'EOF'
cstatic.sh -- Claude-powered static analysis for C89, emacs-compile friendly.

  cstatic.sh [options] [dir | file.c file.h ...]

With no path argument, analyzes the current directory.

Options:
  -j N            parallel Claude jobs               (default 4)
  -m MODEL        model: opus | sonnet | haiku | id  (default opus)
  -e LEVEL        effort: low|medium|high|xhigh|max  (default high)
  -r              recurse into subdirectories        (default: top level only)
  -x GLOB         exclude files matching GLOB (repeatable, matched on basename
                  and on the path relative to the analyzed directory)
  -c N            lines per analysis chunk           (default 600, 0 = no chunking)
  -n N            max findings reported per chunk    (default 10)
  -t SECS         per-Claude-call timeout            (default 1200)
  --no-verify     skip the skeptical second pass (faster, noisier)
  --no-global     skip the whole-project cross-file pass
  --no-merge      skip the final pass that collapses duplicate root causes
  -k              keep the work directory (raw Claude output) for debugging
  -q              suppress progress lines; print findings only
  -h              this help

A ".cstaticignore" file in the analyzed directory adds one exclude glob per
line (blank lines and #comments ignored).

Environment overrides: CSTATIC_CLAUDE CSTATIC_MODEL CSTATIC_EFFORT
CSTATIC_JOBS CSTATIC_CHUNK_LINES CSTATIC_CHUNK_OVERLAP CSTATIC_MAX_FINDINGS
CSTATIC_TIMEOUT
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -m) MODEL="$2"; shift 2 ;;
        -e) EFFORT="$2"; shift 2 ;;
        -c) CHUNK_LINES="$2"; shift 2 ;;
        -n) MAX_FINDINGS="$2"; shift 2 ;;
        -t) JOB_TIMEOUT="$2"; shift 2 ;;
        -x) EXCLUDES+=("$2"); shift 2 ;;
        -r) RECURSE=1; shift ;;
        -k) KEEP_TMP=1; shift ;;
        -q) QUIET=1; shift ;;
        --no-verify) DO_VERIFY=0; shift ;;
        --no-global) DO_GLOBAL=0; shift ;;
        --no-merge) DO_MERGE=0; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; while [ $# -gt 0 ]; do ARGFILES+=("$1"); shift; done ;;
        -*) echo "cstatic: unknown option $1" >&2; usage >&2; exit 2 ;;
        *) ARGFILES+=("$1"); shift ;;
    esac
done

# ------------------------------------------------------------- sanity check

if [ ! -x "$CLAUDE" ]; then
    if command -v claude >/dev/null 2>&1; then
        CLAUDE="$(command -v claude)"
    else
        echo "cstatic: cannot find the claude executable at '$CLAUDE'" >&2
        echo "cstatic: set CSTATIC_CLAUDE to its full path" >&2
        exit 1
    fi
fi

TIMEOUT_CMD=""
command -v timeout >/dev/null 2>&1 && TIMEOUT_CMD="timeout ${JOB_TIMEOUT}s"

# -------------------------------------------------- work out what to analyze

ROOT=""
declare -a FILES=()

if [ ${#ARGFILES[@]} -eq 0 ]; then
    ROOT="$PWD"
elif [ ${#ARGFILES[@]} -eq 1 ] && [ -d "${ARGFILES[0]}" ]; then
    ROOT="$(cd "${ARGFILES[0]}" && pwd)"
else
    ROOT="$PWD"
    for f in "${ARGFILES[@]}"; do
        if [ ! -f "$f" ]; then
            echo "cstatic: no such file: $f" >&2
            exit 1
        fi
        FILES+=("$(cd "$(dirname "$f")" && pwd)/$(basename "$f")")
    done
fi

# .cstaticignore
if [ -f "$ROOT/.cstaticignore" ]; then
    while IFS= read -r pat || [ -n "$pat" ]; do
        case "$pat" in ""|\#*) continue ;; esac
        EXCLUDES+=("$pat")
    done < "$ROOT/.cstaticignore"
fi

is_excluded() {
    # $1 = path relative to ROOT
    local rel="$1" base
    base="$(basename "$rel")"
    local pat
    for pat in ${EXCLUDES[@]+"${EXCLUDES[@]}"}; do
        # shellcheck disable=SC2053
        [[ "$base" == $pat ]] && return 0
        # shellcheck disable=SC2053
        [[ "$rel" == $pat ]] && return 0
        [[ "$rel" == $pat/* ]] && return 0
    done
    return 1
}

if [ ${#FILES[@]} -eq 0 ]; then
    if [ "$RECURSE" -eq 1 ]; then
        FINDARGS=(-type f)
    else
        FINDARGS=(-maxdepth 1 -type f)
    fi
    while IFS= read -r f; do
        rel="${f#"$ROOT"/}"
        is_excluded "$rel" && continue
        FILES+=("$f")
    done < <(find "$ROOT" "${FINDARGS[@]}" \( -name '*.c' -o -name '*.h' \) \
                  -not -path '*/.git/*' | LC_ALL=C sort)
fi

if [ ${#FILES[@]} -eq 0 ]; then
    echo "cstatic: no .c or .h files found in $ROOT"
    exit 0
fi

# ------------------------------------------------------------ work directory

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cstatic.XXXXXX")"
cleanup() {
    if [ "$KEEP_TMP" -eq 1 ]; then
        echo "cstatic: work directory kept at $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
mkdir -p "$WORK/prompts" "$WORK/raw" "$WORK/logs" "$WORK/verified"

note() { [ "$QUIET" -eq 1 ] || echo "$*"; }

START_TS=$(date +%s)

# ------------------------------------------------------------ shared prompt

# Everything both passes need to know about what a real bug is.
BUG_CATALOG=$(cat <<'EOF'
Memory and pointers
  - writing or reading past the end of an array or malloc'd block: off-by-one,
    a `<=` where `<` belongs, an index used before it is range-checked, a
    length that forgets the room for the NUL terminator
  - sizeof(pointer) where sizeof(array) was meant; sizeof the wrong type in a
    malloc/calloc/memset/memcpy size expression
  - strcpy / strcat / sprintf / scanf %s into a buffer that a realistic input
    can overrun
  - strncpy leaving the destination unterminated; strncat's size argument
    misunderstood (it is the space remaining, not the buffer size)
  - use after free, double free, freeing a pointer that did not come from
    malloc, freeing an interior pointer
  - realloc's result assigned over the only copy of the old pointer (the block
    leaks and is lost when realloc fails), or a stale alias kept to a block
    that realloc may have moved
  - a malloc'd block or an open FILE* leaked on an early-return / error path
  - returning or storing the address of a local (stack) object
  - memcpy on regions that can overlap (memmove is required)
  - reading an uninitialized variable, struct field, or array element,
    including structs only partially filled in by an init function
  - pointer arithmetic with the wrong element type, or a byte offset scaled
    twice

Integers and arithmetic
  - a signed/unsigned comparison or conversion that flips a bounds check
    (int i compared against an unsigned or size_t length; a negative value
    that becomes huge)
  - a negative value used as an index, a size, or a loop count
  - truncation or overflow because the expression is computed in int and only
    then widened or stored (long total = a * b; with int a, b)
  - integer division where floating point was intended
  - shift by a count >= the type's width, or shift of a negative signed value
  - a plain char passed to an isalpha/toupper/... macro, or compared against a
    value above 127, where signedness matters
  - float or double compared with == in a place where rounding makes it fail

Control flow and logic
  - `=` where `==` was meant, or `&`/`|` where `&&`/`||` was meant
  - operator precedence mistakes: a & b == c, *p++ vs (*p)++, x << 1 + 2,
    a missing paren around a ternary
  - a stray `;` right after if / while / for
  - a switch case that falls through where it clearly should not, or a missing
    default that drops data on the floor
  - a condition that is always true or always false, or an else-if branch that
    can never be reached
  - a loop whose induction variable is not advanced on some path, advanced
    twice, or whose termination test can be stepped over
  - the wrong index variable used in a nested loop (i where j was meant) --
    the classic copy-paste bug
  - an inner declaration shadowing an outer one so an intended write lands on
    the wrong object
  - a non-void function that can fall off the end without returning a value
  - an inverted or off-by-one comparison in a bounds or termination check
  - a break/continue that exits the wrong level of nesting

Library use
  - printf/scanf conversion specifiers that do not match the argument types
    (%d for a long or size_t, %f for a float in scanf, %s for a non-string),
    a missing & in scanf, a missing field width on %s
  - ignoring the return value of fread/fwrite/fgets/snprintf where a short
    read or truncation changes behaviour
  - atoi/strtol error cases silently yielding 0
  - passing a buffer that is not NUL-terminated to a str* function
  - a qsort/bsearch comparator that returns a - b on values that can overflow,
    or that is not a consistent total order
  - fopen mode that does not match how the data is read or written
  - a function returning a pointer to a static buffer, whose caller holds it
    across another call to the same function

Macros and headers
  - a macro parameter or the macro body not parenthesized
  - a macro that evaluates an argument more than once, invoked with a
    side-effecting or expensive argument
  - a manifest constant or enum that disagrees with the array size it is
    supposed to describe
  - the same struct, enum or constant declared differently in two places
  - a prototype in a header that does not match the definition (argument
    count, types, or the presence of a return value)

Undefined behaviour
  - an object modified twice between sequence points (i = i++, a[i] = i++)
  - reliance on the evaluation order of function arguments or of operands
  - type punning through a cast to an incompatible pointer type
  - calling through a function pointer of the wrong type
  - division or modulo by a value that can be zero

State and lifetime
  - global or static state that silently assumes a particular call order, or
    an init that must run first and can be skipped
  - an "owner" that frees something another module still points at, or two
    owners that both free it
EOF
)

NOT_A_FINDING=$(cat <<'EOF'
  - style, naming, indentation, comment quality, missing const or static
  - "consider refactoring", performance advice, or C99/C11 modernization --
    this code is deliberately C89 and its style is deliberate
  - portability to platforms other than a normal 32- or 64-bit machine
  - a missing NULL check after malloc, UNLESS the requested size is large or
    derived from input data
  - anything you cannot tie to a concrete failing execution that you can
    describe in one sentence
  - anything the compiler would reject or obviously warn about: this code
    already compiles cleanly and runs correctly in ordinary use, so an
    "obvious" bug is almost certainly you misreading the code
  - the same root cause reported at several different lines: report it once,
    at the single line the programmer should look at first
  - a defect in code you did not actually read the definition of
EOF
)

OUTPUT_CONTRACT=$(cat <<'EOF'
Emit each finding as exactly this block, and emit nothing else at all --
no preamble, no markdown, no code fences, no closing summary:

<<<FINDING>>>
FILE: path/of/the/file/containing/the/bug.c
LINE: 199
COL: 8
SEV: error
MSG: one line, under 95 characters, lower case like a gcc diagnostic
DETAIL: first explanation line, under 88 characters
DETAIL: second explanation line (optional)
DETAIL: third explanation line (optional)
<<<END>>>

Field rules:
  FILE  the file the bug is IN, which may be a header rather than the file you
        were asked to focus on.  Write it exactly as it appears relative to the
        project root, or as an absolute path.
  LINE  the exact line the programmer should jump to.  Line numbers come from
        the Read tool's numbering and must be re-checked against it -- a wrong
        line number makes the finding useless.
  COL   a 1-based column, or 1 if you are not sure.
  SEV   "error" for memory corruption, a crash, a leak, or a plainly wrong
        result; "warning" for a real bug whose effect is conditional or minor.
  MSG   what is wrong, concretely.  Name the variable, buffer, or expression.
  DETAIL  at most 3 lines.  Say the concrete execution that goes wrong ("when
        the input line is longer than 31 chars ..."), cite the fact you looked
        up ("buf is char[32], declared at world.h@44"), and, if it fits in the
        remaining room, the fix.  Write any location you cite inside a MSG or
        DETAIL line in the form "world.h@44", never "world.h:44" -- a colon
        there would be mistaken for a second finding.

If, after honestly looking, you have no finding that survives your own
attempt to refute it, emit exactly this single line and nothing else:

<<<NOFINDINGS>>>
EOF
)

SYSTEM_APPEND="You are running as the analysis backend of a non-interactive static-analysis tool. Your entire stdout is parsed by a script. Emit ONLY the finding blocks that the user prompt specifies, in exactly the specified format: no preamble, no commentary, no markdown fences, no closing summary. You have read-only tools; never attempt to modify, create, or delete any file."

# ------------------------------------------------------------- claude runner

run_claude() {
    # $1 prompt file, $2 stdout file, $3 log file
    # shellcheck disable=SC2086
    ( cd "$ROOT" && $TIMEOUT_CMD "$CLAUDE" -p \
        --dangerously-skip-permissions \
        --tools "Read,Grep,Glob" \
        --model "$MODEL" \
        --effort "$EFFORT" \
        --disable-slash-commands \
        --no-session-persistence \
        --output-format text \
        --append-system-prompt "$SYSTEM_APPEND" \
    ) < "$1" > "$2" 2> "$3"
}

# ------------------------------------------------------- build analysis jobs

declare -a JOB_ID=() JOB_LABEL=()
job_n=0

add_analysis_job() {
    # $1 abs file, $2 rel file, $3 first line, $4 last line, $5 total lines
    local abs="$1" rel="$2" lo="$3" hi="$4" total="$5"
    local id label region
    job_n=$((job_n + 1))
    id=$(printf "a%03d" "$job_n")
    if [ "$lo" -eq 1 ] && [ "$hi" -ge "$total" ]; then
        region="the entire file (lines 1 to $total)"
        label="$rel"
    else
        region="lines $lo through $hi of this $total-line file"
        label="$rel lines $lo-$hi"
    fi

    cat > "$WORK/prompts/$id.txt" <<EOF
You are a senior C programmer auditing a C89 code base for latent bugs.  You
cannot compile or run anything; you read code.

PROJECT ROOT: $ROOT
TARGET FILE:  $rel
FOCUS REGION: $region

What you may assume, because it is true:
  - The project already compiles cleanly and behaves correctly in ordinary use.
  - So the bugs that remain are the subtle ones: the path that is rarely taken,
    the buffer that is only just big enough, the index that is off by one at
    one boundary, the two declarations that drifted apart.
  - The code is C89 on purpose.  Do not suggest C99 or C11 features.

How to work:
  1. Read the target file with the Read tool.  Its line numbers are the only
     line numbers you may quote.
  2. Use Grep and Glob to look up the ground truth for everything the focus
     region touches: the real size of every array, the real type of every
     struct field, the real expansion of every macro, the real contract and
     failure mode of every function it calls, every other place a global is
     written.  NEVER guess any of these -- if you did not read it, you may not
     report a bug that depends on it.
  3. Where the focus region is part of a larger file, still read enough of the
     rest of the file to know what the surrounding function does.
  4. For every candidate bug, spend real effort trying to prove yourself WRONG.
     Write out, in your head, a concrete execution: which call, which input,
     which sequence of events makes the program misbehave, and what the wrong
     behaviour is.  If you cannot construct that execution, or if some check
     elsewhere already prevents it, DISCARD the candidate.  A false alarm costs
     the human more than a missed bug.
  5. Report the survivors, most serious first, at most $MAX_FINDINGS of them.
     Reporting three real bugs beats reporting three real bugs and seven guesses.

WHAT COUNTS AS A FINDING
$BUG_CATALOG

WHAT IS NOT A FINDING
$NOT_A_FINDING

OUTPUT FORMAT
$OUTPUT_CONTRACT
EOF

    JOB_ID+=("$id")
    JOB_LABEL+=("$label")
}

for abs in "${FILES[@]}"; do
    rel="${abs#"$ROOT"/}"
    total=$(wc -l < "$abs" | tr -d ' ')
    [ -z "$total" ] && total=0
    if [ "$total" -eq 0 ]; then
        continue
    fi
    if [ "$CHUNK_LINES" -le 0 ] || [ "$total" -le "$CHUNK_LINES" ]; then
        add_analysis_job "$abs" "$rel" 1 "$total" "$total"
    else
        step=$((CHUNK_LINES - CHUNK_OVERLAP))
        [ "$step" -lt 1 ] && step=$CHUNK_LINES
        lo=1
        while [ "$lo" -le "$total" ]; do
            hi=$((lo + CHUNK_LINES - 1))
            [ "$hi" -gt "$total" ] && hi=$total
            add_analysis_job "$abs" "$rel" "$lo" "$hi" "$total"
            [ "$hi" -ge "$total" ] && break
            lo=$((lo + step))
        done
    fi
done

# ------------------------------------------------- whole-project (global) job

if [ "$DO_GLOBAL" -eq 1 ] && [ ${#FILES[@]} -gt 1 ]; then
    job_n=$((job_n + 1))
    gid=$(printf "a%03d" "$job_n")
    {
        cat <<EOF
You are a senior C programmer auditing a C89 code base.  Other analysts are
reading each file in isolation.  Your job is the one thing they cannot do:
find the bugs that live BETWEEN files, in the seams where two pieces of the
program disagree with each other.

PROJECT ROOT: $ROOT
FILES IN THIS ANALYSIS:
EOF
        for abs in "${FILES[@]}"; do echo "  ${abs#"$ROOT"/}"; done
        cat <<EOF

Look specifically for:
  - a prototype in a header that disagrees with the definition in the .c file:
    argument count, argument types, return type, or pointer-vs-value
  - a struct, union or enum defined differently in two translation units, or a
    struct whose field order one file assumes and another does not
  - a manifest constant, enum, or #define that must agree with an array
    dimension, a table size, a switch, or a file format -- and does not
  - a table, switch, or if-chain that must be kept in step with an enum, and
    has fallen behind it (a new enum value with no corresponding case or row)
  - ownership and lifetime contracts across module boundaries: who mallocs and
    who frees, a pointer handed out that the owner later frees or reallocs, a
    pointer to a static buffer held across the next call
  - initialization order: a module that must be initialized before another is
    used, where some path can skip or reverse that
  - a global written by one file and read by another with a different
    assumption about when it is valid, or a shared global with two owners
  - units and encodings that disagree at an interface: bytes vs elements,
    seconds vs milliseconds, inclusive vs exclusive end, 0-based vs 1-based,
    normalized vs raw
  - a function whose documented or implied error return is ignored at every
    call site
  - two copies of the same logic that have drifted apart

Method: use Grep and Glob aggressively to find every declaration, definition,
and call site of anything you suspect.  Read them.  Do not report a mismatch
you have not read both sides of.  Then try to refute yourself: is there some
cast, macro, or check elsewhere that makes it fine after all?  If so, drop it.
Report at most $MAX_FINDINGS findings, most serious first.  Report each at the
line the programmer should look at first.

WHAT IS NOT A FINDING
$NOT_A_FINDING

OUTPUT FORMAT
$OUTPUT_CONTRACT
EOF
    } > "$WORK/prompts/$gid.txt"
    JOB_ID+=("$gid")
    JOB_LABEL+=("<whole project: cross-file consistency>")
fi

TOTAL_JOBS=${#JOB_ID[@]}

# ------------------------------------------------------------- run the jobs

note "cstatic v$VERSION -- ${#FILES[@]} source file(s) in $ROOT"
note "cstatic: model=$MODEL effort=$EFFORT jobs=$JOBS  ($TOTAL_JOBS analysis pass(es))"
note ""

# set by run_pool: how many jobs of the last pool produced nothing at all.
# A run where every job died must never be reported as "no findings".
POOL_FAILED=0
POOL_TOTAL=0

run_pool() {
    # runs prompts listed in $1 (id per line), with progress prefix $2
    local listfile="$1" phase="$2"
    local -a queued=()
    local total_n running=0 i=0
    total_n=$(wc -l < "$listfile" | tr -d ' ')
    while IFS=$'\t' read -r id label; do
        i=$((i + 1))
        run_claude "$WORK/prompts/$id.txt" "$WORK/raw/$id.out" "$WORK/logs/$id.log" &
        queued+=("$id:$i:$label")
        running=$((running + 1))
        if [ "$running" -ge "$JOBS" ]; then
            wait -n 2>/dev/null
            running=$((running - 1))
        fi
    done < "$listfile"
    wait

    # report results in submission order
    local rec rest jid jlabel jidx n
    POOL_FAILED=0
    POOL_TOTAL="$total_n"
    for rec in ${queued[@]+"${queued[@]}"}; do
        jid="${rec%%:*}"; rest="${rec#*:}"
        jidx="${rest%%:*}"; jlabel="${rest#*:}"
        if [ ! -s "$WORK/raw/$jid.out" ]; then
            POOL_FAILED=$((POOL_FAILED + 1))
            note "cstatic: [$jidx/$total_n] $phase $jlabel -- FAILED (no output, see log)"
            if [ "$QUIET" -eq 0 ] && [ -s "$WORK/logs/$jid.log" ]; then
                # strip anything emacs could mistake for a source location
                sed -e 's/:\([0-9]\)/ \1/g' -e 's/^/cstatic:     /' \
                    "$WORK/logs/$jid.log" | head -4
            fi
            continue
        fi
        n=$(grep -c '^<<<FINDING>>>' "$WORK/raw/$jid.out" 2>/dev/null)
        [ -z "$n" ] && n=0
        note "cstatic: [$jidx/$total_n] $phase $jlabel -- $n candidate(s)"
    done
}

: > "$WORK/joblist.txt"
for k in "${!JOB_ID[@]}"; do
    printf '%s\t%s\n' "${JOB_ID[$k]}" "${JOB_LABEL[$k]}" >> "$WORK/joblist.txt"
done

run_pool "$WORK/joblist.txt" "read"
READ_FAILED="$POOL_FAILED"
VERIFY_FAILED=0

# If nothing ran, the folder has not been analyzed.  Saying "no findings" here
# would be a lie that reads exactly like a clean bill of health.
if [ "$READ_FAILED" -ge "$TOTAL_JOBS" ]; then
    echo ""
    echo "cstatic: every analysis job failed -- nothing was analyzed."
    echo "cstatic: this is a tool failure, not a clean result."
    if [ "$KEEP_TMP" -eq 0 ]; then
        echo "cstatic: re-run with -k to keep the logs in the work directory."
    fi
    exit 1
fi

# ------------------------------------------------- parse + normalize + dedupe

# Turns <<<FINDING>>> blocks into tab separated records:
#   file <TAB> line <TAB> col <TAB> sev <TAB> msg <TAB> detail (\x01 separated)
PARSER='
function flush(   key) {
  if (!inblock) return
  inblock = 0
  if (file == "" || line !~ /^[0-9]+$/) return
  if (msg == "") return
  # normalize the path
  sub("^" ROOTQ "/", "", file)
  sub("^\\./", "", file)
  if (col !~ /^[0-9]+$/ || col+0 < 1) col = 1
  if (sev != "error" && sev != "warning") sev = "warning"
  printf "%s\t%s\t%s\t%s\t%s\t%s\n", file, line+0, col+0, sev, msg, detail
}
BEGIN { inblock = 0 }
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

cat "$WORK"/raw/*.out 2>/dev/null \
    | awk -v ROOTQ="$ROOT" "$PARSER" > "$WORK/candidates.raw.tsv"

# Dedupe on file+line, keeping the record with the longest explanation
# (chunk overlap and the global pass both produce near-duplicates).
sort -t$'\t' -k1,1 -k2,2n "$WORK/candidates.raw.tsv" 2>/dev/null | awk -F'\t' '
{
  key = $1 "\t" $2
  if (!(key in best) || length($0) > length(best[key])) best[key] = $0
  if (!(key in ord)) { ord[key] = ++n; keys[n] = key }
}
END { for (i = 1; i <= n; i++) print best[keys[i]] }
' > "$WORK/candidates.tsv"

NCAND=$(wc -l < "$WORK/candidates.tsv" | tr -d ' ')
[ -z "$NCAND" ] && NCAND=0

note ""
note "cstatic: $NCAND candidate finding(s) after dedupe"

if [ "$NCAND" -eq 0 ]; then
    note ""
    echo "cstatic: no findings."
    if [ "$READ_FAILED" -gt 0 ]; then
        echo "cstatic: WARNING -- $READ_FAILED of $TOTAL_JOBS analysis job(s)" \
             "failed, so part of the code was never read."
    fi
    ELAPSED=$(( $(date +%s) - START_TS ))
    note "cstatic: done in ${ELAPSED}s."
    exit 0
fi

# ------------------------------------------------------------- verify pass

if [ "$DO_VERIFY" -eq 0 ]; then
    cp "$WORK/candidates.tsv" "$WORK/final.tsv"
else
    note "cstatic: verifying (each candidate gets a fresh, skeptical reader)"
    note ""

    # group candidates by the file they point INTO
    cut -f1 "$WORK/candidates.tsv" | LC_ALL=C sort -u > "$WORK/targets.txt"

    : > "$WORK/vjoblist.txt"
    vn=0
    while IFS= read -r tgt; do
        vn=$((vn + 1))
        vid=$(printf "v%03d" "$vn")
        awk -F'\t' -v t="$tgt" -v OFS='\t' '$1 == t' "$WORK/candidates.tsv" \
            > "$WORK/raw/$vid.cands.tsv"

        {
            cat <<EOF
Another analyst has proposed the bugs listed below in a C89 code base.  The
analyst was working quickly and had incomplete context.  Your job is the
opposite of theirs: you are the skeptic, and you must try to REFUTE each claim.

PROJECT ROOT: $ROOT
FILE UNDER REVIEW: $tgt

This code already compiles cleanly and runs correctly in ordinary use, so the
prior probability that any given claim is wrong is high.  The most common
failure modes of the analyst you are checking are:
  - it guessed at an array size, a struct field type, a macro expansion, or a
    callee's behaviour instead of reading it
  - it missed a check, clamp, early return, or invariant established earlier in
    the function or by the only caller
  - it reported a theoretical undefined behaviour that cannot actually be
    reached by any input the program accepts
  - it misread C89 operator precedence, integer promotion, or pointer arithmetic
  - its line number is off, or points at a symptom rather than the defect
  - it reported the same root cause more than once

For EACH candidate below:
  1. Read the cited line and the whole function containing it.
  2. Read the actual definition of every array, struct, macro, constant, and
     called function the claim depends on.  Grep for every caller of the
     function, so you know what values can really arrive.
  3. Give it one of three verdicts.

     REACHABLE  -- the defect is real, and some execution the program can
       actually perform today reaches it.  You can name it: these arguments,
       this input, this call sequence.  Emit the block with SEV: error.

     LATENT -- the defect is real in the code as written, but nothing in the
       program as it stands drives it there yet.  The failing path needs a
       caller that does not exist, an allocation that fails, an input no
       current call site can produce, or a function nobody calls yet.  This is
       still worth reporting: it is exactly the bug that bites when someone
       adds the next caller.  Emit the block with SEV: warning, and say in a
       DETAIL line what would have to happen to trigger it.

     WRONG -- emit nothing at all.  Use this, and only this, when the claim is
       not a defect: a check, clamp, invariant or early return elsewhere
       prevents it; the analyst misread C89 precedence, integer promotion, or
       pointer arithmetic; the analyst guessed at a size, type, or macro and
       guessed wrong; or the construct is simply correct.

     The line between LATENT and WRONG is the important one.  "Real bug that
     has not bitten yet" is LATENT, never WRONG.  "I cannot tell whether this
     is a bug" is WRONG -- when you genuinely cannot decide, drop it.

  4. Drop any candidate that duplicates the root cause of another candidate you
     are keeping; keep only the one at the line where the fix belongs.
  5. If the claim is real but the line number is wrong, keep it with the
     corrected line number.
  6. If the claim is real but the wording is vague, keep it and rewrite MSG and
     DETAIL so they name the variable and the triggering condition.

CANDIDATES:

EOF
            awk -F'\t' '{
                printf "[%s]\n", NR
                printf "  FILE: %s\n  LINE: %s\n  COL: %s\n  SEV: %s\n  MSG: %s\n", $1, $2, $3, $4, $5
                n = split($6, d, "\001")
                for (i = 1; i <= n; i++) if (d[i] != "") printf "  DETAIL: %s\n", d[i]
                printf "\n"
            }' "$WORK/raw/$vid.cands.tsv"

            cat <<EOF

OUTPUT FORMAT

Emit one block for each candidate you judged REACHABLE or LATENT, and nothing
at all for the ones you judged WRONG.  Emit nothing else -- no preamble, no
markdown, no summary, no explanation of your rejections:

<<<FINDING>>>
FILE: $tgt
LINE: 199
COL: 8
SEV: error
MSG: one line, under 95 characters, lower case like a gcc diagnostic
DETAIL: the concrete execution that goes wrong, under 88 characters
DETAIL: the fact you verified, e.g. "buf is char[32], world.h@44" (optional)
DETAIL: the fix, if it fits (optional)
<<<END>>>

SEV is "error" for REACHABLE and "warning" for LATENT.

The LINE must be re-checked against the Read tool's numbering; it is the line
the programmer will jump to.  At most 3 DETAIL lines.  Write any location you
cite inside a MSG or DETAIL line as "world.h@44", never "world.h:44".

If every candidate is WRONG, emit exactly this single line and nothing else:

<<<NOFINDINGS>>>
EOF
        } > "$WORK/prompts/$vid.txt"

        printf '%s\t%s\n' "$vid" "$tgt" >> "$WORK/vjoblist.txt"
    done < "$WORK/targets.txt"

    run_pool "$WORK/vjoblist.txt" "verify"
    VERIFY_FAILED="$POOL_FAILED"

    # A verifier that died rejects everything it was given, silently.  Rather
    # than lose those candidates, fall back to reporting them unverified.
    if [ "$VERIFY_FAILED" -gt 0 ]; then
        echo "cstatic: WARNING -- $VERIFY_FAILED verification job(s) failed;" \
             "their candidates are reported below unverified."
    fi

    cat "$WORK"/raw/v*.out 2>/dev/null \
        | awk -v ROOTQ="$ROOT" "$PARSER" > "$WORK/verified.tsv"

    # keep the unverified candidates of any verifier that died
    while IFS=$'\t' read -r vid tgt; do
        if [ ! -s "$WORK/raw/$vid.out" ] && [ -s "$WORK/raw/$vid.cands.tsv" ]; then
            cat "$WORK/raw/$vid.cands.tsv" >> "$WORK/verified.tsv"
        fi
    done < "$WORK/vjoblist.txt"

    sort -t$'\t' -k1,1 -k2,2n "$WORK/verified.tsv" \
        | awk -F'\t' '
            { key = $1 "\t" $2
              if (!(key in best) || length($0) > length(best[key])) best[key] = $0
              if (!(key in ord)) { ord[key] = ++n; keys[n] = key } }
            END { for (i = 1; i <= n; i++) print best[keys[i]] }
          ' > "$WORK/final.tsv"
fi

# -------------------------------------------------------------- merge pass

# Verification is grouped by the file a finding points into, so two verifiers
# never see each other's work.  One root cause reported at both the defect and
# its call site therefore survives twice.  One last cheap pass collapses those.

NPRE=$(wc -l < "$WORK/final.tsv" | tr -d ' ')
[ -z "$NPRE" ] && NPRE=0

if [ "$DO_MERGE" -eq 1 ] && [ "$NPRE" -gt 1 ]; then
    note ""
    note "cstatic: collapsing findings that share one root cause"
    {
        cat <<EOF
Below is the final list of bugs found in a C89 code base.  Each was already
verified independently.  They were verified one source file at a time, so the
same root cause can appear more than once: typically once at the line where
the defect lives and once at a call site that triggers it, or twice with
different wording.

Your only job is to collapse those.  For each group of entries that describe
ONE underlying defect -- one thing the programmer would fix with one edit --
keep exactly one entry and drop the rest.  Keep the entry at the line where
the fix belongs, which is usually the defect itself rather than the caller
that happens to trigger it.  Read the code if you need to decide.

Do NOT drop an entry just because it is in the same function, or the same
area, or is less interesting than another.  Two genuinely different defects
that happen to be adjacent are two entries.  When in doubt, keep both.

FINDINGS:

EOF
        awk -F'\t' '{
            printf "[%d] %s line %s  (%s)\n      %s\n", NR, $1, $2, $4, $5
            n = split($6, d, "\001")
            for (i = 1; i <= n; i++) if (d[i] != "") printf "      . %s\n", d[i]
            printf "\n"
        }' "$WORK/final.tsv"

        cat <<'EOF'

OUTPUT FORMAT

Emit one line for each entry to DROP, and nothing else at all -- no preamble,
no markdown, no summary:

DROP: 7 duplicate of 3, same strcpy overflow seen from the call site

If nothing should be dropped, emit exactly this single line and nothing else:

DROP: none
EOF
    } > "$WORK/prompts/m001.txt"

    run_claude "$WORK/prompts/m001.txt" "$WORK/raw/m001.out" "$WORK/logs/m001.log"

    if [ -s "$WORK/raw/m001.out" ]; then
        grep -oE '^[[:space:]]*DROP:[[:space:]]*[0-9]+' "$WORK/raw/m001.out" \
            2>/dev/null | grep -oE '[0-9]+' | LC_ALL=C sort -u > "$WORK/drop.txt"
        NDROP=$(wc -l < "$WORK/drop.txt" | tr -d ' ')
        [ -z "$NDROP" ] && NDROP=0
        # never let a confused merge pass throw the whole report away
        if [ "$NDROP" -gt 0 ] && [ "$NDROP" -lt "$NPRE" ]; then
            awk 'NR == FNR { drop[$1 + 0] = 1; next }
                 !(FNR in drop)' "$WORK/drop.txt" "$WORK/final.tsv" \
                 > "$WORK/merged.tsv"
            mv "$WORK/merged.tsv" "$WORK/final.tsv"
            note "cstatic: merged $NDROP duplicate finding(s)"
        else
            note "cstatic: no duplicates to merge"
        fi
    else
        note "cstatic: merge pass produced no output, keeping all findings"
    fi
fi

# ------------------------------------------------------------ print findings

NFINAL=$(wc -l < "$WORK/final.tsv" | tr -d ' ')
[ -z "$NFINAL" ] && NFINAL=0

note ""
note "cstatic: ---------------------------------------------------------------"
note ""

sort -t$'\t' -k1,1 -k2,2n "$WORK/final.tsv" | awk -F'\t' '
# Emacs compilation-mode happily parses a "world.c:32" -- or a "world.c line
# 32", or a "world.c, line 32" -- that appears in the middle of an explanation,
# turning one finding into two jump targets and doubling the list the human has
# to page through.  Rewrite every such citation into "world.c@32", which stays
# readable but which no compilation-mode rule recognizes as a location.
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
      # "world.c:32:5" -- absorb the column too
      if (match(s, /^:[0-9]/)) { s = "." substr(s, 2) }
    }
    else { out = out m }
  }
  out = out s
  # any colon still glued to a digit could start a location; break it up
  while (match(out, /:[0-9]/)) {
    out = substr(out, 1, RSTART - 1) " " substr(out, RSTART + 1)
  }
  return out
}
function emit_wrapped(text,    words, i, n, cur) {
  n = split(text, words, " ")
  cur = ""
  for (i = 1; i <= n; i++) {
    if (cur == "") { cur = words[i] }
    else if (length(cur) + 1 + length(words[i]) <= 74) { cur = cur " " words[i] }
    else { if (out < 4) { print "    " cur; out++ }; cur = words[i] }
  }
  if (cur != "" && out < 4) { print "    " cur; out++ }
}
{
  printf "%s:%s:%s: %s: %s\n", $1, $2, $3, $4, sanitize($5)
  out = 0
  n = split($6, d, "\001")
  for (i = 1; i <= n; i++) if (d[i] != "") emit_wrapped(sanitize(d[i]))
  print ""
}
'

ELAPSED=$(( $(date +%s) - START_TS ))
NERR=$(awk -F'\t' '$4 == "error"' "$WORK/final.tsv" | wc -l | tr -d ' ')
NWARN=$(awk -F'\t' '$4 != "error"' "$WORK/final.tsv" | wc -l | tr -d ' ')

if [ "$NFINAL" -eq 0 ]; then
    echo "cstatic: no findings survived verification."
else
    echo "cstatic: $NFINAL finding(s): $NERR error(s), $NWARN warning(s)."
fi
if [ "$READ_FAILED" -gt 0 ]; then
    echo "cstatic: WARNING -- $READ_FAILED of $TOTAL_JOBS analysis job(s)" \
         "failed, so part of the code was never read."
fi
note "cstatic: analyzed ${#FILES[@]} file(s) in ${ELAPSED}s."
note "cstatic: these are suggestions from a language model -- verify each one."

exit 0
