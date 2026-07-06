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
make                    # builds ./cwave ; *.o and cwave are gitignored
./cwave [file.wav|file.ogg]
CWAVE_PROFILE=1 ./cwave  # print a per-phase startup timing breakdown to stderr
CWAVE_QUIT=N ./cwave     # auto-quit after N presented frames (N<1 => 1), e.g.
                         # `time CWAVE_QUIT=1 ./cwave` times launch-to-1st-frame
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

For pure-logic correctness (e.g. the `Sequence` block list), a standalone C
test that links `audio.o` and diffs the block-list result against a naive
contiguous reference is faster and more rigorous than eyeballing screenshots —
see `fuzz.c` (repo-local, gitignored) (random insert/delete/splice/effect fuzz, bit-exact,
also runnable under valgrind and with a tiny `-DSEQ_BLOCK_FRAMES` to stress
splitting/coalescing). Do both.

## File map

- `cwave.c` — everything UI/app: per-document state (`Doc`, see Tabs below),
  block-list rendering, playback (SDL audio callback), editing actions, effects
  dispatch, marks, file browser, ImGui panels, main loop. Big file, well-
  sectioned by banners.
- `audio.c` / `audio.h` — `AudioClip` (planar float32 per channel, `[-1,1]`, a
  *contiguous* leaf buffer), the in-place DSP primitives (normalize, amplify,
  fade, silence, reverse, peak), WAV/OGG load, WAV save — AND the `Sequence`
  block-list document (the editable audio), see below.
- `gui.cpp` / `gui.h` — the C↔ImGui shim.
- `Makefile`, `shot.sh`, `shot2.sh`, `README.md`, `userNotes.txt`.

## Architecture notes & invariants

**Tabs — multiple open documents.** The app holds a **heap-grown** array
`Doc *g_docs` (capacity `g_docsCap`, doubled on demand by `ensureDocCap` — there
is **no hard limit** on open files) with `g_numDocs` / `g_curDoc`; each `Doc` is
one open file/tab and owns *all*
per-document state — its `Sequence clip`, marks, selection, view (viewStart /
samplesPerPixel), undo/redo chains, `path`, `dirty`, and `fmt` (its WAV save
format). The huge body of editing/rendering code is unchanged because it still
writes `app.foo`, but `app` is now a **macro**: `#define app (g_docs[g_curDoc])`.
So "the current document" is just an array index; **switching tabs is O(1)** —
nothing is rebuilt (each Doc already carries its own block-summary bins, undo
chain, etc.). Anything genuinely *global* (not per-document) lives in the `ui`
struct — most importantly the **clipboard** (shared, so copy/paste crosses
tabs), plus dialog scratch, the file browser, and the New-file params.
`buildTabBar()` draws the ImGui tab strip (`gui_begin_tab_bar`/`gui_tab_item`)
in a fixed panel of height `TABBAR_H` between the menu bar and the overview
(renderWaveform reserves the same room). Selecting a tab → `switchTab`; the
per-tab close button / Ctrl+W → `closeDoc`. Keyboard tab nav: **left/right**
cycle with wrap-around, **up/down** jump to the first/last tab (all via
`switchTab`). **Invariants:** (1) any edit still goes through `app` and is
bracketed by the audio lock; (2) `switchTab`/`closeDoc` `stopPlayback()` first,
because `player.clip` points at a specific `Doc`'s `&clip` and `closeDoc` shifts
the `Doc` array (struct-copy, which moves the heap pointers with it) — playback
must not be reading a doc that is about to move or be freed; the same hazard
applies to `ensureDocCap`'s `realloc` (it may move/free the whole array), so it
moves the buffer **under the audio lock** and re-points `player.clip` at
`&g_docs[g_curDoc].clip` in the new array; (3) there is always
≥1 doc (closing the last replaces it with a fresh empty one). **`switchTab`
keeps transport live:** if playback was running it stops the callback (so it
never reads the old doc), switches, then *restarts* in the new tab from its
cursor/selection (looping the selection if one exists, `player.loop` carries
over) — so a user can open a folder of samples, hit Play once, and arrow through
them all in turn. Opening/creating a file **reuses a blank current tab**
(untitled + clean + empty, `isDocBlank`) instead of piling on another empty tab,
else it appends a new one. `openFile()` creates/focuses the target tab
immediately (showing the filename) and the async loader fills it in
`finishLoad`, targeting `loadTargetDoc` so a mid-load tab switch still lands the
audio in the right tab. **Command-line files open into separate tabs:** every
argv path (a shell wildcard expands to many) is queued in `g_pendingOpen[]`
(malloc'd to hold every argv entry — no cap), and
`pumpPendingOpens()` (called each main-loop iteration) feeds them one at a time
to the single-flight async loader; once the whole batch is in, focus lands on
tab 0. Programmatic tab focus uses `g_forceSelectDoc`, and `buildTabBar` lets
that force **win over the previously-selected tab's stale "active" return** (an
ImGui `SetSelected` only takes effect next frame) so a scripted switch isn't
immediately dragged back.

