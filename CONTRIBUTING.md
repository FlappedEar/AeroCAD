# Contributing to AeroCAD

## Ground rules

- Code, comments, commit messages, issues and documentation are written in English.
- Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`
  (or `#` for CMake/YAML). By contributing you agree to license your work under
  GPL-3.0-or-later.
- Work happens on branches and lands through pull requests. CI (`CI` workflow: Linux and
  macOS) must be green before merging. The `Release build` workflow (packaged macOS app)
  runs on `main` only and does not block pull requests. Reference the Jira issue key (e.g. `CAD-7`) in the
  branch name and PR title.

## Architecture rules

- Every user-visible operation is a **command** (`src/commands`): a name, a JSON Schema
  for its parameters and a JSON result. The UI calls commands; the MCP server will expose
  the same commands to an AI agent. Do not put document logic in QML or in Qt classes.
- Modules below `commands` (`core`, `geometry`, `io`, `model`) do not depend on Qt.
- Open CASCADE types stay inside `geometry` and `io` (`AeroCAD::geometry_occt`); public
  headers of other modules must not include OCCT headers.
- Model units are millimetres, Z up.

## Style

- C++20, formatted with the repository `.clang-format` (4 spaces, 120 columns).
- Code must build warning-free with `-DAEROCAD_WARNINGS_AS_ERRORS=ON` on MSVC, Apple clang
  and GCC.
- New behaviour needs unit tests (Catch2, `tests/`). Tests must not depend on files
  outside the repository; generate fixtures in a temporary directory where possible.

See [docs/building.md](docs/building.md) for build instructions.
