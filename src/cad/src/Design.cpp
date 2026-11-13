// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/Design.hpp"

#include "aerocad/cad/Expression.hpp"
#include "aerocad/cad/SketchSolver.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <sstream>
#include <utility>

namespace aerocad::cad {

using geometry::Plane;
using geometry::SketchCurve;
using geometry::Vec2;

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr int kMaxParameterDepth = 32;

const Json& required(const Json& params, const char* key)
{
    auto it = params.find(key);
    if (it == params.end() || it->is_null())
        throw DesignError(std::string("Missing parameter '") + key + "'");
    return *it;
}

bool has(const Json& params, const char* key)
{
    auto it = params.find(key);
    return it != params.end() && !it->is_null();
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isReservedName(const std::string& n)
{
    static const std::set<std::string> reserved{"pi", "mm", "cm", "m", "in", "deg", "sqrt", "sin",
                                                "cos", "tan", "abs", "min", "max"};
    return reserved.contains(n);
}

bool isValidParameterName(const std::string& n)
{
    if (n.empty() || std::isdigit(static_cast<unsigned char>(n[0])))
        return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

} // namespace

geometry::Shape Design::scanSurfaceUnder(const model::Part& scan, const core::BoundingBox& body)
{
    // The scan around the body: the body's box grown by a margin, so the fitted surface
    // reaches past the body on every side.
    const core::Vec3 size = body.max - body.min;
    const double margin = 0.15 * size.length() + 3.0;
    core::BoundingBox region;
    region.extend(body.min - core::Vec3{margin, margin, margin});
    region.extend(body.max + core::Vec3{margin, margin, margin});
    for (const auto& c : m_scanSurfaces) {
        if (c.part == scan.id && c.revision == scan.revision && (c.region.min - region.min).length() < 1e-6 &&
            (c.region.max - region.max).length() < 1e-6)
            return c.face;
    }
    const auto& vertices = scan.mesh->vertices();
    auto inside = [&](const core::Vec3& v) {
        return v.x >= region.min.x && v.x <= region.max.x && v.y >= region.min.y && v.y <= region.max.y &&
               v.z >= region.min.z && v.z <= region.max.z;
    };
    core::TriangleSet near;
    const auto& triangles = scan.mesh->triangles();
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const auto& t = triangles[i];
        if (inside(vertices[t[0]]) && inside(vertices[t[1]]) && inside(vertices[t[2]]))
            near.push_back(static_cast<std::uint32_t>(i));
    }
    if (near.size() < 10)
        throw DesignError("The scan '" + scan.name + "' does not reach the body");
    constexpr std::size_t maxTriangles = 200000;
    if (near.size() > maxTriangles)
        throw DesignError("The scan around the body is too dense to fit (" + std::to_string(near.size()) +
                          " triangles, at most " + std::to_string(maxTriangles) +
                          "); reduce it first (Scan > Simplify)");
    geometry::SurfaceFitResult fit;
    try {
        fit = geometry::fitSurface(*scan.mesh, near);
    } catch (const geometry::SurfaceFitError& e) {
        throw DesignError(std::string("Could not fit a surface to the scan under the body: ") + e.what());
    }
    if (fit.face.isNull())
        throw DesignError("Could not fit a surface to the scan under the body");
    if (m_scanSurfaces.size() >= 8)
        m_scanSurfaces.erase(m_scanSurfaces.begin());
    m_scanSurfaces.push_back({scan.id, scan.revision, region, fit.face});
    return fit.face;
}

std::string_view toString(FeatureStatus::State state)
{
    switch (state) {
    case FeatureStatus::State::Ok: return "ok";
    case FeatureStatus::State::Error: return "error";
    case FeatureStatus::State::Suppressed: return "suppressed";
    case FeatureStatus::State::RolledBack: return "rolled_back";
    }
    return "unknown";
}

const std::vector<std::string>& Design::featureTypes()
{
    static const std::vector<std::string> types{"sketch", "extrude", "revolve", "fillet", "chamfer",
                                                "shell",  "combine", "box",     "cylinder", "sphere", "torus", "base_body", "hole",
                                                "mirror",  "rectangular_pattern", "circular_pattern", "path_pattern", "pipe", "rib", "coil", "loft", "sweep", "plane", "axis",
                                                "part_on_surface", "fin_on_surface", "move", "scale", "press_pull", "align", "boundary_fill", "split", "split_face", "replace_face", "delete_faces", "draft", "thread",
                                                "fit_selection", "fit_regions", "trim_by_sketch", "offset_surface", "thicken_surface"};
    return types;
}

// ---------------------------------------------------------------------------------------
// Parameters and expressions

std::optional<double> Design::parameterValue(std::string_view name) const
{
    auto it = std::find_if(m_state.parameters.begin(), m_state.parameters.end(),
                           [&](const Parameter& p) { return p.name == name; });
    if (it == m_state.parameters.end())
        return std::nullopt;
    thread_local int depth = 0;
    if (depth > kMaxParameterDepth)
        throw ExpressionError("Parameter '" + std::string(name) + "' refers to itself (cycle)");
    ++depth;
    struct Guard {
        ~Guard() { --depth; }
    } guard;
    return evaluateExpression(it->expression, [this](std::string_view n) { return parameterValue(n); });
}

double Design::evaluate(const Json& value) const
{
    if (value.is_number())
        return value.get<double>();
    if (value.is_string())
        return evaluateExpression(value.get<std::string>(), [this](std::string_view n) { return parameterValue(n); });
    throw DesignError("Expected a number or an expression, got " + value.dump());
}

void Design::setParameter(const std::string& name, const std::string& expression, const std::string& comment)
{
    if (!isValidParameterName(name) || isReservedName(name))
        throw DesignError("Invalid parameter name '" + name + "'");
    const State before = m_state;
    auto it = std::find_if(m_state.parameters.begin(), m_state.parameters.end(),
                           [&](const Parameter& p) { return p.name == name; });
    if (it == m_state.parameters.end())
        m_state.parameters.push_back({name, expression, comment});
    else
        *it = {name, expression, comment.empty() ? it->comment : comment};
    try {
        // Validates syntax, names and cycles of this and all dependent parameters.
        for (const auto& p : m_state.parameters)
            (void)parameterValue(p.name);
    } catch (const std::exception& e) {
        m_state = before;
        throw DesignError(e.what());
    }
    regenerate();
}

void Design::removeParameter(const std::string& name)
{
    const auto n = std::erase_if(m_state.parameters, [&](const Parameter& p) { return p.name == name; });
    if (n == 0)
        throw DesignError("No parameter '" + name + "'");
    regenerate();
}

// ---------------------------------------------------------------------------------------
// Timeline editing

const Feature* Design::feature(FeatureId id) const
{
    auto it = std::find_if(m_state.features.begin(), m_state.features.end(), [id](const Feature& f) { return f.id == id; });
    return it == m_state.features.end() ? nullptr : &*it;
}

FeatureId Design::owningFeature(FeatureId id) const
{
    return feature(id) == nullptr && feature(featureOfBody(id)) != nullptr ? featureOfBody(id) : id;
}

std::size_t Design::indexOf(FeatureId id) const
{
    // A body id of a feature with several bodies stands for that feature.
    const FeatureId want = owningFeature(id);
    for (std::size_t i = 0; i < m_state.features.size(); ++i) {
        if (m_state.features[i].id == want)
            return i;
    }
    throw DesignError("No feature with id " + std::to_string(id));
}

std::string Design::defaultName(const std::string& type) const
{
    std::string base = type == "part_on_surface" ? "PartOnSurface" : type == "fin_on_surface" ? "Fin"
                       : type == "press_pull"     ? "PressPull"
                       : type == "split"          ? "Split"
                       : type == "split_face"     ? "SplitFace"
                       : type == "replace_face"   ? "ReplaceFace"
                       : type == "boundary_fill"  ? "BoundaryFill"
                       : type == "delete_faces"   ? "DeleteFace"
                       : type == "fit_selection"  ? "FitSurface"
                       : type == "fit_regions"    ? "FitPatches"
                       : type == "trim_by_sketch" ? "TrimSurface"
                       : type == "offset_surface" ? "OffsetSurface"
                       : type == "thicken_surface" ? "ThickenSurface"
                                                  : type;
    base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
    for (int n = 1;; ++n) {
        const std::string candidate = base + std::to_string(n);
        if (std::none_of(m_state.features.begin(), m_state.features.end(),
                         [&](const Feature& f) { return f.name == candidate; }))
            return candidate;
    }
}

void Design::restore(State state)
{
    m_state = std::move(state);
    regenerate();
}

void Design::regenerateChecked(FeatureId id, const State& rollback)
{
    regenerate();
    const FeatureStatus st = status(id);
    if (st.state == FeatureStatus::State::Error) {
        m_state = rollback;
        regenerate();
        throw DesignError(st.message);
    }
}

FeatureId Design::addFeature(std::string type, Json params, std::string name)
{
    const auto& types = featureTypes();
    if (std::find(types.begin(), types.end(), type) == types.end())
        throw DesignError("Unknown feature type '" + type + "'");
    if (!params.is_object())
        throw DesignError("Feature parameters must be an object");
    const State before = m_state;
    Feature f;
    f.id = m_state.nextId++;
    f.name = name.empty() ? defaultName(type) : std::move(name);
    f.type = std::move(type);
    f.params = std::move(params);
    const FeatureId id = f.id;
    m_state.features.insert(m_state.features.begin() + static_cast<std::ptrdiff_t>(m_state.marker), std::move(f));
    ++m_state.marker;
    regenerateChecked(id, before);
    return id;
}

void Design::editFeature(FeatureId id, const Json& patch)
{
    if (!patch.is_object())
        throw DesignError("Feature parameters must be an object");
    const State before = m_state;
    Feature& f = m_state.features[indexOf(id)];
    f.params.merge_patch(patch);
    // A newly picked revolve axis or sweep path replaces one marked as deleted (a merge
    // patch would keep the marker). A path given without curves uses all of its sketch.
    if (patch.contains("axis") && patch["axis"].is_object() && !patch["axis"].contains("deletedLine") &&
        f.params["axis"].is_object())
        f.params["axis"].erase("deletedLine");
    if (patch.contains("path") && patch["path"].is_object() && !patch["path"].contains("deletedCurves") &&
        f.params["path"].is_object() && f.params["path"].contains("deletedCurves")) {
        f.params["path"].erase("deletedCurves");
        if (!patch["path"].contains("curves"))
            f.params["path"].erase("curves");
    }
    // Newly picked sketch points (or a new placement) for a hole replace deleted ones.
    if ((patch.contains("sketch_points") || patch.contains("sketch") || patch.contains("point")) &&
        !patch.contains("deletedSketchPoints"))
        f.params.erase("deletedSketchPoints");
    regenerateChecked(id, before);
}

void Design::renameFeature(FeatureId id, std::string name)
{
    if (name.empty())
        throw DesignError("Feature name must not be empty");
    m_state.features[indexOf(id)].name = std::move(name);
    regenerate();
}

void Design::setSuppressed(FeatureId id, bool suppressed)
{
    m_state.features[indexOf(id)].suppressed = suppressed;
    regenerate();
}

void Design::moveFeature(FeatureId id, std::size_t position)
{
    const std::size_t from = indexOf(id);
    auto& fs = m_state.features;
    if (position >= fs.size())
        throw DesignError("Position " + std::to_string(position) + " is past the end of the timeline (" +
                          std::to_string(fs.size()) + " features)");
    if (position == from)
        return;
    const State before = m_state;
    std::vector<FeatureId> working;
    for (const auto& f : fs) {
        if (status(f.id).state == FeatureStatus::State::Ok)
            working.push_back(f.id);
    }
    Feature moved = std::move(fs[from]);
    fs.erase(fs.begin() + static_cast<std::ptrdiff_t>(from));
    fs.insert(fs.begin() + static_cast<std::ptrdiff_t>(position), std::move(moved));
    if (from < m_state.marker && position >= m_state.marker)
        --m_state.marker;
    else if (from >= m_state.marker && position < m_state.marker)
        ++m_state.marker;
    regenerate();
    for (const FeatureId w : working) {
        const FeatureStatus st = status(w);
        if (st.state != FeatureStatus::State::Error)
            continue;
        const Feature* f = feature(w);
        const std::string name = f ? f->name : std::to_string(w);
        m_state = before;
        regenerate();
        throw DesignError("Cannot move it there: " + name + " would fail (" + st.message + ")");
    }
}

void Design::removeFeature(FeatureId id)
{
    const std::size_t i = indexOf(id);
    // Deleting the edited sketch ends its edit: features after it that were active come back.
    const bool editedSketch = m_state.sketchEdit && m_state.sketchEdit->sketch == id && sketchEditTarget(m_state);
    m_state.features.erase(m_state.features.begin() + static_cast<std::ptrdiff_t>(i));
    if (i < m_state.marker)
        --m_state.marker;
    if (editedSketch) {
        const SketchEdit& e = *m_state.sketchEdit;
        std::size_t target = m_state.marker;
        if (e.atEnd) {
            target = m_state.features.size();
        } else {
            for (std::size_t k = 0; k < m_state.features.size(); ++k) {
                if (std::find(e.active.begin(), e.active.end(), m_state.features[k].id) != e.active.end())
                    target = std::max(target, k + 1);
            }
        }
        m_state.marker = target;
    }
    if (m_state.sketchEdit && m_state.sketchEdit->sketch == id)
        m_state.sketchEdit.reset();
    regenerate();
}

void Design::setMarker(std::size_t marker)
{
    if (marker > m_state.features.size())
        throw DesignError("Marker position " + std::to_string(marker) + " is past the end of the timeline (" +
                          std::to_string(m_state.features.size()) + " features)");
    m_state.marker = marker;
    m_state.sketchEdit.reset();
    regenerate();
}

bool Design::beginSketchEdit(FeatureId sketch)
{
    const std::size_t index = indexOf(sketch);
    if (m_state.features[index].type != "sketch")
        throw DesignError(m_state.features[index].name + " is not a sketch");
    if (m_state.sketchEdit && m_state.sketchEdit->sketch == sketch && sketchEditTarget(m_state))
        return false;
    const std::size_t before = m_state.marker;
    m_state = committedState(std::move(m_state));
    const std::size_t i = indexOf(sketch);
    if (i + 1 < m_state.marker) {
        SketchEdit e;
        e.sketch = sketch;
        e.atEnd = m_state.marker == m_state.features.size();
        for (std::size_t k = i + 1; k < m_state.marker; ++k)
            e.active.push_back(m_state.features[k].id);
        m_state.sketchEdit = std::move(e);
        m_state.marker = i + 1;
    }
    if (m_state.marker == before)
        return false;
    regenerate();
    return true;
}

bool Design::endSketchEdit()
{
    const std::size_t before = m_state.marker;
    m_state = committedState(std::move(m_state));
    if (m_state.marker == before)
        return false;
    regenerate();
    return true;
}

std::optional<std::size_t> sketchEditTarget(const Design::State& state)
{
    if (!state.sketchEdit)
        return std::nullopt;
    const Design::SketchEdit& e = *state.sketchEdit;
    const auto& fs = state.features;
    const auto it = std::find_if(fs.begin(), fs.end(), [&](const Feature& f) { return f.id == e.sketch; });
    if (it == fs.end())
        return std::nullopt;
    const auto sketchAt = static_cast<std::size_t>(it - fs.begin());
    if (state.marker != sketchAt + 1)
        return std::nullopt;
    if (e.atEnd)
        return fs.size();
    std::size_t target = sketchAt + 1;
    for (std::size_t k = 0; k < fs.size(); ++k) {
        if (std::find(e.active.begin(), e.active.end(), fs[k].id) != e.active.end())
            target = std::max(target, k + 1);
    }
    return target;
}

Design::State committedState(Design::State state)
{
    if (const auto target = sketchEditTarget(state); target && *target > state.marker)
        state.marker = *target;
    state.sketchEdit.reset();
    return state;
}

FeatureStatus Design::status(FeatureId id) const
{
    auto it = m_statuses.find(id);
    return it == m_statuses.end() ? FeatureStatus{FeatureStatus::State::Error, "Unknown feature", nullptr} : it->second;
}

// ---------------------------------------------------------------------------------------
// Regeneration

namespace {

/// Evaluation helpers bound to one design.
struct Eval {
    const Design& design;

    double num(const Json& v) const { return design.evaluate(v); }
    double num(const Json& params, const char* key) const { return num(required(params, key)); }
    double num(const Json& params, const char* key, double fallback) const
    {
        return has(params, key) ? num(params.at(key)) : fallback;
    }

    Vec2 vec2(const Json& v) const
    {
        if (!v.is_array() || v.size() != 2)
            throw DesignError("Expected a 2D point [u, v], got " + v.dump());
        return {num(v[0]), num(v[1])};
    }
    Vec2 vec2(const Json& params, const char* key) const { return vec2(required(params, key)); }

    core::Vec3 vec3(const Json& v) const
    {
        if (!v.is_array() || v.size() != 3)
            throw DesignError("Expected a 3D point [x, y, z], got " + v.dump());
        return {num(v[0]), num(v[1]), num(v[2])};
    }
    core::Vec3 vec3(const Json& params, const char* key, core::Vec3 fallback) const
    {
        return has(params, key) ? vec3(params.at(key)) : fallback;
    }
};

/// Finds the edge or face a reference means after earlier features changed the body.
/// References carry the picked point and, when picked through the UI or commands, the
/// point relative to the body's bounding box plus the edge direction / face normal and
/// type. The relative point follows the body when dimensions change (a box grows: the
/// picked corner edge moves with it); direction and type break ties between neighbours.
std::optional<std::size_t> matchTopology(const geometry::Shape& shape, const Json& ref, bool edgesWanted,
                                         const Eval& ev)
{
    core::Vec3 point{};
    const auto box = shape.bounds();
    const core::Vec3 size = box.size();
    if (ref.contains("relative")) {
        const core::Vec3 rel = ev.vec3(ref.at("relative"));
        point = {box.min.x + rel.x * size.x, box.min.y + rel.y * size.y, box.min.z + rel.z * size.z};
    } else {
        point = ev.vec3(ref.at("point"));
    }
    const bool hasHints = ref.contains("direction") || ref.contains("normal") || ref.contains("kind");
    if (!hasHints)
        return edgesWanted ? geometry::nearestEdge(shape, point) : geometry::nearestFace(shape, point);
    const double diag = std::max(1e-9, size.length());
    const std::string kind = ref.value("kind", std::string());
    std::optional<std::size_t> best;
    double bestScore = 0.0;
    auto consider = [&](std::size_t i, double dist, const std::string& type, double alignment) {
        if (!std::isfinite(dist))
            return;
        double score = dist + 0.5 * diag * (1.0 - alignment);
        if (!kind.empty() && type != kind)
            score += 0.25 * diag;
        if (!best || score < bestScore - 1e-9) {
            best = i;
            bestScore = score;
        }
    };
    if (edgesWanted) {
        const auto info = geometry::edges(shape);
        const auto dist = geometry::edgeDistances(shape, point);
        const core::Vec3 want = ref.contains("direction") ? ev.vec3(ref.at("direction")).normalized() : core::Vec3{};
        for (std::size_t i = 0; i < info.size() && i < dist.size(); ++i) {
            double alignment = 1.0;
            if (ref.contains("direction") && info[i].type == "line")
                alignment = std::abs((info[i].end - info[i].start).normalized().dot(want));
            consider(i, dist[i], info[i].type, alignment);
        }
    } else {
        const auto info = geometry::faces(shape);
        const auto dist = geometry::faceDistances(shape, point);
        const core::Vec3 want = ref.contains("normal") ? ev.vec3(ref.at("normal")).normalized() : core::Vec3{};
        for (std::size_t i = 0; i < info.size() && i < dist.size(); ++i) {
            const double alignment = ref.contains("normal") ? (info[i].normal.dot(want) + 1.0) / 2.0 : 1.0;
            consider(i, dist[i], info[i].type, alignment);
        }
    }
    return best;
}

SketchCurve lineCurve(Vec2 a, Vec2 b, bool construction)
{
    SketchCurve c{SketchCurve::Kind::Line, {a, b}};
    c.construction = construction;
    return c;
}

SketchPointRef parsePointRef(const Json& j)
{
    if (j.is_string() && lower(j.get<std::string>()) == "origin")
        return {SketchPointRef::Kind::Origin, 0};
    if (!j.is_object())
        throw DesignError("Constraint point must be \"origin\" or {curve, point}, got " + j.dump());
    SketchPointRef ref;
    ref.curve = required(j, "curve").get<std::size_t>();
    const std::string which = lower(j.value("point", std::string("start")));
    if (which == "start")
        ref.kind = SketchPointRef::Kind::Start;
    else if (which == "end")
        ref.kind = SketchPointRef::Kind::End;
    else if (which == "center")
        ref.kind = SketchPointRef::Kind::Center;
    else if (which == "axis")
        ref.kind = SketchPointRef::Kind::Axis;
    else if (which == "apex")
        ref.kind = SketchPointRef::Kind::Apex;
    else if (which == "fit") {
        ref.kind = SketchPointRef::Kind::Fit;
        ref.index = required(j, "index").get<std::size_t>();
    } else
        throw DesignError("Unknown constraint point '" + which + "' (start, end, center, axis, apex, fit)");
    return ref;
}

SketchConstraint parseConstraint(const Eval& ev, const Json& j)
{
    if (!j.is_object())
        throw DesignError("Sketch constraint must be an object");
    SketchConstraint c;
    try {
        c.type = constraintType(lower(required(j, "type").get<std::string>()));
    } catch (const geometry::ModelingError& e) {
        throw DesignError(e.what());
    }
    if (has(j, "curve"))
        c.curves.push_back(j.at("curve").get<std::size_t>());
    if (has(j, "curves"))
        for (const auto& i : j.at("curves"))
            c.curves.push_back(i.get<std::size_t>());
    if (has(j, "point"))
        c.points.push_back(parsePointRef(j.at("point")));
    if (has(j, "points"))
        for (const auto& p : j.at("points"))
            c.points.push_back(parsePointRef(p));
    if (has(j, "at"))
        for (const auto& p : j.at("at"))
            c.at.push_back(ev.vec2(p));
    if (c.type == SketchConstraint::Type::Tangent && has(j, "side")) {
        const auto side = std::lround(j.at("side").get<double>());
        if (side != 1 && side != -1 && side != -2)
            throw DesignError("tangent side must be 1, -1 or -2");
        c.side = static_cast<int>(side);
    }
    if (isDimension(c.type)) {
        c.value = ev.num(j, "value");
        if (c.type != SketchConstraint::Type::Angle && c.value < 0)
            throw DesignError("Dimension value must not be negative");
    }
    return c;
}

/// Points along the B-spline of a control polygon (Fusion: Spline > Control Point Spline): a clamped
/// open curve (starts and ends on the first and last control point) or a closed periodic one, of the
/// given degree, `perSpan` points per knot span. The sketch keeps it as a spline through these points.
std::vector<Vec2> sampleControlSpline(const std::vector<Vec2>& ctrl, int degree, bool closed, int perSpan)
{
    const int n = static_cast<int>(ctrl.size());
    std::vector<Vec2> poles = ctrl;
    if (closed)
        for (int i = 0; i < degree; ++i)
            poles.push_back(ctrl[static_cast<std::size_t>(i)]);
    const int np = static_cast<int>(poles.size());
    std::vector<double> knots(static_cast<std::size_t>(np + degree + 1));
    for (int j = 0; j < np + degree + 1; ++j) {
        if (closed)
            knots[static_cast<std::size_t>(j)] = j;
        else
            knots[static_cast<std::size_t>(j)] = j <= degree ? 0.0 : (j >= np ? double(np - degree) : double(j - degree));
    }
    const double lo = knots[static_cast<std::size_t>(degree)], hi = knots[static_cast<std::size_t>(np)];
    auto eval = [&](double u) {
        int k = degree;
        while (k < np - 1 && u >= knots[static_cast<std::size_t>(k + 1)])
            ++k;
        std::vector<Vec2> d(static_cast<std::size_t>(degree + 1));
        for (int j = 0; j <= degree; ++j)
            d[static_cast<std::size_t>(j)] = poles[static_cast<std::size_t>(j + k - degree)];
        for (int r = 1; r <= degree; ++r) {
            for (int j = degree; j >= r; --j) {
                const double a = knots[static_cast<std::size_t>(j + k - degree)];
                const double b = knots[static_cast<std::size_t>(j + 1 + k - r)];
                const double w = b > a ? (u - a) / (b - a) : 0.0;
                auto& dj = d[static_cast<std::size_t>(j)];
                const auto& dp = d[static_cast<std::size_t>(j - 1)];
                dj = {(1 - w) * dp.x + w * dj.x, (1 - w) * dp.y + w * dj.y};
            }
        }
        return d[static_cast<std::size_t>(degree)];
    };
    const int spans = closed ? n : np - degree;
    const int total = spans * perSpan;
    std::vector<Vec2> out;
    for (int i = 0; i < total; ++i)
        out.push_back(eval(lo + (hi - lo) * i / total));
    if (!closed)
        out.push_back(poles.back());
    return out;
}

/// Appends the curves of one sketch item. `implicit` receives the constraints that the
/// shape implies (a rectangle has horizontal and vertical sides, a slot tangent ends), so
/// that dimensions added later keep the shape, like Fusion's sketch tools.
void appendCurves(const Eval& ev, const Json& item, std::vector<SketchCurve>& out,
                  std::vector<SketchConstraint>& implicit)
{
    using CT = SketchConstraint::Type;
    const std::size_t first = out.size();
    if (!item.is_object())
        throw DesignError("Sketch curve must be an object");
    const std::string type = lower(required(item, "type").get<std::string>());
    const bool construction = item.value("construction", false);
    auto push = [&](SketchCurve c) {
        c.construction = construction;
        out.push_back(std::move(c));
    };
    if (type == "line") {
        push(lineCurve(ev.vec2(item, "start"), ev.vec2(item, "end"), construction));
    } else if (type == "point") {
        SketchCurve c{SketchCurve::Kind::Point, {ev.vec2(item, "at")}};
        push(c);
    } else if (type == "rectangle" || type == "center_rectangle") {
        Vec2 a;
        Vec2 b;
        if (type == "center_rectangle") {
            const Vec2 c = ev.vec2(item, "center");
            const double w = ev.num(item, "width") / 2;
            const double h = ev.num(item, "height") / 2;
            a = {c.x - w, c.y - h};
            b = {c.x + w, c.y + h};
        } else {
            a = ev.vec2(item, "corner");
            b = has(item, "opposite") ? ev.vec2(item, "opposite")
                                      : Vec2{a.x + ev.num(item, "width"), a.y + ev.num(item, "height")};
        }
        if (std::abs(a.x - b.x) < 1e-9 || std::abs(a.y - b.y) < 1e-9)
            throw DesignError("Rectangle has zero width or height");
        push(lineCurve(a, {b.x, a.y}, construction));
        push(lineCurve({b.x, a.y}, b, construction));
        push(lineCurve(b, {a.x, b.y}, construction));
        push(lineCurve({a.x, b.y}, a, construction));
        implicit.push_back({CT::Horizontal, {first}, {}});
        implicit.push_back({CT::Vertical, {first + 1}, {}});
        implicit.push_back({CT::Horizontal, {first + 2}, {}});
        implicit.push_back({CT::Vertical, {first + 3}, {}});
    } else if (type == "three_point_rectangle") {
        // Fusion's 3-Point Rectangle: one side from start to end at any angle, the other
        // side `height` long, to the left of start -> end (negative: to the right).
        const Vec2 a = ev.vec2(item, "start");
        const Vec2 b = ev.vec2(item, "end");
        const double h = ev.num(item, "height");
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-9 || std::abs(h) < 1e-9)
            throw DesignError("Rectangle has zero width or height");
        const Vec2 up{-(b.y - a.y) / len * h, (b.x - a.x) / len * h};
        const Vec2 c{b.x + up.x, b.y + up.y};
        const Vec2 d{a.x + up.x, a.y + up.y};
        push(lineCurve(a, b, construction));
        push(lineCurve(b, c, construction));
        push(lineCurve(c, d, construction));
        push(lineCurve(d, a, construction));
        implicit.push_back({CT::Perpendicular, {first, first + 1}, {}});
        implicit.push_back({CT::Parallel, {first, first + 2}, {}});
        implicit.push_back({CT::Parallel, {first + 1, first + 3}, {}});
    } else if (type == "circle") {
        SketchCurve c{SketchCurve::Kind::Circle, {ev.vec2(item, "center")}};
        c.radius = has(item, "diameter") ? ev.num(item, "diameter") / 2 : ev.num(item, "radius");
        push(c);
    } else if (type == "arc") {
        SketchCurve c{SketchCurve::Kind::Arc, {ev.vec2(item, "start"), ev.vec2(item, "through"), ev.vec2(item, "end")}};
        c.joinStart = item.value("join_start", true);
        c.joinEnd = item.value("join_end", true);
        push(c);
    } else if (type == "center_arc") {
        const Vec2 c = ev.vec2(item, "center");
        const double r = ev.num(item, "radius");
        const double a0 = ev.num(item, "startAngle") * kDegToRad;
        double a1 = ev.num(item, "endAngle") * kDegToRad;
        if (a1 <= a0)
            a1 += 2 * std::numbers::pi;
        const double am = (a0 + a1) / 2;
        auto at = [&](double a) { return Vec2{c.x + r * std::cos(a), c.y + r * std::sin(a)}; };
        push({SketchCurve::Kind::Arc, {at(a0), at(am), at(a1)}});
    } else if (type == "polygon") {
        // Fusion: Polygon > Inscribed (corners on the circle `radius`), Circumscribed (sides
        // touch it; `angle` points at the middle of a side) or Edge (a side from `start` to
        // `end`, the polygon on its left). All become the same regular polygon.
        const int sides = static_cast<int>(std::lround(ev.num(item, "sides")));
        if (sides < 3 || sides > 256)
            throw DesignError("Polygon needs 3 to 256 sides");
        const std::string mode = lower(item.value("mode", std::string("inscribed")));
        const double halfTurn = std::numbers::pi / sides;
        Vec2 c{};
        double r = 0.0;
        double rot = 0.0;
        if (mode == "edge") {
            const Vec2 a = ev.vec2(item, "start");
            const Vec2 b = ev.vec2(item, "end");
            const double len = std::hypot(b.x - a.x, b.y - a.y);
            if (len < 1e-9)
                throw DesignError("Polygon edge needs two distinct points");
            const Vec2 mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
            const double apothem = len / (2 * std::tan(halfTurn));
            c = Vec2{mid.x - (b.y - a.y) / len * apothem, mid.y + (b.x - a.x) / len * apothem};
            r = len / (2 * std::sin(halfTurn));
            rot = std::atan2(a.y - c.y, a.x - c.x);
        } else {
            c = ev.vec2(item, "center");
            r = ev.num(item, "radius");
            rot = ev.num(item, "angle", 0.0) * kDegToRad;
            if (mode == "circumscribed") {
                r /= std::cos(halfTurn);
                rot -= halfTurn;
            } else if (mode != "inscribed") {
                throw DesignError("Unknown polygon mode '" + mode + "' (inscribed, circumscribed, edge)");
            }
        }
        auto at = [&](int i) {
            const double a = rot + 2 * std::numbers::pi * i / sides;
            return Vec2{c.x + r * std::cos(a), c.y + r * std::sin(a)};
        };
        for (int i = 0; i < sides; ++i)
            push(lineCurve(at(i), at(i + 1), construction));
        for (int i = 1; i < sides; ++i)
            implicit.push_back({CT::Equal, {first, first + static_cast<std::size_t>(i)}, {}});
    } else if (type == "slot") {
        // Two parallel lines joined by half circles. Fusion: Slot > Center to Center (`start`,
        // `end` are the centres of the ends), Overall (they are the far points of the ends) or
        // Center Point (`center` of the slot and `end`, the centre of one end).
        const std::string mode = lower(item.value("mode", std::string("center_to_center")));
        Vec2 a{};
        Vec2 b{};
        const double r = ev.num(item, "width") / 2;
        if (mode == "center_point") {
            const Vec2 m = ev.vec2(item, "center");
            b = ev.vec2(item, "end");
            a = Vec2{2 * m.x - b.x, 2 * m.y - b.y};
        } else {
            a = ev.vec2(item, "start");
            b = ev.vec2(item, "end");
            if (mode == "overall") {
                const double span = std::hypot(b.x - a.x, b.y - a.y);
                if (span <= 2 * r + 1e-9)
                    throw DesignError("An overall slot must be longer than its width");
                const Vec2 u{(b.x - a.x) / span, (b.y - a.y) / span};
                a = Vec2{a.x + u.x * r, a.y + u.y * r};
                b = Vec2{b.x - u.x * r, b.y - u.y * r};
            } else if (mode != "center_to_center") {
                throw DesignError("Unknown slot mode '" + mode + "' (center_to_center, overall, center_point)");
            }
        }
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-9 || r <= 0)
            throw DesignError("Slot needs distinct centres and a positive width");
        const Vec2 d{(b.x - a.x) / len, (b.y - a.y) / len};
        const Vec2 n{-d.y, d.x};
        push(lineCurve(a + n * r, b + n * r, construction));
        push({SketchCurve::Kind::Arc, {b + n * r, b + d * r, b - n * r}});
        push(lineCurve(b - n * r, a - n * r, construction));
        push({SketchCurve::Kind::Arc, {a - n * r, a - d * r, a + n * r}});
        implicit.push_back({CT::Parallel, {first, first + 2}, {}});
        implicit.push_back({CT::Tangent, {first, first + 1}, {}});
        implicit.push_back({CT::Tangent, {first + 2, first + 1}, {}});
        implicit.push_back({CT::Tangent, {first + 2, first + 3}, {}});
        implicit.push_back({CT::Tangent, {first, first + 3}, {}});
    } else if (type == "arc_slot") {
        // Fusion: Slot > Center Point Arc Slot. A slot along an arc about `center`: `radius` is the centre
        // line's, `width` the slot's, from `start_angle` counter-clockwise to `end_angle` (degrees).
        const Vec2 c = ev.vec2(item, "center");
        const double r = ev.num(item, "radius");
        const double w = ev.num(item, "width") / 2;
        const double a0 = ev.num(item, "start_angle") * kDegToRad;
        double a1 = ev.num(item, "end_angle") * kDegToRad;
        if (!(r > 0) || !(w > 0))
            throw DesignError("An arc slot needs a positive radius and width");
        if (w >= r)
            throw DesignError("The arc slot is too wide: its width must be less than twice its radius");
        while (a1 <= a0)
            a1 += 2 * std::numbers::pi;
        if (a1 - a0 >= 2 * std::numbers::pi - 1e-6)
            throw DesignError("An arc slot must span less than a full turn");
        const double mid = (a0 + a1) / 2;
        auto at = [&](double radius, double a) { return Vec2{c.x + radius * std::cos(a), c.y + radius * std::sin(a)}; };
        const Vec2 t1{-std::sin(a1), std::cos(a1)}, t0{-std::sin(a0), std::cos(a0)};
        const Vec2 end1 = at(r, a1), end0 = at(r, a0);
        push({SketchCurve::Kind::Arc, {at(r + w, a0), at(r + w, mid), at(r + w, a1)}});
        push({SketchCurve::Kind::Arc, {at(r + w, a1), {end1.x + t1.x * w, end1.y + t1.y * w}, at(r - w, a1)}});
        push({SketchCurve::Kind::Arc, {at(r - w, a1), at(r - w, mid), at(r - w, a0)}});
        push({SketchCurve::Kind::Arc, {at(r - w, a0), {end0.x - t0.x * w, end0.y - t0.y * w}, at(r + w, a0)}});
        implicit.push_back({CT::Concentric, {first, first + 2}, {}});
        implicit.push_back({CT::Tangent, {first, first + 1}, {}});
        implicit.push_back({CT::Tangent, {first + 2, first + 1}, {}});
        implicit.push_back({CT::Tangent, {first + 2, first + 3}, {}});
        implicit.push_back({CT::Tangent, {first, first + 3}, {}});
    } else if (type == "ellipse") {
        // Fusion's Ellipse: centre, major radius along `angle` (degrees), minor radius.
        const Vec2 c = ev.vec2(item, "center");
        const double major = ev.num(item, "major_radius");
        const double minor = ev.num(item, "minor_radius");
        const double a = ev.num(item, "angle", 0.0) * kDegToRad;
        if (!(major > 0) || !(minor > 0))
            throw DesignError("Ellipse radii must be positive");
        SketchCurve e{SketchCurve::Kind::Ellipse, {c, {c.x + major * std::cos(a), c.y + major * std::sin(a)}}};
        e.radius = minor;
        push(e);
    } else if (type == "elliptical_arc") {
        // Part of an ellipse (a trimmed one), counter-clockwise from start_angle to end_angle
        // (parametric angles in degrees: x = major cos t, y = minor sin t along the axes).
        const Vec2 c = ev.vec2(item, "center");
        const double major = ev.num(item, "major_radius");
        const double minor = ev.num(item, "minor_radius");
        const double a = ev.num(item, "angle", 0.0) * kDegToRad;
        const double t0 = ev.num(item, "start_angle") * kDegToRad;
        double t1 = ev.num(item, "end_angle") * kDegToRad;
        if (!(major > 0) || !(minor > 0))
            throw DesignError("Ellipse radii must be positive");
        while (t1 <= t0)
            t1 += 2 * std::numbers::pi;
        const Vec2 u{std::cos(a), std::sin(a)};
        auto at = [&](double t) {
            const double x = major * std::cos(t), y = minor * std::sin(t);
            return Vec2{c.x + x * u.x - y * u.y, c.y + x * u.y + y * u.x};
        };
        SketchCurve e{SketchCurve::Kind::EllipticalArc, {at(t0), c, {c.x + major * u.x, c.y + major * u.y}, at(t1)}};
        e.radius = minor;
        e.joinStart = item.value("join_start", true);
        e.joinEnd = item.value("join_end", true);
        push(e);
    } else if (type == "text") {
        // Fusion's Text: the letters start at `at` on a baseline turned `angle` degrees, `height` tall.
        const Vec2 at = ev.vec2(item, "at");
        const double height = ev.num(item, "height", 5.0);
        const double a = ev.num(item, "angle", 0.0) * kDegToRad;
        const std::string text = required(item, "text").get<std::string>();
        if (!(height > 0))
            throw DesignError("Text height must be positive");
        if (text.find_first_not_of(" \t\r\n") == std::string::npos)
            throw DesignError("Text is empty");
        SketchCurve t{SketchCurve::Kind::Text, {at, {at.x + height * std::cos(a), at.y + height * std::sin(a)}}};
        t.text = text;
        t.font = item.value("font", std::string());
        t.bold = item.value("bold", false);
        t.italic = item.value("italic", false);
        push(t);
    } else if (type == "conic") {
        // Fusion's Conic Curve: from start to end, pulled towards the apex by rho
        // (0 < rho < 1; 0.5 is a parabola, less an ellipse, more a hyperbola).
        const double rho = ev.num(item, "rho", 0.5);
        if (!(rho > 0) || !(rho < 1))
            throw DesignError("Conic rho must be between 0 and 1");
        SketchCurve e{SketchCurve::Kind::Conic, {ev.vec2(item, "start"), ev.vec2(item, "apex"), ev.vec2(item, "end")}};
        e.radius = rho;
        e.joinStart = item.value("join_start", true);
        e.joinEnd = item.value("join_end", true);
        push(e);
    } else if (type == "airfoil") {
        // A NACA 4-digit airfoil (cosine-spaced points on a closed trailing edge) as two splines: the
        // upper surface from the leading edge `at` to the trailing edge, the lower one back. Together
        // they bound a profile that extrudes, lofts and sweeps like any other.
        const Json& code = required(item, "naca");
        std::string digits;
        if (code.is_number_integer()) {
            digits = std::to_string(code.get<long long>());
            digits.insert(0, digits.size() < 4 ? 4 - digits.size() : 0, '0'); // 12 -> 0012
        } else if (code.is_string()) {
            digits = lower(code.get<std::string>());
            if (digits.rfind("naca", 0) == 0)
                digits.erase(0, 4);
        }
        if (digits.size() != 4 || digits.find_first_not_of("0123456789") != std::string::npos)
            throw DesignError("Airfoil naca needs 4 digits, e.g. \"2412\"");
        const double camber = (digits[0] - '0') / 100.0;
        const double camberAt = (digits[1] - '0') / 10.0;
        const double thick = ((digits[2] - '0') * 10 + (digits[3] - '0')) / 100.0;
        if (thick <= 0)
            throw DesignError("Airfoil thickness (the last two digits) must be above 0");
        if (camber > 0 && camberAt <= 0)
            throw DesignError("A cambered airfoil needs a non-zero position of maximum camber (second digit)");
        const double chord = ev.num(item, "chord");
        if (!(chord > 0))
            throw DesignError("Airfoil chord must be positive");
        const Vec2 le = has(item, "at") ? ev.vec2(item, "at") : Vec2{0, 0};
        const double a = ev.num(item, "angle", 0.0) * kDegToRad;
        const int n = 40;
        auto place = [&](double x, double y) {
            return Vec2{le.x + chord * (x * std::cos(a) - y * std::sin(a)), le.y + chord * (x * std::sin(a) + y * std::cos(a))};
        };
        std::vector<Vec2> over, under;
        for (int i = 0; i <= n; ++i) {
            const double x = (1 - std::cos(std::numbers::pi * i / n)) / 2;
            // The thickness polynomial with the closed trailing edge (-0.1036 x^4).
            const double yt = 5 * thick * (0.2969 * std::sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x * x * x - 0.1036 * x * x * x * x);
            double yc = 0.0, slope = 0.0;
            if (camber > 0) {
                const double p = camberAt;
                if (x < p) {
                    yc = camber / (p * p) * (2 * p * x - x * x);
                    slope = 2 * camber / (p * p) * (p - x);
                } else {
                    yc = camber / ((1 - p) * (1 - p)) * ((1 - 2 * p) + 2 * p * x - x * x);
                    slope = 2 * camber / ((1 - p) * (1 - p)) * (p - x);
                }
            }
            const double th = std::atan(slope);
            const bool end = i == 0 || i == n; // exactly the leading and trailing edge, shared
            over.push_back(end ? place(x, 0.0) : place(x - yt * std::sin(th), yc + yt * std::cos(th)));
            under.push_back(end ? place(x, 0.0) : place(x + yt * std::sin(th), yc - yt * std::cos(th)));
        }
        std::reverse(under.begin(), under.end());
        push(SketchCurve{SketchCurve::Kind::Spline, over});
        push(SketchCurve{SketchCurve::Kind::Spline, under});
    } else if (type == "spline") {
        SketchCurve c{SketchCurve::Kind::Spline, {}};
        for (const auto& p : required(item, "points"))
            c.points.push_back(ev.vec2(p));
        c.closed = item.value("closed", false);
        push(c);
    } else if (type == "control_spline") {
        std::vector<Vec2> ctrl;
        for (const auto& p : required(item, "points"))
            ctrl.push_back(ev.vec2(p));
        const bool closed = item.value("closed", false);
        if (item.contains("degree") && !item["degree"].is_number_integer())
            throw DesignError("A control point spline has an integer degree from 2 to 5");
        const int degree = item.value("degree", 3);
        if (degree < 2 || degree > 5)
            throw DesignError("A control point spline has degree 2 to 5");
        if (static_cast<int>(ctrl.size()) < degree + 1)
            throw DesignError("A degree " + std::to_string(degree) + " control point spline needs at least " + std::to_string(degree + 1) +
                              " control points");
        SketchCurve c{SketchCurve::Kind::Spline, sampleControlSpline(ctrl, degree, closed, 12)};
        c.closed = closed;
        push(c);
    } else {
        throw DesignError("Unknown sketch curve type '" + type + "'");
    }
}


