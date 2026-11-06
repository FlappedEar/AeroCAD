// SPDX-License-Identifier: GPL-3.0-or-later
// surface.*: base surfaces built from a region selection.
#include "CommandHelpers.hpp"

#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/RegionFit.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <algorithm>
#include <memory>
#include <string>

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
