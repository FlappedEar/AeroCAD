// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/Design.hpp"

#include "aerocad/cad/Expression.hpp"
#include "aerocad/cad/SketchSolver.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <numbers>
#include <set>
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
                                                "shell",  "combine", "box",     "cylinder", "base_body", "hole"};
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

std::size_t Design::indexOf(FeatureId id) const
{
    for (std::size_t i = 0; i < m_state.features.size(); ++i) {
        if (m_state.features[i].id == id)
            return i;
    }
    throw DesignError("No feature with id " + std::to_string(id));
}

std::string Design::defaultName(const std::string& type) const
{
    std::string base = type;
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

void Design::removeFeature(FeatureId id)
{
    const std::size_t i = indexOf(id);
    m_state.features.erase(m_state.features.begin() + static_cast<std::ptrdiff_t>(i));
    if (i < m_state.marker)
        --m_state.marker;
    regenerate();
}

void Design::setMarker(std::size_t marker)
{
    if (marker > m_state.features.size())
        throw DesignError("Marker position " + std::to_string(marker) + " is past the end of the timeline (" +
                          std::to_string(m_state.features.size()) + " features)");
    m_state.marker = marker;
    regenerate();
}

FeatureStatus Design::status(FeatureId id) const
{
    auto it = m_statuses.find(id);
    return it == m_statuses.end() ? FeatureStatus{FeatureStatus::State::Error, "Unknown feature"} : it->second;
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
    else
        throw DesignError("Unknown constraint point '" + which + "' (start, end, center)");
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
    if (isDimension(c.type)) {
        c.value = ev.num(j, "value");
        if (c.type != SketchConstraint::Type::Angle && c.value < 0)
            throw DesignError("Dimension value must not be negative");
    }
    return c;
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
    } else if (type == "circle") {
        SketchCurve c{SketchCurve::Kind::Circle, {ev.vec2(item, "center")}};
        c.radius = has(item, "diameter") ? ev.num(item, "diameter") / 2 : ev.num(item, "radius");
        push(c);
    } else if (type == "arc") {
        push({SketchCurve::Kind::Arc, {ev.vec2(item, "start"), ev.vec2(item, "through"), ev.vec2(item, "end")}});
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
        const Vec2 c = ev.vec2(item, "center");
        const double r = ev.num(item, "radius");
        const int sides = static_cast<int>(std::lround(ev.num(item, "sides")));
        const double rot = ev.num(item, "angle", 0.0) * kDegToRad;
        if (sides < 3 || sides > 256)
            throw DesignError("Polygon needs 3 to 256 sides");
        auto at = [&](int i) {
            const double a = rot + 2 * std::numbers::pi * i / sides;
            return Vec2{c.x + r * std::cos(a), c.y + r * std::sin(a)};
        };
        for (int i = 0; i < sides; ++i)
            push(lineCurve(at(i), at(i + 1), construction));
        for (int i = 1; i < sides; ++i)
            implicit.push_back({CT::Equal, {first, first + static_cast<std::size_t>(i)}, {}});
    } else if (type == "slot") {
        // Center-to-center slot: two parallel lines joined by half circles.
        const Vec2 a = ev.vec2(item, "start");
        const Vec2 b = ev.vec2(item, "end");
        const double r = ev.num(item, "width") / 2;
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
    } else if (type == "spline") {
        SketchCurve c{SketchCurve::Kind::Spline, {}};
        for (const auto& p : required(item, "points"))
            c.points.push_back(ev.vec2(p));
        c.closed = item.value("closed", false);
        push(c);
    } else {
        throw DesignError("Unknown sketch curve type '" + type + "'");
    }
}

} // namespace

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

    auto unionAll = [](const std::vector<geometry::Shape>& solids) {
        geometry::Shape result = solids.front();
        for (std::size_t i = 1; i < solids.size(); ++i)
            result = geometry::boolean(result, solids[i], geometry::BooleanOp::Union);
        return result;
    };

    if (f.type == "sketch") {
        SketchResult sk;
        sk.id = f.id;
        sk.name = f.name;
        if (has(p, "face")) {
            const Json& face = p.at("face");
            const Body& b = bodyRef(required(face, "body"));
            std::size_t idx = 0;
            if (has(face, "index")) {
                idx = face.at("index").get<std::size_t>();
            } else {
                const auto found = geometry::nearestFace(b.shape, ev.vec3(required(face, "point")));
                if (!found)
                    throw DesignError("No face found on " + b.name);
                idx = *found;
            }
            const auto plane = geometry::facePlane(b.shape, idx);
            if (!plane)
                throw DesignError("Face " + std::to_string(idx) + " of " + b.name + " is not planar");
            sk.plane = *plane;
        } else {
            const Json plane = p.value("plane", Json("XY"));
            if (plane.is_string()) {
                const std::string name = lower(plane.get<std::string>());
                if (name == "xy")
                    sk.plane = Plane::xy();
                else if (name == "xz")
                    sk.plane = Plane::xz();
                else if (name == "yz")
                    sk.plane = Plane::yz();
                else if (const ReferenceFrame* fr = frame(plane.get<std::string>()))
                    sk.plane = Plane{fr->origin, fr->normal, fr->xDir};
                else
                    throw DesignError("Unknown plane '" + plane.get<std::string>() +
                                      "' (XY, XZ, YZ or the name of a reference frame)");
            } else {
                sk.plane.origin = ev.vec3(plane, "origin", {});
                sk.plane.normal = ev.vec3(plane, "normal", {0, 0, 1});
                sk.plane.xDir = ev.vec3(plane, "xDir", {1, 0, 0});
            }
        }
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
        const auto faces = profiles(sk);
        const std::string extent = lower(p.value("extent", std::string("one_side")));
        double d1 = ev.num(p, "distance");
        double d2 = 0.0;
        if (extent == "symmetric")
            d2 = d1;
        else if (extent == "two_sides")
            d2 = ev.num(p, "distance2");
        else if (extent != "one_side")
            throw DesignError("Unknown extent '" + extent + "' (one_side, symmetric, two_sides)");
        core::Vec3 dir = sk->plane.normal;
        if (p.value("flip", false))
            dir = dir * -1.0;
        std::vector<geometry::Shape> solids;
        for (const auto& face : faces)
            solids.push_back(geometry::extrude(face, dir, d1, d2));
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
        } else if (has(axis, "line")) {
            const auto idx = axis.at("line").get<std::size_t>();
            if (idx >= sk->curves.size() || sk->curves[idx].kind != SketchCurve::Kind::Line)
                throw DesignError("Curve " + std::to_string(idx) + " of " + sk->name + " is not a line");
            origin = sk->plane.toWorld(sk->curves[idx].points[0]);
            dir = sk->plane.toWorld(sk->curves[idx].points[1]) - origin;
        } else {
            origin = ev.vec3(axis, "origin", {});
            dir = ev.vec3(axis, "direction", {0, 0, 1});
        }
        const double angle = ev.num(p, "angle", 360.0);
        std::vector<geometry::Shape> solids;
        for (const auto& face : faces)
            solids.push_back(geometry::revolve(face, origin, dir, angle));
        applyOperation(unionAll(solids));
    } else if (f.type == "fillet" || f.type == "chamfer") {
        Body& b = bodyRef(required(p, "body"));
        const auto idx = resolveIndices(b.shape, required(p, "edges"), true);
        b.shape = f.type == "fillet" ? geometry::fillet(b.shape, idx, ev.num(p, "radius"))
                                     : geometry::chamfer(b.shape, idx, ev.num(p, "distance"));
    } else if (f.type == "shell") {
        Body& b = bodyRef(required(p, "body"));
        const auto idx = has(p, "faces") ? resolveIndices(b.shape, p.at("faces"), false) : std::vector<std::size_t>{};
        b.shape = geometry::shell(b.shape, idx, ev.num(p, "thickness"));
    } else if (f.type == "combine") {
        Body& target = bodyRef(required(p, "target"));
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
        std::vector<FeatureId> tools;
        for (const auto& ref : required(p, "tools")) {
            const Body& t = bodyRef(ref);
            if (t.id == target.id)
                throw DesignError("A body cannot be combined with itself");
            if (std::find(tools.begin(), tools.end(), t.id) == tools.end())
                tools.push_back(t.id);
        }
        if (tools.empty())
            throw DesignError("Select at least one tool body");
        geometry::Shape result = target.shape;
        for (const FeatureId t : tools)
            result = geometry::boolean(result, m_bodies.at(t).shape, bop);
        const FeatureId targetId = target.id;
        if (!keepTools) {
            for (const FeatureId t : tools)
                m_bodies.erase(t);
        }
        if (result.isNull() || result.volume() <= 1e-9)
            m_bodies.erase(targetId);
        else
            m_bodies.at(targetId).shape = result;
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
    } else if (f.type == "hole") {
        // Simple drilled hole (Fusion: Hole, H): a cylinder cut into the body, normal to
        // the planar face at `point`.
        Body& b = bodyRef(required(p, "body"));
        const core::Vec3 point = ev.vec3(required(p, "point"));
        const auto faceIdx = geometry::nearestFace(b.shape, point);
        const auto plane = faceIdx ? geometry::facePlane(b.shape, *faceIdx) : std::nullopt;
        if (!plane)
            throw DesignError("Holes need a planar face at the given point");
        const double r = ev.num(p, "diameter") / 2;
        if (r <= 0)
            throw DesignError("Hole diameter must be positive");
        const bool through = p.value("through", !has(p, "depth"));
        const double depth = through ? b.shape.bounds().diagonal() * 2 : ev.num(p, "depth");
        if (depth <= 0)
            throw DesignError("Hole depth must be positive");
        geometry::Plane base = *plane;
        base.origin = point - plane->normal * plane->normal.dot(point - plane->origin); // onto the face
        SketchCurve c{SketchCurve::Kind::Circle, {{0, 0}}};
        c.radius = r;
        const auto g = geometry::buildSketch(base, {c});
        // Start slightly outside the face so the cut is clean.
        const geometry::Shape tool = geometry::extrude(g.profiles.at(0), plane->normal * -1.0, depth, 1e-3 * depth + 1e-3);
        const geometry::Shape result = geometry::boolean(b.shape, tool, geometry::BooleanOp::Cut);
        if (result.isNull())
            throw DesignError("The hole removed the whole body");
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
            if (cached != m_convertedParts.end() && cached->second.revision == part->revision) {
                shape = cached->second.shape;
            } else {
                shape = geometry::meshToBrep(*part->mesh);
                m_convertedParts[partId] = {part->revision, shape};
            }
        } else {
            throw DesignError("Part '" + part->name + "' has no geometry");
        }
        newBody(shape);
        m_bodies[f.id].name = part->name;
    } else {
        throw DesignError("Unknown feature type '" + f.type + "'");
    }
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
    regenerate();
}

