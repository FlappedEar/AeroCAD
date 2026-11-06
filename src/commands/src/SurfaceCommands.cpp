// SPDX-License-Identifier: GPL-3.0-or-later
// surface.*: base surfaces built from a region selection.
#include "CommandHelpers.hpp"

#include "aerocad/cad/Design.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/RegionFit.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace aerocad::commands {

namespace {

const model::Part& requireSurfacePart(model::Document& doc, const Json& params)
{
    const model::Part& source = requirePart(doc, params);
    if (source.kind == model::PartKind::Sketch)
        throw CommandError("Part " + std::to_string(source.id) + " is a sketch; offset and thicken work on surfaces");
    if (!source.shape || source.shape->isNull())
        throw CommandError("Part " + std::to_string(source.id) +
                           " has no exact surface; fit one with surface.fit_selection first");
    return source;
}

/// Adds a part built from `shape` and pushes an undo entry that removes it again.
Json addShapePart(CommandContext& ctx, const model::Part& source, const geometry::Shape& shape,
                  const std::string& name, const std::string& origin, const std::string& undoTitle)
{
    model::Part part;
    part.name = name;
    part.source = origin + " of part " + std::to_string(source.id);
    part.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(shape));
    part.shape = shape;

    model::Document& doc = ctx.document;
    const model::PartId id = doc.addPart(part);
    part.id = id;
    ctx.undoStack.push({undoTitle + " " + part.name, [&doc, id]() { doc.removePart(id); },
                        [&doc, part]() { doc.addPart(part); }});
    return partToJson(*doc.part(id));
}

/// Diagonal of the outlines' bounding box (mm).
double outlinesDiagonal(const std::vector<geometry::Shape>& outlines)
{
    core::BoundingBox box;
    for (const auto& o : outlines)
        box.merge(o.bounds());
    return box.diagonal();
}

const cad::SketchResult& requireSketch(CommandContext& ctx, const Json& id)
{
    if (ctx.design == nullptr)
        throw CommandError("No design in this session");
    const auto sketchId = id.get<cad::FeatureId>();
    const auto& sketches = ctx.design->sketches();
    const auto it = sketches.find(sketchId);
    if (it == sketches.end())
        throw CommandError("No evaluated sketch with id " + std::to_string(sketchId));
    return it->second;
}

/// The sketch's closed profiles (all, or the given indices).
std::vector<geometry::Shape> sketchOutlines(const cad::SketchResult& sk, const Json& params)
{
    std::vector<geometry::Shape> outlines;
    if (params.contains("profiles")) {
        for (const auto& i : params.at("profiles")) {
            const auto index = i.get<std::size_t>();
            if (index >= sk.geometry.profiles.size())
                throw CommandError("Sketch " + sk.name + " has no profile " + std::to_string(index));
            outlines.push_back(sk.geometry.profiles[index]);
        }
    } else {
        outlines = sk.geometry.profiles;
    }
    if (outlines.empty())
        throw CommandError("Sketch " + sk.name + " has no closed profile; close the outline first");
    return outlines;
}

