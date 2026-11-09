// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/model/Part.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::app {

/// Viewport eligibility shared by exact surface, near-surface and sketch-line picks.
/// A feature dialog can show its consumed sketch while its preview body hides it in
/// the document. Explicitly hidden parts still stay hidden.
struct PickFilter {
    std::string kind;
    std::optional<std::uint64_t> shownSketch;
    std::optional<std::uint64_t> unpickablePlane;
    std::vector<model::PartId> hiddenPartIds;

    [[nodiscard]] bool accepts(const model::Part& part) const
    {
        if (!part.visible && !(part.kind == model::PartKind::Sketch && shownSketch == part.featureId))
            return false;
        if (std::find(hiddenPartIds.begin(), hiddenPartIds.end(), part.id) != hiddenPartIds.end())
            return false;
        if (part.kind == model::PartKind::Plane && unpickablePlane == part.featureId)
            return false;
        if (kind.empty())
            return true;
        if (kind == "solid")
            return part.kind == model::PartKind::Body || (part.kind == model::PartKind::Imported && part.shape);
        return (kind == "body" && part.kind == model::PartKind::Body)
               || (kind == "sketch" && part.kind == model::PartKind::Sketch)
               || (kind == "plane" && part.kind == model::PartKind::Plane)
               || (kind == "imported" && part.kind == model::PartKind::Imported);
    }
};

} // namespace aerocad::app
