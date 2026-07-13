# obliqueVoxels

A voxel-sculpture editor that renders **pure pixel art** from an oblique
top-front projection.  You sculpt with voxels in a free 3D view, then the tool
bakes calculated shading and shadows into a pixel-perfect oblique render you can
export as a PNG.

## The projection (the whole point)

Each voxel maps to at most two pixels' worth of faces:

* its **front** face (a square, `frontH` px tall) and
* its **top** face (a square, `topH` px tall) sitting directly above it.

Depth recedes **straight up** with no horizontal shear: a voxel one step behind
another is drawn directly above it, offset up by `topH` pixels.  This is the
only way both the top square and the front square stay axis-aligned whole
pixels.  A nearer voxel is drawn lower on screen and occludes the ones behind.

Vertical image layout (image y grows downward):

```
bottomY(v,w) = C - v*frontH + w*topH        (v = up, w = toward viewer)
front face rows: [bottomY - frontH, bottomY)
top   face rows: [bottomY - frontH - topH, bottomY - frontH)
```

Occlusion uses a per-pixel depth buffer keyed by `closeness = v*topH + w*frontH`
(nearer wins), so overlaps and shadows are exact.

* **voxel px** = horizontal size of a voxel (and its face width).
* **front/top scrunch** ∈ {1,2,3} foreshortens a face: `faceH = voxPx/scrunch`
  (whole-pixel ratios only — e.g. voxPx 2, top scrunch 2 → 2×1 top squares).
* **orient** 0/90/180/270 yaws the oblique view around vertical.  The four
  named orientations (Front/Right/Back/Left) are aligned to show the SAME face
  as the matching 3D-view preset: Front sees the −z face, Right +x, Back +z,
  Left −x (so `toUVW`/`fromUVW`/`g_frontDir` and the light rotation all agree).

## Shading

Per output pixel the renderer finds the world point on the face, then does
Lambert + point-light attenuation + a DDA shadow ray per light (in the rotated
view frame).  A light's **size** slider (0 = hard point/sun) spreads its shadow
rays over a sphere of source points (a jittered direction for a sun), producing
a soft penumbra like a soft-box.  A separate **soft rays** slider (1..64) sets
exactly how many rays are spent — radius and cost are decoupled, so a wide
penumbra can stay cheap.
Two modes:

1. **Natural** — base color × accumulated (colored) light; may go off-palette.
   The base color is the ramp's mid (non-black) sample via `voxFlatColor`, so a
   ramp whose first index is black still renders lit, not black.
2. **Palette ramp** — brightness indexes the voxel's contiguous palette ramp
   (dragged out on the palette grid).  Auto-oriented by luminance so the dark
   end always maps to the least-lit sample.  A 1-color ramp is flat.

## Build & run

```
make                # gcc -std=c89 app + g++ imgui shim, links SDL2/GLU/GL
./obliqueVoxels [file.ovox]
./shot.sh out.png [args]           # headless Xvfb screenshot
OV_QUIT=N ./obliqueVoxels          # auto-quit after N frames (launch timing)
OV_EXPORT=out.png OV_QUIT=30 ...   # auto-export the oblique render on quit
```

## Layout

* `obliqueVoxels.c` — pure **C89**: voxel spatial hash, grouped undo history,
  orbit camera, fixed-function 3D preview (with a cached real-lit face list for
  "match render" mode), mouse→ray picking (Amanatides-Woo DDA), plane-locked
  region-gesture tools (line/rect/box) + marquee selection & clipboard, the CPU
  oblique renderer, `.ovox` and PNG I/O, and all the ImGui panel logic driven
  through the shim.
* `gui.h` / `gui.cpp` — thin **C** API over Dear ImGui + SDL2/OpenGL2 backends
  (C++), so the app stays C89.  Adds a color-swatch and drag-to-select palette
  grid widget.
* `stbiw.c` — stb_image_write PNG encoder as its own object (relaxed std).
* `stbi.c` — stb_image PNG *reader* (PNG-only) as its own object (relaxed
  std), used by the "import PNG as voxel wall" tool.
* `fs.c` / `fs.h` — POSIX directory listing (opendir/stat/getcwd) for the file
  browser, in its own relaxed-std unit so the main app stays C89.
* `imgui/` — vendored Dear ImGui (only `.o` files are gitignored).

## Controls

* **Right-drag** orbit · **Mid-drag** pan · **Wheel** zoom.
* **Left-click** (Pencil) place/erase · **Left-drag** (Line/Rect/Box/Select)
  region gesture with a live ghost, committed as one undo step.
