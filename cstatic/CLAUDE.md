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

## Pipeline

Five stages, in `cstatic.sh`:

1. **Collect** — every `.c`/`.h` in the folder, top level only unless `-r`,
   minus `-x` globs and `.cstaticignore`. `-f` then narrows that to the files
   the user actually wants searched. Two lists come out: `FILES` (what gets a
   read job) and `ALL_FILES` (everything, pre-`-f`). **The cross-file pass must
   use `ALL_FILES`** — a seam-finder shown only the focused file cannot see a
   seam. `-f` turns that pass off by default anyway, since its whole job is
   scanning the project; `--global` puts it back and scopes its attention to
   where the `-f` files meet the rest.
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

If you edit any prompt, re-run the test and compare against `ANSWERS.md`
before committing. Watch both directions — findings lost, and noise gained.
