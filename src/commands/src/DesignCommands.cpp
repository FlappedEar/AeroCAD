// SPDX-License-Identifier: GPL-3.0-or-later
// CAD commands: sketches, features and the parametric timeline. Each command edits the
// cad::Design, which regenerates and is mirrored into the document as parts. Undo
// restores the previous timeline state.
#include "aerocad/commands/Session.hpp"

#include "aerocad/core/Align.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

// --- Schema helpers ---------------------------------------------------------------------

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

Json numberOrExpr(const std::string& description)
{
    return Json{{"type", Json::array({"number", "string"})},
                {"description", description + " Number or expression with user parameters, e.g. \"width / 2\"."}};
}

Json point2(const std::string& description)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"number", "string"})}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", description + " [u, v] in sketch coordinates (mm)."}};
}

Json point3(const std::string& description)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"number", "string"})}}},
                {"minItems", 3},
                {"maxItems", 3},
                {"description", description + " [x, y, z] in mm."}};
}

Json featureIdSchema(const std::string& description)
{
    return Json{{"type", "integer"}, {"minimum", 1}, {"description", description}};
}

Json bodyRefSchema(const std::string& description)
{
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Body id (the id of the feature that created it, see "
                                              "timeline.list / document.parts featureId) or body name."}};
}

Json curveSchema()
{
    return Json{
        {"type", "object"},
        {"description",
         "A sketch curve. type selects the fields: line {start, end}; rectangle {corner, width, height} or "
         "{corner, opposite}; center_rectangle {center, width, height}; circle {center, radius | diameter}; "
         "arc {start, through, end} (three-point arc); center_arc {center, radius, startAngle, endAngle} "
         "(degrees, counter-clockwise); polygon {center, radius, sides, angle?}; slot {start, end, width} "
         "(centre-to-centre); spline {points, closed?}. construction: true excludes it from profiles."},
        {"properties",
         {{"type",
           {{"type", "string"},
            {"enum", {"line", "rectangle", "center_rectangle", "circle", "arc", "center_arc", "polygon", "slot",
                      "spline"}}}},
          {"start", point2("Start point.")},
          {"end", point2("End point.")},
          {"through", point2("Point on the arc.")},
          {"corner", point2("Rectangle corner.")},
          {"opposite", point2("Opposite rectangle corner.")},
          {"center", point2("Centre.")},
          {"width", numberOrExpr("Width (mm).")},
          {"height", numberOrExpr("Height (mm).")},
          {"radius", numberOrExpr("Radius (mm).")},
          {"diameter", numberOrExpr("Diameter (mm).")},
          {"startAngle", numberOrExpr("Start angle (degrees).")},
          {"endAngle", numberOrExpr("End angle (degrees).")},
          {"sides", numberOrExpr("Number of polygon sides.")},
          {"angle", numberOrExpr("Polygon rotation (degrees).")},
          {"points", {{"type", "array"}, {"items", point2("Fit point.")}, {"minItems", 2}}},
          {"closed", {{"type", "boolean"}}},
          {"construction", {{"type", "boolean"}}}}},
        {"required", {"type"}},
        {"additionalProperties", false}};
}

Json constraintPointSchema()
{
    return Json{{"type", Json::array({"string", "object"})},
                {"description", "\"origin\" (sketch origin) or {curve: index, point: start | end | center}."},
                {"properties", {{"curve", {{"type", "integer"}, {"minimum", 0}}},
                                {"point", {{"type", "string"}, {"enum", {"start", "end", "center"}}}}}}};
}

Json constraintSchema()
{
    return Json{
        {"type", "object"},
        {"description",
         "A sketch constraint or dimension. Curves are referred to by index in sketch.curves (a rectangle is four "
         "lines, a slot two lines and two arcs). Geometric: coincident {points: 2}; point_on_curve {point, curve}; "
         "midpoint {point, curve: line}; horizontal / vertical {curve: line} or {points: 2}; parallel, "
         "perpendicular, collinear {curves: 2 lines}; tangent {curves: line/arc/circle + arc/circle}; equal "
         "{curves: 2 lines or 2 arcs/circles}; concentric {curves: 2}; symmetric {points: 2, curve: line}; fix "
         "{points and/or curves}. Dimensions (value: number or expression): distance {points: 2} or {point, "
         "curve: line}; horizontal_distance / vertical_distance {points: 2}; length {curve: line}; radius / "
         "diameter {curve}; angle {curves: 2 lines} (degrees)."},
        {"properties",
         {{"type",
           {{"type", "string"},
            {"enum", {"coincident", "point_on_curve", "midpoint", "horizontal", "vertical", "parallel",
                      "perpendicular", "collinear", "tangent", "equal", "concentric", "symmetric", "fix",
                      "distance", "horizontal_distance", "vertical_distance", "length", "radius", "diameter",
                      "angle"}}}},
          {"curve", {{"type", "integer"}, {"minimum", 0}}},
          {"curves", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}}}},
          {"point", constraintPointSchema()},
          {"points", {{"type", "array"}, {"items", constraintPointSchema()}}},
          {"value", numberOrExpr("Dimension value (mm, or degrees for angle).")},
          {"at", {{"type", "array"},
                  {"items", point2("Position.")},
                  {"description", "fix: positions to hold the points at. sketch.add_constraints fills it with the "
                                  "current positions."}}}}},
        {"required", {"type"}},
        {"additionalProperties", false}};
}

Json constraintsSchema()
{
    return Json{{"type", "array"}, {"items", constraintSchema()},
                {"description", "Sketch constraints and dimensions (solved after the curves)."}};
}

Json curvesSchema()
{
    return Json{{"type", "array"}, {"items", curveSchema()}, {"description", "Sketch curves."}};
}

Json planeSchema()
{
    return Json{{"type", Json::array({"string", "object"})},
                {"description",
                 "Sketch plane: \"XY\" (top, normal +Z), \"XZ\" (front, normal -Y), \"YZ\" (right, normal +X), "
                 "the name of a reference frame (frame.list), or {origin: [x,y,z], normal: [x,y,z], "
                 "xDir: [x,y,z]}. Z is up."}};
}

Json faceRefSchema()
{
    return Json{{"type", "object"},
                {"description", "Planar face of a body to sketch on: {body, index} or {body, point: [x,y,z]} "
                                "(the face nearest to the point; robust when earlier features change)."},
                {"properties", {{"body", bodyRefSchema("Body.")}, {"index", {{"type", "integer"}, {"minimum", 0}}},
                                {"point", point3("Point on the face.")}}},
                {"required", {"body"}},
                {"additionalProperties", false}};
}

