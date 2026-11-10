# CLAUDE.md — cedit

Orientation for a fresh session.  Read this first, then `userNotes.txt`
(Jason's bug reports / requests — fix those first), then `README.md` for
the user-facing story.  **Keep this file current** when you change the
architecture or learn something non-obvious.

## What this is

cedit is a clean-room graphical programmer's editor for **C89 only**, meant
as an emacs-killer for Jason's workflow:  instant launch, smooth scrolling
of huge files, emacs keys, cc-mode-quality auto-indent in his Ratliff-like
style, auto-formatting (paren padding, `type  name` params, declaration
tables), compile + step through errors, launch a debugger at file:line.

## Hard constraints

- App code (`*.c`) is **pure C89**:  `-std=c89 -pedantic -Wall -Wextra
  -Wshadow ...` with **zero warnings**.  Decls at top of block, `/* */`
  only.  POSIX via `-D_DEFAULT_SOURCE`.
- Linked deps:  **SDL2 + OpenGL only**.
- Dear ImGui (C++, in `imgui/`) is walled off behind `gui.h` / `gui.cpp`
  (the only C++ of ours).  We use it almost purely as a draw-list
  renderer + font rasterizer;  the editor draws its own text grid.
  Legacy `opengl2` backend, like cwave.
- **Write code in Jason's style** (it is also the editor's default
  style):  Ratliff braces (closing `}` indented with the body),
  `if( x )`, `foo( a, b )`, `a[ i ]`, two spaces between type and name,
  declaration tables (`int    i;`), struct members at +8, `in`/`out`
  param prefixes, banner comments between sections.
  `tests/run.sh` enforces that our own sources survive `--check-indent`.

## Build, run, test

    make                     # ./cedit ;  *.o and cedit are gitignored
    tests/run.sh             # keystroke-script regression tests (no display)
    tests/run.sh --update    # regenerate .expected (then READ the diffs)
    ./shot.sh out.png FILE   # screenshot under private Xvfb :98
    KEYS="ctrl+x ctrl+f" ./shot.sh out.png FILE   # xdotool keys first
    CEDIT_QUIT_AFTER=1 ./cedit FILE   # time launch-to-first-frame

Never draw to Jason's real display;  always Xvfb (`shot.sh` does it).
Read the PNGs back with the Read tool.

`--script KEYS FILE` is the fastest way to test editing behavior:  tokens
are `"typed text"` (with `\n` = Enter) and keys like `C-x`, `M-/`, `RET`,
`TAB`, `S-Down`, `F4`.  It runs the real `ed_keyDown` / `ed_textInput`.

To test the indenter against real hand-indented code, Jason's
Chessamphetamine source is at `../caSim/Chessamphetamine/` (maxigin.h,
chess.h, game.c ...):  `./cedit --check-indent ../caSim/Chessamphetamine/*.h`
(~61k lines;  ~267 differ, all hand alignments — keep it that way).

## File map

- `cedit.c` — main, SDL loop, all rendering (tab bar, text view, compile
  pane, status line, minibuffer, completion list), mouse, headless modes.
- `editor.c/h` — the `Editor ed` global:  buffer list + MRU, minibuffer
  prompts with completion (files / buffers / commands), key tables
  (`globalKeys`, `ctrlXKeys`, `ctrlCKeys`, `metaGKeys`), the M-x
  `commands[]` table (also generates the F1 help), kill ring, isearch,
  query replace, dabbrev, electric typing, compile/next-error/debug glue.
- `textBuffer.c/h` — lines array, undo/redo, markers.  No SDL.
- `syntax.c/h` — line-at-a-time C lexer, per-line start-state cache,
  highlighter, typedef-name set.  No SDL.
- `indent.c/h` — the indentation engine.  No SDL.
- `format.c/h` — paren padding, param tables, declaration tables.  No SDL.
- `config.c/h` — the settings table (also writes the commented default
  files).  Adding a setting = a field in `Config` + one `entries[]` row +
  a default in `setDefaults`.
- `compile.c/h` — fork/pipe/nonblocking compile, gcc message parsing,
  detached debugger launch.
- `gui.cpp/h` — ImGui shim.  `fontData.c` — embedded Cousine TTF.

## Architecture notes & invariants

**Lines + arena.**  `Line.cap > 0` = own malloc'd text;  `cap < 0` = slice
of the buffer's `arena` (the file as read), usable in place up to `-cap`
bytes, copied out by `lineReserve` on first growth.  Never `free()` a
line's text directly — use `lineFree`.  This made loading a 1M-line
file 3x faster.

