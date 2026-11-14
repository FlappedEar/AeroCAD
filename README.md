# AeroCAD

A native CAD application (macOS Apple Silicon; Linux for development; Windows is postponed) for
designing parts on top of an arbitrary 3D model, such as a scan of a bumper or a whole car:

1. Load any 3D file (STL, OBJ, PLY, 3MF, STEP, IGES, BREP), including multi-million
   triangle 3D scans.
2. Inspect the model: orbit, pan, zoom.
3. Select a region on the model; the application builds a base surface on it.
4. Design on that surface with CAD tools (sketch, extrude, shell, fillet, boolean, ...).
5. Export the result to STEP, STL or 3MF.
6. Run a CFD simulation separately and get charts.
7. An AI agent (over MCP) can run the same operations as the user with explicit access to the current project/session; New, Open and recovery revoke it. See [docs/mcp.md](docs/mcp.md).

Status (2026-11-14): all seven steps work end to end and are developed in the open. Missing:
the aero shaping step (ridge, lip, fade-out on a part made on a scan, CAD-24/CAD-25) and the
unaided 15-minute usability check (CAD-37). The path that must always work is the
[golden journey](docs/golden-journey.md); `scripts/golden-journey.sh` runs it against a build.
Requirements, decisions and the plan are in the Confluence space AE; issues are in Jira project CAD.

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

Details and troubleshooting: [docs/macos.md](docs/macos.md).

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