Json operationSchema(bool withNewBody = true)
{
    Json values = withNewBody ? Json::array({"new_body", "join", "cut", "intersect"})
                              : Json::array({"join", "cut", "intersect"});
    return Json{{"type", "string"},
                {"enum", values},
                {"description", "What to do with the result (Fusion's Operation): new_body (default), join, cut or "
                                "intersect. join/cut/intersect act on `bodies`, or on all bodies the result "
                                "touches."}};
}

Json bodiesSchema()
{
    return Json{{"type", "array"},
                {"items", bodyRefSchema("Body.")},
                {"description", "Target bodies for join/cut/intersect. Default: every body the result touches."}};
}

Json topologyListSchema(const std::string& what)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"integer", "object"})}}},
                {"minItems", 1},
                {"description", what + " to use: indices from body." + what + "s, or {point: [x,y,z]} to take the " +
                                    what + " nearest to a point. Point references are stored with the point relative "
                                    "to the body's bounding box and the " + what + "'s direction and type, so they "
                                    "follow the body when earlier dimensions change."}};
}

Json profileSelection()
{
    return Json{{"sketch", featureIdSchema("Sketch feature id.")},
                {"profiles", {{"type", "array"},
                              {"items", {{"type", "integer"}, {"minimum", 0}}},
                              {"description", "Profile indices (see sketch.profiles). Default: all profiles."}}},
                {"points", {{"type", "array"},
                            {"items", point2("Point inside the profile.")},
                            {"description", "Select profiles by a point inside them (alternative to profiles)."}}}};
}

Json merged(Json a, const Json& b)
{
    for (const auto& [k, v] : b.items())
        a[k] = v;
    return a;
}

// --- Result helpers ---------------------------------------------------------------------

cad::Design& design(CommandContext& ctx)
{
    if (ctx.design == nullptr)
        throw CommandError("No design in this session");
    return *ctx.design;
}

model::PartId partOf(const model::Document& doc, cad::FeatureId id, model::PartKind kind)
{
    for (const auto& p : doc.parts()) {
        if (p.kind == kind && p.featureId == id)
            return p.id;
    }
    return 0;
}

Json vec(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

Json featureToJson(const cad::Design& d, const cad::Feature& f, std::size_t index)
{
    const auto st = d.status(f.id);
    Json j{{"id", f.id},
           {"index", index},
           {"type", f.type},
           {"name", f.name},
           {"params", f.params},
           {"suppressed", f.suppressed},
           {"status", std::string(cad::toString(st.state))}};
    if (!st.message.empty())
        j["error"] = st.message;
    return j;
}

Json bodiesToJson(const cad::Design& d, const model::Document& doc)
{
    Json out = Json::array();
    for (const auto& [id, b] : d.bodies()) {
        const auto bb = b.shape.bounds();
        out.push_back({{"id", id},
                       {"name", b.name},
                       {"partId", partOf(doc, id, model::PartKind::Body)},
                       {"volume", b.shape.volume()},
                       {"faces", b.shape.faceCount()},
                       {"bounds", {{"min", vec(bb.min)}, {"max", vec(bb.max)}}}});
    }
    return out;
}

Json timelineToJson(const cad::Design& d, const model::Document& doc)
{
    Json features = Json::array();
    for (std::size_t i = 0; i < d.features().size(); ++i)
        features.push_back(featureToJson(d, d.features()[i], i));
    Json params = Json::array();
    for (const auto& p : d.parameters()) {
        Json pj{{"name", p.name}, {"expression", p.expression}, {"comment", p.comment}};
        try {
            pj["value"] = *d.parameterValue(p.name);
        } catch (const std::exception& e) {
            pj["error"] = e.what();
        }
        params.push_back(pj);
    }
    return Json{{"features", features},
                {"marker", d.marker()},
                {"parameters", params},
                {"bodies", bodiesToJson(d, doc)}};
}

/// Runs a design change as one undoable step and mirrors the result into the document.
template <typename Fn>
Json designChange(CommandContext& ctx, const std::string& label, Fn&& fn)
{
    cad::Design& d = design(ctx);
    const cad::Design::State before = d.state();
    Json result;
    try {
        result = fn(d);
    } catch (const cad::DesignError& e) {
        throw CommandError(e.what());
    }
    const cad::Design::State after = d.state();
    model::Document& doc = ctx.document;
    d.syncDocument(doc);
    cad::Design* dp = &d;
    ctx.undoStack.push({label,
                        [dp, &doc, before]() {
                            dp->restore(before);
                            dp->syncDocument(doc);
                        },
                        [dp, &doc, after]() {
                            dp->restore(after);
                            dp->syncDocument(doc);
                        }});
    return result;
}

/// Result of commands that add a feature.
Json addedFeature(cad::Design& d, const model::Document& doc, cad::FeatureId id)
{
    const cad::Feature* f = d.feature(id);
    std::size_t index = 0;
    for (; index < d.features().size(); ++index)
        if (d.features()[index].id == id)
            break;
    Json j = featureToJson(d, *f, index);
    j["bodies"] = bodiesToJson(d, doc);
    if (auto it = d.sketches().find(id); it != d.sketches().end()) {
        j["profiles"] = it->second.geometry.profiles.size();
        j["degreesOfFreedom"] = it->second.degreesOfFreedom;
    }
    return j;
}

Json surfaceRefSchema()
{
    return Json{
        {"type", "object"},
        {"description",
         "Sketch over a curved part (a fitted surface or a scanned mesh): the sketch plane is the best-fit plane "
         "of {part}'s region selection when it has one, else of the area within `radius` of `point` (a scan), "
         "else of the whole part (a fitted surface). The plane's normal points the way the surface faces, its "
         "X axis along the long side of the area, and it sits just above the area so the whole outline is "
         "visible. Project the outline back onto the curved surface with surface.trim_by_sketch. The resolved "
         "plane is stored, so the sketch does not move when the part changes."},
        {"properties",
         {{"part", {{"type", "integer"}, {"minimum", 1}, {"description", "Part id (document.parts)."}}},
          {"point", point3("Clicked point on the part.")},
          {"radius",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"description", "Size of the area around `point` (mm). Default: a tenth of the part's size."}}},
          {"useSelection", {{"type", "boolean"}, {"description", "Use the region selection (default when set)."}}},
          {"across",
           {{"type", "boolean"},
            {"description",
             "Sketch across the surface instead of over it, for a fin sticking out of it (a canard on a bumper, a "
             "winglet): the plane stands on the surface, through a point of the area, its X axis along the "
             "surface normal (outward), so x > 0 is outside. Draw the fin so its root reaches into the surface "
             "(x < 0); surface.fin_from_sketch cuts the root to the surface."}}},
          {"angle",
           {{"type", "number"},
            {"description", "With across: turns the fin about the surface normal, degrees from level (0: the fin "
                            "is horizontal where the surface allows)."}}}}},
        {"required", {"part"}},
        {"additionalProperties", false}};
}

