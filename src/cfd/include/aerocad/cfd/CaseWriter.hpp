// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/cfd/Settings.hpp"
#include "aerocad/core/Mesh.hpp"

#include <filesystem>

namespace aerocad::cfd {

/// Mapping between model coordinates (millimetres, Z up, any flow direction) and the
/// coordinates of the generated OpenFOAM case (metres, air flowing along +X, Z up).
///
///   case = R * (model - origin) * 0.001
///
/// R is a rotation about Z by a multiple of 90 degrees.
struct CaseFrame {
    core::Vec3 origin; ///< Model point (mm) that becomes the case origin.
    int quarterTurns = 0; ///< Rotation about +Z applied to model vectors, in 90 degree steps.

    static CaseFrame forFlow(std::string_view flowDirection, const core::Vec3& origin);

    [[nodiscard]] core::Vec3 rotate(const core::Vec3& v) const;   ///< Model -> case direction.
    [[nodiscard]] core::Vec3 unrotate(const core::Vec3& v) const; ///< Case -> model direction.
    [[nodiscard]] core::Vec3 toCase(const core::Vec3& modelMm) const;
    [[nodiscard]] core::Vec3 toModel(const core::Vec3& caseM) const;

    [[nodiscard]] Json toJson() const;
    static CaseFrame fromJson(const Json& json);
};

/// Geometry to simulate, in model coordinates (mm).
struct CaseGeometry {
    /// The part forces are reported for.
    core::TriangleMesh part;
    /// Other geometry in the flow (for example the section of the body the part is
    /// mounted on). Walls, but not included in the forces. May be empty.
    core::TriangleMesh context;
};

/// What was generated, stored as case.json in the case directory.
struct CaseInfo {
    CaseFrame frame;
    Settings settings;
    double referenceArea = 0.0;   ///< m^2 (effective: given or computed frontal area).
    double referenceLength = 0.0; ///< m.
    double characteristicLength = 0.0; ///< Largest part dimension, m.
    core::BoundingBox partBounds;   ///< Case frame, m.
    core::BoundingBox domain;       ///< Case frame, m.
    double baseCellSize = 0.0;      ///< Background mesh cell size, m.
    std::uint64_t geometryHash = 0; ///< FNV-1a of the written surfaces (reproducibility).

    [[nodiscard]] Json toJson() const;
    static CaseInfo fromJson(const Json& json);
};

/// Area of the projection of a mesh onto the plane normal to +X (case frame), computed by
/// rasterising the projected triangles on a grid of `resolution` x `resolution` cells over
/// the projected bounding rectangle. Error is about one cell row along the outline.
[[nodiscard]] double frontalArea(const core::TriangleMesh& caseMesh, int resolution = 512);

/// Computes the case layout (frame, reference values, domain) without writing anything.
/// Throws std::runtime_error when the geometry is empty.
CaseInfo planCase(const CaseGeometry& geometry, const Settings& settings);

/// Writes a complete, self-contained OpenFOAM case (system/, constant/, 0.orig/, Allrun)
/// into `directory` (created if needed; existing case files are overwritten). Throws
/// std::runtime_error when the geometry is empty or the directory cannot be written.
///
/// Running `./Allrun` inside an OpenFOAM environment meshes and solves the case and
/// writes the progress files read by RunManager: `aerocad.stage` (current step) and
/// `aerocad.status` (`finished` or `failed <step>`).
CaseInfo writeCase(const std::filesystem::path& directory, const CaseGeometry& geometry, const Settings& settings);

/// The geometryHash writeCase would record, without writing anything (same part and
/// surroundings in the same case frame give the same hash).
[[nodiscard]] std::uint64_t caseGeometryHash(const CaseGeometry& geometry, const Settings& settings);

/// Hash of a mesh's vertex positions and triangles (model units). Identifies the geometry
/// a run was made from, e.g. to tell whether a part in another document is the same one.
[[nodiscard]] std::uint64_t meshFingerprint(const core::TriangleMesh& mesh);

} // namespace aerocad::cfd
