// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::io {

enum class FileFormat { Unknown, Stl, Obj, Ply, ThreeMf, Step, Iges, Brep };

[[nodiscard]] std::string_view formatName(FileFormat format);

/// Detects the format from the file extension (case-insensitive).
[[nodiscard]] FileFormat formatFromExtension(const std::filesystem::path& path);

/// File extensions (without dot, lower case) accepted by importFile().
[[nodiscard]] std::vector<std::string> supportedImportExtensions();

/// One body of a file that holds several (STEP products, CAD-227), in millimetres.
struct ImportBody {
    /// Name from the file; empty when the file has none (the caller names it).
    std::string name;
    /// Colour from the file, "#rrggbb", or empty.
    std::string color;
    core::TriangleMesh mesh;
    std::optional<geometry::Shape> shape;
};

struct ImportResult {
    FileFormat format = FileFormat::Unknown;
    /// Display / mesh representation. Always filled on success. STEP, IGES and 3MF
    /// coordinates are normalized to millimetres; unitless formats retain file coordinates.
    core::TriangleMesh mesh;
    /// Exact B-rep geometry, only for CAD formats (STEP, IGES, BREP), in the same units as mesh.
    std::optional<geometry::Shape> shape;
    /// Colour from the file for a single-body result, "#rrggbb", or empty.
    std::string color;
    /// Two or more bodies when the file names them separately: one part each. mesh and
    /// shape still hold all of them together.
    std::vector<ImportBody> bodies;
    /// Non-fatal problems (skipped elements, unsupported extensions, ...).
    std::vector<std::string> warnings;
};

/// Thrown when a file cannot be read or parsed.
class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when the progress callback asked to stop the import.
class ImportCancelled : public ImportError {
public:
    ImportCancelled() : ImportError("Import cancelled") {}
};

/// Progress callback: receives the completed fraction (0.0 .. 1.0, non-decreasing).
/// It is called with 0.0 at the start, about every 1% of the work and with 1.0 at the
/// end, on the importing thread. Return false to cancel (the import throws ImportCancelled).
using ImportProgress = std::function<bool(double fraction)>; // return false to cancel

struct ImportOptions {
    ImportProgress progress;
};

/// Imports a 3D file. STEP, IGES and 3MF use file units and return millimetres.
/// STL, OBJ, PLY and BREP preserve their unitless coordinates; the caller chooses their unit.
/// Throws ImportError on failure.
[[nodiscard]] ImportResult importFile(const std::filesystem::path& path);
[[nodiscard]] ImportResult importFile(const std::filesystem::path& path, const ImportOptions& options);

// Format-specific readers (also used by tests). The mesh readers report progress;
// the others ignore the options (importFile() reports 0 and 1 for them).
[[nodiscard]] ImportResult readStl(const std::filesystem::path& path, const ImportOptions& options = {});
[[nodiscard]] ImportResult readObj(const std::filesystem::path& path, const ImportOptions& options = {});
[[nodiscard]] ImportResult readPly(const std::filesystem::path& path, const ImportOptions& options = {});
[[nodiscard]] ImportResult read3mf(const std::filesystem::path& path);
[[nodiscard]] ImportResult readStep(const std::filesystem::path& path);
[[nodiscard]] ImportResult readIges(const std::filesystem::path& path);
[[nodiscard]] ImportResult readBrep(const std::filesystem::path& path);

} // namespace aerocad::io
