# CLAUDE.md — cwave

Orientation for a fresh session. Read this first, then skim `userNotes.txt`
for the current to-do list. **Keep this file current**: when you change the
architecture, add an invariant, or learn something non-obvious, update the
relevant section here in the same commit.

## What this is

cwave is a fast, stable, clean-room graphical **audio editor** (like
Audacity / OceanAudio / Sound Forge) — NOT a DAW. Playback + editing only,
no recording. The prime directive is **launch instantly, stay stable**; the
existing Linux editors are slow/bloated/crashy and this is the antidote.

## Hard constraints (do not violate)

- Application code (`cwave.c`, `audio.c`, `audio.h`) is **pure C89**, compiled
  `-std=c89 -Wall -Wextra` with **zero warnings**. Declarations at the top of
  a block; `/* */` comments only; no `//`, no C99 mid-block decls, no VLAs.
- Only linked deps: **SDL2** (window, GL context, audio out), **OpenGL**,
  **libpthread** (async file load). Nothing else.
- **Dear ImGui** (C++) is compiled in but walled off behind a thin C shim
  (`gui.h` / `gui.cpp`, `extern "C"`). All app logic stays in C; if the app
  needs a new ImGui call, add a `gui_*` wrapper rather than leaking C++ out.
- **OGG** in via `stb_vorbis.c` (decode only — no OGG export exists). **WAV**
  read/write is hand-rolled in `audio.c`. Only WAV + OGG are supported.
- Rendering is **legacy fixed-function OpenGL** (glBegin/glEnd) paired with
  ImGui's `opengl2` backend — deliberately, so there is no shader compile and
  startup is instant. Don't introduce shaders / modern GL.

## Build & run

```
make                 # builds ./cwave ; *.o and cwave are gitignored
./cwave [file.wav|file.ogg]
```

Committed to `main` directly (personal local repo, linear history). Commit
only when asked, with a detailed message; end messages with the
`Co-Authored-By: Claude Opus 4.8 (1M context)` line.

## Headless screenshots & interaction testing

There is no display you should draw to directly (the user is working on this
machine). Run under Xvfb and grab with scrot:

- `./shot.sh out.png [args...]` — launch under Xvfb `:99`, screenshot once.
- `./shot2.sh prefix D1 D2 [args...]` — two shots at two delays.
- **xdotool** is available for driving input. Pattern that works: start cwave
  under Xvfb, `xdotool search --name '^cwave$'` then `windowmove $WID 0 0` so
  window pixel coords are predictable (SDL centers it in the 1200x720 virtual
  screen otherwise), then `mousemove/click/key`, then `scrot`. See the
  scratchpad `itest*.sh` from the marks work for a template.
- Read the PNGs back with the Read tool to actually see the UI.

For pure-logic correctness (e.g. the pyramid), a standalone C test that copies
the relevant functions and diffs incremental-vs-full is faster and more
rigorous than eyeballing screenshots. Do both.

## File map

- `cwave.c` — everything UI/app: state (`App app`), waveform mip pyramid,
  playback (SDL audio callback), editing actions, effects dispatch, marks,
  file browser, ImGui panels, main loop. Big file, well-sectioned by banners.
- `audio.c` / `audio.h` — `AudioClip` (planar float32 per channel, `[-1,1]`),
  WAV/OGG load, WAV save, and the in-place DSP primitives (normalize, amplify,
  fade, silence, reverse, delete/trim/insert, peak).
- `gui.cpp` / `gui.h` — the C↔ImGui shim.
- `Makefile`, `shot.sh`, `shot2.sh`, `README.md`, `userNotes.txt`.

## Architecture notes & invariants

**Audio clip.** `AudioClip` is planar: one `float*` per channel, `numFrames`
samples each, values in `[-1,1]`. Mixing to stereo for playback is cheap.
`audio_delete_range` shrinks logical length without shrinking the allocation.

**Playback.** One SDL audio callback thread mixes all channels → stereo with a
volume. `Player player` holds playhead/range/flags. **Invariant: any edit that
mutates or frees clip data must do so under `audio_lock()/audio_unlock()`** so
the callback never touches freed memory. Edits also `stopPlayback()` first.

**Async load.** Large files decode on a short-lived pthread (`loadWorker`),
building the clip AND its pyramid off-thread; main loop polls a progress value
and swaps the finished clip in under the audio lock. Keeps startup responsive.

**Waveform summary pyramid (`WaveCache`).** Min/max mip levels: level 0 bins
`WF_MIN_BIN`(256) samples, each higher level doubles. Rendering picks the
coarsest level whose bin ≤ samples-per-pixel (`waveCacheLevel`), so zoom/scroll
stay O(pixels) not O(frames). When zoomed in past 1 sample/pixel it draws raw
samples instead. **Note:** at moderate zoom on a short file, spp may be < 256,
so the pyramid isn't consulted and raw samples are scanned — keep that in mind
when a change "looks right" on test.wav but you haven't exercised the cache.

