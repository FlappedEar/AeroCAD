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
  Formats: STL, OBJ, 3MF, STEP/STP, IGES/IGS, BREP.
- **Navigation** follows Autodesk Fusion's defaults (see [ui.md](ui.md)):
  **pan** = middle drag or two-finger swipe, **orbit** = Shift + middle drag or
  Shift + two-finger swipe, **zoom** = scroll wheel or pinch. Without a middle button,
  the Orbit / Pan / Zoom tools in the navigation bar (bottom of the view) map left drag.
- **ViewCube** (top right of the view): click a face, edge or corner to look from there,
  drag it to orbit, click the house for the Home view, right-click for projection and Home
  settings. **Fit all:** F6 or the Fit button. Standard views in the View menu.
- **Select:** click a part in the view or in the Parts list. The Properties panel
  shows its size and lets you rename, scale or remove it.
- **Undo / redo:** Cmd+Z / Cmd+Shift+Z.

## Troubleshooting

- *"Could not find a package configuration file provided by Qt6"*: pass
  `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"`, or for the vcpkg setup set
  `CMAKE_PREFIX_PATH` to your Qt install.
- To check the renderer without clicking around, run the smoke test:
  `build/release/bin/AeroCAD.app/Contents/MacOS/AeroCAD --smoke-test tests/data/tetrahedron.stl --screenshot /tmp/aerocad.png`