/// A solved sketch curve as a single sketch item (line or three-point arc).
/// A line, arc or circle of a sketch for trimming: a segment a-b, or a circle (centre,
/// radius) with the arc from angle `a0` over `sweep` (signed; 2 pi for a full circle).
struct TrimCurve {
    bool line = true;
    Vec2 a, b;
    Vec2 c;
    double r = 0.0, a0 = 0.0, sweep = 0.0;
    /// An ellipse or elliptical arc: major axis direction `u`, radii `ra` (along u) and `rb`;
    /// a0 and sweep are then parametric angles (x = ra cos t, y = rb sin t).
    bool ell = false;
    Vec2 u{1, 0};
    double ra = 0.0, rb = 0.0;
};

/// The angle of `p` about the centre of a circular or elliptical trim curve (parametric for
/// an ellipse).
double angleOn(const TrimCurve& t, const Vec2& p)
{
    if (!t.ell)
        return std::atan2(p.y - t.c.y, p.x - t.c.x);
    const Vec2 d{p.x - t.c.x, p.y - t.c.y};
    const double x = d.x * t.u.x + d.y * t.u.y, y = t.u.x * d.y - t.u.y * d.x;
    return std::atan2(y / t.rb, x / t.ra);
}

/// The point at angle `a` of a circular or elliptical trim curve.
Vec2 pointAtAngle(const TrimCurve& t, double a)
{
    if (!t.ell)
        return {t.c.x + t.r * std::cos(a), t.c.y + t.r * std::sin(a)};
    const double x = t.ra * std::cos(a), y = t.rb * std::sin(a);
    return {t.c.x + x * t.u.x - y * t.u.y, t.c.y + x * t.u.y + y * t.u.x};
}

Vec2 circleThrough(const Vec2& p1, const Vec2& p2, const Vec2& p3)
{
    const double d = 2 * (p1.x * (p2.y - p3.y) + p2.x * (p3.y - p1.y) + p3.x * (p1.y - p2.y));
    if (std::abs(d) < 1e-12)
        throw DesignError("Degenerate arc");
    const double s1 = p1.x * p1.x + p1.y * p1.y, s2 = p2.x * p2.x + p2.y * p2.y, s3 = p3.x * p3.x + p3.y * p3.y;
    return {(s1 * (p2.y - p3.y) + s2 * (p3.y - p1.y) + s3 * (p1.y - p2.y)) / d,
            (s1 * (p3.x - p2.x) + s2 * (p1.x - p3.x) + s3 * (p2.x - p1.x)) / d};
}

double wrap2pi(double a)
{
    const double t = std::fmod(a, 2 * std::numbers::pi);
    return t < 0 ? t + 2 * std::numbers::pi : t;
}

std::optional<TrimCurve> trimCurve(const SketchCurve& k)
{
    TrimCurve t;
    if (k.kind == SketchCurve::Kind::Line) {
        t.a = k.points[0];
        t.b = k.points[1];
        return t;
    }
    t.line = false;
    if (k.kind == SketchCurve::Kind::Circle) {
        t.c = k.points[0];
        t.r = k.radius;
        t.sweep = 2 * std::numbers::pi;
        return t;
    }
    if (k.kind == SketchCurve::Kind::Ellipse || k.kind == SketchCurve::Kind::EllipticalArc) {
        const bool arc = k.kind == SketchCurve::Kind::EllipticalArc;
        t.ell = true;
        t.c = k.points[arc ? 1 : 0];
        const Vec2 ax{k.points[arc ? 2 : 1].x - t.c.x, k.points[arc ? 2 : 1].y - t.c.y};
        t.ra = std::hypot(ax.x, ax.y);
        t.rb = k.radius;
        if (t.ra < 1e-12 || t.rb < 1e-12)
            return std::nullopt;
        t.u = {ax.x / t.ra, ax.y / t.ra};
        if (!arc) {
            t.sweep = 2 * std::numbers::pi;
            return t;
        }
        t.a0 = angleOn(t, k.points[0]);
        t.sweep = wrap2pi(angleOn(t, k.points[3]) - t.a0); // counter-clockwise
        if (t.sweep < 1e-12)
            t.sweep = 2 * std::numbers::pi;
        return t;
    }
    if (k.kind != SketchCurve::Kind::Arc)
        return std::nullopt;
    t.c = circleThrough(k.points[0], k.points[1], k.points[2]);
    t.r = std::hypot(k.points[0].x - t.c.x, k.points[0].y - t.c.y);
    auto ang = [&](const Vec2& p) { return std::atan2(p.y - t.c.y, p.x - t.c.x); };
    t.a0 = ang(k.points[0]);
    double sweep = wrap2pi(ang(k.points[2]) - t.a0);
    if (wrap2pi(ang(k.points[1]) - t.a0) > sweep)
        sweep -= 2 * std::numbers::pi; // clockwise
    t.sweep = sweep;
    return t;
}

/// Parameter in [0, 1] of point `p` (on or near the curve) along it; for a circle the
/// angle from 0 over 2 pi. nullopt when the point is outside an arc.
std::optional<double> trimParam(const TrimCurve& t, const Vec2& p, double tol = 1e-9)
{
    if (t.line) {
        const Vec2 d = t.b - t.a;
        const double l2 = d.x * d.x + d.y * d.y;
        const double u = ((p.x - t.a.x) * d.x + (p.y - t.a.y) * d.y) / l2;
        if (u < -tol || u > 1 + tol)
            return std::nullopt;
        return std::clamp(u, 0.0, 1.0);
    }
    const double a = angleOn(t, p);
    double along = t.sweep >= 0 ? wrap2pi(a - t.a0) : wrap2pi(t.a0 - a);
    const double span = std::abs(t.sweep);
    if (along > span + tol) {
        if (2 * std::numbers::pi - along < tol)
            along = 0.0;
        else
            return std::nullopt;
    }
    return std::min(along, span) / span;
}

Vec2 trimPoint(const TrimCurve& t, double u)
{
    if (t.line)
        return t.a + (t.b - t.a) * u;
    return pointAtAngle(t, t.a0 + t.sweep * u);
}

/// How far `x` is off curve `t` taken whole (a line as infinite, an arc as its circle,
/// an elliptical arc as its ellipse), signed: 0 on it.
double offCurve(const TrimCurve& t, const Vec2& x)
{
    if (t.line) {
        const Vec2 d = t.b - t.a;
        return (d.x * (x.y - t.a.y) - d.y * (x.x - t.a.x)) / std::max(1e-300, std::hypot(d.x, d.y));
    }
    if (!t.ell)
        return std::hypot(x.x - t.c.x, x.y - t.c.y) - t.r;
    const Vec2 d{x.x - t.c.x, x.y - t.c.y};
    const double px = (d.x * t.u.x + d.y * t.u.y) / t.ra, py = (t.u.x * d.y - t.u.y * d.x) / t.rb;
    return (px * px + py * py - 1) * (t.ra + t.rb) / 4;
}

/// Where curve `p` (along its length) crosses curve `q`: sign changes of q's offset along
/// p, sampled and then bisected (for pairs with an ellipse, which have no closed form here).
/// Two crossings close together (a small circle cutting the ellipse) can fall between two
/// samples with the same sign, so a sample nearer to q than both neighbours is refined to
/// the nearest approach between them, and the crossings either side of it are bisected.
std::vector<Vec2> crossingsAlong(const TrimCurve& p, const TrimCurve& q)
{
    std::vector<Vec2> pts;
    const int n = 720;
    auto f = [&](double s) { return offCurve(q, trimPoint(p, s)); };
    auto root = [&](double lo, double hi) {
        double flo = f(lo);
        for (int k = 0; k < 80; ++k) {
            const double mid = (lo + hi) / 2, fm = f(mid);
            if ((fm < 0) == (flo < 0)) {
                lo = mid;
                flo = fm;
            } else {
                hi = mid;
            }
        }
        return trimPoint(p, (lo + hi) / 2);
    };
    std::vector<double> fs(n + 1);
    for (int i = 0; i <= n; ++i)
        fs[i] = f(static_cast<double>(i) / n);
    for (int i = 1; i <= n; ++i) {
        const double s0 = static_cast<double>(i - 1) / n, s1 = static_cast<double>(i) / n;
        const double f0 = fs[i - 1], f1 = fs[i];
        if (f0 == 0.0)
            pts.push_back(trimPoint(p, s0));
        else if ((f0 < 0) != (f1 < 0) && f1 != 0.0)
            pts.push_back(root(s0, s1));
    }
    // A dip towards q at a sample (nearer than both neighbours, wrapping round a closed
    // curve) without a sign change: refine the nearest approach between the neighbours.
    const bool closed = !p.line && std::abs(p.sweep) >= 2 * std::numbers::pi - 1e-12;
    for (int i = 0; i < (closed ? n : n + 1); ++i) {
        const double fi = fs[i];
        const bool hasLeft = i > 0 || closed, hasRight = i < n;
        const double fl = i > 0 ? fs[i - 1] : fs[n - 1], fr = hasRight ? fs[i + 1] : 0.0;
        if (fi == 0.0 || (hasLeft && ((fl < 0) != (fi < 0) || std::abs(fi) > std::abs(fl))) ||
            (hasRight && ((fr < 0) != (fi < 0) || std::abs(fi) > std::abs(fr))))
            continue;
        const double sign = fi < 0 ? -1.0 : 1.0;
        const double from = hasLeft ? static_cast<double>(i - 1) / n : 0.0;
        const double to = hasRight ? static_cast<double>(i + 1) / n : 1.0;
        double lo = from, hi = to;
        const double g = (std::sqrt(5.0) - 1) / 2;
        double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
        double fa = sign * f(a), fb = sign * f(b);
        for (int k = 0; k < 100; ++k) {
            if (fa < fb) {
                hi = b;
                b = a;
                fb = fa;
                a = hi - g * (hi - lo);
                fa = sign * f(a);
            } else {
                lo = a;
                a = b;
                fa = fb;
                b = lo + g * (hi - lo);
                fb = sign * f(b);
            }
        }
        const double m = (lo + hi) / 2;
        if (sign * f(m) < 0) {
            pts.push_back(root(from, m));
            pts.push_back(root(m, to));
        }
    }
    if (fs[n] == 0.0 && !closed)
        pts.push_back(trimPoint(p, 1.0));
    return pts;
}

/// Points where two curves cross.
std::vector<Vec2> crossings(const TrimCurve& p, const TrimCurve& q)
{
    std::vector<Vec2> pts;
    auto lineCircle = [&](const TrimCurve& l, const TrimCurve& c) {
        const Vec2 d = l.b - l.a, f = l.a - c.c;
        const double A = d.x * d.x + d.y * d.y, B = 2 * (f.x * d.x + f.y * d.y), C = f.x * f.x + f.y * f.y - c.r * c.r;
        const double disc = B * B - 4 * A * C;
        if (disc < -1e-12 * A * c.r * c.r)
            return;
        const double sq = std::sqrt(std::max(0.0, disc));
        for (const double u : {(-B - sq) / (2 * A), (-B + sq) / (2 * A)})
            pts.push_back(l.a + d * u);
    };
    auto lineEllipse = [&](const TrimCurve& l, const TrimCurve& e) {
        // In the ellipse's own scaled frame it is the unit circle and the line stays a line
        // (same parameter along it).
        auto local = [&](const Vec2& x) {
            const Vec2 d{x.x - e.c.x, x.y - e.c.y};
            return Vec2{(d.x * e.u.x + d.y * e.u.y) / e.ra, (e.u.x * d.y - e.u.y * d.x) / e.rb};
        };
        TrimCurve ll = l, unit;
        ll.a = local(l.a);
        ll.b = local(l.b);
        unit.line = false;
        unit.r = 1.0;
        unit.sweep = 2 * std::numbers::pi;
        const std::size_t first = pts.size();
        lineCircle(ll, unit);
        const Vec2 d = l.b - l.a, dl = ll.b - ll.a;
        for (std::size_t i = first; i < pts.size(); ++i) {
            const Vec2 w = pts[i] - ll.a;
            const double s = (w.x * dl.x + w.y * dl.y) / std::max(1e-300, dl.x * dl.x + dl.y * dl.y);
            pts[i] = l.a + d * s;
        }
    };
    if (p.ell || q.ell) {
        if (p.line)
            lineEllipse(p, q);
        else if (q.line)
            lineEllipse(q, p);
        else
            pts = crossingsAlong(p, q);
    } else if (p.line && q.line) {
        const Vec2 r = p.b - p.a, s = q.b - q.a;
        const double den = r.x * s.y - r.y * s.x;
        if (std::abs(den) < 1e-12)
            return pts;
        const Vec2 w = q.a - p.a;
        const double u = (w.x * s.y - w.y * s.x) / den;
        pts.push_back(p.a + r * u);
    } else if (p.line) {
        lineCircle(p, q);
    } else if (q.line) {
        lineCircle(q, p);
    } else {
        const Vec2 d = q.c - p.c;
        const double dist = std::hypot(d.x, d.y);
        if (dist < 1e-12 || dist > p.r + q.r + 1e-9 || dist < std::abs(p.r - q.r) - 1e-9)
            return pts;
        const double a = (p.r * p.r - q.r * q.r + dist * dist) / (2 * dist);
        const double h = std::sqrt(std::max(0.0, p.r * p.r - a * a));
        const Vec2 m = p.c + d * (a / dist);
        const Vec2 n{-d.y / dist, d.x / dist};
        pts.push_back(m + n * h);
        if (h > 1e-12)
            pts.push_back(m - n * h);
    }
    // Only where both curves really are (within their ends / arc spans).
    std::vector<Vec2> out;
    for (const auto& x : pts) {
        if (trimParam(p, x, 1e-7) && trimParam(q, x, 1e-7))
            out.push_back(x);
    }
    return out;
}

/// A piece of a trimmed curve, from u0 to u1, as a sketch item.
Json trimPiece(const TrimCurve& t, double u0, double u1, bool construction)
{
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    Json item;
    if (t.line)
        item = {{"type", "line"}, {"start", pt(trimPoint(t, u0))}, {"end", pt(trimPoint(t, u1))}};
    else if (t.ell)
        item = {{"type", "elliptical_arc"},
                {"center", pt(t.c)},
                {"major_radius", t.ra},
                {"minor_radius", t.rb},
                {"angle", std::atan2(t.u.y, t.u.x) / kDegToRad},
                {"start_angle", (t.a0 + t.sweep * std::min(u0, u1)) / kDegToRad},
                {"end_angle", (t.a0 + t.sweep * std::max(u0, u1)) / kDegToRad}};
    else
        item = {{"type", "arc"},
                {"start", pt(trimPoint(t, u0))},
                {"through", pt(trimPoint(t, (u0 + u1) / 2))},
                {"end", pt(trimPoint(t, u1))}};
    if (construction)
        item["construction"] = true;
    return item;
}

/// Line segments (pairs of points) of a polyline list (pairs) cut into dashes.
std::vector<core::Vec3> dashed(const std::vector<core::Vec3>& segments, double dash)
{
    std::vector<core::Vec3> out;
    double phase = 0.0; // distance along the current dash period
    for (std::size_t i = 0; i + 1 < segments.size(); i += 2) {
        const core::Vec3 a = segments[i], b = segments[i + 1];
        const double len = (b - a).length();
        double t = 0.0;
        while (t < len) {
            const bool on = phase < dash;
            const double step = std::min(len - t, (on ? dash : 2 * dash) - phase);
            if (on) {
                out.push_back(a + (b - a) * (t / len));
                out.push_back(a + (b - a) * ((t + step) / len));
            }
            t += step;
            phase += step;
            if (phase >= 2 * dash - 1e-12)
                phase = 0.0;
        }
    }
    return out;
}

/// What a sketch part draws: its curves, construction curves dashed (Fusion).
std::vector<core::Vec3> sketchSegments(const SketchResult& s)
{
    std::vector<SketchCurve> solid, construction, points;
    for (const auto& c : s.curves)
        (c.kind == SketchCurve::Kind::Point ? points : c.construction ? construction : solid).push_back(c);
    std::vector<core::Vec3> out;
    if (construction.empty()) {
        out = geometry::edgeSegments(s.geometry.curves);
    } else {
        if (!solid.empty())
            out = geometry::edgeSegments(geometry::curveEdges(s.plane, solid));
        const geometry::Shape edges = geometry::curveEdges(s.plane, construction);
        const auto box = edges.bounds();
        const double dash = std::clamp((box.max - box.min).length() / 60.0, 0.4, 4.0);
        for (const auto& p : dashed(geometry::edgeSegments(edges), dash))
            out.push_back(p);
    }
    // Sketch points: a small cross each, sized to the sketch (Fusion draws a dot).
    if (!points.empty()) {
        double size = 0.0;
        if (!out.empty()) {
            core::Vec3 lo = out.front(), hi = out.front();
            for (const auto& p : out) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            size = (hi - lo).length();
        }
        const double h = std::clamp(size / 80.0, 0.3, 2.0);
        const core::Vec3 u = s.plane.xDir * h, v = s.plane.yDir() * h;
        for (const auto& c : points) {
            const core::Vec3 p = s.plane.toWorld(c.points[0]);
            out.push_back(p - u);
            out.push_back(p + u);
            out.push_back(p - v);
            out.push_back(p + v);
        }
    }
    return out;
}

/// An ellipse curve as a sketch item (centre, radii, axis angle in degrees).
Json ellipseItem(const SketchCurve& c)
{
    const Vec2 ctr = c.points[0], end = c.points[1];
    return Json{{"type", "ellipse"},
                {"center", Json::array({ctr.x, ctr.y})},
                {"major_radius", std::hypot(end.x - ctr.x, end.y - ctr.y)},
                {"minor_radius", c.radius},
                {"angle", std::atan2(end.y - ctr.y, end.x - ctr.x) / kDegToRad}};
}

/// An elliptical arc curve as a sketch item (parametric angles in degrees, counter-clockwise).
Json ellipticalArcItem(const SketchCurve& c)
{
    const Vec2 ctr = c.points[1], end = c.points[2];
    const double a = std::hypot(end.x - ctr.x, end.y - ctr.y), b = c.radius;
    const Vec2 u{(end.x - ctr.x) / a, (end.y - ctr.y) / a};
    auto param = [&](const Vec2& p) {
        const Vec2 d{p.x - ctr.x, p.y - ctr.y};
        return std::atan2((u.x * d.y - u.y * d.x) / b, (d.x * u.x + d.y * u.y) / a) / kDegToRad;
    };
    const double t0 = param(c.points[0]);
    double t1 = param(c.points[3]);
    while (t1 <= t0)
        t1 += 360.0;
    return Json{{"type", "elliptical_arc"},
                {"center", Json::array({ctr.x, ctr.y})},
                {"major_radius", a},
                {"minor_radius", b},
                {"angle", std::atan2(u.y, u.x) / kDegToRad},
                {"start_angle", t0},
                {"end_angle", t1}};
}

/// A text curve as a sketch item.
Json textItem(const SketchCurve& c)
{
    const Vec2 at = c.points[0], e = c.points[1];
    Json item{{"type", "text"},
              {"at", Json::array({at.x, at.y})},
              {"text", c.text},
              {"height", std::hypot(e.x - at.x, e.y - at.y)},
              {"angle", std::atan2(e.y - at.y, e.x - at.x) / kDegToRad}};
    if (!c.font.empty())
        item["font"] = c.font;
    if (c.bold)
        item["bold"] = true;
    if (c.italic)
        item["italic"] = true;
    return item;
}

/// A conic curve as a sketch item.
Json conicItem(const SketchCurve& c)
{
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    return Json{{"type", "conic"},
                {"start", pt(c.points[0])},
                {"end", pt(c.points[2])},
                {"apex", pt(c.points[1])},
                {"rho", c.radius}};
}

Json curveItem(const SketchCurve& c)
{
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    Json item;
    if (c.kind == SketchCurve::Kind::Line)
        item = {{"type", "line"}, {"start", pt(c.points[0])}, {"end", pt(c.points[1])}};
    else if (c.kind == SketchCurve::Kind::Arc)
        item = {{"type", "arc"}, {"start", pt(c.points[0])}, {"through", pt(c.points[1])}, {"end", pt(c.points[2])}};
    else if (c.kind == SketchCurve::Kind::Point)
        item = {{"type", "point"}, {"at", pt(c.points[0])}};
    else if (c.kind == SketchCurve::Kind::Ellipse)
        item = ellipseItem(c);
    else if (c.kind == SketchCurve::Kind::EllipticalArc)
        item = ellipticalArcItem(c);
    else if (c.kind == SketchCurve::Kind::Conic)
        item = conicItem(c);
    else if (c.kind == SketchCurve::Kind::Text)
        item = textItem(c);
    else
        throw DesignError("Only lines and arcs of a shape can be deleted separately");
    if (c.construction)
        item["construction"] = true;
    return item;
}

const char* implicitName(SketchConstraint::Type t)
{
    using CT = SketchConstraint::Type;
    switch (t) {
    case CT::Horizontal: return "horizontal";
    case CT::Vertical: return "vertical";
    case CT::Parallel: return "parallel";
    case CT::Perpendicular: return "perpendicular";
    case CT::Tangent: return "tangent";
    case CT::Equal: return "equal";
    default: return nullptr;
    }
}

