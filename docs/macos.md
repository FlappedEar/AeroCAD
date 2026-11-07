# Running AeroCAD on a Mac (Apple Silicon)

Build it from source with Homebrew. This exact path is checked on every pull request
by the `CI` workflow (`macOS Apple Silicon (Homebrew)` job). No prebuilt app is
published for now.

## Build from source with Homebrew

Requirements: macOS 13 or newer on Apple Silicon, Xcode Command Line Tools
(`xcode-select --install`) and [Homebrew](https://brew.sh).

```sh
# 1. Tools and libraries (Qt 6 with Qt Quick 3D, Open CASCADE, small libraries)
brew install cmake ninja qt opencascade nlohmann-json pugixml catch2

# 2. Get the code
git clone https://github.com/FlappedEar/AeroCAD.git
cd AeroCAD

# 3. Configure, build, test
cmake --preset release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset release
ctest --test-dir build/release --output-on-failure

# 4. Run
open build/release/bin/AeroCAD.app
# or with a file:
build/release/bin/AeroCAD.app/Contents/MacOS/AeroCAD ~/Downloads/part.step
```

To update later: `git pull`, then repeat step 3.

Use `--preset dev` instead of `--preset release` for a debug build
(output in `build/dev/`).

## Alternative: pinned toolchain (vcpkg + Qt 6.8)

Only needed to test against Qt 6.8 / Open CASCADE 8 or an older macOS deployment
target. Not run in CI. The first configure compiles Open CASCADE (~25 min).

```sh
brew install cmake ninja
pipx install aqtinstall          # or: pip3 install aqtinstall
aqt install-qt mac desktop 6.8.3 clang_64 -m qtquick3d qtshadertools -O ~/Qt
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg
export CMAKE_PREFIX_PATH=~/Qt/6.8.3/macos
cmake --preset vcpkg-macos
cmake --build --preset vcpkg-macos
ctest --test-dir build/vcpkg-macos --output-on-failure
open build/vcpkg-macos/bin/AeroCAD.app
```

## Using the app

- **Import:** File > Import (Cmd+O), or drag a file onto the 3D view.
  Formats: STL, OBJ, PLY (ASCII and binary meshes; point clouds are not supported), 3MF,
  STEP/STP, IGES/IGS, BREP.
- **Navigation** follows Autodesk Fusion's defaults (see [ui.md](ui.md)):
  **pan** = middle drag or two-finger swipe, **orbit** = Shift + middle drag or
  Shift + two-finger swipe, **zoom** = scroll wheel or pinch. Without a middle button,
  the Orbit / Pan / Zoom tools in the navigation bar (bottom of the view) map left drag.
- **ViewCube** (top right of the view): click a face, edge or corner to look from there,
  drag it to orbit, click the house for the Home view, right-click for projection and Home
  settings. **Fit all:** F6 or the Fit button. Standard views in the View menu.
- **Select:** click a part in the view or in the Parts list. The Properties panel
  shows its size and lets you rename, scale or remove it.
- **Region selection** (the toolbar at the top left of the 3D view; the selection is
  shown in orange and listed under *Region selection* in the Properties panel):
  - **Region** (key `1`): click the surface to select the smooth area around the point,
    up to sharp edges. *Max bend* sets how sharp an edge must be to stop it; *Max total*
    limits how far the region may turn away from the clicked spot (for smooth parts
    without edges, such as a duct).
  - **Lasso** (key `2`): drag a loop around the area. Only the visible surface is
    selected unless *Select through* is on.
  - **Brush** (key `3`): paint with the left button. `[` and `]` change the radius.
    *Connected only* keeps the brush from going through thin walls.
  - *New / Add / Remove* decides how the new selection combines with the old one.
    Grow, Shrink, Invert and Clear are in the Properties panel.
  - Navigation stays on the middle button and touchpad (see [ui.md](ui.md)). `Esc` goes
    back to *Navigate*.
- **Base surface from the selection:** in the Properties panel, set a tolerance and click
  *Fit surface to selection*. A new part with one smooth NURBS surface (exact B-rep),
  trimmed to the selection outline, is added. The status bar shows the deviation both
  ways (mesh to surface and surface to mesh) and whether it is within the tolerance.
  Works best on compact regions that do not fold over (less than about 180 degrees of
  wrap); long curved strips get a warning.
- **Offset and thicken:** select a surface part (for example the fitted one). The
  *Surface* group in the Properties panel has *Offset* (a new surface moved along the
  normal by the distance, negative for the other side; use it for clearances) and
  *Thicken to solid* (a closed solid of the given thickness, outside / inside / both
  sides of the surface). The fitted surface's normal points the way the mesh faces.
  *Convert to Body* turns the solid into a timeline body for sketches, fillets, combine.
- **A part on the curved surface (aileron, flap, a mounting base):** after *Fit surface
  to selection* a sketch on the new surface starts by itself (or: *Sketch*, then click the
  scan or a fitted surface); the view turns to the clicked place. What you draw lies on the
  surface; clicks land on the surface point under the cursor from any view, on scans of any
  size. Draw the closed outline and click *Finish Sketch*: the *Part on surface* box offers
  a thickness and a side (inward / outward / both); *Create* makes the solid. On a scan the
  part follows the skin you see under the outline, bridging holes, with edges exactly on the
  outline; no selection or fit is needed. (*Properties > Surface > Trim by sketch*
  gives just the cut-out surface.)
- **A fin on the surface (canard, dive plane, winglet):** *Sketch*, tick *Fin across the
  surface* (optionally an angle), then click the scan where the fin stands. The sketch
  stands across the surface; the orange line is where it cuts the surface. Draw the fin's
  outline with its root a little past that line, click *Finish Sketch*, set the thickness
  in *Fin on surface* and click *Create*: the root is cut to the surface, so the fin sits
  flush on its outer skin (holes and a second layer behind it in the scan are ignored).
  A straight root drawn across a curved surface is carried on down where the surface
  falls away under it, so the fin never hangs in the air at the ends of its chord.
  *Root fillet* (default 2 mm, 0 for none) rounds the joint to the surface; when a radius
  does not fit, the fin is made without it and the status bar says so.
  *Fins* makes several at once (a stacked pair of canards): copies spaced along the sketch
  normal (negative spacing for the other side), each with its root set on the surface
  where it lands. *Both sides* also makes them on the other side of the car, mirrored in
  the scan's own centre plane (found from the scan; or pick the YZ / XZ / XY plane) and
  fitted to the skin there. One undo takes them all back.
- **Print files:** select a solid and click *Export for printing...* (also *File > Export
  for Printing...* and *Export > For Printing*), then pick a folder: every visible solid is
  written as its own `<name>.stl` (finely tessellated, mm) and `<name>.step`. The scan is
  not a solid, so it is left out; hide any solid you do not want. If one is not watertight,
  nothing is written and the message says which. *File > Export* also writes exact parts
  with the fine mesh now.
- **Undo / redo:** Cmd+Z / Cmd+Shift+Z (also for selection changes).

## Troubleshooting

- *"Could not find a package configuration file provided by Qt6"*: pass
  `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"`, or for the vcpkg setup set
  `CMAKE_PREFIX_PATH` to your Qt install.
- To check the renderer without clicking around, run the smoke test:
  `build/release/bin/AeroCAD.app/Contents/MacOS/AeroCAD --smoke-test tests/data/tetrahedron.stl --screenshot /tmp/aerocad.png`
