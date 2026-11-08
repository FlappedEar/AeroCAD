// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::cfd {

using Json = nlohmann::json;

/// A whitespace separated OpenFOAM function object table (`# Time  col1  col2 ...`):
/// one row per iteration. Columns are looked up by the names in the last header line.
struct Table {
    std::vector<std::string> columns; ///< Without the leading "Time".
    std::vector<double> time;
    std::vector<std::vector<double>> values; ///< values[column][row]
    bool incomplete = false; ///< A truncated row or uncommitted tail remains; valid rows remain for live charts.
    bool invalidTime = false; ///< A data row had a non-numeric or non-finite time.

    [[nodiscard]] const std::vector<double>* column(std::string_view name) const;
    [[nodiscard]] bool empty() const { return time.empty(); }
};

/// Reads a table; missing file = empty table. Non-numeric cells (solver names, true/false)
/// become NaN. Restarted runs append rows with repeated times; the later committed row wins.
/// An unterminated last line is incomplete and cannot replace the committed history.
[[nodiscard]] Table readTable(const std::filesystem::path& path);

/// Legacy ASCII VTK polydata as written by OpenFOAM's surface and set writers.
struct PolyData {
    std::vector<core::Vec3> points;
    std::vector<std::vector<std::uint32_t>> polygons;
    std::vector<std::vector<std::uint32_t>> lines;
    std::map<std::string, std::vector<double>, std::less<>> scalars;     ///< Per point.
    std::map<std::string, std::vector<core::Vec3>, std::less<>> vectors; ///< Per point.

    [[nodiscard]] bool empty() const { return points.empty(); }
};

/// Throws std::runtime_error on a malformed file.
[[nodiscard]] PolyData readVtk(const std::filesystem::path& path);

/// The polygons of `data` whose centre lies inside `box`, with only the points they use
/// (scalars and vectors follow). Lines are dropped. Used to keep a cutting plane through
/// the whole wind tunnel to the region around the part.
[[nodiscard]] PolyData cropPolygons(const PolyData& data, const core::BoundingBox& box);

/// The parts of the lines of `data` inside `box` (a line that leaves the box and comes
/// back becomes two lines), with only the points they use. Polygons are dropped.
[[nodiscard]] PolyData cropLines(const PolyData& data, const core::BoundingBox& box);

/// Appends `other` to `data` (points, polygons, lines; per-point fields present in both).
void appendPolyData(PolyData& data, const PolyData& other);

/// A/B difference on a surface (CAD-141): `a` with, for each of its points, the scalars of
/// the nearest point of `b` minus its own, as "d<name>" (dCp, dpressure, ...). Points with
/// no point of `b` within `maxDistance` (the variant's shape differs there) get NaN.
[[nodiscard]] PolyData surfaceDifference(const PolyData& a, const PolyData& b, double maxDistance);

/// Directories that OpenFOAM writes a function object's output into, sorted by time.
[[nodiscard]] std::vector<std::filesystem::path> timeDirectories(const std::filesystem::path& functionObjectDir);

/// Quality checks of a run (CAD-53).
struct QualityReport {
    std::optional<bool> meshOk;          ///< checkMesh verdict, empty when not run yet.
    std::optional<std::int64_t> cells;   ///< Volume mesh cells.
    int failedMeshChecks = 0;
    bool solverConverged = false;        ///< Residual targets reached before the iteration limit.
    std::optional<double> continuityError; ///< Last "sum local" time step continuity error.
    std::map<std::string, double> finalResiduals; ///< Initial residual of the last iteration.
    /// Relative band (max - min) / |mean| of Cd over the last 20 % of the iterations.
    std::optional<double> cdFluctuation;
    std::vector<std::string> warnings;
    /// Required evidence: "passed", "failed" or "missing" for each check.
    std::map<std::string, std::string> checks;
    /// "ok", "warning" or "unknown" (not enough data).
    std::string status = "unknown";

    [[nodiscard]] Json toJson() const;
};

/// Reads the logs and function object output of a case and assesses the run.
[[nodiscard]] QualityReport assessQuality(const std::filesystem::path& caseDir);

/// Iteration the solver last reported in log.simpleFoam (0 when not started).
[[nodiscard]] int lastSolverIteration(const std::filesystem::path& caseDir);

} // namespace aerocad::cfd