/// Re-indexes the curve references of a constraint; false when it refers to a removed curve.
bool remapConstraint(Json& c, const std::vector<std::optional<std::size_t>>& map)
{
    auto curve = [&](Json& idx) {
        if (!idx.is_number_integer())
            return true;
        const auto i = idx.get<std::size_t>();
        if (i >= map.size() || !map[i])
            return false;
        idx = *map[i];
        return true;
    };
    auto point = [&](Json& ref) { return !ref.is_object() || !ref.contains("curve") || curve(ref["curve"]); };
    if (c.contains("curve") && !curve(c["curve"]))
        return false;
    if (c.contains("curves") && c["curves"].is_array())
        for (auto& i : c["curves"])
            if (!curve(i))
                return false;
    if (c.contains("point") && !point(c["point"]))
        return false;
    if (c.contains("points") && c["points"].is_array())
        for (auto& r : c["points"])
            if (!point(r))
                return false;
    return true;
}

/// A sketch item rewritten with where its curves are now (`c`: the solved curves it
/// made, in order), so a drag result stays. Implicit shape constraints still hold, so a
/// rectangle stays a rectangle, a polygon regular.
Json itemFromSolved(const Json& item, const SketchCurve* c, std::size_t n)
{
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    Json out = item;
    const std::string type = lower(item.at("type").get<std::string>());
    auto arcCenter = [](const SketchCurve& a) {
        const Vec2 p1 = a.points[0], p2 = a.points[1], p3 = a.points[2];
        const double d = 2 * (p1.x * (p2.y - p3.y) + p2.x * (p3.y - p1.y) + p3.x * (p1.y - p2.y));
        const double s1 = p1.x * p1.x + p1.y * p1.y, s2 = p2.x * p2.x + p2.y * p2.y, s3 = p3.x * p3.x + p3.y * p3.y;
        return Vec2{(s1 * (p2.y - p3.y) + s2 * (p3.y - p1.y) + s3 * (p1.y - p2.y)) / d,
                    (s1 * (p3.x - p2.x) + s2 * (p1.x - p3.x) + s3 * (p2.x - p1.x)) / d};
    };
    if (type == "line") {
        out["start"] = pt(c[0].points[0]);
        out["end"] = pt(c[0].points[1]);
    } else if (type == "rectangle") {
        out.erase("width");
        out.erase("height");
        out["corner"] = pt(c[0].points[0]);
        out["opposite"] = pt(c[2].points[0]);
    } else if (type == "center_rectangle") {
        const Vec2 a = c[0].points[0], b = c[2].points[0];
        out["center"] = pt({(a.x + b.x) / 2, (a.y + b.y) / 2});
        // Signed, so the corners keep their places (a corner dragged past the opposite one).
        out["width"] = b.x - a.x;
        out["height"] = b.y - a.y;
    } else if (type == "three_point_rectangle") {
        const Vec2 a = c[0].points[0], b = c[0].points[1], e = c[1].points[1];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        out["start"] = pt(a);
        out["end"] = pt(b);
        // Signed distance of the far side, to the left of start -> end.
        out["height"] = len > 1e-12 ? ((b.x - a.x) * (e.y - b.y) - (b.y - a.y) * (e.x - b.x)) / len : 0.0;
    } else if (type == "circle") {
        out.erase("diameter");
        out["center"] = pt(c[0].points[0]);
        out["radius"] = c[0].radius;
    } else if (type == "arc") {
        out["start"] = pt(c[0].points[0]);
        out["through"] = pt(c[0].points[1]);
        out["end"] = pt(c[0].points[2]);
    } else if (type == "center_arc") {
        const Vec2 ctr = arcCenter(c[0]);
        auto ang = [&](const Vec2& p) { return std::atan2(p.y - ctr.y, p.x - ctr.x) / kDegToRad; };
        out["center"] = pt(ctr);
        out["radius"] = std::hypot(c[0].points[0].x - ctr.x, c[0].points[0].y - ctr.y);
        out["startAngle"] = ang(c[0].points[0]);
        out["endAngle"] = ang(c[0].points[2]);
    } else if (type == "polygon") {
        Vec2 ctr{0, 0};
        for (std::size_t i = 0; i < n; ++i)
            ctr = ctr + c[i].points[0] * (1.0 / static_cast<double>(n));
        const Vec2 v = c[0].points[0];
        out["center"] = pt(ctr);
        out["radius"] = std::hypot(v.x - ctr.x, v.y - ctr.y);
        out["angle"] = std::atan2(v.y - ctr.y, v.x - ctr.x) / kDegToRad;
        // Whatever way it was drawn, it is now an inscribed polygon with these numbers.
        for (const char* k : {"mode", "start", "end"})
            out.erase(k);
    } else if (type == "slot") {
        const Vec2 a = arcCenter(c[3]), b = arcCenter(c[1]);
        out["start"] = pt(a);
        out["end"] = pt(b);
        out["width"] = 2 * std::hypot(c[1].points[0].x - b.x, c[1].points[0].y - b.y);
        for (const char* k : {"mode", "center"})
            out.erase(k);
    } else if (type == "arc_slot") {
        const Vec2 ctr = arcCenter(c[0]);
        const double outer = std::hypot(c[0].points[0].x - ctr.x, c[0].points[0].y - ctr.y);
        const double inner = std::hypot(c[2].points[0].x - ctr.x, c[2].points[0].y - ctr.y);
        out["center"] = pt(ctr);
        out["radius"] = (outer + inner) / 2;
        out["width"] = outer - inner;
        out["start_angle"] = std::atan2(c[0].points[0].y - ctr.y, c[0].points[0].x - ctr.x) / kDegToRad;
        out["end_angle"] = std::atan2(c[0].points[2].y - ctr.y, c[0].points[2].x - ctr.x) / kDegToRad;
    } else if (type == "spline" || type == "control_spline") {
        Json pts = Json::array();
        for (const auto& q : c[0].points)
            pts.push_back(pt(q));
        out["points"] = pts;
        if (type == "control_spline") {
            // Once moved by the solver it is an ordinary spline through its sampled points.
            out["type"] = "spline";
            out["closed"] = c[0].closed;
            out.erase("degree");
        }
    } else if (type == "point") {
        out["at"] = pt(c[0].points[0]);
    } else if (type == "ellipse") {
        out = ellipseItem(c[0]);
        if (item.contains("construction"))
            out["construction"] = item["construction"];
    } else if (type == "elliptical_arc") {
        Json e = ellipticalArcItem(c[0]);
        for (const char* k : {"construction", "join_start", "join_end"})
            if (item.contains(k))
                e[k] = item[k];
        out = e;
    } else if (type == "text") {
        out = textItem(c[0]);
        for (const char* k : {"construction"})
            if (item.contains(k))
                out[k] = item[k];
    } else if (type == "airfoil") {
        // Moved or turned by the solver: its leading and trailing edge say where it is now.
        const Vec2 le = c[0].points.front(), te = c[0].points.back();
        out["at"] = pt(le);
        out["chord"] = std::hypot(te.x - le.x, te.y - le.y);
        out["angle"] = std::atan2(te.y - le.y, te.x - le.x) / kDegToRad;
    } else if (type == "conic") {
        out["start"] = pt(c[0].points[0]);
        out["apex"] = pt(c[0].points[1]);
        out["end"] = pt(c[0].points[2]);
    }
    return out;
}

bool sameCurve(const SketchCurve& a, const SketchCurve& b, double tol)
{
    if (a.kind != b.kind || a.points.size() != b.points.size() || std::abs(a.radius - b.radius) > tol)
        return false;
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        if (std::hypot(a.points[i].x - b.points[i].x, a.points[i].y - b.points[i].y) > tol)
            return false;
    }
    return true;
}

/// `out` (an item rewritten from solved curves) with the values of `item` that did not
/// change put back as written, so their expressions stay.
Json keepExpressions(const Eval& ev, const Json& item, Json out)
{
    auto equal = [](double a, double b) { return std::abs(a - b) <= 1e-7 * std::max(1.0, std::abs(a)); };
    // Also inside point arrays ([x, y], spline points), element by element.
    auto restore = [&](auto& self, const Json& written, Json& now) -> void {
        if (written.is_string() && now.is_number()) {
            if (equal(ev.num(written), now.get<double>()))
                now = written;
        } else if (written.is_array() && now.is_array() && written.size() == now.size()) {
            for (std::size_t i = 0; i < written.size(); ++i)
                self(self, written[i], now[i]);
        }
    };
    for (const auto& [key, value] : item.items()) {
        if (out.contains(key))
            restore(restore, value, out[key]);
    }
    if (has(item, "diameter") && out.contains("radius") && equal(ev.num(item.at("diameter")), 2 * ev.num(out.at("radius")))) {
        out.erase("radius");
        out["diameter"] = item.at("diameter");
    }
    // A rectangle drawn with width / height keeps them when its size did not change.
    if (has(item, "width") && has(item, "height") && out.contains("opposite") && out.contains("corner")) {
        const auto& a = out.at("corner");
        const auto& b = out.at("opposite");
        // The corner may hold restored expressions: evaluate, do not read numbers.
        if (equal(ev.num(item.at("width")), ev.num(b[0]) - ev.num(a[0])) &&
            equal(ev.num(item.at("height")), ev.num(b[1]) - ev.num(a[1]))) {
            out.erase("opposite");
            out["width"] = item.at("width");
            out["height"] = item.at("height");
        }
    }
    return out;
}

/// The curve ends the solver welds by position (SketchSolver's endPoint: line and arc ends
/// unless their join is off, open spline ends; an end joins an earlier one within 1e-6), as
/// pairs of end ids (curve * 2 + 0 for the start, + 1 for the end).
std::vector<std::pair<std::size_t, std::size_t>> weldedEnds(const std::vector<SketchCurve>& curves)
{
    struct End {
        Vec2 at;
        std::size_t id;
    };
    std::vector<End> ends;
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    auto end = [&](const Vec2& at, std::size_t id, bool join) {
        if (join) {
            for (const auto& e : ends) {
                if (std::hypot(e.at.x - at.x, e.at.y - at.y) < 1e-6) {
                    pairs.emplace_back(e.id, id);
                    return; // like endPoint: a joined end is not a position to join later ends to
                }
            }
        }
        ends.push_back({at, id});
    };
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const auto& c = curves[i];
        if (c.kind == SketchCurve::Kind::Line && c.points.size() == 2) {
            end(c.points[0], 2 * i, c.joinStart);
            end(c.points[1], 2 * i + 1, c.joinEnd);
        } else if (c.kind == SketchCurve::Kind::EllipticalArc && c.points.size() == 4) {
            end(c.points[0], 2 * i, c.joinStart);
            end(c.points[3], 2 * i + 1, c.joinEnd);
        } else if (c.kind == SketchCurve::Kind::Conic && c.points.size() == 3) {
            end(c.points[0], 2 * i, c.joinStart);
            end(c.points[2], 2 * i + 1, c.joinEnd);
        } else if (c.kind == SketchCurve::Kind::Arc && c.points.size() == 3) {
            end(c.points[0], 2 * i, c.joinStart);
            end(c.points[2], 2 * i + 1, c.joinEnd);
        } else if (c.kind == SketchCurve::Kind::Spline && !c.closed && !c.points.empty()) {
            end(c.points.front(), 2 * i, true);
            if (c.points.size() > 1)
                end(c.points.back(), 2 * i + 1, true);
        } else if (c.kind == SketchCurve::Kind::Point && c.points.size() == 1) {
            end(c.points[0], 2 * i, c.joinStart); // shared like a curve end (SketchSolver)
        }
    }
    return pairs;
}

/// Groups (union-find roots) of `n` elements joined by `pairs`.
std::vector<std::size_t> groupsOf(std::size_t n, const std::vector<std::pair<std::size_t, std::size_t>>& pairs)
{
    std::vector<std::size_t> g(n);
    for (std::size_t i = 0; i < n; ++i)
        g[i] = i;
    auto root = [&](std::size_t i) {
        while (g[i] != i)
            i = g[i] = g[g[i]];
        return i;
    };
    for (const auto& [a, b] : pairs)
        if (a < n && b < n)
            g[root(a)] = root(b);
    for (std::size_t i = 0; i < n; ++i)
        g[i] = root(i);
    return g;
}

/// The sketch items rewritten where `solved` (the curves they make, in order) has them.
/// Items whose curves match `shown` (the curves before the change; nullptr: none match)
/// stay as written. With `exact`, the solver's welds of curve ends (by stored position) are
/// kept as they are: an item that cannot make its solved curves again (a polygon pulled out
/// of regular) stays as written, and so do the items welded to it, and items whose
/// rewritten ends would weld to ends they were not welded to (two ends that only meet after
/// solving) or part from ends they were.
Json itemsFromSolved(const Eval& ev, const Json& items, const std::vector<SketchCurve>* shown,
                     const std::vector<SketchCurve>& solved, bool exact)
{
    const std::size_t n = items.size();
    std::vector<Json> out(n);
    std::vector<std::vector<SketchCurve>> drawnOf(n), nowOf(n); // as written / as rewritten
    std::vector<bool> keep(n, false);
    std::vector<std::size_t> itemOf; // curve -> item
    std::size_t first = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const Json& item = items[k];
        std::vector<SketchConstraint> implicit;
        appendCurves(ev, item, drawnOf[k], implicit);
        const std::size_t m = drawnOf[k].size();
        if (first + m > solved.size())
            throw DesignError("Sketch changed while it was solved");
        bool moved = shown == nullptr || shown->size() != solved.size();
        for (std::size_t i = 0; i < m && !moved; ++i)
            moved = !sameCurve((*shown)[first + i], solved[first + i], 1e-7);
        out[k] = item;
        nowOf[k] = drawnOf[k];
        if (moved) {
            out[k] = keepExpressions(ev, item, itemFromSolved(item, solved.data() + first, m));
            if (exact) {
                std::vector<SketchCurve> again;
                std::vector<SketchConstraint> unused;
                try {
                    appendCurves(ev, out[k], again, unused);
                } catch (const DesignError&) {
                    again.clear();
                }
                bool same = again.size() == m;
                for (std::size_t i = 0; i < m && same; ++i)
                    same = sameCurve(again[i], solved[first + i], 1e-6);
                if (same)
                    nowOf[k] = std::move(again);
                else
                    keep[k] = true;
            }
        }
        for (std::size_t i = 0; i < m; ++i)
            itemOf.push_back(k);
        first += m;
    }
    if (exact) {
        std::vector<SketchCurve> drawn;
        for (const auto& d : drawnOf)
            drawn.insert(drawn.end(), d.begin(), d.end());
        const auto drawnPairs = weldedEnds(drawn);
        const auto drawnGroups = groupsOf(2 * drawn.size(), drawnPairs);
        // Items welded together as drawn: a kept item keeps its whole group as written.
        const auto itemGroups = [&] {
            std::vector<std::pair<std::size_t, std::size_t>> p;
            for (const auto& [a, b] : drawnPairs)
                p.emplace_back(itemOf[a / 2], itemOf[b / 2]);
            return groupsOf(n, p);
        }();
        for (bool changed = true; changed;) {
            changed = false;
            std::set<std::size_t> keptGroups;
            for (std::size_t k = 0; k < n; ++k)
                if (keep[k])
                    keptGroups.insert(itemGroups[k]);
            for (std::size_t k = 0; k < n; ++k)
                if (!keep[k] && keptGroups.count(itemGroups[k]) != 0)
                    keep[k] = changed = true;
            std::vector<SketchCurve> now;
            for (std::size_t k = 0; k < n; ++k) {
                const auto& c = keep[k] ? drawnOf[k] : nowOf[k];
                now.insert(now.end(), c.begin(), c.end());
            }
            const auto nowPairs = weldedEnds(now);
            const auto nowGroups = groupsOf(2 * now.size(), nowPairs);
            auto keepBoth = [&](std::size_t a, std::size_t b) {
                for (const std::size_t e : {a, b}) {
                    if (!keep[itemOf[e / 2]])
                        keep[itemOf[e / 2]] = changed = true;
                }
            };
            for (const auto& [a, b] : nowPairs)
                if (drawnGroups[a] != drawnGroups[b])
                    keepBoth(a, b); // would weld ends that were apart
            for (const auto& [a, b] : drawnPairs)
                if (nowGroups[a] != nowGroups[b])
                    keepBoth(a, b); // would part welded ends
        }
        for (std::size_t k = 0; k < n; ++k)
            if (keep[k])
                out[k] = items[k];
    }
    Json newItems = Json::array();
    for (auto& o : out)
        newItems.push_back(std::move(o));
    return newItems;
}

} // namespace

void Design::keepSolvedSketch(FeatureId sketchId)
{
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch" || !f.params.contains("curves"))
        return;
    const auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        return; // not evaluated (rolled back, suppressed or failed): nothing is shown
    const Eval ev{*this};
    const Json& items = f.params.at("curves");
    std::size_t total = 0;
    try {
        for (const auto& item : items) {
            std::vector<SketchCurve> made;
            std::vector<SketchConstraint> implicit;
            appendCurves(ev, item, made, implicit);
            total += made.size();
        }
    } catch (const DesignError&) {
        return;
    }
    if (total != it->second.curves.size())
        return;
    // Where the items put the curves before solving: unchanged items stay as written.
    std::vector<SketchCurve> drawn;
    std::vector<SketchConstraint> implicit;
    for (const auto& item : items)
        appendCurves(ev, item, drawn, implicit);
    Json newItems = itemsFromSolved(ev, items, &drawn, it->second.curves, true);
    if (newItems != items)
        f.params["curves"] = std::move(newItems);
}

void Design::moveSketchPoint(FeatureId sketchId, const Json& point, const geometry::Vec2& to)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    if (!point.is_object() || !point.contains("curve"))
        throw DesignError("Give the point to drag as {curve, point}");
    const Json items = f.params.value("curves", Json::array());
    const Json constraints = f.params.value("constraints", Json::array());
    const Eval ev{*this};
    // Where the curves are before the drag, to leave the items that do not move as they
    // are (with their parameter expressions).
    std::vector<SketchCurve> original;
    if (auto o = m_sketches.find(sketchId); o != m_sketches.end())
        original = o->second.curves;

    // 1. Solve with the point held at the cursor. A polygon keeps its corner angles too, so
    // a dragged vertex leaves it regular (its sides alone are only kept equal).
    Json held = constraints;
    held.push_back({{"type", "fix"}, {"points", Json::array({point})}, {"at", Json::array({Json::array({to.x, to.y})})}});
    {
        std::size_t at = 0;
        for (const auto& item : items) {
            std::vector<SketchCurve> made;
            std::vector<SketchConstraint> implicit;
            appendCurves(ev, item, made, implicit);
            if (lower(item.at("type").get<std::string>()) == "polygon") {
                const double turn = 360.0 / static_cast<double>(made.size());
                for (std::size_t i = 0; i + 1 < made.size(); ++i)
                    held.push_back({{"type", "angle"}, {"curves", Json::array({at + i, at + i + 1})}, {"value", turn}});
            }
            at += made.size();
        }
    }
    f.params["constraints"] = held;
    regenerate();
    const FeatureStatus st = status(sketchId);
    auto it = m_sketches.find(sketchId);
    if (st.state == FeatureStatus::State::Error || it == m_sketches.end()) {
        m_state = before;
        regenerate();
        throw DesignError("That point cannot move there: its constraints hold it (" + st.message + ")");
    }
    const std::vector<SketchCurve> solved = it->second.curves;

    // 2. Keep the result: each item that moved rewritten where its curves are now, without
    // the hold; values that did not change keep their expressions.
    const Json newItems = itemsFromSolved(ev, items, &original, solved, false);
    Feature& g = m_state.features[indexOf(sketchId)];
    g.params["curves"] = newItems;
    if (constraints.empty() && !before.features[indexOf(sketchId)].params.contains("constraints"))
        g.params.erase("constraints");
    else
        g.params["constraints"] = constraints;
    regenerateChecked(sketchId, before);
}

namespace {

/// An axis-aligned rectangle item (`c`: its four solved sides) as a 3-point rectangle with
/// the same corners and side order, which keeps its shape at any angle.
Json asThreePointRectangle(const Json& item, const SketchCurve* c)
{
    const Vec2 a = c[0].points[0], b = c[0].points[1], e = c[1].points[1];
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    Json out{{"type", "three_point_rectangle"},
             {"start", Json::array({a.x, a.y})},
             {"end", Json::array({b.x, b.y})},
             {"height", len > 1e-12 ? ((b.x - a.x) * (e.y - b.y) - (b.y - a.y) * (e.x - b.x)) / len : 0.0}};
    if (item.value("construction", false))
        out["construction"] = true;
    return out;
}

/// The constraint point references that pin down a curve's position.
std::vector<Json> curvePointRefs(const SketchCurve& c, std::size_t i)
{
    using K = SketchCurve::Kind;
    auto ref = [&](const char* name) { return Json{{"curve", i}, {"point", name}}; };
    switch (c.kind) {
    case K::Line: return {ref("start"), ref("end")};
    case K::Arc: return {ref("start"), ref("end"), ref("center")};
    case K::Circle: return {ref("center")};
    case K::Point: return {ref("start")};
    case K::Ellipse: return {ref("center"), ref("end")};
    // An elliptical arc's ends and centre do not fix its axis when its ends are the minor
    // axis tips, so its axis end is held too.
    case K::EllipticalArc: return {ref("start"), ref("end"), ref("center"), ref("axis")};
    case K::Conic: return {ref("start"), ref("end"), ref("apex")};
    // A text's start and its height point (size and angle).
    case K::Text: return {ref("start"), ref("end")};
    case K::Spline: {
        std::vector<Json> out;
        for (std::size_t k = 0; k < c.points.size(); ++k)
            out.push_back(Json{{"curve", i}, {"point", "fit"}, {"index", k}});
        return out;
    }
    }
    return {};
}

/// Where a constraint point reference of a solved curve is (start, end, center, fit).
Vec2 curvePointAt(const SketchCurve& c, const Json& ref)
{
    using K = SketchCurve::Kind;
    const std::string name = ref.at("point").get<std::string>();
    if (name == "fit")
        return c.points.at(ref.at("index").get<std::size_t>());
    if (c.kind == K::Point || c.kind == K::Circle)
        return c.points[0];
    if (c.kind == K::Ellipse)
        return name == "center" ? c.points[0] : c.points[1];
    if (c.kind == K::EllipticalArc && name == "center")
        return c.points[1];
    if (c.kind == K::EllipticalArc && name == "axis")
        return c.points[2];
    if (c.kind == K::Conic)
        return name == "apex" ? c.points[1] : name == "end" ? c.points[2] : c.points[0];
    if (name == "center") {
        const Vec2 p1 = c.points[0], p2 = c.points[1], p3 = c.points[2];
        const double d = 2 * (p1.x * (p2.y - p3.y) + p2.x * (p3.y - p1.y) + p3.x * (p1.y - p2.y));
        const double s1 = p1.x * p1.x + p1.y * p1.y, s2 = p2.x * p2.x + p2.y * p2.y, s3 = p3.x * p3.x + p3.y * p3.y;
        return Vec2{(s1 * (p2.y - p3.y) + s2 * (p3.y - p1.y) + s3 * (p1.y - p2.y)) / d,
                    (s1 * (p3.x - p2.x) + s2 * (p1.x - p3.x) + s3 * (p2.x - p1.x)) / d};
    }
    return name == "start" ? c.points.front() : c.points.back();
}

/// The same curve where it matters: an arc by its ends and centre (the solver may put its
/// through point anywhere on it), anything else point by point.
bool sameShape(const SketchCurve& a, const SketchCurve& b, double tol)
{
    if (a.kind != SketchCurve::Kind::Arc || b.kind != SketchCurve::Kind::Arc)
        return sameCurve(a, b, tol);
    const Json c{{"point", "center"}};
    auto close = [&](const Vec2& p, const Vec2& q) { return std::hypot(p.x - q.x, p.y - q.y) <= tol; };
    return close(a.points.front(), b.points.front()) && close(a.points.back(), b.points.back()) &&
           close(curvePointAt(a, c), curvePointAt(b, c));
}

// A point moved to a placement: pivot + scale R(angle) (p - pivot) + offset.
Vec2 placedPoint(const Vec2& p, const SketchPlacement& place)
{
    const double a = place.angleDegrees * kDegToRad;
    const double ca = std::cos(a) * place.scale, sa = std::sin(a) * place.scale;
    const Vec2 d = p - place.pivot;
    return Vec2{place.pivot.x + ca * d.x - sa * d.y + place.offset.x, place.pivot.y + sa * d.x + ca * d.y + place.offset.y};
}

// `c` moved to a placement (its radius scaled too; a conic's rho is a ratio and stays).
SketchCurve placedCurve(SketchCurve c, const SketchPlacement& place)
{
    for (auto& p : c.points)
        p = placedPoint(p, place);
    if (c.kind != SketchCurve::Kind::Conic)
        c.radius *= place.scale;
    return c;
}

// A length dimension (distance, length, radius, ...) scaled by `s`: a number is multiplied,
// an expression wrapped ("(w/2)*0.5"). Other constraints are left alone.
void scaleDimension(Json& c, double s)
{
    static const std::set<std::string> lengths{"distance", "horizontal_distance", "vertical_distance",
                                               "length", "radius", "diameter", "major_radius",
                                               "minor_radius"};
    if (s == 1.0 || !c.contains("value") || !lengths.contains(lower(c.value("type", std::string()))))
        return;
    Json& v = c["value"];
    if (v.is_number())
        v = v.get<double>() * s;
    else if (v.is_string()) {
        std::ostringstream out;
        out.precision(17);
        out << "(" << v.get<std::string>() << ")*" << s;
        v = out.str();
    }
}

// Each sketch item's curves as [first, first + count), in sketch.curves order.
std::vector<std::pair<std::size_t, std::size_t>> itemRanges(const Eval& ev, const Json& items, std::size_t curveCount,
                                                            const std::string& name)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::size_t first = 0;
    for (const auto& item : items) {
        std::vector<SketchCurve> made;
        std::vector<SketchConstraint> implicit;
        appendCurves(ev, item, made, implicit);
        ranges.emplace_back(first, made.size());
        first += made.size();
    }
    if (first != curveCount)
        throw DesignError(name + " changed while it was solved");
    return ranges;
}


} // namespace

void Design::stretchSketch(FeatureId sketchId, const geometry::Vec2& windowMin, const geometry::Vec2& windowMax,
                           const geometry::Vec2& delta)
{
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y))
        throw DesignError("The stretch distance must be a number");
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    const std::vector<SketchCurve> curves = it->second.curves;
    const double lox = std::min(windowMin.x, windowMax.x), hix = std::max(windowMin.x, windowMax.x);
    const double loy = std::min(windowMin.y, windowMax.y), hiy = std::max(windowMin.y, windowMax.y);
    constexpr double tol = 1e-9;
    auto inside = [&](const Vec2& q) { return q.x >= lox - tol && q.x <= hix + tol && q.y >= loy - tol && q.y <= hiy + tol; };

    // Hold every point inside the window where it goes; the rest is left to the solver, so curves
    // crossing the window edge stretch (a line keeps its far end, a rectangle its other corners)
    // and curves wholly inside move. Fusion: Sketch > Modify > Stretch.
    Json constraints = f.params.value("constraints", Json::array());
    Json held = constraints;
    std::size_t holds = 0;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const auto refs = curvePointRefs(curves[i], i);
        bool all = !refs.empty();
        for (const Json& ref : refs)
            all = all && inside(curvePointAt(curves[i], ref));
        for (const Json& ref : refs) {
            const Vec2 at = curvePointAt(curves[i], ref);
            if (!inside(at))
                continue;
            const Vec2 to{at.x + delta.x, at.y + delta.y};
            held.push_back({{"type", "fix"}, {"points", Json::array({ref})}, {"at", Json::array({Json::array({to.x, to.y})})}});
            ++holds;
        }
        if (all && curves[i].kind == SketchCurve::Kind::Circle)
            held.push_back({{"type", "radius"}, {"curve", i}, {"value", curves[i].radius}});
        if (all && (curves[i].kind == SketchCurve::Kind::Ellipse || curves[i].kind == SketchCurve::Kind::EllipticalArc))
            held.push_back({{"type", "minor_radius"}, {"curve", i}, {"value", curves[i].radius}});
    }
    if (holds == 0)
        throw DesignError("The window holds no sketch points: drag it over the geometry to stretch");
    const Eval ev{*this};
    Json items = f.params.value("curves", Json::array());
    const auto ranges = itemRanges(ev, items, curves.size(), f.name);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (lower(items[k].at("type").get<std::string>()) != "polygon")
            continue;
        const auto [first, count] = ranges[k];
        const double turn = 360.0 / static_cast<double>(count);
        for (std::size_t i = 0; i + 1 < count; ++i)
            held.push_back({{"type", "angle"}, {"curves", Json::array({first + i, first + i + 1})}, {"value", turn}});
    }
    f.params["constraints"] = held;
    regenerate();
    const FeatureStatus st = status(sketchId);
    it = m_sketches.find(sketchId);
    if (st.state == FeatureStatus::State::Error || it == m_sketches.end()) {
        m_state = before;
        regenerate();
        throw DesignError("That cannot stretch: the constraints hold the geometry (" + st.message + ")");
    }
    const Json newItems = itemsFromSolved(ev, items, &curves, it->second.curves, false);
    Feature& g = m_state.features[indexOf(sketchId)];
    g.params["curves"] = newItems;
    if (constraints.empty() && !before.features[indexOf(sketchId)].params.contains("constraints"))
        g.params.erase("constraints");
    else
        g.params["constraints"] = constraints;
    regenerateChecked(sketchId, before);
}

void Design::moveSketchCurves(FeatureId sketchId, const std::vector<std::size_t>& picked, const SketchPlacement& place,
                              bool copy)
{
    if (!(place.scale > 0.0) || !std::isfinite(place.scale))
        throw DesignError("The scale factor must be more than 0");
    if (copy) {
        patternSketchCurves(sketchId, picked, {place});
        return;
    }
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    const std::vector<SketchCurve> curves = it->second.curves;
    std::vector<bool> sel(curves.size(), false);
    for (const std::size_t i : picked) {
        if (i >= curves.size())
            throw DesignError(f.name + " has no curve " + std::to_string(i));
        sel[i] = true;
    }
    if (picked.empty())
        throw DesignError("Select the sketch curves to move");
    const bool turned = place.angleDegrees != 0.0;
    const bool scaled = place.scale != 1.0;
    const std::string cannot = scaled ? "Those curves cannot be scaled" : "Those curves cannot move there";

    const Eval ev{*this};
    Json items = f.params.value("curves", Json::array());
    // Scaled, the dimensions among the picked curves scale with them (Fusion: Sketch Scale).
    Json constraints = f.params.value("constraints", Json::array());
    if (scaled) {
        std::vector<std::optional<std::size_t>> self(curves.size());
        for (std::size_t i = 0; i < curves.size(); ++i)
            if (sel[i])
                self[i] = i;
        // Only dimensions wholly among them: one to the sketch origin positions the curves
        // and stays.
        for (auto& c : constraints) {
            Json probe = c;
            if (c.dump().find("\"origin\"") == std::string::npos && remapConstraint(probe, self))
                scaleDimension(c, place.scale);
        }
    }
    const auto ranges = itemRanges(ev, items, curves.size(), f.name);
    auto whole = [&](std::size_t k) {
        for (std::size_t i = 0; i < ranges[k].second; ++i)
            if (!sel[ranges[k].first + i])
                return false;
        return ranges[k].second > 0;
    };
    // A rectangle drawn along the axes keeps horizontal and vertical sides; turned, it
    // becomes a 3-point rectangle (same corners and sides).
    auto turnable = [&](std::size_t k) {
        const std::string type = lower(items[k].at("type").get<std::string>());
        if (turned && (type == "rectangle" || type == "center_rectangle"))
            return asThreePointRectangle(items[k], curves.data() + ranges[k].first);
        return items[k];
    };

    // Move: hold every point of the picked curves where it goes and let the solver move the
    // rest (connected curves stretch), as a drag does.
    for (std::size_t k = 0; k < items.size(); ++k)
        if (whole(k))
            items[k] = turnable(k);
    Json held = constraints;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (!sel[i])
            continue;
        for (const Json& ref : curvePointRefs(curves[i], i)) {
            const Vec2 to = placedPoint(curvePointAt(curves[i], ref), place);
            held.push_back({{"type", "fix"}, {"points", Json::array({ref})}, {"at", Json::array({Json::array({to.x, to.y})})}});
        }
        // A circle keeps its size too (its centre alone does not hold it).
        if (curves[i].kind == SketchCurve::Kind::Circle)
            held.push_back({{"type", "radius"}, {"curve", i}, {"value", curves[i].radius * place.scale}});
        if (curves[i].kind == SketchCurve::Kind::Ellipse || curves[i].kind == SketchCurve::Kind::EllipticalArc)
            held.push_back({{"type", "minor_radius"}, {"curve", i}, {"value", curves[i].radius * place.scale}});
    }
    // A polygon keeps its corner angles too (its sides alone are only kept equal), as in a
    // point drag, so it stays regular while one side moves.
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (lower(items[k].at("type").get<std::string>()) != "polygon")
            continue;
        const auto [first, count] = ranges[k];
        const double turn = 360.0 / static_cast<double>(count);
        for (std::size_t i = 0; i + 1 < count; ++i)
            held.push_back({{"type", "angle"}, {"curves", Json::array({first + i, first + i + 1})}, {"value", turn}});
    }
    f.params["curves"] = items;
    f.params["constraints"] = held;
    regenerate();
    const FeatureStatus st = status(sketchId);
    it = m_sketches.find(sketchId);
    if (st.state == FeatureStatus::State::Error || it == m_sketches.end()) {
        m_state = before;
        regenerate();
        throw DesignError(cannot + ": their constraints hold them (" + st.message + ")");
    }
    const Json newItems = itemsFromSolved(ev, items, &curves, it->second.curves, false);
    Feature& g = m_state.features[indexOf(sketchId)];
    g.params["curves"] = newItems;
    if (constraints.empty() && !before.features[indexOf(sketchId)].params.contains("constraints"))
        g.params.erase("constraints");
    else
        g.params["constraints"] = constraints;
    regenerateChecked(sketchId, before);
    // Rewritten as items, the picked curves must still be where they were asked to go.
    it = m_sketches.find(sketchId);
    for (std::size_t i = 0; i < curves.size() && it != m_sketches.end(); ++i) {
        if (sel[i] && (i >= it->second.curves.size() || !sameShape(it->second.curves[i], placedCurve(curves[i], place), 1e-6))) {
            m_state = before;
            regenerate();
            throw DesignError(cannot + ": their shape holds them");
        }
    }
}