/// Triangles of a scan under a sketch outline: connected to the triangle nearest `seed`,
/// facing the sketch, and inside the outline's rectangle (sketch coordinates) grown by
/// `margin`. Enough area around the outline for a smooth fit; the outline cuts it later.
core::TriangleSet meshUnderSketch(const core::TriangleMesh& mesh, const cad::SketchResult& sk, const core::Vec3& seed,
                                  double margin)
{
    double u0 = std::numeric_limits<double>::max(), v0 = u0, u1 = -u0, v1 = -u0;
    for (const auto& c : sk.curves) {
        if (c.construction)
            continue;
        const double r = c.kind == geometry::SketchCurve::Kind::Circle ? c.radius : 0.0;
        for (const auto& p : c.points) {
            u0 = std::min(u0, p.x - r);
            u1 = std::max(u1, p.x + r);
            v0 = std::min(v0, p.y - r);
            v1 = std::max(v1, p.y + r);
        }
    }
    // Arcs bulge past their points; the margin covers that.
    u0 -= margin;
    v0 -= margin;
    u1 += margin;
    v1 += margin;

    const auto& v = mesh.vertices();
    auto inside = [&](std::uint32_t t) {
        const auto& tri = mesh.triangles()[t];
        const core::Vec3 c = (v[tri[0]] + v[tri[1]] + v[tri[2]]) * (1.0 / 3.0);
        const auto uv = sk.plane.toLocal(c);
        return uv.x >= u0 && uv.x <= u1 && uv.y >= v0 && uv.y <= v1 && mesh.faceNormal(t).dot(sk.plane.normal) > 0.2;
    };
    std::uint32_t start = core::MeshAdjacency::none;
    double best = std::numeric_limits<double>::max();
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        if (!inside(t))
            continue;
        const auto& tri = mesh.triangles()[t];
        const double d = ((v[tri[0]] + v[tri[1]] + v[tri[2]]) * (1.0 / 3.0) - seed).length();
        if (d < best) {
            best = d;
            start = t;
        }
    }
    if (start == core::MeshAdjacency::none)
        throw CommandError("No part of the mesh faces the sketch under its outline");
    const core::MeshAdjacency adjacency(mesh);
    std::vector<char> seen(mesh.triangleCount(), 0);
    core::TriangleSet out{start};
    seen[start] = 1;
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (const auto n : adjacency.neighbours(out[i])) {
            if (n != core::MeshAdjacency::none && !seen[n] && inside(n)) {
                seen[n] = 1;
                out.push_back(n);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

void registerSurfaceCommands(CommandRegistry& r)
{
    r.add({"surface.fit_selection", "Fit surface to selection",
           "Fits one smooth B-spline (NURBS) surface to the region selection of a mesh part and adds it as a new "
           "part with exact geometry, trimmed to the selection outline. Reports the deviation both ways (mesh to "
           "surface and surface to mesh) in mm. Holes of the selection are cut out. Any amount of wrap works as "
           "long as the region is a simple sheet (no handles); see method.",
           objectSchema(
               {{"id", idParam()},
                {"tolerance",
                 {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance in mm. Default 0.25."}}},
                {"gridSize",
                 {{"type", "integer"},
                  {"minimum", 4},
                  {"maximum", 200},
                  {"description", "Resampling grid nodes along the longer side. Default: from the point count."}}},
                {"method",
                 {{"type", "string"},
                  {"enum", {"auto", "plane", "unfold"}},
                  {"description", "unfold: region flattened by a conformal map, any amount of wrap (mirror "
                                  "housings, long curved strips), needs a simple sheet; plane: height field over "
                                  "the best-fit plane, no folds; auto (default): unfold, else plane."}}},
                {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& source = requirePart(ctx.document, params);
              if (!source.mesh || !source.selection || source.selection->empty())
                  throw CommandError("Select a region on part " + std::to_string(source.id) + " first");

              geometry::SurfaceFitOptions options;
              options.tolerance = params.value("tolerance", options.tolerance);
              options.gridSize = params.value("gridSize", options.gridSize);
              const std::string method = params.value("method", std::string("auto"));
              options.method = method == "plane"    ? geometry::SurfaceFitMethod::Plane
                               : method == "unfold" ? geometry::SurfaceFitMethod::Unfold
                                                    : geometry::SurfaceFitMethod::Auto;
              geometry::SurfaceFitResult fit;
              try {
                  fit = geometry::fitSurface(*source.mesh, *source.selection, options);
              } catch (const geometry::SurfaceFitError& e) {
                  throw CommandError(e.what());
              }

              model::Part part;
              part.name = params.contains("name") ? params["name"].get<std::string>() : source.name + " surface";
              part.source = "surface.fit_selection of part " + std::to_string(source.id);
              part.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(fit.face));
              part.shape = fit.face;

              model::Document& doc = ctx.document;
              const model::PartId id = doc.addPart(part);
              part.id = id;
              ctx.undoStack.push({"Fit surface " + part.name, [&doc, id]() { doc.removePart(id); },
                                  [&doc, part]() { doc.addPart(part); }});

              const auto& rep = fit.report;
              Json result = partToJson(*doc.part(id));
              result["fit"] = {{"meshToSurfaceMax", rep.meshToSurfaceMax},
                               {"meshToSurfaceRms", rep.meshToSurfaceRms},
                               {"surfaceToMeshMax", rep.surfaceToMeshMax},
                               {"outsideTrimMax", rep.outsideTrimMax},
                               {"tolerance", options.tolerance},
                               {"withinTolerance", rep.withinTolerance},
                               {"poles", {rep.uPoles, rep.vPoles}},
                               {"trimmed", rep.trimmed},
                               {"holes", rep.holes},
                               {"method", rep.method},
                               {"milliseconds", rep.milliseconds}};
              result["warnings"] = rep.warnings;
              return result;
          });
    r.add({"surface.fit_regions", "Fit surfaces to mesh",
           "Turns a scanned mesh (or its region selection) into exact geometry: splits it into smooth patches at "
           "creases, fits a B-spline surface to each and sews them into one shell, a solid when it closes up "
           "(a watertight scan becomes a solid body). Tubes and strongly bent patches are split further. Adds "
           "the result as a new part and reports each patch's deviation.",
           objectSchema(
               {{"id", idParam()},
                {"creaseAngle",
                 {{"type", "number"},
                  {"exclusiveMinimum", 0},
                  {"maximum", 180},
                  {"description", "Angle between neighbouring triangles (degrees) that splits patches. Default 30."}}},
                {"tolerance",
                 {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance in mm. Default 0.25."}}},
                {"useSelection",
                 {{"type", "boolean"},
                  {"description", "Only the region selection (default when there is one); false: the whole mesh."}}},
                {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& source = requirePart(ctx.document, params);
              if (!source.mesh || source.mesh->triangleCount() == 0)
                  throw CommandError("Part " + std::to_string(source.id) + " has no mesh");
              const bool hasSelection = source.selection && !source.selection->empty();
              const bool useSelection = params.value("useSelection", hasSelection);
              if (useSelection && !hasSelection)
                  throw CommandError("Select a region on part " + std::to_string(source.id) + " first");

              geometry::RegionFitOptions options;
              options.creaseAngleDeg = params.value("creaseAngle", options.creaseAngleDeg);
              options.fit.tolerance = params.value("tolerance", options.fit.tolerance);
              geometry::RegionFitResult fit;
              try {
                  fit = geometry::fitRegions(*source.mesh, useSelection ? *source.selection : core::TriangleSet{},
                                             options);
              } catch (const geometry::SurfaceFitError& e) {
                  throw CommandError(e.what());
              }
              Json result = addShapePart(ctx, source, fit.shape,
                                         params.value("name", source.name + (fit.solid ? " solid" : " surfaces")),
                                         "surface.fit_regions", "Fit surfaces");
              Json patches = Json::array();
              double worst = 0.0;
              bool allWithin = true;
              for (const auto& p : fit.patches) {
                  Json j{{"triangles", p.triangles.size()}};
                  if (p.error.empty()) {
                      j["meshToSurfaceMax"] = p.report.meshToSurfaceMax;
                      j["surfaceToMeshMax"] = p.report.surfaceToMeshMax;
                      j["withinTolerance"] = p.report.withinTolerance;
                      j["method"] = p.report.method;
                      worst = std::max(worst, p.report.meshToSurfaceMax);
                      allWithin = allWithin && p.report.withinTolerance;
                  } else {
                      j["error"] = p.error;
                      allWithin = false;
                  }
                  patches.push_back(std::move(j));
              }
              result["fit"] = {{"patches", patches},
                               {"faces", fit.faces},
                               {"freeEdges", fit.freeEdges},
                               {"solid", fit.solid},
                               {"volume", fit.volume},
                               {"meshToSurfaceMax", worst},
                               {"withinTolerance", allWithin},
                               {"tolerance", options.fit.tolerance},
                               {"sewTolerance", fit.sewTolerance},
                               {"milliseconds", fit.milliseconds}};
              result["warnings"] = fit.warnings;
              return result;
          });
    r.add({"surface.trim_by_sketch", "Trim surface by sketch",
           "Projects the closed profiles of a sketch onto a curved surface part along the sketch normal and keeps "
           "the part of the surface inside them (keep: inside, default) or outside, as a new surface part that "
           "lies exactly on the original surface. Typical use: fit a surface to a scan (surface.fit_selection), "
           "sketch the outline of a part over it (sketch.create with surface: {part}), trim, then thicken "
           "(surface.thicken) into a solid that follows the scan, e.g. an aileron or a flap.",
           objectSchema({{"id", idParam()},
                         {"sketch", {{"type", "integer"}, {"minimum", 1}, {"description", "Sketch feature id."}}},
                         {"profiles",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Profile indices (sketch.profiles); default all."}}},
                         {"keep", {{"type", "string"}, {"enum", {"inside", "outside"}}}},
                         {"extend",
                          {{"type", "boolean"},
                           {"description",
                            "Use the whole fitted surface, past its own (ragged) trim outline and holes, so the "
                            "edges are the sketch outline's (default true; single-face surfaces)."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"id", "sketch"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& source = requireSurfacePart(ctx.document, params);
              const auto& sk = requireSketch(ctx, params.at("sketch"));
              const auto outlines = sketchOutlines(sk, params);
              const std::string keep = params.value("keep", std::string("inside"));
              geometry::Shape shape;
              try {
                  shape = geometry::trimSurface(*source.shape, outlines, sk.plane.normal, keep != "outside",
                                                params.value("extend", true));
              } catch (const geometry::OffsetError& e) {
                  throw CommandError(e.what());
              }
              Json result = addShapePart(ctx, source, shape, params.value("name", source.name + " " + sk.name),
                                         "surface.trim_by_sketch", "Trim surface");
              result["area"] = geometry::surfaceArea(shape);
              result["faces"] = shape.faceCount();
              result["keep"] = keep;
              return result;
          });
    r.add({"surface.part_from_sketch", "Part on surface",
           "One step from a sketch outline to a solid part that follows a curved surface or scan, e.g. an aileron "
           "or flap drawn over a scanned wing: the outline is projected onto the surface along the sketch normal, "
           "the surface inside it is cut out and thickened. On a scan (mesh) a smooth surface is first fitted to "
           "the area under the outline, so no region selection or separate fit is needed. The edges follow the "
           "sketch outline exactly. The sketch should be made with sketch.create surface: {part}; that part is "
           "used unless `id` is given.",
           objectSchema({{"sketch", {{"type", "integer"}, {"minimum", 1}, {"description", "Sketch feature id."}}},
                         {"id", idParam()},
                         {"thickness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "mm."}}},
                         {"side",
                          {{"type", "string"},
                           {"enum", {"inside", "outside", "both"}},
                           {"description", "inside (default): from the surface into the object, against the way "
                                           "the surface faces; outside: away from it; both: half each way."}}},
                         {"profiles",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Profile indices (sketch.profiles); default all."}}},
                         {"tolerance",
                          {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance (scan), mm. Default 0.25."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"sketch", "thickness"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto& sk = requireSketch(ctx, params.at("sketch"));
              const auto outlines = sketchOutlines(sk, params);
              Json on;
              if (const cad::Feature* f = ctx.design->feature(sk.id); f != nullptr)
                  on = f->params.value("onSurface", Json::object());
              model::PartId partId = 0;
              if (params.contains("id"))
                  partId = params.at("id").get<model::PartId>();
              else if (on.contains("part"))
                  partId = on.at("part").get<model::PartId>();
              else
                  throw CommandError("Sketch " + sk.name + " was not made over a surface; give the surface part `id`");
              const model::Part* source = ctx.document.part(partId);
              if (source == nullptr)
                  throw CommandError("No part with id " + std::to_string(partId));
              if (source->kind == model::PartKind::Sketch)
                  throw CommandError("Part " + std::to_string(partId) + " is a sketch");

              const double thickness = params.at("thickness").get<double>();
              const std::string sideName = params.value("side", std::string("inside"));
              const geometry::ThickenSide side = sideName == "outside" ? geometry::ThickenSide::Outside
                                                 : sideName == "both"  ? geometry::ThickenSide::Both
                                                                       : geometry::ThickenSide::Inside;
              Json fitReport;
              geometry::Shape surface;
              if (source->shape && !source->shape->isNull()) {
                  surface = *source->shape;
              } else {
                  if (!source->mesh || source->mesh->triangleCount() == 0)
                      throw CommandError("Part " + std::to_string(partId) + " has no surface");
                  core::Vec3 seed = sk.plane.origin;
                  if (on.contains("point") && on.value("part", Json()) == partId) {
                      const auto& p = on.at("point");
                      seed = {p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>()};
                  }
                  const auto box = outlinesDiagonal(outlines);
                  const double margin = std::max(5.0, 0.15 * box);
                  const auto region = meshUnderSketch(*source->mesh, sk, seed, margin);
                  geometry::SurfaceFitOptions options;
                  options.tolerance = params.value("tolerance", options.tolerance);
                  try {
                      auto fit = geometry::fitSurface(*source->mesh, region, options);
                      surface = fit.face;
                      fitReport = {{"triangles", region.size()},
                                   {"meshToSurfaceMax", fit.report.meshToSurfaceMax},
                                   {"meshToSurfaceRms", fit.report.meshToSurfaceRms},
                                   {"withinTolerance", fit.report.withinTolerance},
                                   {"method", fit.report.method}};
                  } catch (const geometry::SurfaceFitError& e) {
                      throw CommandError(std::string("Could not fit a surface under the outline: ") + e.what());
                  }
              }
              geometry::Shape solid;
              double area = 0.0;
              try {
                  const auto skin = geometry::trimSurface(surface, outlines, sk.plane.normal, true, true);
                  area = geometry::surfaceArea(skin);
                  solid = geometry::thickenSurface(skin, thickness, side);
              } catch (const geometry::OffsetError& e) {
                  throw CommandError(e.what());
              }
              Json result = addShapePart(ctx, *source, solid, params.value("name", sk.name + " part"),
                                         "surface.part_from_sketch", "Part on surface");
              result["thickness"] = thickness;
              result["side"] = sideName;
              result["area"] = area;
              result["volume"] = solid.volume();
              if (!fitReport.is_null())
                  result["fit"] = fitReport;
              return result;
          });
    r.add({"surface.offset", "Offset surface",
           "Adds a copy of a surface part moved by a distance along its normal (negative: against it), for "
           "example a clearance surface that keeps 2 mm off a body. The fitted base surface's normal points to "
           "the side the mesh triangles face (usually outward).",
           objectSchema({{"id", idParam()},
                         {"distance", {{"type", "number"}, {"description", "Offset in mm along the normal; not 0."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"id", "distance"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& source = requireSurfacePart(ctx.document, params);
              const double distance = params.at("distance").get<double>();
              geometry::Shape shape;
              try {
                  shape = geometry::offsetSurface(*source.shape, distance);
              } catch (const geometry::OffsetError& e) {
                  throw CommandError(e.what());
              }
              Json result = addShapePart(ctx, source, shape, params.value("name", source.name + " offset"),
                                         "surface.offset", "Offset");
              result["distance"] = distance;
              result["area"] = geometry::surfaceArea(shape);
              return result;
          });

    r.add({"surface.thicken", "Thicken surface",
           "Turns a surface part into a closed solid of the given thickness, with side walls along its boundary. "
           "side: outside (along the normal, default), inside (against it) or both (half each way). The solid "
           "can be used by the solid modelling commands.",
           objectSchema({{"id", idParam()},
                         {"thickness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "mm."}}},
                         {"side", {{"type", "string"}, {"enum", {"outside", "inside", "both"}}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"id", "thickness"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& source = requireSurfacePart(ctx.document, params);
              const double thickness = params.at("thickness").get<double>();
              const std::string sideName = params.value("side", std::string("outside"));
              const geometry::ThickenSide side = sideName == "inside" ? geometry::ThickenSide::Inside
                                                 : sideName == "both" ? geometry::ThickenSide::Both
                                                                      : geometry::ThickenSide::Outside;
              geometry::Shape shape;
              try {
                  shape = geometry::thickenSurface(*source.shape, thickness, side);
              } catch (const geometry::OffsetError& e) {
                  throw CommandError(e.what());
              }
              Json result = addShapePart(ctx, source, shape, params.value("name", source.name + " solid"),
                                         "surface.thicken", "Thicken");
              result["thickness"] = thickness;
              result["side"] = sideName;
              result["volume"] = shape.volume();
              return result;
          });
}

} // namespace aerocad::commands
