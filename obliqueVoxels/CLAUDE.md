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
* **orient** 0/90/180/270 yaws the oblique view around vertical.

## Shading

Per output pixel the renderer finds the world point on the face, then does
Lambert + point-light attenuation + a DDA shadow ray per light (in the rotated
view frame).  Two modes:

1. **Natural** — base color × accumulated (colored) light; may go off-palette.
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

* `obliqueVoxels.c` — pure **C89**: voxel spatial hash, undo history, orbit
  camera, fixed-function 3D preview, mouse→ray picking (Amanatides-Woo DDA),
  the CPU oblique renderer, `.ovox` and PNG I/O, and all the ImGui panel logic
  driven through the shim.
* `gui.h` / `gui.cpp` — thin **C** API over Dear ImGui + SDL2/OpenGL2 backends
  (C++), so the app stays C89.  Adds a color-swatch and drag-to-select palette
  grid widget.
* `stbiw.c` — stb_image_write PNG encoder as its own object (relaxed std).
* `imgui/` — vendored Dear ImGui (only `.o` files are gitignored).

## Controls

* **Left-drag** orbit · **Left-click** paint/erase · **Mid/Right-drag** pan ·
  **Wheel** zoom.
* **B** pencil · **E** erase · **Ctrl+Z / Ctrl+Y** undo / redo.
* Pencil sticks a voxel to the clicked face, or drops onto the ground plane
  (y=0) when the ray misses all voxels.

## File format `.ovox`

Human-readable, one record per line: a header, the embedded palette (`C r g b`),
`AMBIENT`, `L` lights, a `RENDER` params line, then `V x y z color rampStart
rampLen` per voxel.

## Notes

* No threads; launches in ~0.2 s.
* The oblique render recomputes on every edit (`g_renderDirty`); shadow rays are
  capped so large sculptures stay responsive.
* `userNotes.txt` is where testers log issues to fix — check it each session.
