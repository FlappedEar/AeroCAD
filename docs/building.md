# Building AeroCAD

Requirements: CMake 3.24+, Ninja, a C++20 compiler (MSVC 2022, Apple clang 15+, GCC 13+),
Qt 6.4+ with Qt Quick 3D and Qt Shader Tools (CI uses Qt 6.8), Open CASCADE 7.6+
(CI uses 8.0 from vcpkg).

The small libraries (nlohmann_json, pugixml, miniz, Catch2) are taken from the system or
vcpkg if present, otherwise CMake downloads them (FetchContent).

## Windows 11 x64

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
