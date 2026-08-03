# cstatic

Claude-powered static analysis for C89 projects, with GCC-style output that
Emacs `M-x compile` can page through with `C-x \``.

It never compiles or runs your code.  It reads it.

    game.c:199:8: error: strcpy into name[16] overruns when inName is longer
        addItem() copies an arbitrary caller string into Item.name, which is
        char[16] (world.h:10).  main.c:12 passes a 25-char literal, so this
        already corrupts the next item's value field.  Use strncpy + NUL.

## The idea

You already compiled the program.  You already ran it.  It works, as far as
you can tell.  What is left is the off-by-one that only bites at one boundary,
the `realloc` whose old pointer is lost on failure, the `i` that should have
been a `j`, the two headers that quietly disagree about an array size.

Compilers do not find those.  A careful reader does.  `cstatic` puts a careful
reader on every file in the folder, in parallel, and then puts a second,
skeptical reader on every claim the first one made.

## Install

Nothing to install beyond the Claude Code CLI.  The script looks for it at
`~/.local/bin/claude`, or on your `PATH`, or wherever `CSTATIC_CLAUDE` points.

    chmod +x cstatic.sh

## Use it from Emacs

From a buffer in the project you want analyzed:

    M-x compile RET ~/checkout/generatedTools/cstatic/cstatic.sh RET

Then walk away.  A few minutes later (longer for a big project) come back to
the `*compilation*` buffer and use the ordinary keys:

| key | does |
|-----|------|
| `C-x \`` | jump to the next finding |
| `M-g M-p` / `M-g M-n` | previous / next finding |
| `RET` on a line | jump to that finding |

`error:` means the bug is reachable today; `warning:` means it is real but
nothing currently drives the program into it.  Set `compilation-skip-threshold`
to 2 if you want `C-x \`` to visit only the errors.

Findings are printed as a GCC diagnostic line plus up to four indented
explanation lines, so a finding fits in a half-height compilation window and
the explanation lines are not mistaken for separate errors.

To make it the default for a project, put this in a `.dir-locals.el`:

```elisp
((c-mode . ((compile-command . "~/checkout/generatedTools/cstatic/cstatic.sh "))))
```

## Use it from a shell

    ./cstatic.sh                  # analyze the current directory
    ./cstatic.sh ~/src/mygame     # analyze another directory
    ./cstatic.sh game.c world.h   # analyze just these files
    ./cstatic.sh -f world.h       # just the one file you were working on
    ./cstatic.sh -f 'net*.c'      # ...or the few that match a glob
    ./cstatic.sh -r               # recurse into subdirectories
    ./cstatic.sh -x 'stb_*.c' -x imgui   # skip vendored code
    ./cstatic.sh -m sonnet -j 8   # cheaper and wider
    ./cstatic.sh --no-verify      # skip the skeptical pass: faster, noisier

`./cstatic.sh -h` lists every option.

### Looking at just one file

`-f` is the opposite of `-x`: instead of naming what to skip, it names the
only thing to search.  Wrote a new header and want a second pair of eyes on
just that, without paying to re-audit the whole folder?

    ./cstatic.sh -f world.h

Claude still **reads** the rest of the project — that is how it knows the real
size of an array or what a callee actually does, and the accuracy of the whole
tool depends on it.  It just does not go hunting for bugs anywhere else.

Quote the glob (`-f '*.h'`) so your shell does not expand it first.  `-f` is
repeatable, and it matches on the bare file name or on the path relative to
the analyzed folder.  A glob that matches nothing is an error rather than an
empty report, because a typo should not look like a clean bill of health.

Because `-f` means "do not scan the whole folder", it turns off the cross-file
pass.  Add `--global` to keep it — the pass then sees the entire project but
is told to concentrate on the seams where your `-f` files meet the rest.

## How it works

1. **Collect.**  Every `.c` and `.h` file in the folder (top level only unless
   you pass `-r`), minus anything matched by `-x` or by a `.cstaticignore`
   file in the folder.

2. **Read.**  One Claude job per file.  Files longer than 600 lines are split
   into overlapping chunks so that attention stays on the code rather than
   spreading thin over thousands of lines; each chunk still gets the whole
   project to grep through.  The prompt hands Claude a long catalog of the bug
   classes that actually survive compilation in C89, tells it the code already
   works so anything "obvious" is probably a misreading, and requires it to
   look up the real size of every array and the real body of every callee
   rather than guessing.

3. **Cross-file pass.**  One extra job looks only at the seams between files:
   prototypes that drifted from definitions, an enum that outgrew the switch
   that handles it, a constant that no longer matches the array it describes,
   who mallocs and who frees, units that disagree at an interface.