void Design::patternSketchCurves(FeatureId sketchId, const std::vector<std::size_t>& picked,
                                 const std::vector<SketchPlacement>& placements)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    const std::vector<SketchCurve> curves = it->second.curves;
    std::vector<bool> sel(curves.size(), false);
    for (const std::size_t i : picked) {
        if (i >= curves.size())
            throw DesignError(f.name + " has no curve " + std::to_string(i));
        sel[i] = true;
    }
    if (picked.empty())
        throw DesignError("Select the sketch curves to copy");
    if (placements.empty())
        throw DesignError("Nothing to copy: no placements");

    const Eval ev{*this};
    Json items = f.params.value("curves", Json::array());
    const Json constraints = f.params.value("constraints", Json::array());
    const auto ranges = itemRanges(ev, items, curves.size(), f.name);
    auto whole = [&](std::size_t k) {
        for (std::size_t i = 0; i < ranges[k].second; ++i)
            if (!sel[ranges[k].first + i])
                return false;
        return ranges[k].second > 0;
    };

    // One copy per placement, each at the end: whole items stay items (a rectangle stays a
    // rectangle, turned it becomes a 3-point rectangle); single curves of a shape become
    // lines, arcs or points. Constraints among the copied curves come along; ones tied to
    // the origin or that fix a point stay with the originals, and so do directions along
    // the sketch axes (horizontal, a horizontal distance) when the copy is turned.
    Json all = constraints;
    std::vector<std::vector<std::optional<std::size_t>>> maps;
    std::size_t next = curves.size();
    for (const SketchPlacement& place : placements) {
        const bool turned = place.angleDegrees != 0.0;
        std::vector<std::optional<std::size_t>> map(curves.size());
        for (std::size_t k = 0; k < ranges.size(); ++k) {
            const auto [first, count] = ranges[k];
            if (whole(k)) {
                const std::string type = lower(items[k].at("type").get<std::string>());
                const Json item = turned && (type == "rectangle" || type == "center_rectangle")
                                      ? asThreePointRectangle(items[k], curves.data() + first)
                                      : items[k];
                std::vector<SketchCurve> now;
                for (std::size_t i = 0; i < count; ++i)
                    now.push_back(placedCurve(curves[first + i], place));
                items.push_back(itemFromSolved(item, now.data(), count));
                for (std::size_t i = 0; i < count; ++i)
                    map[first + i] = next++;
                continue;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!sel[first + i])
                    continue;
                items.push_back(curveItem(placedCurve(curves[first + i], place)));
                map[first + i] = next++;
            }
        }
        for (auto c : constraints) {
            const std::string type = lower(c.value("type", std::string()));
            if (type == "fix" || c.dump().find("\"origin\"") != std::string::npos)
                continue;
            if (turned && (type == "horizontal" || type == "vertical" || type == "horizontal_distance" ||
                           type == "vertical_distance"))
                continue;
            if (remapConstraint(c, map)) {
                scaleDimension(c, place.scale);
                all.push_back(c);
            }
        }
        maps.push_back(std::move(map));
    }
    f.params["curves"] = items;
    if (!all.empty())
        f.params["constraints"] = all;
    regenerateChecked(sketchId, before);
    // The copies must be where they were asked to go (as the preview shows them).
    it = m_sketches.find(sketchId);
    for (std::size_t n = 0; n < placements.size(); ++n) {
        for (std::size_t i = 0; i < curves.size() && it != m_sketches.end(); ++i) {
            if (!maps[n][i])
                continue;
            if (*maps[n][i] >= it->second.curves.size() ||
                !sameShape(it->second.curves[*maps[n][i]], placedCurve(curves[i], placements[n]), 1e-6)) {
                m_state = before;
                regenerate();
                throw DesignError("The copied curves' constraints do not let them sit there");
            }
        }
    }
}

void Design::mirrorSketchCurves(FeatureId sketchId, const std::vector<std::size_t>& picked, std::size_t lineIdx)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    const std::vector<SketchCurve> curves = it->second.curves;
    if (lineIdx >= curves.size() || curves[lineIdx].kind != SketchCurve::Kind::Line)
        throw DesignError("Pick a line of " + f.name + " to mirror about");
    const Vec2 a = curves[lineIdx].points[0], b = curves[lineIdx].points[1];
    const Vec2 d = b - a;
    const double l2 = d.x * d.x + d.y * d.y;
    if (l2 < 1e-18)
        throw DesignError("The mirror line has no length");
    auto mirror = [&](const Vec2& p) {
        const double t = ((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2;
        const Vec2 foot = a + d * t;
        return foot * 2.0 - p;
    };
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    std::vector<std::size_t> list;
    for (const std::size_t i : picked) {
        if (i >= curves.size())
            throw DesignError(f.name + " has no curve " + std::to_string(i));
        if (i != lineIdx && std::find(list.begin(), list.end(), i) == list.end())
            list.push_back(i);
    }
    if (list.empty())
        throw DesignError("Select the curves to mirror (not only the mirror line)");

    Json items = f.params.value("curves", Json::array());
    Json constraints = f.params.value("constraints", Json::array());
    std::size_t next = curves.size();
    for (const std::size_t i : list) {
        const SketchCurve& c = curves[i];
        Json item;
        std::vector<const char*> points;
        bool swappedEnds = false;
        switch (c.kind) {
        case SketchCurve::Kind::Line:
            item = {{"type", "line"}, {"start", pt(mirror(c.points[0]))}, {"end", pt(mirror(c.points[1]))}};
            points = {"start", "end"};
            break;
        case SketchCurve::Kind::Arc:
            item = {{"type", "arc"},
                    {"start", pt(mirror(c.points[0]))},
                    {"through", pt(mirror(c.points[1]))},
                    {"end", pt(mirror(c.points[2]))}};
            points = {"start", "end", "center"};
            break;
        case SketchCurve::Kind::Circle:
            item = {{"type", "circle"}, {"center", pt(mirror(c.points[0]))}, {"radius", c.radius}};
            points = {"center"};
            break;
        case SketchCurve::Kind::Spline: {
            Json p = Json::array();
            for (const auto& q : c.points)
                p.push_back(pt(mirror(q)));
            item = {{"type", "spline"}, {"points", p}, {"closed", c.closed}};
            points = {"start", "end"}; // its ends follow; inner fit points cannot be constrained
            break;
        }
        case SketchCurve::Kind::Point:
            item = {{"type", "point"}, {"at", pt(mirror(c.points[0]))}};
            points = {"start"};
            break;
        case SketchCurve::Kind::Ellipse: {
            SketchCurve m = c;
            m.points = {mirror(c.points[0]), mirror(c.points[1])};
            item = ellipseItem(m);
            points = {"center", "end"};
            break;
        }
        case SketchCurve::Kind::EllipticalArc: {
            // Mirrored, counter-clockwise turns clockwise: the copy runs from the mirrored
            // end to the mirrored start.
            SketchCurve m = c;
            m.points = {mirror(c.points[3]), mirror(c.points[1]), mirror(c.points[2]), mirror(c.points[0])};
            item = ellipticalArcItem(m);
            points = {"center"};
            swappedEnds = true;
            break;
        }
        case SketchCurve::Kind::Text:
            throw DesignError("Mirror does not work on text");
        case SketchCurve::Kind::Conic: {
            // A conic has no direction to keep: the mirrored points are the mirrored conic.
            SketchCurve m = c;
            m.points = {mirror(c.points[0]), mirror(c.points[1]), mirror(c.points[2])};
            item = conicItem(m);
            points = {"start", "end", "apex"};
            break;
        }
        }
        if (c.construction)
            item["construction"] = true;
        items.push_back(item);
        for (const char* name : points)
            constraints.push_back({{"type", "symmetric"},
                                   {"points", Json::array({{{"curve", i}, {"point", name}}, {{"curve", next}, {"point", name}}})},
                                   {"curve", lineIdx}});
        if (swappedEnds)
            for (const auto& [from, to] : {std::pair{"start", "end"}, std::pair{"end", "start"}})
                constraints.push_back({{"type", "symmetric"},
                                       {"points", Json::array({{{"curve", i}, {"point", from}}, {{"curve", next}, {"point", to}}})},
                                       {"curve", lineIdx}});
        if (c.kind == SketchCurve::Kind::Circle || c.kind == SketchCurve::Kind::Arc ||
            c.kind == SketchCurve::Kind::Ellipse || c.kind == SketchCurve::Kind::EllipticalArc)
            constraints.push_back({{"type", "equal"}, {"curves", Json::array({i, next})}});
        ++next;
    }
    f.params["curves"] = items;
    f.params["constraints"] = constraints;
    regenerateChecked(sketchId, before);
}

namespace {
/// Adds projected or intersected curves to a sketch feature's items, fixed where they are (Fusion keeps
/// such geometry fixed), with a radius dimension on circles and arcs. `next` is the index the first one gets.
void appendFixedCurves(Feature& f, std::size_t next, const std::vector<SketchCurve>& made)
{
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    Json items = f.params.value("curves", Json::array());
    Json constraints = f.params.value("constraints", Json::array());
    for (const auto& c : made) {
        if (c.kind == SketchCurve::Kind::Circle) {
            items.push_back({{"type", "circle"}, {"center", pt(c.points[0])}, {"radius", c.radius}});
        } else if (c.kind == SketchCurve::Kind::Spline) {
            Json p = Json::array();
            for (const auto& q : c.points)
                p.push_back(pt(q));
            items.push_back({{"type", "spline"}, {"points", p}, {"closed", c.closed}});
        } else {
            items.push_back(curveItem(c));
        }
        // Fusion keeps projected geometry fixed.
        constraints.push_back({{"type", "fix"}, {"curve", next}});
        if (c.kind == SketchCurve::Kind::Circle || c.kind == SketchCurve::Kind::Arc)
            constraints.push_back({{"type", "radius"}, {"curve", next}, {"value",
                c.kind == SketchCurve::Kind::Circle ? c.radius
                    : std::hypot(c.points[0].x - circleThrough(c.points[0], c.points[1], c.points[2]).x,
                                 c.points[0].y - circleThrough(c.points[0], c.points[1], c.points[2]).y)}});
        ++next;
    }
    f.params["curves"] = items;
    f.params["constraints"] = constraints;
}
} // namespace

void Design::intersectToSketch(FeatureId sketchId, FeatureId bodyId)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto sk = m_sketches.find(sketchId);
    auto body = m_bodies.find(bodyId);
    if (sk == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    if (body == m_bodies.end())
        throw DesignError("No body " + std::to_string(bodyId));
    std::vector<SketchCurve> made;
    try {
        made = geometry::intersectCurves(body->second.shape, sk->second.plane);
    } catch (const geometry::ModelingError& e) {
        throw DesignError(e.what());
    }
    if (made.empty())
        throw DesignError("The sketch plane does not cut " + body->second.name);
    appendFixedCurves(f, sk->second.curves.size(), made);
    regenerateChecked(sketchId, before);
}

void Design::projectToSketch(FeatureId sketchId, FeatureId bodyId, const Json& edgeRefs)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto sk = m_sketches.find(sketchId);
    auto body = m_bodies.find(bodyId);
    if (sk == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    if (body == m_bodies.end())
        throw DesignError("No body " + std::to_string(bodyId));
    const Eval ev{*this};
    std::vector<std::size_t> idx;
    if (!edgeRefs.is_array() || edgeRefs.empty())
        throw DesignError("Select at least one edge");
    for (const auto& item : edgeRefs) {
        if (item.is_number_integer()) {
            idx.push_back(item.get<std::size_t>());
        } else {
            const auto i = matchTopology(body->second.shape, item, true, ev);
            if (!i)
                throw DesignError("No edge near " + item.dump());
            idx.push_back(*i);
        }
    }
    std::vector<SketchCurve> made;
    try {
        made = geometry::projectEdges(body->second.shape, idx, sk->second.plane);
    } catch (const geometry::ModelingError& e) {
        throw DesignError(e.what());
    }
    if (made.empty())
        throw DesignError("The edges are seen end-on from this sketch: nothing to project");
    appendFixedCurves(f, sk->second.curves.size(), made);
    regenerateChecked(sketchId, before);
}

void Design::offsetSketchCurves(FeatureId sketchId, const std::vector<std::size_t>& picked, double distance)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    std::vector<SketchCurve> chain;
    for (const std::size_t i : picked) {
        if (i >= it->second.curves.size())
            throw DesignError(f.name + " has no curve " + std::to_string(i));
        chain.push_back(it->second.curves[i]);
    }
    std::vector<SketchCurve> made;
    try {
        made = geometry::offsetCurves(chain, distance);
    } catch (const geometry::ModelingError& e) {
        throw DesignError(e.what());
    }
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    Json items = f.params.value("curves", Json::array());
    for (const auto& c : made) {
        if (c.kind == SketchCurve::Kind::Circle) {
            items.push_back({{"type", "circle"}, {"center", pt(c.points[0])}, {"radius", c.radius}});
        } else if (c.kind == SketchCurve::Kind::Spline) {
            Json p = Json::array();
            for (const auto& q : c.points)
                p.push_back(pt(q));
            Json item{{"type", "spline"}, {"points", p}};
            if (c.closed)
                item["closed"] = true;
            items.push_back(item);
        } else {
            items.push_back(curveItem(c));
        }
    }
    f.params["curves"] = items;
    regenerateChecked(sketchId, before);
}

namespace {

/// Where two sketch lines meet: the shared end and each line's far end.
struct SketchCorner {
    Vec2 corner, farA, farB;
};

std::optional<SketchCorner> sketchCorner(const SketchCurve& la, const SketchCurve& lb)
{
    auto close = [](const Vec2& p, const Vec2& q) { return std::hypot(p.x - q.x, p.y - q.y) < 1e-6; };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            if (close(la.points[i], lb.points[j]))
                return SketchCorner{la.points[i], la.points[1 - i], lb.points[1 - j]};
    return std::nullopt;
}

/// Horizontal or vertical when the line `l` is drawn so (kept when it is shortened).
std::optional<Json> axisConstraint(const SketchCurve& l, std::size_t index)
{
    const double dx = l.points[1].x - l.points[0].x, dy = l.points[1].y - l.points[0].y;
    if (std::abs(dy) < 1e-9 * std::max(1.0, std::abs(dx)))
        return Json{{"type", "horizontal"}, {"curve", index}};
    if (std::abs(dx) < 1e-9 * std::max(1.0, std::abs(dy)))
        return Json{{"type", "vertical"}, {"curve", index}};
    return std::nullopt;
}

/// The collinear constraints that tie line `ia` or `ib` to another curve, as (0 for `ia` or
/// 1 for `ib`, the other curve's index once `ia` and `ib` are deleted). A fillet or chamfer
/// replaces both lines with shortened ones and puts these back on them, so an earlier
/// chamfer on the neighbouring corner stays tied to its side.
std::vector<std::pair<std::size_t, std::size_t>> collinearPartners(const Json& constraints, std::size_t ia,
                                                                   std::size_t ib)
{
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const auto& c : constraints) {
        if (!c.is_object() || c.value("type", std::string()) != "collinear" || !c.contains("curves") ||
            !c["curves"].is_array() || c["curves"].size() != 2 || !c["curves"][0].is_number_integer() ||
            !c["curves"][1].is_number_integer())
            continue;
        const std::size_t a = c["curves"][0].get<std::size_t>(), b = c["curves"][1].get<std::size_t>();
        for (const auto& [mine, other] : {std::pair{a, b}, std::pair{b, a}}) {
            if ((mine != ia && mine != ib) || other == ia || other == ib)
                continue;
            const std::size_t shifted = other - (ia < other ? 1 : 0) - (ib < other ? 1 : 0);
            out.emplace_back(mine == ia ? 0 : 1, shifted);
        }
    }
    return out;
}

} // namespace

void Design::filletSketchCorner(FeatureId sketchId, std::size_t ia, std::size_t ib, double radius)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    const Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    const std::vector<SketchCurve> curves = it->second.curves;
    if (ia == ib || ia >= curves.size() || ib >= curves.size() || curves[ia].kind != SketchCurve::Kind::Line ||
        curves[ib].kind != SketchCurve::Kind::Line)
        throw DesignError("A sketch fillet rounds the corner of two lines");
    if (!(radius > 0))
        throw DesignError("The fillet radius must be positive");
    const SketchCurve& la = curves[ia];
    const SketchCurve& lb = curves[ib];
    // The shared corner and the far ends.
    const double tol = 1e-6;
    const auto corner = sketchCorner(la, lb);
    if (!corner)
        throw DesignError("The two lines do not meet at a corner");
    const Vec2 c = corner->corner, farA = corner->farA, farB = corner->farB;
    auto unit = [](const Vec2& v) {
        const double l = std::hypot(v.x, v.y);
        return Vec2{v.x / l, v.y / l};
    };
    const Vec2 u = unit(farA - c), v = unit(farB - c);
    const double cosT = std::clamp(u.x * v.x + u.y * v.y, -1.0, 1.0);
    const double theta = std::acos(cosT); // angle between the lines at the corner
    if (theta < 1e-6 || theta > std::numbers::pi - 1e-6)
        throw DesignError("The lines are in line: there is no corner to round");
    const double t = radius / std::tan(theta / 2); // corner to tangent point
    const double lenA = std::hypot(farA.x - c.x, farA.y - c.y), lenB = std::hypot(farB.x - c.x, farB.y - c.y);
    if (t >= lenA - tol || t >= lenB - tol)
        throw DesignError("The radius is too large for these lines");
    const Vec2 ta = c + u * t, tb = c + v * t;
    const Vec2 bis = unit(u + v);
    const double toCenter = radius / std::sin(theta / 2);
    const Vec2 center = c + bis * toCenter;
    const Vec2 mid = center - bis * radius; // the arc's middle, nearest the corner

    auto pt = [](const Vec2& p) { return Json::array({p.x, p.y}); };
    auto lineItem = [&](const SketchCurve& orig, const Vec2& far, const Vec2& tp) {
        Json item{{"type", "line"}, {"start", pt(far)}, {"end", pt(tp)}};
        if (orig.construction)
            item["construction"] = true;
        return item;
    };
    const auto partners = collinearPartners(f.params.value("constraints", Json::array()), ia, ib);
    try {
        deleteSketchCurves(sketchId, {ia, ib});
        Feature& g = m_state.features[indexOf(sketchId)];
        Json items = g.params.value("curves", Json::array());
        // Index of the first new curve: the curves left after the delete.
        std::size_t first = m_sketches.count(sketchId) ? m_sketches.at(sketchId).curves.size() : curves.size() - 2;
        items.push_back(lineItem(la, farA, ta));
        items.push_back(lineItem(lb, farB, tb));
        Json arc{{"type", "arc"}, {"start", pt(ta)}, {"through", pt(mid)}, {"end", pt(tb)}};
        // Rounding a construction corner gives a construction arc: it must not bound a profile.
        if (la.construction && lb.construction)
            arc["construction"] = true;
        items.push_back(arc);
        g.params["curves"] = items;
        Json constraints = g.params.value("constraints", Json::array());
        constraints.push_back({{"type", "tangent"}, {"curves", Json::array({first, first + 2})}});
        constraints.push_back({{"type", "tangent"}, {"curves", Json::array({first + 1, first + 2})}});
        constraints.push_back({{"type", "radius"}, {"curve", first + 2}, {"value", radius}});
        if (const auto h = axisConstraint(la, first))
            constraints.push_back(*h);
        if (const auto h = axisConstraint(lb, first + 1))
            constraints.push_back(*h);
        for (const auto& [side, other] : partners)
            constraints.push_back({{"type", "collinear"}, {"curves", Json::array({first + side, other})}});
        g.params["constraints"] = constraints;
        regenerateChecked(sketchId, before);
    } catch (...) {
        m_state = before;
        regenerate();
        throw;
    }
}

void Design::chamferSketchCorner(FeatureId sketchId, std::size_t ia, std::size_t ib, double distance)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    const Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end())
        throw DesignError(f.name + " is not evaluated");
    const std::vector<SketchCurve> curves = it->second.curves;
    if (ia == ib || ia >= curves.size() || ib >= curves.size() || curves[ia].kind != SketchCurve::Kind::Line ||
        curves[ib].kind != SketchCurve::Kind::Line)
        throw DesignError("A sketch chamfer cuts the corner of two lines");
    if (!(distance > 0))
        throw DesignError("The chamfer distance must be positive");
    const SketchCurve& la = curves[ia];
    const SketchCurve& lb = curves[ib];
    const double tol = 1e-6;
    const auto corner = sketchCorner(la, lb);
    if (!corner)
        throw DesignError("The two lines do not meet at a corner");
    const Vec2 c = corner->corner, farA = corner->farA, farB = corner->farB;
    const double lenA = std::hypot(farA.x - c.x, farA.y - c.y), lenB = std::hypot(farB.x - c.x, farB.y - c.y);
    const Vec2 u{(farA.x - c.x) / lenA, (farA.y - c.y) / lenA}, v{(farB.x - c.x) / lenB, (farB.y - c.y) / lenB};
    if (std::abs(u.x * v.y - u.y * v.x) < 1e-9)
        throw DesignError("The lines are in line: there is no corner to cut");
    if (distance >= lenA - tol || distance >= lenB - tol)
        throw DesignError("The chamfer distance is too large for these lines");
    const Vec2 ta = c + u * distance, tb = c + v * distance;

    auto pt = [](const Vec2& p) { return Json::array({p.x, p.y}); };
    auto lineItem = [&](const Vec2& a, const Vec2& b, bool construction) {
        Json item{{"type", "line"}, {"start", pt(a)}, {"end", pt(b)}};
        if (construction)
            item["construction"] = true;
        return item;
    };
    const auto partners = collinearPartners(f.params.value("constraints", Json::array()), ia, ib);
    try {
        deleteSketchCurves(sketchId, {ia, ib});
        Feature& g = m_state.features[indexOf(sketchId)];
        Json items = g.params.value("curves", Json::array());
        std::size_t first = m_sketches.count(sketchId) ? m_sketches.at(sketchId).curves.size() : curves.size() - 2;
        // The shortened lines, the chamfer line, and (as Fusion shows it) the cut-off corner
        // as two construction lines carrying the chamfer distances.
        items.push_back(lineItem(farA, ta, la.construction));
        items.push_back(lineItem(farB, tb, lb.construction));
        items.push_back(lineItem(ta, tb, la.construction && lb.construction));
        items.push_back(lineItem(ta, c, true));
        items.push_back(lineItem(tb, c, true));
        g.params["curves"] = items;
        Json constraints = g.params.value("constraints", Json::array());
        constraints.push_back({{"type", "collinear"}, {"curves", Json::array({first, first + 3})}});
        constraints.push_back({{"type", "collinear"}, {"curves", Json::array({first + 1, first + 4})}});
        constraints.push_back({{"type", "length"}, {"curve", first + 3}, {"value", distance}});
        constraints.push_back({{"type", "length"}, {"curve", first + 4}, {"value", distance}});
        if (const auto h = axisConstraint(la, first))
            constraints.push_back(*h);
        if (const auto h = axisConstraint(lb, first + 1))
            constraints.push_back(*h);
        for (const auto& [side, other] : partners)
            constraints.push_back({{"type", "collinear"}, {"curves", Json::array({first + side, other})}});
        g.params["constraints"] = constraints;
        regenerateChecked(sketchId, before);
        // The chamfer must be where it was asked for (the constraints could flip it).
        it = m_sketches.find(sketchId);
        const SketchCurve want{SketchCurve::Kind::Line, {ta, tb}};
        if (it == m_sketches.end() || first + 2 >= it->second.curves.size() ||
            !sameCurve(it->second.curves[first + 2], want, 1e-6))
            throw DesignError("The corner's constraints do not let it be chamfered");
    } catch (...) {
        m_state = before;
        regenerate();
        throw;
    }
}

namespace {

/// What another curve bounds an extension with: the curve itself, or a spline or conic as
/// the short segments of its drawn shape (128 per span, 256 along a conic, so the end lands
/// within a hair of it).
std::vector<TrimCurve> extendBoundaries(const SketchCurve& k)
{
    if (auto t = trimCurve(k))
        return {*t};
    std::vector<TrimCurve> out;
    std::vector<Vec2> pts;
    if (k.kind == SketchCurve::Kind::Conic) {
        for (int i = 0; i <= 256; ++i)
            pts.push_back(geometry::conicPoint(k, i / 256.0));
    } else if (k.kind == SketchCurve::Kind::Spline) {
        try {
            pts = geometry::sampleSpline(k, 128);
        } catch (const geometry::ModelingError&) {
            return out;
        }
    }
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        TrimCurve seg;
        seg.a = pts[i];
        seg.b = pts[i + 1];
        out.push_back(seg);
    }
    return out;
}

/// A curve cut where the other curves cross it, around the point clicked: the piece under
/// the click and the pieces either side of it (sketch items, in order along the curve).
struct CurveSplit {
    Json before;  ///< null when the picked piece starts the curve
    Json picked;  ///< null when nothing crosses the curve (the whole curve is the piece)
    Json after;   ///< null when the picked piece ends the curve; a circle's rest is here
};

/// A spline cut where the other curves cross it, around the point clicked (Fusion trims
/// splines too). It is taken as the polyline other curves are cut against (128 points a
/// span, see extendBoundaries), so a line and the spline cut each other at the same point.
/// Each piece is a spline again: through its ends, the original fit points between them and
/// as many more points of the original as keep it within 0.1 % of the spline's size of it.
CurveSplit splitSpline(const std::vector<SketchCurve>& curves, std::size_t curve, const Vec2& at, bool pickedOnly)
{
    const SketchCurve& k = curves[curve];
    std::vector<Vec2> pts;
    try {
        pts = geometry::sampleSpline(k, 128);
    } catch (const geometry::ModelingError& e) {
        throw DesignError(e.what());
    }
    const std::size_t n = pts.size() - 1;
    if (n == 0)
        throw DesignError("The spline has no length");
    // A point along the polyline: u in [0, 1] (a closed spline wraps round).
    auto pointAt = [&](double u) {
        if (k.closed)
            u -= std::floor(u);
        const double x = std::clamp(u, 0.0, 1.0) * static_cast<double>(n);
        const std::size_t i = std::min(static_cast<std::size_t>(x), n - 1);
        const double f = x - static_cast<double>(i);
        return pts[i] + (pts[i + 1] - pts[i]) * f;
    };
    // The nearest point of the polyline to q among segments [from, to) (wrapping round a
    // closed spline, clamped for an open one): {distance, u}.
    auto nearest = [&](const Vec2& q, std::ptrdiff_t from, std::ptrdiff_t to) {
        const auto sn = static_cast<std::ptrdiff_t>(n);
        if (!k.closed) {
            from = std::max<std::ptrdiff_t>(from, 0);
            to = std::min<std::ptrdiff_t>(to, sn);
        } else if (to - from > sn) {
            from = 0;
            to = sn;
        }
        double best = 1e300, u = 0.0;
        for (std::ptrdiff_t j = from; j < to; ++j) {
            const auto i = static_cast<std::size_t>(((j % sn) + sn) % sn);
            const Vec2 d = pts[i + 1] - pts[i];
            const double l2 = d.x * d.x + d.y * d.y;
            const double f = l2 > 0 ? std::clamp(((q.x - pts[i].x) * d.x + (q.y - pts[i].y) * d.y) / l2, 0.0, 1.0) : 0.0;
            const Vec2 x = pts[i] + d * f;
            const double dist = std::hypot(q.x - x.x, q.y - x.y);
            if (dist < best) {
                best = dist;
                u = (static_cast<double>(i) + f) / static_cast<double>(n);
            }
        }
        return std::pair{best, u};
    };
    auto segIndex = [&](double u) { return static_cast<std::ptrdiff_t>(std::floor(u * static_cast<double>(n))); };
    const double spans = static_cast<double>(std::max<std::size_t>(1, k.points.size() - (k.closed ? 0 : 1)));

    // Where the others cross it: segment pairs, pruned by the boxes of runs of 64 segments
    // (a spline against another spline is many thousand segments each).
    struct Box {
        Vec2 lo{1e300, 1e300}, hi{-1e300, -1e300};
        void add(const Vec2& v)
        {
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y)};
        }
        void add(const Box& b)
        {
            add(b.lo);
            add(b.hi);
        }
        [[nodiscard]] bool meets(const Box& b) const
        {
            const double m = 1e-7;
            return lo.x <= b.hi.x + m && b.lo.x <= hi.x + m && lo.y <= b.hi.y + m && b.lo.y <= hi.y + m;
        }
    };
    auto boxOf = [](const TrimCurve& t) {
        Box b;
        if (t.line) {
            b.add(t.a);
            b.add(t.b);
        } else {
            const double r = t.ell ? std::max(t.ra, t.rb) : t.r;
            b.add(Vec2{t.c.x - r, t.c.y - r});
            b.add(Vec2{t.c.x + r, t.c.y + r});
        }
        return b;
    };
    const std::size_t run = 64;
    std::vector<Box> runs((n + run - 1) / run);
    for (std::size_t i = 0; i < n; ++i) {
        runs[i / run].add(pts[i]);
        runs[i / run].add(pts[i + 1]);
    }
    std::vector<double> cuts;
    for (std::size_t j = 0; j < curves.size(); ++j) {
        if (j == curve)
            continue;
        const std::vector<TrimCurve> others = extendBoundaries(curves[j]);
        std::vector<Box> otherBoxes;
        std::vector<Box> otherRuns((others.size() + run - 1) / run);
        for (std::size_t o = 0; o < others.size(); ++o) {
            otherBoxes.push_back(boxOf(others[o]));
            otherRuns[o / run].add(otherBoxes.back());
        }
        for (std::size_t r = 0; r < runs.size(); ++r)
            for (std::size_t q = 0; q < otherRuns.size(); ++q) {
                if (!runs[r].meets(otherRuns[q]))
                    continue;
                for (std::size_t i = r * run; i < std::min(n, (r + 1) * run); ++i) {
                    TrimCurve seg;
                    seg.a = pts[i];
                    seg.b = pts[i + 1];
                    if (std::hypot(seg.b.x - seg.a.x, seg.b.y - seg.a.y) < 1e-12)
                        continue;
                    Box sb;
                    sb.add(seg.a);
                    sb.add(seg.b);
                    for (std::size_t o = q * run; o < std::min(others.size(), (q + 1) * run); ++o) {
                        if (!sb.meets(otherBoxes[o]))
                            continue;
                        for (const Vec2& x : crossings(seg, others[o]))
                            if (const auto f = trimParam(seg, x, 1e-9))
                                cuts.push_back((static_cast<double>(i) + *f) / static_cast<double>(n));
                    }
                }
            }
    }
    std::sort(cuts.begin(), cuts.end());
    const double eps = 1e-7;
    std::vector<double> inner;
    for (double c : cuts) {
        if (k.closed && c >= 1 - eps)
            c = 0.0; // the closing point is the start
        if (k.closed || (c > eps && c < 1 - eps))
            if (inner.empty() || c - inner.back() > eps)
                inner.push_back(c);
    }
    std::sort(inner.begin(), inner.end());
    if (k.closed && inner.size() >= 2 && inner.front() + 1 - inner.back() <= eps)
        inner.pop_back();

    // The fit points, as places along it.
    // Each is looked for within a span and a half of where its share of the chord length
    // puts it (the spline is parametrised by chord length).
    std::vector<double> fits;
    {
        std::vector<double> along{0.0};
        const std::size_t m = k.points.size();
        for (std::size_t i = 1; i < m + (k.closed ? 1 : 0); ++i) {
            const Vec2 a = k.points[i - 1], b = k.points[i % m];
            along.push_back(along.back() + std::hypot(b.x - a.x, b.y - a.y));
        }
        const double total = std::max(along.back(), 1e-300);
        const auto w = static_cast<std::ptrdiff_t>(std::ceil(1.5 * static_cast<double>(n) / spans)) + 2;
        for (std::size_t i = 0; i < m; ++i) {
            const std::ptrdiff_t c = segIndex(along[i] / total);
            fits.push_back(nearest(k.points[i], c - w, c + w).second);
        }
    }
    auto piece = [&](double lo, double hi) {
        // Anchors: the ends and the fit points between them (a closed spline's wrap round).
        std::vector<double> anchors{lo};
        std::vector<double> inside;
        for (double f : fits) {
            for (const double g : {f, f + 1.0, f - 1.0})
                if (g > lo + eps && g < hi - eps && (k.closed || g == f))
                    inside.push_back(g);
        }
        std::sort(inside.begin(), inside.end());
        for (const double g : inside)
            if (g - anchors.back() > eps)
                anchors.push_back(g);
        if (hi - anchors.back() <= eps)
            anchors.pop_back();
        anchors.push_back(hi);
        // Between the anchors, evenly spread points of the original (an interpolating spline
        // overshoots between uneven ones), as few as keep the piece within 0.1 % of the
        // spline's size of the original.
        Vec2 bmin = pts.front(), bmax = pts.front();
        for (const Vec2& q : pts) {
            bmin = {std::min(bmin.x, q.x), std::min(bmin.y, q.y)};
            bmax = {std::max(bmax.x, q.x), std::max(bmax.y, q.y)};
        }
        const double tol = 1e-3 * std::hypot(bmax.x - bmin.x, bmax.y - bmin.y);
        // Each span of the trial is compared with its own stretch of the original (and two
        // segments either side), which bounds the work for a long spline.
        std::vector<Vec2> chosen;
        for (const double h : {1.0, 0.5, 0.25, 0.125, 0.0625}) {
            chosen.clear();
            std::vector<double> us;
            for (std::size_t i = 0; i < anchors.size(); ++i) {
                us.push_back(anchors[i]);
                if (i + 1 == anchors.size())
                    break;
                const double len = anchors[i + 1] - anchors[i];
                const int m = std::max(1, static_cast<int>(std::lround(len * spans / h)));
                for (int j = 1; j < m; ++j)
                    us.push_back(anchors[i] + len * j / m);
            }
            if (us.size() < 3)
                us.insert(us.begin() + 1, (us.front() + us.back()) / 2);
            for (const double a : us)
                chosen.push_back(pointAt(a));
            SketchCurve trial{SketchCurve::Kind::Spline, chosen};
            double worst = 0.0;
            try {
                // The trial is parametrised by chord length too, and sampled evenly over it:
                // a sample's share of the parameter range tells its span.
                const int per = 16;
                const std::vector<Vec2> samples = geometry::sampleSpline(trial, per);
                std::vector<double> chord{0.0};
                for (std::size_t i = 1; i < chosen.size(); ++i)
                    chord.push_back(chord.back() + std::hypot(chosen[i].x - chosen[i - 1].x, chosen[i].y - chosen[i - 1].y));
                const double total = std::max(chord.back(), 1e-300);
                std::size_t span = 0;
                for (std::size_t q = 0; q < samples.size(); ++q) {
                    const double f = total * static_cast<double>(q) / static_cast<double>(samples.size() - 1);
                    while (span + 2 < chosen.size() && chord[span + 1] < f)
                        ++span;
                    worst = std::max(worst, nearest(samples[q], segIndex(us[span]) - 2, segIndex(us[span + 1]) + 3).first);
                }
            } catch (const geometry::ModelingError&) {
                worst = 1e300;
            }
            if (worst <= tol)
                break;
        }
        Json p = Json::array();
        for (const Vec2& q : chosen)
            p.push_back(Json::array({q.x, q.y}));
        Json item{{"type", "spline"}, {"points", p}};
        if (k.construction)
            item["construction"] = true;
        return item;
    };

    CurveSplit out;
    const double u = nearest(at, 0, static_cast<std::ptrdiff_t>(n)).second;
    if (k.closed) {
        if (inner.size() >= 2) {
            auto after = std::upper_bound(inner.begin(), inner.end(), u);
            const double hi = after == inner.end() ? inner.front() + 1.0 : *after;
            const double lo = after == inner.begin() ? inner.back() - 1.0 : *(after - 1);
            out.picked = piece(lo, hi);
            if (!pickedOnly)
                out.after = piece(hi, lo + 1.0);
        }
        return out;
    }
    double lo = 0.0, hi = 1.0;
    for (const double c : inner) {
        if (c <= u)
            lo = c;
        else {
            hi = c;
            break;
        }
    }
    if (lo <= eps && hi >= 1 - eps)
        return out;
    if (lo > eps && !pickedOnly)
        out.before = piece(0.0, lo);
    out.picked = piece(lo, hi);
    if (hi < 1 - eps && !pickedOnly)
        out.after = piece(hi, 1.0);
    return out;
}