**Incremental pyramid update — the key perf invariant.** A full
`waveCacheBuild` is O(numFrames) (~300 ms on the 28-min `glz.wav`). So
length-preserving effects must NOT full-rebuild. `waveUpdateRange(s,e)` patches
only the affected bins via `waveCacheUpdateRange` (~0.07 ms), and takes the
fast path ONLY when `wave.valid && waveBuiltGen==waveGen && lengths match` —
otherwise it defers to a full rebuild so **the overview can never drift out of
sync with the samples**. Structural edits (cut/paste/delete/trim/undo/redo)
use `bumpWave()` (deferred full rebuild in `renderWaveform`). If you add a new
effect: length-preserving → `waveUpdateRange`; length-changing → `bumpWave`.

**Undo/redo — delta records, NOT whole-clip copies.** Each `UndoRec` is a
self-reversing *splice*: it stores only the frames at `[at,at+oldLen)` before
the edit (`oldData`) and `[at,at+newLen)` after (`newData`). So undo cost
scales with the *edit size*, not the file length — the whole point, since the
old whole-clip snapshot made every edit O(numFrames) (the real cause of "edits
are slow on long files"). Edit actions bracket the mutation with
`beginEdit(at,oldLen)` (snapshot the span about to change) … `commitEdit(newLen)`
(snapshot the new span, push the record). `doUndo`/`doRedo` just call
`audio_splice` with the record's data and move the record between the undo/redo
stacks — no re-copy. **Invariant: `beginEdit`'s `[at,oldLen)` must exactly cover
every frame the edit changes, and `commitEdit`'s `newLen` the resulting span.**
Length-preserving edits (`oldLen==newLen`) undo via `waveUpdateRange`; length-
changing ones via `bumpWave`. Trim is the one inherently-O(n) case (its record
holds the whole pre-trim clip). Marks are NOT in the record — only clamped back
into range after undo/redo (`marksClampToClip`). `clearUndoRedo()` on load/new.

**`audio_splice`** (in `audio.c`) is the single primitive behind delete
(`insLen=0`), insert (`removeLen=0`), in-place overwrite (`removeLen==insLen`,
no realloc) and undo/redo. `audio_insert`/`audio_delete_range` are `realloc` +
`memmove` (O(tail)), not full rebuilds — paste-at-end is a realloc + small copy.

**Playback survives edits.** Edit actions no longer `stopPlayback()`; they
mutate under `audio_lock` and then call `refreshPlayback()`, which re-clamps the
loop range / playhead (and re-syncs `followSel` to the new selection) so a
looping selection keeps playing straight through an effect/cut/paste.

**Marks.** Carets in a strip (`MARK_STRIP_H`) along the top of the wave. A mark
is a `long frame` that must track its sample as edits shift audio: on every
structural edit call `marksAdjustInsert(at,len)` / `marksAdjustDelete(s,e)`;
trim remaps manually; effects on a selection drop marks at the edges. Auto-made
at edit seams; manual via `M` / Marks menu. Selection edges snap to a nearby
mark in screen space (`snapFrameToMark`). Click a caret to select, shift-click
for multi; delete via menu (`Delete Selected Marks`, `Clear Marks in Selection`
— drops every caret inside the current selection — or `Clear All Marks`). **If
you add an edit that changes length, you MUST adjust marks or they'll point at
the wrong samples.** Each mark carries a `color` index into the 10-hue
`markColors[]` palette; blue/cyan is deliberately absent (the waveform is blue),
so the hues are five bright (orange, yellow, green, magenta, red) + five dark
(tan, dark green, purple, maroon, brown). A group made together (a selection's
two edges, a paste's two seams, a lone cursor mark) shares one hue from
`newMarkColor()` so related carets read together. Carets also appear as tiny
hued triangles in the overview strip.

**Paste semantics.** `actPaste` lays the clipboard down and leaves it
*un-selected*, dropping the cursor at the end of the inserted span so repeated
pastes chain end-to-end ("train cars"). It calls `ensureFrameVisible(end)` so
the view scrolls to follow the advancing cursor (e.g. pasting at the end keeps
the growing tail on screen). Seams are still marked.

**Coordinates.** `frameToPixel`/`pixelToFrame` convert using `viewStart` +
`samplesPerPixel`. `app.wfX/Y/W/H` (wave) and `app.ovX/Y/W/H` (overview) are
recomputed every frame in `renderWaveform`; input handlers read last frame's
values, which is fine because layout is stable frame-to-frame. `clampView`
allows scrolling `EDGE_PAD_PX` (20px) past either end; that over-scroll region
is painted solid black in `renderWaveform` as a "beyond the data" boundary cue.
The overview also shows the bare cursor (yellow) when there is no selection and
the live playhead (green), mirroring the main view.

## Style

Match the surrounding code: the existing banner-comment section headers, the
`( spaced parens )` call style, generous explanatory comments on the *why*.
Keep the zero-warning C89 bar. Prefer editing in place over rewrites.
