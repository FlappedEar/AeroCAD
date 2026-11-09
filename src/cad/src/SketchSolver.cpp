// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/SketchSolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace aerocad::cad {

using geometry::ModelingError;
using geometry::SketchCurve;
using geometry::Vec2;
using Kind = SketchCurve::Kind;
using Type = SketchConstraint::Type;

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
/// Curve ends closer than this are merged into one point.
constexpr double kMergeTolerance = 1e-6;
/// A constraint counts as satisfied when its residual is below this (mm or rad).
constexpr double kTolerance = 1e-7;

struct NameEntry {
    Type type;
    std::string_view name;
};

constexpr std::array kNames{
    NameEntry{Type::Coincident, "coincident"},
    NameEntry{Type::PointOnCurve, "point_on_curve"},
    NameEntry{Type::Midpoint, "midpoint"},
    NameEntry{Type::Horizontal, "horizontal"},
    NameEntry{Type::Vertical, "vertical"},
    NameEntry{Type::Parallel, "parallel"},
    NameEntry{Type::Perpendicular, "perpendicular"},
    NameEntry{Type::Collinear, "collinear"},
    NameEntry{Type::Tangent, "tangent"},
    NameEntry{Type::Equal, "equal"},
    NameEntry{Type::Concentric, "concentric"},
    NameEntry{Type::Symmetric, "symmetric"},
    NameEntry{Type::Fix, "fix"},
    NameEntry{Type::Distance, "distance"},
    NameEntry{Type::HorizontalDistance, "horizontal_distance"},
    NameEntry{Type::VerticalDistance, "vertical_distance"},
    NameEntry{Type::Length, "length"},
    NameEntry{Type::Radius, "radius"},
    NameEntry{Type::Diameter, "diameter"},
    NameEntry{Type::Angle, "angle"},
};

