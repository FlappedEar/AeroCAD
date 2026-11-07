// SPDX-License-Identifier: GPL-3.0-or-later
// Inspect commands: measure, clearance between bodies, section analysis and heat maps
// (deviation between a scan and a part, wall thickness). They read the document and set
// view state (InspectState); none of them changes the document.
#include "aerocad/commands/Inspect.hpp"
#include "aerocad/commands/Session.hpp"

#include "aerocad/cad/Design.hpp"
#include "aerocad/core/Measure.hpp"
#include "aerocad/geometry/Inspect.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace aerocad::commands {

// --- InspectState -----------------------------------------------------------------------

bool SectionAnalysis::cuts(model::PartId id) const
{
    return enabled && (parts.empty() || std::find(parts.begin(), parts.end(), id) != parts.end());
}

void InspectState::setSection(SectionAnalysis section)
{
    m_section = std::move(section);
    changed();
}

void InspectState::setHeatMap(HeatMap map)
{
    m_heatMap = std::move(map);
    changed();
}

void InspectState::clear()
{
    m_section = {};
    m_heatMap = {};
    changed();
}

void InspectState::changed()
{
    ++m_revision;
    if (m_listener)
        m_listener();
}

std::shared_ptr<const core::MeshBvh> InspectState::bvh(const std::shared_ptr<const core::TriangleMesh>& mesh)
{
    for (const auto& b : m_bvhCache)
        if (&b->mesh() == mesh.get())
            return b;
    auto built = std::make_shared<const core::MeshBvh>(mesh);
    m_bvhCache.insert(m_bvhCache.begin(), built);
    constexpr std::size_t kCacheSize = 4;
    if (m_bvhCache.size() > kCacheSize)
        m_bvhCache.resize(kCacheSize);
    return built;
}

bool InspectState::closedMesh(const std::shared_ptr<const core::TriangleMesh>& mesh)
{
    for (const auto& [m, closed] : m_closedCache)
        if (m == mesh)
            return closed;
    const bool closed = core::openEdgeCount(*mesh) == 0;
    m_closedCache.insert(m_closedCache.begin(), {mesh, closed});
    constexpr std::size_t kCacheSize = 4;
    if (m_closedCache.size() > kCacheSize)
        m_closedCache.resize(kCacheSize);
    return closed;
}

std::shared_ptr<const core::TriangleMesh> InspectState::denseMesh(const geometry::Shape& shape, double deflection)
{
    for (const auto& d : m_denseCache)
        if (d.shape.impl() == shape.impl() && d.deflection == deflection)
            return d.mesh;
    const auto box = shape.bounds();
    const double diagonal = std::max((box.max - box.min).length(), 1e-9);
    geometry::TessellationParams params;
    params.relativeLinearDeflection = deflection / diagonal;
    params.angularDeflection = 0.1;
    auto mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(shape, params));
    m_denseCache.insert(m_denseCache.begin(), Dense{shape, deflection, mesh});
    constexpr std::size_t kCacheSize = 2;
    if (m_denseCache.size() > kCacheSize)
        m_denseCache.resize(kCacheSize);
    return mesh;
}

