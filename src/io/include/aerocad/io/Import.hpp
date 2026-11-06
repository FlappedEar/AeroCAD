// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::io {

enum class FileFormat { Unknown, Stl, Obj, ThreeMf, Step, Iges, Brep };

[[nodiscard]] std::string_view formatName(FileFormat format);

/// Detects the format from the file extension (case-insensitive).
[[nodiscard]] FileFormat formatFromExtension(const std::filesystem::path& path);

/// File extensions (without dot, lower case) accepted by importFile().
[[nodiscard]] std::vector<std::string> supportedImportExtensions();

struct ImportResult {
    FileFormat format = FileFormat::Unknown;
    /// Display / mesh representation. Always filled on success.
    core::TriangleMesh mesh;
    /// Exact B-rep geometry, only for CAD formats (STEP, IGES, BREP).
    std::optional<geometry::Shape> shape;
    /// Non-fatal problems (skipped elements, unsupported extensions, ...).
    std::vector<std::string> warnings;
};

/// Thrown when a file cannot be read or parsed.
class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Imports a 3D file. Mesh coordinates are in millimetres.
/// Throws ImportError on failure.
[[nodiscard]] ImportResult importFile(const std::filesystem::path& path);

// Format-specific readers (also used by tests).
[[nodiscard]] ImportResult readStl(const std::filesystem::path& path);
[[nodiscard]] ImportResult readObj(const std::filesystem::path& path);
[[nodiscard]] ImportResult read3mf(const std::filesystem::path& path);
[[nodiscard]] ImportResult readStep(const std::filesystem::path& path);
[[nodiscard]] ImportResult readIges(const std::filesystem::path& path);
[[nodiscard]] ImportResult readBrep(const std::filesystem::path& path);

} // namespace aerocad::io
