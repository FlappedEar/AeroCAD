# AeroCAD

A native CAD application (Windows 11 x64, macOS Apple Silicon) for designing parts on
top of an arbitrary 3D model:

1. Load any 3D file (STL, OBJ, 3MF, STEP, IGES, BREP).
2. Inspect the model: orbit, pan, zoom.
3. Select a region on the model; the application builds a base surface on it.
4. Design on that surface with CAD tools (sketch, extrude, shell, fillet, boolean, ...).
5. Export the result to STEP, STL or 3MF.
6. Run a CFD simulation separately and get charts.
7. An AI agent (over MCP) can run the same operations as the user.

Status: early skeleton (milestone 0.1, epic CAD-1 "Foundation").

## Stack

C++20, Qt 6 (Qt Quick, Qt Quick 3D), Open CASCADE Technology, CMake.
See [docs/architecture.md](docs/architecture.md) and [docs/building.md](docs/building.md).

## Quick start on a Mac

```sh
brew install cmake ninja qt opencascade nlohmann-json pugixml catch2
git clone https://github.com/FlappedEar/AeroCAD.git && cd AeroCAD
cmake --preset release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset release
open build/release/bin/AeroCAD.app
```

Details, the prebuilt download and troubleshooting: [docs/macos.md](docs/macos.md).

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
