// SPDX-License-Identifier: GPL-3.0-or-later
// Commands that give an AI agent (MCP client) the context it needs before it changes the
// document: a project summary with units and revision, and measurements that report the
// method used and data-quality warnings.
#include "aerocad/commands/Session.hpp"

#include "aerocad/core/Measure.hpp"

#include "CommandHelpers.hpp"

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
           "their ids, names, kind (imported = scan or file, body = solid or surface from the timeline, sketch, "
           "plane), the timeline feature that made them (featureId; tools taking `sketch` or `feature` want that "
           "id), sizes and bounds, and the undo state. Call this first, and again after changes. "
           "All lengths are in millimetres, Z is up.",
           Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}},
          [](CommandContext& ctx, const Json&) {
              Json parts = Json::array();
              for (const auto& p : ctx.document.parts()) {
                  const core::BoundingBox box = p.mesh ? p.mesh->bounds() : core::BoundingBox{};
                  parts.push_back({{"id", p.id},
                                   {"name", p.name},
                                   {"kind", p.kind == model::PartKind::Body     ? "body"
                                            : p.kind == model::PartKind::Sketch ? "sketch"
                                            : p.kind == model::PartKind::Plane  ? "plane"
                                                                                : "imported"},
                                   {"featureId", p.featureId},
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

// --- The 3D view (only in the AeroCAD window) ------------------------------------------

constexpr const char* kNeedsWindow =
    "This tool needs the AeroCAD window: it looks at the 3D view, which aerocad-mcp --headless does not have. "
    "Use project_get_context, document_parts and geometry_measure instead.";

Json viewDirectionSchema()
{
    return Json{{"description",
                 "Where the camera looks from: front (-Y), back (+Y), right (+X), left (-X), top (+Z), bottom (-Z), "
                 "iso (front-right-top), or [x, y, z], a direction from the model towards the camera (Z up). "
                 "Omitted: the current view direction."},
                {"oneOf",
                 Json::array({Json{{"type", "string"},
                                   {"enum", {"front", "back", "right", "left", "top", "bottom", "iso", "home"}}},
                              pointSchema("Direction towards the camera.")})}};
}

Json viewFramingProperties()
{
    return Json{{"direction", viewDirectionSchema()},
                {"part", {{"type", "integer"}, {"minimum", 1}, {"description", "Fill the view with this part."}}},
                {"center", pointSchema("Fill the view with the area of `radius` around this point (mm).")},
                {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "With center: mm."}}},
                {"fit", {{"type", "boolean"}, {"description", "Fit all visible parts (default when the direction "
                                                              "changes and no part or center is given)."}}},
                {"orthographic", {{"type", "boolean"}, {"description", "Parallel projection instead of perspective."}}}};
}

void registerViewCommands(CommandRegistry& r)
{
    Json shot = viewFramingProperties();
    shot["area"] = {{"type", "string"},
                    {"enum", {"viewport", "window"}},
                    {"description", "viewport (default): only the 3D view, so view_pick can turn a pixel into a "
                                    "point. window: the whole AeroCAD window as the user sees it (panels, "
                                    "dialogs, status bar)."}};
    shot["maxSize"] = {{"type", "integer"},
                       {"minimum", 200},
                       {"maximum", 2000},
                       {"description", "Longest side of the image in pixels (default 1024)."}};
    r.add({"view.screenshot", "Screenshot",
           "Shows you what the user sees: returns an image of the AeroCAD 3D view (or the whole window), optionally "
           "after turning the camera (direction) and framing a part or an area first. Use it to check a result "
           "(did the fin land on the bumper? is the sketch where you meant it?), to find where to work on a scan, "
           "and before telling the user that something is done. The result also lists the camera, the visible parts "
           "and the size of the image; view_pick turns pixel positions in it into 3D points on the model.",
           objectSchema(shot)},
          [](CommandContext&, const Json&) -> Json { throw CommandError(kNeedsWindow); });

    r.add({"view.set", "Set view",
           "Turns and frames the camera of the AeroCAD 3D view, as the ViewCube and Zoom to Fit do: a standard "
           "direction or any direction, all parts, one part, or an area around a point. Does not change the "
           "document. Returns the camera.",
           objectSchema(viewFramingProperties())},
          [](CommandContext&, const Json&) -> Json { throw CommandError(kNeedsWindow); });

    r.add({"view.pick", "Pick in view",
           "What is under positions of the last view_screenshot (area viewport): for each [x, y] (0..1 from the "
           "left and from the top of the image) the part hit, the 3D point on its surface (mm) and the surface "
           "normal there, or null when the ray misses. Use it to turn \"the corner of the bumper there\" into "
           "coordinates for sketch_create (surface: {part, point}) or selection tools. The camera must not have "
           "moved since the screenshot.",
           objectSchema({{"points",
                          {{"type", "array"},
                           {"minItems", 1},
                           {"maxItems", 50},
                           {"items",
                            {{"type", "array"},
                             {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                             {"minItems", 2},
                             {"maxItems", 2}}},
                           {"description", "Image positions [x, y], 0..1."}}}},
                        Json::array({"points"}))},
          [](CommandContext&, const Json&) -> Json { throw CommandError(kNeedsWindow); });
}

void registerShowResults(CommandRegistry& r)
{
    r.add({"view.show_results", "Show results in 3D",
           "Shows a finished wind tunnel run in the AeroCAD 3D view, as the user does in SIMULATE > Results: the "
           "surface pressure (Cp) map on the part, streamlines, sections, wake or vortex cores. Opens the "
           "simulation panel on that run. Then view_screenshot shows it to you. run: \"\" hides the results. "
           "Only for runs whose part is in the open document unchanged (simulation_list: sourceInDocument).",
           objectSchema({{"run", {{"type", "string"}, {"description", "Run id (simulation_list)."}}},
                         {"fields",
                          {{"type", "array"},
                           {"items",
                            {{"type", "string"},
                             {"enum", {"surface", "streamlines", "sectionHorizontal", "sectionVertical", "sectionCross",
                                       "wake", "vortices"}}}},
                           {"description", "What to draw (default surface and streamlines): surface = Cp on the "
                                           "part; sections and streamlines = flow speed."}}}},
                        Json::array({"run"}))},
          [](CommandContext&, const Json&) -> Json { throw CommandError(kNeedsWindow); });
}

// --- How to do things: workflow recipes for an agent -----------------------------------

const char* const kGuideTopics[][2] = {
    {"basics",
     "1. project_get_context: parts (id, name, bounds), units (mm, Z up), undo state. timeline_list: features "
     "(sketches, extrudes, fits...) with their ids and status. Part ids and feature ids are different numbers: "
     "tools that take `sketch`, `feature` or `body` want a feature id from timeline_list; tools that take `id` or "
     "`part` want a part id from project_get_context.\n"
     "2. view_screenshot (direction: iso, part: <id>) to see the model. Look again after every change that "
     "matters; tell the user what you see, not only what the tool returned.\n"
     "3. Make changes. Every document change is one undo step (edit_undo). Check results with geometry_measure, "
     "timeline_list (status ok/error and report.warnings) and a screenshot.\n"
     "4. project_save {path} only when the user asks or at the end of a task they asked to keep."},
    {"scan",
     "Imported scan (STL/OBJ/PLY/3MF/STEP): document_import {path, units?}. Then: mesh_stats {id} (holes, "
     "non-manifold edges) -> mesh_repair {id} if needed. Wrong size: part_convert_units {id, from: 'm'|'cm'|'in'} "
     "or part_calibrate {id, a, b, distance} (two points a known distance apart). Wrong orientation: part_align or part_transform. Too heavy: "
     "mesh_decimate {id, targetTriangles}. Check: geometry_measure {kind: 'part', id}."},
    {"sketch_on_surface",
     "A part that follows a curved scan (aileron, panel, cover):\n"
     "1. Find a point on the scan: view_screenshot {part: <scan id>, direction: ...}, then view_pick {points: "
     "[[x, y]]} -> point and normal. (Without the window: geometry_measure {kind: 'point_to_part', id, point} "
     "gives the closest point and normal.)\n"
     "2. sketch_create {surface: {part, point, radius}, curves: [...]} -> a sketch on the best-fit plane over that "
     "area; sketch coordinates are mm in that plane, origin near `point`. Check with sketch_profiles {sketch} "
     "that the outline is closed (profiles not empty).\n"
     "3. surface_part_from_sketch {sketch, thickness} (id defaults to the sketch's surface part) makes the solid part that follows the "
     "scan (one step: fit, trim, thicken). Or step by step: surface_fit_selection / surface_fit_regions, "
     "surface_trim_by_sketch, surface_thicken.\n"
     "4. timeline_list: the new feature has status ok; read report.warnings (approximate fit, thin walls). "
     "view_screenshot {part: <new part>} to show it."},
    {"fin",
     "A fin standing out of a curved surface (canard / dive plane on a bumper, winglet):\n"
     "1. Point on the surface where the root goes: view_screenshot + view_pick, or geometry_measure "
     "point_to_part.\n"
     "2. sketch_create {surface: {part, point, across: true}, curves: [...]}. The plane stands on the surface: "
     "sketch x is along the surface normal (x > 0 outside), y along the surface. Draw a CLOSED outline (e.g. a "
     "spline with closed: true, or lines that end where they started) whose root reaches into the surface "
     "(x < 0, e.g. x = -10).\n"
     "3. sketch_profiles {sketch, surfaceLine: true}: profiles must not be empty; surfaceCurve is where the "
     "surface is in sketch coordinates; the root must cross it.\n"
     "4. surface_fin_from_sketch {id: <surface part id>, sketch, thickness, rootFillet?, count?, mirror?}.\n"
     "5. timeline_list for status and warnings; view_screenshot {part: <fin part id>} to check that the fin sits "
     "on the surface."},
    {"solid",
     "Parametric solid: sketch_create {plane: 'XY'|'XZ'|'YZ'|{origin, normal, xDir}|face: {body, point}, curves, "
     "constraints} -> sketch_profiles -> feature_extrude {sketch, distance, profiles?, operation} / "
     "feature_revolve. Then feature_fillet, feature_chamfer, feature_shell, feature_hole, patterns, "
     "feature_combine. Edges and faces by index: body_edges {body} / body_faces {body}. Change dimensions with "
     "feature_edit or user parameters (parameter_set). Bodies are listed in timeline_list.bodies."},
    {"cfd",
     "Wind tunnel (OpenFOAM):\n"
     "1. simulation_environment: is an engine ready? If not, tell the user (Docker must run); "
     "simulation_install_engine downloads the image only when the user agrees.\n"
     "2. Check the part first: geometry_measure {kind: 'part', id} (closed? size in mm plausible? a model in "
     "metres imported as mm is 1000x too small and the solve diverges). simulation_propose_run {id, speed, "
     "flowDirection, ground, mesh, ...} resolves the settings, reference area, domain and engine without "
     "starting anything: show them to the user.\n"
     "3. simulation_run {id, speed, ..., label} (the same settings) returns at once with a run id; poll simulation_status {run} "
     "every 30-60 s (state running/finished/failed, progress, remainingSeconds). Do not start several runs at "
     "once unless asked.\n"
     "4. simulation_results {run}: forces, Cd/Cl (mean over the last 20 %), quality checks. simulation_field "
     "for values on the surface or a section. view_show_results {run} draws Cp and streamlines in the 3D view; "
     "then view_screenshot to see where the pressure is high.\n"
     "5. A/B: variant_create / variant_activate {variant} (or two parts), run both with the same settings, "
     "simulation_compare {a, b}; read its verdict (comparable / not comparable) before drawing conclusions."},
    {"variants",
     "A/B design variants: variant_create {name} copies the active variant's timeline and parameters; "
     "variant_activate {variant} switches; feature_* / parameter_* / sketch_* edits apply to the active one; imported "
     "parts are shared. variant_compare lists the differences; variant_toggle switches between the last two."},
    {"export",
     "document_export_check {ids?, format?, print?} first (open edges, thin walls, size); it returns a state "
     "to pass as expectedState. document_export {ids?, path, format?} writes STL/3MF/STEP (ids default: all "
     "visible bodies and imported parts); document_export_for_print {ids?, folder} writes print-ready STL and "
     "STEP of the visible solids. Paths are absolute on the user's computer; ask the user where to save if "
     "they did not say."},
};

void registerGuide(CommandRegistry& r)
{
    Json topics = Json::array();
    for (const auto& t : kGuideTopics)
        topics.push_back(t[0]);
    r.add({"agent.guide", "How to use AeroCAD",
           "Step-by-step recipes for common tasks with these tools, in the order a user would do them in the app. "
           "Read 'basics' first, then the topic you need: scan (import and clean a scan), sketch_on_surface "
           "(a part following a curved scan), fin (canard/winglet on a surface), solid (parametric sketch and "
           "features), cfd (wind tunnel run and results), variants (A/B), export. Without topic: all of them.",
           objectSchema({{"topic", {{"type", "string"}, {"enum", topics}, {"description", "Which recipe."}}}})},
          [](CommandContext&, const Json& params) {
              const std::string topic = params.value("topic", std::string());
              Json out = Json::object();
              for (const auto& t : kGuideTopics)
                  if (topic.empty() || topic == t[0])
                      out[t[0]] = t[1];
              return Json{{"guide", out}};
          });
}

} // namespace

void registerAgentCommands(CommandRegistry& registry)
{
    registerProjectContext(registry);
    registerMeasure(registry);
    registerViewCommands(registry);
    registerShowResults(registry);
    registerGuide(registry);
}

} // namespace aerocad::commands
