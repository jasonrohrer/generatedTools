# CLAUDE.md — cstatic

Orientation for a fresh session. Read this first, then read `README.md` for
the user-facing story. **Keep this file current**: when you change a stage,
change a prompt in a way that matters, or learn something non-obvious about
how Emacs parses the output, update the relevant section here in the same
commit.

## What this is

`cstatic.sh` is a Claude-powered static analyzer for C89 projects. Jason runs
it from Emacs with `M-x compile`, walks away, and comes back to a list of
suspected bugs he can page through with `C-x \``.

It never compiles or runs anything. It is invoked only on code that **already
compiles cleanly and already appears to work**, so the target is the subtle
survivor: the off-by-one at one boundary, the `realloc` whose old pointer is
lost, the `i` that should have been a `j`, two headers that quietly disagree.

This is a **prompt-crafting project**. The shell script is plumbing; the
prompts inside it are the product.

## Hard constraints (do not violate)

- **Output must stay parseable by Emacs `compilation-mode`.** A finding is
  one `file:line:col: error|warning: message` line followed by at most four
  indented explanation lines. Nothing else in the whole run may look like a
  location. Total per finding stays under five lines so it fits in a
  half-height compilation window.
- **`compilation-mode` also parses `world.c:32`, `world.c line 32`, and
  `world.c, line 99` when they appear inside an explanation line.** Verified
  empirically against Emacs 30.1, not guessed. The `sanitize()` awk function
  in the print stage rewrites all of those to `world.c@32`, which no
  compilation-mode rule recognizes. If you touch that function, re-run the
  parse check. Forms confirmed safe: `@32`, `(32)`, `#32`, `L32`, `[x.c 32]`.
- **The analyzer is read-only.** Every Claude job runs with
  `--tools "Read,Grep,Glob"`. It must never be able to edit the user's source.
- **Exit status is always 0** on a completed run, so Emacs says "Compilation
  finished" rather than "exited abnormally", whether or not bugs were found.
- **Never try to compile the code under analysis.** That is the whole premise.
- **`exec > >(tee -a "$LOG")` mirrors the whole report to a log file**, because
  running `make` in the compilation buffer destroys the findings. That process
  substitution is a background job that never exits, so a bare `wait` hangs
  forever. `run_pool` must wait on its analysis jobs **by recorded PID**. This
  already cost one hung run; do not "simplify" it back to `wait`.

## Pipeline

Five stages, in `cstatic.sh`:

1. **Collect** — every `.c`/`.h` in the folder, top level only unless `-r`,
   minus `-x` globs and `.cstaticignore`, then narrowed by `-f`.
2. **Read** — one Claude job per file, chunked at 600 lines with 80 lines of
   overlap for longer files. Prompt = `BUG_CATALOG` + `NOT_A_FINDING` +
   `OUTPUT_CONTRACT`. Plus one **global** job that looks only at cross-file
   seams (prototype drift, enum vs switch, ownership, units).
3. **Verify** — one job per *target* file, given every candidate pointing into
   it, asked to **refute**. Three verdicts: reachable → `error`,
   latent → `warning`, wrong → dropped silently.
4. **Merge** — one job that collapses candidates sharing a root cause.
   Necessary because stage 3 groups by file and so cannot see across files;
   without it the same bug appears at both the defect and its call site.
5. **Print** — sort by file and line, sanitize citations, wrap to 74 columns.

Claude's output is parsed from `<<<FINDING>>>` blocks by the `PARSER` awk
program into tab-separated records: `file, line, col, sev, msg, detail`, where
detail lines are joined with `\001`.

`PARSER`'s `fixpath()` repairs the `FILE:` value against `knownpaths.txt`, the
list of source files that actually exist under the root: exact match, then
**longest** suffix match, then an unambiguous bare file name. This is not
optional polish — the first isolation run emitted
`cstatic/tests/buggy/world.c` for what had to be `world.c`, and Emacs cannot
follow a path like that. The prompts now also state the root and the short
name to use, but `fixpath()` is the backstop that makes it not matter.

## Two modes, and why `-f` is not just a filter

Without `-f`, the tool audits a **project**: jobs get `Read,Grep,Glob`, are
told to look up ground truth anywhere, and the verifier grounds every claim in
what the real call sites can actually pass.

With `-f` (`ISOLATE=1`) the tool audits a **file**, reproducing what happens
when you paste one file into a chat window. This is Jason's explicit
requirement, and it is the opposite posture:

- Jobs get `--tools "Read"` only. They *cannot* search the folder. This is a
  mechanical guarantee, not a request in a prompt.
- `include_closure()` resolves the file's quoted `#include`s transitively and
  `closure_block()` names them in the prompt as the only other openable files.
  Angle-bracket includes are skipped; the closure is capped at `MAX_CLOSURE`.
- Both the analysis and the verify prompt switch to isolation variants. The
  load-bearing sentence is in the verifier: **never reject a claim because no
  current caller triggers it** — it cannot see the callers, so that reasoning
  is unavailable to it.
- Verdict names change with the posture: reachable/latent become
  sound/conditional, since "reachable" is meaningless without callers.
- The cross-file pass is off. It is the exact thing isolation refuses to do.

Consequence worth remembering: `-f` is **stricter** on a given file than a full
run, not more lenient. A whole-project run demotes an unreachable defect to a
warning; isolation has nothing to demote it against.

## The two calibration knobs that actually matter

- **The verify prompt's line between "latent" and "wrong."** An earlier
  version had only confirm/reject, and the verifier rejected *every* real bug
  whose failing path no current caller reached — the `realloc` clobber, the
  `int` overflow before widening. All were genuine. The three-way verdict
  exists to keep those as warnings. If findings start disappearing, look here
  first.
- **`NOT_A_FINDING`.** This is what keeps the false-positive rate at zero on
  the test project. Loosen it and the report fills with unchecked-`malloc`
  noise and refactoring advice.

## Testing

`tests/buggy/` is a five-file C89 program that compiles clean under
`gcc -std=c89 -pedantic` and runs, with about twenty planted bugs.
`tests/ANSWERS.md` is the key — it lives **outside** `tests/buggy/` on purpose,
so the analyzer cannot read it. It also lists deliberate non-bugs that a
careless analyzer reports as false positives.

    tests/runTest.sh

runs the analysis and then feeds the output through the real
`compilation-mode` parser via `tests/checkEmacsParse.el`. That check must come
out even: **one jump target per finding, zero from explanation lines.**

`tests/exampleRun.txt` is a recorded good run: 14 findings, no false
positives, every line number landing exactly on the defect, about four minutes
on `opus` with `-e high`.

Test isolation mode separately, since it takes the other branch of every
prompt:

    ./cstatic.sh -f world.c tests/buggy

A good isolation run on `world.c` finds **more** in that file than a full run
does (8 vs 5 in the reference), because it cannot excuse anything by pointing
at what the callers happen to do. `tests/exampleRunIsolated.txt` is the
recorded one.

If you edit any prompt, re-run the test and compare against `ANSWERS.md`
before committing. Watch both directions — findings lost, and noise gained.