**New dialog & save formats.** "New" opens a dialog (channels / sample-rate
preset / sample format) → `createNewDoc`. Internally audio is always float32;
`Doc.fmt` (`FMT_PCM16/24/8/F32`) is only a *save* preference, defaulted from the
loaded file's bit depth (`audio_load_progress` now reports `outBits`/`outFloat`)
and overridable in the Save-As dialog's Format combo. `seq_save_wav_fmt`
(8-bit unsigned, 16/24/32-int signed LE, or 32-float) does the encoding;
`seq_save_wav` is now just the 16-bit wrapper. Cross-format / cross-rate paste
needs no conversion: `seq_insert_clip` keeps the destination's channel count &
rate and maps clipboard channels (extra dest channels duplicate source ch0), so
pasting between differently-formatted tabs "just works". **Geometry adoption is
opt-in per doc via `Doc.adoptGeom`:** only a *throwaway blank* tab (startup /
after-close, `adoptGeom=1`) takes on the clipboard's channels+rate on paste; a
New-dialog doc (`createNewDoc` clears the flag) or a loaded file (`finishLoad`
clears it) **preserves** its chosen geometry and channel-maps the source into it
instead. `actPaste` does the adoption (calls `seq_set_empty` + `open_audio` when
`clipLen()==0 && adoptGeom` and the geometry differs). `seq_insert_clip` itself
now only auto-adopts when the sequence has *no* geometry at all
(`numChannels==0`), so an empty-but-configured doc is never clobbered.

**Audio clip.** `AudioClip` is planar: one `float*` per channel, `numFrames`
samples each, values in `[-1,1]`. It is now the *leaf buffer* type — used for
individual `Sequence` blocks, the copy/paste clipboard, undo deltas, and
load/save scratch. Its DSP primitives (`audio_amplify`, `audio_silence`,
`audio_reverse`, `audio_peak`) run per-block inside the `Sequence` effects.