double cross(Vec2 a, Vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

double dot(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}

double len(Vec2 a)
{
    return std::hypot(a.x, a.y);
}

double safe(double v)
{
    return std::max(v, 1e-12);
}

double signOf(double v)
{
    return v < 0 ? -1.0 : 1.0;
}

double wrapAngle(double a)
{
    const double pi = std::numbers::pi;
    while (a > pi)
        a -= 2 * pi;
    while (a <= -pi)
        a += 2 * pi;
    return a;
}

/// A point of the model: a pair of variables, or the fixed sketch origin.
struct P {
    std::size_t var = kNone; ///< x at var, y at var + 1; kNone = origin
    [[nodiscard]] Vec2 at(const std::vector<double>& x) const
    {
        return var == kNone ? Vec2{0, 0} : Vec2{x[var], x[var + 1]};
    }
};

/// Circle-like curve: centre plus a radius variable (circle) or start point (arc).
struct Circ {
    P center;
    P start;                  ///< arcs: radius = |start - center|
    std::size_t rvar = kNone; ///< circles
    [[nodiscard]] double radius(const std::vector<double>& x) const
    {
        return rvar != kNone ? x[rvar] : len(start.at(x) - center.at(x));
    }
};

struct Residual {
    std::vector<std::size_t> vars;
    std::size_t count = 1;
    std::function<void(const std::vector<double>&, double*)> eval;
    std::size_t constraint = kNone; ///< kNone for built-in residuals (arc radius)
};

struct CurveVars {
    std::vector<P> pts; ///< Line: start, end. Arc: start, end, center. Circle: center. Spline: points.
    std::size_t rvar = kNone;
    bool ccw = true; ///< arcs: direction from start through the middle to end
};

class Model {
public:
    Model(const std::vector<SketchCurve>& curves, const std::vector<SketchConstraint>& constraints) : m_curves(curves)
    {
        buildVariables();
        m_x0 = m_x;
        for (std::size_t i = 0; i < m_curves.size(); ++i) {
            if (m_curves[i].kind == Kind::Arc) {
                const Circ c = circ(i);
                const P e = m_vars[i].pts[1];
                add(kNone, varsOf({c.center, c.start, e}), 1, [c, e](const std::vector<double>& x, double* r) {
                    r[0] = len(e.at(x) - c.center.at(x)) - c.radius(x);
                });
            }
        }
        for (std::size_t i = 0; i < constraints.size(); ++i)
            addConstraint(i, constraints[i]);
        preAlign(constraints);
    }

    [[nodiscard]] std::size_t variableCount() const { return m_x.size(); }
    [[nodiscard]] std::size_t residualCount() const
    {
        std::size_t m = 0;
        for (const auto& r : m_residuals)
            m += r.count;
        return m;
    }

    void residuals(const std::vector<double>& x, std::vector<double>& out) const
    {
        out.assign(residualCount(), 0.0);
        std::size_t row = 0;
        for (const auto& r : m_residuals) {
            r.eval(x, out.data() + row);
            row += r.count;
        }
    }

    /// Dense Jacobian (rows = residuals, row-major), central differences.
    void jacobian(std::vector<double>& x, std::vector<double>& jac) const
    {
        const std::size_t n = m_x.size();
        jac.assign(residualCount() * n, 0.0);
        std::size_t row = 0;
        std::vector<double> plus(8);
        std::vector<double> minus(8);
        for (const auto& r : m_residuals) {
            plus.resize(r.count);
            minus.resize(r.count);
            for (const std::size_t v : r.vars) {
                const double old = x[v];
                const double h = 1e-6 * std::max(1.0, std::abs(old));
                x[v] = old + h;
                r.eval(x, plus.data());
                x[v] = old - h;
                r.eval(x, minus.data());
                x[v] = old;
                for (std::size_t k = 0; k < r.count; ++k)
                    jac[(row + k) * n + v] += (plus[k] - minus[k]) / (2 * h);
            }
            row += r.count;
        }
    }

    [[nodiscard]] std::vector<double>& x() { return m_x; }
    [[nodiscard]] const std::vector<Residual>& residualList() const { return m_residuals; }

    void writeBack(std::vector<SketchCurve>& curves) const
    {
        for (std::size_t i = 0; i < curves.size(); ++i) {
            auto& c = curves[i];
            const auto& v = m_vars[i];
            switch (c.kind) {
            case Kind::Line:
                c.points = {v.pts[0].at(m_x), v.pts[1].at(m_x)};
                break;
            case Kind::Circle:
                c.points = {v.pts[0].at(m_x)};
                c.radius = std::abs(m_x[v.rvar]);
                break;
            case Kind::Arc: {
                const Vec2 s = v.pts[0].at(m_x);
                const Vec2 e = v.pts[1].at(m_x);
                const Vec2 ctr = v.pts[2].at(m_x);
                const double r = len(s - ctr);
                const double a0 = std::atan2(s.y - ctr.y, s.x - ctr.x);
                double a1 = std::atan2(e.y - ctr.y, e.x - ctr.x);
                if (v.ccw) {
                    while (a1 <= a0)
                        a1 += 2 * std::numbers::pi;
                } else {
                    while (a1 >= a0)
                        a1 -= 2 * std::numbers::pi;
                }
                const double am = (a0 + a1) / 2;
                c.points = {s, ctr + Vec2{std::cos(am), std::sin(am)} * r, e};
                break;
            }
            case Kind::Spline:
                for (std::size_t k = 0; k < c.points.size(); ++k)
                    c.points[k] = v.pts[k].at(m_x);
                break;
            }
        }
    }

private:
    // --- Variables ---------------------------------------------------------------------

    P newPoint(Vec2 p)
    {
        P out{m_x.size()};
        m_x.push_back(p.x);
        m_x.push_back(p.y);
        return out;
    }

    /// Curve ends are shared between curves that touch.
    P endPoint(Vec2 p)
    {
        for (const auto& [pos, handle] : m_ends) {
            if (len(pos - p) < kMergeTolerance)
                return handle;
        }
        const P h = newPoint(p);
        m_ends.emplace_back(p, h);
        return h;
    }

    void buildVariables()
    {
        m_vars.resize(m_curves.size());
        for (std::size_t i = 0; i < m_curves.size(); ++i) {
            const auto& c = m_curves[i];
            auto& v = m_vars[i];
            switch (c.kind) {
            case Kind::Line:
                if (c.points.size() != 2)
                    throw ModelingError("Sketch line needs two points");
                v.pts = {endPoint(c.points[0]), endPoint(c.points[1])};
                break;
            case Kind::Arc: {
                if (c.points.size() != 3)
                    throw ModelingError("Sketch arc needs three points");
                const Vec2 s = c.points[0];
                const Vec2 t = c.points[1];
                const Vec2 e = c.points[2];
                const double d = 2 * cross(t - s, e - t);
                if (std::abs(d) < 1e-12)
                    throw ModelingError("Sketch arc points are collinear");
                // Circumcentre of the three points.
                const double ss = dot(s, s);
                const double tt = dot(t, t);
                const double ee = dot(e, e);
                const Vec2 ctr{(ss * (t.y - e.y) + tt * (e.y - s.y) + ee * (s.y - t.y)) /
                                   (2 * (s.x * (t.y - e.y) + t.x * (e.y - s.y) + e.x * (s.y - t.y))),
                               (ss * (e.x - t.x) + tt * (s.x - e.x) + ee * (t.x - s.x)) /
                                   (2 * (s.x * (t.y - e.y) + t.x * (e.y - s.y) + e.x * (s.y - t.y)))};
                v.ccw = d > 0;
                v.pts = {endPoint(s), endPoint(e), newPoint(ctr)};
                break;
            }
            case Kind::Circle:
                if (c.points.empty())
                    throw ModelingError("Sketch circle needs a centre");
                v.pts = {newPoint(c.points[0])};
                v.rvar = m_x.size();
                m_x.push_back(c.radius);
                break;
            case Kind::Spline:
                for (std::size_t k = 0; k < c.points.size(); ++k) {
                    const bool end = !c.closed && (k == 0 || k + 1 == c.points.size());
                    v.pts.push_back(end ? endPoint(c.points[k]) : newPoint(c.points[k]));
                }
                break;
            }
        }
    }

    static std::vector<std::size_t> varsOf(std::initializer_list<P> pts, std::initializer_list<std::size_t> extra = {})
    {
        std::vector<std::size_t> out;
        for (const P& p : pts) {
            if (p.var != kNone) {
                out.push_back(p.var);
                out.push_back(p.var + 1);
            }
        }
        for (const std::size_t e : extra) {
            if (e != kNone)
                out.push_back(e);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    static std::vector<std::size_t> circVars(const Circ& c, std::initializer_list<P> pts = {})
    {
        std::vector<std::size_t> out = varsOf({c.center, c.start}, {c.rvar});
        for (const std::size_t v : varsOf(pts))
            out.push_back(v);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // --- Curve access ------------------------------------------------------------------

    const SketchCurve& curve(std::size_t i) const
    {
        if (i >= m_curves.size())
            throw ModelingError("Constraint refers to curve " + std::to_string(i) + ", the sketch has " +
                                std::to_string(m_curves.size()) + " curves");
        return m_curves[i];
    }

    std::pair<P, P> line(std::size_t i) const
    {
        if (curve(i).kind != Kind::Line)
            throw ModelingError("Curve " + std::to_string(i) + " is not a line");
        return {m_vars[i].pts[0], m_vars[i].pts[1]};
    }

    bool isCirc(std::size_t i) const
    {
        const Kind k = curve(i).kind;
        return k == Kind::Arc || k == Kind::Circle;
    }

    Circ circ(std::size_t i) const
    {
        const auto& v = m_vars[i];
        if (curve(i).kind == Kind::Circle)
            return {v.pts[0], {}, v.rvar};
        if (curve(i).kind == Kind::Arc)
            return {v.pts[2], v.pts[0], kNone};
        throw ModelingError("Curve " + std::to_string(i) + " is not an arc or a circle");
    }

    P point(const SketchPointRef& ref) const
    {
        if (ref.kind == SketchPointRef::Kind::Origin)
            return {};
        const auto& c = curve(ref.curve);
        const auto& v = m_vars[ref.curve];
        switch (ref.kind) {
        case SketchPointRef::Kind::Start:
            if (c.kind == Kind::Circle)
                break;
            return v.pts.front();
        case SketchPointRef::Kind::End:
            if (c.kind == Kind::Circle)
                break;
            return c.kind == Kind::Spline ? v.pts.back() : v.pts[1];
        case SketchPointRef::Kind::Center:
            if (c.kind == Kind::Circle)
                return v.pts[0];
            if (c.kind == Kind::Arc)
                return v.pts[2];
            break;
        case SketchPointRef::Kind::Fit:
            if (c.kind == Kind::Spline && ref.index < v.pts.size())
                return v.pts[ref.index];
            break;
        case SketchPointRef::Kind::Origin:
            break;
        }
        throw ModelingError("Curve " + std::to_string(ref.curve) + " has no such point");
    }

    // --- Start guess -------------------------------------------------------------------

    /// Turning a line by 90 degrees is a singular configuration for the orientation
    /// residuals (their gradient vanishes, the solver would rather collapse the line).
    /// Lines whose orientation constraint is far from satisfied are therefore turned
    /// about their midpoint in the start guess first.
    void preAlign(const std::vector<SketchConstraint>& constraints)
    {
        for (const auto& c : constraints) {
            std::size_t target = kNone;
            std::optional<double> angle; // wanted direction of the line `target` (radians)
            auto dirOf = [this](std::size_t i) {
                const auto& v = m_vars[i];
                const Vec2 d = v.pts[1].at(m_x) - v.pts[0].at(m_x);
                return std::atan2(d.y, d.x);
            };
            const bool lines =
                !c.curves.empty() && std::all_of(c.curves.begin(), c.curves.end(), [this](std::size_t i) {
                    return i < m_curves.size() && m_curves[i].kind == Kind::Line;
                });
            if (!lines)
                continue;
            if ((c.type == Type::Horizontal || c.type == Type::Vertical) && c.curves.size() == 1) {
                target = c.curves[0];
                angle = c.type == Type::Horizontal ? 0.0 : std::numbers::pi / 2;
            } else if ((c.type == Type::Parallel || c.type == Type::Perpendicular || c.type == Type::Collinear) &&
                       c.curves.size() == 2) {
                target = c.curves[1];
                angle = dirOf(c.curves[0]) + (c.type == Type::Perpendicular ? std::numbers::pi / 2 : 0.0);
            }
            if (target == kNone || !angle)
                continue;
            const double current = dirOf(target);
            // Lines have no direction for these constraints: compare modulo 180 degrees.
            double delta = std::remainder(*angle - current, std::numbers::pi);
            if (std::abs(delta) < std::numbers::pi / 6)
                continue;
            const auto& v = m_vars[target];
            const Vec2 a = v.pts[0].at(m_x);
            const Vec2 b = v.pts[1].at(m_x);
            const Vec2 mid = (a + b) * 0.5;
            const double cs = std::cos(delta);
            const double sn = std::sin(delta);
            auto turn = [&](P p, Vec2 q) {
                if (p.var == kNone)
                    return;
                const Vec2 d = q - mid;
                m_x[p.var] = mid.x + cs * d.x - sn * d.y;
                m_x[p.var + 1] = mid.y + sn * d.x + cs * d.y;
            };
            turn(v.pts[0], a);
            turn(v.pts[1], b);
        }
    }

    // --- Constraints -------------------------------------------------------------------

    void add(std::size_t constraint, std::vector<std::size_t> vars, std::size_t count,
             std::function<void(const std::vector<double>&, double*)> eval)
    {
        m_residuals.push_back({std::move(vars), count, std::move(eval), constraint});
    }

    void needPoints(const SketchConstraint& c, std::size_t n) const
    {
        if (c.points.size() < n)
            throw ModelingError(std::string(toString(c.type)) + " needs " + std::to_string(n) + " point(s)");
    }

    void needCurves(const SketchConstraint& c, std::size_t n) const
    {
        if (c.curves.size() < n)
            throw ModelingError(std::string(toString(c.type)) + " needs " + std::to_string(n) + " curve(s)");
    }

    void fixPoint(std::size_t ci, P p, std::optional<Vec2> target = std::nullopt)
    {
        if (p.var == kNone)
            return;
        const Vec2 at = target ? *target : p.at(m_x0);
        add(ci, varsOf({p}), 2, [p, at](const std::vector<double>& x, double* r) {
            const Vec2 q = p.at(x);
            r[0] = q.x - at.x;
            r[1] = q.y - at.y;
        });
    }

    void addConstraint(std::size_t ci, const SketchConstraint& c)
    {
        const auto& x0 = m_x0;
        switch (c.type) {
        case Type::Coincident: {
            needPoints(c, 2);
            const P a = point(c.points[0]);
            const P b = point(c.points[1]);
            add(ci, varsOf({a, b}), 2, [a, b](const std::vector<double>& x, double* r) {
                const Vec2 d = b.at(x) - a.at(x);
                r[0] = d.x;
                r[1] = d.y;
            });
            break;
        }
        case Type::PointOnCurve: {
            needPoints(c, 1);
            needCurves(c, 1);
            const P p = point(c.points[0]);
            if (isCirc(c.curves[0])) {
                const Circ k = circ(c.curves[0]);
                add(ci, circVars(k, {p}), 1, [p, k](const std::vector<double>& x, double* r) {
                    r[0] = len(p.at(x) - k.center.at(x)) - k.radius(x);
                });
            } else {
                const auto sega = line(c.curves[0]);
                const P a = sega.first;
                const P b = sega.second;
                add(ci, varsOf({p, a, b}), 1, [p, a, b](const std::vector<double>& x, double* r) {
                    const Vec2 d = b.at(x) - a.at(x);
                    r[0] = cross(d, p.at(x) - a.at(x)) / safe(len(d));
                });
            }
            break;
        }
        case Type::Midpoint: {
            needPoints(c, 1);
            needCurves(c, 1);
            const P p = point(c.points[0]);
            const auto sega = line(c.curves[0]);
            const P a = sega.first;
            const P b = sega.second;
            add(ci, varsOf({p, a, b}), 2, [p, a, b](const std::vector<double>& x, double* r) {
                const Vec2 m = (a.at(x) + b.at(x)) * 0.5;
                r[0] = p.at(x).x - m.x;
                r[1] = p.at(x).y - m.y;
            });
            break;
        }
        case Type::Horizontal:
        case Type::Vertical: {
            P a;
            P b;
            if (!c.curves.empty()) {
                const auto seg = line(c.curves[0]);
                a = seg.first;
                b = seg.second;
            } else {
                needPoints(c, 2);
                a = point(c.points[0]);
                b = point(c.points[1]);
            }
            const bool h = c.type == Type::Horizontal;
            add(ci, varsOf({a, b}), 1, [a, b, h](const std::vector<double>& x, double* r) {
                const Vec2 d = b.at(x) - a.at(x);
                r[0] = h ? d.y : d.x;
            });
            break;
        }
        case Type::Parallel:
        case Type::Perpendicular:
        case Type::Collinear: {
            needCurves(c, 2);
            const auto sega1 = line(c.curves[0]);
            const P a1 = sega1.first;
            const P b1 = sega1.second;
            const auto sega2 = line(c.curves[1]);
            const P a2 = sega2.first;
            const P b2 = sega2.second;
            const bool perp = c.type == Type::Perpendicular;
            add(ci, varsOf({a1, b1, a2, b2}), 1, [=](const std::vector<double>& x, double* r) {
                const Vec2 d1 = b1.at(x) - a1.at(x);
                const Vec2 d2 = b2.at(x) - a2.at(x);
                r[0] = (perp ? dot(d1, d2) : cross(d1, d2)) / safe(len(d1));
            });
            if (c.type == Type::Collinear) {
                add(ci, varsOf({a1, b1, a2}), 1, [=](const std::vector<double>& x, double* r) {
                    const Vec2 d1 = b1.at(x) - a1.at(x);
                    r[0] = cross(d1, a2.at(x) - a1.at(x)) / safe(len(d1));
                });
            }
            break;
        }
        case Type::Tangent: {
            needCurves(c, 2);
            std::size_t i = c.curves[0];
            std::size_t j = c.curves[1];
            if (!isCirc(i))
                std::swap(i, j);
            if (!isCirc(i))
                throw ModelingError("Tangent needs at least one arc or circle");
            const Circ k = circ(i);
            if (isCirc(j)) {
                const Circ l = circ(j);
                const double d0 = len(k.center.at(x0) - l.center.at(x0));
                const double r1 = k.radius(x0);
                const double r2 = l.radius(x0);
                const bool external = std::abs(d0 - (r1 + r2)) <= std::abs(d0 - std::abs(r1 - r2));
                const double s = signOf(r1 - r2);
                std::vector<std::size_t> vars = circVars(k);
                for (const std::size_t v : circVars(l))
                    vars.push_back(v);
                add(ci, vars, 1, [k, l, external, s](const std::vector<double>& x, double* r) {
                    const double d = len(k.center.at(x) - l.center.at(x));
                    r[0] = external ? d - (k.radius(x) + l.radius(x)) : d - s * (k.radius(x) - l.radius(x));
                });
            } else {
                const auto sega = line(j);
                const P a = sega.first;
                const P b = sega.second;
                auto signedDist = [k, a, b](const std::vector<double>& x) {
                    const Vec2 d = b.at(x) - a.at(x);
                    return cross(d, k.center.at(x) - a.at(x)) / safe(len(d));
                };
                const double s = signOf(signedDist(x0));
                add(ci, circVars(k, {a, b}), 1, [k, s, signedDist](const std::vector<double>& x, double* r) {
                    r[0] = s * signedDist(x) - k.radius(x);
                });
            }
            break;
        }
        case Type::Equal: {
            needCurves(c, 2);
            if (isCirc(c.curves[0]) && isCirc(c.curves[1])) {
                const Circ k = circ(c.curves[0]);
                const Circ l = circ(c.curves[1]);
                std::vector<std::size_t> vars = circVars(k);
                for (const std::size_t v : circVars(l))
                    vars.push_back(v);
                add(ci, vars, 1, [k, l](const std::vector<double>& x, double* r) { r[0] = k.radius(x) - l.radius(x); });
            } else {
                const auto sega1 = line(c.curves[0]);
                const P a1 = sega1.first;
                const P b1 = sega1.second;
                const auto sega2 = line(c.curves[1]);
                const P a2 = sega2.first;
                const P b2 = sega2.second;
                add(ci, varsOf({a1, b1, a2, b2}), 1, [=](const std::vector<double>& x, double* r) {
                    r[0] = len(b1.at(x) - a1.at(x)) - len(b2.at(x) - a2.at(x));
                });
            }
            break;
        }
        case Type::Concentric: {
            needCurves(c, 2);
            const P a = circ(c.curves[0]).center;
            const P b = circ(c.curves[1]).center;
            add(ci, varsOf({a, b}), 2, [a, b](const std::vector<double>& x, double* r) {
                const Vec2 d = b.at(x) - a.at(x);
                r[0] = d.x;
                r[1] = d.y;
            });
            break;
        }
        case Type::Symmetric: {
            needPoints(c, 2);
            needCurves(c, 1);
            const P p = point(c.points[0]);
            const P q = point(c.points[1]);
            const auto sega = line(c.curves[0]);
            const P a = sega.first;
            const P b = sega.second;
            add(ci, varsOf({p, q, a, b}), 2, [=](const std::vector<double>& x, double* r) {
                const Vec2 d = b.at(x) - a.at(x);
                const Vec2 m = (p.at(x) + q.at(x)) * 0.5;
                r[0] = cross(d, m - a.at(x)) / safe(len(d));
                r[1] = dot(d, q.at(x) - p.at(x)) / safe(len(d));
            });
            break;
        }
        case Type::Fix: {
            if (c.points.empty() && c.curves.empty())
                throw ModelingError("fix needs points or curves");
            if (!c.at.empty() && c.at.size() != c.points.size())
                throw ModelingError("fix: 'at' needs one position per point");
            for (std::size_t k = 0; k < c.points.size(); ++k)
                fixPoint(ci, point(c.points[k]), c.at.empty() ? std::nullopt : std::optional<Vec2>(c.at[k]));
            for (const std::size_t i : c.curves) {
                (void)curve(i);
                for (const P& p : m_vars[i].pts)
                    fixPoint(ci, p);
                if (const std::size_t rv = m_vars[i].rvar; rv != kNone) {
                    const double r0 = x0[rv];
                    add(ci, {rv}, 1, [rv, r0](const std::vector<double>& x, double* r) { r[0] = x[rv] - r0; });
                }
            }
            break;
        }
        case Type::Distance: {
            needPoints(c, 1);
            const P p = point(c.points[0]);
            const double v = c.value;
            if (c.points.size() >= 2) {
                const P q = point(c.points[1]);
                add(ci, varsOf({p, q}), 1,
                    [p, q, v](const std::vector<double>& x, double* r) { r[0] = len(q.at(x) - p.at(x)) - v; });
            } else {
                needCurves(c, 1);
                const auto sega = line(c.curves[0]);
                const P a = sega.first;
                const P b = sega.second;
                auto signedDist = [p, a, b](const std::vector<double>& x) {
                    const Vec2 d = b.at(x) - a.at(x);
                    return cross(d, p.at(x) - a.at(x)) / safe(len(d));
                };
                const double s = signOf(signedDist(x0));
                add(ci, varsOf({p, a, b}), 1,
                    [s, v, signedDist](const std::vector<double>& x, double* r) { r[0] = s * signedDist(x) - v; });
            }
            break;
        }
        case Type::HorizontalDistance:
        case Type::VerticalDistance: {
            needPoints(c, 2);
            const P p = point(c.points[0]);
            const P q = point(c.points[1]);
            const bool h = c.type == Type::HorizontalDistance;
            const Vec2 d0 = q.at(x0) - p.at(x0);
            const double s = signOf(h ? d0.x : d0.y);
            const double v = c.value;
            add(ci, varsOf({p, q}), 1, [p, q, h, s, v](const std::vector<double>& x, double* r) {
                const Vec2 d = q.at(x) - p.at(x);
                r[0] = s * (h ? d.x : d.y) - v;
            });
            break;
        }
        case Type::Length: {
            needCurves(c, 1);
            const auto sega = line(c.curves[0]);
            const P a = sega.first;
            const P b = sega.second;
            const double v = c.value;
            add(ci, varsOf({a, b}), 1,
                [a, b, v](const std::vector<double>& x, double* r) { r[0] = len(b.at(x) - a.at(x)) - v; });
            break;
        }
        case Type::Radius:
        case Type::Diameter: {
            needCurves(c, 1);
            const Circ k = circ(c.curves[0]);
            const double v = c.type == Type::Diameter ? c.value / 2 : c.value;
            add(ci, circVars(k), 1, [k, v](const std::vector<double>& x, double* r) { r[0] = k.radius(x) - v; });
            break;
        }
        case Type::Angle: {
            needCurves(c, 2);
            const auto sega1 = line(c.curves[0]);
            const P a1 = sega1.first;
            const P b1 = sega1.second;
            const auto sega2 = line(c.curves[1]);
            const P a2 = sega2.first;
            const P b2 = sega2.second;
            auto angle = [=](const std::vector<double>& x) {
                const Vec2 d1 = b1.at(x) - a1.at(x);
                const Vec2 d2 = b2.at(x) - a2.at(x);
                return std::atan2(cross(d1, d2), dot(d1, d2));
            };
            const double target = signOf(angle(x0)) * c.value * std::numbers::pi / 180.0;
            add(ci, varsOf({a1, b1, a2, b2}), 1,
                [angle, target](const std::vector<double>& x, double* r) { r[0] = wrapAngle(angle(x) - target); });
            break;
        }
        }
    }

    std::vector<SketchCurve> m_curves;
    std::vector<CurveVars> m_vars;
    std::vector<std::pair<Vec2, P>> m_ends;
    std::vector<double> m_x;
    std::vector<double> m_x0;
    std::vector<Residual> m_residuals;
};

double maxAbs(const std::vector<double>& v)
{
    double m = 0.0;
    for (const double d : v)
        m = std::max(m, std::abs(d));
    return m;
}

double sumSquares(const std::vector<double>& v)
{
    double s = 0.0;
    for (const double d : v)
        s += d * d;
    return s;
}

/// Solves the symmetric system A x = b in place (Gaussian elimination, partial pivoting).
bool solveLinear(std::vector<double>& a, std::vector<double>& b, std::size_t n)
{
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row) {
            if (std::abs(a[row * n + col]) > std::abs(a[pivot * n + col]))
                pivot = row;
        }
        if (std::abs(a[pivot * n + col]) < 1e-300)
            return false;
        if (pivot != col) {
            for (std::size_t k = 0; k < n; ++k)
                std::swap(a[col * n + k], a[pivot * n + k]);
            std::swap(b[col], b[pivot]);
        }
        for (std::size_t row = col + 1; row < n; ++row) {
            const double f = a[row * n + col] / a[col * n + col];
            if (f == 0.0)
                continue;
            for (std::size_t k = col; k < n; ++k)
                a[row * n + k] -= f * a[col * n + k];
            b[row] -= f * b[col];
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        double s = b[i];
        for (std::size_t k = i + 1; k < n; ++k)
            s -= a[i * n + k] * b[k];
        b[i] = s / a[i * n + i];
    }
    return true;
}

/// Numerical rank of a row-major m x n matrix (rows normalised first).
std::size_t rank(std::vector<double> j, std::size_t m, std::size_t n)
{
    for (std::size_t r = 0; r < m; ++r) {
        double norm = 0.0;
        for (std::size_t c = 0; c < n; ++c)
            norm += j[r * n + c] * j[r * n + c];
        norm = std::sqrt(norm);
        if (norm > 0)
            for (std::size_t c = 0; c < n; ++c)
                j[r * n + c] /= norm;
    }
    std::size_t rk = 0;
    std::vector<bool> used(m, false);
    for (std::size_t c = 0; c < n && rk < m; ++c) {
        std::size_t best = kNone;
        double bestValue = 1e-8;
        for (std::size_t r = 0; r < m; ++r) {
            if (!used[r] && std::abs(j[r * n + c]) > bestValue) {
                bestValue = std::abs(j[r * n + c]);
                best = r;
            }
        }
        if (best == kNone)
            continue;
        used[best] = true;
        ++rk;
        for (std::size_t r = 0; r < m; ++r) {
            if (r == best || used[r])
                continue;
            const double f = j[r * n + c] / j[best * n + c];
            if (f == 0.0)
                continue;
            for (std::size_t k = c; k < n; ++k)
                j[r * n + k] -= f * j[best * n + k];
        }
    }
    return rk;
}

int dofAt(const Model& model, std::vector<double> x)
{
    const std::size_t n = model.variableCount();
    const std::size_t m = model.residualCount();
    if (m == 0)
        return static_cast<int>(n);
    std::vector<double> jac;
    model.jacobian(x, jac);
    return static_cast<int>(n - rank(std::move(jac), m, n));
}

} // namespace

SketchConstraint::Type constraintType(std::string_view name)
{
    for (const auto& e : kNames) {
        if (e.name == name)
            return e.type;
    }
    std::string known;
    for (const auto& e : kNames)
        known += (known.empty() ? "" : ", ") + std::string(e.name);
    throw ModelingError("Unknown sketch constraint '" + std::string(name) + "' (" + known + ")");
}

std::string_view toString(SketchConstraint::Type type)
{
    for (const auto& e : kNames) {
        if (e.type == type)
            return e.name;
    }
    return "unknown";
}

bool isDimension(SketchConstraint::Type type)
{
    switch (type) {
    case Type::Distance:
    case Type::HorizontalDistance:
    case Type::VerticalDistance:
    case Type::Length:
    case Type::Radius:
    case Type::Diameter:
    case Type::Angle:
        return true;
    default:
        return false;
    }
}

SketchSolveResult solveSketch(std::vector<SketchCurve>& curves, const std::vector<SketchConstraint>& constraints)
{
    Model model(curves, constraints);
    SketchSolveResult result;
    std::vector<double>& x = model.x();
    const std::size_t n = model.variableCount();
    std::vector<double> r;
    model.residuals(x, r);
    double cost = sumSquares(r);
    double lambda = 1e-3;
    std::vector<double> jac;
    std::vector<double> a(n * n);
    std::vector<double> g(n);
    std::vector<double> trial;
    std::vector<double> rTrial;
    const std::size_t m = r.size();
    for (int iter = 0; iter < 200 && maxAbs(r) > kTolerance * 0.01; ++iter) {
        result.iterations = iter + 1;
        model.jacobian(x, jac);
        std::fill(a.begin(), a.end(), 0.0);
        std::fill(g.begin(), g.end(), 0.0);
        for (std::size_t row = 0; row < m; ++row) {
            const double* jr = jac.data() + row * n;
            for (std::size_t i = 0; i < n; ++i) {
                if (jr[i] == 0.0)
                    continue;
                g[i] -= jr[i] * r[row];
                for (std::size_t k = 0; k < n; ++k)
                    a[i * n + k] += jr[i] * jr[k];
            }
        }
        bool improved = false;
        for (int attempt = 0; attempt < 30; ++attempt) {
            std::vector<double> lhs = a;
            std::vector<double> step = g;
            for (std::size_t i = 0; i < n; ++i)
                lhs[i * n + i] += lambda * (1.0 + a[i * n + i]);
            if (!solveLinear(lhs, step, n)) {
                lambda *= 10;
                continue;
            }
            trial = x;
            for (std::size_t i = 0; i < n; ++i)
                trial[i] += step[i];
            model.residuals(trial, rTrial);
            const double c = sumSquares(rTrial);
            if (c < cost) {
                x = trial;
                r = rTrial;
                cost = c;
                lambda = std::max(lambda / 3, 1e-12);
                improved = true;
                break;
            }
            lambda *= 4;
        }
        if (!improved)
            break;
    }

    // Report the constraints that are still violated.
    std::size_t row = 0;
    for (const auto& res : model.residualList()) {
        bool bad = false;
        for (std::size_t k = 0; k < res.count; ++k)
            bad = bad || std::abs(r[row + k]) > kTolerance * 10;
        row += res.count;
        if (bad) {
            result.solved = false;
            if (res.constraint != kNone && std::find(result.unsatisfied.begin(), result.unsatisfied.end(),
                                                     res.constraint) == result.unsatisfied.end())
                result.unsatisfied.push_back(res.constraint);
        }
    }
    result.degreesOfFreedom = dofAt(model, x);
    if (result.solved)
        model.writeBack(curves);
    return result;
}

int degreesOfFreedom(const std::vector<SketchCurve>& curves, const std::vector<SketchConstraint>& constraints)
{
    Model model(curves, constraints);
    return dofAt(model, model.x());
}

} // namespace aerocad::cad
