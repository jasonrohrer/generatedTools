# cedit

A fast, focused, graphical editor for C89 code.  Not an IDE:  it edits C
files, compiles your project, steps through compiler errors, and launches
your debugger at the current line.  That's it.

The point is what emacs + cc-mode does well (strict, configurable
auto-indentation that handles the hard cases, including Ratliff style),
without what it does badly (seconds to launch, sluggish scrolling through
big highlighted files).

- Starts to a usable window in a fraction of a second.
- Opens and scrolls a million-line file smoothly (only visible lines are
  ever highlighted).
- Pure C89 + SDL2 + OpenGL.  Dear ImGui is compiled in (for font
  rendering and GL plumbing), behind a tiny C shim.


## Build

    make
    ./cedit [+LINE] [FILE...]

Needs `libsdl2-dev`, OpenGL, gcc/g++.


## Keys

Emacs-style.  Press **F1** inside cedit for the full list (also
`M-x help`).  The highlights:

| Keys | |
|---|---|
| `C-x C-f` | find file (Tab completes paths, Up/Down picks) |
| `C-x C-s`, `C-x s` | save, save all |
| `C-x b` | switch buffer (Enter = previous buffer) |
| `C-Tab`, `C-S-Tab`, `M-1`..`M-9` | next / previous / numbered buffer |
| `Tab` | re-indent line (or region) |
| `C-M-\` | re-indent region, or the current function |
| `M-/` | complete the word from this and other buffers (repeat cycles) |
| `C-s`, `C-r`, `M-%` | incremental search, query replace |
| `C-c C-c`, `F5` | compile (editing the command), recompile |
| `F4`, `C-x ``, `M-g n` / `S-F4`, `M-g p` | next / previous error |
| `C-c C-d`, `F9` | launch the debugger at this file:line |
| `M-x` | any command by name |

Shift + movement selects;  typing replaces the selection.

Two copy buffers, like emacs and other X programs:  `M-w` / `C-w` /
`C-y` use the clipboard (and the kill ring), while any selected region
(mouse drag, Shift + movement, `C-Space` + movement) automatically
becomes the X PRIMARY selection, and the middle mouse button pastes
PRIMARY at the click.


## Indentation

Indentation is computed like cc-mode does it:  relative to the actual
indentation of the lines that open the enclosing constructs, so
indenting one line is cheap and never needs the whole file.  Re-indenting
a whole 25,000 line file takes about 50 ms.

The default style is Ratliff-like, matching this emacs setup:

    block-close 4, defun-close 4, class-close 4, inclass 8,
    substatement-open 0, case-label 4, arglist-close c-lineup-arglist,
    auto-newline + hungry-delete on

```c
typedef struct Foo {
        int    count;
        char  *name;
    } Foo;

static int foo( int          inX,
                const char  *inName ) {
    int  total  =  0;

    switch( inX ) {
        case 1:
            total = 2;
            break;
        }
    if( total > 5 ) {
        return 1;
        }
    else {
        return 0;
        }
    }
```

Also handled:  continued argument lists line up after the `(`,
continued assignments line up after the `=`, `else` finds its `if`,
brace lists and enums, `do { } while( x );`, `extern "C" {`, block
comments, preprocessor lines at column 0, multi-line array dimensions.

Tested against ~61,000 lines of hand-indented Ratliff-style code:  it
agrees on 99.56% of lines, and the rest are hand alignments.

Every offset is configurable in `style.conf` (K&R:  `closeBraceOffset =
0`, `structIndent = 4`).


## Auto-formatting

When you leave a line with Enter (or auto-newline), or press Tab on it,
cedit tidies it:

- `foo(a,b[i])` becomes `foo( a, b[ i ] )` (casts stay tight: `(int)x`)
- spaces around assignments (`x=6;` becomes `x = 6;`), comparisons
  and `&&` / `||` (`x==5` becomes `x == 5`), and before a block's brace
  (`while(1){` becomes `while( 1 ) {`)
- spaces around every other binary operator (`a+b*c` becomes
  `a + b * c`, also `/ % & | ^ << >>` and `? :`), and after unary
  `!`, `~` and `-` (`!x` becomes `! x`, `-y` becomes `- y`, but a
  negative number stays `-1`).  The pointer operators hug their
  target:  `int  *p  =  &x;`, `z = *p;`.  Whether a `*` is a
  multiplication or a declaration's star is decided from context
  (known type names, Capitalized names, statement starts, parameter
  lists), and a line that starts with an operator continuing the line
  above (`* ( 255 - a )`) is recognized as binary.
