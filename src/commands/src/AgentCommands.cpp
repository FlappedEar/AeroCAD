// SPDX-License-Identifier: GPL-3.0-or-later
// Commands that give an AI agent (MCP client) the context it needs before it changes the
// document: a project summary with units and revision, and measurements that report the
// method used and data-quality warnings.
#include "aerocad/commands/Session.hpp"

#include "aerocad/core/Measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

Json vec(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

core::Vec3 toVec(const Json& j)
{
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

Json bounds(const core::BoundingBox& box)
{
    if (box.isEmpty())
        return nullptr;
    return Json{{"min", vec(box.min)}, {"max", vec(box.max)}, {"size", vec(box.size())}};
}

Json pointSchema(const char* description)
{
    return Json{{"type", "array"},
                {"items", {{"type", "number"}}},
                {"minItems", 3},
                {"maxItems", 3},
                {"description", description}};
}

const model::Part& partById(const model::Document& doc, const Json& params, const char* key)
{
    if (!params.contains(key))
        throw CommandError(std::string("Parameter '") + key + "' is required for this kind of measurement");
    const auto id = params.at(key).get<model::PartId>();
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    if (!p->mesh || p->mesh->isEmpty())
        throw CommandError("Part " + std::to_string(id) + " has no geometry");
    return *p;
}

void addTessellationWarning(const model::Part& p, Json& warnings)
{
    if (p.shape)
        warnings.push_back("Part " + std::to_string(p.id) +
                           " has exact (B-rep) geometry; the value was measured on its tessellated mesh and can "
                           "deviate by up to the tessellation tolerance (about 0.1 % of the part size).");
}

Json measureDistance(const Json& params)
{
    if (!params.contains("points") || params["points"].size() != 2)
        throw CommandError("kind 'distance' needs 'points': two [x, y, z] points in mm");
    const core::Vec3 a = toVec(params["points"][0]);
    const core::Vec3 b = toVec(params["points"][1]);
    const core::Vec3 d = b - a;
    return Json{{"kind", "distance"},
                {"distance", d.length()},
                {"delta", vec(d)},
                {"unit", "mm"},
                {"method", "Euclidean distance between the two given points."},
                {"warnings", Json::array()}};
}

Json measurePart(const model::Document& doc, const Json& params)
{
    const model::Part& p = partById(doc, params, "id");
    const core::TriangleMesh& mesh = *p.mesh;
    Json warnings = Json::array();

    double volume = 0.0;
    std::string volumeMethod;
    // Closedness of exact geometry comes from its topology (solids), not from the mesh, so
    // the mesh edge scan (slow on a big display mesh) is left out for solids.
    const bool hasSolid = p.shape && p.shape->solidCount() > 0;
    const std::size_t openEdges = hasSolid ? 0 : core::openEdgeCount(mesh);
    const bool closed = hasSolid || openEdges == 0;
    if (hasSolid) {
        volume = p.shape->volume();
        volumeMethod = "Exact volume of the B-rep solid(s); closedness from the B-rep topology.";
    } else {
        volume = std::abs(core::enclosedVolume(mesh));
        volumeMethod = "Volume enclosed by the triangle mesh (divergence theorem).";
        if (openEdges > 0)
            warnings.push_back("The mesh is not closed (" + std::to_string(openEdges) +
                               " open or non-manifold edges): the volume is not reliable.");
    }

    // Centre of mass (uniform density): exact for solids, from the enclosed mesh otherwise
    // (only when the mesh is closed, else it means nothing).
    std::optional<core::Vec3> centre;
    if (hasSolid)
        centre = p.shape->centerOfMass();
    else if (closed)
        centre = core::enclosedCentroid(mesh);
    const bool exactArea = p.shape && !p.shape->isNull();
    Json out{{"kind", "part"},
             {"id", p.id},
             {"name", p.name},
             {"bounds", bounds(mesh.bounds())},
             {"surfaceArea", exactArea ? p.shape->surfaceArea() : mesh.surfaceArea()},
             {"volume", volume},
             {"closed", closed},
             {"openEdges", openEdges},
             {"triangles", mesh.triangleCount()},
             {"units", {{"length", "mm"}, {"area", "mm^2"}, {"volume", "mm^3"}}},
             {"method", std::string(exactArea ? "Bounds from the display mesh, surface area exact from the B-rep faces. "
                                              : "Bounds and surface area from the display mesh. ")
                            + volumeMethod + " Centre of mass for uniform density."},
             {"warnings", warnings}};
    if (centre)
        out["centerOfMass"] = vec(*centre);
    return out;
}

Json measurePointToPart(const model::Document& doc, const Json& params)
{
    const model::Part& p = partById(doc, params, "id");
    if (!params.contains("point"))
        throw CommandError("kind 'point_to_part' needs 'point': [x, y, z] in mm");
    const auto hit = core::closestPointOnMesh(*p.mesh, toVec(params["point"]));
    Json warnings = Json::array();
    addTessellationWarning(p, warnings);
    return Json{{"kind", "point_to_part"},
                {"id", p.id},
                {"distance", hit->distance},
                {"closestPoint", vec(hit->point)},
                {"normal", vec(p.mesh->faceNormal(hit->triangle))},
                {"unit", "mm"},
                {"method", "Exact point-to-triangle distance to the closest triangle of the part mesh."},
                {"warnings", warnings}};
}

/// Minimum distance between two parts. Exact for the vertex-to-surface pairs it checks;
/// edge-edge closest pairs are not considered, so the result is an upper bound that
/// converges with mesh density.
Json measureClearance(const model::Document& doc, const Json& params)
{
    const model::Part& a = partById(doc, params, "id");
    const model::Part& b = partById(doc, params, "other_id");
    if (a.id == b.id)
        throw CommandError("'id' and 'other_id' must be different parts");

    Json warnings = Json::array();
    addTessellationWarning(a, warnings);
    addTessellationWarning(b, warnings);

    // Bound the brute-force work (vertex count x triangle count) so the call stays interactive.
    constexpr double budget = 4.0e7;
    struct Best {
        double distance = std::numeric_limits<double>::max();
        core::Vec3 from, to;
    } best;
    bool sampled = false;
    auto scan = [&](const core::TriangleMesh& from, const core::TriangleMesh& to, bool swap) {
        const double work = static_cast<double>(from.vertexCount()) * static_cast<double>(to.triangleCount());
        const std::size_t stride = work > budget ? static_cast<std::size_t>(work / budget) + 1 : 1;
        sampled = sampled || stride > 1;
        for (std::size_t i = 0; i < from.vertexCount(); i += stride) {
            const core::Vec3& v = from.vertices()[i];
            const auto hit = core::closestPointOnMesh(to, v);
            if (hit && hit->distance < best.distance) {
                best.distance = hit->distance;
                best.from = swap ? hit->point : v;
                best.to = swap ? v : hit->point;
            }
        }
    };
    scan(*a.mesh, *b.mesh, false);
    scan(*b.mesh, *a.mesh, true);
    if (sampled)
        warnings.push_back("The meshes are large; only a subset of vertices was checked, so the true clearance "
                           "can be smaller.");
    warnings.push_back("Does not detect intersecting parts: overlapping parts can report a positive clearance.");

    return Json{{"kind", "clearance"},
                {"id", a.id},
                {"other_id", b.id},
                {"distance", best.distance},
                {"pointOnPart", vec(best.from)},
                {"pointOnOther", vec(best.to)},
                {"unit", "mm"},
                {"method", "Minimum vertex-to-surface distance in both directions between the two part meshes "
                           "(an upper bound of the true minimum distance)."},
                {"warnings", warnings}};
}

void registerProjectContext(CommandRegistry& r)
{
    r.add({"project.get_context", "Project context",
           "Summary of the open project for an AI agent: units and axes, the document revision, all parts with "
           "their ids, names, sizes and bounds, and the undo state. Call this first, and again after changes. "
           "All lengths are in millimetres, Z is up.",
           Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}},
          [](CommandContext& ctx, const Json&) {
              Json parts = Json::array();
              for (const auto& p : ctx.document.parts()) {
                  const core::BoundingBox box = p.mesh ? p.mesh->bounds() : core::BoundingBox{};
                  parts.push_back({{"id", p.id},
                                   {"name", p.name},
                                   {"source", p.source},
                                   {"visible", p.visible},
                                   {"hasBrep", p.shape.has_value()},
                                   {"triangles", p.mesh ? p.mesh->triangleCount() : 0},
                                   {"revision", p.revision},
                                   {"bounds", bounds(box)}});
              }
              return Json{{"units", {{"length", "mm"}, {"angle", "deg"}}},
                          {"axes", "Right-handed, Z up"},
                          {"revision", ctx.document.revision()},
                          {"parts", parts},
                          {"bounds", bounds(ctx.document.bounds())},
                          {"history",
                           {{"canUndo", ctx.undoStack.canUndo()},
                            {"canRedo", ctx.undoStack.canRedo()},
                            {"undoLabel", ctx.undoStack.undoLabel()},
                            {"redoLabel", ctx.undoStack.redoLabel()}}}};
          });
}

void registerMeasure(CommandRegistry& r)
{
    r.add({"geometry.measure", "Measure",
           "Measures geometry. All lengths in millimetres. kind='distance': distance between two points "
           "(needs 'points'). kind='part': bounds, surface area, volume, closedness and centre of mass "
           "(uniform density) of a part (needs 'id'). "
           "kind='point_to_part': distance from a point to the surface of a part (needs 'id', 'point'). "
           "kind='clearance': minimum distance between two parts (needs 'id', 'other_id'). The result names the "
           "method used and lists data-quality warnings.",
           Json{{"type", "object"},
                {"properties",
                 {{"kind",
                   {{"type", "string"},
                    {"enum", {"distance", "part", "point_to_part", "clearance"}},
                    {"description", "What to measure."}}},
                  {"points",
                   {{"type", "array"},
                    {"items", pointSchema("[x, y, z] in mm")},
                    {"minItems", 2},
                    {"maxItems", 2},
                    {"description", "kind='distance': two [x, y, z] points in mm."}}},
                  {"point", pointSchema("kind='point_to_part': [x, y, z] in mm.")},
                  {"id", {{"type", "integer"}, {"minimum", 1}, {"description", "Part id (see project.get_context)."}}},
                  {"other_id",
                   {{"type", "integer"}, {"minimum", 1}, {"description", "kind='clearance': the second part id."}}}}},
                {"required", {"kind"}},
                {"additionalProperties", false}}},
          [](CommandContext& ctx, const Json& params) {
              const std::string kind = params.at("kind").get<std::string>();
              if (kind == "distance")
                  return measureDistance(params);
              if (kind == "part")
                  return measurePart(ctx.document, params);
              if (kind == "point_to_part")
                  return measurePointToPart(ctx.document, params);
              return measureClearance(ctx.document, params);
          });
}

} // namespace

void registerAgentCommands(CommandRegistry& registry)
{
    registerProjectContext(registry);
    registerMeasure(registry);
}

} // namespace aerocad::commands
