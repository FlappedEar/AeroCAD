// SPDX-License-Identifier: GPL-3.0-or-later
// Commands for working with scanned meshes: statistics and repair, unit conversion,
// scale calibration by a known dimension, alignment to the world axes.
#include "aerocad/commands/Session.hpp"

#include "aerocad/core/Align.hpp"
#include "aerocad/core/MeshRepair.hpp"
#include "aerocad/core/MeshSimplify.hpp"
#include "aerocad/core/Units.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aerocad::commands {

namespace {

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

Json idParam()
{
    return Json{{"type", "integer"}, {"minimum", 1}, {"description", "Part id (see document.parts)."}};
}

Json pointParam(const std::string& description)
{
    return Json{{"type", "array"},
                {"items", {{"type", "number"}}},
                {"minItems", 3},
                {"maxItems", 3},
                {"description", description}};
}

Json unitParam(const std::string& description)
{
    return Json{{"type", "string"}, {"enum", {"um", "mm", "cm", "m", "in", "ft"}}, {"description", description}};
}

core::Vec3 toVec(const Json& j)
{
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

Json vecToJson(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

Json affineToJson(const core::Affine& a)
{
    // Row-major 4x4, last row (0 0 0 1).
    const core::Mat3& m = a.linear;
    return Json::array({m(0, 0), m(0, 1), m(0, 2), a.translation.x, m(1, 0), m(1, 1), m(1, 2), a.translation.y, m(2, 0),
                        m(2, 1), m(2, 2), a.translation.z, 0.0, 0.0, 0.0, 1.0});
}

Json statsToJson(const core::MeshStats& s)
{
    return Json{{"vertices", s.vertices},
                {"triangles", s.triangles},
                {"unusedVertices", s.unusedVertices},
                {"degenerateTriangles", s.degenerateTriangles},
                {"duplicateTriangles", s.duplicateTriangles},
                {"boundaryEdges", s.boundaryEdges},
                {"holes", s.holes},
                {"nonManifoldEdges", s.nonManifoldEdges},
                {"inconsistentEdges", s.inconsistentEdges},
                {"components", s.components},
                {"closed", s.boundaryEdges == 0 && s.nonManifoldEdges == 0 && s.triangles > 0}};
}

Json boundsToJson(const core::BoundingBox& box)
{
    if (box.isEmpty())
        return nullptr;
    return Json{{"min", vecToJson(box.min)}, {"max", vecToJson(box.max)}, {"size", vecToJson(box.size())}};
}

/// The imported part a scan command edits. Timeline bodies and sketches are refused:
/// their geometry is owned by features.
const model::Part& requireEditablePart(const model::Document& doc, const Json& params, const char* what)
{
    const auto id = params.at("id").get<model::PartId>();
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    if (p->kind != model::PartKind::Imported)
        throw CommandError(std::string("Cannot ") + what + " '" + p->name +
                           "': it is produced by the design timeline; use feature.* commands instead");
    if (!p->mesh || p->mesh->isEmpty())
        throw CommandError("Part '" + p->name + "' has no mesh");
    return *p;
}

/// Mesh-only edits (repair, decimate) would make the mesh disagree with an exact B-rep.
void requireMeshOnly(const model::Part& p, const char* what)
{
    if (p.shape)
        throw CommandError(std::string("Cannot ") + what + " '" + p.name +
                           "': it has exact CAD geometry (STEP/IGES/BREP); mesh repair is for scans and mesh files");
}

/// Replaces the part's mesh (and optionally its shape) and records one undo step.
void replaceGeometry(CommandContext& ctx, const model::Part& before, std::string label,
                     std::shared_ptr<const core::TriangleMesh> mesh, std::optional<geometry::Shape> shape)
{
    const model::PartId id = before.id;
    model::Part saved = before;
    ctx.document.modifyPart(id, [&](model::Part& p) {
        p.mesh = std::move(mesh);
        p.shape = std::move(shape);
    });
    model::Part after = *ctx.document.part(id);
    model::Document& doc = ctx.document;
    ctx.undoStack.push({std::move(label),
                        [&doc, id, saved]() { doc.modifyPart(id, [&](model::Part& p) { p = saved; }); },
                        [&doc, id, after]() { doc.modifyPart(id, [&](model::Part& p) { p = after; }); }});
}

/// Applies a transform to mesh and exact shape. Uniform scale goes through
/// geometry::scaled, rigid motions through geometry::transformed.
void applyTransform(CommandContext& ctx, const model::Part& part, const std::string& label, const core::Affine& tf,
                    std::optional<std::pair<double, core::Vec3>> uniformScale = std::nullopt)
{
    auto mesh = std::make_shared<core::TriangleMesh>(*part.mesh);
    core::transformMesh(*mesh, tf);
    std::optional<geometry::Shape> shape = part.shape;
    if (shape) {
        if (uniformScale)
            shape = geometry::scaled(*shape, uniformScale->first, uniformScale->second);
        else
            shape = geometry::transformed(*shape, tf);
    }
    replaceGeometry(ctx, part, label, std::move(mesh), std::move(shape));
}

core::Affine scaleAbout(double factor, const core::Vec3& origin)
{
    core::Mat3 s = core::Mat3::fromRows({factor, 0, 0}, {0, factor, 0}, {0, 0, factor});
    return {s, origin - origin * factor};
}

Json partSummary(const model::Part& p)
{
    return Json{{"id", p.id},
                {"name", p.name},
                {"vertices", p.mesh ? p.mesh->vertexCount() : 0},
                {"triangles", p.mesh ? p.mesh->triangleCount() : 0},
                {"bounds", boundsToJson(p.mesh ? p.mesh->bounds() : core::BoundingBox{})}};
}

void registerMeshCommands(CommandRegistry& r)
{
    r.add({"mesh.stats", "Mesh statistics",
           "Topology report of a part's mesh: triangles, holes (boundary loops), duplicate and degenerate "
           "triangles, flipped normals (inconsistent edges), non-manifold edges, shells, surface area. Use it to "
           "decide whether mesh.repair is needed.",
           objectSchema({{"id", idParam()}}, {"id"})},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<model::PartId>();
              const model::Part* p = ctx.document.part(id);
              if (p == nullptr)
                  throw CommandError("No part with id " + std::to_string(id));
              if (!p->mesh)
                  throw CommandError("Part '" + p->name + "' has no mesh");
              Json out = statsToJson(core::meshStats(*p->mesh));
              out["id"] = p->id;
              out["area"] = p->mesh->surfaceArea();
              out["bounds"] = boundsToJson(p->mesh->bounds());
              return out;
          });

    r.add({"mesh.repair", "Repair mesh",
           "Repairs a scanned/imported mesh in place (undoable): merges coincident or close vertices, removes "
           "degenerate and duplicate triangles, makes normals consistent (outwards for closed shells), fills "
           "holes up to `maxHoleEdges` boundary edges and drops unused vertices. Returns what was changed and the "
           "statistics after the repair.",
           objectSchema(
               {{"id", idParam()},
                {"mergeDistance",
                 {{"type", "number"},
                  {"minimum", 0},
                  {"description", "Merge vertices closer than this (mm). Default 0: identical positions only."}}},
                {"removeDegenerate", {{"type", "boolean"}, {"description", "Default true."}}},
                {"removeDuplicates", {{"type", "boolean"}, {"description", "Default true."}}},
                {"fixOrientation", {{"type", "boolean"}, {"description", "Default true."}}},
                {"maxHoleEdges",
                 {{"type", "integer"},
                  {"minimum", 0},
                  {"description", "Fill holes with at most this many edges; 0 (default) disables filling."}}}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "repair");
              requireMeshOnly(part, "repair");
              core::RepairOptions opt;
              opt.mergeDistance = params.value("mergeDistance", 0.0);
              opt.removeDegenerate = params.value("removeDegenerate", true);
              opt.removeDuplicates = params.value("removeDuplicates", true);
              opt.fixOrientation = params.value("fixOrientation", true);
              opt.maxHoleEdges = params.value("maxHoleEdges", std::size_t{0});
              core::RepairReport rep;
              auto mesh = std::make_shared<const core::TriangleMesh>(core::repairMesh(*part.mesh, opt, &rep));
              if (mesh->isEmpty())
                  throw CommandError("Repair would remove every triangle of '" + part.name + "'");
              const core::MeshStats after = core::meshStats(*mesh);
              replaceGeometry(ctx, part, "Repair " + part.name, std::move(mesh), std::nullopt);
              return Json{{"id", part.id},
                          {"mergedVertices", rep.mergedVertices},
                          {"removedDegenerate", rep.removedDegenerate},
                          {"removedDuplicates", rep.removedDuplicates},
                          {"removedUnusedVertices", rep.removedUnusedVertices},
                          {"flippedTriangles", rep.flippedTriangles},
                          {"filledHoles", rep.filledHoles},
                          {"addedTriangles", rep.addedTriangles},
                          {"stats", statsToJson(after)}};
          });

    r.add({"mesh.flip_normals", "Flip normals", "Reverses the orientation of every triangle of a part's mesh.",
           objectSchema({{"id", idParam()}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "flip normals of");
              requireMeshOnly(part, "flip normals of");
              auto mesh = std::make_shared<core::TriangleMesh>(*part.mesh);
              core::flipNormals(*mesh);
              replaceGeometry(ctx, part, "Flip normals " + part.name, std::move(mesh), std::nullopt);
              return partSummary(*ctx.document.part(part.id));
          });

    r.add(
        {"mesh.decimate", "Decimate mesh",
         "Permanently reduces a mesh to at most `targetTriangles` (vertex clustering; undoable). Not needed "
         "for display: the viewport already shows big meshes at a reduced level of detail while fitting and "
         "modelling use the full mesh.",
         objectSchema({{"id", idParam()},
                       {"targetTriangles",
                        {{"type", "integer"}, {"minimum", 4}, {"description", "Upper bound for the triangle count."}}}},
                      {"id", "targetTriangles"}),
         true},
        [](CommandContext& ctx, const Json& params) {
            const model::Part& part = requireEditablePart(ctx.document, params, "decimate");
            requireMeshOnly(part, "decimate");
            const auto target = params.at("targetTriangles").get<std::size_t>();
            const std::size_t before = part.mesh->triangleCount();
            auto mesh = std::make_shared<const core::TriangleMesh>(core::decimateToTarget(*part.mesh, target));
            if (mesh->isEmpty())
                throw CommandError("Decimation removed every triangle; use a larger targetTriangles");
            replaceGeometry(ctx, part, "Decimate " + part.name, std::move(mesh), std::nullopt);
            Json out = partSummary(*ctx.document.part(part.id));
            out["trianglesBefore"] = before;
            return out;
        });
}

void registerPlacementCommands(CommandRegistry& r)
{
    r.add({"part.convert_units", "Convert units",
           "Rescales a part whose coordinates were read in the wrong unit, e.g. a scan saved in metres or inches "
           "but imported as millimetres: from=\"m\" scales by 1000. Scaling is about the world origin.",
           objectSchema({{"id", idParam()},
                         {"from", unitParam("Unit the coordinates are actually in.")},
                         {"to", unitParam("Unit they were interpreted as (default mm, the document unit).")}},
                        {"id", "from"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "convert units of");
              const auto from = core::parseLengthUnit(params.at("from").get<std::string>());
              const auto to = core::parseLengthUnit(params.value("to", std::string("mm")));
              if (!from || !to)
                  throw CommandError("Unknown unit");
              const double factor = core::toMillimetres(*from) / core::toMillimetres(*to);
              applyTransform(ctx, part, "Convert " + part.name + " from " + std::string(core::unitSymbol(*from)),
                             scaleAbout(factor, {}), std::make_pair(factor, core::Vec3{}));
              Json out = partSummary(*ctx.document.part(part.id));
              out["factor"] = factor;
              return out;
          });

    r.add({"part.calibrate", "Calibrate scale",
           "Scales a part so the distance between two points on it equals a known dimension (e.g. a gauge block or "
           "a measured feature of the scanned object). Point `a` stays fixed.",
           objectSchema(
               {{"id", idParam()},
                {"a", pointParam("First point [x, y, z] in mm, e.g. picked on the model.")},
                {"b", pointParam("Second point [x, y, z] in mm.")},
                {"distance",
                 {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "True distance between a and b."}}},
                {"units", unitParam("Unit of `distance` (default mm).")}},
               {"id", "a", "b", "distance"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "calibrate");
              const core::Vec3 a = toVec(params.at("a"));
              const core::Vec3 b = toVec(params.at("b"));
              const double measured = (b - a).length();
              if (!(measured > 1e-9))
                  throw CommandError("Points a and b coincide");
              const auto unit = core::parseLengthUnit(params.value("units", std::string("mm")));
              if (!unit)
                  throw CommandError("Unknown unit");
              const double wanted = params.at("distance").get<double>() * core::toMillimetres(*unit);
              const double factor = wanted / measured;
              applyTransform(ctx, part, "Calibrate " + part.name, scaleAbout(factor, a), std::make_pair(factor, a));
              Json out = partSummary(*ctx.document.part(part.id));
              out["factor"] = factor;
              out["measured"] = measured;
              out["distance"] = wanted;
              return out;
          });

    r.add({"part.transform", "Move/rotate part",
           "Moves and/or rotates a part rigidly: rotation first (about `center`, default the bounding box centre), "
           "then translation.",
           objectSchema(
               {{"id", idParam()},
                {"translate", pointParam("Offset [dx, dy, dz] in mm.")},
                {"axis", pointParam("Rotation axis direction.")},
                {"angle",
                 {{"type", "number"}, {"description", "Rotation angle in degrees (right-hand rule about axis)."}}},
                {"center", pointParam("Rotation centre in mm.")}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "transform");
              core::Affine tf;
              if (params.contains("angle") != params.contains("axis"))
                  throw CommandError("`axis` and `angle` must be given together");
              if (params.contains("angle")) {
                  const core::Vec3 axis = toVec(params.at("axis"));
                  if (axis.length() < 1e-12)
                      throw CommandError("Rotation axis is zero");
                  const core::Vec3 c =
                      params.contains("center") ? toVec(params.at("center")) : part.mesh->bounds().center();
                  const core::Mat3 rot =
                      core::Mat3::rotation(axis, params.at("angle").get<double>() * std::numbers::pi / 180.0);
                  tf = core::Affine{rot, c - rot * c};
              }
              if (params.contains("translate"))
                  tf = core::Affine::translationBy(toVec(params.at("translate"))).after(tf);
              applyTransform(ctx, part, "Move " + part.name, tf);
              Json out = partSummary(*ctx.document.part(part.id));
              out["transform"] = affineToJson(tf);
              return out;
          });

    r.add({"part.align", "Align to world",
           "Orients a part (typically a scan) to the world axes. A reference plane is fitted to `points` (3 or "
           "more picked points), to `triangles` (indices of a selected region) or given directly by `origin` + "
           "`normal`. The plane is turned onto the `target` world plane (its normal onto +Z for \"xy\", +Y for "
           "\"xz\", +X for \"yz\"; `flip` reverses it), the plane origin (or `origin`) moves to the world origin, "
           "and `xDirection` (optional) becomes the world X axis (Y for target \"yz\").",
           objectSchema(
               {{"id", idParam()},
                {"points",
                 {{"type", "array"},
                  {"items", pointParam("Point on the reference plane.")},
                  {"minItems", 3},
                  {"description", "Points on the reference plane, mm."}}},
                {"triangles",
                 {{"type", "array"},
                  {"items", {{"type", "integer"}, {"minimum", 0}}},
                  {"minItems", 1},
                  {"description", "Triangle indices of the reference region (e.g. from a selection)."}}},
                {"normal", pointParam("Reference plane normal (used with `origin` instead of points/triangles).")},
                {"origin", pointParam("Point that moves to the world origin (default: centroid of the reference).")},
                {"xDirection", pointParam("Direction in the part that becomes the in-plane first axis.")},
                {"target",
                 {{"type", "string"}, {"enum", {"xy", "xz", "yz"}}, {"description", "World plane (default xy)."}}},
                {"flip", {{"type", "boolean"}, {"description", "Reverse the plane normal."}}},
                {"keepPosition",
                 {{"type", "boolean"},
                  {"description", "Only rotate (about the plane origin); do not move to the world origin."}}}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "align");
              const int sources = static_cast<int>(params.contains("points")) +
                                  static_cast<int>(params.contains("triangles")) +
                                  static_cast<int>(params.contains("normal"));
              if (sources != 1)
                  throw CommandError("Give exactly one of `points`, `triangles` or `normal` (+ `origin`)");

              core::Vec3 origin;
              core::Vec3 normal;
              double rms = 0.0;
              if (params.contains("normal")) {
                  if (!params.contains("origin"))
                      throw CommandError("`normal` needs `origin`");
                  normal = toVec(params.at("normal"));
                  origin = toVec(params.at("origin"));
              } else {
                  std::vector<core::Vec3> pts;
                  if (params.contains("points")) {
                      for (const auto& p : params.at("points"))
                          pts.push_back(toVec(p));
                  } else {
                      std::vector<std::size_t> tris;
                      for (const auto& t : params.at("triangles")) {
                          const auto i = t.get<std::size_t>();
                          if (i >= part.mesh->triangleCount())
                              throw CommandError("Triangle index " + std::to_string(i) + " out of range");
                          tris.push_back(i);
                      }
                      pts = core::triangleVertices(*part.mesh, tris);
                  }
                  const auto plane = core::fitPlane(pts);
                  if (!plane)
                      throw CommandError("Reference points are collinear; pick points that span a plane");
                  normal = plane->normal;
                  rms = plane->rms;
                  origin = params.contains("origin") ? toVec(params.at("origin")) : plane->origin;
                  if (params.contains("triangles")) {
                      // Orient the fitted normal like the region's own face normals.
                      core::Vec3 avg;
                      for (const auto& t : params.at("triangles"))
                          avg = avg + part.mesh->faceNormal(t.get<std::size_t>());
                      if (avg.dot(normal) < 0.0)
                          normal = normal * -1.0;
                  }
              }
              if (normal.length() < 1e-12)
                  throw CommandError("Reference normal is zero");
              if (params.value("flip", false))
                  normal = normal * -1.0;

              std::optional<core::Vec3> xHint;
              if (params.contains("xDirection"))
                  xHint = toVec(params.at("xDirection"));
              auto frame = core::frameToWorld(origin, normal, xHint);
              if (!frame)
                  throw CommandError("Cannot build a frame from the reference plane");
              core::Affine tf = *frame;
              const std::string target = params.value("target", std::string("xy"));
              if (target == "xz") // +Z -> +Y, in-plane X stays X
                  tf = core::Affine{core::Mat3::rotation({1, 0, 0}, -std::numbers::pi / 2), {}}.after(tf);
              else if (target == "yz") // +Z -> +X, in-plane X -> +Y
                  tf = core::Affine{core::Mat3::fromRows({0, 0, 1}, {1, 0, 0}, {0, 1, 0}), {}}.after(tf);
              if (params.value("keepPosition", false))
                  tf = core::Affine::translationBy(origin).after(tf);
              applyTransform(ctx, part, "Align " + part.name, tf);
              Json out = partSummary(*ctx.document.part(part.id));
              out["transform"] = affineToJson(tf);
              out["planeOrigin"] = vecToJson(origin);
              out["planeNormal"] = vecToJson(normal.normalized());
              out["fitRms"] = rms;
              return out;
          });

    r.add({"part.place", "Place part",
           "Moves a part so its bounding box is centred on the world origin in X and Y, and either centred in Z "
           "(`onFloor` false) or resting on the Z=0 plane (`onFloor` true, default).",
           objectSchema({{"id", idParam()}, {"onFloor", {{"type", "boolean"}}}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& part = requireEditablePart(ctx.document, params, "place");
              const core::BoundingBox box = part.mesh->bounds();
              const core::Vec3 c = box.center();
              const bool onFloor = params.value("onFloor", true);
              const core::Vec3 offset{-c.x, -c.y, onFloor ? -box.min.z : -c.z};
              applyTransform(ctx, part, "Place " + part.name, core::Affine::translationBy(offset));
              Json out = partSummary(*ctx.document.part(part.id));
              out["offset"] = vecToJson(offset);
              return out;
          });
}

} // namespace

void registerScanCommands(CommandRegistry& registry)
{
    registerMeshCommands(registry);
    registerPlacementCommands(registry);
}

} // namespace aerocad::commands
