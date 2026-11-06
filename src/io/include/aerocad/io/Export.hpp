// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/geometry/Shape.hpp"
#include "aerocad/io/Import.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace aerocad::io {

/// One body to export. Pointers are borrowed for the duration of the call.
struct ExportItem {
    std::string name;
    /// Mesh representation (required for STL and 3MF).
    const core::TriangleMesh* mesh = nullptr;
    /// Exact geometry (required for STEP).
    const geometry::Shape* shape = nullptr;
};

/// Thrown when a file cannot be written or the data does not fit the format.
class ExportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Formats exportFile() can write: STL, 3MF, STEP.
[[nodiscard]] bool canExport(FileFormat format);

/// File extensions (without dot, lower case) accepted by exportFile().
[[nodiscard]] std::vector<std::string> supportedExportExtensions();

/// Writes the items to a file. Units are millimetres in every format.
/// `format` Unknown means: detect from the file extension.
/// The file is written to a temporary name next to the target and renamed on success,
/// so a failed export never leaves a truncated file behind.
/// Throws ExportError on failure.
void exportFile(const std::filesystem::path& path, const std::vector<ExportItem>& items,
                FileFormat format = FileFormat::Unknown);

// Format-specific writers (also used by tests). Same contract as exportFile().

/// Binary STL. All items are merged into one solid; facet normals are computed from
/// the vertex winding.
void writeStl(const std::filesystem::path& path, const std::vector<ExportItem>& items);

/// 3MF core specification, unit="millimeter", one mesh object and build item per item.
/// Vertices at identical positions are merged so tessellated B-rep parts stay manifold.
void write3mf(const std::filesystem::path& path, const std::vector<ExportItem>& items);

/// STEP AP214, millimetres. Several items are written as one compound.
void writeStep(const std::filesystem::path& path, const std::vector<ExportItem>& items);

} // namespace aerocad::io
