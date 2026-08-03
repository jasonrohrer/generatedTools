# Answer key for tests/buggy

This little C89 program compiles cleanly under `gcc -std=c89 -pedantic` and
runs without crashing on a typical desktop, which is exactly the situation
cstatic is meant for.  Every bug below was planted on purpose.

Keep this file OUT of `tests/buggy/`, so the analyzer cannot read it.

To score a run:

    ./cstatic.sh -k tests/buggy

then compare the reported lines against the table.  A good run finds most of
the "loud" bugs and few or no false alarms.  Findings not on this list are not
automatically wrong -- read them before calling them false positives.

## world.h

| id | line | bug |
|----|------|-----|
| H1 | 7 | `SQUARE(x)` expands to `x * x` with no parens, so `SQUARE(GRID_W - 2)` is `12 - 2*12 - 2` = -14, not 100 |

## world.c

| id | approx line | bug |
|----|------|-----|
| W1 | 15 | `w->items` malloc result never checked, and `newWorld(0)` yields a zero-size block |
| W2 | 40 | `len > MAX_NAME_LEN` should be `>=`; with a 16+ char title `title[16] = '\0'` writes one past `char title[16]` |
| W3 | 55 | `realloc` result assigned straight over `inWorld->items`: the old block is leaked and lost if realloc fails.  Also `capacity * 2` stays 0 when capacity is 0 |
| W4 | 72 | `strcpy( slot->name, inName )` into `char name[16]`; main.c passes "potion of extreme healing" (25 chars) |
| W5 | 86 | `i <= inWorld->numItems` reads one element past the end |
| W6 | 100 | `value * count` is computed in `int` and only then added to a `long` total |
| W7 | 113 | `%d` used to print `long totalValue` |

## grid.c

| id | approx line | bug |
|----|------|-----|
| G1 | 38 | `getCell` does no bounds checking although `setCell` right above it does; main.c feeds it unclamped argv values |
| G2 | 68 | stray `;` after `if( sGrid[inY][x] != 0 )`, so `count++` runs for every column |
| G3 | 79 | `memcpy( outBuffer, sGrid, sizeof( outBuffer ) )` copies `sizeof(int*)` bytes, not the grid |
| G4 | 89 | `case 1:` has no `break`, so value 1 is described as "floor" |
| G5 | 83 | `describeCell` returns a pointer to a `static` buffer; two live calls collide (see M2) |
| G6 | 118 | `scaleGrid` divides by `inDenominator` with no zero check |
| G7 | 145 | `realloc` result assigned over the only copy of `out`; `malloc` result also unchecked |

## main.c

| id | approx line | bug |
|----|------|-----|
| M1 | 44 | `value` is read uninitialized when `inText` is NULL, then used as a grid index |
| M2 | 57 | both `describeCell()` calls in one `printf` return the same static buffer, so cell A and cell B always print the same text |
| M3 | 91 | `copyGrid( buffer )` fills only 8 of the 480 bytes (call site of G3) |
| M4 | 100 | `w->totalValue` is read after `freeWorld( w )` -- use after free |
| M5 | 84 | `x` and `y` come from argv and are passed to `getCell` with no range check |

## Deliberate non-bugs (report these and it is a false positive)

- `fillRect` clips correctly inside the inner loop.
- `setCell` bounds-checks both axes correctly.
- `clearGrid` / `dumpGrid` / `scaleGrid` loop bounds are all correct.
- `freeWorld` handles NULL.
- The `sGridReady` flag is set and never read; that is dead, not wrong.
