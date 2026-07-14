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

**Smooth self-shadow suppression:** when a shadow ray for a *smooth* face is
blocked by a voxel in the receiver's own 3×3×3 neighbourhood whose every
*visible* face is also smooth (part of the same contiguous curved surface),
that occlusion is ignored (the DDA steps past it).  This kills the dark stripe a
smooth voxel would otherwise cast onto its immediate smooth neighbour as the
fitted surface curves away from the light, while more distant protrusions — and
any occluder that shows a flat facet — still cast real shadows onto the smooth
surface.  (`shadowedWorld` reads the receiver cell / face-smoothness that
`shadeWorld` publishes into `g_shadRecv*`.)
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

* `obliqueVoxels.c` — pure **C89**: layered voxel spatial hash (per-layer
  hashes unioned into a composite for render/pick), grouped undo history,
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
* **Left-click** (Pencil) place/erase · **Left-drag** (Pencil) scribble: paints a
  translucent ghost block onto every real surface the cursor sweeps (each sticks
  to the closest *real* voxel face or the ground, never ghost-on-ghost),
  committed as one undo step on release — left-drag never orbits (right-drag
  does).  **Left-drag** (Line/Rect/Box/Select) region gesture with a live ghost,
  committed as one undo step.
* Tools **1** Pencil · **2** Line · **3** Rect · **4** Box · **5** Select ·
  **6** Scribble (paint a selection over whatever voxels the drag touches;
  erase mode un-paints) · **7** Cylinder · **8** Sphere · **9** Smoother
  (paint per-face smoothness over whatever faces the drag touches; erase
  un-smooths) · **0** Eyedropper (click a voxel to load its color/ramp into the
  current paint color) · **Image wall** (no shortcut; armed by File ▸ Import PNG
  as voxel wall).