CurveSplit splitCurve(const std::vector<SketchCurve>& curves, std::size_t curve, const Vec2& at, const char* tool,
                      bool pickedOnly = false)
{
    const SketchCurve& k = curves[curve];
    if (k.kind == SketchCurve::Kind::Spline)
        return splitSpline(curves, curve, at, pickedOnly);
    const auto target = trimCurve(k);
    if (!target)
        throw DesignError(std::string(tool) + " works on lines, arcs, circles, ellipses and splines");

    // Where the others cross it, as parameters along it.
    std::vector<double> cuts;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (i == curve)
            continue;
        for (const TrimCurve& other : extendBoundaries(curves[i]))
            for (const Vec2& x : crossings(*target, other)) {
                if (const auto u = trimParam(*target, x, 1e-7))
                    cuts.push_back(*u);
            }
    }
    std::sort(cuts.begin(), cuts.end());
    const auto pu = trimParam(*target, at, 1.0); // the click is near the curve, not on it
    double u = pu ? *pu : 0.5;
    if (!target->line && std::abs(target->sweep) < 2 * std::numbers::pi) {
        // An arc: the click's angle, clamped to the arc.
        const double a = angleOn(*target, at);
        const double along = target->sweep >= 0 ? wrap2pi(a - target->a0) : wrap2pi(target->a0 - a);
        const double span = std::abs(target->sweep);
        u = along <= span ? along / span : (along - span < 2 * std::numbers::pi - along ? 1.0 : 0.0);
    }

    CurveSplit out;
    const double eps = 1e-6;
    const bool full = !target->line && std::abs(target->sweep) >= 2 * std::numbers::pi - 1e-12;
    std::vector<double> inner;
    for (const double c : cuts) {
        if (full || (c > eps && c < 1 - eps))
            if (inner.empty() || c - inner.back() > eps)
                inner.push_back(c);
    }
    if (full) {
        if (inner.size() >= 2) {
            // The piece under the click, and the arc around the rest of the circle.
            auto after = std::upper_bound(inner.begin(), inner.end(), u);
            const double hi = after == inner.end() ? inner.front() + 1.0 : *after;
            const double lo = after == inner.begin() ? inner.back() - 1.0 : *(after - 1);
            out.picked = trimPiece(*target, lo, hi, k.construction);
            out.after = trimPiece(*target, hi, lo + 1.0, k.construction);
        }
        return out;
    }
    double lo = 0.0, hi = 1.0;
    for (const double c : inner) {
        if (c <= u)
            lo = c;
        else {
            hi = c;
            break;
        }
    }
    if (lo <= eps && hi >= 1 - eps)
        return out;
    if (lo > eps)
        out.before = trimPiece(*target, 0.0, lo, k.construction);
    out.picked = trimPiece(*target, lo, hi, k.construction);
    if (hi < 1 - eps)
        out.after = trimPiece(*target, hi, 1.0, k.construction);
    return out;
}

} // namespace

void Design::trimSketchCurve(FeatureId sketchId, std::size_t curve, const geometry::Vec2& at)
{
    splitSketchCurve(sketchId, curve, at, false);
}

void Design::breakSketchCurve(FeatureId sketchId, std::size_t curve, const geometry::Vec2& at)
{
    splitSketchCurve(sketchId, curve, at, true);
}

Json Design::sketchCurvePiece(FeatureId sketchId, std::size_t curve, const geometry::Vec2& at) const
{
    const Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end() || curve >= it->second.curves.size())
        throw DesignError(f.name + " has no curve " + std::to_string(curve));
    return splitCurve(it->second.curves, curve, at, "Break", true).picked;
}

void Design::splitSketchCurve(FeatureId sketchId, std::size_t curve, const geometry::Vec2& at, bool keep)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    const Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end() || curve >= it->second.curves.size())
        throw DesignError(f.name + " has no curve " + std::to_string(curve));
    CurveSplit split;
    try {
        split = splitCurve(it->second.curves, curve, at, keep ? "Break" : "Trim");
    } catch (...) {
        m_state = before;
        throw;
    }
    if (keep && split.picked.is_null()) {
        m_state = before;
        throw DesignError("Nothing to break at: no other curve crosses it");
    }
    // Trim drops the piece under the click (a curve crossing nothing goes whole); Break keeps it.
    Json pieces = Json::array();
    for (const Json* p : {&split.before, keep ? &split.picked : nullptr, &split.after})
        if (p && !p->is_null())
            pieces.push_back(*p);

    try {
        deleteSketchCurves(sketchId, {curve});
        Feature& g = m_state.features[indexOf(sketchId)];
        Json items = g.params.value("curves", Json::array());
        for (const auto& p : pieces)
            items.push_back(p);
        g.params["curves"] = items;
        regenerateChecked(sketchId, before);
    } catch (...) {
        m_state = before;
        regenerate();
        throw;
    }
}

geometry::SketchCurve Design::extendedSketchCurve(FeatureId sketchId, std::size_t curve, const geometry::Vec2& at,
                                                  bool* atStart) const
{
    const Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    auto it = m_sketches.find(sketchId);
    if (it == m_sketches.end() || curve >= it->second.curves.size())
        throw DesignError(f.name + " has no curve " + std::to_string(curve));
    const std::vector<SketchCurve>& curves = it->second.curves;
    const SketchCurve& k = curves[curve];
    if (k.kind != SketchCurve::Kind::Line && k.kind != SketchCurve::Kind::Arc)
        throw DesignError("Extend works on lines and arcs");

    // Only a curve drawn on its own: the sides of a rectangle, polygon or slot keep their shape.
    {
        const Eval ev{*this};
        std::size_t first = 0;
        for (const auto& item : f.params.value("curves", Json::array())) {
            std::vector<SketchCurve> made;
            std::vector<SketchConstraint> implicit;
            appendCurves(ev, item, made, implicit);
            if (curve < first + made.size()) {
                const std::string type = lower(item.value("type", std::string()));
                if (made.size() != 1 || (type != "line" && type != "arc" && type != "center_arc"))
                    throw DesignError("Extend works on lines and arcs drawn on their own, not on the sides of a "
                                      "rectangle, polygon or slot");
                break;
            }
            first += made.size();
        }
    }

    const Vec2 p0 = k.points.front();
    const Vec2 p1 = k.points.back();
    const bool fromStart = std::hypot(at.x - p0.x, at.y - p0.y) < std::hypot(at.x - p1.x, at.y - p1.y);
    const Vec2 e = fromStart ? p0 : p1;
    if (atStart != nullptr)
        *atStart = fromStart;

    // An end shared with another curve stays where it is (Fusion extends free ends).
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (i == curve)
            continue;
        const auto& c = curves[i];
        // A sketch point on the end is joined to it too (the solver shares it like an end).
        const bool hasEnds = c.kind == SketchCurve::Kind::Line || c.kind == SketchCurve::Kind::Arc ||
                             c.kind == SketchCurve::Kind::EllipticalArc || c.kind == SketchCurve::Kind::Conic ||
                             c.kind == SketchCurve::Kind::Point || (c.kind == SketchCurve::Kind::Spline && !c.closed);
        if (!hasEnds || c.points.empty())
            continue;
        for (const Vec2& q : {c.points.front(), c.points.back()})
            if (std::hypot(q.x - e.x, q.y - e.y) < 1e-6)
                throw DesignError("That end is joined to another curve");
    }

    // How far the sketch reaches, for a ray long enough to meet every curve.
    double reach = 1.0;
    for (const auto& c : curves)
        for (const Vec2& q : c.points)
            reach = std::max(reach, std::hypot(q.x - e.x, q.y - e.y) + c.radius);
    reach *= 4.0;

    SketchCurve out = k;
    if (k.kind == SketchCurve::Kind::Line) {
        const Vec2 from = fromStart ? p1 : p0;
        const double len = std::hypot(e.x - from.x, e.y - from.y);
        if (len < 1e-12)
            throw DesignError("The line has no length");
        const Vec2 d{(e.x - from.x) / len, (e.y - from.y) / len};
        TrimCurve ray;
        ray.a = e;
        ray.b = e + d * reach;
        double best = -1.0;
        for (std::size_t i = 0; i < curves.size(); ++i) {
            if (i == curve)
                continue;
            for (const TrimCurve& other : extendBoundaries(curves[i]))
                for (const Vec2& x : crossings(ray, other)) {
                    const double t = (x.x - e.x) * d.x + (x.y - e.y) * d.y;
                    if (t > 1e-6 && (best < 0 || t < best))
                        best = t;
                }
        }
        if (best < 0)
            throw DesignError("Nothing to extend to: no curve lies beyond that end");
        (fromStart ? out.points.front() : out.points.back()) = e + d * best;
        return out;
    }

    // An arc: on around its circle, in the direction it runs at that end.
    const auto arc = trimCurve(k);
    if (!arc || std::abs(arc->sweep) >= 2 * std::numbers::pi - 1e-9)
        throw DesignError("Extend works on lines and arcs");
    const double sense = arc->sweep >= 0 ? 1.0 : -1.0;
    const double aEnd = fromStart ? arc->a0 : arc->a0 + arc->sweep;
    const double dir = fromStart ? -sense : sense;
    const double room = 2 * std::numbers::pi - std::abs(arc->sweep); // up to its other end
    TrimCurve circle = *arc;
    circle.a0 = 0.0;
    circle.sweep = 2 * std::numbers::pi;
    double best = -1.0;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (i == curve)
            continue;
        for (const TrimCurve& other : extendBoundaries(curves[i]))
            for (const Vec2& x : crossings(circle, other)) {
                const double a = std::atan2(x.y - arc->c.y, x.x - arc->c.x);
                const double along = dir > 0 ? wrap2pi(a - aEnd) : wrap2pi(aEnd - a);
                if (along > 1e-9 && along < room - 1e-9 && (best < 0 || along < best))
                    best = along;
            }
    }
    if (best < 0)
        throw DesignError("Nothing to extend to: no curve lies beyond that end");
    const double a0 = fromStart ? arc->a0 - sense * best : arc->a0;
    const double sweep = arc->sweep + sense * best;
    auto on = [&](double a) { return Vec2{arc->c.x + arc->r * std::cos(a), arc->c.y + arc->r * std::sin(a)}; };
    out.points = {on(a0), on(a0 + sweep / 2), on(a0 + sweep)};
    return out;
}

void Design::extendSketchCurve(FeatureId sketchId, std::size_t curve, const geometry::Vec2& at)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    bool fromStart = false;
    SketchCurve made;
    try {
        made = extendedSketchCurve(sketchId, curve, at, &fromStart);
    } catch (...) {
        m_state = before;
        throw;
    }
    // The item that makes the curve, rewritten with the new end (an arc as three points).
    Feature& f = m_state.features[indexOf(sketchId)];
    const Eval ev{*this};
    Json items = f.params.value("curves", Json::array());
    std::size_t first = 0;
    auto pt = [](const Vec2& v) { return Json::array({v.x, v.y}); };
    for (auto& item : items) {
        std::vector<SketchCurve> c;
        std::vector<SketchConstraint> implicit;
        appendCurves(ev, item, c, implicit);
        if (curve < first + c.size()) {
            if (made.kind == SketchCurve::Kind::Line) {
                item[fromStart ? "start" : "end"] = pt(fromStart ? made.points.front() : made.points.back());
            } else {
                Json arc{{"type", "arc"}, {"start", pt(made.points[0])}, {"through", pt(made.points[1])}, {"end", pt(made.points[2])}};
                for (const char* key : {"construction", "join_start", "join_end"})
                    if (item.contains(key))
                        arc[key] = item[key];
                item = arc;
            }
            break;
        }
        first += c.size();
    }
    f.params["curves"] = items;
    try {
        regenerateChecked(sketchId, before);
    } catch (const DesignError& e) {
        throw DesignError(std::string("Cannot extend: ") + e.what());
    }
    // The constraints may solve the rewritten curve elsewhere (a length dimension pulls both
    // ends back): then the extension did not happen, so nothing changes.
    const auto it = m_sketches.find(sketchId);
    bool kept = it != m_sketches.end() && curve < it->second.curves.size();
    if (kept) {
        const SketchCurve& got = it->second.curves[curve];
        const auto distance = [](const Vec2& a, const Vec2& b) { return std::hypot(a.x - b.x, a.y - b.y); };
        const double tol = 1e-6 * std::max(1.0, distance(made.points.front(), made.points.back()));
        kept = !got.points.empty() && distance(got.points.front(), made.points.front()) <= tol
               && distance(got.points.back(), made.points.back()) <= tol;
    }
    if (!kept) {
        m_state = before;
        regenerate();
        throw DesignError("Cannot extend: the curve's constraints (a length or radius dimension, a fixed point) "
                          "hold it where it is; remove them first");
    }
}

void Design::setSketchConstruction(FeatureId sketchId, const std::vector<std::size_t>& picked, bool construction)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    if (picked.empty())
        throw DesignError("Select at least one sketch curve");
    const Eval ev{*this};
    const Json items = f.params.value("curves", Json::array());
    std::vector<std::vector<SketchCurve>> made;
    std::vector<std::vector<SketchConstraint>> implicit;
    std::size_t total = 0;
    for (const auto& item : items) {
        made.emplace_back();
        implicit.emplace_back();
        appendCurves(ev, item, made.back(), implicit.back());
        total += made.back().size();
    }
    const SketchResult* solved = nullptr;
    if (auto it = m_sketches.find(sketchId); it != m_sketches.end() && it->second.curves.size() == total)
        solved = &it->second;
    std::vector<bool> on(total, false);
    for (const std::size_t i : picked) {
        if (i >= total)
            throw DesignError(f.name + " has no curve " + std::to_string(i));
        on[i] = true;
    }

    Json newItems = Json::array();
    Json extra = Json::array();
    std::size_t first = 0;
    for (std::size_t k = 0; k < made.size(); ++k) {
        const std::size_t n = made[k].size();
        std::size_t count = 0;
        for (std::size_t j = 0; j < n; ++j)
            count += on[first + j] ? 1 : 0;
        if (count == 0 || count == n) {
            Json item = items[k];
            if (count == n) {
                if (construction)
                    item["construction"] = true;
                else
                    item.erase("construction");
            }
            newItems.push_back(item);
            first += n;
            continue;
        }
        // Part of a shape: its curves become single items in the same order (indices stay),
        // where they are shown now, and its own constraints become explicit ones.
        for (std::size_t j = 0; j < n; ++j) {
            Json item = curveItem(solved != nullptr ? solved->curves[first + j] : made[k][j]);
            const bool c = on[first + j] ? construction : made[k][j].construction;
            if (c)
                item["construction"] = true;
            else
                item.erase("construction");
            newItems.push_back(item);
        }
        for (const auto& c : implicit[k]) {
            const char* name = implicitName(c.type);
            if (name == nullptr)
                continue;
            Json curves = Json::array();
            for (const std::size_t local : c.curves)
                curves.push_back(first + local);
            Json e{{"type", name}};
            if (curves.size() == 1)
                e["curve"] = curves[0];
            else
                e["curves"] = curves;
            extra.push_back(e);
        }
        first += n;
    }
    f.params["curves"] = newItems;
    if (!extra.empty()) {
        Json constraints = f.params.value("constraints", Json::array());
        for (const auto& e : extra)
            constraints.push_back(e);
        f.params["constraints"] = constraints;
    }
    regenerateChecked(sketchId, before);
}

void Design::deleteSketchCurves(FeatureId sketchId, const std::vector<std::size_t>& removed)
{
    const State before = m_state;
    keepSolvedSketch(sketchId);
    Feature& f = m_state.features[indexOf(sketchId)];
    if (f.type != "sketch")
        throw DesignError("Feature " + std::to_string(sketchId) + " is not a sketch");
    if (removed.empty())
        throw DesignError("Select at least one sketch curve");
    const Eval ev{*this};
    const Json items = f.params.value("curves", Json::array());

    // Expand every item as the evaluation does, to know which curves it makes.
    std::vector<std::vector<SketchCurve>> made;
    std::vector<std::vector<SketchConstraint>> implicit;
    std::size_t total = 0;
    for (const auto& item : items) {
        made.emplace_back();
        implicit.emplace_back();
        appendCurves(ev, item, made.back(), implicit.back());
        total += made.back().size();
    }
    // The solved curves of the sketch as shown (same order as the expansion above).
    const SketchResult* solved = nullptr;
    if (auto it = m_sketches.find(sketchId); it != m_sketches.end() && it->second.curves.size() == total)
        solved = &it->second;
    std::vector<bool> gone(total, false);
    for (const std::size_t i : removed) {
        if (i >= total)
            throw DesignError(f.name + " has no curve " + std::to_string(i));
        gone[i] = true;
    }

    Json newItems = Json::array();
    Json extra = Json::array(); // the kept constraints of split shapes
    std::vector<std::optional<std::size_t>> map(total);
    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;
    for (std::size_t k = 0; k < made.size(); ++k) {
        const std::size_t first = oldIndex;
        const std::size_t n = made[k].size();
        oldIndex += n;
        std::size_t kept = 0;
        for (std::size_t j = 0; j < n; ++j)
            kept += gone[first + j] ? 0 : 1;
        if (kept == n) { // untouched: keep the item as written (with its expressions)
            for (std::size_t j = 0; j < n; ++j)
                map[first + j] = newIndex++;
            newItems.push_back(items[k]);
            continue;
        }
        if (kept == 0)
            continue;
        // Split: the remaining curves as single items, where they are shown now (solved by
        // the dimensions), so deleting one side does not reset the others.
        for (std::size_t j = 0; j < n; ++j) {
            if (gone[first + j])
                continue;
            map[first + j] = newIndex++;
            newItems.push_back(curveItem(solved != nullptr ? solved->curves[first + j] : made[k][j]));
        }
        // The shape's own constraints: those between kept curves stay; equal sides (a
        // polygon chains them to its first side) are chained to the first kept side.
        std::optional<std::size_t> equalAnchor;
        std::set<std::size_t> equalLinked;
        for (const auto& c : implicit[k]) {
            const char* name = implicitName(c.type);
            if (name == nullptr)
                continue;
            if (c.type == SketchConstraint::Type::Equal) {
                for (const std::size_t local : c.curves) {
                    const auto& to = map[first + local];
                    if (!to)
                        continue;
                    if (!equalAnchor)
                        equalAnchor = *to;
                    else if (*to != *equalAnchor && equalLinked.insert(*to).second)
                        extra.push_back({{"type", "equal"}, {"curves", Json::array({*equalAnchor, *to})}});
                }
                continue;
            }
            Json curves = Json::array();
            bool ok = true;
            for (const std::size_t local : c.curves) {
                const auto& to = map[first + local];
                ok = ok && to.has_value();
                if (ok)
                    curves.push_back(*to);
            }
            if (!ok)
                continue;
            Json e{{"type", name}};
            if (curves.size() == 1)
                e["curve"] = curves[0];
            else
                e["curves"] = curves;
            extra.push_back(e);
        }
    }

    Json constraints = Json::array();
    for (auto c : f.params.value("constraints", Json::array()))
        if (remapConstraint(c, map))
            constraints.push_back(c);
    for (const auto& e : extra)
        constraints.push_back(e);

    f.params["curves"] = newItems;
    if (!constraints.empty() || f.params.contains("constraints"))
        f.params["constraints"] = constraints;

    // Later features that name curves of this sketch follow the new indices. A reference to
    // a removed curve is marked as deleted (it never points at a curve drawn later), and
    // the feature reports an error, like Fusion's red timeline steps.
    auto mapped = [&](const Json& idx) -> std::optional<std::size_t> {
        if (!idx.is_number_integer() || idx.get<std::size_t>() >= map.size())
            return std::nullopt;
        return map[idx.get<std::size_t>()];
    };
    for (auto& g : m_state.features) {
        if (g.type == "revolve" && g.params.value("sketch", Json()) == sketchId && g.params.contains("axis") &&
            g.params["axis"].is_object() && g.params["axis"].contains("line")) {
            Json& axis = g.params["axis"];
            if (const auto to = mapped(axis["line"]))
                axis["line"] = *to;
            else
                axis = Json{{"deletedLine", axis["line"]}};
        }
        if ((g.type == "sweep" || g.type == "path_pattern" || g.type == "pipe" || g.type == "rib" || g.type == "plane") &&
            g.params.contains("path") &&
            g.params["path"].is_object() &&
            g.params["path"].value("sketch", Json()) == sketchId && g.params["path"].contains("curves")) {
            Json& path = g.params["path"];
            Json kept = Json::array();
            for (const auto& i : path["curves"]) {
                if (const auto to = mapped(i))
                    kept.push_back(*to);
                else
                    path["deletedCurves"] = true;
            }
            path["curves"] = kept;
        }
        if (g.type == "hole" && g.params.value("sketch", Json()) == sketchId && g.params.contains("sketch_points")) {
            Json kept = Json::array();
            for (const auto& i : g.params["sketch_points"]) {
                if (const auto to = mapped(i))
                    kept.push_back(*to);
                else
                    g.params["deletedSketchPoints"] = true;
            }
            g.params["sketch_points"] = kept;
        }
    }
    regenerateChecked(sketchId, before);
}