/// The sketch plane over (an area of) a curved part, see surfaceRefSchema. Returns the
/// plane object for the sketch params plus a short report.
std::pair<Json, Json> surfaceSketchPlane(const model::Document& doc, const Json& ref)
{
    const auto partId = ref.at("part").get<model::PartId>();
    const model::Part* part = doc.part(partId);
    if (part == nullptr)
        throw CommandError("No part with id " + std::to_string(partId));
    if (part->kind == model::PartKind::Sketch)
        throw CommandError("Part " + std::to_string(partId) + " is a sketch");
    if (!part->mesh || part->mesh->triangleCount() == 0)
        throw CommandError("Part " + std::to_string(partId) + " has no surface");
    const core::TriangleMesh& mesh = *part->mesh;

    std::vector<std::size_t> triangles;
    std::string area;
    const bool hasSelection = part->selection && !part->selection->empty();
    if (ref.value("useSelection", hasSelection) && hasSelection) {
        triangles.assign(part->selection->begin(), part->selection->end());
        area = "selection";
    } else if (ref.contains("point") && (!part->shape || ref.value("across", false))) {
        // A scan, or a fin's place: only the area around the click. A fitted surface is
        // otherwise used whole.
        const auto& pt = ref.at("point");
        const core::Vec3 point{pt.at(0).get<double>(), pt.at(1).get<double>(), pt.at(2).get<double>()};
        const double radius = ref.value("radius", std::max(mesh.bounds().diagonal() * 0.1, 1e-3));
        const core::MeshAdjacency adjacency(mesh);
        for (const auto t : core::selectBrushDab(mesh, &adjacency, point, radius))
            triangles.push_back(t);
        area = "area around the point";
    } else {
        triangles.resize(mesh.triangleCount());
        for (std::size_t i = 0; i < triangles.size(); ++i)
            triangles[i] = i;
        area = "whole part";
    }
    const auto points = core::triangleVertices(mesh, triangles);
    const auto fit = core::fitPlaneToTriangles(mesh, triangles);
    if (!fit)
        throw CommandError("The " + area + " of part " + std::to_string(partId) +
                           " is too small or flat in one direction to fit a sketch plane");

    // Orient the normal the way the triangles face (area-weighted average normal).
    core::Vec3 facing;
    for (const std::size_t t : triangles) {
        const auto& tri = mesh.triangles()[t];
        const auto& v = mesh.vertices();
        facing = facing + (v[tri[1]] - v[tri[0]]).cross(v[tri[2]] - v[tri[0]]);
    }
    core::Vec3 normal = fit->normal;
    if (facing.dot(normal) < 0.0)
        normal = normal * -1.0;
    if (ref.value("across", false)) {
        // Fin plane: stands on the surface. "Up along the surface" is world Z projected
        // into the tangent plane (the long side when the surface faces up or down); the fin
        // is level, turned by `angle` about the surface normal.
        core::Vec3 up = core::Vec3{0, 0, 1} - normal * normal.z;
        if (up.length() < 0.3) {
            // Facing up or down: along the long side of the area, or world X for a
            // roundish area (no clear long side), else world Y.
            core::Vec3 along = fit->elongation >= 0.2 ? fit->major : core::Vec3{1, 0, 0};
            if (std::abs(along.dot(normal)) > 0.9)
                along = {0, 1, 0};
            up = along - normal * along.dot(normal);
        }
        up = up.normalized();
        const double a = ref.value("angle", 0.0) * 3.14159265358979323846 / 180.0;
        const core::Vec3 finNormal = (up * std::cos(a) + normal.cross(up) * std::sin(a)).normalized();
        // Through the clicked point (on the surface), else the point of the area nearest
        // its centroid, so x = 0 is on the surface.
        core::Vec3 on = fit->origin;
        if (ref.contains("point")) {
            const auto& pt = ref.at("point");
            on = {pt.at(0).get<double>(), pt.at(1).get<double>(), pt.at(2).get<double>()};
        } else {
            double best = std::numeric_limits<double>::max();
            for (const auto& p : points) {
                const double d = (p - fit->origin).length();
                if (d < best) {
                    best = d;
                    on = p;
                }
            }
        }
        Json plane{{"origin", Json::array({on.x, on.y, on.z})},
                   {"normal", Json::array({finNormal.x, finNormal.y, finNormal.z})},
                   {"xDir", Json::array({normal.x, normal.y, normal.z})}};
        Json report{{"part", partId}, {"area", area}, {"triangles", triangles.size()}, {"rms", fit->rms},
                    {"across", true}};
        return {plane, report};
    }
    // Lift the plane to the highest point of the area, so no part of the outline hides
    // under the surface.
    double top = 0.0;
    for (const auto& p : points)
        top = std::max(top, (p - fit->origin).dot(normal));
    const core::Vec3 origin = fit->origin + normal * top;
    // X along the long side; for a roundish area (no clear long side) along world X, or
    // world Y when X is nearly along the normal.
    core::Vec3 xHint = fit->major;
    if (fit->elongation < 0.2)
        xHint = std::abs(normal.x) < 0.9 ? core::Vec3{1, 0, 0} : core::Vec3{0, 1, 0};
    core::Vec3 xDir = xHint - normal * xHint.dot(normal);
    if (xDir.length() < 1e-9)
        xDir = std::abs(normal.z) < 0.9 ? core::Vec3{0, 0, 1}.cross(normal) : core::Vec3{1, 0, 0};
    xDir = xDir.normalized();

    Json plane{{"origin", Json::array({origin.x, origin.y, origin.z})},
               {"normal", Json::array({normal.x, normal.y, normal.z})},
               {"xDir", Json::array({xDir.x, xDir.y, xDir.z})}};
    Json report{{"part", partId},
                {"area", area},
                {"triangles", triangles.size()},
                {"rms", fit->rms},
                {"height", top}};
    return {plane, report};
}

/// Copies the command params into feature params, without the command-only keys.
Json featureParams(const Json& params)
{
    Json p = params;
    p.erase("name");
    return p;
}

