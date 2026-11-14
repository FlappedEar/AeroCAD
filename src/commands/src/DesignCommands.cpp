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
#include <functional>
#include <cmath>
#include <numbers>
#include <array>
#include <deque>
#include <map>
#include <vector>
#include <limits>
#include <optional>
#include <tuple>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

/// Douglas-Peucker simplification of a 2D polyline.
void simplifyPolyline(const std::vector<std::array<double, 2>>& pts, std::size_t a, std::size_t b, double tol,
                      std::vector<bool>& keep)
{
    if (b <= a + 1)
        return;
    const double dx = pts[b][0] - pts[a][0], dy = pts[b][1] - pts[a][1];
    const double len = std::hypot(dx, dy);
    double worst = -1.0;
    std::size_t at = a;
    for (std::size_t i = a + 1; i < b; ++i) {
        const double ex = pts[i][0] - pts[a][0], ey = pts[i][1] - pts[a][1];
        const double d = len > 1e-12 ? std::abs(dx * ey - dy * ex) / len : std::hypot(ex, ey);
        if (d > worst) {
            worst = d;
            at = i;
        }
    }
    if (worst > tol) {
        keep[at] = true;
        simplifyPolyline(pts, a, at, tol, keep);
        simplifyPolyline(pts, at, b, tol, keep);
    }
}

/// Where a fin sketch's plane cuts the surface, for an agent: the cut segments chained into
/// polylines, the one passing nearest the sketch origin (the picked point on the surface)
/// kept and simplified to at most `maxPoints` points. The raw segments (surfaceLine) run to
/// thousands on a scan, too many to read.
Json surfaceCurveFromSegments(std::vector<std::array<double, 2>> all, double reach, std::size_t maxPoints)
{
    // Only near the sketch: the plane may cut the whole scan.
    std::vector<std::array<double, 2>> seg;
    for (std::size_t i = 0; i + 1 < all.size(); i += 2)
        if (std::hypot(all[i][0], all[i][1]) <= reach && std::hypot(all[i + 1][0], all[i + 1][1]) <= reach) {
            seg.push_back(all[i]);
            seg.push_back(all[i + 1]);
        }
    if (seg.size() < 2)
        return nullptr;
    // Chain by shared end points (the slicer emits each crossing point once per triangle).
    const auto key = [](const std::array<double, 2>& p) {
        return std::make_pair(std::llround(p[0] * 1e4), std::llround(p[1] * 1e4));
    };
    std::map<std::pair<long long, long long>, std::vector<std::size_t>> ends;
    const std::size_t n = seg.size() / 2;
    for (std::size_t i = 0; i < n; ++i) {
        ends[key(seg[2 * i])].push_back(i);
        ends[key(seg[2 * i + 1])].push_back(i);
    }
    std::vector<bool> used(n, false);
    std::vector<std::vector<std::array<double, 2>>> lines;
    for (std::size_t start = 0; start < n; ++start) {
        if (used[start])
            continue;
        used[start] = true;
        std::deque<std::array<double, 2>> line{seg[2 * start], seg[2 * start + 1]};
        for (int side = 0; side < 2; ++side) {
            for (;;) {
                const auto& tip = side == 0 ? line.back() : line.front();
                auto it = ends.find(key(tip));
                std::size_t next = n;
                if (it != ends.end())
                    for (std::size_t c : it->second)
                        if (!used[c]) {
                            next = c;
                            break;
                        }
                if (next == n)
                    break;
                used[next] = true;
                const bool forward = key(seg[2 * next]) == key(tip);
                const auto& far = seg[2 * next + (forward ? 1 : 0)];
                if (side == 0)
                    line.push_back(far);
                else
                    line.push_front(far);
            }
        }
        lines.emplace_back(line.begin(), line.end());
    }
    const auto distanceToOrigin = [](const std::vector<std::array<double, 2>>& l) {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& p : l)
            best = std::min(best, std::hypot(p[0], p[1]));
        return best;
    };
    const auto nearest = std::min_element(lines.begin(), lines.end(), [&](const auto& a, const auto& b) {
        return distanceToOrigin(a) < distanceToOrigin(b);
    });
    const auto& pts = *nearest;
    double tol = 0.2; // below a scan's noise is detail an agent cannot use
    std::vector<bool> keep;
    for (int round = 0; round < 30; ++round, tol *= 1.6) {
        keep.assign(pts.size(), false);
        keep.front() = keep.back() = true;
        simplifyPolyline(pts, 0, pts.size() - 1, tol, keep);
        if (static_cast<std::size_t>(std::count(keep.begin(), keep.end(), true)) <= maxPoints)
            break;
    }
    Json points = Json::array();
    for (std::size_t i = 0; i < pts.size(); ++i)
        if (keep[i])
            points.push_back(Json::array({pts[i][0], pts[i][1]}));
    return Json{{"points", points},
                {"tolerance", tol},
                {"within", reach},
                {"note", "Where the surface is in sketch coordinates (x along the surface normal, 0 at the "
                         "surface), simplified to within `tolerance` mm, up to `within` mm from the sketch "
                         "origin. The fin root must reach below these x values."}};
}

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
                {"description", description + " (or expression)"}};
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

/// A path of connected sketch curves (sweep, pipe, rib, pattern on path). One shared definition, so the
/// tool listing shows it once.
Json pathSchema()
{
    return Json{{"type", "object"},
                {"description", "{sketch: id, curves: [indices]} (default: all curves of that sketch); "
                                "the curves must connect."},
                {"properties", {{"sketch", featureIdSchema("Path sketch.")},
                                {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}},
                                            {"description", "Curve indices (default: all)."}}}}},
                {"required", {"sketch"}}};
}

Json bodyRefSchema(const std::string& description)
{
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Body id (its feature id, see timeline.list) or name."}};
}

Json curveSchema()
{
    return Json{
        {"type", "object"},
        {"description",
         "A sketch curve. type selects the fields: line {start, end}; rectangle {corner, width, height} or "
         "{corner, opposite}; center_rectangle {center, width, height}; three_point_rectangle {start, end, height} (signed: "
         "positive to the left of start -> end); circle {center, radius | diameter}; "
         "arc {start, through, end} (three-point arc); center_arc {center, radius, startAngle, endAngle} "
         "(degrees, counter-clockwise); polygon {center, radius, sides, angle?, mode?} (circumscribed: radius to a side) or {mode: edge, start, end, sides} (left of start -> end); slot {start, end, width, mode?} (overall: start, end are the far points) "
         "or {mode: center_point, center, end, width}; spline {points, closed?}; point {at} (a sketch point: no edge, no profile; "
         "joins a curve end there); tangent_arc {from: {curve, point: start | end}, end} "
         "(add_curves only: leaves a line or arc end tangentially); "
         "tangent_circle {curves, near, radius?} (add_curves only: tangent to 2 or 3 lines, arcs or circles); "
         "arc_slot {center, radius, width, start_angle, end_angle} (slot along an arc); "
         "control_spline {points (control polygon), degree? 2-5, closed?}. "
         "conic {start, end, apex, rho?} (tangent at its ends to the lines towards apex); "
         "airfoil {naca \"2412\" | dat (.dat file text) | coordinates, chord, at? (leading edge), angle?}; "
         "text {at, text, height?, angle?, font?, bold?, italic?} (letters are profiles; at = baseline start, "
         "height mm, default 5). "
         "construction: true excludes it from profiles."},
        {"properties",
         {{"type",
           {{"type", "string"},
            {"description", "Curve kind; selects which other fields apply."},
            {"enum", {"line", "rectangle", "center_rectangle", "three_point_rectangle", "circle", "arc", "center_arc", "polygon", "slot",
                      "spline", "point", "tangent_arc", "tangent_circle", "arc_slot", "control_spline", "ellipse", "elliptical_arc", "conic", "airfoil", "text"}}}},
          {"curves", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}},
                      {"description", "tangent_circle: 2 or 3 line, arc or circle indices (`radius` optional for 2)."}}},
          {"near", point2("tangent_circle: a point near the circle wanted.")},
          {"from", {{"type", "object"},
                    {"description", "tangent_arc: the end it starts from, {curve: index, point: start | end}."}}},
          {"start", point2("Start point.")},
          {"end", point2("End point.")},
          {"at", point2("point: its position.")},
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
          {"mode", {{"type", "string"},
                    {"enum", {"inscribed", "circumscribed", "edge", "center_to_center", "overall", "center_point"}},
                    {"description", "polygon: inscribed (default; `radius` to a corner), circumscribed (`radius` to a "
                                    "side) or edge. slot: center_to_center (default), overall or center_point."}}},
          {"angle", numberOrExpr("Polygon rotation, or the direction of an ellipse's major axis (degrees).")},
          {"major_radius", numberOrExpr("ellipse: centre to the end of the major axis (mm).")},
          {"minor_radius", numberOrExpr("ellipse: half the minor axis (mm).")},
          {"start_angle", numberOrExpr("elliptical_arc: parametric start angle (degrees; x = major cos t, y = minor sin t "
                                       "along the axes), counter-clockwise to end_angle. arc_slot: {center, radius of "
                                       "the centre line, width, start_angle, end_angle}, counter-clockwise.")},
          {"end_angle", numberOrExpr("elliptical_arc: parametric end angle (degrees); arc_slot: end angle.")},
          {"text", {{"type", "string"}, {"description", "text: characters."}}},
          {"font", {{"type", "string"}, {"description", "text: family."}}},
          {"bold", {{"type", "boolean"}, {"description", "text: bold."}}},
          {"italic", {{"type", "boolean"}, {"description", "text: italic."}}},
          {"apex", point2("conic: where its end tangents meet.")},
          {"naca", {{"type", Json::array({"string", "integer"})}, {"description", "airfoil: NACA 4-digit code."}}},
          {"dat", {{"type", "string"}, {"description", "airfoil: .dat text (Selig or Lednicer)."}}},
          {"coordinates", {{"type", "array"}, {"items", point2("Point.")}, {"description", "airfoil: [x, y], Selig order."}}},
          {"chord", numberOrExpr("airfoil: chord length (mm).")},
          {"rho", numberOrExpr("conic: 0 to 1, default 0.5 (parabola).")},
          {"points", {{"type", "array"}, {"items", point2("Fit point.")}, {"minItems", 2},
                      {"description", "spline: fit points (2+); control_spline: control points (degree + 1+)."}}},
          {"closed", {{"type", "boolean"}, {"description", "spline, control_spline: closed curve."}}},
          {"degree", {{"type", "integer"}, {"minimum", 2}, {"maximum", 5}, {"description", "control_spline degree, default 3."}}},
          {"construction", {{"type", "boolean"}, {"description", "true = construction geometry, not part of profiles."}}},
          {"join_start",
           {{"type", "boolean"},
            {"description", "arc: false = its start is joined only by constraints, not to another curve end "
                            "that happens to be there (default true)."}}},
          {"join_end", {{"type", "boolean"}, {"description", "arc: as join_start, for its end."}}}}},
        {"required", {"type"}},
        {"additionalProperties", false}};
}

Json constraintPointSchema()
{
    return Json{{"type", Json::array({"string", "object"})},
                {"description", "\"origin\" (sketch origin) or {curve: index, point: start | end | center} "
                                "(an ellipse: center, end = the end of its major axis; axis = that point for an ellipse or elliptical arc; apex = a conic's apex), or a fit point of a spline {curve, point: fit, index} (0 = first fit point)."},
                {"properties", {{"curve", {{"type", "integer"}, {"minimum", 0}, {"description", "Curve index in sketch.curves."}}},
                                {"point", {{"type", "string"}, {"enum", {"start", "end", "center", "axis", "apex", "fit"}},
                                           {"description", "Which point of the curve."}}},
                                {"index", {{"type", "integer"}, {"minimum", 0},
                                           {"description", "point: fit only: fit point index (0 = first)."}}}}}};
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
         "{curves: 2 lines or 2 arcs/circles}; concentric {curves: 2}; symmetric {points: 2, curve: line}, or {curves: 2 lines, 2 circles "
         "or 2 arcs, curve: line} (stored as the matching point symmetries, plus equal radii); fix "
         "{points and/or curves}. Dimensions (value: number or expression): distance {points: 2} or {point, "
         "curve: line}; horizontal_distance / vertical_distance {points: 2}; length {curve: line}; radius / "
         "diameter {curve}; angle {curves: 2 lines} (degrees); major_radius / minor_radius {curve: ellipse} "
         "(horizontal / vertical on an ellipse turn its major axis)."},
        {"properties",
         {{"type",
           {{"type", "string"},
            {"description", "Constraint or dimension kind; selects which other fields apply."},
            {"enum", {"coincident", "point_on_curve", "midpoint", "horizontal", "vertical", "parallel",
                      "perpendicular", "collinear", "tangent", "equal", "concentric", "symmetric", "fix",
                      "distance", "horizontal_distance", "vertical_distance", "length", "radius", "diameter",
                      "angle", "major_radius", "minor_radius"}}}},
          {"curve", {{"type", "integer"}, {"minimum", 0}, {"description", "Curve index in sketch.curves."}}},
          {"curves", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}},
                      {"description", "Curve indices in sketch.curves."}}},
          {"point", constraintPointSchema()},
          {"points", {{"type", "array"}, {"items", constraintPointSchema()}, {"description", "Points, see point."}}},
          {"value", numberOrExpr("Dimension value (mm, or degrees for angle).")},
          {"side", {{"type", "integer"},
                    {"enum", {-2, -1, 1}},
                    {"description", "tangent: which branch (omit = as drawn). Line and arc/circle: 1 = centre left of "
                                    "the line's start->end, -1 = right. Two arcs/circles: 1 = outside, -1 = inside with "
                                    "the first the larger, -2 = inside with the second the larger."}}},
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
                 "the name of a reference frame (frame.list), a construction plane {plane: id or name} "
                 "(feature.plane), or {origin: [x,y,z], normal: [x,y,z], xDir: [x,y,z]}. Z is up."}};
}

Json faceRefSchema()
{
    return Json{{"type", "object"},
                {"description", "Planar face of a body to sketch on: {body, index} or {body, point: [x,y,z]} "
                                "(the face nearest to the point; robust when earlier features change)."},
                {"properties", {{"body", bodyRefSchema("Body.")}, {"index", {{"type", "integer"}, {"minimum", 0}, {"description", "Face index (body.faces)."}}},
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
                {"description", "Fusion's Operation: new_body (default), join, cut or intersect; the last three act on "
                                "`bodies`, or every body the result touches."}};
}

Json bodiesSchema()
{
    return Json{{"type", "array"},
                {"items", bodyRefSchema("Body.")},
                {"description", "Bodies to join/cut/intersect (default: every body touched)."}};
}

Json topologyListSchema(const std::string& what)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"integer", "object"})}}},
                {"minItems", 1},
                {"description", what + " indices (body." + what + "s) or {point: [x,y,z]} for the " + what +
                                    " nearest to it; point references follow the body when earlier dimensions change."}};
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
    if (!st.report.is_null()) {
        j["report"] = st.report;
        // A fit that missed its tolerance is kept as a marked approximation (CAD-85).
        if (st.report.contains("fit") && st.report["fit"].is_object() &&
            !st.report["fit"].value("withinTolerance", true))
            j["approximate"] = true;
    }
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
                       {"solids", b.shape.solidCount()},
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
    Json sketchEdit = nullptr;
    if (const auto& e = d.state().sketchEdit) {
        const auto target = cad::sketchEditTarget(d.state());
        sketchEdit = {{"sketch", e->sketch}, {"restoreTo", target ? Json(*target) : Json(nullptr)}};
    }
    return Json{{"features", features},
                {"marker", d.marker()},
                {"sketchEdit", sketchEdit},
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
    // A command that fails part-way (after some edits) leaves the design as it was: nothing
    // half-applied without an undo step (the document mirror is only synced on success).
    try {
        result = fn(d);
    } catch (const cad::DesignError& e) {
        d.restore(before);
        throw CommandError(e.what());
    } catch (...) {
        d.restore(before);
        throw;
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
        // An agent cannot see the sketch: say why nothing can be made from it yet.
        if (it->second.geometry.profiles.empty())
            j["hint"] = it->second.curves.empty()
                ? "The sketch has no curves yet: add them with sketch.add_curves."
                : "No closed profile: extrude, revolve, part on surface and fin need a closed outline. End the "
                  "last curve where the first one starts (or use rectangle, circle, polygon, slot or a spline with "
                  "closed: true); sketch.curves lists the end points.";
    }
    if (auto it = d.planes().find(id); it != d.planes().end()) {
        const auto& pl = it->second.plane;
        j["plane"] = {{"origin", vec(pl.origin)}, {"normal", vec(pl.normal)}, {"xDir", vec(pl.xDir)}};
    }
    if (auto it = d.axes().find(id); it != d.axes().end())
        j["axis"] = {{"origin", vec(it->second.origin)}, {"direction", vec(it->second.direction)}};
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
         "visible. The resolved plane is stored, so the sketch does not move when the part changes."},
        {"properties",
         {{"part", {{"type", "integer"}, {"minimum", 1}, {"description", "Part id (document.parts)."}}},
          {"point", point3("Clicked point on the part.")},
          {"radius",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"description", "Size of the area around `point` (mm). Default: a tenth of the part's size."}}},
          {"useSelection", {{"type", "boolean"}, {"description", "Use the region selection (default when set)."}}},
          {"facing", point3("Face the plane this way (a closed solid).")},
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
    if (ref.contains("facing")) {
        // A closed solid has no single side: the caller says which (e.g. towards the viewer).
        const auto& f = ref.at("facing");
        facing = {f.at(0).get<double>(), f.at(1).get<double>(), f.at(2).get<double>()};
    }
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