void Design::evaluateFeature(const Feature& f)
{
    const Eval ev{*this};
    const Json& p = f.params;

    auto bodyRef = [&](const Json& ref) -> Body& {
        if (ref.is_number_integer()) {
            auto it = m_bodies.find(ref.get<FeatureId>());
            if (it != m_bodies.end())
                return it->second;
        } else if (ref.is_string()) {
            for (auto& [_, b] : m_bodies) {
                if (b.name == ref.get<std::string>())
                    return b;
            }
        }
        throw DesignError("No body " + ref.dump() + " at this point of the timeline");
    };

    // A plane given as "XY" / "XZ" / "YZ", {plane: construction plane id or name},
    // {body, index or point} (a planar face) or {origin, normal, xDir}.
    auto resolvePlane = [&](const Json& spec) -> Plane {
        if (spec.is_string()) {
            const std::string name = lower(spec.get<std::string>());
            if (name == "xy")
                return Plane::xy();
            if (name == "xz")
                return Plane::xz();
            if (name == "yz")
                return Plane::yz();
            if (const ReferenceFrame* fr = frame(spec.get<std::string>()))
                return Plane{fr->origin, fr->normal, fr->xDir};
            for (const auto& [_, cp] : m_planes) {
                if (lower(cp.name) == name)
                    return cp.plane;
            }
            throw DesignError("Unknown plane '" + spec.get<std::string>() +
                              "' (XY, XZ, YZ, a reference frame or a construction plane)");
        }
        if (!spec.is_object())
            throw DesignError("Invalid plane " + spec.dump());
        if (has(spec, "plane")) {
            const Json& ref = spec.at("plane");
            if (ref.is_number_integer()) {
                auto it = m_planes.find(ref.get<FeatureId>());
                if (it != m_planes.end())
                    return it->second.plane;
            } else if (ref.is_string()) {
                for (const auto& [_, cp] : m_planes) {
                    if (cp.name == ref.get<std::string>())
                        return cp.plane;
                }
            }
            throw DesignError("No construction plane " + ref.dump() + " at this point of the timeline");
        }
        if (has(spec, "body")) {
            const Body& fb = bodyRef(spec.at("body"));
            std::size_t idx = 0;
            if (has(spec, "index")) {
                idx = spec.at("index").get<std::size_t>();
            } else {
                const auto found = geometry::nearestFace(fb.shape, ev.vec3(required(spec, "point")));
                if (!found)
                    throw DesignError("No face found on " + fb.name);
                idx = *found;
            }
            const auto pl = geometry::facePlane(fb.shape, idx);
            if (!pl)
                throw DesignError("Face " + std::to_string(idx) + " of " + fb.name + " is not planar");
            return *pl;
        }
        Plane pl;
        pl.origin = ev.vec3(spec, "origin", {});
        pl.normal = ev.vec3(spec, "normal", {0, 0, 1});
        pl.xDir = ev.vec3(spec, "xDir", {1, 0, 0});
        return pl;
    };

    // A construction axis `ref` {feature: id or name} (Fusion: Construct > Axis) as a point on it
    // and its unit direction.
    auto axisFeature = [&](const Json& ref, core::Vec3& origin, core::Vec3& dir) {
        const Json& id = ref.at("feature");
        const ConstructionAxis* found = nullptr;
        for (const auto& [_, a] : m_axes) {
            if ((id.is_number_integer() && a.id == id.get<FeatureId>()) || (id.is_string() && a.name == id.get<std::string>()))
                found = &a;
        }
        if (found == nullptr)
            throw DesignError("No construction axis " + id.dump() + " at this point of the timeline");
        origin = found->origin;
        dir = found->direction;
    };

    auto resolveIndices = [&](const geometry::Shape& shape, const Json& list, bool edgesWanted) {
        std::vector<std::size_t> out;
        if (!list.is_array() || list.empty())
            throw DesignError(edgesWanted ? "Select at least one edge" : "Select at least one face");
        for (const auto& item : list) {
            if (item.is_number_integer()) {
                out.push_back(item.get<std::size_t>());
            } else if (item.is_object() && (item.contains("relative") || item.contains("point"))) {
                const auto idx = matchTopology(shape, item, edgesWanted, ev);
                if (!idx)
                    throw DesignError("No edge or face near " + item.dump());
                out.push_back(*idx);
            } else {
                throw DesignError("Expected an index or {\"point\": [x, y, z]}, got " + item.dump());
            }
        }
        return out;
    };

    auto newBody = [&](geometry::Shape shape) {
        if (shape.isNull())
            throw DesignError("The feature produced no solid");
        Body b;
        b.id = f.id;
        b.name = "Body" + std::to_string(++m_bodyCounter);
        b.shape = std::move(shape);
        m_bodies[f.id] = std::move(b);
    };

    /// Applies a tool solid to the bodies: new body / join / cut / intersect (Fusion's
    /// "Operation" option of Extrude, Revolve, Box, ...).
    auto applyOperation = [&](const geometry::Shape& tool) {
        const std::string op = lower(p.value("operation", std::string("new_body")));
        if (op == "new_body") {
            newBody(tool);
            return;
        }
        std::vector<FeatureId> targets;
        if (has(p, "bodies")) {
            for (const auto& ref : p.at("bodies")) {
                const FeatureId id = bodyRef(ref).id;
                // The same body may be given twice (by id and by name).
                if (std::find(targets.begin(), targets.end(), id) == targets.end())
                    targets.push_back(id);
            }
        } else {
            for (const auto& [id, b] : m_bodies) {
                if (geometry::touches(b.shape, tool))
                    targets.push_back(id);
            }
        }
        if (op == "join") {
            if (targets.empty()) {
                newBody(tool);
                return;
            }
            Body& first = m_bodies.at(targets.front());
            geometry::Shape result = geometry::boolean(first.shape, tool, geometry::BooleanOp::Union);
            for (std::size_t i = 1; i < targets.size(); ++i) {
                result = geometry::boolean(result, m_bodies.at(targets[i]).shape, geometry::BooleanOp::Union);
                m_bodies.erase(targets[i]);
            }
            first.shape = result;
            return;
        }
        geometry::BooleanOp bop;
        if (op == "cut")
            bop = geometry::BooleanOp::Cut;
        else if (op == "intersect")
            bop = geometry::BooleanOp::Intersect;
        else
            throw DesignError("Unknown operation '" + op + "' (new_body, join, cut, intersect)");
        if (targets.empty())
            throw DesignError("No body to " + op + " (the result does not touch any body)");
        for (const FeatureId id : targets) {
            geometry::Shape r = geometry::boolean(m_bodies.at(id).shape, tool, bop);
            if (r.isNull() || r.volume() <= 1e-9)
                m_bodies.erase(id);
            else
                m_bodies.at(id).shape = std::move(r);
        }
    };

    /// Selected profiles of a sketch (all by default) as separate faces.
    auto profiles = [&](SketchResult*& sketchOut) {
        const FeatureId sid = required(p, "sketch").get<FeatureId>();
        auto it = m_sketches.find(sid);
        if (it == m_sketches.end())
            throw DesignError("No sketch " + std::to_string(sid) + " at this point of the timeline");
        SketchResult& sk = it->second;
        sketchOut = &sk;
        const auto& all = sk.geometry.profiles;
        if (all.empty())
            throw DesignError(sk.name + " has no closed profile");
        std::vector<geometry::Shape> chosen;
        if (has(p, "profiles")) {
            for (const auto& i : p.at("profiles")) {
                const auto idx = i.get<std::size_t>();
                if (idx >= all.size())
                    throw DesignError(sk.name + " has no profile " + std::to_string(idx) + " (it has " +
                                      std::to_string(all.size()) + ")");
                chosen.push_back(all[idx]);
            }
        } else if (has(p, "points")) {
            for (const auto& pt : p.at("points")) {
                const core::Vec3 w = sk.plane.toWorld(ev.vec2(pt));
                bool found = false;
                for (const auto& prof : all) {
                    if (geometry::faceContains(prof, w)) {
                        chosen.push_back(prof);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    throw DesignError("No profile of " + sk.name + " contains point " + pt.dump());
            }
        } else {
            chosen = all;
        }
        if (chosen.empty())
            throw DesignError("No profile selected");
        sk.consumed = true;
        return chosen;
    };

    // Fusion: Extent "To Object": the body (some of its faces) or the imported surface, solid or
    // scan `to` {body, faces?} | {part}, that an extrusion or rib reaches. `reachShapes` are the
    // profiles, `dir` the way they go (a scan is meshed far enough ahead).
    auto resolveExtentTarget = [&](const Json& to, const std::vector<geometry::Shape>& reachShapes, const core::Vec3& dir,
                                   std::vector<std::size_t>& targetFaces, const char* what) {
        geometry::Shape target;
        if (has(to, "body")) {
            const Body& t = bodyRef(to.at("body"));
            target = t.shape;
            if (has(to, "faces"))
                targetFaces = resolveIndices(target, to.at("faces"), false);
        } else if (has(to, "part")) {
            const auto partId = to.at("part").get<model::PartId>();
            const model::Part* part = m_partLookup ? m_partLookup(partId) : nullptr;
            if (part == nullptr)
                throw DesignError("No part with id " + std::to_string(partId));
            if (part->shape && !part->shape->isNull()) {
                target = *part->shape;
            } else if (part->mesh) {
                core::BoundingBox reach;
                for (const auto& face : reachShapes) {
                    const auto b = face.bounds();
                    reach.extend(b.min);
                    reach.extend(b.max);
                }
                // Far enough to reach the scan wherever it is: the size of the box around
                // both the profile and the scan.
                const auto pb = part->mesh->bounds();
                core::BoundingBox both = reach;
                both.extend(pb.min);
                both.extend(pb.max);
                const double span = 2.0 * (both.max - both.min).length();
                reach.extend(reach.min + dir * span);
                reach.extend(reach.max + dir * span);
                target = scanSurfaceUnder(*part, reach);
            } else {
                throw DesignError("Part '" + part->name + "' has no geometry");
            }
        } else if (has(to, "plane")) {
            // A plane (origin plane, construction plane or {origin, normal}), large enough to
            // reach wherever the profile and the bodies are.
            const Plane pl = resolvePlane(to.at("plane")).normalized();
            const core::Vec3 origin = pl.origin + pl.normal * ev.num(to, "offset", 0.0);
            core::BoundingBox reach;
            reach.extend(origin);
            for (const auto& shape : reachShapes) {
                const auto b = shape.bounds();
                reach.extend(b.min);
                reach.extend(b.max);
            }
            for (const auto& [_, body] : m_bodies) {
                if (body.shape.isNull())
                    continue;
                const auto b = body.shape.bounds();
                reach.extend(b.min);
                reach.extend(b.max);
            }
            target = geometry::planeFace(origin, pl.normal, 2.0 * (reach.max - reach.min).length() + 1.0);
        } else {
            throw DesignError(std::string(what) + " to object: give `to` as {body, faces?}, {part} or {plane}");
        }
        return target;
    };

    auto unionAll = [](const std::vector<geometry::Shape>& solids) {
        geometry::Shape result = solids.front();
        for (std::size_t i = 1; i < solids.size(); ++i)
            result = geometry::boolean(result, solids[i], geometry::BooleanOp::Union);
        return result;
    };

    // The connected sketch curves of a Sweep / Pattern on Path `path` {sketch, curves?} as edges.
    auto pathEdgesOf = [&](const Json& path, const char* what) {
        if (path.is_object() && path.value("deletedCurves", false))
            throw DesignError(std::string("Curves of the ") + what + " path were deleted from its sketch: pick the path again");
        const FeatureId pid = required(path, "sketch").get<FeatureId>();
        auto it = m_sketches.find(pid);
        if (it == m_sketches.end())
            throw DesignError("No path sketch " + std::to_string(pid) + " at this point of the timeline");
        std::vector<SketchCurve> curves;
        if (has(path, "curves")) {
            for (const auto& i : path.at("curves")) {
                const auto idx = i.get<std::size_t>();
                if (idx >= it->second.curves.size())
                    throw DesignError(it->second.name + " has no curve " + std::to_string(idx));
                curves.push_back(it->second.curves[idx]);
            }
        } else {
            curves = it->second.curves;
        }
        it->second.consumed = true;
        return geometry::curveEdges(it->second.plane, curves);
    };

    if (f.type == "sketch") {
        SketchResult sk;
        sk.id = f.id;
        sk.name = f.name;
        sk.plane = has(p, "face") ? resolvePlane(p.at("face")) : resolvePlane(p.value("plane", Json("XY")));
        sk.plane = sk.plane.normalized();
        sk.plane.origin = sk.plane.origin + sk.plane.normal * ev.num(p, "offset", 0.0);
        std::vector<SketchConstraint> constraints;
        if (has(p, "curves")) {
            for (const auto& c : p.at("curves"))
                appendCurves(ev, c, sk.curves, constraints);
        }
        const std::size_t implicitCount = constraints.size();
        if (has(p, "constraints")) {
            for (const auto& c : p.at("constraints"))
                constraints.push_back(parseConstraint(ev, c));
        }
        try {
            if (constraints.size() > implicitCount) {
                const auto solved = solveSketch(sk.curves, constraints);
                if (!solved.solved) {
                    std::string list;
                    for (const std::size_t i : solved.unsatisfied) {
                        if (i >= implicitCount)
                            list += (list.empty() ? "" : ", ") + std::to_string(i - implicitCount);
                    }
                    throw DesignError("Sketch constraints conflict" +
                                      (list.empty() ? std::string(" with the drawn shapes")
                                                    : " (constraint " + list + " cannot be satisfied)"));
                }
                sk.degreesOfFreedom = solved.degreesOfFreedom;
            } else {
                sk.degreesOfFreedom = degreesOfFreedom(sk.curves, constraints);
            }
        } catch (const geometry::ModelingError& e) {
            throw DesignError(e.what());
        }
        sk.constraintCount = constraints.size() - implicitCount;
        sk.geometry = geometry::buildSketch(sk.plane, sk.curves);
        m_sketches[f.id] = std::move(sk);
    } else if (f.type == "extrude") {
        SketchResult* sk = nullptr;
        auto faces = profiles(sk);
        const std::string extent = lower(p.value("extent", std::string("one_side")));
        core::Vec3 dir = sk->plane.normal;
        if (p.value("flip", false))
            dir = dir * -1.0;
        // Fusion: Start > Offset. The profile starts `start_offset` away from the sketch plane,
        // along the extrusion direction, and everything (distance, taper, To Object) counts from
        // there.
        const double startOffset = ev.num(p, "start_offset", 0.0);
        if (std::abs(startOffset) > 1e-9) {
            for (auto& face : faces)
                face = geometry::translated(face, dir * startOffset);
        }
        const double taper = ev.num(p, "taper", 0.0);
        if (extent == "to_object" && taper != 0.0)
            throw DesignError("Taper angle is not available with extent To Object");
        if (extent == "to_object") {
            // Fusion: Extent "To Object": up to a body, some of its faces, or an imported
            // surface, solid or scan.
            std::vector<std::size_t> targetFaces;
            const geometry::Shape target = resolveExtentTarget(required(p, "to"), faces, dir, targetFaces, "Extrude");
            std::vector<geometry::Shape> solids;
            for (const auto& face : faces)
                solids.push_back(geometry::extrudeToObject(face, dir, target, targetFaces));
            applyOperation(unionAll(solids));
            return;
        }
        double d1 = 0.0;
        double d2 = 0.0;
        if (extent == "all" || extent == "all_two_sides") {
            // Fusion: Extent "All": through every body at this point of the timeline, one way or both ways.
            const std::string op = lower(p.value("operation", std::string("new_body")));
            if (op != "cut" && op != "intersect")
                throw DesignError("Extent All goes through every body: use it with the operation cut or intersect");
            if (taper != 0.0)
                throw DesignError("Taper angle is not available with extent All");
            // Far enough to leave the biggest box around the profile and all bodies, wherever they are.
            core::BoundingBox reach;
            for (const auto& face : faces) {
                const auto b = face.bounds();
                reach.extend(b.min);
                reach.extend(b.max);
            }
            for (const auto& [_, body] : m_bodies) {
                if (body.shape.isNull())
                    continue;
                const auto b = body.shape.bounds();
                reach.extend(b.min);
                reach.extend(b.max);
            }
            d1 = 2.0 * (reach.max - reach.min).length() + 1.0;
            d2 = extent == "all_two_sides" ? d1 : 0.0;
        } else {
            d1 = ev.num(p, "distance");
            if (extent == "symmetric")
                d2 = d1;
            else if (extent == "two_sides")
                d2 = ev.num(p, "distance2");
            else if (extent != "one_side")
                throw DesignError("Unknown extent '" + extent + "' (one_side, symmetric, two_sides, to_object, all, all_two_sides)");
        }
        std::vector<geometry::Shape> solids;
        for (const auto& face : faces)
            solids.push_back(geometry::extrude(face, dir, d1, d2, taper));
        applyOperation(unionAll(solids));
    } else if (f.type == "revolve") {
        SketchResult* sk = nullptr;
        const auto faces = profiles(sk);
        core::Vec3 origin{};
        core::Vec3 dir{0, 0, 1};
        const Json axis = p.value("axis", Json("Z"));
        if (axis.is_string()) {
            const std::string a = lower(axis.get<std::string>());
            if (a == "x")
                dir = {1, 0, 0};
            else if (a == "y")
                dir = {0, 1, 0};
            else if (a == "z")
                dir = {0, 0, 1};
            else
                throw DesignError("Unknown axis '" + axis.get<std::string>() + "' (X, Y, Z)");
        } else if (has(axis, "deletedLine")) {
            throw DesignError("The axis line was deleted from " + sk->name + ": pick another axis");
        } else if (has(axis, "line")) {
            const auto idx = axis.at("line").get<std::size_t>();
            if (idx >= sk->curves.size() || sk->curves[idx].kind != SketchCurve::Kind::Line)
                throw DesignError("Curve " + std::to_string(idx) + " of " + sk->name + " is not a line");
            origin = sk->plane.toWorld(sk->curves[idx].points[0]);
            dir = sk->plane.toWorld(sk->curves[idx].points[1]) - origin;
        } else if (has(axis, "feature")) {
            axisFeature(axis, origin, dir);
        } else {
            origin = ev.vec3(axis, "origin", {});
            dir = ev.vec3(axis, "direction", {0, 0, 1});
        }
        // Fusion: Direction One Side / Symmetric / Two Sides. Symmetric: `angle` is the total,
        // half on each side of the sketch plane. Two sides: `angle` forwards, `angle2` backwards.
        const std::string extent = lower(p.value("extent", std::string("one_side")));
        double angle = ev.num(p, "angle", 360.0);
        double back = 0.0;
        if (extent == "symmetric") {
            if (angle <= 0.0 || angle > 360.0)
                throw DesignError("Revolve: the angle must be in (0, 360] degrees");
            if (angle < 360.0 - 1e-9)
                back = angle / 2.0;
            angle -= back;
        } else if (extent == "two_sides") {
            back = ev.num(p, "angle2");
            if (back <= 0.0)
                throw DesignError("Revolve: the second angle must be positive");
            if (angle <= 0.0)
                throw DesignError("Revolve: the angle must be positive");
            if (angle + back > 360.0 + 1e-9)
                throw DesignError("Revolve: the two angles add up to more than 360 degrees");
        } else if (extent != "one_side") {
            throw DesignError("Unknown extent '" + extent + "' (one_side, symmetric, two_sides)");
        }
        std::vector<geometry::Shape> solids;
        for (const auto& face : faces) {
            solids.push_back(back > 0.0
                                 ? geometry::revolve(geometry::rotated(face, origin, dir, -back), origin, dir, angle + back)
                                 : geometry::revolve(face, origin, dir, angle));
        }
        applyOperation(unionAll(solids));
    } else if (f.type == "fillet" || f.type == "chamfer") {
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "edges"), true);
        if (f.type == "fillet") {
            if (has(p, "radius_end") && !(ev.num(p, "radius_end") > 0.0))
                throw DesignError("The end radius of a variable fillet must be positive");
            b.shape = geometry::fillet(b.shape, idx, ev.num(p, "radius"), ev.num(p, "radius_end", 0.0));
        } else {
            geometry::ChamferOptions o;
            const std::string type = lower(p.value("type", std::string("equal_distance")));
            if (type == "equal_distance")
                o.type = geometry::ChamferType::EqualDistance;
            else if (type == "two_distances")
                o.type = geometry::ChamferType::TwoDistances;
            else if (type == "distance_angle")
                o.type = geometry::ChamferType::DistanceAngle;
            else
                throw DesignError("Unknown chamfer type '" + type + "' (equal_distance, two_distances, distance_angle)");
            o.distance = ev.num(p, "distance");
            o.distance2 = o.type == geometry::ChamferType::TwoDistances ? ev.num(p, "distance2") : o.distance;
            o.angle = ev.num(p, "angle", 45.0);
            o.flip = p.value("flip", false);
            b.shape = geometry::chamfer(b.shape, idx, o);
        }
    } else if (f.type == "shell") {
        Body& b = bodyRef(required(p, "body"));
        const auto idx = has(p, "faces") ? resolveIndices(b.shape, p.at("faces"), false) : std::vector<std::size_t>{};
        b.shape = geometry::shell(b.shape, idx, ev.num(p, "thickness"));
    } else if (f.type == "press_pull") {
        // Fusion: Press Pull (Q) on flat faces of a body.
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "faces"), false);
        b.shape = geometry::pressPull(b.shape, idx, ev.num(p, "distance"));
    } else if (f.type == "draft") {
        // Fusion: Modify > Draft. The fixed plane is the hinge and its normal the pull
        // direction; a positive angle leans the faces in going along it.
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "faces"), false);
        const Plane pl = resolvePlane(p.value("plane", Json("XY"))).normalized();
        b.shape = geometry::draft(b.shape, idx, pl.origin, pl.normal, ev.num(p, "angle"));
    } else if (f.type == "thread") {
        // Fusion: Create > Thread (modeled): a helical groove cut along cylindrical faces.
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "faces"), false);
        const std::string hand = lower(p.value("handedness", std::string("right")));
        if (hand != "right" && hand != "left")
            throw DesignError("Unknown handedness '" + hand + "' (right, left)");
        b.shape = geometry::thread(b.shape, idx, ev.num(p, "pitch"), ev.num(p, "depth", 0.0), ev.num(p, "length", 0.0),
                                   hand == "left", p.value("flip", false));
    } else if (f.type == "delete_faces") {
        // Fusion: Modify > Delete on faces: the gap closes from the neighbouring faces.
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "faces"), false);
        b.shape = geometry::deleteFaces(b.shape, idx);
    } else if (f.type == "replace_face") {
        // Fusion: Modify > Replace Face: flat faces of the body move onto a plane, a surface,
        // a scan or faces of another body.
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "faces"), false);
        const Json& toRef = required(p, "to");
        if (has(toRef, "body") && bodyRef(toRef.at("body")).id == b.id)
            throw DesignError("Replace Face: a body cannot be its own tool");
        std::vector<std::size_t> targetFaces;
        // The direction only matters for a scan: its surface is taken along the faces' normal.
        const auto faceInfos = geometry::faces(b.shape);
        if (has(toRef, "part")) {
            for (const std::size_t i : idx) {
                if (faceInfos.at(i).normal.dot(faceInfos.at(idx.front()).normal) < 0.999)
                    throw DesignError("Replace Face with a scan or surface part: pick faces that face the same way");
            }
        }
        const geometry::Shape target = resolveExtentTarget(toRef, {b.shape}, faceInfos.at(idx.front()).normal,
                                                           targetFaces, "Replace Face");
        b.shape = geometry::replaceFaces(b.shape, idx, target, targetFaces);
    } else if (f.type == "split_face") {
        // Fusion: Modify > Split Face by a plane: the faces stay on the same solid.
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "faces"), false);
        const Plane pl = resolvePlane(p.value("plane", Json("YZ"))).normalized();
        const core::Vec3 origin = pl.origin + pl.normal * ev.num(p, "offset", 0.0);
        b.shape = geometry::splitFaces(b.shape, idx, origin, pl.normal);
    } else if (f.type == "split") {
        // Fusion: Split Body by a plane. The body keeps what is behind the plane; a new
        // body gets what is in front of it (along the plane's normal).
        Body& b = bodyRef(required(p, "body"));
        const Plane pl = resolvePlane(p.value("plane", Json("YZ"))).normalized();
        const core::Vec3 origin = pl.origin + pl.normal * ev.num(p, "offset", 0.0);
        auto sides = geometry::splitByPlane(b.shape, origin, pl.normal);
        b.shape = std::move(sides.back);
        newBody(std::move(sides.front));
    } else if (f.type == "combine") {
        Body& target = bodyRef(required(p, "target"));
        if (target.shape.solidCount() == 0)
            throw DesignError("Combine target must be a solid body; use a surface as the tool");
        const std::string op = lower(p.value("operation", std::string("join")));
        geometry::BooleanOp bop;
        if (op == "join")
            bop = geometry::BooleanOp::Union;
        else if (op == "cut")
            bop = geometry::BooleanOp::Cut;
        else if (op == "intersect")
            bop = geometry::BooleanOp::Intersect;
        else
            throw DesignError("Unknown operation '" + op + "' (join, cut, intersect)");
        const bool keepTools = p.value("keepTools", false);
        const bool flip = p.value("flip", false);
        std::vector<FeatureId> tools;
        Json surfaceTools = Json::array();
        // Imported parts as tools: exact solids are used as they are; surfaces and scans
        // cut the target on one side (Fusion: Split Body, keeping one side).
        std::vector<const model::Part*> partTools;
        for (const auto& ref : required(p, "tools")) {
            if (ref.is_object() && has(ref, "part")) {
                const auto partId = ref.at("part").get<model::PartId>();
                const model::Part* part = m_partLookup ? m_partLookup(partId) : nullptr;
                if (part == nullptr)
                    throw DesignError("No part with id " + std::to_string(partId));
                if (part->kind != model::PartKind::Imported)
                    throw DesignError("Part '" + part->name + "' is a timeline part: use its body as the tool");
                partTools.push_back(part);
                continue;
            }
            const Body& t = bodyRef(ref);
            if (t.id == target.id)
                throw DesignError("A body cannot be combined with itself");
            if (std::find(tools.begin(), tools.end(), t.id) == tools.end()) {
                tools.push_back(t.id);
                if (t.shape.solidCount() == 0)
                    surfaceTools.push_back({{"id", t.id}, {"name", t.name}});
            }
        }
        if (tools.empty() && partTools.empty())
            throw DesignError("Select at least one tool body");
        auto splitWithSurface = [&](const geometry::Shape& input, const geometry::Shape& surface,
                                    const std::string& name) {
            if (bop == geometry::BooleanOp::Union)
                throw DesignError("'" + name +
                                  "' is a surface or scan and has no volume to join: use Cut (removes what is "
                                  "behind it) or Intersect (keeps what is behind it)");
            // Cut keeps the front (outside), Intersect the back; Flip swaps the sides.
            const bool keepFront = (bop == geometry::BooleanOp::Cut) != flip;
            try {
                return geometry::splitBySurface(input, surface, keepFront);
            } catch (const geometry::OffsetError& e) {
                throw DesignError(e.what());
            }
        };
        geometry::Shape result = target.shape;
        for (const FeatureId t : tools) {
            if (result.isNull())
                break;
            const Body& tool = m_bodies.at(t);
            // A fitted/trimmed/offset timeline surface is the current evaluated shape,
            // not a stale document part and not a solid boolean operand.
            result = tool.shape.solidCount() > 0 ? geometry::boolean(result, tool.shape, bop)
                                                : splitWithSurface(result, tool.shape, tool.name);
        }
        for (const model::Part* part : partTools) {
            if (result.isNull())
                break;
            const bool solidTool = part->shape && !part->shape->isNull() && part->shape->solidCount() > 0;
            if (solidTool) {
                result = geometry::boolean(result, *part->shape, bop);
                continue;
            }
            if (bop == geometry::BooleanOp::Union)
                throw DesignError("'" + part->name +
                                  "' is a surface or scan and has no volume to join: use Cut (removes what is "
                                  "behind it) or Intersect (keeps what is behind it)");
            geometry::Shape surface;
            if (part->shape && !part->shape->isNull())
                surface = *part->shape;
            else if (part->mesh)
                surface = scanSurfaceUnder(*part, result.bounds());
            else
                throw DesignError("Part '" + part->name + "' has no geometry");
            result = splitWithSurface(result, surface, part->name);
        }
        const FeatureId targetId = target.id;
        if (!keepTools) {
            for (const FeatureId t : tools)
                m_bodies.erase(t);
        }
        if (result.isNull() || result.volume() <= 1e-9)
            m_bodies.erase(targetId);
        else
            m_bodies.at(targetId).shape = result;
        // Editors need the evaluated tool kind even after keepTools=false consumed it.
        // Rebuilt with the feature, this also works when reopening older project files.
        m_reports[f.id] = {{"surfaceTools", std::move(surfaceTools)}};
    } else if (f.type == "boundary_fill") {
        // Fusion: Create > Boundary Fill. The closed spaces between the tool bodies / surfaces
        // become one new body (a solid per space); `cells` keeps only the ones picked by a
        // point inside or by index.
        std::vector<FeatureId> tools;
        std::vector<geometry::Shape> shapes;
        for (const auto& ref : required(p, "tools")) {
            if (ref.is_object() && has(ref, "part")) {
                // A solid or surface outside the timeline (a fin, a STEP import, a fitted surface).
                const auto partId = ref.at("part").get<model::PartId>();
                const model::Part* part = m_partLookup ? m_partLookup(partId) : nullptr;
                if (part == nullptr)
                    throw DesignError("No part with id " + std::to_string(partId));
                if (part->kind != model::PartKind::Imported || !part->shape || part->shape->isNull())
                    throw DesignError("Part '" + part->name + "' has no exact shape to fill between: use a solid or surface");
                shapes.push_back(*part->shape);
                continue;
            }
            const Body& t = bodyRef(ref);
            if (std::find(tools.begin(), tools.end(), t.id) == tools.end()) {
                tools.push_back(t.id);
                shapes.push_back(t.shape);
            }
        }
        if (shapes.empty())
            throw DesignError("Select at least one tool body");
        std::vector<geometry::Shape> cells;
        try {
            cells = geometry::boundaryFill(shapes);
        } catch (const geometry::ModelingError& e) {
            throw DesignError(e.what());
        }
        if (has(p, "cells") && !p.at("cells").empty()) {
            std::vector<geometry::Shape> picked;
            std::set<std::size_t> pickedIdx;
            for (const auto& pick : p.at("cells")) {
                if (pick.is_number_integer()) {
                    const auto n = pick.get<long long>();
                    if (n < 0 || static_cast<std::size_t>(n) >= cells.size())
                        throw DesignError("No space " + std::to_string(n) + " (" + std::to_string(cells.size()) +
                                          " spaces; give each as a point [x, y, z] or an index)");
                    const auto i = static_cast<std::size_t>(n);
                    pickedIdx.insert(i);
                    continue;
                }
                const core::Vec3 pt = ev.vec3(pick.is_object() ? required(pick, "point") : pick);
                const auto hit = std::find_if(cells.begin(), cells.end(),
                                              [&](const geometry::Shape& cell) { return geometry::containsPoint(cell, pt); });
                if (hit == cells.end())
                    throw DesignError("No enclosed space at " + std::to_string(pt.x) + ", " + std::to_string(pt.y) + ", " +
                                      std::to_string(pt.z));
                pickedIdx.insert(static_cast<std::size_t>(hit - cells.begin()));
            }
            // Each space once, in the order the fill made them.
            for (const std::size_t i : pickedIdx)
                picked.push_back(cells[i]);
            cells = std::move(picked);
        }
        geometry::Shape filled = cells.size() == 1 ? cells.front() : geometry::makeCompound(cells);
        if (filled.isNull())
            throw DesignError("Boundary Fill produced no solid");
        // Only timeline tools can be removed; imported solids and surfaces stay.
        if (p.value("removeTools", false)) {
            for (const FeatureId t : tools)
                m_bodies.erase(t);
        }
        newBody(std::move(filled));
    } else if (f.type == "move") {
        // Fusion: Modify > Move/Copy (M). Scales the body uniformly and rotates it about an
        // axis through `center` (default: the centre of its bounding box), then translates it.
        Body& body = bodyRef(required(p, "body"));
        core::Affine tf;
        const core::Vec3 c = has(p, "center") ? ev.vec3(p.at("center")) : body.shape.bounds().center();
        const double factor = ev.num(p, "scale", 1.0);
        if (!std::isfinite(factor) || factor < geometry::kMinScaleFactor || factor > geometry::kMaxScaleFactor)
            throw DesignError("scale must be between 0.000001 and 1000000");
        const double angle = ev.num(p, "angle", 0.0);
        if (std::abs(angle) > 1e-12) {
            core::Vec3 axis{0, 0, 1};
            core::Vec3 pivot = c; // a construction axis turns about itself, not about `center`
            const Json a = p.value("axis", Json("Z"));
            if (a.is_string()) {
                const std::string n = lower(a.get<std::string>());
                if (n == "x")
                    axis = {1, 0, 0};
                else if (n == "y")
                    axis = {0, 1, 0};
                else if (n != "z")
                    throw DesignError("Unknown axis '" + a.get<std::string>() + "' (X, Y, Z or [x, y, z])");
            } else if (has(a, "feature")) {
                axisFeature(a, pivot, axis);
            } else {
                axis = ev.vec3(a);
            }
            if (axis.length() < 1e-12)
                throw DesignError("Rotation axis is zero");
            const core::Mat3 rot = core::Mat3::rotation(axis.normalized(), angle * std::numbers::pi / 180.0);
            tf = core::Affine{rot, pivot - rot * pivot};
        }
        if (has(p, "translate"))
            tf = core::Affine::translationBy(ev.vec3(p.at("translate"))).after(tf);
        // Create Copy (Fusion): the moved shape becomes a new body and the original stays.
        geometry::Shape moved = factor != 1.0 ? geometry::scaled(body.shape, factor, c) : body.shape;
        moved = geometry::transformed(moved, tf);
        if (p.value("copy", false))
            newBody(std::move(moved));
        else
            body.shape = std::move(moved);
    } else if (f.type == "align") {
        // Fusion: Modify > Align: turns and moves a body so a planar face of it lies flat against
        // a plane or planar face of another body (faces opposing, like parts mated together).
        Body& body = bodyRef(required(p, "body"));
        const Json& faceRef = required(p, "face");
        Json spec = Json::object({{"body", p.at("body")}});
        if (faceRef.is_number_integer()) {
            spec["index"] = faceRef;
        } else if (faceRef.is_object() && (has(faceRef, "point") || has(faceRef, "index"))) {
            if (has(faceRef, "body") && bodyRef(faceRef.at("body")).id != body.id)
                throw DesignError("The face to align must be a face of the body that moves; the plane or face to align to is `to`");
            spec[has(faceRef, "index") ? "index" : "point"] = faceRef.at(has(faceRef, "index") ? "index" : "point");
        } else {
            throw DesignError("face is a face index, {point: [x, y, z]} or {index} on the body");
        }
        const Plane from = resolvePlane(spec).normalized();
        const Json& toRef = required(p, "to");
        const Plane to = resolvePlane(toRef).normalized();
        if (toRef.is_object() && has(toRef, "body") && bodyRef(toRef.at("body")).id == body.id)
            throw DesignError("A body cannot be aligned to its own face");
        const bool flip = p.value("flip", false);
        if (!(to.normal.length() > 1e-9) || !(from.normal.length() > 1e-9))
            throw DesignError("Align: the plane has no direction (zero normal)");
        const core::Vec3 want = flip ? to.normal : to.normal * -1.0;
        // Smallest turn taking the face normal to the wanted direction; a half turn about any
        // axis square to it when they are opposed.
        const core::Vec3 n1 = from.normal;
        core::Mat3 rot{};
        const core::Vec3 axis = n1.cross(want);
        if (axis.length() > 1e-9) {
            rot = core::Mat3::rotation(axis.normalized(), std::atan2(axis.length(), n1.dot(want)));
        } else if (n1.dot(want) < 0.0) {
            core::Vec3 any = std::abs(n1.x) < 0.9 ? core::Vec3{1, 0, 0} : core::Vec3{0, 1, 0};
            rot = core::Mat3::rotation(n1.cross(any).normalized(), std::numbers::pi);
        }
        core::Affine tf{rot, from.origin - rot * from.origin};
        // Then slide the turned face onto the plane, `offset` mm clear of it along its normal.
        // With `center` its centre also moves over the target face's centre.
        const core::Vec3 centre = tf.apply(from.origin);
        const core::Vec3 gap = to.origin - centre;
        core::Vec3 shift = to.normal * (to.normal.dot(gap) + ev.num(p, "offset", 0.0));
        if (p.value("center", false))
            shift = shift + (gap - to.normal * to.normal.dot(gap));
        tf = core::Affine::translationBy(shift).after(tf);
        body.shape = geometry::transformed(body.shape, tf);
    } else if (f.type == "scale") {
        // Fusion: Modify > Scale. Uniform (`factor`) or non-uniform (`factor_x`, `_y`, `_z`; a
        // missing one is 1) about `center` (default: the centre of the body's bounding box).
        Body& body = bodyRef(required(p, "body"));
        const core::Vec3 c = has(p, "center") ? ev.vec3(p.at("center")) : body.shape.bounds().center();
        const bool nonUniform = has(p, "factor_x") || has(p, "factor_y") || has(p, "factor_z");
        if (nonUniform && has(p, "factor"))
            throw DesignError("Give either `factor` or factor_x / factor_y / factor_z");
        const double uniform = ev.num(p, "factor", 1.0);
        const double fx = ev.num(p, "factor_x", uniform), fy = ev.num(p, "factor_y", uniform), fz = ev.num(p, "factor_z", uniform);
        for (const double v : {fx, fy, fz}) {
            if (!std::isfinite(v) || v < geometry::kMinScaleFactor || v > geometry::kMaxScaleFactor)
                throw DesignError("scale factors must be between 0.000001 and 1000000");
        }
        if (fx == fy && fy == fz)
            body.shape = geometry::scaled(body.shape, fx, c);
        else
            body.shape = geometry::scaledNonUniform(body.shape, fx, fy, fz, c);
    } else if (f.type == "mirror" || f.type == "rectangular_pattern" || f.type == "circular_pattern") {
        // Fusion: Mirror, Rectangular Pattern, Circular Pattern of a body. "join" (default)
        // merges the copies into the body, "new_body" makes one new body of the copies.
        Body& src = bodyRef(required(p, "body"));
        const geometry::Shape original = src.shape;
        std::vector<geometry::Shape> copies;
        if (f.type == "mirror") {
            const Plane mirrorPlane = resolvePlane(p.value("plane", Json("YZ"))).normalized();
            core::Vec3 origin = mirrorPlane.origin;
            const core::Vec3 normal = mirrorPlane.normal;
            origin = origin + normal.normalized() * ev.num(p, "offset", 0.0);
            copies.push_back(geometry::mirrored(original, origin, normal));
        } else {
            auto direction = [&](const Json& d, core::Vec3 fallback) -> core::Vec3 {
                if (d.is_null())
                    return fallback;
                if (d.is_string()) {
                    const std::string a = lower(d.get<std::string>());
                    if (a == "x")
                        return {1, 0, 0};
                    if (a == "y")
                        return {0, 1, 0};
                    if (a == "z")
                        return {0, 0, 1};
                    throw DesignError("Unknown direction '" + d.get<std::string>() + "' (X, Y, Z or [x, y, z])");
                }
                return ev.vec3(d);
            };
            auto count = [&](const char* key) {
                const long n = std::lround(ev.num(p, key, 1.0));
                if (n < 1 || n > 1000)
                    throw DesignError(std::string(key) + " must be between 1 and 1000");
                return static_cast<int>(n);
            };
            if (f.type == "rectangular_pattern") {
                const core::Vec3 d1 = direction(p.value("direction", Json("X")), {1, 0, 0}).normalized();
                const core::Vec3 d2 = direction(p.value("direction2", Json("Y")), {0, 1, 0}).normalized();
                const int n1 = count("count");
                const int n2 = has(p, "count2") ? count("count2") : 1;
                const double s1 = ev.num(p, "spacing");
                const double s2 = n2 > 1 ? ev.num(p, "spacing2") : 0.0;
                for (int i = 0; i < n1; ++i) {
                    for (int j = 0; j < n2; ++j) {
                        if (i == 0 && j == 0)
                            continue;
                        copies.push_back(geometry::translated(original, d1 * (s1 * i) + d2 * (s2 * j)));
                    }
                }
            } else {
                core::Vec3 origin{};
                core::Vec3 dir{0, 0, 1};
                const Json axis = p.value("axis", Json("Z"));
                if (axis.is_string()) {
                    dir = direction(axis, {0, 0, 1});
                } else if (has(axis, "feature")) {
                    axisFeature(axis, origin, dir);
                } else {
                    origin = ev.vec3(axis, "origin", {});
                    dir = ev.vec3(axis, "direction", {0, 0, 1});
                }
                const int n = count("count");
                const double total = ev.num(p, "angle", 360.0);
                // A full circle spreads the copies evenly without one on top of the original.
                const double step = std::abs(std::abs(total) - 360.0) < 1e-9 ? total / n : (n > 1 ? total / (n - 1) : 0.0);
                for (int i = 1; i < n; ++i)
                    copies.push_back(geometry::rotated(original, origin, dir, step * i));
            }
        }
        if (copies.empty())
            throw DesignError("The pattern makes no copies (count 1)");
        const std::string op = lower(p.value("operation", std::string("join")));
        if (op == "join") {
            geometry::Shape result = original;
            for (const auto& c : copies)
                result = geometry::boolean(result, c, geometry::BooleanOp::Union);
            src.shape = result;
        } else if (op == "new_body") {
            newBody(unionAll(copies));
        } else {
            throw DesignError("Unknown operation '" + op + "' (join, new_body)");
        }
    } else if (f.type == "coil") {
        // Fusion: Create > Coil. A spring: `diameter` is the outer diameter of the coil, any two of
        // `revolutions`, `pitch` and `height` fix its length.
        core::Vec3 center = ev.vec3(p, "center", {});
        core::Vec3 axis{0, 0, 1};
        if (has(p, "axis")) {
            const Json& a = p.at("axis");
            if (a.is_object()) {
                // An edge, a cylindrical face or a construction axis: {origin, direction} | {feature}.
                // The coil starts on that axis unless `center` says otherwise.
                core::Vec3 origin{}, dir{0, 0, 1};
                if (has(a, "feature")) {
                    axisFeature(a, origin, dir);
                } else if (has(a, "direction")) {
                    origin = ev.vec3(a, "origin", {});
                    dir = ev.vec3(a, "direction", {0, 0, 1});
                } else {
                    throw DesignError("The coil axis object needs {feature} or {origin, direction}");
                }
                if (dir.length() < 1e-9)
                    throw DesignError("The coil axis direction must not be zero");
                axis = dir;
                if (!has(p, "center"))
                    center = origin;
            } else if (a.is_string()) {
                const std::string name = lower(a.get<std::string>());
                if (name == "x")
                    axis = {1, 0, 0};
                else if (name == "y")
                    axis = {0, 1, 0};
                else if (name != "z")
                    throw DesignError("Unknown axis '" + name + "' (X, Y, Z, [x, y, z], {origin, direction} or {feature})");
            } else {
                axis = ev.vec3(a);
            }
        }
        const double outerDiameter = ev.num(p, "diameter");
        const double size = ev.num(p, "section_size");
        const bool hasRev = has(p, "revolutions"), hasPitch = has(p, "pitch"), hasHeight = has(p, "height");
        if (int(hasRev) + int(hasPitch) + int(hasHeight) != 2)
            throw DesignError("Give two of revolutions, pitch and height");
        double revolutions = hasRev ? ev.num(p, "revolutions") : 0.0;
        double pitch = hasPitch ? ev.num(p, "pitch") : 0.0;
        const double height = hasHeight ? ev.num(p, "height") : 0.0;
        if ((hasRev && !(revolutions > 0.0)) || (hasPitch && !(pitch > 0.0)) || (hasHeight && !(height > 0.0)))
            throw DesignError("The revolutions, pitch and height must be positive");
        if (!hasPitch)
            pitch = height / revolutions;
        if (!hasRev)
            revolutions = height / pitch;
        const std::string section = lower(p.value("section", std::string("circular")));
        const std::string hand = lower(p.value("handedness", std::string("right")));
        if (section != "circular" && section != "square")
            throw DesignError("Unknown section '" + section + "' (circular, square)");
        if (hand != "right" && hand != "left")
            throw DesignError("Unknown handedness '" + hand + "' (right, left)");
        applyOperation(geometry::coil(center, axis, (outerDiameter - size) / 2.0, pitch, revolutions, size,
                                      section == "square", hand == "left"));
    } else if (f.type == "rib") {
        // Fusion: Create > Rib. A wall built on flat sketch curves: `thickness` wide, `depth` tall,
        // either standing up from the sketch plane (normal) or reaching from the curves along the
        // plane (parallel: thickness is then measured across the plane, centred on it).
        const Json& path = required(p, "path");
        const geometry::Shape pathEdges = pathEdgesOf(path, "rib");
        const Plane plane = m_sketches.at(path.at("sketch").get<FeatureId>()).plane;
        const double thickness = ev.num(p, "thickness");
        // Fusion: Extent "To Next": the wall grows until it reaches `to` {body, faces?} | {part}.
        const std::string extent = lower(p.value("extent", std::string("distance")));
        if (extent != "distance" && extent != "to_object")
            throw DesignError("Unknown rib extent '" + extent + "' (distance, to_object)");
        const bool toObject = extent == "to_object";
        const double depth = toObject ? 0.0 : ev.num(p, "depth");
        const std::string direction = lower(p.value("direction", std::string("normal")));
        if (direction != "normal" && direction != "parallel")
            throw DesignError("Unknown rib direction '" + direction + "' (normal, parallel)");
        if (!(thickness > 0.0) || (!toObject && !(depth > 0.0)))
            throw DesignError("The rib thickness and depth must be positive");
        const double sign = p.value("flip", false) ? -1.0 : 1.0;
        const Json* toRef = toObject ? &required(p, "to") : nullptr;
        const auto toTarget = [&](const geometry::Shape& start, const core::Vec3& way) {
            std::vector<std::size_t> targetFaces;
            const geometry::Shape target = resolveExtentTarget(*toRef, {start}, way, targetFaces, "Rib");
            return geometry::extrudeToObject(start, way, target, targetFaces);
        };
        // Fusion: Web. Curves that do not join (crossing, or apart) are walls of their own, all in one body.
        const auto buildWall = [&](const geometry::Shape& group) {
            if (direction == "normal") {
                const geometry::Shape outline = geometry::thickenCurves(group, thickness, plane.normal);
                const core::Vec3 up = plane.normal * sign;
                return toObject ? toTarget(outline, up) : geometry::extrude(outline, up, depth);
            }
            const double length = geometry::pathLength(group);
            const auto ends = geometry::pathFrames(group, {0.0, length});
            const core::Vec3 chord = ends[1].point - ends[0].point;
            if (chord.length() < 1e-6)
                throw DesignError("A parallel rib or web needs open curves: a curve here ends where it starts (use the normal direction)");
            const core::Vec3 side = plane.normal.cross(chord).normalized() * sign;
            if (toObject) {
                // The curves stood up `thickness` across the plane, centred on it, grow along `side`.
                const geometry::Shape band = geometry::translated(geometry::curveWall(group, plane.normal, thickness),
                                                                  plane.normal * (-thickness / 2.0));
                return toTarget(band, side);
            }
            const geometry::Shape wall = geometry::curveWall(group, side, depth);
            return geometry::extrude(wall, plane.normal, thickness / 2.0, thickness / 2.0);
        };
        try {
            std::vector<geometry::Shape> walls;
            for (const auto& group : geometry::splitPaths(pathEdges))
                walls.push_back(buildWall(group));
            applyOperation(unionAll(walls));
        } catch (const geometry::ModelingError& e) {
            throw DesignError(e.what());
        }
    } else if (f.type == "pipe") {
        // Fusion: Create > Pipe. A round (or square) section swept along sketch curves, solid or
        // hollow (`thickness` = wall).
        const geometry::Shape pathEdges = pathEdgesOf(required(p, "path"), "pipe");
        if (has(p, "diameter") && has(p, "radius"))
            throw DesignError("Give the pipe size as either diameter or radius, not both");
        if (!has(p, "diameter") && !has(p, "radius"))
            throw DesignError("Give the pipe size: diameter or radius");
        const double outer = has(p, "diameter") ? ev.num(p, "diameter") / 2.0 : ev.num(p, "radius");
        const double wall = ev.num(p, "thickness", 0.0);
        const std::string section = lower(p.value("section", std::string("circular")));
        if (section != "circular" && section != "square")
            throw DesignError("Unknown section '" + section + "' (circular, square)");
        if (!(outer > 0.0))
            throw DesignError("The pipe size must be positive");
        if (wall < 0.0 || wall >= outer)
            throw DesignError("The wall thickness must be from 0 up to (not including) the pipe radius");
        const auto start = geometry::pathFrames(pathEdges, {0.0}).front();
        Plane at;
        at.origin = start.point;
        at.normal = start.tangent;
        at.xDir = std::abs(start.tangent.z) < 0.9 ? core::Vec3{0, 0, 1}.cross(start.tangent) : core::Vec3{1, 0, 0};
        at = at.normalized();
        auto sectionFace = [&](double half) {
            std::vector<SketchCurve> c;
            if (section == "circular") {
                SketchCurve circle{SketchCurve::Kind::Circle, {{0, 0}}};
                circle.radius = half;
                c.push_back(circle);
            } else {
                c = {lineCurve({-half, -half}, {half, -half}, false), lineCurve({half, -half}, {half, half}, false),
                     lineCurve({half, half}, {-half, half}, false), lineCurve({-half, half}, {-half, -half}, false)};
            }
            return geometry::buildSketch(at, c).profiles.at(0);
        };
        geometry::Shape pipe = geometry::sweep(sectionFace(outer), pathEdges);
        if (wall > 0.0) {
            const geometry::Shape bore = geometry::sweep(sectionFace(outer - wall), pathEdges);
            pipe = geometry::boolean(pipe, bore, geometry::BooleanOp::Cut);
        }
        applyOperation(pipe);
    } else if (f.type == "path_pattern") {
        // Fusion: Create > Pattern > Pattern on Path. Copies of a body along sketch curves; the
        // body stays where it is, each copy moves by the path point it lands on minus the point
        // where the first instance is, and (Orientation: Path Direction) turns with the path.
        Body& src = bodyRef(required(p, "body"));
        const geometry::Shape original = src.shape;
        const geometry::Shape pathEdges = pathEdgesOf(required(p, "path"), "pattern");
        const double length = geometry::pathLength(pathEdges);
        const double countValue = ev.num(p, "count", 2.0);
        const long n = std::lround(countValue);
        if (n < 2 || n > 1000 || std::abs(countValue - static_cast<double>(n)) > 1e-9)
            throw DesignError("count must be a whole number from 2 to 1000");
        const double startFraction = ev.num(p, "start", 0.0);
        if (startFraction < 0.0 || startFraction >= 1.0)
            throw DesignError("start must be from 0 up to (not including) 1: a fraction of the path");
        const double start = startFraction * length;
        const std::string type = lower(p.value("distance_type", std::string("extent")));
        double step = 0.0;
        if (type == "spacing") {
            if (!has(p, "distance"))
                throw DesignError("Spacing needs a distance (the gap between instances)");
            step = ev.num(p, "distance");
        } else if (type == "extent") {
            const double extent = has(p, "distance") ? ev.num(p, "distance") : length - start;
            // Over a whole closed path the last instance would land on the first: spread them evenly.
            const auto ends = geometry::pathFrames(pathEdges, {0.0, length});
            const bool closed = (ends[1].point - ends[0].point).length() < 1e-6 * std::max(1.0, length);
            const bool wholeLoop = closed && start == 0.0 && std::abs(extent - length) < 1e-9 * std::max(1.0, length);
            step = extent / static_cast<double>(wholeLoop ? n : n - 1);
        } else {
            throw DesignError("Unknown distance_type '" + type + "' (extent, spacing)");
        }
        if (!(step > 0.0))
            throw DesignError("The distance must be positive");
        const bool reverse = p.value("flip", false);
        std::vector<double> at;
        for (long i = 0; i < n; ++i) {
            const double d = start + step * static_cast<double>(i);
            if (d > length + 1e-6)
                throw DesignError("The pattern runs off the end of the path (" + std::to_string(length) +
                                  " mm long): use a smaller distance or fewer instances");
            at.push_back(reverse ? length - d : d);
        }
        const auto frames = geometry::pathFrames(pathEdges, at);
        const bool follow = lower(p.value("orientation", std::string("identical"))) == "path_direction";
        auto dirOf = [&](const geometry::PathFrame& fr) { return reverse ? fr.tangent * -1.0 : fr.tangent; };
        std::vector<geometry::Shape> copies;
        for (std::size_t i = 1; i < frames.size(); ++i) {
            geometry::Shape copy = original;
            if (follow) {
                const core::Vec3 a = dirOf(frames[0]), b = dirOf(frames[i]);
                const core::Vec3 axis = a.cross(b);
                const double sine = axis.length();
                const double cosine = a.dot(b);
                if (sine > 1e-9)
                    copy = geometry::rotated(copy, frames[0].point, axis * (1.0 / sine), std::atan2(sine, cosine) * 180.0 / std::numbers::pi);
                else if (cosine < 0.0)
                    throw DesignError("The path turns back on itself: Path Direction cannot orient a copy");
            }
            copies.push_back(geometry::translated(copy, frames[i].point - frames[0].point));
        }
        const std::string op = lower(p.value("operation", std::string("join")));
        if (op == "join") {
            geometry::Shape result = original;
            for (const auto& c : copies)
                result = geometry::boolean(result, c, geometry::BooleanOp::Union);
            src.shape = result;
        } else if (op == "new_body") {
            newBody(unionAll(copies));
        } else {
            throw DesignError("Unknown operation '" + op + "' (join, new_body)");
        }
    } else if (f.type == "loft") {
        // Fusion: Loft through profiles of several sketches, in order.
        std::vector<geometry::Shape> sections;
        for (const auto& sec : required(p, "sections")) {
            FeatureId sid = 0;
            std::size_t profile = 0;
            if (sec.is_number_integer()) {
                sid = sec.get<FeatureId>();
            } else {
                sid = required(sec, "sketch").get<FeatureId>();
                profile = sec.value("profile", std::size_t{0});
            }
            auto it = m_sketches.find(sid);
            if (it == m_sketches.end())
                throw DesignError("No sketch " + std::to_string(sid) + " at this point of the timeline");
            const auto& profs = it->second.geometry.profiles;
            if (profile >= profs.size())
                throw DesignError(it->second.name + " has no profile " + std::to_string(profile));
            sections.push_back(profs[profile]);
            it->second.consumed = true;
        }
        applyOperation(geometry::loft(sections, p.value("solid", true), p.value("ruled", false)));
    } else if (f.type == "sweep") {
        // Fusion: Sweep profiles of `sketch` along curves of another (or the same) sketch.
        SketchResult* sk = nullptr;
        const auto faces = profiles(sk);
        const geometry::Shape pathEdges = pathEdgesOf(required(p, "path"), "sweep");
        // Fusion: Twist Angle, Taper (as an end scale) and Orientation.
        geometry::SweepOptions options;
        options.twistDegrees = ev.num(p, "twist", 0.0);
        options.endScale = ev.num(p, "end_scale", 1.0);
        const std::string orientation = lower(p.value("orientation", std::string("perpendicular")));
        if (orientation != "perpendicular" && orientation != "parallel")
            throw DesignError("Unknown sweep orientation '" + orientation + "' (perpendicular, parallel)");
        options.parallel = orientation == "parallel";
        std::vector<geometry::Shape> solids;
        for (const auto& face : faces)
            solids.push_back(geometry::sweep(face, pathEdges, options));
        applyOperation(unionAll(solids));
    } else if (f.type == "box") {
        const core::Vec3 corner = ev.vec3(p, "corner", {});
        const core::Vec3 size = ev.vec3(required(p, "size"));
        if (size.x <= 0 || size.y <= 0 || size.z <= 0)
            throw DesignError("Box size must be positive");
        Plane base = Plane::xy();
        base.origin = corner;
        const auto g = geometry::buildSketch(
            base, {lineCurve({0, 0}, {size.x, 0}, false), lineCurve({size.x, 0}, {size.x, size.y}, false),
                   lineCurve({size.x, size.y}, {0, size.y}, false), lineCurve({0, size.y}, {0, 0}, false)});
        applyOperation(geometry::extrude(g.profiles.at(0), {0, 0, 1}, size.z));
    } else if (f.type == "cylinder") {
        const core::Vec3 center = ev.vec3(p, "center", {});
        const double r = has(p, "diameter") ? ev.num(p, "diameter") / 2 : ev.num(p, "radius");
        const double h = ev.num(p, "height");
        if (r <= 0)
            throw DesignError("Cylinder radius must be positive");
        Plane base = Plane::xy();
        base.origin = center;
        SketchCurve c{SketchCurve::Kind::Circle, {{0, 0}}};
        c.radius = r;
        const auto g = geometry::buildSketch(base, {c});
        applyOperation(geometry::extrude(g.profiles.at(0), {0, 0, 1}, h));
    } else if (f.type == "sphere") {
        // Fusion: Create > Sphere.
        const core::Vec3 center = ev.vec3(p, "center", {});
        const double d = ev.num(p, "diameter");
        if (d <= 0)
            throw DesignError("Sphere diameter must be positive");
        applyOperation(geometry::sphere(center, d / 2));
    } else if (f.type == "torus") {
        // Fusion: Create > Torus, around +Z: `diameter` of the ring's centre line, `tube_diameter`
        // of the tube.
        const core::Vec3 center = ev.vec3(p, "center", {});
        const double d = ev.num(p, "diameter");
        const double t = ev.num(p, "tube_diameter");
        if (d <= 0 || t <= 0)
            throw DesignError("Torus diameters must be positive");
        if (t >= d)
            throw DesignError("Torus: the tube does not fit; its diameter must be smaller than the ring diameter");
        applyOperation(geometry::torus(center, d / 2, t / 2));
    } else if (f.type == "hole") {
        // Simple drilled hole (Fusion: Hole, H): a cylinder cut into the body. Either one
        // hole normal to the planar face at `point`, or (Fusion: Placement "From Sketch")
        // one hole at every point of a sketch, along the sketch's normal.
        Body& b = bodyRef(required(p, "body"));
        const double r = ev.num(p, "diameter") / 2;
        if (r <= 0)
            throw DesignError("Hole diameter must be positive");
        const bool through = p.value("through", !has(p, "depth"));
        const double depth = through ? 0.0 : ev.num(p, "depth");
        if (!through && depth <= 0)
            throw DesignError("Hole depth must be positive");
        // Fusion: Hole type Counterbore (a wider, flat-bottomed seat) or Countersink (a cone at the mouth).
        const std::string holeType = lower(p.value("hole_type", std::string("simple")));
        if (holeType != "simple" && holeType != "counterbore" && holeType != "countersink")
            throw DesignError("Unknown hole_type '" + holeType + "' (simple, counterbore, countersink)");
        double seatRadius = 0.0, seatDepth = 0.0, sinkAngle = 90.0;
        if (holeType == "counterbore") {
            seatRadius = ev.num(p, "cbore_diameter") / 2;
            seatDepth = ev.num(p, "cbore_depth");
            if (!(seatRadius > r))
                throw DesignError("The counterbore diameter must be larger than the hole diameter");
            if (!(seatDepth > 0))
                throw DesignError("The counterbore depth must be positive");
            if (!through && seatDepth >= depth)
                throw DesignError("The counterbore must be shallower than the hole");
        } else if (holeType == "countersink") {
            seatRadius = ev.num(p, "csink_diameter") / 2;
            sinkAngle = ev.num(p, "csink_angle", 90.0);
            if (!(seatRadius > r))
                throw DesignError("The countersink diameter must be larger than the hole diameter");
            if (!(sinkAngle > 0.0 && sinkAngle < 175.0))
                throw DesignError("The countersink angle must be between 0 and 175 degrees");
            seatDepth = (seatRadius - r) / std::tan(sinkAngle * std::numbers::pi / 360.0);
            if (!through && seatDepth >= depth)
                throw DesignError("The countersink is deeper than the hole: make the hole deeper or the countersink narrower");
        }
        std::vector<geometry::Plane> bases; // origin: hole centre; normal: drilling direction reversed
        if (has(p, "sketch")) {
            if (has(p, "point"))
                throw DesignError("A hole is placed at a point or at a sketch's points, not both");
            const FeatureId sid = p.at("sketch").get<FeatureId>();
            auto it = m_sketches.find(sid);
            if (it == m_sketches.end())
                throw DesignError("No sketch " + std::to_string(sid) + " at this point of the timeline");
            SketchResult& sk = it->second;
            if (p.value("deletedSketchPoints", false))
                throw DesignError("Some sketch points of this hole were deleted; edit the hole to pick them again");
            std::vector<std::size_t> picked;
            if (has(p, "sketch_points")) {
                for (const auto& i : p.at("sketch_points")) {
                    const auto idx = i.get<std::size_t>();
                    if (idx >= sk.curves.size() || sk.curves[idx].kind != SketchCurve::Kind::Point)
                        throw DesignError(sk.name + " has no sketch point " + std::to_string(idx));
                    picked.push_back(idx);
                }
            } else {
                for (std::size_t i = 0; i < sk.curves.size(); ++i)
                    if (sk.curves[i].kind == SketchCurve::Kind::Point)
                        picked.push_back(i);
            }
            if (picked.empty())
                throw DesignError(sk.name + " has no sketch points; add them with Sketch > Point");
            // Drill towards the body (the side of the sketch plane its middle is on); Flip
            // drills the other way.
            core::Vec3 n = sk.plane.normal;
            if (n.dot(b.shape.bounds().center() - sk.plane.origin) > 0)
                n = n * -1.0;
            if (p.value("flip", false))
                n = n * -1.0;
            for (const auto idx : picked) {
                geometry::Plane base = sk.plane;
                base.origin = sk.plane.toWorld(sk.curves[idx].points.at(0));
                base.normal = n;
                bases.push_back(base);
            }
            sk.consumed = true;
        } else {
            const core::Vec3 point = ev.vec3(required(p, "point"));
            const auto faceIdx = geometry::nearestFace(b.shape, point);
            const auto plane = faceIdx ? geometry::facePlane(b.shape, *faceIdx) : std::nullopt;
            if (!plane)
                throw DesignError("Holes need a planar face at the given point");
            geometry::Plane base = *plane;
            base.origin = point - plane->normal * plane->normal.dot(point - plane->origin); // onto the face
            bases.push_back(base);
        }
        geometry::Shape result = b.shape;
        const core::BoundingBox bounds = b.shape.bounds();
        for (const auto& base : bases) {
            // Through all reaches past the body from where the hole starts: a sketch plane
            // may lie some way off the body.
            const double length =
                through ? std::abs(base.normal.dot(bounds.center() - base.origin)) + bounds.diagonal() * 2 : depth;
            SketchCurve c{SketchCurve::Kind::Circle, {{0, 0}}};
            c.radius = r;
            const auto g = geometry::buildSketch(base, {c});
            // Start a hair outside the face so the cut is clean (and does not nick a body
            // on the other side of a sketch plane).
            const geometry::Shape tool = geometry::extrude(g.profiles.at(0), base.normal * -1.0, length, 1e-3);
            result = geometry::boolean(result, tool, geometry::BooleanOp::Cut);
            if (result.isNull())
                throw DesignError("The hole removed the whole body");
            if (holeType != "simple") {
                SketchCurve seat{SketchCurve::Kind::Circle, {{0, 0}}};
                seat.radius = seatRadius;
                const auto sg = geometry::buildSketch(base, {seat});
                // A counterbore is a straight cylinder, a countersink a cone whose walls lean by half its angle.
                // The cone runs a hair past the hole wall, inside the hole, so its edge does not touch it;
                // never past its own apex (small holes with a wide angle have little cone left).
                const double overshoot =
                    holeType == "countersink" ? std::min(1e-2, 0.5 * r / std::tan(sinkAngle * std::numbers::pi / 360.0)) : 0.0;
                const geometry::Shape seatTool = geometry::extrude(sg.profiles.at(0), base.normal * -1.0,
                                                                   seatDepth + overshoot, 1e-3,
                                                                   holeType == "countersink" ? sinkAngle / 2.0 : 0.0);
                result = geometry::boolean(result, seatTool, geometry::BooleanOp::Cut);
                if (result.isNull())
                    throw DesignError("The " + holeType + " removed the whole body");
            }
        }
        b.shape = result;
    } else if (f.type == "base_body") {
        // An imported part as a body: its exact B-rep (STEP, IGES, BREP) or, for meshes,
        // a faceted B-rep built from the triangles.
        const auto partId = required(p, "part").get<model::PartId>();
        const model::Part* part = m_partLookup ? m_partLookup(partId) : nullptr;
        if (part == nullptr)
            throw DesignError("No part with id " + std::to_string(partId));
        if (part->kind != model::PartKind::Imported)
            throw DesignError("Part '" + part->name + "' is already produced by the timeline");
        geometry::Shape shape;
        if (part->shape && !part->shape->isNull()) {
            shape = *part->shape;
        } else if (part->mesh) {
            auto cached = m_convertedParts.find(partId);
            if (cached != m_convertedParts.end() && cached->second.mesh.lock() == part->mesh) {
                shape = cached->second.shape;
            } else {
                shape = geometry::meshToBrep(*part->mesh);
                m_convertedParts[partId] = {part->mesh, shape};
            }
        } else {
            throw DesignError("Part '" + part->name + "' has no geometry");
        }
        newBody(shape);
        m_bodies[f.id].name = part->name;
    } else if (f.type == "plane") {
        // Fusion: Construct > Offset Plane, Plane at Angle, Midplane, Plane Through Three Points.
        Plane pl;
        if (has(p, "path")) {
            for (const char* other : {"points", "base", "base2", "angle", "axis", "about", "tangent"})
                if (has(p, other))
                    throw DesignError(std::string("A plane along a path takes `path`, `position` and `offset`, not `") + other + "`");
        } else if (has(p, "position")) {
            throw DesignError("`position` goes with `path`");
        }
        if (has(p, "tangent")) {
            // Tangent Plane: touches a curved face at `tangent.point` (a point on or near the face: where
            // it should touch), its normal pointing out of the body; `offset` moves it along that normal.
            for (const char* other : {"points", "base", "base2", "angle", "axis", "about"})
                if (has(p, other))
                    throw DesignError(std::string("A tangent plane takes `tangent` and `offset`, not `") + other + "`");
            const Json& ref = p.at("tangent");
            if (!ref.is_object() || !has(ref, "body") || !has(ref, "point"))
                throw DesignError("`tangent` must be {body, point}: click a curved face of a body where the plane should touch it");
            const Body& body = bodyRef(ref.at("body"));
            std::size_t idx = 0;
            if (has(ref, "index")) {
                if (!ref.at("index").is_number_unsigned())
                    throw DesignError("`tangent.index` must be a face index (0 or more)");
                idx = ref.at("index").get<std::size_t>();
            } else {
                const auto found = matchTopology(body.shape, ref, false, ev);
                if (!found)
                    throw DesignError("No face near " + ref.dump() + " on " + body.name);
                idx = *found;
            }
            const auto infos = geometry::faces(body.shape);
            if (idx >= infos.size())
                throw DesignError(body.name + " has no face " + std::to_string(idx));
            // The touch point follows the body: the hint's relative position in its current box, if any.
            core::Vec3 near = ev.vec3(ref.at("point"));
            if (has(ref, "relative")) {
                const core::Vec3 rel = ev.vec3(ref.at("relative"));
                const auto box = body.shape.bounds();
                const core::Vec3 size = box.size();
                near = {box.min.x + rel.x * size.x, box.min.y + rel.y * size.y, box.min.z + rel.z * size.z};
            }
            const auto sp = geometry::faceSurfaceNear(body.shape, idx, near);
            if (!sp)
                throw DesignError("The surface of face " + std::to_string(idx) + " of " + body.name + " has no normal there");
            pl.origin = sp->point;
            pl.normal = sp->normal;
            pl.xDir = std::abs(sp->normal.z) < 0.9 ? core::Vec3{0, 0, 1}.cross(sp->normal) : core::Vec3{1, 0, 0};
            pl = pl.normalized();
            pl.origin = pl.origin + pl.normal * ev.num(p, "offset", 0.0);
        } else if (has(p, "path")) {
            // Plane Along Path: perpendicular to the path at `position` (fraction of its length).
            // The path only guides the plane, so its sketch stays visible (pathEdgesOf would hide it).
            const auto pathSketch = m_sketches.find(required(p.at("path"), "sketch").get<FeatureId>());
            const bool wasConsumed = pathSketch != m_sketches.end() && pathSketch->second.consumed;
            const geometry::Shape pathEdges = pathEdgesOf(p.at("path"), "plane");
            if (pathSketch != m_sketches.end())
                pathSketch->second.consumed = wasConsumed;
            const double position = ev.num(p, "position", 0.0);
            if (!(position >= 0.0 && position <= 1.0))
                throw DesignError("The position along the path must be from 0 (start) to 1 (end)");
            geometry::PathFrame frame;
            try {
                frame = geometry::pathFrames(pathEdges, {position * geometry::pathLength(pathEdges)}).front();
            } catch (const geometry::ModelingError& e) {
                throw DesignError(e.what());
            }
            pl.origin = frame.point;
            pl.normal = frame.tangent;
            pl.xDir = std::abs(frame.tangent.z) < 0.9 ? core::Vec3{0, 0, 1}.cross(frame.tangent) : core::Vec3{1, 0, 0};
            pl = pl.normalized();
            pl.origin = pl.origin + pl.normal * ev.num(p, "offset", 0.0);
        } else if (has(p, "points")) {
            const Json& pts = p.at("points");
            if (!pts.is_array() || pts.size() != 3)
                throw DesignError("A plane through points needs exactly 3 points");
            const core::Vec3 a = ev.vec3(pts[0]);
            const core::Vec3 b = ev.vec3(pts[1]);
            const core::Vec3 c = ev.vec3(pts[2]);
            const core::Vec3 n = (b - a).cross(c - a);
            if (n.length() < 1e-9 * std::max(1.0, (b - a).length() * (c - a).length()))
                throw DesignError("The 3 points of the plane lie on one line");
            pl.origin = a;
            pl.normal = n;
            pl.xDir = b - a;
            pl = pl.normalized();
        } else {
            pl = resolvePlane(p.value("base", Json("XY"))).normalized();
            if (has(p, "base2")) {
                // Midplane between two parallel planes or faces.
                const Plane other = resolvePlane(p.at("base2")).normalized();
                if (std::abs(std::abs(pl.normal.dot(other.normal)) - 1.0) > 1e-6)
                    throw DesignError("A midplane needs two parallel planes");
                pl.origin = pl.origin + pl.normal * (pl.normal.dot(other.origin - pl.origin) * 0.5);
            }
            const double angle = ev.num(p, "angle", 0.0);
            {
                // Plane at angle: turned about the base plane's x or y axis through its origin, or
                // (`about`) about any line: a world axis, an edge or a construction axis. The line
                // is checked even when the angle is 0, so a bad one does not wait for a later edit.
                core::Vec3 pivot = pl.origin;
                core::Vec3 k{};
                if (has(p, "about")) {
                    if (has(p, "axis"))
                        throw DesignError("Give the rotation line as either `axis` (of the base plane) or `about`, not both");
                    const Json& a = p.at("about");
                    core::Vec3 dir{0, 0, 1};
                    if (a.is_string()) {
                        const std::string name = lower(a.get<std::string>());
                        if (name == "x")
                            dir = {1, 0, 0};
                        else if (name == "y")
                            dir = {0, 1, 0};
                        else if (name != "z")
                            throw DesignError("Unknown rotation line '" + name + "' (X, Y, Z, {origin, direction} or {feature})");
                        pivot = {};
                    } else if (has(a, "feature")) {
                        axisFeature(a, pivot, dir);
                    } else if (has(a, "direction")) {
                        pivot = ev.vec3(a, "origin", {});
                        dir = ev.vec3(a, "direction", {0, 0, 1});
                    } else {
                        throw DesignError("`about` must be X, Y, Z, {origin, direction} or {feature}");
                    }
                    if (dir.length() < 1e-9)
                        throw DesignError("The rotation line of the plane has no direction");
                    k = dir.normalized();
                } else {
                    const std::string axis = lower(p.value("axis", std::string("x")));
                    if (axis != "x" && axis != "y")
                        throw DesignError("Plane axis must be \"x\" or \"y\" (of the base plane)");
                    k = axis == "x" ? pl.xDir : pl.yDir();
                }
                if (angle != 0.0) {
                    const double t = angle * std::numbers::pi / 180.0;
                    auto rotate = [&](const core::Vec3& v) {
                        return v * std::cos(t) + k.cross(v) * std::sin(t) + k * (k.dot(v) * (1.0 - std::cos(t)));
                    };
                    pl.origin = pivot + rotate(pl.origin - pivot);
                    pl.normal = rotate(pl.normal);
                    pl.xDir = rotate(pl.xDir);
                    pl = pl.normalized();
                }
            }
            pl.origin = pl.origin + pl.normal * ev.num(p, "offset", 0.0);
        }
        // Drawn a bit larger than the bodies built so far, like Fusion.
        core::BoundingBox box;
        for (const auto& [_, b] : m_bodies) {
            const core::BoundingBox bb = b.shape.bounds();
            if (!bb.isEmpty()) {
                box.extend(bb.min);
                box.extend(bb.max);
            }
        }
        double size = 25.0;
        if (!box.isEmpty())
            size = std::max(size, 0.6 * (box.max - box.min).length());
        m_planes[f.id] = {f.id, f.name, pl, size};
    } else if (f.type == "axis") {
        // Fusion: Construct > Axis Through Two Points, Through Edge, Perpendicular to Face,
        // Through Cylinder/Cone/Torus Face, At Intersection of Two Planes.
        const bool twoPoints = has(p, "from") || has(p, "to");
        const int modes = int(twoPoints) + int(has(p, "line")) + int(has(p, "edge")) + int(has(p, "face")) + int(has(p, "plane"));
        if (modes != 1)
            throw DesignError("Give exactly one of from + to, line, edge, face or plane (+ plane2)");
        if (has(p, "plane2") && !has(p, "plane"))
            throw DesignError("plane2 goes with plane");
        if (has(p, "plane2") && has(p, "point"))
            throw DesignError("`point` goes with a single plane, not with plane2");
        core::Vec3 origin{};
        core::Vec3 dir{0, 0, 1};
        // The body and the topology index of an `edge` / `face` {body, index | point}.
        auto topologyRef = [&](const Json& ref, bool edge, const Body*& body) {
            if (!ref.is_object() || !has(ref, "body"))
                throw DesignError(std::string("`") + (edge ? "edge" : "face") + "` must be {body, index or point}");
            body = &bodyRef(ref.at("body"));
            if (has(ref, "index"))
                return ref.at("index").get<std::size_t>();
            const auto idx = matchTopology(body->shape, ref, edge, ev);
            if (!idx)
                throw DesignError(std::string("No ") + (edge ? "edge" : "face") + " near " + ref.dump());
            return *idx;
        };
        if (twoPoints) {
            if (!has(p, "from") || !has(p, "to"))
                throw DesignError("An axis through two points needs both `from` and `to`");
            origin = ev.vec3(p.at("from"));
            dir = ev.vec3(p.at("to")) - origin;
            if (dir.length() < 1e-9)
                throw DesignError("The 2 points of the axis are the same");
        } else if (has(p, "line")) {
            // A world axis ("X", "Y", "Z"), or a line {origin, direction} (what clicking an edge gives).
            const Json& line = p.at("line");
            if (line.is_object() && has(line, "feature")) {
                axisFeature(line, origin, dir); // a copy of another construction axis
            } else if (line.is_string()) {
                const std::string a = lower(line.get<std::string>());
                if (a != "x" && a != "y" && a != "z")
                    throw DesignError("Unknown axis '" + line.get<std::string>() + "' (X, Y, Z or {origin, direction})");
                dir = a == "x" ? core::Vec3{1, 0, 0} : a == "y" ? core::Vec3{0, 1, 0} : core::Vec3{0, 0, 1};
            } else {
                origin = ev.vec3(line, "origin", {});
                dir = ev.vec3(line, "direction", {0, 0, 1});
            }
            if (dir.length() < 1e-9)
                throw DesignError("The axis direction is zero");
        } else if (has(p, "edge")) {
            const Body* body = nullptr;
            const std::size_t idx = topologyRef(p.at("edge"), true, body);
            const auto infos = geometry::edges(body->shape);
            if (idx >= infos.size())
                throw DesignError("No edge " + std::to_string(idx) + " on " + body->name);
            const auto& e = infos[idx];
            if (!e.hasAxis)
                throw DesignError("Edge " + std::to_string(idx) + " of " + body->name + " is a " + e.type +
                                  ": only straight and circular edges have an axis");
            origin = e.axisOrigin;
            dir = e.axisDirection;
        } else if (has(p, "face")) {
            const Body* body = nullptr;
            const std::size_t idx = topologyRef(p.at("face"), false, body);
            const auto infos = geometry::faces(body->shape);
            if (idx >= infos.size())
                throw DesignError("No face " + std::to_string(idx) + " on " + body->name);
            const auto& fc = infos[idx];
            if (!fc.hasAxis)
                throw DesignError("Face " + std::to_string(idx) + " of " + body->name + " is a " + fc.type +
                                  ": use a cylinder, cone or torus face");
            origin = fc.axisOrigin;
            dir = fc.axisDirection;
        } else if (has(p, "plane2")) {
            const Plane a = resolvePlane(p.at("plane")).normalized();
            const Plane b = resolvePlane(p.at("plane2")).normalized();
            const core::Vec3 cross = a.normal.cross(b.normal);
            if (cross.length() < 1e-9)
                throw DesignError("The 2 planes are parallel: they have no line in common");
            dir = cross;
            // A point of the line: n1 d1 + n2 d2 combined with the cross product.
            const double d1 = a.normal.dot(a.origin);
            const double d2 = b.normal.dot(b.origin);
            origin = (b.normal.cross(cross) * d1 + cross.cross(a.normal) * d2) * (1.0 / cross.dot(cross));
        } else {
            // Perpendicular to a plane or planar face, through `point` (default the plane's origin).
            const Plane pl = resolvePlane(p.at("plane")).normalized();
            dir = pl.normal;
            origin = has(p, "point") ? ev.vec3(p.at("point")) : pl.origin;
        }
        // Drawn a bit longer than the bodies built so far, like a construction plane.
        core::BoundingBox box;
        for (const auto& [_, b] : m_bodies) {
            const core::BoundingBox bb = b.shape.bounds();
            if (!bb.isEmpty()) {
                box.extend(bb.min);
                box.extend(bb.max);
            }
        }
        double size = 25.0;
        if (!box.isEmpty())
            size = std::max(size, 0.6 * (box.max - box.min).length());
        m_axes[f.id] = {f.id, f.name, origin, dir.normalized(), size};
    } else if (auto custom = m_evaluators.find(f.type); custom != m_evaluators.end()) {
        evaluateExternal(f, custom->second);
    } else {
        throw DesignError("Unknown feature type '" + f.type + "'");
    }
}

