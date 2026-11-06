// SPDX-License-Identifier: GPL-3.0-or-later
// selection.*: region selection on a part's mesh (click a smooth region, brush, lasso,
// grow / shrink / invert / clear). The selection is stored in the part, so it is undoable
// like any other change and is the input for fitting a base surface.
#include "CommandHelpers.hpp"

#include "aerocad/core/MeshSelection.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aerocad::commands {

namespace {

Json pointSchema(const char* description)
{
    return Json{{"type", "array"},
                {"items", {{"type", "number"}}},
                {"minItems", 3},
                {"maxItems", 3},
                {"description", description}};
}

Json modeSchema(const char* defaultMode)
{
    return Json{{"type", "string"},
                {"enum", {"replace", "add", "subtract"}},
                {"description",
                 std::string("How the result combines with the current selection. Default: ") + defaultMode + "."}};
}

core::Vec3 toVec(const Json& a)
{
    return {a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
}

core::SelectionMode parseMode(const Json& params, core::SelectionMode fallback)
{
    if (!params.contains("mode"))
        return fallback;
    const std::string m = params["mode"].get<std::string>();
    if (m == "add")
        return core::SelectionMode::Add;
    if (m == "subtract")
        return core::SelectionMode::Subtract;
    return core::SelectionMode::Replace;
}

const model::Part& requireMeshPart(const model::Document& doc, const Json& params)
{
    const model::Part& p = requirePart(doc, params);
    if (!p.mesh || p.mesh->isEmpty())
        throw CommandError("Part " + std::to_string(p.id) + " has no mesh to select on");
    return p;
}

const core::TriangleSet& currentSelection(const model::Part& p)
{
    static const core::TriangleSet empty;
    return p.selection ? *p.selection : empty;
}

Json selectionSummary(const model::Part& p, bool includeTriangles = false)
{
    const core::TriangleSet& sel = currentSelection(p);
    Json out{{"id", p.id},
             {"selectedTriangles", sel.size()},
             {"totalTriangles", p.mesh ? p.mesh->triangleCount() : 0},
             {"areaMm2", p.mesh ? core::selectionArea(*p.mesh, sel) : 0.0}};
    if (includeTriangles)
        out["triangles"] = sel;
    return out;
}

/// Stores a new selection in the part and records it for undo.
Json applySelection(CommandContext& ctx, const model::Part& part, core::TriangleSet selection, const std::string& label)
{
    const model::Part before = part;
    std::shared_ptr<const core::TriangleSet> stored;
    if (!selection.empty())
        stored = std::make_shared<const core::TriangleSet>(std::move(selection));
    ctx.document.modifyPart(before.id, [&](model::Part& p) { p.selection = stored; });
    const model::Part& after = *ctx.document.part(before.id);
    pushPartStateUndo(ctx, label + " (" + before.name + ")", before.id, before, after);
    return selectionSummary(after);
}

} // namespace

void registerSelectionCommands(CommandRegistry& r)
{
    r.add({"selection.get", "Get selection",
           "Returns the region selection of a part: number of selected triangles, their area, and optionally the "
           "triangle indices.",
           objectSchema({{"id", idParam()},
                         {"includeTriangles",
                          {{"type", "boolean"}, {"description", "Include triangle indices. Default false."}}}},
                        {"id"})},
          [](CommandContext& ctx, const Json& params) {
              return selectionSummary(requirePart(ctx.document, params), params.value("includeTriangles", false));
          });

    r.add({"selection.set", "Set selection", "Sets the region selection of a part to the given triangle indices.",
           objectSchema({{"id", idParam()},
                         {"triangles",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Triangle indices of the part mesh."}}},
                         {"mode", modeSchema("replace")}},
                        {"id", "triangles"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& p = requireMeshPart(ctx.document, params);
              core::TriangleSet change;
              for (const auto& t : params.at("triangles")) {
                  const auto index = t.get<std::uint64_t>();
                  if (index >= p.mesh->triangleCount())
                      throw CommandError("Triangle index " + std::to_string(index) + " out of range");
                  change.push_back(static_cast<std::uint32_t>(index));
              }
              core::normalizeSelection(change);
              return applySelection(
                  ctx, p,
                  core::combineSelection(currentSelection(p), change, parseMode(params, core::SelectionMode::Replace)),
                  "Set selection");
          });

    r.add({"selection.pick_region", "Select smooth region",
           "Selects the smooth region around a point on the part: grows from the triangle nearest to the point "
           "across edges until the surface bends more than maxAngle between neighbouring triangles (creases, "
           "sharp edges) or more than maxSeedAngle from the starting triangle.",
           objectSchema({{"id", idParam()},
                         {"point", pointSchema("Point on or near the surface [x, y, z] in mm.")},
                         {"maxAngle",
                          {{"type", "number"},
                           {"minimum", 0},
                           {"maximum", 180},
                           {"description", "Max angle between neighbouring triangles in degrees. Default 20."}}},
                         {"maxSeedAngle",
                          {{"type", "number"},
                           {"minimum", 0},
                           {"maximum", 180},
                           {"description", "Max angle to the starting triangle in degrees. Default 180 (off)."}}},
                         {"mode", modeSchema("replace")}},
                        {"id", "point"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& p = requireMeshPart(ctx.document, params);
              const auto seed = core::nearestTriangle(*p.mesh, toVec(params.at("point")));
              const auto adjacency = core::adjacencyFor(p.mesh);
              core::SmoothRegionOptions options;
              options.maxNeighbourAngleDeg = params.value("maxAngle", options.maxNeighbourAngleDeg);
              options.maxSeedAngleDeg = params.value("maxSeedAngle", options.maxSeedAngleDeg);
              const core::TriangleSet region = core::selectSmoothRegion(*p.mesh, *adjacency, *seed, options);
              return applySelection(
                  ctx, p,
                  core::combineSelection(currentSelection(p), region, parseMode(params, core::SelectionMode::Replace)),
                  "Select region");
          });

    r.add(
        {"selection.brush", "Brush selection",
         "Paints a selection along a stroke: every triangle within radius of a stroke point. By default only "
         "the surface connected to the point under the brush is painted, so the brush does not go through "
         "thin walls.",
         objectSchema(
             {{"id", idParam()},
              {"points",
               {{"type", "array"},
                {"minItems", 1},
                {"items", pointSchema("[x, y, z] in mm.")},
                {"description", "Stroke points on the surface in mm."}}},
              {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Brush radius in mm."}}},
              {"connected",
               {{"type", "boolean"}, {"description", "Paint only the surface connected to each point. Default true."}}},
              {"mode", modeSchema("add")}},
             {"id", "points", "radius"}),
         true},
        [](CommandContext& ctx, const Json& params) {
            const model::Part& p = requireMeshPart(ctx.document, params);
            const double radius = params.at("radius").get<double>();
            std::shared_ptr<const core::MeshAdjacency> adjacency;
            if (params.value("connected", true))
                adjacency = core::adjacencyFor(p.mesh);
            core::TriangleSet stroke;
            for (const auto& point : params.at("points")) {
                const core::TriangleSet dab = core::selectBrushDab(*p.mesh, adjacency.get(), toVec(point), radius);
                stroke = core::combineSelection(stroke, dab, core::SelectionMode::Add);
            }
            return applySelection(
                ctx, p,
                core::combineSelection(currentSelection(p), stroke, parseMode(params, core::SelectionMode::Add)),
                "Brush selection");
        });

    r.add({"selection.lasso", "Lasso selection",
           "Selects the triangles whose centre lies inside a polygon drawn on the screen. The view is given as a "
           "pinhole camera in model coordinates; polygon points are normalized viewport coordinates (x 0..1 left to "
           "right, y 0..1 top to bottom).",
           objectSchema(
               {{"id", idParam()},
                {"polygon",
                 {{"type", "array"},
                  {"minItems", 3},
                  {"items", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}}},
                  {"description", "Polygon [[x, y], ...] in normalized viewport coordinates."}}},
                {"view",
                 objectSchema(
                     {{"eye", pointSchema("Camera position in mm.")},
                      {"forward", pointSchema("Viewing direction.")},
                      {"up", pointSchema("Screen up direction.")},
                      {"fov",
                       {{"type", "number"},
                        {"exclusiveMinimum", 0},
                        {"exclusiveMaximum", 180},
                        {"description", "Vertical field of view in degrees."}}},
                      {"aspect",
                       {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Viewport width / height."}}},
                      {"orthographic",
                       {{"type", "boolean"}, {"description", "Parallel projection (fov unused). Default false."}}},
                      {"height",
                       {{"type", "number"},
                        {"exclusiveMinimum", 0},
                        {"description", "Orthographic only: viewport height in mm."}}}},
                     {"eye", "forward", "up", "fov", "aspect"})},
                {"visibleOnly",
                 {{"type", "boolean"},
                  {"description",
                   "Only triangles visible from the camera. Default true; false selects through the part."}}},
                {"mode", modeSchema("replace")}},
               {"id", "polygon", "view"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& p = requireMeshPart(ctx.document, params);
              const Json& v = params.at("view");
              core::ViewParams view;
              view.eye = toVec(v.at("eye"));
              view.forward = toVec(v.at("forward"));
              view.up = toVec(v.at("up"));
              view.fovYDeg = v.at("fov").get<double>();
              view.aspect = v.at("aspect").get<double>();
              view.orthographic = v.value("orthographic", false);
              view.height = v.value("height", 0.0);
              if (view.orthographic && view.height <= 0.0)
                  throw CommandError("view.height is required for an orthographic view");
              std::vector<std::array<double, 2>> polygon;
              for (const auto& pt : params.at("polygon"))
                  polygon.push_back({pt[0].get<double>(), pt[1].get<double>()});
              core::LassoOptions options;
              options.visibleOnly = params.value("visibleOnly", true);
              const core::TriangleSet lasso = core::selectInLasso(*p.mesh, view, polygon, options);
              return applySelection(
                  ctx, p,
                  core::combineSelection(currentSelection(p), lasso, parseMode(params, core::SelectionMode::Replace)),
                  "Lasso selection");
          });

    r.add({"selection.grow", "Grow / shrink selection",
           "Grows the selection by rings of neighbouring triangles (positive steps) or shrinks it (negative steps).",
           objectSchema({{"id", idParam()},
                         {"steps",
                          {{"type", "integer"},
                           {"minimum", -1000},
                           {"maximum", 1000},
                           {"description", "Rings to add (> 0) or remove (< 0). Default 1."}}}},
                        {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& p = requireMeshPart(ctx.document, params);
              const int steps = params.value("steps", 1);
              const auto adjacency = core::adjacencyFor(p.mesh);
              return applySelection(ctx, p, core::growSelection(*adjacency, currentSelection(p), steps),
                                    steps >= 0 ? "Grow selection" : "Shrink selection");
          });

    r.add({"selection.invert", "Invert selection",
           "Selects every triangle that is not selected, and deselects the rest.",
           objectSchema({{"id", idParam()}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& p = requireMeshPart(ctx.document, params);
              return applySelection(ctx, p, core::invertSelection(p.mesh->triangleCount(), currentSelection(p)),
                                    "Invert selection");
          });

    r.add({"selection.clear", "Clear selection", "Removes the region selection of a part.",
           objectSchema({{"id", idParam()}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part& p = requirePart(ctx.document, params);
              if (!p.selection)
                  return selectionSummary(p);
              return applySelection(ctx, p, {}, "Clear selection");
          });
}

} // namespace aerocad::commands