namespace {

// --- Helpers ------------------------------------------------------------------------------

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

Json point3(const std::string& description)
{
    return Json{{"type", "array"},
                {"items", {{"type", "number"}}},
                {"minItems", 3},
                {"maxItems", 3},
                {"description", description + " [x, y, z] in mm."}};
}

Json partIdSchema(const std::string& description)
{
    return Json{{"type", "integer"}, {"minimum", 1}, {"description", description + " Part id (see document.parts)."}};
}

Json vec(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

core::Vec3 toVec(const Json& j)
{
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

/// Keeps results readable for agents: 1e-9 mm is far below any meaningful precision.
double round9(double v)
{
    return std::round(v * 1e9) / 1e9;
}

InspectState& state(CommandContext& ctx)
{
    if (ctx.inspect == nullptr)
        throw CommandError("Inspect tools are not available in this context");
    return *ctx.inspect;
}

const model::Part& requirePart(const model::Document& doc, model::PartId id)
{
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    return *p;
}

const model::Part& requireMeshPart(const model::Document& doc, model::PartId id)
{
    const model::Part& p = requirePart(doc, id);
    if (!p.mesh || p.mesh->isEmpty())
        throw CommandError("Part '" + p.name + "' has no surface to measure");
    return p;
}

/// The curves of a sketch part as edges: exact lines, arcs and circles on the sketch plane,
/// or, for a sketch laid onto a surface, the drawn (draped) polylines.
std::shared_ptr<const geometry::Shape> sketchShape(CommandContext& ctx, const model::Part& part)
{
    if (ctx.design != nullptr) {
        const auto& sketches = ctx.design->sketches();
        if (const auto it = sketches.find(part.featureId); it != sketches.end() && !it->second.curves.empty()) {
            const cad::Feature* f = ctx.design->feature(part.featureId);
            const bool draped = f != nullptr && f->params.contains("onSurface") &&
                                !f->params.at("onSurface").value("across", false);
            if (!draped) {
                try {
                    auto shape = std::make_shared<const geometry::Shape>(
                        geometry::curveEdges(it->second.plane, it->second.curves));
                    if (!shape->isNull())
                        return shape;
                } catch (const geometry::ModelingError&) {
                    // Fall back to the drawn lines.
                }
            }
        }
    }
    if (part.edgeSegments.size() < 2)
        throw CommandError("Sketch '" + part.name + "' has no curves to measure");
    try {
        return std::make_shared<const geometry::Shape>(geometry::polylineEdges(part.edgeSegments));
    } catch (const geometry::ModelingError& err) {
        throw CommandError(err.what());
    }
}

// --- Measure entities ----------------------------------------------------------------------

/// A resolved measurement selection.
struct Entity {
    std::string kind; ///< point, vertex, edge, face, body
    const model::Part* part = nullptr;
    geometry::SubShape sub;           ///< For B-rep vertex/edge/face/body.
    bool exact = false;               ///< B-rep sub-shape (otherwise point or mesh body).
    const geometry::Shape* shape = nullptr; ///< The part's shape, or `owned` for sketches.
    std::shared_ptr<const geometry::Shape> owned;
    core::Vec3 point{};               ///< The picked / given point.
};

Json entitySchema(const std::string& description)
{
    return Json{
        {"type", "object"},
        {"description",
         description +
             " Either a free point {point}, or a part selection {part, ...}: {part, point, pick?} picks the "
             "vertex, edge or face of a B-rep part under a surface point (pick: auto = vertex/edge within "
             "tolerance, else face; point = the point itself; body = the whole part), {part, face | edge | "
             "vertex} selects by index (see feature.faces / feature.edges), {part} alone is the whole body. "
             "Mesh parts (scans, STL) have no faces or edges: picks on them are points, or the body."},
        {"properties",
         {{"part", partIdSchema("Part to measure on.")},
          {"point", point3("Point on the part surface (e.g. a click) or a free point.")},
          {"pick", {{"type", "string"}, {"enum", {"auto", "point", "vertex", "edge", "face", "body"}}}},
          {"tolerance", {{"type", "number"}, {"exclusiveMinimum", 0},
                         {"description", "Pick radius for vertices and edges (mm). Default: 1% of the part size."}}},
          {"face", {{"type", "integer"}, {"minimum", 0}}},
          {"edge", {{"type", "integer"}, {"minimum", 0}}},
          {"vertex", {{"type", "integer"}, {"minimum", 0}}}}},
        {"additionalProperties", false}};
}

Entity resolveEntity(CommandContext& ctx, const Json& j, const char* name)
{
    Entity e;
    if (!j.contains("part")) {
        if (!j.contains("point"))
            throw CommandError(std::string(name) + ": give a point or a part");
        e.kind = "point";
        e.point = toVec(j["point"]);
        return e;
    }
    e.part = &requirePart(ctx.document, j["part"].get<model::PartId>());
    if (e.part->shape && !e.part->shape->isNull()) {
        e.shape = &*e.part->shape;
    } else if (e.part->kind == model::PartKind::Sketch) {
        // Sketches are measured on their curves (lines, arcs, circles), as drawn.
        e.owned = sketchShape(ctx, *e.part);
        e.shape = e.owned.get();
    }
    const bool brep = e.shape != nullptr;

    auto byIndex = [&](const char* key, geometry::SubShape::Kind kind) {
        if (!brep)
            throw CommandError(std::string(name) + ": part '" + e.part->name + "' is a mesh without " + key + "s");
        e.kind = key;
        e.sub = {kind, j[key].get<std::size_t>()};
        e.exact = true;
        const auto props = geometry::subShapeProperties(*e.shape, e.sub);
        e.point = props.point;
    };
    for (const auto& [key, kind] : {std::pair{"face", geometry::SubShape::Kind::Face},
                                     std::pair{"edge", geometry::SubShape::Kind::Edge},
                                     std::pair{"vertex", geometry::SubShape::Kind::Vertex}}) {
        if (j.contains(key)) {
            byIndex(key, kind);
            return e;
        }
    }

    const std::string pick = j.value("pick", j.contains("point") ? "auto" : "body");
    if (pick == "body" || !j.contains("point")) {
        if (pick != "body")
            throw CommandError(std::string(name) + ": pick '" + pick + "' needs a point");
        e.kind = "body";
        e.sub = {};
        e.exact = brep;
        if (j.contains("point"))
            e.point = toVec(j["point"]);
        else if (e.part->mesh)
            e.point = e.part->mesh->bounds().center();
        return e;
    }
    e.point = toVec(j["point"]);
    if (pick == "point" || !brep) {
        e.kind = "point";
        return e;
    }
    const geometry::Shape& shape = *e.shape;
    if (pick == "auto") {
        const double tol = j.contains("tolerance") ? j["tolerance"].get<double>()
                                                   : std::max(shape.bounds().diagonal() * 0.01, 1e-6);
        const auto sub = geometry::pickSubShape(shape, e.point, tol);
        if (!sub)
            throw CommandError(std::string(name) + ": nothing to pick on '" + e.part->name + "'");
        e.sub = *sub;
    } else {
        const auto kind = pick == "vertex" ? geometry::SubShape::Kind::Vertex
                          : pick == "edge" ? geometry::SubShape::Kind::Edge
                                           : geometry::SubShape::Kind::Face;
        // Nearest sub-shape of the requested kind (infinite tolerance).
        std::optional<std::size_t> index;
        if (kind == geometry::SubShape::Kind::Face)
            index = geometry::nearestFace(shape, e.point);
        else if (kind == geometry::SubShape::Kind::Edge)
            index = geometry::nearestEdge(shape, e.point);
        else {
            double best = 0.0;
            for (const auto& v : geometry::vertices(shape)) {
                const double d = (v.point - e.point).length();
                if (!index || d < best) {
                    index = v.index;
                    best = d;
                }
            }
        }
        if (!index)
            throw CommandError(std::string(name) + ": part '" + e.part->name + "' has no " + pick);
        e.sub = {kind, *index};
    }
    e.kind = geometry::subShapeKindName(e.sub.kind);
    e.exact = true;
    return e;
}

Json entityToJson(const Entity& e)
{
    Json j{{"kind", e.kind}};
    if (e.part != nullptr) {
        j["part"] = e.part->id;
        j["partName"] = e.part->name;
    }
    if (e.exact) {
        if (e.sub.kind != geometry::SubShape::Kind::Whole)
            j["index"] = e.sub.index;
        const auto props = geometry::subShapeProperties(*e.shape, e.sub);
        j["type"] = props.type;
        if (props.area > 0)
            j["area"] = round9(props.area);
        if (props.length > 0)
            j["length"] = round9(props.length);
        if (props.volume > 0)
            j["volume"] = round9(props.volume);
        if (props.radius > 0)
            j["radius"] = round9(props.radius);
        j["position"] = vec(props.point);
    } else if (e.kind == "body" && e.part != nullptr && e.part->mesh) {
        j["type"] = "mesh";
        j["area"] = round9(e.part->mesh->surfaceArea());
    } else {
        j["position"] = vec(e.point);
    }
    return j;
}

/// Minimum distance between two entities and the closest points.
struct Separation {
    double distance = 0.0;
    core::Vec3 a{};
    core::Vec3 b{};
    std::string method; ///< exact, mesh
};

Separation separation(CommandContext& ctx, const Entity& a, const Entity& b)
{
    const bool pointA = a.kind == "point";
    const bool pointB = b.kind == "point";
    if (pointA && pointB)
        return {(b.point - a.point).length(), a.point, b.point, "exact"};
    try {
        if (pointA && b.exact) {
            const auto d = geometry::distance(a.point, *b.shape, b.sub);
            return {d.distance, d.pointA, d.pointB, "exact"};
        }
        if (pointB && a.exact) {
            const auto d = geometry::distance(b.point, *a.shape, a.sub);
            return {d.distance, d.pointB, d.pointA, "exact"};
        }
        if (a.exact && b.exact) {
            const auto d = geometry::distance(*a.shape, a.sub, *b.shape, b.sub);
            return {d.distance, d.pointA, d.pointB, "exact"};
        }
    } catch (const geometry::ModelingError& err) {
        throw CommandError(err.what());
    }
    // A mesh body is involved: surface distances on the display meshes.
    InspectState& st = state(ctx);
    if (pointA || pointB) {
        const Entity& p = pointA ? a : b;
        const Entity& body = pointA ? b : a;
        const auto hit = st.bvh(body.part->mesh)->closestPoint(p.point);
        if (!hit)
            throw CommandError("Part '" + body.part->name + "' has no surface");
        return pointA ? Separation{hit->distance, p.point, hit->point, "mesh"}
                      : Separation{hit->distance, hit->point, p.point, "mesh"};
    }
    // A selected face / edge / vertex of a B-rep against a mesh body: measure from that
    // sub-shape only (sampled), not from its whole part.
    // A whole sketch counts too: its display mesh is the profile fill, not the curves.
    const bool subA = a.exact && (a.sub.kind != geometry::SubShape::Kind::Whole || a.owned);
    const bool subB = b.exact && (b.sub.kind != geometry::SubShape::Kind::Whole || b.owned);
    if (subA || subB) {
        const Entity& sub = subA ? a : b;
        const Entity& body = subA ? b : a;
        const auto meshBvh = st.bvh(requireMeshPart(ctx.document, body.part->id).mesh);
        std::optional<core::SurfaceDistance> best; // pointA on the sub-shape, pointB on the mesh
        try {
            const double step = std::max(sub.shape->bounds().diagonal() * 0.002, 1e-4);
            for (const core::Vec3& p : geometry::subShapePoints(*sub.shape, sub.sub, step)) {
                const auto hit = meshBvh->closestPoint(p, best ? best->distance : std::numeric_limits<double>::infinity());
                if (hit && (!best || hit->distance < best->distance))
                    best = core::SurfaceDistance{hit->distance, p, hit->point};
            }
            // Faces: also project the mesh vertices onto the face.
            if (sub.sub.kind == geometry::SubShape::Kind::Face) {
                const core::MeshBvh faceBvh(std::make_shared<const core::TriangleMesh>(
                    geometry::subShapeMesh(*sub.shape, sub.sub)));
                for (const core::Vec3& v : body.part->mesh->vertices()) {
                    const auto hit = faceBvh.closestPoint(v, best ? best->distance : std::numeric_limits<double>::infinity());
                    if (hit && (!best || hit->distance < best->distance))
                        best = core::SurfaceDistance{hit->distance, hit->point, v};
                }
            }
        } catch (const geometry::ModelingError& err) {
            throw CommandError(err.what());
        }
        if (!best)
            throw CommandError("Cannot measure between empty selections");
        return subA ? Separation{best->distance, best->pointA, best->pointB, "mesh"}
                    : Separation{best->distance, best->pointB, best->pointA, "mesh"};
    }
    const auto d = core::minimumDistance(*st.bvh(requireMeshPart(ctx.document, a.part->id).mesh),
                                         *st.bvh(requireMeshPart(ctx.document, b.part->id).mesh));
    if (!d)
        throw CommandError("Cannot measure between empty parts");
    return {d->distance, d->pointA, d->pointB, "mesh"};
}

/// Wall thickness under a single selection: a face of a solid (exact) or a point on a
/// closed mesh (STL / 3MF part). Open meshes such as scans have no thickness.
std::optional<geometry::ShapeDistance> thicknessAt(CommandContext& ctx, const Entity& e)
{
    if (e.part == nullptr)
        return std::nullopt;
    if (e.exact && e.part->shape && e.sub.kind == geometry::SubShape::Kind::Face) {
        try {
            return geometry::wallThickness(*e.part->shape, e.sub, e.point);
        } catch (const geometry::ModelingError&) {
            return std::nullopt;
        }
    }
    if (e.kind != "point" || e.part->shape || e.part->kind == model::PartKind::Sketch || !e.part->mesh ||
        e.part->mesh->isEmpty())
        return std::nullopt;
    InspectState& st = state(ctx);
    if (!st.closedMesh(e.part->mesh))
        return std::nullopt;
    const auto bvh = st.bvh(e.part->mesh);
    const auto at = bvh->closestPoint(e.point);
    if (!at)
        return std::nullopt;
    const core::Vec3 inward = e.part->mesh->faceNormal(at->triangle) * -1.0;
    if (inward.length() == 0.0)
        return std::nullopt;
    const double eps = std::max(e.part->mesh->bounds().diagonal() * 1e-7, 1e-9);
    const auto hit = bvh->raycast(at->point + inward * eps, inward);
    if (!hit)
        return std::nullopt;
    return geometry::ShapeDistance{hit->t + eps, at->point, hit->point};
}

double angleDegrees(const core::Vec3& a, const core::Vec3& b)
{
    const double c = std::clamp(a.normalized().dot(b.normalized()), -1.0, 1.0);
    return std::acos(c) * 180.0 / std::numbers::pi;
}

/// Angle between two planar faces / straight edges / axes of round faces and circles
/// (Fusion Measure: angle between the selections), or std::nullopt.
std::optional<double> entityAngle(const Entity& a, const Entity& b)
{
    if (!a.exact || !b.exact)
        return std::nullopt;
    const auto da = geometry::subShapeDirection(*a.shape, a.sub);
    const auto db = geometry::subShapeDirection(*b.shape, b.sub);
    if (!da || !db)
        return std::nullopt;
    const double theta = angleDegrees(da->vector, db->vector);
    if (da->isPlaneNormal && db->isPlaneNormal)
        return theta; // dihedral between face normals, 0..180
    if (!da->isPlaneNormal && !db->isPlaneNormal)
        return std::min(theta, 180.0 - theta); // lines are unoriented, 0..90
    return std::abs(90.0 - theta);              // line vs plane, 0..90
}

Json measureClearance(CommandContext& ctx, const model::Part& a, const model::Part& b)
{
    auto bodyEntity = [&ctx](const model::Part& p) {
        Entity e;
        e.kind = "body";
        e.part = &p;
        if (p.shape && !p.shape->isNull()) {
            e.shape = &*p.shape;
        } else if (p.kind == model::PartKind::Sketch) {
            e.owned = sketchShape(ctx, p); // its curves, not the profile fill
            e.shape = e.owned.get();
        }
        e.exact = e.shape != nullptr;
        return e;
    };
    const Entity ea = bodyEntity(a);
    const Entity eb = bodyEntity(b);
    if (!ea.exact)
        requireMeshPart(ctx.document, a.id);
    if (!eb.exact)
        requireMeshPart(ctx.document, b.id);
    const Separation s = separation(ctx, ea, eb);
    Json out{{"distance", round9(s.distance)},
             {"pointA", vec(s.a)},
             {"pointB", vec(s.b)},
             {"method", s.method}};
    if (ea.exact && eb.exact && ea.shape->solidCount() > 0 && eb.shape->solidCount() > 0) {
        double volume = 0.0;
        try {
            volume = geometry::interferenceVolume(*ea.shape, *eb.shape);
        } catch (const geometry::ModelingError& err) {
            throw CommandError(err.what());
        }
        out["interference"] = volume > 1e-9;
        out["interferenceVolume"] = round9(volume);
    } else {
        // Mesh parts: touching or crossing surfaces show as zero distance; full
        // containment is not detected.
        out["interference"] = nullptr;
    }
    return out;
}

// --- Section --------------------------------------------------------------------------------

core::CutPlane sectionPlane(const CommandContext& ctx, const Json& params)
{
    core::CutPlane plane;
    const core::BoundingBox box = ctx.document.bounds();
    const core::Vec3 center = box.center();
    if (params.contains("plane")) {
        const std::string name = params["plane"].get<std::string>();
        // Default cut directions show the section in the matching standard view:
        // XY from Top, XZ from Front, YZ from Right.
        const core::Vec3 n = name == "xy" ? core::Vec3{0, 0, 1} : name == "xz" ? core::Vec3{0, -1, 0} : core::Vec3{1, 0, 0};
        const double offset = params.contains("offset")
                                  ? params["offset"].get<double>()
                                  : (name == "xy" ? center.z : name == "xz" ? center.y : center.x);
        const core::Vec3 axis{std::abs(n.x), std::abs(n.y), std::abs(n.z)};
        plane.origin = axis * offset;
        plane.normal = n;
    } else {
        plane.origin = params.contains("origin") ? toVec(params["origin"]) : center;
        plane.normal = params.contains("normal") ? toVec(params["normal"]) : core::Vec3{0, 0, 1};
        if (params.contains("offset"))
            plane.origin = plane.origin + plane.normal.normalized() * params["offset"].get<double>();
    }
    if (plane.normal.length() < 1e-12)
        throw CommandError("Section normal must not be zero");
    plane.normal = plane.normal.normalized();
    if (params.value("flip", false))
        plane.normal = plane.normal * -1.0;
    return plane;
}

Json sectionResult(const CommandContext& ctx, const SectionAnalysis& s, bool includeCurves, std::size_t maxPoints)
{
    Json parts = Json::array();
    double totalArea = 0.0;
    for (const auto& p : ctx.document.parts()) {
        if (!p.visible || !p.mesh || p.mesh->isEmpty() || !s.cuts(p.id) || p.kind == model::PartKind::Sketch)
            continue;
        const auto curves = core::sectionCurves(*p.mesh, s.plane);
        if (curves.empty())
            continue;
        std::size_t closed = 0;
        double length = 0.0;
        for (const auto& c : curves) {
            closed += c.closed ? 1 : 0;
            length += c.length();
        }
        const double area = core::sectionArea(curves, s.plane);
        totalArea += area;
        Json part{{"id", p.id},
                  {"name", p.name},
                  {"kind", p.kind == model::PartKind::Body ? "body" : "imported"},
                  {"curves", curves.size()},
                  {"closedCurves", closed},
                  {"length", round9(length)},
                  {"area", round9(area)}};
        if (includeCurves) {
            Json list = Json::array();
            std::size_t budget = maxPoints;
            for (const auto& c : curves) {
                // Evenly subsample long curves so the reply stays small.
                const std::size_t n = c.points.size();
                const std::size_t take = std::min(n, std::max<std::size_t>(2, budget / std::max<std::size_t>(1, curves.size())));
                Json pts = Json::array();
                for (std::size_t i = 0; i < take; ++i)
                    pts.push_back(vec(c.points[take <= 1 ? 0 : i * (n - 1) / (take - 1)]));
                list.push_back({{"closed", c.closed}, {"points", pts}, {"pointCount", n}});
                budget -= std::min(budget, take);
            }
            part["polylines"] = list;
        }
        parts.push_back(part);
    }
    return Json{{"enabled", s.enabled},
                {"origin", vec(s.plane.origin)},
                {"normal", vec(s.plane.normal)},
                {"cap", s.cap},
                {"parts", parts},
                {"area", round9(totalArea)}};
}

// --- Heat maps ------------------------------------------------------------------------------

Json statsJson(const core::ValueStats& s, const core::TriangleMesh& mesh)
{
    Json j{{"count", s.count},
           {"min", round9(s.min)},
           {"max", round9(s.max)},
           {"mean", round9(s.mean)},
           {"rms", round9(s.rms)},
           {"stddev", round9(s.stddev)}};
    if (s.count > 0) {
        j["minAt"] = vec(mesh.vertices()[s.minIndex]);
        j["maxAt"] = vec(mesh.vertices()[s.maxIndex]);
    }
    return j;
}

/// Share of values in [lo, hi] and a histogram over the colour range.
void addDistribution(Json& out, const std::vector<double>& values, double lo, double hi, std::size_t bins)
{
    std::vector<std::size_t> counts(bins, 0);
    std::size_t finite = 0;
    std::size_t below = 0;
    std::size_t above = 0;
    for (const double v : values) {
        if (!std::isfinite(v))
            continue;
        ++finite;
        if (v < lo) {
            ++below;
            continue;
        }
        if (v > hi) {
            ++above;
            continue;
        }
        const double t = hi > lo ? (v - lo) / (hi - lo) : 0.5;
        counts[std::min(bins - 1, static_cast<std::size_t>(t * static_cast<double>(bins)))]++;
    }
    out["histogram"] = {{"min", round9(lo)}, {"max", round9(hi)}, {"counts", counts}, {"below", below}, {"above", above}};
    out["noValue"] = values.size() - finite;
}

Json heatMapJson(const HeatMap& h)
{
    if (!h.enabled)
        return Json{{"enabled", false}};
    Json j{{"enabled", true},
           {"kind", h.kind == HeatMap::Kind::Deviation ? "deviation" : "thickness"},
           {"part", h.part},
           {"rangeMin", round9(h.rangeMin)},
           {"rangeMax", round9(h.rangeMax)}};
    if (h.kind == HeatMap::Kind::Deviation)
        j["reference"] = h.reference;
    if (h.mesh)
        j["stats"] = statsJson(h.stats, *h.mesh);
    return j;
}

void registerMeasure(CommandRegistry& r)
{
    r.add({"inspect.measure", "Measure",
           "Measures one or two selections like Fusion's Inspect > Measure: with one selection its "
           "properties (position, length, area, radius, volume) and, on a face of a solid or a point of a "
           "closed mesh, the wall thickness there (thickness: value, pointA on the surface, pointB on the "
           "opposite wall); with two the minimum distance, the closest points, the X/Y/Z deltas and, for "
           "planar faces, straight edges and the axes of round faces and circles, the angle in degrees. "
           "Exact on B-rep parts and sketch curves; on meshes (scans) distances are to the triangle surface.",
           objectSchema({{"a", entitySchema("First selection.")}, {"b", entitySchema("Second selection (optional).")}},
                        {"a"})},
          [](CommandContext& ctx, const Json& params) {
              const Entity a = resolveEntity(ctx, params["a"], "a");
              Json out{{"a", entityToJson(a)}};
              if (!params.contains("b")) {
                  if (const auto t = thicknessAt(ctx, a))
                      out["thickness"] = Json{{"value", round9(t->distance)},
                                              {"pointA", vec(t->pointA)},
                                              {"pointB", vec(t->pointB)}};
                  return out;
              }
              const Entity b = resolveEntity(ctx, params["b"], "b");
              out["b"] = entityToJson(b);
              const Separation s = separation(ctx, a, b);
              out["distance"] = round9(s.distance);
              out["pointA"] = vec(s.a);
              out["pointB"] = vec(s.b);
              out["delta"] = vec({round9(s.b.x - s.a.x), round9(s.b.y - s.a.y), round9(s.b.z - s.a.z)});
              out["method"] = s.method;
              if (const auto angle = entityAngle(a, b))
                  out["angle"] = round9(*angle);
              return out;
          });

    r.add({"inspect.clearance", "Clearance",
           "Minimum clearance between two parts and the closest points. For two B-rep solids also reports "
           "interference (overlap) and the overlapping volume; for meshes (scans) the distance is computed "
           "between the triangle surfaces (0 = touching or crossing).",
           objectSchema({{"a", partIdSchema("First part.")}, {"b", partIdSchema("Second part.")}}, {"a", "b"})},
          [](CommandContext& ctx, const Json& params) {
              const auto ida = params["a"].get<model::PartId>();
              const auto idb = params["b"].get<model::PartId>();
              if (ida == idb)
                  throw CommandError("Clearance needs two different parts");
              return measureClearance(ctx, requirePart(ctx.document, ida), requirePart(ctx.document, idb));
          });
}

void registerSection(CommandRegistry& r)
{
    r.add({"inspect.section", "Section Analysis",
           "Shows a section view: the parts are cut by a plane and the cut is capped (Fusion's Inspect > "
           "Section Analysis). The normal points to the half that is removed. Give either plane (xy, xz, yz; "
           "offset = coordinate along its axis, default the model centre) or origin + normal (offset moves "
           "along the normal). Returns per part the section curves, their length and the enclosed area, so "
           "scan and part sections can be compared. enabled: false turns the section off.",
           objectSchema({{"enabled", {{"type", "boolean"}, {"description", "Default true."}}},
                         {"plane", {{"type", "string"}, {"enum", {"xy", "xz", "yz"}}}},
                         {"origin", point3("Point on the plane.")},
                         {"normal", point3("Plane normal, towards the removed half.")},
                         {"offset", {{"type", "number"}, {"description", "mm, see the description."}}},
                         {"flip", {{"type", "boolean"}, {"description", "Reverse the normal (keep the other half)."}}},
                         {"parts", {{"type", "array"}, {"items", partIdSchema("Part to cut.")},
                                    {"description", "Parts to cut; default all."}}},
                         {"cap", {{"type", "boolean"}, {"description", "Fill closed sections (default true)."}}},
                         {"includeCurves", {{"type", "boolean"}, {"description", "Return the section polylines (subsampled)."}}},
                         {"maxPoints", {{"type", "integer"}, {"minimum", 2}, {"description", "Point budget for includeCurves (default 2000)."}}}})},
          [](CommandContext& ctx, const Json& params) {
              InspectState& st = state(ctx);
              SectionAnalysis s;
              s.enabled = params.value("enabled", true);
              if (s.enabled) {
                  s.plane = sectionPlane(ctx, params);
                  s.cap = params.value("cap", true);
                  if (params.contains("parts")) {
                      for (const auto& id : params["parts"]) {
                          requirePart(ctx.document, id.get<model::PartId>());
                          s.parts.push_back(id.get<model::PartId>());
                      }
                  }
              }
              st.setSection(s);
              if (!s.enabled)
                  return Json{{"enabled", false}};
              return sectionResult(ctx, s, params.value("includeCurves", false),
                                   params.value("maxPoints", std::size_t{2000}));
          });
}

void registerHeatMaps(CommandRegistry& r)
{
    r.add({"inspect.deviation", "Deviation Map",
           "Colours a part (usually a scan) by its signed distance to a reference part (usually the designed "
           "body): positive outside the reference, negative inside. Returns min/max/mean/RMS, where the "
           "extremes are, the share within tolerance and a histogram. The map stays on until inspect.clear.",
           objectSchema({{"measured", partIdSchema("Part to colour (e.g. the scan).")},
                         {"reference", partIdSchema("Reference part (e.g. the CAD body).")},
                         {"range", {{"type", "number"}, {"exclusiveMinimum", 0},
                                    {"description", "Colour scale +/- range (mm). Default: the largest deviation."}}},
                         {"tolerance", {{"type", "number"}, {"exclusiveMinimum", 0},
                                        {"description", "Report the share of points within +/- tolerance (mm)."}}},
                         {"maxDistance", {{"type", "number"}, {"exclusiveMinimum", 0},
                                          {"description", "Ignore points farther than this (mm)."}}}},
                        {"measured", "reference"})},
          [](CommandContext& ctx, const Json& params) {
              InspectState& st = state(ctx);
              const auto measuredId = params["measured"].get<model::PartId>();
              const auto referenceId = params["reference"].get<model::PartId>();
              if (measuredId == referenceId)
                  throw CommandError("Deviation needs two different parts");
              const model::Part& measured = requireMeshPart(ctx.document, measuredId);
              const model::Part& reference = requireMeshPart(ctx.document, referenceId);
              const double maxDistance = params.value("maxDistance", std::numeric_limits<double>::infinity());
              // An exact reference (a fitted surface, a body) is measured on a dense
              // triangulation: a twentieth of the tolerance, or 0.02 % of its size, at
              // least 1 micron and at most 2e-5 of its size (bounded triangle count).
              std::shared_ptr<const core::TriangleMesh> referenceMesh = reference.mesh;
              if (reference.shape && !reference.shape->isNull()) {
                  const auto box = reference.shape->bounds();
                  const double diagonal = (box.max - box.min).length();
                  double deflection = 2e-4 * diagonal;
                  if (params.contains("tolerance"))
                      deflection = std::min(deflection, 0.05 * params["tolerance"].get<double>());
                  deflection = std::max({deflection, 2e-5 * diagonal, 1e-3});
                  referenceMesh = st.denseMesh(*reference.shape, deflection);
              }
              auto values = std::make_shared<std::vector<double>>(
                  core::signedVertexDistances(*measured.mesh, *st.bvh(referenceMesh), maxDistance));
              if (reference.shape && !reference.shape->isNull()) {
                  // Near sewn edges the triangulation can be off the exact surface: the
                  // largest values are measured on the shape itself.
                  const double threshold =
                      params.contains("tolerance") ? 0.02 * params["tolerance"].get<double>() : 0.0;
                  (void)geometry::refineShapeDistances(*reference.shape, measured.mesh->vertices(), *values, threshold,
                                                       20000);
              }

              HeatMap h;
              h.enabled = true;
              h.kind = HeatMap::Kind::Deviation;
              h.part = measuredId;
              h.reference = referenceId;
              h.mesh = measured.mesh;
              h.referenceMesh = reference.mesh;
              h.stats = core::valueStats(*values);
              if (h.stats.count == 0)
                  throw CommandError("No point of '" + measured.name + "' is within maxDistance of '" + reference.name + "'");
              const double range = params.contains("range")
                                       ? params["range"].get<double>()
                                       : std::max({std::abs(h.stats.min), std::abs(h.stats.max), 1e-6});
              h.rangeMin = -range;
              h.rangeMax = range;
              h.values = values;
              h.params = params;

              Json out = heatMapJson(h);
              addDistribution(out, *values, h.rangeMin, h.rangeMax, 10);
              if (params.contains("tolerance")) {
                  const double tol = params["tolerance"].get<double>();
                  std::size_t within = 0;
                  for (const double v : *values)
                      within += std::isfinite(v) && std::abs(v) <= tol ? 1 : 0;
                  out["tolerance"] = tol;
                  out["withinTolerance"] = round9(100.0 * static_cast<double>(within) / static_cast<double>(h.stats.count));
              }
              st.setHeatMap(std::move(h));
              return out;
          });

    r.add({"inspect.thickness", "Thickness Map",
           "Colours a part by its wall thickness: the distance from each surface point straight inwards to "
           "the opposite wall. Red = thin. Points where the ray leaves the part (open meshes) get no value. "
           "Returns min/max/mean, where the thinnest wall is and a histogram.",
           objectSchema({{"id", partIdSchema("Part to analyse.")},
                         {"min", {{"type", "number"}, {"minimum", 0}, {"description", "Colour scale start (red), mm. Default: thinnest wall."}}},
                         {"max", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Colour scale end (blue), mm. Default: thickest wall."}}},
                         {"maxThickness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Ignore walls thicker than this (mm)."}}}},
                        {"id"})},
          [](CommandContext& ctx, const Json& params) {
              InspectState& st = state(ctx);
              const auto id = params["id"].get<model::PartId>();
              const model::Part& part = requireMeshPart(ctx.document, id);
              const double maxT = params.value("maxThickness", std::numeric_limits<double>::infinity());
              auto values = std::make_shared<std::vector<double>>(core::vertexThickness(*st.bvh(part.mesh), maxT));

              HeatMap h;
              h.enabled = true;
              h.kind = HeatMap::Kind::Thickness;
              h.part = id;
              h.mesh = part.mesh;
              h.stats = core::valueStats(*values);
              if (h.stats.count == 0)
                  throw CommandError("Cannot measure the wall thickness of '" + part.name + "' (open surface?)");
              h.rangeMin = params.value("min", h.stats.min);
              h.rangeMax = params.value("max", h.stats.max);
              if (h.rangeMax <= h.rangeMin)
                  h.rangeMax = h.rangeMin + 1e-6;
              h.values = values;
              h.params = params;

              Json out = heatMapJson(h);
              addDistribution(out, *values, h.rangeMin, h.rangeMax, 10);
              st.setHeatMap(std::move(h));
              return out;
          });
}

void registerState(CommandRegistry& r)
{
    r.add({"inspect.state", "Inspect state",
           "The active analyses: section plane (with per-part section results) and heat map.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              InspectState& st = state(ctx);
              Json section = st.section().enabled ? sectionResult(ctx, st.section(), false, 0) : Json{{"enabled", false}};
              return Json{{"section", section}, {"heatMap", heatMapJson(st.heatMap())}};
          });

    r.add({"inspect.clear", "Clear analyses",
           "Turns off the section view and/or the heat map.",
           objectSchema({{"what", {{"type", "string"}, {"enum", {"all", "section", "heatmap"}}, {"description", "Default all."}}}})},
          [](CommandContext& ctx, const Json& params) {
              InspectState& st = state(ctx);
              const std::string what = params.value("what", "all");
              if (what == "section")
                  st.setSection({});
              else if (what == "heatmap")
                  st.setHeatMap({});
              else
                  st.clear();
              return Json{{"cleared", what}};
          });
}

} // namespace

void registerInspectCommands(CommandRegistry& registry)
{
    registerMeasure(registry);
    registerSection(registry);
    registerHeatMaps(registry);
    registerState(registry);
}

} // namespace aerocad::commands