/// Adds matching hints to {point} edge/face references in `items` on the body `bodyRef`: the
/// point relative to the body's bounding box, the edge direction or face normal, and the
/// edge/face type. Regeneration then finds the same edge or face after dimensions change
/// (see matchTopology in Design.cpp). References that already carry hints are left alone.
void addTopologyHints(const cad::Design& d, const Json& bodyRef, Json& items, bool edgesWanted)
{
    if (!items.is_array())
        return;
    const cad::Body* body = nullptr;
    for (const auto& [bid, b] : d.bodies()) {
        if ((bodyRef.is_number_integer() && bodyRef.get<cad::FeatureId>() == bid) ||
            (bodyRef.is_string() && bodyRef.get<std::string>() == b.name))
            body = &b;
    }
    if (body == nullptr)
        return;
    const auto box = body->shape.bounds();
    const core::Vec3 size = box.size();
    auto rel = [&](double v, double lo, double extent) { return extent > 1e-9 ? (v - lo) / extent : 0.0; };
    for (auto& item : items) {
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
}

/// Topology hints for the picked references of fillet / chamfer (edges), shell (faces) and an
/// extrude or rib up to faces of a body (to.faces), and a construction axis through an edge or face.
Json describeTopologyRefs(const cad::Design& d, const std::string& type, Json params)
{
    // Replace Face has both: the faces that move and, as the tool, faces of another body.
    if (type == "replace_face" && params.contains("to") && params["to"].is_object() && params["to"].contains("faces") &&
        params["to"].contains("body"))
        addTopologyHints(d, params["to"]["body"], params["to"]["faces"], false);
    if ((type == "fillet" || type == "chamfer") && params.contains("edges") && params.contains("body"))
        addTopologyHints(d, params["body"], params["edges"], true);
    else if ((type == "shell" || type == "press_pull" || type == "delete_faces" || type == "draft" || type == "thread" || type == "split_face" || type == "replace_face") && params.contains("faces") &&
             params.contains("body"))
        addTopologyHints(d, params["body"], params["faces"], false);
    else if ((type == "extrude" || type == "rib") && params.contains("to") && params["to"].is_object() &&
             params["to"].contains("faces") && params["to"].contains("body"))
        addTopologyHints(d, params["to"]["body"], params["to"]["faces"], false);
    else if (type == "plane" && params.contains("tangent") && params["tangent"].is_object() && params["tangent"].contains("body") &&
             !params["tangent"].contains("index")) {
        // The face a tangent plane touches: {body, point} gets a hint that follows the body.
        Json one = Json::array({params["tangent"]});
        addTopologyHints(d, one[0]["body"], one, false);
        params["tangent"] = one[0];
    } else if (type == "axis") {
        // The edge or face an axis goes through: {body, point} gets a hint that follows the body.
        for (const char* key : {"edge", "face"}) {
            if (params.contains(key) && params[key].is_object() && params[key].contains("body") && !params[key].contains("index")) {
                Json one = Json::array({params[key]});
                addTopologyHints(d, one[0]["body"], one, std::string(key) == "edge");
                params[key] = one[0];
            }
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

// A turn or scale centre: `params[key]` ([u, v]) when given, else the middle of the
// curves' bounds (circles by their radius), as the sketch tools preview it.
geometry::Vec2 curvesPivot(const cad::Design& d, cad::FeatureId id, const std::vector<std::size_t>& picked,
                           const Json& params, const char* key)
{
    if (params.contains(key))
        return {params.at(key)[0].get<double>(), params.at(key)[1].get<double>()};
    geometry::Vec2 pivot{0, 0};
    if (auto it = d.sketches().find(id); it != d.sketches().end()) {
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (const std::size_t i : picked) {
            if (i >= it->second.curves.size())
                continue;
            const auto& c = it->second.curves[i];
            // An ellipse is bounded around its centre (its axis end is on it).
            const bool oval = c.kind == geometry::SketchCurve::Kind::Ellipse;
            std::vector<geometry::Vec2> pts = oval ? std::vector<geometry::Vec2>{c.points[0]} : c.points;
            // A conic is bounded by its curve (not its apex) and text by its letters, as the
            // sketch tools preview it.
            if (c.kind == geometry::SketchCurve::Kind::Conic) {
                pts.clear();
                for (int k = 0; k <= 64; ++k)
                    pts.push_back(geometry::conicPoint(c, k / 64.0));
            } else if (c.kind == geometry::SketchCurve::Kind::Text) {
                try {
                    std::vector<geometry::Vec2> letters;
                    for (const auto& line : geometry::textOutlines(c))
                        letters.insert(letters.end(), line.begin(), line.end());
                    if (!letters.empty())
                        pts = std::move(letters);
                } catch (const geometry::ModelingError&) {
                    // The baseline points stay (no font).
                }
            }
            for (const auto& p : pts) {
                const double rad = c.kind == geometry::SketchCurve::Kind::Circle ? c.radius
                                   : oval ? std::max(c.radius, std::hypot(c.points[1].x - p.x, c.points[1].y - p.y))
                                          : 0.0;
                lo[0] = std::min(lo[0], p.x - rad), lo[1] = std::min(lo[1], p.y - rad);
                hi[0] = std::max(hi[0], p.x + rad), hi[1] = std::max(hi[1], p.y + rad);
            }
        }
        if (lo[0] <= hi[0])
            pivot = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2};
    }
    return pivot;
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

/// Position of a constraint point reference ("origin", {curve, point} or a spline fit point
/// {curve, point: fit, index}) in solved curves.
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
    if (c.kind == K::Point && which != "fit" && !c.points.empty())
        return c.points[0]; // start, end and centre of a sketch point are the point
    if (c.kind == K::Ellipse && (which == "center" || which == "end"))
        return which == "center" ? c.points[0] : c.points[1]; // end: of the major axis
    if (c.kind == K::Ellipse && which == "axis")
        return c.points[1];
    if (c.kind == K::EllipticalArc && (which == "center" || which == "axis"))
        return which == "center" ? c.points[1] : c.points[2];
    if (which == "axis")
        throw CommandError("Curve " + std::to_string(i) + " has no major axis (only ellipses do)");
    if (which == "apex") {
        if (c.kind != K::Conic)
            throw CommandError("Curve " + std::to_string(i) + " has no apex (only conics do)");
        return c.points[1];
    }
    if (which == "center") {
        if (c.kind == K::Circle)
            return c.points[0];
        if (const auto ctr = arcCenter(c))
            return *ctr;
    } else if (which == "fit") {
        const auto k = ref.value("index", std::size_t{0});
        if (c.kind == K::Spline && k < c.points.size())
            return c.points[k];
    } else if (c.kind != K::Circle && c.kind != K::Ellipse && !c.points.empty()) {
        return which == "end" ? c.points.back() : c.points.front();
    }
    throw CommandError("Curve " + std::to_string(i) + " has no point '" + which + "'");
}

/// A tangent arc (Fusion: Arc > Tangent Arc), {from: {curve, point}, end}: an arc that leaves the
/// end of a line or arc in its direction and ends at `end`, as a three-point arc. Returns the
/// arc item, the index of the curve it is tangent to and the tangent side for that constraint
/// (SketchConstraint::side), taken from the solved geometry it was built on.
std::tuple<Json, std::size_t, int> resolveTangentArc(const cad::Design& design,
                                                     const std::vector<geometry::SketchCurve>& curves, const Json& item)
{
    using K = geometry::SketchCurve::Kind;
    if (!item.contains("from") || !item.contains("end"))
        throw CommandError("tangent_arc: give from {curve, point} and end");
    const Json& from = item.at("from");
    const auto i = from.at("curve").get<std::size_t>();
    if (i >= curves.size())
        throw CommandError("No curve " + std::to_string(i) + " in the sketch");
    const std::string which = from.value("point", std::string("end"));
    if (which != "start" && which != "end")
        throw CommandError("tangent_arc: from.point must be start or end");
    const auto& c = curves[i];
    const geometry::Vec2 s = pointPosition(curves, {{"curve", i}, {"point", which}});
    // Direction leaving the curve at that end.
    geometry::Vec2 t{0, 0};
    std::optional<geometry::Vec2> srcCtr;
    if (c.kind == K::Line) {
        const auto& a = c.points[0];
        const auto& b = c.points[1];
        t = which == "end" ? geometry::Vec2{b.x - a.x, b.y - a.y} : geometry::Vec2{a.x - b.x, a.y - b.y};
    } else if (c.kind == K::Arc) {
        const auto ctr = arcCenter(c);
        if (!ctr)
            throw CommandError("tangent_arc: curve " + std::to_string(i) + " is a degenerate arc");
        srcCtr = geometry::Vec2{ctr->x, ctr->y};
        const auto& p = c.points;
        const bool ccw = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x) > 0;
        // Direction of travel at s (counter-clockwise: the radius turned left), reversed at the start.
        const geometry::Vec2 r{s.x - ctr->x, s.y - ctr->y};
        t = ccw ? geometry::Vec2{-r.y, r.x} : geometry::Vec2{r.y, -r.x};
        if (which == "start")
            t = {-t.x, -t.y};
    } else {
        throw CommandError("tangent_arc: start from the end of a line or an arc");
    }
    const double tl = std::hypot(t.x, t.y);
    const auto& e = item.at("end");
    const geometry::Vec2 end{design.evaluate(e.at(0)), design.evaluate(e.at(1))};
    const geometry::Vec2 d{end.x - s.x, end.y - s.y};
    const geometry::Vec2 n{-t.y / tl, t.x / tl}; // left of the direction
    const double side = n.x * d.x + n.y * d.y, dd = d.x * d.x + d.y * d.y;
    if (tl < 1e-12 || dd < 1e-18 || std::abs(side) < 1e-9 * std::sqrt(dd))
        throw CommandError("tangent_arc: the end lies on the tangent line (that would be a straight line)");
    // Centre on the normal at s, equally far from s and end; the arc turns towards the end.
    const double k = dd / (2 * side);
    const geometry::Vec2 ctr{s.x + n.x * k, s.y + n.y * k};
    const double a0 = std::atan2(s.y - ctr.y, s.x - ctr.x);
    double a1 = std::atan2(end.y - ctr.y, end.x - ctr.x);
    if (side > 0) {
        while (a1 <= a0)
            a1 += 2 * std::numbers::pi;
    } else {
        while (a1 >= a0)
            a1 -= 2 * std::numbers::pi;
    }
    const double am = (a0 + a1) / 2, rad = std::abs(k);
    Json arc{{"type", "arc"},
             {"start", Json::array({s.x, s.y})},
             {"through", Json::array({ctr.x + rad * std::cos(am), ctr.y + rad * std::sin(am)})},
             {"end", Json::array({end.x, end.y})}};
    if (item.value("construction", false))
        arc["construction"] = true;
    // Joined to the source by an explicit constraint; the solved start may lie on an unrelated
    // curve end as stored (the solver joins ends by their stored positions).
    arc["join_start"] = false;
    arc["join_end"] = false; // add_curves joins it to a curve end it lands on, by constraint
    // The branch the arc was built on: for a line, the centre's side of start->end; for an arc,
    // outside (centres on opposite sides of the joint) or inside.
    int tangentSide = 1;
    if (srcCtr) {
        const bool outside = (ctr.x - s.x) * (srcCtr->x - s.x) + (ctr.y - s.y) * (srcCtr->y - s.y) < 0;
        const double srcRad = std::hypot(s.x - srcCtr->x, s.y - srcCtr->y);
        tangentSide = outside ? 1 : srcRad >= rad ? -1 : -2;
    }
    else {
        const auto& a = c.points[0];
        const auto& b = c.points[1];
        tangentSide = (b.x - a.x) * (ctr.y - a.y) - (b.y - a.y) * (ctr.x - a.x) > 0 ? 1 : -1;
    }
    return {arc, i, tangentSide};
}

/// A circle tangent to two or three curves of which some are arcs or circles (Apollonius): solved by
/// Newton's method for every way of touching each curve, from many starting guesses. With a given
/// `radius` and two curves the centre nearest `near` is taken; otherwise the circle also passes through
/// `near` (two curves) or is simply the one with the centre nearest to it (three curves). Returns the
/// circle item and, per curve, the explicit tangent side as the tangent constraint wants it
/// (new circle first): line +1/-1 (centre left/right of the line's direction), circle +1 outside,
/// -1 new circle larger and around the other one, -2 the other one larger and around the new one.
std::pair<Json, std::vector<std::pair<std::size_t, int>>> resolveTangentCircleGeneral(
    const cad::Design& design, const std::vector<geometry::SketchCurve>& curves, const Json& item,
    const std::vector<std::size_t>& idx)
{
    using geometry::SketchCurve;
    using geometry::Vec2;
    struct Cv {
        bool line = false;
        Vec2 a{}, n{}; // line: a point, unit normal
        double d = 0;  // line: n . x = d
        Vec2 o{};      // circle: centre
        double R = 0;  // circle: radius
    };
    std::vector<Cv> cv;
    for (const std::size_t i : idx) {
        if (i >= curves.size())
            throw CommandError("No curve " + std::to_string(i) + " in the sketch");
        const auto& c = curves[i];
        Cv k;
        if (c.kind == SketchCurve::Kind::Line) {
            const Vec2 a = c.points[0], b = c.points[1];
            const double len = std::hypot(b.x - a.x, b.y - a.y);
            if (len < 1e-12)
                throw CommandError("tangent_circle: curve " + std::to_string(i) + " is a point");
            k.line = true;
            k.a = a;
            k.n = {-(b.y - a.y) / len, (b.x - a.x) / len};
            k.d = k.n.x * a.x + k.n.y * a.y;
        } else if (c.kind == SketchCurve::Kind::Circle) {
            k.o = c.points[0];
            k.R = c.radius;
        } else if (c.kind == SketchCurve::Kind::Arc) {
            const Vec2 p1 = c.points[0], p2 = c.points[1], p3 = c.points[2];
            const double d = 2 * (p1.x * (p2.y - p3.y) + p2.x * (p3.y - p1.y) + p3.x * (p1.y - p2.y));
            if (std::abs(d) < 1e-12)
                throw CommandError("tangent_circle: curve " + std::to_string(i) + " is a straight arc");
            const double s1 = p1.x * p1.x + p1.y * p1.y, s2 = p2.x * p2.x + p2.y * p2.y, s3 = p3.x * p3.x + p3.y * p3.y;
            k.o = {(s1 * (p2.y - p3.y) + s2 * (p3.y - p1.y) + s3 * (p1.y - p2.y)) / d,
                   (s1 * (p3.x - p2.x) + s2 * (p1.x - p3.x) + s3 * (p2.x - p1.x)) / d};
            k.R = std::hypot(p1.x - k.o.x, p1.y - k.o.y);
        } else {
            throw CommandError("tangent_circle: curve " + std::to_string(i) + " is not a line, arc or circle");
        }
        cv.push_back(k);
    }
    const auto& nr = item.at("near");
    const Vec2 near{design.evaluate(nr.at(0)), design.evaluate(nr.at(1))};
    const bool typed = item.contains("radius");
    const double want = typed ? design.evaluate(item.at("radius")) : 0.0;
    if (typed && !(want > 0))
        throw CommandError("tangent_circle: the radius must be positive");
    if (cv.size() == 3 && typed)
        throw CommandError("tangent_circle: a radius cannot be given with three curves");
    const int nk = static_cast<int>(cv.size());
    // Unknowns (cx, cy, r); with a typed radius r is fixed and one equation is dropped.
    const int unknowns = typed ? 2 : 3;
    struct Sol {
        Vec2 c;
        double r;
        std::vector<int> kind;
    };
    std::vector<Sol> found;
    std::vector<int> kind(static_cast<std::size_t>(nk), 0);
    // Choices per curve: line {0: n.c - d = r, 1: = -r}; circle {0 outside, 1 new inside, 2 new around}.
    std::function<void(int)> enumerate = [&](int at) {
        if (at < nk) {
            for (int c = 0; c < (cv[static_cast<std::size_t>(at)].line ? 2 : 3); ++c) {
                kind[static_cast<std::size_t>(at)] = c;
                enumerate(at + 1);
            }
            return;
        }
        // Residuals and Jacobian rows of the nk curve equations (+ passing through `near` for two curves
        // without a radius).
        auto system = [&](double cx, double cy, double r, double* f, double (*J)[3]) {
            int row = 0;
            for (int i = 0; i < nk; ++i, ++row) {
                const Cv& k = cv[static_cast<std::size_t>(i)];
                const int ch = kind[static_cast<std::size_t>(i)];
                if (k.line) {
                    const double sg = ch == 0 ? 1.0 : -1.0;
                    f[row] = k.n.x * cx + k.n.y * cy - k.d - sg * r;
                    J[row][0] = k.n.x;
                    J[row][1] = k.n.y;
                    J[row][2] = -sg;
                } else {
                    const double dist = std::max(std::hypot(cx - k.o.x, cy - k.o.y), 1e-12);
                    const double ux = (cx - k.o.x) / dist, uy = (cy - k.o.y) / dist;
                    // outside: dist = R + r; new inside: dist = R - r; new around: dist = r - R.
                    const double target = ch == 0 ? k.R + r : ch == 1 ? k.R - r : r - k.R;
                    const double dr = ch == 0 ? -1.0 : ch == 1 ? 1.0 : -1.0;
                    f[row] = dist - target;
                    J[row][0] = ux;
                    J[row][1] = uy;
                    J[row][2] = dr;
                }
            }
            if (!typed && nk == 2) {
                const double dist = std::max(std::hypot(cx - near.x, cy - near.y), 1e-12);
                f[row] = dist - r;
                J[row][0] = (cx - near.x) / dist;
                J[row][1] = (cy - near.y) / dist;
                J[row][2] = -1.0;
            }
        };
        const int rows = typed ? nk : 3; // equations: nk curves (+ through-near)
        if (rows < unknowns)
            return;
        // Two concentric circles and a given radius: both centre equations are the same distance from
        // the common centre, so the system is singular. It has a solution only when both targets agree;
        // the centres then lie on a circle about the common centre and `near` picks the closest one.
        if (typed && nk == 2 && !cv[0].line && !cv[1].line && std::hypot(cv[0].o.x - cv[1].o.x, cv[0].o.y - cv[1].o.y) < 1e-9) {
            auto target = [&](const Cv& k, int ch) { return ch == 0 ? k.R + want : ch == 1 ? k.R - want : want - k.R; };
            const double t0 = target(cv[0], kind[0]), t1 = target(cv[1], kind[1]);
            if (std::abs(t0 - t1) < 1e-9 && t0 > 1e-9) {
                const double dx = near.x - cv[0].o.x, dy = near.y - cv[0].o.y;
                const double len = std::hypot(dx, dy);
                const double ux = len > 1e-12 ? dx / len : 1.0, uy = len > 1e-12 ? dy / len : 0.0;
                found.push_back({{cv[0].o.x + t0 * ux, cv[0].o.y + t0 * uy}, want, kind});
            }
            return;
        }
        // Starting radii and damping follow the size of the geometry, so that the result does not depend
        // on the model's units or extent (the fixed scales cover ordinary millimetre sketches).
        double extent = 0;
        for (const Cv& k : cv)
            extent = std::max(extent, k.line ? std::abs(k.n.x * near.x + k.n.y * near.y - k.d)
                                             : std::hypot(near.x - k.o.x, near.y - k.o.y) + k.R);
        std::vector<double> scales{4.0, 15.0, 60.0, 250.0};
        if (extent > 250.0)
            for (const double m : {0.25, 1.0, 4.0})
                scales.push_back(extent * m);
        for (const double scale : scales) {
            for (int sx = -1; sx <= 1; ++sx) {
                for (int sy = -1; sy <= 1; ++sy) {
                    double cx = near.x + sx * scale, cy = near.y + sy * scale;
                    double r = typed ? want : scale;
                    bool ok = false;
                    for (int it = 0; it < 60; ++it) {
                        double f[3] = {0, 0, 0};
                        double J[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
                        system(cx, cy, r, f, J);
                        double err = 0;
                        for (int i = 0; i < rows; ++i)
                            err = std::max(err, std::abs(f[i]));
                        if (err < 1e-10) {
                            ok = true;
                            break;
                        }
                        // Newton step: a square system (3x3, or 2x2 for a typed radius).
                        double m[3][4] = {};
                        for (int i = 0; i < unknowns; ++i) {
                            for (int j = 0; j < unknowns; ++j)
                                m[i][j] = J[i][j];
                            m[i][unknowns] = -f[i];
                        }
                        bool singular = false;
                        for (int col = 0; col < unknowns && !singular; ++col) {
                            int piv = col;
                            for (int q = col + 1; q < unknowns; ++q)
                                if (std::abs(m[q][col]) > std::abs(m[piv][col]))
                                    piv = q;
                            if (std::abs(m[piv][col]) < 1e-12) {
                                singular = true;
                                break;
                            }
                            for (int k = 0; k <= unknowns; ++k)
                                std::swap(m[col][k], m[piv][k]);
                            for (int q = 0; q < unknowns; ++q) {
                                if (q == col)
                                    continue;
                                const double fct = m[q][col] / m[col][col];
                                for (int k = col; k <= unknowns; ++k)
                                    m[q][k] -= fct * m[col][k];
                            }
                        }
                        if (singular)
                            break;
                        double step[3] = {m[0][unknowns] / m[0][0], m[1][unknowns] / m[1][1], unknowns == 3 ? m[2][unknowns] / m[2][2] : 0.0};
                        const double len = std::max(std::hypot(step[0], step[1]), std::abs(step[2]));
                        const double damp = len > 3 * scale ? 3 * scale / len : 1.0;
                        cx += damp * step[0];
                        cy += damp * step[1];
                        if (!typed)
                            r += damp * step[2];
                    }
                    if (!ok || !(r > 1e-9) || !std::isfinite(cx) || !std::isfinite(cy))
                        continue;
                    bool dup = false;
                    for (const auto& sol : found)
                        dup = dup || (std::hypot(sol.c.x - cx, sol.c.y - cy) < 1e-6 && std::abs(sol.r - r) < 1e-6);
                    if (!dup)
                        found.push_back({{cx, cy}, r, kind});
                }
            }
        }
    };
    enumerate(0);
    if (found.empty())
        throw CommandError("tangent_circle: no circle is tangent to these curves there");
    // Three curves, or a given radius: the centre nearest `near`; two curves through `near`: the smallest.
    const Sol* best = &found[0];
    for (const auto& sol : found) {
        const double ds = std::hypot(sol.c.x - near.x, sol.c.y - near.y), db = std::hypot(best->c.x - near.x, best->c.y - near.y);
        // Equally near centres (concentric solutions): the smaller circle.
        const bool better = (!typed && nk == 2) ? sol.r < best->r
                                                : (ds < db - 1e-9 * std::max(1.0, db) || (std::abs(ds - db) <= 1e-9 * std::max(1.0, db) && sol.r < best->r));
        if (better)
            best = &sol;
    }
    std::vector<std::pair<std::size_t, int>> sides;
    for (int i = 0; i < nk; ++i) {
        const Cv& k = cv[static_cast<std::size_t>(i)];
        const int ch = best->kind[static_cast<std::size_t>(i)];
        int side;
        if (k.line)
            side = ch == 0 ? 1 : -1; // placeholder, fixed below from the real geometry
        else
            side = ch == 0 ? 1 : ch == 1 ? -2 : -1;
        sides.emplace_back(idx[static_cast<std::size_t>(i)], side);
        if (k.line) {
            const auto& c = curves[idx[static_cast<std::size_t>(i)]];
            const Vec2 a = c.points[0], b = c.points[1];
            const double cr = (b.x - a.x) * (best->c.y - a.y) - (b.y - a.y) * (best->c.x - a.x);
            sides.back().second = cr > 0 ? 1 : -1;
        }
    }
    Json circle{{"type", "circle"}, {"center", Json::array({best->c.x, best->c.y})}, {"radius", best->r}};
    if (item.value("construction", false))
        circle["construction"] = true;
    return {circle, sides};
}

/// A circle tangent to two or three lines (Fusion: Circle > 2-Tangent / 3-Tangent): item
/// {curves: [i, j (, k)], near, radius?}, lines of the solved sketch (their extensions count).
/// With three lines the circle nearest to `near` of the (up to four) circles tangent to all of
/// them; with two, `near` picks the wedge and the size (the circle whose centre is nearest to it
/// along the wedge's bisector) unless a `radius` is given. Returns the circle item and, per line,
/// the side of it the centre lies on (the explicit tangent side, as for tangent arcs).
std::pair<Json, std::vector<std::pair<std::size_t, int>>> resolveTangentCircle(
    const cad::Design& design, const std::vector<geometry::SketchCurve>& curves, const Json& item)
{
    if (!item.contains("curves") || !item.at("curves").is_array() || !item.contains("near"))
        throw CommandError("tangent_circle: give curves [2 or 3 line, arc or circle indices] and near [x, y]");
    const auto idx = item.at("curves").get<std::vector<std::size_t>>();
    if (idx.size() != 2 && idx.size() != 3)
        throw CommandError("tangent_circle: pick 2 or 3 curves");
    {
        bool allLines = true;
        for (const std::size_t i : idx)
            allLines = allLines && i < curves.size() && curves[i].kind == geometry::SketchCurve::Kind::Line;
        if (!allLines) {
            if (std::count(idx.begin(), idx.end(), idx[0]) > 1 || (idx.size() == 3 && idx[1] == idx[2]))
                throw CommandError("tangent_circle: pick different curves");
            return resolveTangentCircleGeneral(design, curves, item, idx);
        }
    }
    struct L {
        geometry::Vec2 a, n; // a point and the unit normal
        double d;            // n . x = d on the line
    };
    std::vector<L> lines;
    for (const std::size_t i : idx) {
        if (i >= curves.size())
            throw CommandError("No curve " + std::to_string(i) + " in the sketch");
        const auto& c = curves[i];
        if (c.kind != geometry::SketchCurve::Kind::Line)
            throw CommandError("tangent_circle: curve " + std::to_string(i) + " is not a line");
        const geometry::Vec2 a = c.points[0], b = c.points[1];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-12)
            throw CommandError("tangent_circle: curve " + std::to_string(i) + " is a point");
        const geometry::Vec2 n{-(b.y - a.y) / len, (b.x - a.x) / len};
        lines.push_back({a, n, n.x * a.x + n.y * a.y});
    }
    if (std::count(idx.begin(), idx.end(), idx[0]) > 1 || (idx.size() == 3 && idx[1] == idx[2]))
        throw CommandError("tangent_circle: pick different lines");
    const auto& nr = item.at("near");
    const geometry::Vec2 near{design.evaluate(nr.at(0)), design.evaluate(nr.at(1))};
    geometry::Vec2 centre{};
    double radius = -1.0;
    double best = 1e300;
    if (lines.size() == 3) {
        // n_i . c - s_i r = d_i for the sign choices s_i: three linear equations in (cx, cy, r).
        for (int mask = 0; mask < 8; ++mask) {
            double m[3][4] = {};
            for (int i = 0; i < 3; ++i) {
                m[i][0] = lines[i].n.x;
                m[i][1] = lines[i].n.y;
                m[i][2] = (mask >> i & 1) ? 1.0 : -1.0;
                m[i][3] = lines[i].d;
            }
            // Gauss with partial pivoting.
            bool ok = true;
            for (int col = 0; col < 3 && ok; ++col) {
                int piv = col;
                for (int r = col + 1; r < 3; ++r)
                    if (std::abs(m[r][col]) > std::abs(m[piv][col]))
                        piv = r;
                if (std::abs(m[piv][col]) < 1e-9) {
                    ok = false;
                    break;
                }
                for (int k = 0; k < 4; ++k)
                    std::swap(m[col][k], m[piv][k]);
                for (int r = 0; r < 3; ++r) {
                    if (r == col)
                        continue;
                    const double f = m[r][col] / m[col][col];
                    for (int k = col; k < 4; ++k)
                        m[r][k] -= f * m[col][k];
                }
            }
            if (!ok)
                continue;
            const double cx = m[0][3] / m[0][0], cy = m[1][3] / m[1][1], r = m[2][3] / m[2][2];
            if (!(r > 1e-9))
                continue;
            const double dist = std::hypot(cx - near.x, cy - near.y);
            if (dist < best) {
                best = dist;
                centre = {cx, cy};
                radius = r;
            }
        }
        if (radius < 0)
            throw CommandError("tangent_circle: no circle is tangent to these three lines (parallel lines?)");
    } else {
        // Centre c(r) = c0 + r v for the sign choice (s1, s2): n1.c - s1 r = d1, n2.c - s2 r = d2.
        const double det = lines[0].n.x * lines[1].n.y - lines[0].n.y * lines[1].n.x;
        const bool typed = item.contains("radius");
        const double want = typed ? design.evaluate(item.at("radius")) : 0.0;
        if (typed && !(want > 0))
            throw CommandError("tangent_circle: the radius must be positive");
        if (std::abs(det) < 1e-9) {
            // Parallel lines (n2 = +-n1, so n1.x = sigma d2 on the second): the circle between
            // them, centred on the midline.
            const geometry::Vec2& n = lines[0].n;
            const double sigma = n.x * lines[1].n.x + n.y * lines[1].n.y;
            const double half = std::abs(sigma * lines[1].d - lines[0].d) / 2;
            if (half < 1e-9)
                throw CommandError("tangent_circle: the two lines are the same line");
            if (typed && std::abs(want - half) > 1e-6 * std::max(1.0, half))
                throw CommandError("tangent_circle: between these parallel lines the radius is " + std::to_string(half));
            const double mid = (lines[0].d + sigma * lines[1].d) / 2;
            const geometry::Vec2 along{-n.y, n.x};
            const double t = (near.x - n.x * mid) * along.x + (near.y - n.y * mid) * along.y;
            centre = {n.x * mid + along.x * t, n.y * mid + along.y * t};
            radius = half;
        } else {
            for (const int s1 : {-1, 1}) {
                for (const int s2 : {-1, 1}) {
                    // Solve n1.c = d1 + s1 r, n2.c = d2 + s2 r: c = c0 + r v.
                    auto solve = [&](double r1, double r2) {
                        return geometry::Vec2{(r1 * lines[1].n.y - r2 * lines[0].n.y) / det,
                                              (lines[0].n.x * r2 - lines[1].n.x * r1) / det};
                    };
                    const geometry::Vec2 c0 = solve(lines[0].d, lines[1].d);
                    const geometry::Vec2 v = solve(s1, s2);
                    const double vv = v.x * v.x + v.y * v.y;
                    double r = typed ? want : ((near.x - c0.x) * v.x + (near.y - c0.y) * v.y) / vv;
                    if (!(r > 1e-9))
                        continue;
                    const geometry::Vec2 c{c0.x + r * v.x, c0.y + r * v.y};
                    const double dist = std::hypot(c.x - near.x, c.y - near.y);
                    if (dist < best) {
                        best = dist;
                        centre = c;
                        radius = r;
                    }
                }
            }
            if (radius < 0)
                throw CommandError("tangent_circle: the click is not between the lines' sides");
        }
    }
    std::vector<std::pair<std::size_t, int>> sides;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        const auto& c = curves[idx[k]];
        const geometry::Vec2 a = c.points[0], b = c.points[1];
        const double cr = (b.x - a.x) * (centre.y - a.y) - (b.y - a.y) * (centre.x - a.x);
        sides.emplace_back(idx[k], cr > 0 ? 1 : -1);
    }
    Json circle{{"type", "circle"}, {"center", Json::array({centre.x, centre.y})}, {"radius", radius}};
    if (item.value("construction", false))
        circle["construction"] = true;
    return {circle, sides};
}

/// The ends of lines, arcs and open splines at `at` (the solver joins those ends), as
/// {curve, "start" | "end"}.
std::vector<std::pair<std::size_t, std::string>> curveEndsAt(const std::vector<geometry::SketchCurve>& curves,
                                                             geometry::Vec2 at)
{
    using K = geometry::SketchCurve::Kind;
    std::vector<std::pair<std::size_t, std::string>> out;
    for (std::size_t j = 0; j < curves.size(); ++j) {
        const auto& c = curves[j];
        // A sketch point has one position; a curve ending on it stays joined to it.
        if (c.kind == K::Point && !c.points.empty()) {
            if (std::hypot(c.points[0].x - at.x, c.points[0].y - at.y) < 1e-6)
                out.emplace_back(j, "start");
            continue;
        }
        const bool open = c.kind == K::Line || (c.kind == K::Arc && c.points.size() == 3) ||
                          (c.kind == K::Conic && c.points.size() == 3) || (c.kind == K::Spline && !c.closed);
        if (!open || c.points.size() < 2)
            continue;
        // As close as the solver's own joining (positions shown are rounded).
        if (std::hypot(c.points.front().x - at.x, c.points.front().y - at.y) < 1e-6)
            out.emplace_back(j, "start");
        if (std::hypot(c.points.back().x - at.x, c.points.back().y - at.y) < 1e-6)
            out.emplace_back(j, "end");
    }
    return out;
}

/// A tangent arc as built: its item index in the sketch's curves, its curve index, and the
/// start, through and end points it should solve to.
struct PlacedArc {
    std::size_t item;
    std::size_t curve;
    std::array<geometry::Vec2, 3> points;
    std::size_t from; ///< the curve it is tangent to (already joined by a constraint)
};

/// The solve starts from the stored geometry, so a source that constraints moved far from where it
/// was drawn drags the new, free arc away from where it was built. The stored arc is only the
/// solver's starting point: shift its points by the remaining error and solve again, until the arc
/// is where it was put or it stops getting closer (only when there is an error to remove).
void settleTangentArcs(cad::Design& d, cad::FeatureId id, Json& curves, Json& constraints,
                       const std::vector<PlacedArc>& placed)
{
    static constexpr const char* keys[] = {"start", "through", "end"};
    std::vector<double> history;
    for (int iter = 0; iter < 30 && !placed.empty(); ++iter) {
        const auto solved = d.sketches().find(id);
        if (solved == d.sketches().end())
            return;
        std::vector<std::array<geometry::Vec2, 3>> errs;
        double worst = 0;
        for (const auto& a : placed) {
            if (a.curve >= solved->second.curves.size() || solved->second.curves[a.curve].points.size() < 3)
                return;
            const auto& pts = solved->second.curves[a.curve].points;
            std::array<geometry::Vec2, 3> e;
            for (int k = 0; k < 3; ++k) {
                e[k] = {a.points[k].x - pts[k].x, a.points[k].y - pts[k].y};
                if (k != 1) // the through point only steers; the solver keeps ends and centre
                    worst = std::max(worst, std::hypot(e[k].x, e[k].y));
            }
            errs.push_back(e);
        }
        history.push_back(worst);
        if (worst < 1e-6 || (history.size() > 5 && worst > 0.99 * history[history.size() - 6]))
            return;
        if (iter == 0) {
            // Moving the stored arc breaks the joins the solver made by stored position (e.g. a
            // line drawn on from the arc's end in the same batch): keep them as constraints.
            const auto& sc = solved->second.curves;
            for (const auto& a : placed) {
                for (const auto& [k, name] : {std::pair{0, "start"}, std::pair{2, "end"}}) {
                    for (const auto& [j, other] : curveEndsAt(sc, sc[a.curve].points[k])) {
                        if (j == a.curve || (j == a.from && k == 0))
                            continue;
                        const Json join{{"type", "coincident"},
                                        {"points", Json::array({Json{{"curve", a.curve}, {"point", name}},
                                                                Json{{"curve", j}, {"point", other}}})}};
                        if (std::find(constraints.begin(), constraints.end(), join) == constraints.end())
                            constraints.push_back(join);
                    }
                }
            }
        }
        for (std::size_t i = 0; i < placed.size(); ++i) {
            Json& item = curves[placed[i].item];
            for (int k = 0; k < 3; ++k) {
                Json& p = item[keys[k]];
                p = Json::array({p.at(0).get<double>() + errs[i][k].x, p.at(1).get<double>() + errs[i][k].y});
            }
        }
        d.editFeature(id, {{"curves", curves}, {"constraints", constraints}});
    }
}

/// A new fix constraint holds the geometry where it is now (after earlier dimensions),
/// not where it was drawn: curves are expanded to their points and the solved positions
/// are stored in "at". A fixed circle also keeps its radius.
/// Symmetric on two whole items, {curves: [a, b], curve: line} (Fusion: Symmetry on curves), as the
/// point constraints it stands for: two lines get each end of `a` held symmetric to the end of `b`
/// nearer its mirror image (so a line drawn the other way is not flipped); two circles get
/// symmetric centres and equal radii; two arcs get symmetric ends (by direction), centres and
/// equal radii, so the whole arc is mirrored. Other constraints are returned unchanged.
Json resolveSymmetric(const std::vector<geometry::SketchCurve>& curves, const Json& c)
{
    if (c.value("type", std::string()) != "symmetric" || !c.contains("curves"))
        return Json::array({c});
    using K = geometry::SketchCurve::Kind;
    const auto& ids = c.at("curves");
    if (ids.size() != 2 || !c.contains("curve"))
        throw CommandError("symmetric: give two curves and the symmetry line (curve)");
    const auto a = ids[0].get<std::size_t>(), b = ids[1].get<std::size_t>(), axis = c.at("curve").get<std::size_t>();
    for (const std::size_t i : {a, b, axis})
        if (i >= curves.size())
            throw CommandError("No curve " + std::to_string(i) + " in the sketch");
    if (curves[axis].kind != K::Line)
        throw CommandError("symmetric: the symmetry curve must be a line");
    if (a == b || axis == a || axis == b)
        throw CommandError("symmetric: the two curves and the symmetry line must all be different");
    const auto ref = [](std::size_t i, const char* point) { return Json{{"curve", i}, {"point", point}}; };
    const auto sym = [&](const char* pa, const char* pb) {
        return Json{{"type", "symmetric"}, {"points", {ref(a, pa), ref(b, pb)}}, {"curve", axis}};
    };
    const K ka = curves[a].kind, kb = curves[b].kind;
    if (ka != kb || (ka != K::Line && ka != K::Circle && ka != K::Arc))
        throw CommandError("symmetric: pick two lines, two circles or two arcs");
    if (ka == K::Circle)
        return Json::array({sym("center", "center"), {{"type", "equal"}, {"curves", {a, b}}}});
    if (ka == K::Arc) {
        // A mirror image runs the other way round: with both arcs counter-clockwise (or both
        // clockwise) the start of one is the mirrored end of the other; else start to start.
        // Ends and centres symmetric (and equal radii, as Sketch Mirror) fix the whole arc.
        const auto ccw = [](const geometry::SketchCurve& k) {
            const auto& p = k.points;
            return (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x) > 0;
        };
        const bool swap = ccw(curves[a]) == ccw(curves[b]);
        return Json::array({sym("start", swap ? "end" : "start"), sym("end", swap ? "start" : "end"),
                            sym("center", "center"), {{"type", "equal"}, {"curves", {a, b}}}});
    }
    // Lines: the start of `a` mirrored about the axis, matched with the nearer end of `b`.
    const auto p0 = curves[axis].points[0], p1 = curves[axis].points[1], q = curves[a].points[0];
    const double dx = p1.x - p0.x, dy = p1.y - p0.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? ((q.x - p0.x) * dx + (q.y - p0.y) * dy) / l2 : 0.0;
    const double mx = 2 * (p0.x + t * dx) - q.x, my = 2 * (p0.y + t * dy) - q.y;
    const auto d = [&](const geometry::Vec2& v) { return std::hypot(v.x - mx, v.y - my); };
    const bool same = d(curves[b].points[0]) <= d(curves[b].points[1]);
    return Json::array({sym("start", same ? "start" : "end"), sym("end", same ? "end" : "start")});
}

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
        } else if (k.kind == K::Ellipse || k.kind == K::EllipticalArc) {
            // The whole ellipse: centre, major axis end and minor radius (and an arc's ends).
            if (k.kind == K::EllipticalArc) {
                points.push_back({{"curve", i}, {"point", "start"}});
                points.push_back({{"curve", i}, {"point", "end"}});
            }
            points.push_back({{"curve", i}, {"point", "center"}});
            points.push_back({{"curve", i}, {"point", "axis"}});
            out.push_back({{"type", "minor_radius"}, {"curve", i}, {"value", k.radius}});
        } else if (k.kind == K::Text) {
            // Its start and height point (the letters follow).
            for (const char* p : {"start", "end"})
                points.push_back({{"curve", i}, {"point", p}});
        } else if (k.kind == K::Conic) {
            // Its ends and apex (rho does not move).
            for (const char* p : {"start", "end", "apex"})
                points.push_back({{"curve", i}, {"point", p}});
        } else {
            points.push_back({{"curve", i}, {"point", "start"}});
            points.push_back({{"curve", i}, {"point", "end"}});
            // A fixed spline holds its fit points between the ends too (they can be dragged).
            for (std::size_t f = 1; k.kind == K::Spline && f + 1 < k.points.size(); ++f)
                points.push_back({{"curve", i}, {"point", "fit"}, {"index", f}});
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
           "surface or scan (surface: best-fit plane), with curves. Closed regions become profiles for "
           "extrude/revolve. Dimensions can be expressions using user parameters (parameter.set).",
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
                  // New curves join the ends as shown, not where they were first drawn.
                  d.keepSolvedSketch(id);
                  Json curves = f->params.value("curves", Json::array());
                  Json constraints = f->params.value("constraints", Json::array());
                  std::vector<PlacedArc> placed;
                  for (const auto& c : params.at("curves")) {
                      if (c.value("type", std::string()) == "tangent_circle") {
                          // Like a tangent arc, it refers to the solved lines.
                          d.editFeature(id, {{"curves", curves}});
                          const auto solved = d.sketches().find(id);
                          if (solved == d.sketches().end())
                              throw CommandError("The sketch is not evaluated");
                          const std::size_t next = solved->second.curves.size();
                          auto [circle, sides] = resolveTangentCircle(d, solved->second.curves, c);
                          curves.push_back(circle);
                          for (const auto& [line, side] : sides)
                              constraints.push_back({{"type", "tangent"}, {"curves", Json::array({next, line})}, {"side", side}});
                          continue;
                      }
                      if (c.value("type", std::string()) != "tangent_arc") {
                          curves.push_back(c);
                          continue;
                      }
                      // It refers to the solved curves: apply what came before it first (a
                      // failure later restores the design). The arc is the next curve.
                      d.editFeature(id, {{"curves", curves}});
                      const auto solved = d.sketches().find(id);
                      if (solved == d.sketches().end())
                          throw CommandError("The sketch is not evaluated");
                      const std::size_t next = solved->second.curves.size();
                      auto [arc, from, side] = resolveTangentArc(d, solved->second.curves, c);
                      auto pt = [&arc](const char* k) {
                          return geometry::Vec2{arc[k][0].get<double>(), arc[k][1].get<double>()};
                      };
                      placed.push_back({curves.size(), next, {pt("start"), pt("through"), pt("end")}, from});
                      curves.push_back(arc);
                      // The side is explicit for the same reason: the solver would pick it from
                      // the stored source, which may lie across the new centre.
                      constraints.push_back({{"type", "tangent"}, {"curves", Json::array({from, next})}, {"side", side}});
                      // Joined explicitly: the solver joins ends by their stored positions,
                      // which differ from the solved ones once dimensions have moved the source.
                      constraints.push_back({{"type", "coincident"},
                                             {"points", Json::array({Json{{"curve", from}, {"point", c.at("from").value("point", std::string("end"))}},
                                                                     Json{{"curve", next}, {"point", "start"}}})}});
                      // An end put on a curve end (as shown, i.e. solved) joins it by constraint:
                      // the solver's own joining compares stored positions.
                      const geometry::Vec2 endAt{arc["end"][0].get<double>(), arc["end"][1].get<double>()};
                      for (const auto& [j, other] : curveEndsAt(solved->second.curves, endAt))
                          constraints.push_back({{"type", "coincident"},
                                                 {"points", Json::array({Json{{"curve", next}, {"point", "end"}},
                                                                         Json{{"curve", j}, {"point", other}}})}});
                  }
                  d.editFeature(id, {{"curves", curves}, {"constraints", constraints}});
                  settleTangentArcs(d, id, curves, constraints, placed);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.delete_curves", "Delete Sketch Curves",
           "Removes curves from a sketch (Fusion: select sketch curves, Delete). curves are indices from "
           "sketch.curves. A rectangle, polygon or slot that loses some curves becomes separate lines and "
           "arcs that keep their horizontal/vertical/equal/parallel/tangent constraints. Constraints on the "
           "removed curves are removed; the other constraints are re-indexed.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves",
                          {{"type", "array"},
                           {"minItems", 1},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Curve indices as sketch.curves lists them."}}}},
                        {"sketch", "curves"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto curves = params.at("curves").get<std::vector<std::size_t>>();
              designChange(ctx, curves.size() == 1 ? "Delete Sketch Curve" : "Delete Sketch Curves",
                           [&](cad::Design& d) {
                               (void)sketchFeature(d, id);
                               d.deleteSketchCurves(id, curves);
                               return Json{};
                           });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.mirror", "Mirror Sketch Curves",
           "Mirrors sketch curves about a line of the same sketch (Fusion: Sketch > Mirror). The copies are added "
           "at the end of sketch.curves and held symmetric to the originals (symmetry constraints, equal radii), "
           "so they follow dimension changes.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"line", {{"type", "integer"}, {"description", "Index of the mirror line (sketch.curves)."}}}},
                        {"sketch", "curves", "line"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Mirror Sketch Curves", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.mirrorSketchCurves(id, params.at("curves").get<std::vector<std::size_t>>(), params.at("line").get<std::size_t>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.project", "Project",
           "Projects edges of a body onto a sketch's plane (Fusion: Sketch > Project): lines, circles and arcs "
           "parallel to the plane stay so, other edges become splines. The curves are added at the end of "
           "sketch.curves, fixed where they are, not linked to the body.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"body", bodyRefSchema("Body whose edges are projected.")},
                         {"edges", {{"type", "array"}, {"minItems", 1},
                                    {"description", "Edge indices (body.edges) or {point: [x,y,z]} near each edge."}}}},
                        {"sketch", "body", "edges"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Project", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.projectToSketch(id, requireBody(d, params.at("body")).id, params.at("edges"));
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.intersect", "Intersect",
           "Adds the curves where a body is cut by a sketch's plane (Fusion: Intersect): lines, circles and arcs stay so, other curves become splines. They are added at the "
           "end of sketch.curves, fixed where they are, not linked to the body.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"body", bodyRefSchema("Body cut by the sketch plane.")}},
                        {"sketch", "body"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Intersect", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.intersectToSketch(id, requireBody(d, params.at("body")).id);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.offset", "Sketch Offset",
           "Offsets a connected chain of sketch curves (Fusion: Sketch > Offset): `distance` > 0 outwards for a "
           "closed chain (to the left of an open one), < 0 the other way. Corners are rounded. The offset curves "
           "are added at the end of sketch.curves.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"distance", {{"type", "number"}, {"description", "Offset in mm; sign picks the side (see above)."}}}},
                        {"sketch", "curves", "distance"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Sketch Offset", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.offsetSketchCurves(id, params.at("curves").get<std::vector<std::size_t>>(), params.at("distance").get<double>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.fillet", "Sketch Fillet",
           "Rounds the corner where two sketch lines meet (Fusion: Sketch > Fillet): both lines are shortened "
           "and joined by a tangent arc with a radius dimension (edit it like any dimension). The two lines and the "
           "arc are added at the end of sketch.curves; constraints on the old lines are removed, horizontal or "
           "vertical lines stay so.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 2}, {"maxItems", 2},
                                     {"description", "The two lines (indices from sketch.curves)."}}},
                         {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fillet radius in mm."}}}},
                        {"sketch", "curves", "radius"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto curves = params.at("curves").get<std::vector<std::size_t>>();
              if (curves.size() != 2)
                  throw CommandError("Give the two lines of the corner");
              designChange(ctx, "Sketch Fillet", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.filletSketchCorner(id, curves[0], curves[1], params.at("radius").get<double>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.chamfer", "Sketch Chamfer",
           "Cuts the corner where two sketch lines meet (Fusion: Sketch > Chamfer, equal distance): both lines are "
           "shortened by `distance` from the corner and joined by a straight line. The cut-off corner stays as two "
           "construction lines with length dimensions (edit them like any dimension). The new curves are added at "
           "the end of sketch.curves (shortened lines, chamfer line, two construction lines); constraints on the "
           "old lines are removed, horizontal or vertical lines stay so.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 2}, {"maxItems", 2},
                                     {"description", "The two lines (indices from sketch.curves)."}}},
                         {"distance", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Distance from the corner along each line, mm."}}}},
                        {"sketch", "curves", "distance"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto curves = params.at("curves").get<std::vector<std::size_t>>();
              if (curves.size() != 2)
                  throw CommandError("Give the two lines of the corner");
              designChange(ctx, "Sketch Chamfer", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.chamferSketchCorner(id, curves[0], curves[1], params.at("distance").get<double>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.trim", "Trim",
           "Trims a sketch curve (Fusion: Trim, T): removes the piece of `curve` nearest to `at` (sketch "
           "coordinates) between its nearest crossings with other curves; a curve crossing nothing is removed "
           "whole. Lines, arcs and circles (a circle becomes an arc). The remaining pieces are added at the end of "
           "sketch.curves; constraints on the trimmed curve are removed.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curve", {{"type", "integer"}, {"description", "Index from sketch.curves."}}},
                         {"at", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                 {"description", "[u, v] in sketch coordinates (mm) near the piece or end to act on."}}}},
                        {"sketch", "curve", "at"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& at = params.at("at");
              designChange(ctx, "Trim", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.trimSketchCurve(id, params.at("curve").get<std::size_t>(), {at[0].get<double>(), at[1].get<double>()});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.break", "Break",
           "Breaks a sketch curve (Fusion: Break): the piece of `curve` nearest to `at` (sketch coordinates) "
           "between its nearest crossings with other curves becomes a curve of its own, as do the pieces either "
           "side; nothing is removed. Lines, arcs and circles (a circle becomes two arcs). The pieces are added "
           "at the end of sketch.curves; constraints on the curve are removed. Refused when nothing crosses it.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curve", {{"type", "integer"}, {"description", "Index from sketch.curves."}}},
                         {"at", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                 {"description", "[u, v] in sketch coordinates (mm) near the piece or end to act on."}}}},
                        {"sketch", "curve", "at"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& at = params.at("at");
              designChange(ctx, "Break", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.breakSketchCurve(id, params.at("curve").get<std::size_t>(), {at[0].get<double>(), at[1].get<double>()});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.break_preview", "Break preview",
           "The piece sketch.break (or sketch.trim) would split off at `at`, without changing the sketch: "
           "{breakable: true, kind (line / arc / elliptical_arc / spline), points (an arc: start, through, end; an elliptical "
           "arc or a spline: start, end, plus samples to draw it)}, or {breakable: false, "
           "reason}.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curve", {{"type", "integer"}, {"description", "Index from sketch.curves."}}},
                         {"at", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                 {"description", "[u, v] in sketch coordinates (mm) near the piece or end to act on."}}}},
                        {"sketch", "curve", "at"})},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& at = params.at("at");
              try {
                  const Json piece = design(ctx).sketchCurvePiece(id, params.at("curve").get<std::size_t>(),
                                                                  {at[0].get<double>(), at[1].get<double>()});
                  if (piece.is_null())
                      return Json{{"breakable", false}, {"reason", "Nothing to break at: no other curve crosses it"}};
                  if (piece.at("type") == "elliptical_arc") {
                      // Its ends and the drawn curve (samples), counter-clockwise.
                      const double cx = piece.at("center")[0].get<double>(), cy = piece.at("center")[1].get<double>();
                      const double ma = piece.at("major_radius").get<double>(), mi = piece.at("minor_radius").get<double>();
                      const double rot = piece.at("angle").get<double>() * std::numbers::pi / 180;
                      const double t0 = piece.at("start_angle").get<double>() * std::numbers::pi / 180;
                      const double t1 = piece.at("end_angle").get<double>() * std::numbers::pi / 180;
                      auto at2 = [&](double t) {
                          const double x = ma * std::cos(t), y = mi * std::sin(t);
                          return Json::array({cx + x * std::cos(rot) - y * std::sin(rot), cy + x * std::sin(rot) + y * std::cos(rot)});
                      };
                      Json samples = Json::array();
                      for (int k = 0; k <= 64; ++k)
                          samples.push_back(at2(t0 + (t1 - t0) * k / 64));
                      return Json{{"breakable", true}, {"kind", "elliptical_arc"},
                                  {"points", Json::array({at2(t0), at2(t1)})}, {"samples", samples}};
                  }
                  if (piece.at("type") == "spline") {
                      // A piece of a spline: its ends and the drawn curve (samples).
                      geometry::SketchCurve sp{geometry::SketchCurve::Kind::Spline, {}};
                      for (const auto& q : piece.at("points"))
                          sp.points.push_back({q[0].get<double>(), q[1].get<double>()});
                      Json samples = Json::array();
                      for (const auto& q : geometry::sampleSpline(sp, 16))
                          samples.push_back(Json::array({q.x, q.y}));
                      return Json{{"breakable", true}, {"kind", "spline"},
                                  {"points", Json::array({piece.at("points").front(), piece.at("points").back()})},
                                  {"samples", samples}};
                  }
                  const bool arc = piece.at("type") == "arc";
                  Json pts = arc ? Json::array({piece.at("start"), piece.at("through"), piece.at("end")})
                                 : Json::array({piece.at("start"), piece.at("end")});
                  return Json{{"breakable", true}, {"kind", arc ? "arc" : "line"}, {"points", pts}};
              } catch (const cad::DesignError& e) {
                  return Json{{"breakable", false}, {"reason", e.what()}};
              }
          });

    r.add({"sketch.extend", "Extend",
           "Extends a sketch line or arc (Fusion: Extend): the end of `curve` nearer `at` (sketch coordinates) "
           "moves along the line, or around the arc's circle, to the nearest crossing with another curve. The "
           "curve keeps its index and constraints. Refused when nothing is in the way, the end is joined to "
           "another curve, or the curve is a side of a rectangle, polygon or slot.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curve", {{"type", "integer"}, {"description", "Index from sketch.curves."}}},
                         {"at", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                 {"description", "[u, v] in sketch coordinates (mm) near the piece or end to act on."}}}},
                        {"sketch", "curve", "at"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& at = params.at("at");
              designChange(ctx, "Extend", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.extendSketchCurve(id, params.at("curve").get<std::size_t>(), {at[0].get<double>(), at[1].get<double>()});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.extend_preview", "Extend preview",
           "What sketch.extend would make, without changing the sketch: {extendable, end (start / end), kind, "
           "points} of the lengthened curve, or {extendable: false, reason}.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curve", {{"type", "integer"}, {"description", "Index from sketch.curves."}}},
                         {"at", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                 {"description", "[u, v] in sketch coordinates (mm) near the piece or end to act on."}}}},
                        {"sketch", "curve", "at"})},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& at = params.at("at");
              bool fromStart = false;
              try {
                  const auto c = design(ctx).extendedSketchCurve(id, params.at("curve").get<std::size_t>(),
                                                                 {at[0].get<double>(), at[1].get<double>()}, &fromStart);
                  Json pts = Json::array();
                  for (const auto& q : c.points)
                      pts.push_back(Json::array({q.x, q.y}));
                  return Json{{"extendable", true},
                              {"end", fromStart ? "start" : "end"},
                              {"kind", c.kind == geometry::SketchCurve::Kind::Line ? "line" : "arc"},
                              {"points", pts}};
              } catch (const cad::DesignError& e) {
                  return Json{{"extendable", false}, {"reason", e.what()}};
              }
          });

    r.add({"sketch.set_construction", "Construction",
           "Turns sketch curves into construction geometry or back (Fusion: Construction, X). Construction "
           "curves are drawn dashed and bound no profile; they still take constraints. One side of a rectangle, "
           "polygon or slot can be switched: the shape becomes lines and arcs that keep its constraints. Curve "
           "indices (sketch.curves) do not change.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"construction", {{"type", "boolean"}, {"description", "true (default): construction; false: normal."}}}},
                        {"sketch", "curves"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto curves = params.at("curves").get<std::vector<std::size_t>>();
              const bool on = params.value("construction", true);
              designChange(ctx, on ? "Construction" : "Normal Geometry", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.setSketchConstruction(id, curves, on);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.move_point", "Drag Sketch Point",
           "Drags a point of a sketch (Fusion: drag sketch geometry): the point goes to `to` (sketch "
           "coordinates) and the solver moves what its constraints leave free; dimensions and other "
           "constraints keep holding. Refused when the constraints do not let the point move there.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"point",
                          {{"type", "object"},
                           {"description", "{curve: index from sketch.curves, point: start | end | center}"}}},
                         {"to", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                 {"description", "New position, [u, v] in sketch coordinates (mm)."}}}},
                        {"sketch", "point", "to"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& to = params.at("to");
              designChange(ctx, "Drag Sketch Point", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.moveSketchPoint(id, params.at("point"), {to[0].get<double>(), to[1].get<double>()});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.move_curves", "Move/Copy Sketch Curves",
           "Moves or copies sketch curves (Fusion: Sketch > Move/Copy): by `dx`, `dy` (mm) and turned by `angle` "
           "(degrees, counterclockwise) about `pivot` (default: the middle of the curves' bounds). A move holds "
           "the curves where they go and the solver moves what they are connected to, like a drag; refused when "
           "constraints hold them. With `copy`, moved copies are added at the end of sketch.curves with the "
           "constraints among them, and the originals stay.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"dx", {{"type", "number"}, {"description", "Move along the sketch's x (mm), default 0."}}},
                         {"dy", {{"type", "number"}, {"description", "Move along the sketch's y (mm), default 0."}}},
                         {"angle", {{"type", "number"}, {"description", "Turn (degrees, counterclockwise), default 0."}}},
                         {"pivot", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                    {"description", "Centre of the turn, [u, v]; default the middle of the curves."}}},
                         {"copy", {{"type", "boolean"}, {"description", "Add moved copies instead of moving (Fusion: Create Copy)."}}}},
                        {"sketch", "curves"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto picked = params.at("curves").get<std::vector<std::size_t>>();
              const bool copy = params.value("copy", false);
              designChange(ctx, copy ? "Copy Sketch Curves" : "Move Sketch Curves", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  const geometry::Vec2 pivot = curvesPivot(d, id, picked, params, "pivot");
                  d.moveSketchCurves(id, picked,
                                     {{params.value("dx", 0.0), params.value("dy", 0.0)}, params.value("angle", 0.0), pivot},
                                     copy);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.stretch", "Sketch Stretch",
           "Stretch (Fusion): points inside `window` move by `dx`, `dy`; curves wholly inside move, those crossing "
           "its edge stretch. Refused if empty or held by constraints.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"window", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2},
                                     {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                                     {"description", "Opposite corners [[u, v], [u, v]]."}}},
                         {"dx", {{"type", "number"}, {"description", "Move along x (mm), default 0."}}},
                         {"dy", {{"type", "number"}, {"description", "Move along y (mm), default 0."}}}},
                        {"sketch", "window"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const Json& w = params.at("window");
              for (const auto& corner : w)
                  if (!corner.is_array() || corner.size() != 2 || !corner[0].is_number() || !corner[1].is_number())
                      throw CommandError("window is two corners [[u, v], [u, v]] of numbers");
              designChange(ctx, "Stretch Sketch", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.stretchSketch(id, {w.at(0).at(0).get<double>(), w.at(0).at(1).get<double>()},
                                  {w.at(1).at(0).get<double>(), w.at(1).at(1).get<double>()},
                                  {params.value("dx", 0.0), params.value("dy", 0.0)});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.scale_curves", "Sketch Scale",
           "Scales sketch curves by `factor` about `point` (Fusion: Sketch > Sketch Scale; default point: the "
           "middle of the curves' bounds). Lengths, distances and radii dimensioned among the curves scale with "
           "them (an expression becomes (expr)*factor); curves joined to them stretch, as in Move. Refused when "
           "other constraints hold the curves.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"factor", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Scale factor, e.g. 2 or 0.5."}}},
                         {"point", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                    {"description", "Point that stays, [u, v]; default the middle of the curves."}}}},
                        {"sketch", "curves", "factor"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto picked = params.at("curves").get<std::vector<std::size_t>>();
              const double factor = params.at("factor").get<double>();
              if (!(factor > 0.0) || !std::isfinite(factor))
                  throw CommandError("The scale factor must be more than 0");
              designChange(ctx, "Sketch Scale", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.moveSketchCurves(id, picked, {{0, 0}, 0.0, curvesPivot(d, id, picked, params, "point"), factor}, false);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.circular_pattern", "Sketch Circular Pattern",
           "Copies sketch curves around a centre point (Fusion: Sketch > Circular Pattern): `count` instances "
           "including the originals, spread over `angle` degrees counterclockwise (default 360: evenly around "
           "the full circle; less: the first and last instances `angle` apart). The copies are added at the end of "
           "sketch.curves with the constraints among them; they are not linked to the originals afterwards.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"center", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}},
                                     {"description", "Centre of the pattern, [u, v] in sketch coordinates."}}},
                         {"count", {{"type", "integer"}, {"minimum", 2}, {"maximum", 360},
                                    {"description", "Number of instances, the originals included."}}},
                         {"angle", {{"type", "number"}, {"description", "Total angle in degrees, default 360."}}}},
                        {"sketch", "curves", "center", "count"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const int count = params.at("count").get<int>();
              const double total = params.value("angle", 360.0);
              if (count < 2)
                  throw CommandError("A pattern needs at least 2 instances");
              if (std::abs(total) < 1e-9 || std::abs(total) > 360.0 + 1e-9)
                  throw CommandError("The pattern angle must be between 0 and 360 degrees");
              // Full circle: the last instance would land on the first.
              const bool full = std::abs(std::abs(total) - 360.0) < 1e-9;
              const double step = full ? total / count : total / (count - 1);
              const geometry::Vec2 center{params.at("center")[0].get<double>(), params.at("center")[1].get<double>()};
              std::vector<cad::SketchPlacement> places;
              for (int i = 1; i < count; ++i)
                  places.push_back({{0, 0}, step * i, center});
              designChange(ctx, "Sketch Circular Pattern", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.patternSketchCurves(id, params.at("curves").get<std::vector<std::size_t>>(), places);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.rectangular_pattern", "Sketch Rectangular Pattern",
           "Copies sketch curves in rows and columns (Fusion: Sketch > Rectangular Pattern): `count_x` instances "
           "`spacing_x` mm apart along the first direction (the sketch's x turned by `angle` degrees), `count_y` "
           "instances `spacing_y` mm apart at right angles, originals included (negative spacing goes the other "
           "way). The copies are added at the end of sketch.curves with their constraints, not linked to the "
           "originals.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")},
                         {"curves", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1},
                                     {"description", "Curve indices from sketch.curves."}}},
                         {"count_x", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000},
                                      {"description", "Instances along the first direction, default 1."}}},
                         {"spacing_x", {{"type", "number"}, {"description", "Distance between them (mm)."}}},
                         {"count_y", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000},
                                      {"description", "Instances along the second direction, default 1."}}},
                         {"spacing_y", {{"type", "number"}, {"description", "Distance between them (mm)."}}},
                         {"angle", {{"type", "number"}, {"description", "Direction of the first row (degrees from the sketch's x), default 0."}}}},
                        {"sketch", "curves"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const int nx = params.value("count_x", 1), ny = params.value("count_y", 1);
              const double sx = params.value("spacing_x", 0.0), sy = params.value("spacing_y", 0.0);
              if (nx < 1 || ny < 1 || nx * ny < 2)
                  throw CommandError("A pattern needs at least 2 instances");
              if (nx * ny > 1000)
                  throw CommandError("A pattern can have at most 1000 instances");
              if ((nx > 1 && std::abs(sx) < 1e-9) || (ny > 1 && std::abs(sy) < 1e-9))
                  throw CommandError("Give the spacing between the instances");
              const double a = params.value("angle", 0.0) * std::numbers::pi / 180.0;
              const geometry::Vec2 u{std::cos(a), std::sin(a)}, v{-std::sin(a), std::cos(a)};
              std::vector<cad::SketchPlacement> places;
              for (int j = 0; j < ny; ++j)
                  for (int i = 0; i < nx; ++i)
                      if (i != 0 || j != 0)
                          places.push_back({{u.x * sx * i + v.x * sy * j, u.y * sx * i + v.y * sy * j}, 0.0, {0, 0}});
              designChange(ctx, "Sketch Rectangular Pattern", [&](cad::Design& d) {
                  (void)sketchFeature(d, id);
                  d.patternSketchCurves(id, params.at("curves").get<std::vector<std::size_t>>(), places);
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
                                           "plane cuts the surface, in sketch coordinates: surfaceCurve, one "
                                           "simplified polyline through the picked point (read this one), and "
                                           "surfaceLine, all raw cut segments as point pairs (for drawing; "
                                           "thousands on a scan)."}}}},
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
              Json surfaceCurve = nullptr;
              if (params.value("surfaceLine", false) && onSurface.is_object() && onSurface.value("across", false) &&
                  onSurface.contains("part")) {
                  const model::Part* surface = ctx.document.part(onSurface.at("part").get<model::PartId>());
                  if (surface != nullptr) {
                      if (const model::Part* scan = ctx.document.part(model::fittedScanId(*surface));
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
                      std::vector<std::array<double, 2>> local;
                      for (const auto& p : cut) {
                          const auto uv = sk.plane.toLocal(p);
                          surfaceLine.push_back(Json::array({uv.x, uv.y}));
                          local.push_back({uv.x, uv.y});
                      }
                      const double near = std::max(box.isEmpty() ? 0.0 : 1.5 * box.diagonal(), 50.0);
                      surfaceCurve = surfaceCurveFromSegments(std::move(local), near, 60);
                  }
              }
              return Json{{"sketch", id},
                          {"surfaceLine", surfaceLine},
                          {"surfaceCurve", surfaceCurve},
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
                  // Branches (tangent side, ...) follow the shape as shown.
                  d.keepSolvedSketch(id);
                  auto solved = d.sketches().find(id);
                  for (const auto& c : params.at("constraints")) {
                      if (solved == d.sketches().end()) {
                          list.push_back(c);
                          continue;
                      }
                      for (const auto& sym : resolveSymmetric(solved->second.curves, c))
                          for (const auto& resolved : resolveFix(solved->second.curves, sym))
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
                         {"index", {{"type", "integer"}, {"minimum", 0},
                                    {"description", "Position in the constraints list of sketch.curves."}}},
                         {"value", numberOrExpr("New dimension value.")},
                         {"remove", {{"type", "boolean"}, {"description", "true deletes the constraint."}}}},
                        {"sketch", "index"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto index = params.at("index").get<std::size_t>();
              const bool remove = params.value("remove", false);
              designChange(ctx, remove ? "Delete Constraint" : "Edit Dimension", [&](cad::Design& d) {
                  Json list = sketchFeature(d, id).params.value("constraints", Json::array());
                  d.keepSolvedSketch(id);
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
           "with the remaining degrees of freedom; a spline also has samples, points along the real curve. "
           "Use the indices in sketch.add_constraints.",
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
                  static const char* kinds[] = {"line", "arc", "circle", "spline", "point", "ellipse",
                                                "elliptical_arc", "conic", "text"};
                  Json pts = Json::array();
                  for (const auto& p : c.points)
                      pts.push_back(Json::array({p.x, p.y}));
                  Json cj{{"index", i}, {"kind", kinds[static_cast<int>(c.kind)]}, {"points", pts},
                          {"construction", c.construction}};
                  if (c.kind == geometry::SketchCurve::Kind::Circle)
                      cj["radius"] = c.radius;
                  if (c.kind == geometry::SketchCurve::Kind::Ellipse ||
                      c.kind == geometry::SketchCurve::Kind::EllipticalArc) {
                      // Centre, the end of the major axis, radii and the axis angle (degrees);
                      // samples draw it (an elliptical arc: counter-clockwise from its start to
                      // its end).
                      const bool arc = c.kind == geometry::SketchCurve::Kind::EllipticalArc;
                      const geometry::Vec2 o = c.points[arc ? 1 : 0], e = c.points[arc ? 2 : 1];
                      const double major = std::hypot(e.x - o.x, e.y - o.y);
                      const double turn = std::atan2(e.y - o.y, e.x - o.x);
                      cj["center"] = Json::array({o.x, o.y});
                      cj["majorRadius"] = major;
                      cj["minorRadius"] = c.radius;
                      cj["angle"] = turn * 180 / std::numbers::pi;
                      double t0 = 0.0, span = 2 * std::numbers::pi;
                      if (arc && major > 1e-12 && c.radius > 1e-12) {
                          auto param = [&](const geometry::Vec2& p) {
                              const double dx = p.x - o.x, dy = p.y - o.y;
                              const double x = dx * std::cos(turn) + dy * std::sin(turn);
                              const double y = -dx * std::sin(turn) + dy * std::cos(turn);
                              return std::atan2(y / c.radius, x / major);
                          };
                          t0 = param(c.points[0]);
                          span = std::fmod(param(c.points[3]) - t0 + 4 * std::numbers::pi, 2 * std::numbers::pi);
                          if (span < 1e-12)
                              span = 2 * std::numbers::pi;
                      }
                      Json samples = Json::array();
                      for (int k = 0; k <= 96; ++k) {
                          const double t = t0 + span * k / 96;
                          const double u = major * std::cos(t), w = c.radius * std::sin(t);
                          samples.push_back(Json::array({o.x + u * std::cos(turn) - w * std::sin(turn),
                                                         o.y + u * std::sin(turn) + w * std::cos(turn)}));
                      }
                      cj["samples"] = samples;
                  }
                  if (c.kind == geometry::SketchCurve::Kind::Text) {
                      // points: start of the baseline, the point a height along it; outlines
                      // draw the letters.
                      cj["text"] = c.text;
                      cj["font"] = c.font;
                      cj["bold"] = c.bold;
                      cj["italic"] = c.italic;
                      cj["height"] = std::hypot(c.points[1].x - c.points[0].x, c.points[1].y - c.points[0].y);
                      cj["angle"] = std::atan2(c.points[1].y - c.points[0].y, c.points[1].x - c.points[0].x) * 180 /
                                    std::numbers::pi;
                      Json outlines = Json::array();
                      try {
                          for (const auto& line : geometry::textOutlines(c)) {
                              Json o = Json::array();
                              for (const auto& p : line)
                                  o.push_back(Json::array({p.x, p.y}));
                              outlines.push_back(o);
                          }
                      } catch (const geometry::ModelingError&) {
                          // Left out: the start and height points are drawn instead.
                      }
                      cj["outlines"] = outlines;
                  }
                  if (c.kind == geometry::SketchCurve::Kind::Conic) {
                      // points: start, apex, end; samples draw it.
                      cj["rho"] = c.radius;
                      Json samples = Json::array();
                      for (int k = 0; k <= 64; ++k) {
                          const auto p = geometry::conicPoint(c, k / 64.0);
                          samples.push_back(Json::array({p.x, p.y}));
                      }
                      cj["samples"] = samples;
                  }
                  if (c.kind == geometry::SketchCurve::Kind::Spline) {
                      cj["closed"] = c.closed;
                      // The real curve for drawing (points holds only the fit points).
                      try {
                          Json samples = Json::array();
                          for (const auto& p : geometry::sampleSpline(c))
                              samples.push_back(Json::array({p.x, p.y}));
                          cj["samples"] = samples;
                      } catch (const geometry::ModelingError&) {
                          // Left out: the fit points are drawn instead.
                      }
                  }
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
                            {"enum", {"one_side", "symmetric", "two_sides", "to_object", "all", "all_two_sides"}},
                            {"description", "one_side (default); symmetric: `distance` on both sides; two_sides: "
                                            "`distance` forwards and `distance2` backwards; to_object: up to `to`; "
                                            "all / all_two_sides: through all bodies (operation cut / intersect), one way "
                                            "or both. to_object and all take no `distance`."}}},
                {"to", {{"type", "object"},
                        {"description", "For extent to_object (Fusion: To Object): {body: id} (up to the first "
                                        "face reached), {body: id, faces: [index or {point: [x,y,z]}]} (up to "
                                        "those faces) or {part: id} (an imported surface, solid or scan). Curved "
                                        "targets are followed."}}},
                {"distance2", numberOrExpr("Backward distance for two_sides (mm).")},
                {"flip", {{"type", "boolean"}, {"description", "Extrude against the sketch normal."}}},
                {"start_offset", numberOrExpr("Start offset from the sketch plane along the extrusion direction (mm).")},
                {"taper", numberOrExpr("Taper angle in degrees (Fusion: Taper Angle), default 0: positive narrows "
                                       "the profile away from the sketch plane, negative widens it (each side with "
                                       "symmetric / two_sides). |taper| < 89; not with to_object or all.")},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch"});

    addFeatureCommand(
        r, "revolve", "Revolve",
        "Revolves sketch profiles around an axis (Fusion: Revolve).",
        merged(profileSelection(),
               {{"axis", {{"type", Json::array({"string", "object"})},
                          {"description", "\"X\", \"Y\", \"Z\" (world axes through the origin, default Z), "
                                          "{line: curve index of a sketch line}, {feature: construction axis} or "
                                          "{origin: [x,y,z], direction: [x,y,z]}."}}},
                {"extent", {{"type", "string"},
                            {"enum", {"one_side", "symmetric", "two_sides"}},
                            {"description", "one_side (default); symmetric: `angle` in total, half each side; two_sides: "
                                            "`angle` forwards, `angle2` backwards (at most 360 together)."}}},
                {"angle", numberOrExpr("Angle in degrees, default 360.")},
                {"angle2", numberOrExpr("Backward angle for two_sides (degrees).")},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch"});

    const Json patternOperation{{"type", "string"},
                                {"enum", {"join", "new_body"}},
                                {"description", "join (default): the copies are merged into the body; new_body: the "
                                                "copies become one new body."}};
    const Json directionSchema{{"type", Json::array({"string", "array"})},
                               {"description", "\"X\", \"Y\", \"Z\" or [x, y, z]."}};

    addFeatureCommand(r, "mirror", "Mirror",
                      "Mirrors a body about a plane (Fusion: Create > Mirror).",
                      {{"body", bodyRefSchema("Body to mirror.")},
                       {"plane", {{"type", Json::array({"string", "object"})},
                                  {"description", "\"XY\", \"XZ\", \"YZ\" (default), {origin, normal}, or a planar "
                                                  "face {body, index | point}."}}},
                       {"offset", numberOrExpr("Offset of the mirror plane along its normal (mm).")},
                       {"operation", patternOperation}},
                      {"body"});

    addFeatureCommand(r, "draft", "Draft",
                      "Tilts faces of a body by an angle (Fusion: Modify > Draft), e.g. the walls of a part that "
                      "must come out of a mould. Each face pivots about the line where it meets the fixed plane; "
                      "the plane's normal is the pull direction. A positive angle leans the faces in going along "
                      "the pull direction (the part narrows away from the plane), a negative one leans them out.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"plane", {{"type", Json::array({"string", "object"})},
                                  {"description", "Fixed plane: \"XY\" (default), \"XZ\", \"YZ\", {plane: construction "
                                                  "plane id or name}, {origin, normal}, or a planar face {body, index | point}."}}},
                       {"angle", numberOrExpr("Draft angle in degrees (negative leans the faces out).")}},
                      {"body", "faces", "angle"});

    addFeatureCommand(r, "split_face", "Split Face",
                      "Splits faces of a body along a plane (Fusion: Split Face); the body stays one solid, e.g. to "
                      "press-pull only part of a face.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"plane", {{"type", Json::array({"string", "object"})},
                                  {"description", "Splitting plane (as for split)."}}},
                       {"offset", numberOrExpr("Offset of the plane along its normal (mm).")}},
                      {"body", "faces"});

    addFeatureCommand(r, "replace_face", "Replace Face",
                      "Moves flat faces of a body onto a plane, surface, scan or face of another body (Fusion: Modify > "
                      "Replace Face): the body grows up to the tool or is cut back to it, sides straight along the face normal.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"to", {{"type", "object"}, {"description", "Tool: {body, faces?}, {part} or {plane, offset?}."}}}},
                      {"body", "faces", "to"});

    addFeatureCommand(r, "thread", "Thread",
                      "A modeled screw thread on cylindrical faces (Fusion: Create > Thread): a helical V groove cut along "
                      "the axis, inwards on an outer cylinder (its diameter stays the major one), outwards in a hole.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"pitch", numberOrExpr("Distance between turns (mm).")},
                       {"depth", numberOrExpr("Groove depth (mm), default 0.6134 * pitch.")},
                       {"length", numberOrExpr("Threaded length from the lower end, default the whole face.")},
                       {"handedness", {{"type", "string"}, {"enum", {"right", "left"}}, {"description", "Default right."}}},
                       {"flip", {{"type", "boolean"}, {"description", "Start at the other end."}}}},
                      {"body", "faces", "pitch"});

    addFeatureCommand(r, "delete_faces", "Delete Face",
                      "Removes faces of a body and heals the gap from the neighbouring faces (Fusion: Modify > "
                      "Delete on faces): pick all faces of a hole, fillet or boss to make it disappear, e.g. to "
                      "clean an imported part. Fails with a clear error when the neighbours cannot close the gap.",
                      {{"body", bodyRefSchema("Body.")}, {"faces", topologyListSchema("face")}},
                      {"body", "faces"});

    addFeatureCommand(r, "split", "Split Body",
                      "Cuts a body with a plane into two bodies (Fusion: Modify > Split Body), e.g. to print a "
                      "part larger than the printer bed. The body keeps what is behind the plane; a new body "
                      "gets what is in front of it (along the plane's normal). The result's id is the new body.",
                      {{"body", bodyRefSchema("Body to split.")},
                       {"plane", {{"type", Json::array({"string", "object"})},
                                  {"description", "\"XY\", \"XZ\", \"YZ\" (default), {plane: construction plane id or "
                                                  "name}, {origin, normal}, or a planar face {body, index | point}."}}},
                       {"offset", numberOrExpr("Offset of the plane along its normal (mm).")}},
                      {"body"});

    addFeatureCommand(r, "move", "Move/Rotate",
                      "Moves, rotates and/or uniformly scales a body (Fusion: Modify > Move/Copy, M): scale and "
                      "rotation about `axis` through `center` (default the body's bounding box centre) first, then "
                      "translation.",
                      {{"body", bodyRefSchema("Body to move.")},
                       {"translate", point3("Offset [dx, dy, dz] in mm.")},
                       {"axis", {{"type", Json::array({"string", "array", "object"})},
                                 {"description", "\"X\", \"Y\", \"Z\", [x, y, z] or {feature: construction axis} (turns about that line)."}}},
                       {"angle", numberOrExpr("Rotation in degrees (right-hand rule about the axis).")},
                       {"scale", numberOrExpr("Uniform scale factor about `center` (default 1).")},
                       {"center", point3("Rotation and scale centre [x, y, z] in mm.")},
                       {"copy", {{"type", "boolean"}, {"description", "Add the moved body as a copy; the original stays."}}}},
                      {"body"});

    addFeatureCommand(r, "align", "Align",
                      "Turns and moves a body so a planar face of it lies flat on a plane or planar face of another "
                      "body (Fusion: Align), faces opposing like mated parts.",
                      {{"body", bodyRefSchema("Body to move.")},
                       {"face", {{"type", Json::array({"integer", "object"})},
                                 {"description", "Planar face: index or {point}."}}},
                       {"to", {{"type", Json::array({"string", "object"})},
                               {"description", "Plane to lie on: a face {body, index | point}, a construction plane or \"XY\" / \"XZ\" / \"YZ\"."}}},
                       {"offset", numberOrExpr("Gap left between the faces (mm), default 0.")},
                       {"flip", {{"type", "boolean"}, {"description", "Same direction instead of opposing."}}},
                       {"center", {{"type", "boolean"}, {"description", "Also centre it on the target."}}}},
                      {"body", "face", "to"});

    addFeatureCommand(r, "boundary_fill", "Boundary Fill",
                      "Fills the closed spaces enclosed by tool bodies or surfaces (Fusion: Create > Boundary Fill) "
                      "with a new body, one solid per space, e.g. the gap between a part and a surface.",
                      {{"tools", {{"type", "array"}, {"minItems", 1}, {"items", {{"oneOf", Json::array({bodyRefSchema("Tool body or surface."),
                                                                    {{"type", "object"},
                                                                     {"properties", {{"part", {{"type", "integer"}, {"minimum", 1}, {"description", "Imported part id."}}}}},
                                                                     {"required", {"part"}}}})}}},
                                  {"description", "Bodies, surfaces or {part: id} that bound the spaces."}}},
                       {"cells", {{"type", "array"},
                                  {"description", "Spaces to keep: a point [x, y, z] (or {point}) inside each, or a space index; default all."}}},
                       {"removeTools", {{"type", "boolean"}, {"description", "Delete the tools afterwards."}}}},
                      {"tools"});

    addFeatureCommand(r, "scale", "Scale",
                      "Scales a body (Fusion: Modify > Scale): `factor`, or `factor_x` / `_y` / `_z` (missing: 1), about `center`.",
                      {{"body", bodyRefSchema("Body to scale.")},
                       {"factor", numberOrExpr("Uniform factor.")},
                       {"factor_x", numberOrExpr("Factor along X.")},
                       {"factor_y", numberOrExpr("Factor along Y.")},
                       {"factor_z", numberOrExpr("Factor along Z.")},
                       {"center", point3("Centre [x, y, z] (default: bounding box centre).")}},
                      {"body"});

    addFeatureCommand(r, "rectangular_pattern", "Rectangular Pattern",
                      "Copies a body in one or two directions (Fusion: Create > Pattern > Rectangular Pattern).",
                      {{"body", bodyRefSchema("Body to pattern.")},
                       {"direction", directionSchema},
                       {"count", numberOrExpr("Number of instances in the first direction, including the body.")},
                       {"spacing", numberOrExpr("Distance between instances (mm).")},
                       {"direction2", directionSchema},
                       {"count2", numberOrExpr("Number of instances in the second direction (default 1).")},
                       {"spacing2", numberOrExpr("Distance in the second direction (mm).")},
                       {"operation", patternOperation}},
                      {"body", "count", "spacing"});

    addFeatureCommand(r, "pipe", "Pipe",
                      "A round or square section swept along sketch curves (Fusion: Create > Pipe), solid or hollow.",
                      {{"path", pathSchema()},
                       {"diameter", numberOrExpr("Outer diameter (square: side), mm.")},
                       {"radius", numberOrExpr("Instead of diameter.")},
                       {"thickness", numberOrExpr("Wall, mm; 0 (default): solid.")},
                       {"section", {{"type", "string"}, {"enum", {"circular", "square"}}, {"description", "Default circular."}}},
                       {"operation", operationSchema()}},
                      {"path"});

    addFeatureCommand(r, "coil", "Coil",
                      "A spring (Fusion: Create > Coil): a round or square wire winding about an axis. Give two of "
                      "revolutions, pitch and height.",
                      {{"center", point3("Start centre [x, y, z] (default origin).")},
                       {"axis", {{"type", Json::array({"string", "array", "object"})},
                                 {"description", "X, Y, Z (default), [x, y, z], {origin, direction} or {feature}; the coil starts on an object axis."}}},
                       {"diameter", numberOrExpr("Outer diameter of the coil (mm).")},
                       {"section_size", numberOrExpr("Wire diameter (square: side), mm.")},
                       {"revolutions", numberOrExpr("Turns.")},
                       {"pitch", numberOrExpr("Rise per turn (mm); must exceed section_size.")},
                       {"height", numberOrExpr("Total rise (mm).")},
                       {"section", {{"type", "string"}, {"enum", {"circular", "square"}}, {"description", "Default circular."}}},
                       {"handedness", {{"type", "string"}, {"enum", {"right", "left"}}, {"description", "Default right."}}},
                       {"operation", operationSchema()}},
                      {"diameter", "section_size"});

    addFeatureCommand(r, "rib", "Rib",
                      "A wall on flat sketch curves (Fusion: Create > Rib). direction normal: `depth` up from the "
                      "sketch plane; parallel: `depth` along the plane, to the right of the curves' start-to-end "
                      "direction (flip: left), thickness centred on the plane.",
                      {{"path", pathSchema()},
                       {"thickness", numberOrExpr("Wall thickness (mm).")},
                       {"depth", numberOrExpr("Wall height (mm); not with extent to_object.")},
                       {"extent", {{"type", "string"}, {"enum", {"distance", "to_object"}}, {"description", "to_object: grow up to `to`."}}},
                       {"to", {{"type", "object"}, {"description", "extent to_object: {body, faces?} or {part}."}}},
                       {"direction", {{"type", "string"}, {"enum", {"normal", "parallel"}}, {"description", "Default normal."}}},
                       {"flip", {{"type", "boolean"}, {"description", "Other side."}}},
                       {"operation", operationSchema()}},
                      {"path", "thickness"});

    addFeatureCommand(r, "path_pattern", "Pattern on Path",
                      "Copies a body along sketch curves (Fusion: Pattern on Path); the body stays, copies move along the path.",
                      {{"body", bodyRefSchema("Body to pattern.")},
                       {"path", pathSchema()},
                       {"count", numberOrExpr("Instances including the body (2 to 1000).")},
                       {"distance", numberOrExpr("extent: length the instances cover (default: rest of the path); spacing: gap between instances (mm).")},
                       {"distance_type", {{"type", "string"}, {"enum", {"extent", "spacing"}}, {"description", "Meaning of distance (default extent)."}}},
                       {"start", numberOrExpr("First instance: fraction of the path, 0 (default) to below 1.")},
                       {"orientation", {{"type", "string"}, {"enum", {"identical", "path_direction"}},
                                        {"description", "path_direction turns copies with the path."}}},
                       {"flip", {{"type", "boolean"}, {"description", "Count start and distance from the other end of the path."}}},
                       {"operation", patternOperation}},
                      {"body", "path"});

    addFeatureCommand(r, "circular_pattern", "Circular Pattern",
                      "Copies a body around an axis (Fusion: Create > Pattern > Circular Pattern). A full 360 degree "
                      "angle spreads the instances evenly.",
                      {{"body", bodyRefSchema("Body to pattern.")},
                       {"axis", {{"type", Json::array({"string", "object"})},
                                 {"description", "\"X\", \"Y\", \"Z\" (default), {feature: construction axis} or {origin, direction}."}}},
                       {"count", numberOrExpr("Number of instances, including the body.")},
                       {"angle", numberOrExpr("Total angle in degrees, default 360.")},
                       {"operation", patternOperation}},
                      {"body", "count"});

    addFeatureCommand(
        r, "loft", "Loft",
        "Lofts through closed profiles of two or more sketches, in order (Fusion: Create > Loft).",
        {{"sections", {{"type", "array"},
                       {"minItems", 2},
                       {"items", {{"type", Json::array({"integer", "object"})}}},
                       {"description", "Sketch ids, {sketch, profile}; first/last may be {point: [x, y, z]} or a one-point sketch."}}},
         {"solid", {{"type", "boolean"}, {"description", "Closed solid (default) or open surface."}}},
         {"ruled", {{"type", "boolean"}, {"description", "Straight sections between profiles."}}},
         {"operation", operationSchema()},
         {"bodies", bodiesSchema()}},
        {"sections"});

    addFeatureCommand(
        r, "sweep", "Sweep",
        "Sweeps sketch profiles along a path made of sketch curves (Fusion: Create > Sweep). The path is usually "
        "drawn in a sketch perpendicular to the profile.",
        merged(profileSelection(),
               {{"path", pathSchema()},
                {"twist", numberOrExpr("Spin about the path (deg); smooth open path.")},
                {"end_scale", numberOrExpr("Size at the path end, relative to the start.")},
                {"orientation", {{"type", "string"}, {"enum", {"perpendicular", "parallel"}},
                                 {"description", "Default perpendicular; parallel keeps the profile direction."}}},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch", "path"});

    {
        const Json basePlane{{"type", Json::array({"string", "object"})},
                             {"description", "\"XY\", \"XZ\", \"YZ\", a construction plane {plane: id or name}, "
                                             "a planar face {body, point or index}, or {origin, normal, xDir}."}};
        addFeatureCommand(
            r, "plane", "Construction Plane",
            "Creates a construction plane to sketch on (sketch.create plane {plane: id}): offset (base + offset), at angle "
            "(angle about its x or y axis, or `about`), midplane (base + base2), through three points, along a path, tangent to a face.",
            {{"base", basePlane},
             {"offset", numberOrExpr("Offset along the plane normal (mm), applied after the angle.")},
             {"angle", numberOrExpr("Rotation in degrees about the base plane's axis through its origin.")},
             {"axis", {{"type", "string"}, {"enum", {"x", "y"}}, {"description", "Axis of the base plane for angle (default x)."}}},
             {"tangent", {{"type", "object"}, {"description", "Touch a curved face: {body, point} (where it touches; optional index); offset moves it along the outward normal."}}},
             {"path", pathSchema()},
             {"position", numberOrExpr("With path: 0 (start) to 1 (end) along it (default 0).")},
             {"about", {{"type", Json::array({"string", "object"})},
                        {"description", "Turn about this line instead of `axis`: X, Y, Z, {origin, direction} or {feature}."}}},
             {"base2", basePlane},
             {"points", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", point3("Point.")},
                         {"description", "Three points the plane passes through (instead of base)."}}}},
            Json::array());

        const Json topoRef{{"type", "object"}, {"description", "{body, index or point}."}};
        addFeatureCommand(
            r, "axis", "Construction Axis",
            "Creates a construction axis (Fusion: Construct > Axis); use it as {feature: id or name} in revolve, circular_pattern, "
            "move, coil, plane. Give one of from + to, line, edge (straight or circular), face (cylinder, cone, torus) or "
            "plane (perpendicular to it, through `point`; with plane2: where they meet).",
            {{"from", point3("Start point.")},
             {"to", point3("Second point.")},
             {"line", {{"type", Json::array({"string", "object"})}, {"description", "X, Y, Z, {origin, direction} or {feature}."}}},
             {"edge", topoRef},
             {"face", topoRef},
             {"plane", basePlane},
             {"plane2", basePlane},
             {"point", point3("With plane: where the axis starts.")}},
            Json::array());
    }

    addFeatureCommand(r, "fillet", "Fillet", "Rounds edges of a body with a constant radius (Fusion: Fillet, shortcut F). Tangent chains are "
                      "rounded with the picked edge; a radius that does not fit is reported with the largest that does.",
                      {{"body", bodyRefSchema("Body.")},
                       {"edges", topologyListSchema("edge")},
                       {"radius", numberOrExpr("Radius (mm); variable: at the edge start.")},
                       {"radius_end", numberOrExpr("Radius at the edge end (variable).")}},
                      {"body", "edges", "radius"});

    addFeatureCommand(r, "chamfer", "Chamfer",
                      "Bevels edges of a body (Fusion: Chamfer): equal distance (default), two distances, or a "
                      "distance and an angle. Tangent chains are bevelled with the picked edge.",
                      {{"body", bodyRefSchema("Body.")},
                       {"edges", topologyListSchema("edge")},
                       {"type", {{"type", "string"},
                                 {"enum", {"equal_distance", "two_distances", "distance_angle"}},
                                 {"description", "Chamfer type (default equal_distance)."}}},
                       {"distance", numberOrExpr("Distance (mm), on the first face for two_distances / distance_angle.")},
                       {"distance2", numberOrExpr("Distance on the other face (mm), two_distances.")},
                       {"angle", numberOrExpr("Angle to the first face in degrees, distance_angle (default 45).")},
                       {"flip", {{"type", "boolean"}, {"description", "Swap which face is the first one."}}}},
                      {"body", "edges", "distance"});

    addFeatureCommand(r, "shell", "Shell",
                      "Hollows a body, removing the selected faces and keeping inside walls of the given thickness.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"thickness", numberOrExpr("Wall thickness (mm).")}},
                      {"body", "thickness"});

    addFeatureCommand(r, "press_pull", "Press Pull",
                      "Moves faces of a body along their outward normal (Fusion: Press Pull, shortcut Q): a "
                      "positive distance adds material, a negative one removes it. A curved face is offset (Fusion's "
                      "Offset Face); a fillet face is refused (change the fillet's radius instead).",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"distance", numberOrExpr("Distance (mm) along the outward normal; negative pushes in.")}},
                      {"body", "faces", "distance"});

    addFeatureCommand(
        r, "combine", "Combine",
        "Boolean between bodies (Fusion: Combine): joins, cuts or intersects the target with the tool bodies. A tool "
        "can also be an imported part, {\"part\": id}: an exact solid (a fin, a STEP part) works like a body; a "
        "surface or a scan cuts the target on one side (Fusion: Split Body). Cut removes what is behind the surface "
        "(inside the scan, against its triangles' normals), Intersect keeps only that; flip swaps the sides. For a "
        "scan, a smooth surface is fitted to the scan around the target first, so the cut face is clean.",
        {{"target", bodyRefSchema("Target body.")},
         {"tools", {{"type", "array"},
                    {"description", "Tool bodies or {part: id}."},
                    {"items", {{"oneOf", Json::array({bodyRefSchema("Tool body."),
                                                      {{"type", "object"},
                                                       {"properties", {{"part", {{"type", "integer"}, {"minimum", 1},
                                                                                 {"description", "Imported part id (document.parts)."}}}}},
                                                       {"required", {"part"}}}})}}},
                    {"minItems", 1}}},
         {"operation", operationSchema(false)},
         {"keepTools", {{"type", "boolean"}, {"description", "Keep the tool bodies (default false). Imported parts are always kept."}}},
         {"flip", {{"type", "boolean"}, {"description", "Surface or scan tools: swap which side is removed."}}}},
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

    addFeatureCommand(r, "sphere", "Sphere", "Creates a ball (Fusion: Sphere).",
                      {{"center", point3("Centre, default origin.")},
                       {"diameter", numberOrExpr("Diameter (mm).")},
                       {"operation", operationSchema()},
                       {"bodies", bodiesSchema()}},
                      {"diameter"});

    addFeatureCommand(r, "torus", "Torus", "Creates a ring around +Z, lying in the XY plane (Fusion: Torus).",
                      {{"center", point3("Centre of the ring, default origin.")},
                       {"diameter", numberOrExpr("Diameter of the ring's centre line (mm).")},
                       {"tube_diameter", numberOrExpr("Diameter of the tube (mm); smaller than `diameter`.")},
                       {"operation", operationSchema()},
                       {"bodies", bodiesSchema()}},
                      {"diameter", "tube_diameter"});

    addFeatureCommand(r, "hole", "Hole",
                      "Drills round holes into a body (Fusion: Hole, shortcut H): one normal to the planar "
                      "face at `point`, or one at every point of `sketch` (Fusion: Placement From Sketch; "
                      "draw them with type \"point\" in sketch.add_curves), along the sketch's normal "
                      "towards the body. Through all unless `depth` is given.",
                      {{"body", bodyRefSchema("Body.")},
                       {"point", point3("Hole centre on a planar face (or give `sketch`).")},
                       {"sketch", featureIdSchema("Sketch whose points are the hole centres (instead of `point`).")},
                       {"sketch_points",
                        {{"type", "array"},
                         {"items", {{"type", "integer"}, {"minimum", 0}}},
                         {"description", "Curve indices (sketch.curves) of the sketch points to drill; default all."}}},
                       {"flip", {{"type", "boolean"}, {"description", "With `sketch`: drill away from the body side."}}},
                       {"diameter", numberOrExpr("Diameter (mm).")},
                       {"depth", numberOrExpr("Depth (mm); omit for through all.")},
                       {"through", {{"type", "boolean"}, {"description", "Through all (default when no depth)."}}},
                       {"hole_type", {{"type", "string"}, {"enum", {"simple", "counterbore", "countersink"}}, {"description", "Default simple."}}},
                       {"cbore_diameter", numberOrExpr("Counterbore diameter (mm).")},
                       {"cbore_depth", numberOrExpr("Counterbore depth (mm).")},
                       {"csink_diameter", numberOrExpr("Countersink diameter at the face (mm).")},
                       {"csink_angle", numberOrExpr("Countersink angle in degrees, default 90.")}},
                      {"body", "diameter"});

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
              // The body id of a second fin stands for the step that made it.
              const auto id = design(ctx).owningFeature(params.at("id").get<cad::FeatureId>());
              designChange(ctx, "Edit Feature", [&](cad::Design& d) {
                  // Picked edges / faces in the patch get the same matching hints as when added.
                  Json patch = params.at("params");
                  if (const auto* f = d.feature(id); f != nullptr && patch.is_object()) {
                      Json merged = f->params;
                      merged.merge_patch(patch);
                      const Json described = describeTopologyRefs(d, f->type, merged);
                      for (const char* key : {"edges", "faces", "to"})
                          if (patch.contains(key) && !patch[key].is_null() && described.contains(key))
                              patch[key] = described[key];
                  }
                  d.editFeature(id, patch);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.rename", "Rename Feature", "Renames a timeline feature.",
           objectSchema({{"id", featureIdSchema("Feature id.")}, {"name", {{"type", "string"}, {"minLength", 1}, {"description", "New name."}}}},
                        {"id", "name"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              // The body id of a second fin stands for the step that made it.
              const auto id = design(ctx).owningFeature(params.at("id").get<cad::FeatureId>());
              designChange(ctx, "Rename Feature", [&](cad::Design& d) {
                  d.renameFeature(id, params.at("name").get<std::string>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.suppress", "Suppress Feature",
           "Suppresses (skips) or unsuppresses a feature; the model regenerates without it.",
           objectSchema({{"id", featureIdSchema("Feature id.")}, {"suppressed", {{"type", "boolean"}, {"description", "true skips the feature, false restores it."}}}},
                        {"id", "suppressed"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              // The body id of a second fin stands for the step that made it.
              const auto id = design(ctx).owningFeature(params.at("id").get<cad::FeatureId>());
              const bool s = params.at("suppressed").get<bool>();
              designChange(ctx, s ? "Suppress Feature" : "Unsuppress Feature", [&](cad::Design& d) {
                  d.setSuppressed(id, s);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"timeline.move_feature", "Move Feature",
           "Moves a feature to another place in the timeline (Fusion: drag a feature in the timeline); position is "
           "its new index in timeline.list features. Refused, with the design unchanged, when a feature would then "
           "fail (moved before something it uses, or after a feature that uses it).",
           objectSchema({{"id", featureIdSchema("Feature id.")},
                         {"position", {{"type", "integer"}, {"minimum", 0}, {"description", "New index in the timeline."}}}},
                        {"id", "position"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = design(ctx).owningFeature(params.at("id").get<cad::FeatureId>());
              designChange(ctx, "Move Feature", [&](cad::Design& d) {
                  d.moveFeature(id, params.at("position").get<std::size_t>());
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"feature.delete", "Delete Feature",
           "Deletes a feature from the timeline. Features that depended on it report errors (see timeline.list).",
           objectSchema({{"id", featureIdSchema("Feature id.")}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              // The body id of a second fin stands for the step that made it.
              const auto id = design(ctx).owningFeature(params.at("id").get<cad::FeatureId>());
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
           "rolled_back), the marker position, user parameters and the resulting bodies. A feature whose surface "
           "fit missed the requested tolerance has approximate: true and says so in report.warnings.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return timelineToJson(design(ctx), ctx.document); });

    r.add({"sketch.begin_edit", "Edit Sketch",
           "Starts editing an existing sketch (Fusion: Edit Sketch): when features after it are active, the "
           "timeline is rolled back to just after the sketch, so the sketch is edited without the solids built "
           "from it. sketch.end_edit rolls forward again, to the end or to just after the last feature that was "
           "active (deleted features are skipped, rolled-back ones stay rolled back), unless the marker was moved "
           "meanwhile. Deleting the edited sketch also rolls forward. Saves and variants store the rolled-forward "
           "marker. Starting another edit ends the previous one.",
           objectSchema({{"id", {{"type", "integer"}, {"description", "The sketch feature."}}}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              const cad::Design& cur = design(ctx);
              const auto& fs = cur.features();
              const auto it = std::find_if(fs.begin(), fs.end(), [&](const cad::Feature& f) { return f.id == id; });
              if (it == fs.end())
                  throw CommandError("No feature with id " + std::to_string(id));
              const bool rollsBack = static_cast<std::size_t>(it - fs.begin()) + 1 < cur.marker();
              if (rollsBack || cur.state().sketchEdit) {
                  designChange(ctx, "Edit Sketch", [&](cad::Design& d) {
                      d.beginSketchEdit(id);
                      return Json{};
                  });
              } else if (it->type != "sketch") {
                  throw CommandError(it->name + " is not a sketch");
              }
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"sketch.end_edit", "Finish Sketch",
           "Ends the sketch edit started by sketch.begin_edit: rolls the timeline forward to where it was, unless "
           "the marker was moved meanwhile. Does nothing without a sketch edit.",
           objectSchema(Json::object()), true},
          [](CommandContext& ctx, const Json&) {
              cad::Design& d = design(ctx);
              if (d.state().sketchEdit && cad::sketchEditTarget(d.state()).value_or(d.marker()) != d.marker()) {
                  designChange(ctx, "Finish Sketch", [&](cad::Design& dd) {
                      dd.endSketchEdit();
                      return Json{};
                  });
              } else if (d.state().sketchEdit) {
                  // Nothing to roll forward: drop the edit without an undo step.
                  cad::Design::State st = d.state();
                  st.sketchEdit.reset();
                  d.restore(std::move(st));
                  d.syncDocument(ctx.document);
              }
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"timeline.set_marker", "Move Timeline Marker",
           "Moves the timeline marker (Fusion: roll back). Features after it are not evaluated and new features "
           "are inserted at it. 0 = before the first feature; omit position to move to the end.",
           objectSchema({{"position", {{"type", "integer"}, {"minimum", 0},
                                       {"description", "Number of features before the marker (default: all)."}}}}), true},
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
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}, {"description", "Parameter name, e.g. width."}}},
                         {"expression", {{"type", Json::array({"string", "number"})},
                                         {"description", "Value or expression, e.g. 40, \"30 mm\", \"width / 2\"."}}},
                         {"comment", {{"type", "string"}, {"description", "Free-text note."}}}},
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

    r.add({"parameter.evaluate", "Evaluate Expression",
           "Evaluates a number or an expression with the user parameters and units (as a dimension would) "
           "without changing the design; returns {value} in mm (or degrees, or a plain number).",
           objectSchema({{"expression", {{"type", Json::array({"string", "number"})},
                                         {"description", "Number or expression, e.g. \"width / 2 + 5 mm\"."}}}},
                        {"expression"})},
          [](CommandContext& ctx, const Json& params) {
              return Json{{"value", design(ctx).evaluate(params.at("expression"))}};
          });

    r.add({"parameter.remove", "Remove Parameter", "Removes a user parameter.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}, {"description", "Name to remove."}}}}, {"name"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto name = params.at("name").get<std::string>();
              designChange(ctx, "Remove Parameter " + name, [&](cad::Design& d) {
                  d.removeParameter(name);
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });
}

/// The design as it is just before feature `params.before` (Fusion rolls the timeline back to
/// the feature being edited), or null without `before`. Evaluated on a copy; the design and
/// its undo history are unchanged.
std::optional<cad::Design> rolledBack(const cad::Design& d, const Json& params)
{
    if (!params.contains("before"))
        return std::nullopt;
    const auto id = d.owningFeature(params.at("before").get<cad::FeatureId>());
    const auto& fs = d.features();
    const auto it = std::find_if(fs.begin(), fs.end(), [&](const cad::Feature& f) { return f.id == id; });
    if (it == fs.end())
        throw CommandError("Unknown feature " + std::to_string(id));
    const auto index = static_cast<std::size_t>(it - fs.begin());
    if (index >= d.marker())
        return std::nullopt;
    std::optional<cad::Design> copy(d);
    copy->setMarker(index);
    return copy;
}

void registerBodyCommands(CommandRegistry& r)
{
    r.add({"body.faces", "Body faces",
           "Lists the faces of a body: index, surface type, area, centre and outward normal. Use the index or "
           "the centre as {point} in sketch.create face, feature.shell faces. With {point}, also gives "
           "nearest: the face closest to it and the outward normal there (curved faces too).",
           objectSchema({{"body", bodyRefSchema("Body.")}, {"point", point3("Point on or near the body.")},
                         {"before", {{"type", "integer"}, {"minimum", 1}, {"description", "Feature id: the body as it is just before this feature (when editing it)."}}}},
                        {"body"})},
          [](CommandContext& ctx, const Json& params) {
              const auto rolled = rolledBack(design(ctx), params);
              const cad::Body& b = requireBody(rolled ? *rolled : design(ctx), params.at("body"));
              Json out = Json::array();
              for (const auto& f : geometry::faces(b.shape)) {
                  Json face{{"index", f.index},
                            {"type", f.type},
                            {"area", f.area},
                            {"center", vec(f.center)},
                            {"normal", vec(f.normal)}};
                  if (f.hasAxis)
                      face["axis"] = {{"origin", vec(f.axisOrigin)}, {"direction", vec(f.axisDirection)}};
                  out.push_back(face);
              }
              Json result{{"body", b.id}, {"name", b.name}, {"faces", out}};
              if (params.contains("point")) {
                  const auto& pt = params.at("point");
                  const core::Vec3 p{pt.at(0).get<double>(), pt.at(1).get<double>(), pt.at(2).get<double>()};
                  if (const auto idx = geometry::nearestFace(b.shape, p)) {
                      Json nearest{{"index", *idx}};
                      if (const auto n = geometry::faceNormalNear(b.shape, *idx, p))
                          nearest["normal"] = vec(*n);
                      result["nearest"] = nearest;
                  }
              }
              return result;
          });

    r.add({"body.edges", "Body edges",
           "Lists the edges of a body: index, curve type, length, start, middle and end points. Use the index "
           "or the middle as {point} in feature.fillet / feature.chamfer edges. With {point}, also gives "
           "nearest: the index of the edge closest to it.",
           objectSchema({{"body", bodyRefSchema("Body.")}, {"point", point3("Point on or near the body.")},
                         {"before", {{"type", "integer"}, {"minimum", 1}, {"description", "Feature id: the body as it is just before this feature (when editing it)."}}}},
                        {"body"})},
          [](CommandContext& ctx, const Json& params) {
              const auto rolled = rolledBack(design(ctx), params);
              const cad::Body& b = requireBody(rolled ? *rolled : design(ctx), params.at("body"));
              Json out = Json::array();
              for (const auto& e : geometry::edges(b.shape)) {
                  Json edge{{"index", e.index},
                            {"type", e.type},
                            {"length", e.length},
                            {"start", vec(e.start)},
                            {"middle", vec(e.middle)},
                            {"end", vec(e.end)}};
                  if (e.hasAxis)
                      edge["axis"] = {{"origin", vec(e.axisOrigin)}, {"direction", vec(e.axisDirection)}};
                  if (e.seam)
                      edge["seam"] = true;
                  out.push_back(edge);
              }
              Json result{{"body", b.id}, {"name", b.name}, {"edges", out}};
              if (params.contains("point")) {
                  const auto& pt = params.at("point");
                  const core::Vec3 p{pt.at(0).get<double>(), pt.at(1).get<double>(), pt.at(2).get<double>()};
                  if (const auto idx = geometry::nearestEdge(b.shape, p))
                      result["nearest"] = Json{{"index", *idx}};
              }
              return result;
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
                         {"comment", {{"type", "string"}, {"description", "Free-text note."}}}},
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
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}, {"description", "Name to remove."}}}}, {"name"}), true},
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