* **Layers** (Tools panel): up to 16 independent voxel layers, listed top
  (highest z-order) first.  Every edit acts on the single **active** layer
  (click a layer's name to activate it); the oblique render, the 3D preview and
  mouse picking all act on the **composite** — every *visible* layer unioned,
  with a higher layer winning where two layers occupy the same cell (a union
  performed before shading, so overlaps shade exactly).  Each row has a
  visibility checkbox; **+ Add** / **Delete** / **Up** / **Down** (reorder the
  z-order) / a **name** field.  Undo is per-layer (each recorded edit remembers
  its layer and is replayed there); a *structural* change (add/delete/reorder a
  layer) clears the undo history since history can't span a layer-set change.
  Implementation: `g_vox`/`g_voxCap`/`g_voxUsed`/`g_voxTomb` are macros aliasing
  the *active* layer's spatial-hash fields, so every existing edit and render
  loop runs unchanged; a render or pick pass calls `ensureFlat()` to union the
  visible layers into a reserved `FLAT_LAYER` scratch slot and temporarily
  points `g_activeLayer` at it, so the same shading/occlusion/DDA code sees the
  composite with no per-call branching.
* **Cross-layer selection** is kept live *positionally*: switching the active
  layer projects the set of selected cell positions onto whatever the new layer
  has at those cells.  So selecting a sphere in one layer then switching to a
  box layer leaves the sphere∩box overlap selected — enabling boolean gestures
  (intersection by switching, union by Copy→switch→Paste-at-0, difference by
  Delete on the projected selection).
* **Symmetry planes** (Tools panel): checkboxes **mirror X/Y/Z** mirror every
  edit — draw, erase, select, smooth, sphere — across up to three planes at
  once (so up to 8-fold symmetry).  Each enabled axis gets a **pos** slider (the
  plane sits at that integer cell boundary) and a **+0.5** checkbox that shifts
  it to the centre of that column of cells, so an odd-width shape stays
  symmetric about its middle voxel.  A smoothed face mirrors to the correct
  opposite face; a cell that maps to itself (centre column) is edited only once.
* **Move selection** (Selection panel): **mx/my/mz** sliders translate the
  selection live — a green ghost previews where it lands while the originals
  stay put — and **Commit Move** bakes it as one undo step (overwriting any
  colliding voxels, which undo restores), carrying each voxel's color/ramp and
  per-face smooth mask.  **Reset Move** zeroes the offset without committing.
* **Live drag hit-marks**: while dragging a delete region (line/rect/box/
  cylinder/sphere in Erase mode) or a Select marquee, every *existing* voxel the
  region intersects gets a wire outline plus an X across its faces — red for
  delete, yellow for select — so what will be affected reads clearly even where
  the translucent ghost hides it behind solid voxels.
* Any slider accepts **Ctrl+click to type** an exact value (and typed values may
  exceed the slider's range).
* Modes **B** draw · **E** erase — every tool obeys the mode (erase a whole
  line/box, marquee-deselect, etc.).
* **thickness** slider extrudes Line/Rect/Box/Cylinder along the started face's
  normal, so Box is a solid rectangular prism in one drag.  **Cylinder** is the
  same drag but its footprint is the inscribed (jaggy) ellipse.
* **Sphere** drags a perfect ball out of the clicked surface; a **sphere depth**
  slider sinks the ball into the surface (dome at depth≈radius; erase digs a
  crater).  The ball grows with drag distance.
* **Smooth shading is per-FACE.**  Each voxel carries a 6-bit mask
  (`smoothFaces`) of which of its faces are smooth; only *visible* faces
  actually shade, but all six are remembered so a face exposed later by an edit
  is already set.  A **smooth** face is shaded not with its blocky axis normal
  but with the *fitted surface normal* — the negated gradient of the local
  solid-occupancy field over a `smooth radius` neighbourhood (a "curve of best
  fit"), so a voxel sphere shades like a real sphere instead of showing
  bright-top/dark-front stripes.  Two global render params tune it: **smooth
  radius** (1–4) sets how broad the fit is, **smooth amount** (0–1) blends
  between the flat face normal and the fitted normal.  The effect appears in
  both the oblique render and the "match render" 3D preview.  (Only the
  *shading* normal changes — the voxel geometry, occlusion and shadow-ray
  origins are untouched.)
* **Meeting constraints** are what make corners work automatically, with no
  separate "corner" state.  A smooth face is "met" to its four in-plane
  neighbour faces (found by the local surface: a *coplanar* face if the next
  cell is solid & open, a *perpendicular* face on this voxel at a convex edge,
  or a perpendicular face on the diagonal cell at a concave edge).  Where a
  neighbour face is **not** smooth, the fitted normal is constrained so the two
  faces meet sanely: a non-smooth **coplanar** neighbour locks us fully flat
  (we abut a flat run); a non-smooth **perpendicular** neighbour zeroes the
  tangent component along its axis (keeping us at 90° to it while free to rotate
  about the other in-plane axis).  So a flat washer ring whose rim faces are
  smooth but whose top/bottom faces are flat rounds only circumferentially (the
  flat caps pin the vertical tangent), and a cylinder's top rim keeps a crisp
  flat cap edge with no dark patch.  A **bevel sign-guard** additionally fixes
  "inner corner" voxels: at a near-symmetric corner the fitted gradient's
  primary signal cancels and far cells can tip its in-plane component the *wrong*
  way (a shading discontinuity).  So the meeting scan also builds a purely
  geometric bevel direction — the flat face normal plus the outward normal of
  every *smooth* perpendicular neighbour it rounds toward — and where the fitted
  normal's tangent points opposite that geometry, that component is flipped to
  agree.  Coplanar smooth surfaces (spheres) accumulate no bevel, so they are
  untouched.
* Set smoothness two ways: the **Smoother** tool (9) paints it face-by-face —
  drag over faces to mark them smooth (Draw) or flat (Erase); or, in the
  Selection panel, **Smooth** / **Unsmooth** set/clear *all six* faces of every
  selected voxel at once.  Either way it is undoable and saved in `.ovox`.  An
  **auto-smooth new faces** checkbox (under Draw/Erase, for the drawing tools)
  marks every face of each placed voxel smooth as you draw — so you can rough
  out a smooth sphere directly — and, on erase, marks the newly-exposed cavity
  faces smooth (carve a smooth hollow).
* Both the oblique renderer and the "match render" 3D preview now shade in true
  **world space** at the same surface point, so a voxel's baked pixels and its
  3D-preview faces always agree (an earlier mismatch came from the renderer
  shading in the negated-axis view frame, which shifted local-light distances).
* **Display** menu toggles: preview shading mode, voxel edges, **Smooth faces
  (cyan X)** which draws a cyan outline and an X across every visible smooth
  face, **Surface normals** (every visible face gets a translucent tile lying in
  the plane perpendicular to the shading normal it will actually use — tilted
  cyan for a smooth face, square grey for a flat one — plus a spike along that
  normal, so "how is this face shaded?" reads at a glance while tuning smooth
  radius/amount), and **Hide voxels (faces only)** which stops drawing the solid
  voxel faces so those surface tiles/normals can be inspected on their own.  The
  tiles/spikes are lifted just above their faces and drawn with the depth test
  ON, so near patches correctly occlude far ones (no jumbled bleed-through).
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
  toggles one.  The Select tool's marquee sits **in the layer of voxels you drag
  on** (the clicked solid cell, not the empty cell above it); the **below/above**
  depth sliders then sweep it *below* (into the solid) and *above* (out of the
  surface) that layer.  This anchoring is identical in Draw and Erase mode — so
  dragging Erase over the same spot with the same below/above deselects exactly
  the voxels a Draw drag there would select (Erase no longer mirrors the sweep).
  **Ctrl+A** all · **Esc** clear · **Del** delete · Copy/Recolor and
  **Ctrl+C / Ctrl+V** paste (at an editable x,y,z offset).  **Invert** selects
  every unselected voxel.  **Extrude** sweeps the selected shape along a chosen
  axis (±X/±Y/±Z) by a distance, filling a prism whose cross-section is the
  selection (copies inherit each source voxel's color/ramp) — the whole
  extruded volume becomes the new selection so it can be repeated.
* Drag the thin handle at either **panel/view boundary** to resize the side
  panels; the palette grid re-wraps to the new width.  (The handles are hidden
  while a menu/popup is open so they can't paint over an open dropdown's text.)
* The **oblique-render preview** (right panel) is centred in its area.  The
  **zoom** slider magnifies about the centre; the **scroll wheel** over the
  preview zooms about the cursor; **left-drag** pans; **reset** recentres and
  restores the default zoom.
* **Ctrl+Z / Ctrl+Y** undo / redo (whole gesture at a time).
* Clicks are gated on the real 3D-viewport rect + an immediate popup-open
  check (not ImGui's one-frame-late hover), so nothing leaks between panels
  and the view.

## File format `.ovox`

Human-readable, one record per line: a header (`OBLIQUEVOXELS <version>`), the
embedded palette (`C r g b`), `AMBIENT`, `L` lights, a `RENDER` params line,
an `ACTIVE <layerIndex>` line, then for each layer a `LAYER <index> <visible>
<name...>` line (the name runs to end of line and may contain spaces) followed
by that layer's `V x y z color rampStart rampLen [smoothFaceMask]` voxel lines —
the trailing smooth field is optional so older 6-field voxel lines still load.
This is **version 3**; older **version 1/2** files have no `LAYER`/`ACTIVE`
lines and load as a single layer.  In the current
**version 2** format it is a 6-bit per-face smooth mask (bit = 1<<faceDir6,
order +Y +Z −Z +X −X … i.e. +Y0 −Y1 +Z2 −Z3 +X4 −X5); in the legacy **version
1** format it was a 0/1/2 whole-voxel smooth flag, which loads by mapping any
nonzero value to "all six faces smooth" (`0x3F`).  A light
line is `L x y z color intensity enabled [infinite [size [samples]]]` — the
trailing `infinite` flag (1 = directional "sun", parallel rays and no distance
falloff, with x,y,z read as a direction), `size` (soft-light radius, 0 = hard)
and `samples` (soft-shadow ray count, default 8) are optional so older files
still load.  The `RENDER` line is `RENDER shadingMode voxPx frontScrunch
topScrunch orient [smoothRadius [smoothAmount]]` (the two smoothing params are
optional trailing fields).  (Legacy `S x y z` smoother-cell lines from older
builds are silently ignored.)  **File ▸ Import lighting** reads just the
`AMBIENT`/`L` lines from another `.ovox` and replaces the current scene's
lighting (for lighting a matched set identically).

## Notes

* No threads; launches in ~0.2 s.
* The oblique render recomputes on every edit (`g_renderDirty`); shadow rays are
  capped so large sculptures stay responsive.
* `userNotes.txt` is where testers log issues to fix — check it each session.
