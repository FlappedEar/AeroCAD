// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the built-in command implementations (private to the module).
#pragma once

#include "aerocad/cad/Design.hpp"
#include "aerocad/commands/CommandRegistry.hpp"

#include <string>
#include <utility>

namespace aerocad::commands {

class FitStage;

inline Json vecToJson(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

inline Json boundsToJson(const core::BoundingBox& box)
{
    if (box.isEmpty())
        return nullptr;
    return Json{{"min", vecToJson(box.min)}, {"max", vecToJson(box.max)}, {"size", vecToJson(box.size())}};
}

inline Json partToJson(const model::Part& p)
{
    return Json{
        {"id", p.id},
        {"name", p.name},
        {"source", p.source},
        {"visible", p.visible},
        {"color", p.color},
        {"vertices", p.mesh ? p.mesh->vertexCount() : 0},
        {"triangles", p.mesh ? p.mesh->triangleCount() : 0},
        {"hasBrep", p.shape.has_value()},
        {"kind", p.kind == model::PartKind::Body     ? "body"
                 : p.kind == model::PartKind::Sketch ? "sketch"
                 : p.kind == model::PartKind::Plane  ? "plane"
                                                     : "imported"},
        {"featureId", p.featureId},
        {"frozen", model::frozenSurfaceResult(p)},
        {"selectedTriangles", p.selection ? p.selection->size() : 0},
        {"bounds", boundsToJson(p.mesh ? p.mesh->bounds() : core::BoundingBox{})},
    };
}

inline const model::Part& requirePart(const model::Document& doc, const Json& params)
{
    const auto id = params.at("id").get<model::PartId>();
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    return *p;
}

/// Bodies and sketches are produced by the timeline; geometry edits must go through it.
inline void requireImported(const model::Part& p, const char* what)
{
    if (p.kind != model::PartKind::Imported)
        throw CommandError(std::string("Cannot ") + what + " '" + p.name +
                           "': it is produced by the design timeline (feature " + std::to_string(p.featureId) +
                           "); use feature.delete / feature.edit instead");
}

inline Json idParam()
{
    return Json{{"type", "integer"}, {"minimum", 1}, {"description", "Part id."}};
}

inline Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

/// Records a part-level change in the undo stack: swaps the part state.
inline void pushPartStateUndo(CommandContext& ctx, std::string label, model::PartId id, model::Part before,
                              model::Part after)
{
    model::Document& doc = ctx.document;
    ctx.undoStack.push({std::move(label),
                        [&doc, id, before]() { doc.modifyPart(id, [&](model::Part& p) { p = before; }); },
                        [&doc, id, after]() { doc.modifyPart(id, [&](model::Part& p) { p = after; }); }});
}

/// Registers selection.* (SelectionCommands.cpp).
void registerSelectionCommands(CommandRegistry& registry);

/// Registers surface.* (SurfaceCommands.cpp).
void registerSurfaceCommands(CommandRegistry& registry);

/// Lets the design timeline build part_on_surface and fin_on_surface features from the
/// document's scans and surfaces (SurfaceCommands.cpp).
/// Fit steps take results staged in `fitStage` (may be null).
void registerSurfaceFeatures(cad::Design& design, const model::Document& document, FitStage* fitStage = nullptr);

} // namespace aerocad::commands