void Design::setFeatureEvaluator(const std::string& type, FeatureEvaluator evaluator)
{
    m_evaluators[type] = std::move(evaluator);
    m_evaluated.clear();
}

void Design::evaluateExternal(const Feature& f, const FeatureEvaluator& ev)
{
    // The sketch it is built from: used (hidden like Fusion), and part of the cache key.
    std::string key = f.type + "\n" + f.params.dump() + "\n";
    for (const auto& prm : m_state.parameters) {
        // A parameter that cannot be evaluated (one it uses was removed) only fails the
        // features that use it, not every step on a scan.
        char value[32] = "?";
        try {
            if (const auto v = parameterValue(prm.name))
                std::snprintf(value, sizeof value, "%.17g", *v); // round-trip precision
        } catch (const std::exception&) {
            std::snprintf(value, sizeof value, "invalid");
        }
        key += prm.name + "=" + value + ";";
    }
    if (f.params.contains("sketch") && f.params.at("sketch").is_number_integer()) {
        const auto sid = f.params.at("sketch").get<FeatureId>();
        auto it = m_sketches.find(sid);
        if (it == m_sketches.end())
            throw DesignError("No sketch " + std::to_string(sid) + " at this point of the timeline");
        it->second.consumed = true;
        const auto& pl = it->second.plane;
        char buf[256];
        std::snprintf(buf, sizeof buf, "\n%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n", pl.origin.x,
                      pl.origin.y, pl.origin.z, pl.normal.x, pl.normal.y, pl.normal.z, pl.xDir.x, pl.xDir.y, pl.xDir.z);
        key += buf;
        for (const auto& pt : geometry::edgeSegments(it->second.geometry.curves)) {
            std::snprintf(buf, sizeof buf, "%.12g,%.12g,%.12g;", pt.x, pt.y, pt.z);
            key += buf;
        }
        // The profiles too: turning an outline into construction geometry keeps its edges
        // but removes its profile.
        for (const auto& profile : it->second.geometry.profiles) {
            key += "\nprofile:";
            for (const auto& pt : geometry::edgeSegments(profile)) {
                std::snprintf(buf, sizeof buf, "%.12g,%.12g,%.12g;", pt.x, pt.y, pt.z);
                key += buf;
            }
        }
    }
    if (ev.inputs)
        key += "\n" + ev.inputs(f);
    // Stored in project files: a short digest (FNV-1a, 64 bit, and the length) instead of
    // the whole description.
    {
        std::uint64_t h = 1469598103934665603ull;
        for (const unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ull;
        }
        char digest[48];
        std::snprintf(digest, sizeof digest, "%016llx-%zu", static_cast<unsigned long long>(h), key.size());
        key = digest;
    }

    auto& cache = m_evaluated[f.id];
    const FeatureEvaluator::Output* out = nullptr;
    for (const auto& e : cache)
        if (e.key == key)
            out = &e.output;
    if (out == nullptr) {
        ++m_evaluatorRuns;
        FeatureEvaluator::Output fresh = ev.evaluate(f, *this);
        if (fresh.bodies.empty())
            throw DesignError("The feature produced no solid");
        constexpr std::size_t keep = 4;
        if (cache.size() >= keep)
            cache.erase(cache.begin());
        cache.push_back({key, std::move(fresh)});
        out = &cache.back().output;
    }
    for (std::size_t k = 0; k < out->bodies.size(); ++k) {
        const auto& [shape, name] = out->bodies[k];
        if (shape.isNull())
            continue;
        const unsigned slot = k < out->bodySlots.size() ? out->bodySlots[k] : static_cast<unsigned>(k);
        const FeatureId id = slot == 0 ? f.id : extraBodyId(f.id, slot);
        m_bodies[id] = Body{id, name, shape};
    }
    m_reports[f.id] = out->report;
    m_usedKeys[f.id] = key;
}