void Design::setPartInfo(FeatureId feature, model::PartId part, std::string name, bool visible)
{
    m_partIds[feature] = part;
    m_partMeta[feature] = {std::move(name), visible};
}

std::vector<Design::PartInfo> Design::partInfos() const
{
    std::vector<PartInfo> out;
    for (const auto& [feature, part] : m_partIds) {
        PartInfo info{feature, part, {}, true};
        if (auto meta = m_partMeta.find(feature); meta != m_partMeta.end()) {
            info.name = meta->second.name;
            info.visible = meta->second.visible;
        }
        out.push_back(std::move(info));
    }
    return out;
}

void Design::regenerate()
{
    m_bodies.clear();
    m_sketches.clear();
    m_statuses.clear();
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
        wanted[id] = {id,
                      model::PartKind::Sketch,
                      s.name,
                      std::nullopt,
                      std::make_shared<const core::TriangleMesh>(geometry::tessellate(profiles)),
                      geometry::edgeSegments(s.geometry.curves),
                      s.consumed};
    }

    // Remember user-controlled settings of every timeline part, so a part that disappears
    // (suppress, roll back, delete) comes back with them (unsuppress, undo, ...).
    for (const auto& part : document.parts()) {
        if (part.kind != model::PartKind::Imported)
            m_partMeta[part.featureId] = {part.name, part.visible};
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
            part.edgeSegments = std::move(w.segments);
            part.visible = !(w.kind == model::PartKind::Sketch && w.consumed);
            if (auto meta = m_partMeta.find(fid); meta != m_partMeta.end()) {
                part.name = meta->second.name;
                part.visible = meta->second.visible;
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

Json stateToJson(const Design::State& state)
{
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