/// Adds matching hints to {point} edge/face references of fillet, chamfer and shell: the
/// point relative to the body's bounding box, the edge direction or face normal, and the
/// edge/face type. Regeneration then finds the same edge after dimensions change (see
/// matchTopology in Design.cpp). References that already carry hints are left alone.
Json describeTopologyRefs(const cad::Design& d, const std::string& type, Json params)
{
    const bool edgesWanted = type == "fillet" || type == "chamfer";
    if (!edgesWanted && type != "shell")
        return params;
    const char* key = edgesWanted ? "edges" : "faces";
    if (!params.contains(key) || !params[key].is_array() || !params.contains("body"))
        return params;
    const cad::Body* body = nullptr;
    for (const auto& [bid, b] : d.bodies()) {
        const Json& ref = params["body"];
        if ((ref.is_number_integer() && ref.get<cad::FeatureId>() == bid) ||
            (ref.is_string() && ref.get<std::string>() == b.name))
            body = &b;
    }
    if (body == nullptr)
        return params;
    const auto box = body->shape.bounds();
    const core::Vec3 size = box.size();
    auto rel = [&](double v, double lo, double extent) { return extent > 1e-9 ? (v - lo) / extent : 0.0; };
    for (auto& item : params[key]) {
        if (!item.is_object() || !item.contains("point") || item.contains("relative") || !item["point"].is_array())
            continue;
        bool numeric = true;
        for (const auto& c : item["point"])
            numeric = numeric && c.is_number();
        if (!numeric || item["point"].size() != 3)
            continue;
        const core::Vec3 p{item["point"][0].get<double>(), item["point"][1].get<double>(),
                           item["point"][2].get<double>()};
        item["relative"] = Json::array({rel(p.x, box.min.x, size.x), rel(p.y, box.min.y, size.y),
                                        rel(p.z, box.min.z, size.z)});
        if (edgesWanted) {
            const auto idx = geometry::nearestEdge(body->shape, p);
            if (!idx)
                continue;
            const auto info = geometry::edges(body->shape).at(*idx);
            item["kind"] = info.type;
            if (info.type == "line")
                item["direction"] = vec((info.end - info.start).normalized());
        } else {
            const auto idx = geometry::nearestFace(body->shape, p);
            if (!idx)
                continue;
            const auto info = geometry::faces(body->shape).at(*idx);
            item["kind"] = info.type;
            item["normal"] = vec(info.normal);
        }
    }
    return params;
}

void addFeatureCommand(CommandRegistry& r, const std::string& type, const std::string& title,
                       const std::string& description, Json properties, Json required)
{
    properties["name"] = {{"type", "string"}, {"description", "Optional feature name for the timeline."}};
    r.add({"feature." + type, title, description, objectSchema(std::move(properties), std::move(required)), true},
          [type, title](CommandContext& ctx, const Json& params) {
              cad::FeatureId id = 0;
              designChange(ctx, title, [&](cad::Design& d) {
                  id = d.addFeature(type, describeTopologyRefs(d, type, featureParams(params)),
                                    params.value("name", std::string()));
                  return Json{};
              });
              // The document is synced only after designChange; build the result now.
              return addedFeature(design(ctx), ctx.document, id);
          });
}

const cad::Body& requireBody(const cad::Design& d, const Json& ref)
{
    if (ref.is_number_integer()) {
        if (auto it = d.bodies().find(ref.get<cad::FeatureId>()); it != d.bodies().end())
            return it->second;
    } else if (ref.is_string()) {
        for (const auto& [_, b] : d.bodies())
            if (b.name == ref.get<std::string>())
                return b;
    }
    throw CommandError("No body " + ref.dump());
}

const cad::Feature& sketchFeature(const cad::Design& d, cad::FeatureId id)
{
    const cad::Feature* f = d.feature(id);
    if (f == nullptr || f->type != "sketch")
        throw cad::DesignError("No sketch with id " + std::to_string(id));
    return *f;
}

/// Centre of a three-point arc (circumcentre), std::nullopt for degenerate arcs.
std::optional<geometry::Vec2> arcCenter(const geometry::SketchCurve& c)
{
    if (c.kind != geometry::SketchCurve::Kind::Arc || c.points.size() != 3)
        return std::nullopt;
    const auto& a = c.points[0];
    const auto& b = c.points[1];
    const auto& e = c.points[2];
    const double den = 2 * (a.x * (b.y - e.y) + b.x * (e.y - a.y) + e.x * (a.y - b.y));
    if (std::abs(den) < 1e-12)
        return std::nullopt;
    const double aa = a.x * a.x + a.y * a.y;
    const double bb = b.x * b.x + b.y * b.y;
    const double ee = e.x * e.x + e.y * e.y;
    return geometry::Vec2{(aa * (b.y - e.y) + bb * (e.y - a.y) + ee * (a.y - b.y)) / den,
                          (aa * (e.x - b.x) + bb * (a.x - e.x) + ee * (b.x - a.x)) / den};
}

/// Position of a constraint point reference ("origin" or {curve, point}) in solved curves.
geometry::Vec2 pointPosition(const std::vector<geometry::SketchCurve>& curves, const Json& ref)
{
    if (ref.is_string())
        return {0, 0};
    const auto i = ref.at("curve").get<std::size_t>();
    if (i >= curves.size())
        throw CommandError("No curve " + std::to_string(i) + " in the sketch");
    const auto& c = curves[i];
    const std::string which = ref.value("point", std::string("start"));
    using K = geometry::SketchCurve::Kind;
    if (which == "center") {
        if (c.kind == K::Circle)
            return c.points[0];
        if (const auto ctr = arcCenter(c))
            return *ctr;
    } else if (c.kind != K::Circle && !c.points.empty()) {
        return which == "end" ? c.points.back() : c.points.front();
    }
    throw CommandError("Curve " + std::to_string(i) + " has no point '" + which + "'");
}

