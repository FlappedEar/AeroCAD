// SPDX-License-Identifier: GPL-3.0-or-later
// The AeroCAD project file (.aerocad): a zip container with
//   manifest.json      versioned manifest (format, version, parts, design timeline,
//                      parameters, reference frames, variants, view)
//   meshes/<id>.amesh  imported meshes, lossless (double precision, see MeshCodec)
//   shapes/<id>.brep   exact geometry of imported parts (Open CASCADE BREP)
//   selections/<id>.u32  region selection of a part (triangle indices)
//   results/<feature>-<k>.brep  bodies of timeline steps built on a scan (part / fin on
//                      surface), so opening does not refit them
// Bodies and sketches are otherwise not stored: they are regenerated from the timeline on load.
// Units are millimetres, Z up. See docs/project-format.md.
#pragma once

#include "aerocad/cad/Design.hpp"
#include "aerocad/cad/Variants.hpp"
#include "aerocad/model/Part.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::project {

using Json = nlohmann::json;

/// Manifest "format" value.
inline constexpr std::string_view kFormatName = "aerocad.project";
/// Current manifest version. Readers accept every version up to this one.
inline constexpr int kFormatVersion = 1;
/// File name extension, including the dot.
inline constexpr std::string_view kFileExtension = ".aerocad";

class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Saved settings of a document part produced by the timeline (body or sketch).
struct TimelinePart {
    cad::FeatureId feature = 0;
    /// Unknown for features without a part right now (suppressed, rolled back, other variant).
    std::optional<model::PartKind> kind;
    model::PartId id = 0;
    std::string name;
    bool visible = true;
    /// Region selection on the regenerated part; restored only if the indices still fit.
    std::shared_ptr<const core::TriangleSet> selection;
    /// Appearance colour ("#rrggbb"), empty = default.
    std::string color;
};

/// Everything a project file holds.
struct ProjectData {
    /// Imported parts (kind Imported) with mesh and / or exact shape.
    std::vector<model::Part> parts;
    std::vector<TimelinePart> timelineParts;
    /// Results of timeline steps built on a scan (cad::FeatureEvaluator), reused on load
    /// while their inputs match.
    std::vector<cad::Design::StoredResult> timelineResults;
    /// Design state of the active variant.
    cad::Design::State design;
    /// All variants; the stored state of the active one is ignored (`design` is used).
    cad::VariantSet variants;
    /// UI view state (camera), opaque to the file format.
    Json view = Json::object();
    /// Application name and version that wrote the file.
    std::string generator;
    /// For copies (autosave): path of the project the copy was made from, UTF-8.
    std::string originalPath;
    /// Version of the file that was read (load only).
    int formatVersion = kFormatVersion;
};

/// Serialises a project to the bytes of a .aerocad file. Throws ProjectError.
[[nodiscard]] std::string encodeProject(const ProjectData& data);
/// Parses the bytes of a .aerocad file. Throws ProjectError for invalid files and for files
/// written by a newer format version.
[[nodiscard]] ProjectData decodeProject(std::string_view bytes);

/// Writes the project atomically (see writeFileAtomically). Throws ProjectError.
void saveProject(const std::filesystem::path& path, const ProjectData& data);
/// Reads a project file. Throws ProjectError.
[[nodiscard]] ProjectData loadProject(const std::filesystem::path& path);

/// Replaces `path` with `data` so that readers see either the old or the new file, never
/// a partial one: writes a temporary file next to it, flushes it to disk and renames it
/// over the target. `beforeRename` (tests) runs after the temporary file is written; if it
/// throws, the temporary file is removed and the target is left untouched.
/// Throws ProjectError.
void writeFileAtomically(const std::filesystem::path& path, std::string_view data,
                         const std::function<void()>& beforeRename = {});

/// Reads a whole file. Throws ProjectError.
[[nodiscard]] std::string readFile(const std::filesystem::path& path);

/// Lossless binary mesh encoding used for meshes/<id>.amesh: magic "AEROMESH", uint32
/// version (1), uint64 vertex count, uint64 triangle count, vertices as 3 x float64,
/// triangles as 3 x uint32, all little-endian.
[[nodiscard]] std::string encodeMesh(const core::TriangleMesh& mesh);
/// Throws ProjectError for malformed data.
[[nodiscard]] core::TriangleMesh decodeMesh(std::string_view data);

/// Region selections (selections/<id>.u32): triangle indices as little-endian uint32.
[[nodiscard]] std::string encodeIndices(const std::vector<std::uint32_t>& indices);
[[nodiscard]] std::vector<std::uint32_t> decodeIndices(std::string_view data);

} // namespace aerocad::project
