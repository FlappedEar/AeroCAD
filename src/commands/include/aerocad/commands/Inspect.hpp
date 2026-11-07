// SPDX-License-Identifier: GPL-3.0-or-later
// State of the Inspect analyses (section, heat maps). Analyses are view state: they do not
// change the document and are not recorded in the undo history, but they are set through
// commands (inspect.*) so that an AI agent can drive them like the user.
#pragma once

#include "aerocad/core/MeshQuery.hpp"
#include "aerocad/core/MeshSection.hpp"
#include "aerocad/model/Part.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aerocad::commands {

struct SectionAnalysis {
    bool enabled = false;
    core::CutPlane plane;
    /// Parts that are cut; empty = all parts.
    std::vector<model::PartId> parts;
    /// Fill the cut with a cap (closed section loops only).
    bool cap = true;

    [[nodiscard]] bool cuts(model::PartId id) const;
};

/// Per-vertex values shown as colours on one part (deviation or wall thickness).
struct HeatMap {
    enum class Kind { Deviation, Thickness };
    bool enabled = false;
    Kind kind = Kind::Deviation;
    model::PartId part = 0;
    model::PartId reference = 0; ///< Deviation only.
    /// The mesh the values belong to (values[i] is vertex i); a part whose mesh is no longer
    /// this one shows no heat map until the analysis is run again.
    std::shared_ptr<const core::TriangleMesh> mesh;
    /// Deviation: the reference mesh the values were computed against.
    std::shared_ptr<const core::TriangleMesh> referenceMesh;
    std::shared_ptr<const std::vector<double>> values; ///< NaN = no value.
    /// Colour scale: rangeMin is blue, rangeMax red (deviation: green at 0).
    double rangeMin = 0.0;
    double rangeMax = 0.0;
    core::ValueStats stats;
    /// Parameters of the command that produced it (to run it again after edits).
    nlohmann::json params;
};

class InspectState {
public:
    using Listener = std::function<void()>;

    [[nodiscard]] const SectionAnalysis& section() const { return m_section; }
    [[nodiscard]] const HeatMap& heatMap() const { return m_heatMap; }

    void setSection(SectionAnalysis section);
    void setHeatMap(HeatMap map);
    void clear();

    /// Called after every change.
    void setListener(Listener listener) { m_listener = std::move(listener); }
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

    /// Spatial index of a mesh, cached for the last few meshes (index building is the
    /// expensive part of repeated measurements on large scans).
    [[nodiscard]] std::shared_ptr<const core::MeshBvh> bvh(const std::shared_ptr<const core::TriangleMesh>& mesh);

private:
    void changed();

    SectionAnalysis m_section;
    HeatMap m_heatMap;
    std::uint64_t m_revision = 0;
    Listener m_listener;
    std::vector<std::shared_ptr<const core::MeshBvh>> m_bvhCache;
};

} // namespace aerocad::commands
