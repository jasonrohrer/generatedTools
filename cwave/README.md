# cwave

A fast, stable graphical audio editor for Linux — think Audacity /
OceanAudio / Sound Forge, but small and instant to launch.

cwave is an *editor*, not a DAW: it plays back and edits existing audio
files.  It does not record.

## Features

- Loads **WAV** (8/16/24/32-bit PCM and 32-bit float) and **OGG Vorbis**.
- Saves **WAV** (16-bit PCM by default; 32-bit float supported in code).
- Multi-channel waveform display with peak (min/max) rendering when
  zoomed out and connected per-sample rendering (with sample dots) when
  zoomed in.
- Mouse selection with time / frame readout; shift-click to extend.
- Mouse-wheel zoom centered on the cursor; Fit / Zoom-to-selection.
- Playback of the whole file, the selection, or from the cursor, with
  looping and a volume control.  All channels are mixed down to stereo.
- Editing: cut / copy / paste / delete / trim-to-selection, with a
  32-level undo/redo history.
- Effects: Normalize, Amplify, Fade In, Fade Out, Silence, Reverse.
  Effects act on the selection, or the whole file if nothing is selected.

## Building

Dependencies: SDL2, OpenGL (Mesa), and a C/C++ compiler.  Dear ImGui and
`stb_vorbis.c` are compiled in from this tree.

```
make
./cwave [file.wav|file.ogg]
```

## Design notes

- The application code (`cwave.c`, `audio.c`) is **pure C89**.  Dear ImGui
  is C++, so it is wrapped by a thin C shim (`gui.h` / `gui.cpp`); all
  application logic stays in C.
- The waveform is drawn with **legacy fixed-function OpenGL**, paired with
  ImGui's `opengl2` backend, so there is no shader compilation and startup
  is instant.
- Audio output uses one SDL2 audio callback thread.  Edits stop playback
  and mutate the clip under the audio lock, so the callback never touches
  freed memory.
- WAV reading/writing is hand-rolled.  OGG is decoded via `stb_vorbis`.

## Keyboard shortcuts

| Key            | Action                       |
|----------------|------------------------------|
| Space          | Play / Stop                  |
| Del / Backspace| Delete selection             |
| F              | Fit file in window           |
| + / -          | Zoom in / out                |
| Home / End     | Cursor to start / end        |
| Esc            | Clear selection              |
| Ctrl+O/S/N     | Open / Save / New            |
| Ctrl+Z/Y       | Undo / Redo                  |
| Ctrl+X/C/V     | Cut / Copy / Paste           |
| Ctrl+A         | Select all                   |

## Known limitations

- **OGG export is not supported.**  `stb_vorbis` decodes OGG but cannot
  encode it, and no other encoder is linked, so files are saved as WAV.