* Tools **1** Pencil · **2** Line · **3** Rect · **4** Box · **5** Select ·
  **6** Scribble (paint a selection over whatever voxels the drag touches;
  erase mode un-paints) · **7** Cylinder · **8** Sphere · **9** Smoothers ·
  **Image wall** (no shortcut; armed by File ▸ Import PNG as voxel wall).
* Modes **B** draw · **E** erase — every tool obeys the mode (erase a whole
  line/box, marquee-deselect, etc.).
* **thickness** slider extrudes Line/Rect/Box/Cylinder along the started face's
  normal, so Box is a solid rectangular prism in one drag.  **Cylinder** is the
  same drag but its footprint is the inscribed (jaggy) ellipse.
* **Sphere** drags a perfect ball out of the clicked surface; a **sphere depth**
  slider sinks the ball into the surface (dome at depth≈radius; erase digs a
  crater).  The ball grows with drag distance.
* **Smoothers** are translucent anti-stair-step markers placed like a 1-thick
  Line into empty cells (skipping any voxel the line crosses; erase removes
  them).  A smoother averages the voxel faces it touches into one uniform patch
  (a mid-ramp color in palette mode, the brighter of two adjacent), killing the
  bright-top/dark-front stripes on curved surfaces.  They only show while the
  Smoothers tool is active, and vanish if a voxel later covers them.  The effect
  appears in both the oblique render and the "match render" 3D preview.
* **Image wall** imports a PNG (with alpha) as a flat wall of voxels, one flat
  single-colour voxel per opaque pixel (nearest-palette colour; alpha < 128 is
  skipped).  Placement is a three-click gesture: click 1 fixes the bottom-left
  corner cell, click 2 aims the image-width axis, click 3 aims a perpendicular
  image-height axis and commits.  Both axes snap to one of the six face
  directions by matching the cursor's on-screen direction away from the corner
  (a `worldToScreen` gizmo projection), so a vertical +Y wall or a flat floor
  is reachable from any camera.  Stage 1 ghosts a translucent column of
  image-width voxels; stage 2 ghosts the whole wall tinted per pixel.  The
  wall commits as one undo step; **Esc** resets the placement, and the image
  stays loaded so you can stamp more copies.  The image itself is not saved in
  `.ovox` — only the placed voxels are.
* Selection: marquee-drag adds voxels (erase mode removes), single-click
  toggles one.  The Select tool has **below/above** depth sliders that sweep the
  marquee down into the solid (and up out of it) from the clicked surface.
  **Ctrl+A** all · **Esc** clear · **Del** delete · Copy/Recolor and
  **Ctrl+C / Ctrl+V** paste (at an editable x,y,z offset).  **Invert** selects
  every unselected voxel.  **Extrude** sweeps the selected shape along a chosen
  axis (±X/±Y/±Z) by a distance, filling a prism whose cross-section is the
  selection (copies inherit each source voxel's color/ramp) — the whole
  extruded volume becomes the new selection so it can be repeated.
* Drag the thin handle at either **panel/view boundary** to resize the side
  panels; the palette grid re-wraps to the new width.
* **Ctrl+Z / Ctrl+Y** undo / redo (whole gesture at a time).
* Clicks are gated on the real 3D-viewport rect + an immediate popup-open
  check (not ImGui's one-frame-late hover), so nothing leaks between panels
  and the view.

## File format `.ovox`

Human-readable, one record per line: a header, the embedded palette (`C r g b`),
`AMBIENT`, `L` lights, a `RENDER` params line, then `V x y z color rampStart
rampLen` per voxel.  A light line is `L x y z color intensity enabled [infinite [size [samples]]]`
— the trailing `infinite` flag (1 = directional "sun", parallel rays and no
distance falloff, with x,y,z read as a direction), `size` (soft-light radius,
0 = hard) and `samples` (soft-shadow ray count, default 8) are optional so older
files still load.  Trailing `S x y z` lines record smoother cells.  **File ▸ Import
lighting** reads just the `AMBIENT`/`L` lines from another `.ovox` and replaces
the current scene's lighting (for lighting a matched set identically).

## Notes

* No threads; launches in ~0.2 s.
* The oblique render recomputes on every edit (`g_renderDirty`); shadow rays are
  capped so large sculptures stay responsive.
* `userNotes.txt` is where testers log issues to fix — check it each session.