/// A new fix constraint holds the geometry where it is now (after earlier dimensions),
/// not where it was drawn: curves are expanded to their points and the solved positions
/// are stored in "at". A fixed circle also keeps its radius.
Json resolveFix(const std::vector<geometry::SketchCurve>& curves, const Json& c)
{
    Json out = Json::array();
    if (c.value("type", std::string()) != "fix" || c.contains("at")) {
        out.push_back(c);
        return out;
    }
    Json points = c.value("points", Json::array());
    if (c.contains("point"))
        points.push_back(c.at("point"));
    std::vector<std::size_t> ids;
    if (c.contains("curve"))
        ids.push_back(c.at("curve").get<std::size_t>());
    for (const auto& i : c.value("curves", Json::array()))
        ids.push_back(i.get<std::size_t>());
    using K = geometry::SketchCurve::Kind;
    for (const std::size_t i : ids) {
        if (i >= curves.size())
            throw CommandError("No curve " + std::to_string(i) + " in the sketch");
        const auto& k = curves[i];
        if (k.kind == K::Circle) {
            points.push_back({{"curve", i}, {"point", "center"}});
            out.push_back({{"type", "radius"}, {"curve", i}, {"value", k.radius}});
        } else {
            points.push_back({{"curve", i}, {"point", "start"}});
            points.push_back({{"curve", i}, {"point", "end"}});
            if (k.kind == K::Arc)
                points.push_back({{"curve", i}, {"point", "center"}});
        }
    }
    Json at = Json::array();
    for (const auto& ref : points) {
        const auto pos = pointPosition(curves, ref);
        at.push_back(Json::array({pos.x, pos.y}));
    }
    out.insert(out.begin(), Json{{"type", "fix"}, {"points", points}, {"at", at}});
    return out;
}

// --- Registration -----------------------------------------------------------------------

void registerSketchCommands(CommandRegistry& r)
{
    r.add({"sketch.create", "Create Sketch",
           "Creates a 2D sketch on a standard plane, an arbitrary plane, a planar face of a body or over a curved "
           "surface or scan (surface: best-fit plane), with curves "
           "(lines, rectangles, circles, arcs, polygons, slots, splines). Closed regions become profiles for "
           "extrude/revolve. Dimensions can be expressions using user parameters (parameter.set), so editing a "
           "parameter updates the model.",
           objectSchema({{"plane", planeSchema()},
                         {"face", faceRefSchema()},
                         {"surface", surfaceRefSchema()},
                         {"offset", numberOrExpr("Offset of the plane along its normal (mm).")},
                         {"curves", curvesSchema()},
                         {"constraints", constraintsSchema()},
                         {"name", {{"type", "string"}, {"description", "Optional name, default SketchN."}}}}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const int refs = static_cast<int>(params.contains("plane")) + static_cast<int>(params.contains("face")) +
                               static_cast<int>(params.contains("surface"));
              if (refs > 1)
                  throw CommandError("Give one of plane, face or surface");
              Json feature = featureParams(params);
              Json surfaceReport;
              if (params.contains("surface")) {
                  auto [plane, report] = surfaceSketchPlane(ctx.document, params.at("surface"));
                  feature.erase("surface");
                  feature["plane"] = std::move(plane);
                  // Remembered for "part from sketch" (surface.part_from_sketch), which cuts
                  // the part out of this surface or scan; sketch evaluation ignores it.
                  Json on{{"part", params.at("surface").at("part")}};
                  if (params.at("surface").contains("point"))
                      on["point"] = params.at("surface").at("point");
                  if (params.at("surface").value("across", false))
                      on["across"] = true;
                  feature["onSurface"] = std::move(on);
                  surfaceReport = std::move(report);
              }
              cad::FeatureId id = 0;
              designChange(ctx, "Create Sketch", [&](cad::Design& d) {
                  id = d.addFeature("sketch", feature, params.value("name", std::string()));
                  return Json{};
              });
              Json result = addedFeature(design(ctx), ctx.document, id);
              if (!surfaceReport.is_null())
                  result["surface"] = surfaceReport;
              return result;
          });

    r.add({"sketch.add_curves", "Add Sketch Curves", "Appends curves to an existing sketch.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")}, {"curves", curvesSchema()}},
                        {"sketch", "curves"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Edit Sketch", [&](cad::Design& d) {
                  const cad::Feature* f = d.feature(id);
                  if (f == nullptr || f->type != "sketch")
                      throw cad::DesignError("No sketch with id " + std::to_string(id));
                  Json curves = f->params.value("curves", Json::array());
                  for (const auto& c : params.at("curves"))
                      curves.push_back(c);
                  d.editFeature(id, {{"curves", curves}});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.profiles", "Sketch profiles",
           "Lists the closed profiles of a sketch with their area and centroid (sketch and world coordinates), "
           "to pick profiles for extrude/revolve.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"surfaceLine",
                          {{"type", "boolean"},
                           {"description", "For a sketch across a surface (a fin): also return where the sketch "
                                           "plane cuts the surface, as segments in sketch coordinates."}}}},
                        {"sketch"})},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto& sketches = design(ctx).sketches();
              auto it = sketches.find(id);
              if (it == sketches.end())
                  throw CommandError("No evaluated sketch with id " + std::to_string(id));
              const auto& sk = it->second;
              Json out = Json::array();
              for (std::size_t i = 0; i < sk.geometry.profiles.size(); ++i) {
                  const auto c = geometry::centroid(sk.geometry.profiles[i]);
                  const auto uv = sk.plane.toLocal(c);
                  out.push_back({{"index", i},
                                 {"area", geometry::area(sk.geometry.profiles[i])},
                                 {"centroid", Json::array({uv.x, uv.y})},
                                 {"centroidWorld", vec(c)}});
              }
              Json onSurface;
              if (const cad::Feature* f = design(ctx).feature(id); f != nullptr && f->params.contains("onSurface"))
                  onSurface = f->params.at("onSurface");
              // Across a surface (a fin): where the sketch plane cuts it, in sketch
              // coordinates, so the root can be drawn into it. Seen from the sketch normal
              // the surface itself hides that line.
              Json surfaceLine = Json::array();
              if (params.value("surfaceLine", false) && onSurface.is_object() && onSurface.value("across", false) &&
                  onSurface.contains("part")) {
                  const model::Part* surface = ctx.document.part(onSurface.at("part").get<model::PartId>());
                  if (surface != nullptr) {
                      if (const model::Part* scan = ctx.document.part(model::sourcePartId(*surface));
                          scan != nullptr && scan->mesh && !scan->shape)
                          surface = scan;
                  }
                  if (surface != nullptr && surface->mesh) {
                      core::BoundingBox box;
                      for (const auto& p : geometry::edgeSegments(sk.geometry.curves))
                          box.extend(p);
                      const double reach = std::max(box.isEmpty() ? 0.0 : 3.0 * box.diagonal(),
                                                    0.3 * surface->mesh->bounds().diagonal());
                      auto cut = core::sliceMesh(*surface->mesh, sk.plane.origin, sk.plane.normal, reach);
                      // Drawn on every repaint: keep the pieces nearest the sketch origin.
                      constexpr std::size_t maxSegments = 4000;
                      if (cut.size() / 2 > maxSegments) {
                          std::vector<std::pair<double, std::size_t>> byDistance;
                          for (std::size_t i = 0; i + 1 < cut.size(); i += 2)
                              byDistance.push_back({((cut[i] + cut[i + 1]) * 0.5 - sk.plane.origin).length(), i});
                          std::nth_element(byDistance.begin(), byDistance.begin() + maxSegments, byDistance.end());
                          std::vector<core::Vec3> nearest;
                          for (std::size_t k = 0; k < maxSegments; ++k) {
                              nearest.push_back(cut[byDistance[k].second]);
                              nearest.push_back(cut[byDistance[k].second + 1]);
                          }
                          cut = std::move(nearest);
                      }
                      for (const auto& p : cut) {
                          const auto uv = sk.plane.toLocal(p);
                          surfaceLine.push_back(Json::array({uv.x, uv.y}));
                      }
                  }
              }
              return Json{{"sketch", id},
                          {"surfaceLine", surfaceLine},
                          {"onSurface", onSurface},
                          {"plane", {{"origin", vec(sk.plane.origin)},
                                     {"normal", vec(sk.plane.normal)},
                                     {"xDir", vec(sk.plane.xDir)},
                                     {"yDir", vec(sk.plane.yDir())}}},
                          {"curves", sk.curves.size()},
                          {"profiles", out}};
          });

    r.add({"sketch.add_constraints", "Add Sketch Constraints",
           "Adds geometric constraints and dimensions to a sketch (Fusion: Sketch > Constraints, Sketch "
           "Dimension, shortcut D). The curves move as little as possible to satisfy them; constraints that "
           "conflict are rejected. Dimension values can be expressions using user parameters.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")}, {"constraints", constraintsSchema()}},
                        {"sketch", "constraints"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Add Constraint", [&](cad::Design& d) {
                  Json list = sketchFeature(d, id).params.value("constraints", Json::array());
                  auto solved = d.sketches().find(id);
                  for (const auto& c : params.at("constraints")) {
                      if (solved == d.sketches().end()) {
                          list.push_back(c);
                          continue;
                      }
                      for (const auto& resolved : resolveFix(solved->second.curves, c))
                          list.push_back(resolved);
                  }
                  d.editFeature(id, {{"constraints", list}});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.edit_constraint", "Edit Sketch Constraint",
           "Changes the value of a sketch dimension, or removes a constraint (remove: true). index is the "
           "position in sketch.curves' constraints list.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"index", {{"type", "integer"}, {"minimum", 0}}},
                         {"value", numberOrExpr("New dimension value.")},
                         {"remove", {{"type", "boolean"}}}},
                        {"sketch", "index"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto index = params.at("index").get<std::size_t>();
              const bool remove = params.value("remove", false);
              designChange(ctx, remove ? "Delete Constraint" : "Edit Dimension", [&](cad::Design& d) {
                  Json list = sketchFeature(d, id).params.value("constraints", Json::array());
                  if (index >= list.size())
                      throw cad::DesignError("Sketch has no constraint " + std::to_string(index));
                  if (remove) {
                      list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                  } else {
                      if (!params.contains("value"))
                          throw cad::DesignError("Give value or remove");
                      list[index]["value"] = params.at("value");
                  }
                  d.editFeature(id, {{"constraints", list}});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.curves", "Sketch curves",
           "Lists the solved curves of a sketch (index, kind, points in sketch coordinates) and its constraints, "
           "with the remaining degrees of freedom. Use the indices in sketch.add_constraints.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")}}, {"sketch"})},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const cad::Design& d = design(ctx);
              const cad::Feature& f = sketchFeature(d, id);
              auto it = d.sketches().find(id);
              if (it == d.sketches().end())
                  throw CommandError("No evaluated sketch with id " + std::to_string(id));
              const auto& sk = it->second;
              Json curves = Json::array();
              for (std::size_t i = 0; i < sk.curves.size(); ++i) {
                  const auto& c = sk.curves[i];
                  static const char* kinds[] = {"line", "arc", "circle", "spline"};
                  Json pts = Json::array();
                  for (const auto& p : c.points)
                      pts.push_back(Json::array({p.x, p.y}));
                  Json cj{{"index", i}, {"kind", kinds[static_cast<int>(c.kind)]}, {"points", pts},
                          {"construction", c.construction}};
                  if (c.kind == geometry::SketchCurve::Kind::Circle)
                      cj["radius"] = c.radius;
                  if (const auto ctr = arcCenter(c))
                      cj["center"] = Json::array({ctr->x, ctr->y});
                  curves.push_back(cj);
              }
              Json constraints = Json::array();
              const Json list = f.params.value("constraints", Json::array());
              for (std::size_t i = 0; i < list.size(); ++i) {
                  Json cj = list[i];
                  cj["index"] = i;
                  if (cj.contains("value")) {
                      try {
                          cj["resolved"] = d.evaluate(cj.at("value"));
                      } catch (const std::exception&) {
                      }
                  }
                  constraints.push_back(cj);
              }
              return Json{{"sketch", id},
                          {"curves", curves},
                          {"constraints", constraints},
                          {"degreesOfFreedom", sk.degreesOfFreedom}};
          });
}