std::vector<Design::StoredResult> Design::currentResults() const
{
    std::vector<StoredResult> out;
    for (const auto& [id, key] : m_usedKeys) {
        auto it = m_evaluated.find(id);
        if (it == m_evaluated.end())
            continue;
        for (const auto& e : it->second)
            if (e.key == key)
                out.push_back({id, e.key, e.output});
    }
    return out;
}

void Design::seedResult(StoredResult result)
{
    auto& cache = m_evaluated[result.feature];
    cache.push_back({std::move(result.key), std::move(result.output)});
}

void Design::setFrame(ReferenceFrame fr)
{
    const std::string key = lower(fr.name);
    if (fr.name.empty() || key == "xy" || key == "xz" || key == "yz")
        throw DesignError("Invalid frame name '" + fr.name + "'");
    const core::Vec3 n = fr.normal.normalized();
    const core::Vec3 x = (fr.xDir - n * n.dot(fr.xDir)).normalized();
    if (n.length() < 0.5 || x.length() < 0.5)
        throw DesignError("Frame '" + fr.name + "': normal and xDir must be non-zero and not parallel");
    fr.normal = n;
    fr.xDir = x;
    auto it = std::find_if(m_state.frames.begin(), m_state.frames.end(),
                           [&](const ReferenceFrame& f) { return f.name == fr.name; });
    if (it == m_state.frames.end())
        m_state.frames.push_back(std::move(fr));
    else
        *it = std::move(fr);
    regenerate();
}

void Design::removeFrame(const std::string& name)
{
    const auto n = std::erase_if(m_state.frames, [&](const ReferenceFrame& f) { return f.name == name; });
    if (n == 0)
        throw DesignError("No frame '" + name + "'");
    regenerate();
}

const ReferenceFrame* Design::frame(std::string_view name) const
{
    auto it = std::find_if(m_state.frames.begin(), m_state.frames.end(),
                           [&](const ReferenceFrame& f) { return f.name == name; });
    return it == m_state.frames.end() ? nullptr : &*it;
}

void Design::clear()
{
    m_state = State{};
    m_partIds.clear();
    m_sketchConsumed.clear();
    m_partMeta.clear();
    m_convertedParts.clear();
    m_scanSurfaces.clear();
    m_evaluated.clear();
    regenerate();
}

void Design::setPartInfo(FeatureId feature, model::PartId part, std::string name, bool visible,
                         std::string color)
{
    m_partIds[feature] = part;
    m_partMeta[feature] = {std::move(name), visible, std::move(color)};
}

std::vector<Design::PartInfo> Design::partInfos() const
{
    std::vector<PartInfo> out;
    for (const auto& [feature, part] : m_partIds) {
        PartInfo info{feature, part, {}, true, {}};
        if (auto meta = m_partMeta.find(feature); meta != m_partMeta.end()) {
            info.name = meta->second.name;
            info.visible = meta->second.visible;
            info.color = meta->second.color;
        }
        out.push_back(std::move(info));
    }
    return out;
}

void Design::regenerate()
{
    m_bodies.clear();
    m_sketches.clear();
    m_planes.clear();
    m_axes.clear();
    m_statuses.clear();
    m_reports.clear();
    m_usedKeys.clear();
    m_bodyCounter = 0;
    for (std::size_t i = 0; i < m_state.features.size(); ++i) {
        const Feature& f = m_state.features[i];
        FeatureStatus st;
        if (i >= m_state.marker) {
            st.state = FeatureStatus::State::RolledBack;
        } else if (f.suppressed) {
            st.state = FeatureStatus::State::Suppressed;
        } else {
            try {
                evaluateFeature(f);
                if (auto r = m_reports.find(f.id); r != m_reports.end())
                    st.report = r->second;
            } catch (const std::exception& e) {
                st.state = FeatureStatus::State::Error;
                st.message = e.what();
            }
        }
        m_statuses[f.id] = st;
    }
}

// ---------------------------------------------------------------------------------------
// Document mirror

namespace {

/// Steepest rise over run a draped line or fill follows; steeper is a hole, a gap or the edge
/// of the skin, not the surface under the sketch.
constexpr double maxDrapeSlope = 3.0;

/// A sketch drawn over a curved surface (params onSurface) is shown lying on it: its
/// line segments are split finely and moved along the sketch normal onto the surface
/// part's mesh. Where there is no skin to lie on (a hole, past the edge), a line runs
/// straight between the nearest points that have one.
std::vector<core::Vec3> drapeSegments(const std::vector<core::Vec3>& segments, const core::TriangleMesh& surface,
                                      const core::Vec3& normal)
{
    core::BoundingBox box;
    for (const auto& p : segments)
        box.extend(p);
    const double step = std::max(box.diagonal() / 300.0, 1e-3);
    const core::Vec3 n = normal.normalized();
    // A hair above the surface, so the lines are not hidden in it.
    const double lift = std::max(box.diagonal() * 1.5e-3, 1e-3);

    std::vector<core::Vec3> fine;
    std::vector<std::size_t> starts; // of each input segment in `fine`
    for (std::size_t i = 0; i + 1 < segments.size(); i += 2) {
        const core::Vec3 a = segments[i];
        const core::Vec3 b = segments[i + 1];
        const int pieces = std::clamp(static_cast<int>(std::ceil((b - a).length() / step)), 1, 400);
        starts.push_back(fine.size());
        for (int k = 0; k <= pieces; ++k)
            fine.push_back(a + (b - a) * (static_cast<double>(k) / pieces));
    }
    starts.push_back(fine.size());
    std::vector<char> missed;
    const auto draped = core::projectOntoMesh(surface, fine, n, nullptr, &missed);

    std::vector<double> height(fine.size(), 0.0);
    for (std::size_t k = 0; k < fine.size(); ++k)
        height[k] = (draped[k] - fine[k]).dot(n);
    std::vector<core::Vec3> out;
    out.reserve(fine.size() * 2);
    for (std::size_t s = 0; s + 1 < starts.size(); ++s) {
        const std::size_t begin = starts[s], end = starts[s + 1];
        // From the start of the segment on, a point counts when it continues the skin.
        std::size_t anchor = end;
        for (std::size_t k = begin; k < end; ++k) {
            if (missed[k] != 0)
                continue;
            if (anchor != end) {
                const double run = (fine[k] - fine[anchor]).length();
                if (std::abs(height[k] - height[anchor]) > maxDrapeSlope * run + 1e-6) {
                    missed[k] = 1;
                    continue;
                }
            }
            anchor = k;
        }
        // Straight across the gaps; a segment with no skin at all stays on the plane.
        std::size_t last = end;
        for (std::size_t k = begin; k < end; ++k) {
            if (missed[k] == 0) {
                if (last != end)
                    for (std::size_t g = last + 1; g < k; ++g)
                        height[g] = height[last] +
                                    (height[k] - height[last]) * static_cast<double>(g - last) / static_cast<double>(k - last);
                else
                    for (std::size_t g = begin; g < k; ++g)
                        height[g] = height[k];
                last = k;
            }
        }
        if (last != end) {
            for (std::size_t g = last + 1; g < end; ++g)
                height[g] = height[last];
        } else {
            for (std::size_t g = begin; g < end; ++g)
                height[g] = 0.0;
        }
        for (std::size_t k = begin; k + 1 < end; ++k) {
            out.push_back(fine[k] + n * (height[k] + lift));
            out.push_back(fine[k + 1] + n * (height[k + 1] + lift));
        }
    }
    return out;
}

/// The sketch's filled profiles, split into small triangles and laid onto the surface
/// like the lines (a flat fill would hover above a curved surface).
core::TriangleMesh drapeFill(const core::TriangleMesh& fill, const core::TriangleMesh& surface, const core::Vec3& normal)
{
    if (fill.triangleCount() == 0)
        return fill;
    const double step = std::max(fill.bounds().diagonal() / 60.0, 1e-3);
    std::vector<core::Vec3> v = fill.vertices();
    std::vector<core::TriangleMesh::Triangle> tris = fill.triangles();
    // 1-to-4 splits while any edge is long (edge midpoints shared through a map).
    for (int level = 0; level < 8 && tris.size() < 60000; ++level) {
        bool longEdge = false;
        for (const auto& t : tris)
            for (int k = 0; k < 3 && !longEdge; ++k)
                longEdge = (v[t[k]] - v[t[(k + 1) % 3]]).length() > step;
        if (!longEdge)
            break;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            auto [it, added] = mid.try_emplace({key.first, key.second}, static_cast<std::uint32_t>(v.size()));
            if (added)
                v.push_back((v[a] + v[b]) * 0.5);
            return it->second;
        };
        std::vector<core::TriangleMesh::Triangle> next;
        next.reserve(tris.size() * 4);
        for (const auto& t : tris) {
            const auto ab = midpoint(t[0], t[1]);
            const auto bc = midpoint(t[1], t[2]);
            const auto ca = midpoint(t[2], t[0]);
            next.push_back({t[0], ab, ca});
            next.push_back({ab, t[1], bc});
            next.push_back({ca, bc, t[2]});
            next.push_back({ab, bc, ca});
        }
        tris = std::move(next);
    }
    const core::Vec3 n = normal.normalized();
    std::vector<char> missed;
    auto draped = core::projectOntoMesh(surface, v, n, nullptr, &missed);
    // Only what lies on the skin: nothing over holes, past the edge, or down a wall to
    // whatever is behind the skin.
    std::vector<core::TriangleMesh::Triangle> kept;
    kept.reserve(tris.size());
    for (const auto& t : tris) {
        bool ok = missed[t[0]] == 0 && missed[t[1]] == 0 && missed[t[2]] == 0;
        for (int k = 0; k < 3 && ok; ++k) {
            const auto a = t[k], b = t[(k + 1) % 3];
            const double rise = std::abs((draped[a] - v[a]).dot(n) - (draped[b] - v[b]).dot(n));
            ok = rise <= maxDrapeSlope * (v[a] - v[b]).length();
        }
        if (ok)
            kept.push_back(t);
    }
    const core::Vec3 lift = n * std::max(fill.bounds().diagonal() * 1e-3, 5e-4);
    for (auto& p : draped)
        p = p + lift;
    return core::TriangleMesh(std::move(draped), std::move(kept));
}

} // namespace

void Design::syncDocument(model::Document& document)
{
    struct Wanted {
        FeatureId id;
        model::PartKind kind;
        std::string name;
        std::optional<geometry::Shape> shape;
        std::shared_ptr<const core::TriangleMesh> mesh;
        std::vector<core::Vec3> segments;
        bool consumed = false;
    };
    std::map<FeatureId, Wanted> wanted;
    for (const auto& [id, b] : m_bodies) {
        wanted[id] = {id,
                      model::PartKind::Body,
                      b.name,
                      b.shape,
                      std::make_shared<const core::TriangleMesh>(geometry::tessellate(b.shape)),
                      geometry::edgeSegments(b.shape),
                      false};
    }
    for (const auto& [id, s] : m_sketches) {
        const geometry::Shape profiles = geometry::makeCompound(s.geometry.profiles);
        auto segments = sketchSegments(s);
        auto fill = geometry::tessellate(profiles);
        if (const Feature* f = feature(id); f != nullptr && f->params.contains("onSurface") && !segments.empty()) {
            const Json& on = f->params.at("onSurface");
            const model::Part* surface =
                on.contains("part") ? document.part(on.at("part").get<model::PartId>()) : nullptr;
            // Lay it on the scan the surface was fitted to when there is one: the outline
            // may reach past the fitted area, and the scan is what is seen.
            if (surface != nullptr) {
                if (const model::Part* scan = document.part(model::fittedScanId(*surface));
                    scan != nullptr && scan->mesh && !scan->shape)
                    surface = scan;
            }
            if (!on.value("across", false) && surface != nullptr && surface->mesh) {
                segments = drapeSegments(segments, *surface->mesh, s.plane.normal);
                fill = drapeFill(fill, *surface->mesh, s.plane.normal);
            }
        }
        wanted[id] = {id,
                      model::PartKind::Sketch,
                      s.name,
                      std::nullopt,
                      std::make_shared<const core::TriangleMesh>(std::move(fill)),
                      std::move(segments),
                      s.consumed};
    }
    for (const auto& [id, cp] : m_planes) {
        const geometry::Plane& pl = cp.plane;
        const double h = cp.size;
        const core::Vec3 u = pl.xDir * h;
        const core::Vec3 v = pl.yDir() * h;
        const std::array<core::Vec3, 4> c{pl.origin - u - v, pl.origin + u - v, pl.origin + u + v, pl.origin - u + v};
        auto mesh = std::make_shared<core::TriangleMesh>();
        for (const auto& corner : c)
            mesh->addVertex(corner);
        mesh->addTriangle(0, 1, 2);
        mesh->addTriangle(0, 2, 3);
        std::vector<core::Vec3> outline;
        for (std::size_t i = 0; i < 4; ++i) {
            outline.push_back(c[i]);
            outline.push_back(c[(i + 1) % 4]);
        }
        wanted[id] = {id, model::PartKind::Plane, cp.name, std::nullopt, std::move(mesh), std::move(outline), false};
    }
    for (const auto& [id, ca] : m_axes) {
        // A thin four-sided bar along the axis: something to see and to click.
        const core::Vec3 d = ca.direction.normalized();
        const core::Vec3 helper = std::abs(d.z) < 0.9 ? core::Vec3{0, 0, 1} : core::Vec3{1, 0, 0};
        const core::Vec3 u = d.cross(helper).normalized();
        const core::Vec3 v = d.cross(u).normalized();
        const double r = std::max(0.4, ca.size * 0.02);
        const core::Vec3 a = ca.origin - d * ca.size;
        const core::Vec3 b = ca.origin + d * ca.size;
        const std::array<core::Vec3, 4> ring{u * r, v * r, u * -r, v * -r};
        auto mesh = std::make_shared<core::TriangleMesh>();
        for (const auto& c : ring)
            mesh->addVertex(a + c);
        for (const auto& c : ring)
            mesh->addVertex(b + c);
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint32_t j = (i + 1) % 4;
            mesh->addTriangle(i, j, 4 + j);
            mesh->addTriangle(i, 4 + j, 4 + i);
        }
        mesh->addTriangle(0, 2, 1);
        mesh->addTriangle(0, 3, 2);
        mesh->addTriangle(4, 5, 6);
        mesh->addTriangle(4, 6, 7);
        wanted[id] = {id, model::PartKind::Axis, ca.name, std::nullopt, std::move(mesh), std::vector<core::Vec3>{a, b}, false};
    }

    // Remember user-controlled settings of every timeline part, so a part that disappears
    // (suppress, roll back, delete) comes back with them (unsuppress, undo, ...).
    for (const auto& part : document.parts()) {
        if (part.kind != model::PartKind::Imported)
            m_partMeta[part.featureId] = {part.name, part.visible, part.color};
    }

    // Remove parts whose feature output disappeared.
    std::vector<model::PartId> stale;
    for (const auto& part : document.parts()) {
        if (part.kind == model::PartKind::Imported)
            continue;
        auto it = wanted.find(part.featureId);
        if (it == wanted.end() || it->second.kind != part.kind)
            stale.push_back(part.id);
    }
    for (const auto id : stale)
        document.removePart(id);

    for (auto& [fid, w] : wanted) {
        // Preserve the provenance contract used by deviation inspection and sketch
        // draping. The dependency itself lives in the feature's saved params.
        std::string source;
        if (const Feature* f = feature(featureOfBody(fid)); f != nullptr && f->params.contains("id")) {
            std::string operation;
            if (f->type == "fit_selection" || f->type == "fit_regions" || f->type == "trim_by_sketch")
                operation = f->type;
            else if (f->type == "offset_surface")
                operation = "offset";
            else if (f->type == "thicken_surface")
                operation = "thicken";
            if (!operation.empty())
                source = "surface." + operation + " of part " + std::to_string(f->params.at("id").get<model::PartId>());
        }
        const model::Part* existing = nullptr;
        for (const auto& part : document.parts()) {
            if (part.kind == w.kind && part.featureId == fid) {
                existing = &part;
                break;
            }
        }
        const bool wasConsumed = m_sketchConsumed[fid];
        if (existing != nullptr) {
            document.modifyPart(existing->id, [&](model::Part& part) {
                // A region selection holds triangle indices: keep it only while the
                // regenerated tessellation has the same topology.
                if (part.selection && !(part.mesh && w.mesh && part.mesh->triangles() == w.mesh->triangles()))
                    part.selection.reset();
                part.mesh = w.mesh;
                part.shape = w.shape;
                part.source = source;
                part.edgeSegments = w.segments;
                if (w.kind == model::PartKind::Sketch && w.consumed && !wasConsumed)
                    part.visible = false; // Fusion hides a sketch once a feature uses it
                else if (w.kind == model::PartKind::Sketch && !w.consumed && wasConsumed)
                    part.visible = true; // ... and shows it again when that feature goes away
            });
        } else {
            model::Part part;
            auto known = m_partIds.find(fid);
            part.id = known == m_partIds.end() ? 0 : known->second;
            part.name = w.name;
            part.kind = w.kind;
            part.featureId = fid;
            part.mesh = w.mesh;
            part.shape = w.shape;
            part.source = source;
            part.edgeSegments = std::move(w.segments);
            part.visible = !(w.kind == model::PartKind::Sketch && w.consumed);
            if (auto meta = m_partMeta.find(fid); meta != m_partMeta.end()) {
                part.name = meta->second.name;
                part.visible = meta->second.visible;
                part.color = meta->second.color;
            }
            m_partIds[fid] = document.addPart(std::move(part));
        }
        m_sketchConsumed[fid] = w.consumed;
    }
}

// ---------------------------------------------------------------------------------------
// Serialisation

namespace {

Json vec3ToJson(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

core::Vec3 vec3FromJson(const Json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        throw DesignError(std::string("Invalid vector '") + what + "'");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

template <typename T>
T field(const Json& j, const char* key, const char* what)
{
    auto it = j.find(key);
    if (it == j.end())
        throw DesignError(std::string("Missing '") + key + "' in " + what);
    try {
        return it->get<T>();
    } catch (const Json::exception&) {
        throw DesignError(std::string("Invalid '") + key + "' in " + what);
    }
}

} // namespace

Json stateToJson(const Design::State& live)
{
    const Design::State state = committedState(live);
    Json features = Json::array();
    for (const auto& f : state.features) {
        features.push_back(
            {{"id", f.id}, {"type", f.type}, {"name", f.name}, {"params", f.params}, {"suppressed", f.suppressed}});
    }
    Json parameters = Json::array();
    for (const auto& p : state.parameters)
        parameters.push_back({{"name", p.name}, {"expression", p.expression}, {"comment", p.comment}});
    Json frames = Json::array();
    for (const auto& f : state.frames) {
        frames.push_back({{"name", f.name},
                          {"origin", vec3ToJson(f.origin)},
                          {"normal", vec3ToJson(f.normal)},
                          {"xDir", vec3ToJson(f.xDir)},
                          {"comment", f.comment}});
    }
    return Json{{"features", features},
                {"parameters", parameters},
                {"frames", frames},
                {"nextId", state.nextId},
                {"marker", state.marker}};
}

Design::State stateFromJson(const Json& json)
{
    if (!json.is_object())
        throw DesignError("Design state must be an object");
    Design::State state;
    const auto& types = Design::featureTypes();
    std::set<FeatureId> ids;
    FeatureId maxId = 0;
    for (const auto& j : json.value("features", Json::array())) {
        Feature f;
        f.id = field<FeatureId>(j, "id", "feature");
        f.type = field<std::string>(j, "type", "feature");
        f.name = field<std::string>(j, "name", "feature");
        f.params = j.value("params", Json::object());
        f.suppressed = j.value("suppressed", false);
        if (f.id == 0 || !ids.insert(f.id).second)
            throw DesignError("Invalid or duplicate feature id " + std::to_string(f.id));
        if (std::find(types.begin(), types.end(), f.type) == types.end())
            throw DesignError("Unknown feature type '" + f.type + "'");
        if (!f.params.is_object())
            throw DesignError("Parameters of feature " + f.name + " must be an object");
        maxId = std::max(maxId, f.id);
        state.features.push_back(std::move(f));
    }
    for (const auto& j : json.value("parameters", Json::array())) {
        state.parameters.push_back({field<std::string>(j, "name", "parameter"),
                                    field<std::string>(j, "expression", "parameter"), j.value("comment", "")});
    }
    for (const auto& j : json.value("frames", Json::array())) {
        ReferenceFrame f;
        f.name = field<std::string>(j, "name", "frame");
        f.origin = vec3FromJson(j.value("origin", Json::array({0, 0, 0})), "origin");
        f.normal = vec3FromJson(j.value("normal", Json::array({0, 0, 1})), "normal");
        f.xDir = vec3FromJson(j.value("xDir", Json::array({1, 0, 0})), "xDir");
        f.comment = j.value("comment", "");
        state.frames.push_back(std::move(f));
    }
    state.nextId = std::max<FeatureId>(json.value("nextId", FeatureId{1}), maxId + 1);
    state.marker = std::min<std::size_t>(json.value("marker", state.features.size()), state.features.size());
    return state;
}

} // namespace aerocad::cad