- parameters get two spaces between type and name:  `int  inX`
- arguments of calls and parameters of functions go one per line, lined
  up after the `(`;  the parameters become a table:

```c
int myFunction( int        inArgA,
                MyStruct  *inStruct ) {
    x = myFunction( 10,
                    &currentStruct );
```

- the operands of `&&` and `||` inside parens (or after `return`) go
  on their own lines, with the operator on a line by itself:

```c
    if( ! spinning
        &&
        ( formationShowing
          ||
          draftingPieces ) ) {

    return b->pressed
           &&
           ! pressedBefore;
```

- `for( )` clauses go on their own lines as a table, with `++` / `--`
  in the value column:

```c
    for( x = 0;
         x < 10;
         x   ++ ) {

    for( y =  BH - 1;
         y >= BH - 3;
         y    -- ) {
```

- runs of declarations are lined up into a table, with pointer stars
  hanging left of the name.  One blank line inside a run keeps the
  table going (two start a new one), and an initializer that is a call
  continuing onto more lines is still part of the table:

```c
    Deck           *d      =  getDeck();
    int             i;

    unsigned char  *bytes  =  NULL;
    int             x      =  maxigin_randRange( inRand,
                                                 inStartX,
                                                 inEndX );
```

Tab on an unfinished declaration (`float ` or `char *`, or a parameter
on its own line) lines the table up as if it were finished and puts the
cursor where the name will go.

Each rule can be turned off in `style.conf` (`assignSpaces`,
`compareSpaces`, `operatorSpaces`, `unarySpaces`, `splitBoolOps`, `spaceBeforeBrace`, `splitArgLists`, `splitForClauses`,
`alignForClauses`, `alignDeclarations`, `declTableBlankLines`, ...).
`C-c C-f` formats a region / the current function, `M-x format-buffer`
the whole file.


## Plain text files

Only C files (`.c` `.h` `.cpp` `.cc` `.hpp` ...) get syntax highlighting,
auto-indent, electric characters, formatting and bracket matching.
Anything else (`notes.txt`, `Makefile`, `README.md`) is edited as plain
text:  Enter keeps the previous line's indentation, Tab is
indent-relative like emacs text-mode (to the next word start on the line
above, else the next 4-column stop), Tab on a region shifts its lines
right, and Backspace deletes one character.  The status line shows
`(Text)` instead of `(C/ah)`.


## Long lines

Lines longer than the window are shown wrapped onto extra rows, with a
hooked arrow in the line number gutter of each continuation row (like
the emacs fringe arrows).  The file itself is never changed.  The
cursor, mouse clicks, selections, search highlights, paging and the
scroll margin all work in screen rows;  `C-n` / `C-p` still move by
whole lines.  `wrapLines = 0` in `style.conf` scrolls sideways instead.


## Configuration

Plain `key = value` files, read in this order (later wins):

    ~/.cedit/colors.conf     every syntax and UI color
    ~/.cedit/style.conf      indentation, formatting, display
    ~/.cedit/cedit.conf
    ./cedit.conf            per-folder:  compile and debug commands

Run `cedit --write-config` (or `M-x write-default-config`) to get fully
commented copies of all of them in `~/.cedit/`.

`windowCols` is the number of text columns the window opens with, not
counting the line numbers and the scroll bar:  with `windowCols = 80`
the window's text area ends exactly at the `fillColumn = 80` ruler (a
cursor at the end of an 80 column line shows as a bar at the edge).
The scroll bar can be dragged, or clicked to jump;  it marks the cursor
line and the current compile error (`showScrollBar = 0` hides it).

A typical per-folder `./cedit.conf`:

    compileCommand = ./build.sh
    debugProgram   = ./myGame
    debugCommand   = myDebugger %e %f:%l

In `debugCommand`, `%f` is the current file name, `%p` its full path,
`%l` the cursor's line, `%e` the `debugProgram`.  If they aren't set,
cedit asks the first time you press F9;  `M-x save-folder-config` writes
your answers into `./cedit.conf`.


## Command line tools

    cedit --reindent FILE        print FILE re-indented
    cedit --format FILE          ... and auto-formatted
    cedit --check-indent FILE..  list lines cedit would indent differently
    cedit --script KEYS [FILE]   replay keystrokes, print the buffer
                                 (used by tests/run.sh)