**Final newline.**  `tb_load` strips exactly one trailing `\n`;
`tb_save` writes `\n` after EVERY line (only a lone empty line writes
an empty file).  So files round-trip, a blank last line survives, and a
missing final newline gets added on save.

**All edits go through `tb_insert` / `tb_delete`** (or `tb_setLineIndent`
which uses them):  that keeps undo, markers, `changeCount` and the syntax
state cache (`stateValidThrough`) right.  Undo records coalesce typed
runs;  `tb_beginGroup` / `tb_continueGroup` decide what one C-/ undoes
(`runCommand` starts a group per command;  typing joins up to 20 chars).

**The cursor is a marker** (`tb->cursorMarker`), so edits anywhere
(reindent, format, other code) keep it on its text.  `tb->cursorLine/Col`
mirror it for undo and the renderer:  in editor.c always use
`getCur` / `setCur` / `syncCur`, never write the fields directly.  The
mark is marker `markMarker`;  compile errors get markers when first
visited so they track edits.

**Scroll is in lines/columns** (`scrollY`, `targetScrollY` floats), not
pixels, so font zoom keeps the view.  Commands just move the cursor (or
set `ed.recenterRequest`);  `updateScroll` in cedit.c keeps it visible
and animates.

**Syntax state cache.**  `syn_getLineState(tb, line)` lazily lexes
forward from `stateValidThrough` (fast path skips lines without `/` `"`
or trailing `\`).  Edits invalidate from the edited line.  Highlighting
only lexes visible lines.

**Indent engine** (`indent.c`, read its header comment):  find an anchor
(a column-0 identifier line outside comments, i.e. a top-level
declaration), parse forward with a frame stack (`{` `(` `[`, with
`isList`, `isSwitch`, `isStruct`, `isExternC`, `alignCol`) plus a
statement state (`active`, `expectSub` for if/while/for/else/do bodies,
`assignCol`, per-block if-stack for `else`), then `decideIndent` from the
first token of the target line.  `ind_indentRegion` carries ONE parse
forward line by line (feeding each line after re-indenting it) — that's
why whole-file reindent is ~50 ms.  Comment bodies are never re-aligned
by region indent;  they shift rigidly with their opener line
(`ind_commentIndent` is only for Tab/Enter).  Behaviors were tuned
against Jason's real code — data beats intuition here (e.g. args after a
line-ending `x = foo(` use line indent +4, NOT the right-hand side, 18
vs 5 in his code;  but plain continuations of `x = a` align after `=`).

**Electric typing** (`electric()` in editor.c):  reindent on `{ } : ; #`
and a leading `else`;  auto-newline after `{` (not lists/enums/inside
parens), `;` (not inside parens), `}` (not for struct bodies or
do-blocks, so `} Foo;` and `} while( x );` work), and `case X:` (hanging
colon).  `;` typed alone after a `}` line joins it (`};`).  Enter
reindents + formats the line being left.

**Formatter** (`format.c`):  `fmt_formatLine` = `spaceTokens` (paren
padding, commas, `x = 6`, `) {`) -> `splitLine` (breaks after every `,`
of a call/param list and `;` of a `for(`, then re-indents the new lines
so they align after the `(`;  paren groups are classified by the token
before the `(`, and `findOpenParen` looks back for the group a
continuation line is inside) -> `formatForTable` (3-row table, `++` in
the value column, matching Jason's `y =  BH - 1; / y >= BH - 3; /
y    --`) -> `formatParamList`.  Edits can add lines, so the public
entry points carry the cursor in a temporary marker
(`beginCursor`/`endCursor`);  single-line space edits still map it by
hand (`applyEditsCur`) since a deleted gap would collapse a marker.
Declaration rows may span lines (initializer call continued:
`DeclRow.lastLine`;  continuation lines are re-indented after the row
moves) and tables span `declTableBlankLines` (1) blank lines — both are
how Jason's real code looks.  A leading storage class (`static extern register auto`) is a
sub-column of the type (`storageEnd`/`restStart`, gap
`declStorageSpaces`;  `const`/`unsigned` stay with the type).  Only
*literal* one-line initializers (number, `-`number, char, string;
`valueEnd`/`semiStart`) are right-aligned to a common `;` column
(`declAlignValuesRight`) — Jason's real code right-aligns `0`/`-1` but
leaves `p & MASK` / calls left-aligned.  Uninitialized rows (`int  spriteA;`)
push the `=` column out to their `;` + `declEqualsSpaces` (arrays don't).  `FMT_PARTIAL` (Tab) lets the cursor line
be an unfinished row (`float `, `char *`) whose name goes at line end.

**Window width.**  `windowCols` = text columns exactly (gutter is >= 4
digits so it doesn't jump);  `gui_init` builds the font atlas so the real
advance is known and main() resizes the window right after.

**Keys.**  `ed_keyDown` returns 1 if it consumed the key;  cedit.c then
drops the SDL_TEXTINPUT for that keypress (otherwise `M-g g` types a `g`
into the goto prompt).  Printable keys return 0 and arrive as text.

**Selections.**  `updatePrimarySelection` (top of `ed_tick`) offers the
active region as the X PRIMARY selection whenever its bounds / the
buffer's `changeCount` change;  middle click = `ed_mousePastePrimary`.
The clipboard stays the kill ring's (`pushKill` / `cmdYank`).

**Bool splitting.**  `splitLine` breaks before and after `&&` / `||`
at the top level of a non-for paren group (or a line starting with
`return`);  the indenter aligns `return` continuations after `return `
(Jason's code:  `return b->pressed / && / ! pressedBefore;`).

**Compile.**  Output streams into `ed.compileBuffer` (not in the buffer
list);  `comp_poll` is called from `ed_tick` every loop iteration while
running (the loop wakes every 15 ms then).  Indented lines after a
message are its detail lines (gcc excerpts, cstatic explanations).
Notes are skipped by next-error, like emacs.

**Plain text buffers.**  `TextBuffer.isC` (from `tb_isCPath`, set in
`openFile` and save-as;  `tb_create` defaults to 1 so headless tools
and `*scratch*` are C).  When 0:  `syn_highlightLine` leaves everything
`HL_TEXT`, `syn_scanTypedefs` returns at once (a prose file must not
pollute the global typedef set), `reindentLine` / `electric` / hungry
delete do nothing, Enter is `plainNewLineIndent` (copies the previous
line's leading whitespace literally), Tab is `plainTab` (a real `\t`),
C commands say "Not a C buffer" (`checkC`), and cedit.c skips bracket
matching.

**Tabs.**  `tb_load( ..., inExpandTabs, ... )` expands tabs only for C
files;  plain text buffers keep real `\t` (Makefiles).  So a column is
a byte offset, but the screen works in *display* columns:  `tb_visCol` /
`tb_colAtVis` (textBuffer.c) convert, `VCOL()` in cedit.c.  `ViewRow`
segStart/segEnd, `scrollX`, `ed_lineRows` and `goalCol` (C-n / C-p) are
display columns;  `drawTextView` converts every byte span before
drawing and draws a tabbed line from a copy expanded to cells.  Both
functions fast-path lines with no tab (memchr), so C buffers pay ~0.

**Line wrap** (cedit.c "Visual rows").  `ed.wrapCols` = text columns
when `wrapLines`, else 0;  set by `drawTextView` every frame,
`ed_lineRows` (editor.c, also used by paging) gives a line's row count.
A line exactly `wrapCols` wide takes ONE row (the end-of-line cursor is
a bar at the edge).  `scrollY` is still a line number;  its fraction is
a fraction of the top line's rows.  `buildViewRows` lays out
`viewRows[]` (line, segStart, segEnd, y) once per frame;  every draw
pass and `mouseToTextPos` go through it (`spanRect`, `COL_X`).
`updateScroll` keeps the cursor's row in the margin with
`topWithRowsAbove` / `topWithRowAtMost`, which only walk a screen of
lines.  `C-n` / `C-p` are still logical-line moves.

**Operator spacing** (`classifyOperators` in format.c) labels each
token OP_BINARY / OP_UNARY_SPACE (`! ~ -x`) / OP_UNARY_HUG (`*p &x -1`,
declaration stars) / OP_UNSURE from the token before it (`isOperand`;
a cast's `)` is not an operand).  `*` after a name goes through
`isDeclStar` (type names, Capitalized names, decl keywords, statement
starts, `braceDepth == 0` parameter lists).  A line starting with an
operator looks at the previous code line's last token
(`prevLineEndsWithValue`) — Jason's code has `* ( 255 - a )` and
`& 0xFF` continuation lines.  Against Chessamphetamine the only
changes are exactly the requested rule (`-x` -> `- x`, `& x` -> `&x`,
`!x` -> `! x`) and tight operators in imported (sha1-ish) code.

**Huge files.**  Typedef scanning for files > 20k lines is deferred to
`ed_tick` after the first frame (`needsTypeScan`).