4. **Refute.**  Every candidate goes to a fresh Claude that has not seen the
   first one's reasoning and is asked to *disprove* it: read the whole
   function, read the actual declarations, find the callers, and see whether
   something already prevents the failure.  It returns one of three verdicts,
   and this is where most of the noise dies:

   - **reachable** -- real, and some execution the program can perform today
     gets there.  Printed as `error:`.
   - **latent** -- real in the code as written, but nothing drives it there
     yet: it needs a caller that does not exist, an allocation failure, or an
     input no current call site produces.  Printed as `warning:`, with a line
     saying what would have to happen.  These are the ones that bite when
     somebody adds the next caller.
   - **wrong** -- not a defect at all.  Dropped silently.

5. **Merge.**  Verification happens one file at a time, so a single root cause
   reported both at the defect and at the call site that triggers it survives
   twice.  A last pass collapses those into one entry, at the line where the
   fix belongs.

6. **Print.**  Survivors are sorted by file and line and printed in GCC
   format.  Locations quoted inside an explanation are written `world.h@44`
   rather than `world.h:44`, because `compilation-mode` would otherwise turn
   the explanation into a second, bogus jump target.

Everything Claude does here is read-only: the jobs run with only the `Read`,
`Grep`, and `Glob` tools, so the analyzer cannot touch your source.

## Options

| option | default | meaning |
|--------|---------|---------|
| `-j N` | 4 | parallel Claude jobs |
| `-m MODEL` | `opus` | `opus`, `sonnet`, `haiku`, or a full model id |
| `-e LEVEL` | `high` | reasoning effort: `low` `medium` `high` `xhigh` `max` |
| `-r` | off | recurse into subdirectories |
| `-f GLOB` | -- | search only these files for bugs (repeatable); implies `--no-global` |
| `-x GLOB` | -- | exclude files (repeatable) |
| `-c N` | 600 | lines per chunk; `0` disables chunking |
| `-n N` | 10 | maximum findings reported per chunk |
| `-t SECS` | 1200 | per-Claude-call timeout |
| `--no-verify` | off | skip the refutation pass |
| `--no-global` | off | skip the cross-file pass |
| `--global` | off | keep the cross-file pass even under `-f` |
| `--no-merge` | off | skip the duplicate-collapsing pass |
| `-k` | off | keep the work directory (raw Claude output, prompts, logs) |
| `-q` | off | print findings only, no progress lines |

Every default can also be set from the environment: `CSTATIC_MODEL`,
`CSTATIC_JOBS`, `CSTATIC_EFFORT`, `CSTATIC_CHUNK_LINES`,
`CSTATIC_CHUNK_OVERLAP`, `CSTATIC_MAX_FINDINGS`, `CSTATIC_TIMEOUT`,
`CSTATIC_CLAUDE`.

## Cost and time

Roughly two to three Claude calls per source file (analysis, plus a share of
the verification pass), more for long files that get chunked.  On `opus` with
`-e high` that is minutes, not seconds -- which is fine, because the intended
workflow is to start it and walk away.  For a quick sweep of a large tree,
`-m sonnet -j 8 --no-verify` is much faster and much noisier.

`-x` matters here.  A vendored `stb_*.c` or a bundled library is thousands of
lines you did not write and do not want to pay to audit.

## Testing it

`tests/buggy/` is a small C89 program that compiles cleanly under
`gcc -std=c89 -pedantic` and runs, but has around twenty planted bugs.
`tests/ANSWERS.md` lists them, including a few deliberate non-bugs that a
careless analyzer will report as false positives.  It lives outside
`tests/buggy/` so the analyzer cannot read it.

    ./cstatic.sh tests/buggy

`tests/runTest.sh` runs the analysis and then feeds the output through the real
`compilation-mode` parser, reporting every location Emacs would let you jump
to.  The parse check must come out even: one jump target per finding, and none
from the explanation lines.

    tests/runTest.sh

A recorded run of it is in `tests/exampleRun.txt`.

### What a good run looks like

The recorded run found 15 of the roughly 17 distinct planted root causes in
about four minutes, reported no false positives, did not report any of the
deliberate non-bugs, and put every line number exactly on the defect rather
than near it.  Four of the fifteen came back as `warning:` -- real defects
that no current call site drives into, including both `realloc` clobbers and
the `int` multiply that overflows before it is widened to `long`.

It missed the unguarded divide in `scaleGrid` and the zero-capacity case in
`growItems`.  Both are the same shape: a function nothing currently calls the
wrong way.  Expect that shape to be the weak spot.

## Caveats

This is a language model reading your code.  It is good at the kinds of bugs
listed above and it is not a proof of anything.  Read every finding before you
act on it; the explanation always names the concrete failing execution, so
checking one takes less time than finding it did.

A quiet run does not mean the code is correct.  It means this pass did not
find anything it could defend.