void registerFeatureCommands(CommandRegistry& r)
{
    addFeatureCommand(
        r, "extrude", "Extrude",
        "Extrudes sketch profiles along the sketch normal (Fusion: Extrude, shortcut E). Creates a new body or "
        "joins / cuts / intersects existing bodies.",
        merged(profileSelection(),
               {{"distance", numberOrExpr("Distance along the sketch normal (mm); negative goes the other way.")},
                {"extent", {{"type", "string"},
                            {"enum", {"one_side", "symmetric", "two_sides"}},
                            {"description", "one_side (default); symmetric: `distance` on both sides; two_sides: "
                                            "`distance` forwards and `distance2` backwards."}}},
                {"distance2", numberOrExpr("Backward distance for two_sides (mm).")},
                {"flip", {{"type", "boolean"}, {"description", "Extrude against the sketch normal."}}},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch", "distance"});

    addFeatureCommand(
        r, "revolve", "Revolve",
        "Revolves sketch profiles around an axis (Fusion: Revolve).",
        merged(profileSelection(),
               {{"axis", {{"type", Json::array({"string", "object"})},
                          {"description", "\"X\", \"Y\", \"Z\" (world axes through the origin, default Z), "
                                          "{line: curve index of a sketch line} or {origin: [x,y,z], direction: "
                                          "[x,y,z]}."}}},
                {"angle", numberOrExpr("Angle in degrees, default 360.")},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch"});

    addFeatureCommand(r, "fillet", "Fillet", "Rounds edges of a body with a constant radius (Fusion: Fillet, shortcut F).",
                      {{"body", bodyRefSchema("Body.")},
                       {"edges", topologyListSchema("edge")},
                       {"radius", numberOrExpr("Radius (mm).")}},
                      {"body", "edges", "radius"});

    addFeatureCommand(r, "chamfer", "Chamfer", "Bevels edges of a body with an equal distance (Fusion: Chamfer).",
                      {{"body", bodyRefSchema("Body.")},
                       {"edges", topologyListSchema("edge")},
                       {"distance", numberOrExpr("Distance (mm).")}},
                      {"body", "edges", "distance"});

    addFeatureCommand(r, "shell", "Shell",
                      "Hollows a body, removing the selected faces and keeping inside walls of the given thickness.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"thickness", numberOrExpr("Wall thickness (mm).")}},
                      {"body", "thickness"});

    addFeatureCommand(r, "combine", "Combine",
                      "Boolean between bodies (Fusion: Combine): joins, cuts or intersects the target with the tool "
                      "bodies.",
                      {{"target", bodyRefSchema("Target body.")},
                       {"tools", {{"type", "array"}, {"items", bodyRefSchema("Tool body.")}, {"minItems", 1}}},
                       {"operation", operationSchema(false)},
                       {"keepTools", {{"type", "boolean"}, {"description", "Keep the tool bodies (default false)."}}}},
                      {"target", "tools"});

    addFeatureCommand(r, "box", "Box", "Creates an axis-aligned box (Fusion: Box).",
                      {{"corner", point3("Minimum corner, default origin.")},
                       {"size", point3("Size along X, Y, Z.")},
                       {"operation", operationSchema()},
                       {"bodies", bodiesSchema()}},
                      {"size"});

    addFeatureCommand(r, "cylinder", "Cylinder", "Creates a cylinder along +Z (Fusion: Cylinder).",
                      {{"center", point3("Centre of the base, default origin.")},
                       {"radius", numberOrExpr("Radius (mm).")},
                       {"diameter", numberOrExpr("Diameter (mm), alternative to radius.")},
                       {"height", numberOrExpr("Height (mm).")},
                       {"operation", operationSchema()},
                       {"bodies", bodiesSchema()}},
                      {"height"});

    addFeatureCommand(r, "hole", "Hole",
                      "Drills a round hole into a body, normal to the planar face at `point` (Fusion: Hole, "
                      "shortcut H). Through all unless `depth` is given.",
                      {{"body", bodyRefSchema("Body.")},
                       {"point", point3("Hole centre on a planar face.")},
                       {"diameter", numberOrExpr("Diameter (mm).")},
                       {"depth", numberOrExpr("Depth (mm); omit for through all.")},
                       {"through", {{"type", "boolean"}, {"description", "Through all (default when no depth)."}}}},
                      {"body", "point", "diameter"});

    r.add({"feature.base_body", "Convert to Body",
           "Turns an imported part into a timeline body so it can be sketched on, cut, joined, filleted "
           "(Fusion: base feature / Mesh to BRep). B-rep imports (STEP, IGES, BREP) are used as they are; "
           "meshes (STL, OBJ, 3MF) become faceted bodies with coplanar triangles merged into planar faces "
           "(up to 50 000 triangles). The imported part is hidden.",
           objectSchema({{"part", {{"type", "integer"}, {"minimum", 1}, {"description", "Imported part id (document.parts)."}}},
                         {"name", {{"type", "string"}, {"description", "Optional feature name."}}}},
                        {"part"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto partId = params.at("part").get<model::PartId>();
              model::Document& doc = ctx.document;
              const model::Part* source = doc.part(partId);
              if (source == nullptr)
                  throw CommandError("No part with id " + std::to_string(partId));
              cad::Design& d = design(ctx);
              const cad::Design::State before = d.state();
              cad::FeatureId id = 0;
              try {
                  id = d.addFeature("base_body", featureParams(params), params.value("name", std::string()));
              } catch (const cad::DesignError& e) {
                  throw CommandError(e.what());
              }
              const cad::Design::State after = d.state();
              const bool wasVisible = source->visible;
              doc.modifyPart(partId, [](model::Part& p) { p.visible = false; });
              d.syncDocument(doc);
              cad::Design* dp = &d;
              ctx.undoStack.push({"Convert to Body",
                                  [dp, &doc, before, partId, wasVisible]() {
                                      dp->restore(before);
                                      dp->syncDocument(doc);
                                      doc.modifyPart(partId, [&](model::Part& p) { p.visible = wasVisible; });
                                  },
                                  [dp, &doc, after, partId]() {
                                      doc.modifyPart(partId, [](model::Part& p) { p.visible = false; });
                                      dp->restore(after);
                                      dp->syncDocument(doc);
                                  }});
              return addedFeature(d, doc, id);
          });

    r.add({"feature.edit", "Edit Feature",
           "Changes parameters of a feature in the timeline (JSON merge patch: given keys replace, null removes) "
           "and regenerates the model. Fails without changes if the feature would fail.",
           objectSchema({{"id", featureIdSchema("Feature id.")},
                         {"params", {{"type", "object"}, {"description", "Parameters to change."}}}},
                        {"id", "params"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              designChange(ctx, "Edit Feature", [&](cad::Design& d) {
                  d.editFeature(id, params.at("params"));
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.rename", "Rename Feature", "Renames a timeline feature.",
           objectSchema({{"id", featureIdSchema("Feature id.")}, {"name", {{"type", "string"}, {"minLength", 1}}}},
                        {"id", "name"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              designChange(ctx, "Rename Feature", [&](cad::Design& d) {
                  d.renameFeature(id, params.at("name").get<std::string>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.suppress", "Suppress Feature",
           "Suppresses (skips) or unsuppresses a feature; the model regenerates without it.",
           objectSchema({{"id", featureIdSchema("Feature id.")}, {"suppressed", {{"type", "boolean"}}}},
                        {"id", "suppressed"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              const bool s = params.at("suppressed").get<bool>();
              designChange(ctx, s ? "Suppress Feature" : "Unsuppress Feature", [&](cad::Design& d) {
                  d.setSuppressed(id, s);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.delete", "Delete Feature",
           "Deletes a feature from the timeline. Features that depended on it report errors (see timeline.list).",
           objectSchema({{"id", featureIdSchema("Feature id.")}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              designChange(ctx, "Delete Feature", [&](cad::Design& d) {
                  d.removeFeature(id);
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });
}

void registerTimelineCommands(CommandRegistry& r)
{
    r.add({"timeline.list", "Timeline",
           "Lists the design timeline: features in order with parameters and status (ok, error, suppressed, "
           "rolled_back), the marker position, user parameters and the resulting bodies.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return timelineToJson(design(ctx), ctx.document); });

    r.add({"timeline.set_marker", "Move Timeline Marker",
           "Moves the timeline marker (Fusion: roll back). Features after it are not evaluated and new features "
           "are inserted at it. 0 = before the first feature; omit position to move to the end.",
           objectSchema({{"position", {{"type", "integer"}, {"minimum", 0}}}}), true},
          [](CommandContext& ctx, const Json& params) {
              designChange(ctx, "Move Timeline Marker", [&](cad::Design& d) {
                  d.setMarker(params.contains("position") ? params.at("position").get<std::size_t>()
                                                          : d.features().size());
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"parameter.set", "Set Parameter",
           "Creates or changes a user parameter (Fusion: Change Parameters). The expression may use other "
           "parameters and units (mm, cm, m, in, deg). Features using it regenerate.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}}},
                         {"expression", {{"type", Json::array({"string", "number"})}}},
                         {"comment", {{"type", "string"}}}},
                        {"name", "expression"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto name = params.at("name").get<std::string>();
              const Json& e = params.at("expression");
              const std::string expr = e.is_string() ? e.get<std::string>() : e.dump();
              designChange(ctx, "Set Parameter " + name, [&](cad::Design& d) {
                  d.setParameter(name, expr, params.value("comment", std::string()));
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"parameter.remove", "Remove Parameter", "Removes a user parameter.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}}}}, {"name"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto name = params.at("name").get<std::string>();
              designChange(ctx, "Remove Parameter " + name, [&](cad::Design& d) {
                  d.removeParameter(name);
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });
}

void registerBodyCommands(CommandRegistry& r)
{
    r.add({"body.faces", "Body faces",
           "Lists the faces of a body: index, surface type, area, centre and outward normal. Use the index or "
           "the centre as {point} in sketch.create face, feature.shell faces.",
           objectSchema({{"body", bodyRefSchema("Body.")}}, {"body"})},
          [](CommandContext& ctx, const Json& params) {
              const cad::Body& b = requireBody(design(ctx), params.at("body"));
              Json out = Json::array();
              for (const auto& f : geometry::faces(b.shape)) {
                  out.push_back({{"index", f.index},
                                 {"type", f.type},
                                 {"area", f.area},
                                 {"center", vec(f.center)},
                                 {"normal", vec(f.normal)}});
              }
              return Json{{"body", b.id}, {"name", b.name}, {"faces", out}};
          });

    r.add({"body.edges", "Body edges",
           "Lists the edges of a body: index, curve type, length, start, middle and end points. Use the index "
           "or the middle as {point} in feature.fillet / feature.chamfer edges.",
           objectSchema({{"body", bodyRefSchema("Body.")}}, {"body"})},
          [](CommandContext& ctx, const Json& params) {
              const cad::Body& b = requireBody(design(ctx), params.at("body"));
              Json out = Json::array();
              for (const auto& e : geometry::edges(b.shape)) {
                  out.push_back({{"index", e.index},
                                 {"type", e.type},
                                 {"length", e.length},
                                 {"start", vec(e.start)},
                                 {"middle", vec(e.middle)},
                                 {"end", vec(e.end)}});
              }
              return Json{{"body", b.id}, {"name", b.name}, {"edges", out}};
          });
}

Json frameToJson(const cad::ReferenceFrame& f)
{
    const auto v = [](const core::Vec3& p) { return Json::array({p.x, p.y, p.z}); };
    return Json{{"name", f.name},
                {"origin", v(f.origin)},
                {"normal", v(f.normal)},
                {"xDir", v(f.xDir)},
                {"yDir", v(f.normal.cross(f.xDir))},
                {"comment", f.comment}};
}

Json framesToJson(const cad::Design& d)
{
    Json out = Json::array();
    for (const auto& f : d.frames())
        out.push_back(frameToJson(f));
    return Json{{"frames", out}};
}

core::Vec3 vecParam(const Json& params, const char* key, core::Vec3 fallback)
{
    if (!params.contains(key))
        return fallback;
    const Json& a = params.at(key);
    return {a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
}

void registerFrameCommands(CommandRegistry& r)
{
    const Json vec3 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
    auto withDescription = [](Json schema, const std::string& text) {
        schema["description"] = text;
        return schema;
    };
    r.add({"frame.set", "Set Reference Frame",
           "Creates or replaces a named reference frame (work plane): origin, normal (Z axis) and xDir (X axis, "
           "made orthogonal to the normal), mm. Use the name as sketch.create plane to sketch on it. Changing a "
           "frame regenerates the sketches that use it. Frames belong to the design (undoable, per variant).",
           objectSchema({{"name",
                          {{"type", "string"}, {"minLength", 1}, {"description", "Frame name (not XY / XZ / YZ)."}}},
                         {"origin", withDescription(vec3, "[x, y, z] in mm, default [0, 0, 0].")},
                         {"normal", withDescription(vec3, "Z axis / plane normal, default [0, 0, 1].")},
                         {"xDir", withDescription(vec3, "X axis, default [1, 0, 0].")},
                         {"comment", {{"type", "string"}}}},
                        {"name"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              cad::ReferenceFrame f;
              f.name = params.at("name").get<std::string>();
              if (const cad::ReferenceFrame* old = design(ctx).frame(f.name))
                  f = *old;
              f.origin = vecParam(params, "origin", f.origin);
              f.normal = vecParam(params, "normal", f.normal);
              f.xDir = vecParam(params, "xDir", f.xDir);
              f.comment = params.value("comment", f.comment);
              designChange(ctx, "Set Frame " + f.name, [&](cad::Design& d) {
                  d.setFrame(f);
                  return Json{};
              });
              return frameToJson(*design(ctx).frame(f.name));
          });

    r.add({"frame.remove", "Remove Reference Frame",
           "Removes a reference frame. Sketches placed on it then report errors.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}}}}, {"name"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto name = params.at("name").get<std::string>();
              designChange(ctx, "Remove Frame " + name, [&](cad::Design& d) {
                  d.removeFrame(name);
                  return Json{};
              });
              return framesToJson(design(ctx));
          });

    r.add({"frame.list", "Reference frames", "Lists the reference frames (work planes) of the design.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return framesToJson(design(ctx)); });
}

} // namespace

void registerDesignCommands(CommandRegistry& registry)
{
    registerSketchCommands(registry);
    registerFeatureCommands(registry);
    registerTimelineCommands(registry);
    registerBodyCommands(registry);
    registerFrameCommands(registry);
}

} // namespace aerocad::commands