**Sequence — the block-list document (THE core data model).** The editable
audio (`app.clip`, and the load worker's result) is a `Sequence`: an ordered
array of `Block*`, each block owning a bounded contiguous `AudioClip`
(≤ `SEQ_BLOCK_FRAMES`, ~256K frames) **plus its own min/max summary bins**
(one bin per `SEQ_BIN`=256 frames). This is what makes paste/delete/cut
**position-independent** — measured ~0.4–4.8 ms at start/middle/end of the
28-min `glz.wav`, vs. up to 800 ms before. Why both costs vanish:
- *Structural edit* (`seq_insert_clip`/`seq_delete_range`/`seq_splice`): only
  the ≤2 boundary blocks are split (a bounded copy via `seq_split_at`) and the
  small `Block*` array is relinked; unchanged blocks are untouched. No O(tail)
  `memmove`.
- *Overview*: each block keeps its own bins, so a shift never invalidates
  downstream summaries — there is **no global pyramid to rebuild** (the old
  `WaveCache`/`waveGen`/`bumpWave` machinery is GONE). Rendering composites
  block bins directly via `seq_col_minmax`, which costs ≤ `numFrames/256`
  bin-reads per frame (trivial even zoomed all the way out).
`seq_reindex` keeps `start[i]` (absolute frame of block i) + `numFrames`
current after every mutation. `seq_coalesce_around` merges undersized adjacent
blocks after an edit so repeated small pastes (train cars) don't fragment the
list into thousands of tiny blocks (verified: 20 000 end-pastes → 153 blocks).
**Invariant: every `Sequence` mutation must leave `start[]`/`numFrames` and each
touched block's bins consistent** — verified bit-exact vs. a naive contiguous
reference by `fuzz.c` (repo-local, gitignored) (also run under valgrind, and byte-exact WAV
save). If you add a length-preserving effect, do it per-block and call
`block_bins()` on each touched block (or go read→`audio_*`→write via
`seq_write_range`, which refreshes bins for you).

**Playback.** **Audio is opened lazily** — `SDL_Init` requests VIDEO only, and
neither the SDL audio subsystem nor a device exists until the *first* playback.
`playFrom` calls `ensureAudio(rate)` (→ `SDL_InitSubSystem(AUDIO)` once, then
`open_audio`); a blank editor never pays the sound-server (PulseAudio/PipeWire)
connect cost — that was a big avoidable chunk of the "launch instantly" budget.
Everything else that used to `open_audio` eagerly (`switchTab`, `closeDoc`,
`createNewDoc`, `actPaste` geom-adopt, `finishLoad`) now does so **only if
`audioSubsysReady`** — i.e. it *reconfigures* an already-open device to the new
rate but never spins audio up; the next `playFrom` opens it if needed. Editing
works fully with no audio device at all. (Startup is profileable:
`CWAVE_PROFILE=1 ./cwave` prints a per-phase stderr breakdown.)
One SDL audio callback thread mixes all channels → stereo with a
volume. Because the clip is a block list, the callback keeps a **cursor** —
`seq_locate(playhead)` once per callback (cheap binary search, ~once/1024
frames) then advances (block idx + local offset), crossing block boundaries and
re-seeking on a loop wrap. `Player player` holds playhead/range/flags.
**Invariant: any edit that mutates or frees clip data must do so under
`audio_lock()/audio_unlock()`** so the callback never touches freed block
memory. Edits no longer `stopPlayback()` — see "Playback survives edits".
The on-screen playhead only advances once per callback (the display samples it
per video frame), so `open_audio` sizes the SDL buffer to keep the callback
period ~constant (~20 ms) across sample rates — pow2 nearest `rate/50`, clamped
`[128,2048]`. A fixed 1024-sample buffer looked smooth at 44.1 kHz but jerky at
8 kHz (128 ms/callback); scaling it fixed that. **Loop follows the live
selection:** the main-loop sync block re-points `playStart/End` at the current
selection whenever a selection exists AND (`followSel` OR `player.loop`), so
checking Loop and then selecting mid-playback redefines the loop region at once
without a Stop/Play cycle.

**Async load.** Large files decode on a short-lived pthread (`loadWorker`) into
a contiguous `AudioClip`, then `seq_adopt_clip` splits that into blocks +
per-block bins (phase 1, "Building overview"); the main loop polls progress and
swaps the finished `Sequence` in under the audio lock. (Adopt copies the decoded
clip into blocks then frees it, so load briefly holds ~2× the audio in RAM — an
accepted tradeoff; edits, not load, were the perf complaint.)

**Undo/redo — delta records, NOT whole-clip copies.** Each `UndoRec` is a
self-reversing *splice*: it stores only the frames at `[at,at+oldLen)` before
the edit (`oldData`) and `[at,at+newLen)` after (`newData`). So undo cost
scales with the *edit size*, not the file length — the whole point, since the
old whole-clip snapshot made every edit O(numFrames) (the real cause of "edits
are slow on long files"). A record ALSO snapshots the *editor* state (selection
+ a malloc'd copy of the whole mark table) as it stood **before** and **after**
the edit, so undo/redo restore not just the audio but exactly what was selected
and which marks existed — undoing a cut brings its selection back, undoing a
paste removes the seam marks it created (`selBefore*/selAfter*`, `marksBefore/
marksAfter`, freed in `undoRecFree`). Edit actions bracket the mutation with
`beginEdit(at,oldLen)` (called FIRST — snapshots the span about to change AND the
pre-edit selection/marks) … `commitEdit(newLen)` (called LAST, after the action
has set the final selection/marks — snapshots the new span AND the post-edit
selection/marks, then pushes the record). **Invariant: `commitEdit` must be the
last thing an edit action does, after selection and marks are in their final
state**, or the redo snapshot will be stale. `doUndo`/`doRedo` call
`seq_splice` with the record's data, move the record between the undo/redo
stacks (no re-copy), then restore the before/after selection + marks via
`restoreMarks`. **Invariant: `beginEdit`'s `[at,oldLen)` must exactly cover
every frame the edit changes, and `commitEdit`'s `newLen` the resulting span.**
The `Sequence` keeps every block's summary bins current, so undo/redo need no
overview bookkeeping at all. Trim is the one still-O(n) case (its record holds
the whole pre-trim clip — `beginEdit(0, clipLen())`); it is implemented as two
`seq_delete_range`s (drop the tail then the head), each itself O(1)-ish, so only
the *undo snapshot copy* is O(n), not the edit. `clearUndoRedo()` on load/new.
`captureRange` flattens the undo span out of the block list via `seq_read_range`
and steals its channel pointers.

**`seq_splice`** (in `audio.c`) is the single primitive behind undo/redo:
`seq_splice(at, removeLen, ins[], insLen)` = a `seq_delete_range` then a
`seq_insert_clip`, both block relinks (no O(tail) copy). Structural edit actions
call `seq_delete_range` / `seq_insert_clip` directly.

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
