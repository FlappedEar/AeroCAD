# Building AeroCAD

Requirements: CMake 3.24+, Ninja, a C++20 compiler (MSVC 2022, Apple clang 15+, GCC 13+),
Qt 6.4+ with Qt Quick 3D and Qt Shader Tools (CI uses Qt 6.8), Open CASCADE 7.6+
(CI uses 8.0 from vcpkg).

The small libraries (nlohmann_json, pugixml, miniz, Catch2) are taken from the system or
vcpkg if present, otherwise CMake downloads them (FetchContent). miniz is the exception: a
copy of its 3.0.2 release is bundled in `third_party/miniz` (MIT licence), because its
GitHub download kept failing in CI.

## Windows 11 x64

Not built in CI for now; the code and presets are kept portable.

1. Install Visual Studio 2022 (Desktop development with C++), CMake and Ninja.
2. Install Qt 6.8 for MSVC 2022 64-bit with the *Qt Quick 3D* and *Qt Shader Tools* modules
   (Qt online installer or `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtquick3d qtshadertools`).
3. Clone vcpkg and set `VCPKG_ROOT` to it.
4. In a *x64 Native Tools Command Prompt*:

   ```
   set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
   cmake --preset vcpkg-windows
   cmake --build --preset vcpkg-windows
   ctest --test-dir build/vcpkg-windows --output-on-failure
   build\vcpkg-windows\bin\AeroCAD.exe
   ```

   The first configure builds Open CASCADE through vcpkg, which takes a while.

## macOS (Apple Silicon)

See [macos.md](macos.md) for the Homebrew setup and the prebuilt download. The vcpkg build:

```
brew install cmake ninja
# Qt 6.8 for macOS with Qt Quick 3D and Qt Shader Tools, e.g. via aqtinstall:
#   aqt install-qt mac desktop 6.8.3 clang_64 -m qtquick3d qtshadertools
export CMAKE_PREFIX_PATH=~/Qt/6.8.3/macos
export VCPKG_ROOT=~/vcpkg   # git clone https://github.com/microsoft/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
cmake --preset vcpkg-macos
cmake --build --preset vcpkg-macos
ctest --test-dir build/vcpkg-macos --output-on-failure
open build/vcpkg-macos/bin/AeroCAD.app
```

## Linux (Ubuntu 24.04, development only)

```
sudo apt install ninja-build qt6-base-dev qt6-declarative-dev qt6-quick3d-dev qt6-shadertools-dev \
  libxkbcommon-dev qml6-module-qtquick qml6-module-quick3d qml6-module-qtquick-controls \
  qml6-module-qtquick-layouts qml6-module-qtquick-dialogs libocct-data-exchange-dev \
  libocct-modeling-algorithms-dev libtbb-dev nlohmann-json3-dev libpugixml-dev catch2
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/AeroCAD
```

## Tests

- `ctest -LE gui` runs the unit tests only.
- `ctest -L gui` runs the application smoke test: it starts AeroCAD with
  `--smoke-test <file>`, imports the file through the command layer, renders a few frames
  and checks that the model is visible in the viewport. Add `--screenshot out.png` when
  running it by hand to save the frame.

To check a workflow on a real file, `--ui-script <file>` drives the running application:
each line is a JavaScript statement evaluated in `Main.qml`, with a `ui` object for real
mouse events (`ui.click`, `ui.doubleClick`, `ui.drag`, `ui.clickText`), `ui.wheel`, `ui.drop(x, y, [files])` (a file drop), `ui.shot(path)`, `ui.wait(ms)` and
`ui.log`. See `src/app/UiScript.cpp`. The application exits after the last line.

## Command scripts and export validation

`aerocad-cmd` (built next to the app, no Qt needed) runs a JSON array of commands, the
same calls the UI and the MCP server make, and prints one JSON result per line:

```sh
echo '[{"name": "feature.box", "params": {"size": [30, 20, 10]}},
       {"name": "document.export", "params": {"path": "/tmp/box.step"}}]' | ./build/release/bin/aerocad-cmd -
```

`tools/validate_exports.py` builds a few models with it, exports STL, 3MF and STEP, and
reads them back with independent tools: lib3mf (3MF Consortium reference library, strict
mode), trimesh (watertightness, winding, volume, bounds) and gmsh (STEP solids, volume,
bounds). It is not part of CI; run it after changing an exporter:

```sh
python3 -m venv .venv && .venv/bin/pip install lib3mf trimesh numpy networkx lxml gmsh
.venv/bin/python tools/validate_exports.py --cmd build/release/bin/aerocad-cmd
```

All three independent readers are required. The summary reports required, executed,
passed, failed and missing reader invocations per format (plus STEP text checks).
Missing packages, unavailable native libraries, reader exceptions and invalid files
produce a nonzero result; available checks still run, but partial coverage is never
reported as complete success. Install the reported dependencies and rerun the validator.
`--keep DIR` keeps the exported files for inspection. The script remains a local check;
do not add its external readers or slow validation runs to blocking CI.

The validator's failure/coverage logic has a dependency-free regression suite:

```sh
python3 -m unittest discover -s tests/tools -p 'test_validate_exports.py' -v
```
