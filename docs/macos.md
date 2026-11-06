# Running AeroCAD on a Mac (Apple Silicon)

There are two ways: download the app that CI builds, or build it yourself.
Both are checked by CI on every pull request (`macOS Apple Silicon` and
`macOS Apple Silicon (Homebrew)` jobs).

## Option A: download the CI build (no tools needed)

1. Open the repository's **Actions** tab, pick the latest green **CI** run on `main`
   (or on a pull request), scroll to **Artifacts** and download
   **AeroCAD-macOS-ARM64**. You must be signed in to GitHub.
2. Unzip it and open `AeroCAD.dmg`, then drag **AeroCAD** to **Applications**.
3. The build is not notarized by Apple, so the first start is blocked by Gatekeeper.
   Either right-click AeroCAD in Applications, choose **Open** and confirm, or run once:

   ```sh
   xattr -dr com.apple.quarantine /Applications/AeroCAD.app
   ```

Artifacts are kept for 14 days.

## Option B: build from source with Homebrew (recommended for development)

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

## Option C: the exact CI toolchain (vcpkg + Qt 6.8)

Use this only if you need to reproduce the release build. The first configure
compiles Open CASCADE and takes a while.

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
- **Orbit:** left drag. **Pan:** right or middle drag, or Shift + left drag
  (on a trackpad: Shift + click-drag). **Zoom:** scroll wheel, two-finger swipe or pinch.
- **Fit all:** F or double-click. Standard views in the View menu.
- **Select:** click a part in the view or in the Parts list. The Properties panel
  shows its size and lets you rename, scale or remove it.
- **Undo / redo:** Cmd+Z / Cmd+Shift+Z.

## Troubleshooting

- *"Could not find a package configuration file provided by Qt6"*: pass
  `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"` (Option B) or set `CMAKE_PREFIX_PATH`
  to your Qt install (Option C).
- *"AeroCAD is damaged and can't be opened"* for a downloaded build: remove the
  quarantine flag with the `xattr` command above.
- To check the renderer without clicking around, run the smoke test:
  `build/release/bin/AeroCAD.app/Contents/MacOS/AeroCAD --smoke-test tests/data/tetrahedron.stl --screenshot /tmp/aerocad.png`
