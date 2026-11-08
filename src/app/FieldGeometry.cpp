// SPDX-License-Identifier: GPL-3.0-or-later
#include "FieldGeometry.hpp"

#include "AppController.hpp"

#include "aerocad/cfd/RunManager.hpp"

#include <QByteArray>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace aerocad::app {

namespace {

// Moreland's "cool to warm" diverging map (pressure: blue suction, red stagnation) and
// viridis (speed), 9 stops each, linearly interpolated.
constexpr std::array<std::array<int, 3>, 9> kDiverging{{{59, 76, 192},
                                                        {98, 130, 234},
                                                        {141, 176, 254},
                                                        {184, 208, 249},
                                                        {221, 221, 221},
                                                        {245, 196, 173},
                                                        {244, 154, 123},
                                                        {222, 96, 77},
                                                        {180, 4, 38}}};
constexpr std::array<std::array<int, 3>, 9> kSequential{{{68, 1, 84},
                                                         {71, 45, 123},
                                                         {59, 82, 139},
                                                         {44, 114, 142},
                                                         {33, 145, 140},
                                                         {40, 174, 128},
                                                         {94, 201, 98},
                                                         {173, 220, 48},
                                                         {253, 231, 37}}};

// Google's Turbo colour map (Apache 2.0), polynomial approximation by its author, used
// from 0.08 to 0.95 so both ends stay saturated (deep blue to red) rather than near black.
QColor turbo(double t)
{
    t = 0.08 + 0.87 * t;
    const double r = 0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943))));
    const double g = 0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604))));
    const double b = 0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973))));
    return QColor::fromRgbF(static_cast<float>(std::clamp(r, 0.0, 1.0)), static_cast<float>(std::clamp(g, 0.0, 1.0)),
                            static_cast<float>(std::clamp(b, 0.0, 1.0)));
}

QColor interpolate(const std::array<std::array<int, 3>, 9>& map, double t)
{
    t = t * static_cast<double>(map.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(t), map.size() - 2);
    const double f = t - static_cast<double>(i);
    auto mix = [&](int c) {
        return static_cast<int>(std::lround(map[i][static_cast<std::size_t>(c)] * (1 - f) +
                                            map[i + 1][static_cast<std::size_t>(c)] * f));
    };
    return QColor(mix(0), mix(1), mix(2));
}

} // namespace

QString FieldGeometry::resolvedPalette(const QString& scalar, const QString& palette)
{
    if (palette == QLatin1String("rainbow") || palette == QLatin1String("coolwarm") || palette == QLatin1String("viridis"))
        return palette;
    if (scalar == QLatin1String("speed"))
        return QStringLiteral("viridis");
    // A/B differences (dCp, dpressure): blue = lower in B, white = no change, red = higher.
    return scalar.startsWith(QLatin1Char('d')) ? QStringLiteral("coolwarm") : QStringLiteral("rainbow");
}

QColor FieldGeometry::mapColor(const QString& scalar, double t, const QString& palette)
{
    if (!std::isfinite(t))
        t = 0.5;
    t = std::clamp(t, 0.0, 1.0);
    const QString p = resolvedPalette(scalar, palette);
    if (p == QLatin1String("rainbow"))
        return turbo(t);
    return interpolate(p == QLatin1String("viridis") ? kSequential : kDiverging, t);
}

double FieldGeometry::scalePosition(const QString& scalar, double value, double lo, double hi)
{
    if (!std::isfinite(value))
        return std::nan("");
    const bool pressure = scalar == QLatin1String("Cp") || scalar == QLatin1String("pressure") ||
                          scalar == QLatin1String("p") || scalar == QLatin1String("dCp") ||
                          scalar == QLatin1String("dpressure");
    if (pressure && lo < 0.0 && hi > 0.0)
        return value < 0.0 ? 0.5 * (value - lo) / -lo : 0.5 + 0.5 * value / hi;
    return hi > lo ? (value - lo) / (hi - lo) : 0.5;
}

FieldGeometry::FieldGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent)
{
    // A valid empty layout until the first (queued) rebuild: a frame drawn before it with
    // stride 0 makes Qt Quick 3D divide by zero (crash seen on a large F1 result).
    rebuild();
}

QVariantList FieldGeometry::colormap() const
{
    QVariantList out;
    for (int i = 0; i <= 16; ++i)
        out.append(mapColor(m_scalar, i / 16.0, m_palette));
    return out;
}

void FieldGeometry::setController(AppController* controller)
{
    if (m_controller == controller)
        return;
    m_controller = controller;
    schedule(true);
    emit changed();
}

void FieldGeometry::setRun(const QString& run)
{
    if (m_run == run)
        return;
    m_run = run;
    schedule(true);
    emit changed();
}

void FieldGeometry::setCompareRun(const QString& run)
{
    if (m_compareRun == run)
        return;
    m_compareRun = run;
    schedule(true);
    emit changed();
}

void FieldGeometry::setKind(const QString& kind)
{
    if (m_kind == kind)
        return;
    m_kind = kind;
    schedule(true);
    emit changed();
}

void FieldGeometry::setScalar(const QString& scalar)
{
    if (m_scalar == scalar)
        return;
    m_scalar = scalar;
    schedule(false);
    emit changed();
}

void FieldGeometry::setPalette(const QString& palette)
{
    if (m_palette == palette)
        return;
    m_palette = palette;
    schedule(false);
    emit changed();
}

void FieldGeometry::setRangeMin(double v)
{
    if (m_rangeMin == v)
        return;
    m_rangeMin = v;
    schedule(false);
    emit changed();
}

void FieldGeometry::setRangeMax(double v)
{
    if (m_rangeMax == v)
        return;
    m_rangeMax = v;
    schedule(false);
    emit changed();
}

void FieldGeometry::setTubeRadius(double r)
{
    if (m_tubeRadius == r)
        return;
    m_tubeRadius = r;
    schedule(false);
    emit changed();
}

void FieldGeometry::setRevision(int revision)
{
    if (m_revision == revision)
        return;
    m_revision = revision;
    schedule(true);
    emit changed();
}

void FieldGeometry::reload()
{
    m_data = {};
    if (m_controller && !m_run.isEmpty()) {
        try {
            auto& sims = m_controller->session().simulations();
            m_data = !m_compareRun.isEmpty() && m_kind == QLatin1String("surface")
                         ? sims.surfaceDifference(m_run.toStdString(), m_compareRun.toStdString())
                         : sims.field(m_run.toStdString(), m_kind.toStdString());
        } catch (const std::exception&) {
            m_data = {};
        }
    }
    rebuild();
}

void FieldGeometry::schedule(bool reloadFiles)
{
    // Setting several properties in a row (as QML does on creation) reads and builds once.
    m_reloadPending = m_reloadPending || reloadFiles;
    if (m_updatePending)
        return;
    m_updatePending = true;
    QMetaObject::invokeMethod(this, [this] {
        m_updatePending = false;
        const bool files = m_reloadPending;
        m_reloadPending = false;
        if (files)
            reload();
        else
            rebuild();
    }, Qt::QueuedConnection);
}

void FieldGeometry::rebuild()
{
    clear();
    constexpr int stride = kFloatsPerVertex * static_cast<int>(sizeof(float));
    setStride(stride);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::NormalSemantic, 3 * static_cast<int>(sizeof(float)),
                 QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::ColorSemantic, 6 * static_cast<int>(sizeof(float)),
                 QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::IndexSemantic, 0, QQuick3DGeometry::Attribute::U32Type);
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);

    const std::vector<double>* values = nullptr;
    if (auto it = m_data.scalars.find(m_scalar.toStdString()); it != m_data.scalars.end())
        values = &it->second;
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    if (values != nullptr)
        for (const double v : *values)
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
    if (lo > hi)
        lo = hi = 0.0;
    m_dataMin = lo;
    m_dataMax = hi;
    const double a = m_rangeMin < m_rangeMax ? m_rangeMin : lo;
    const double b = m_rangeMin < m_rangeMax ? m_rangeMax : hi;
    auto colorOf = [&](std::uint32_t idx) {
        const double v = values != nullptr && idx < values->size() ? (*values)[idx] : std::nan("");
        if (!std::isfinite(v))
            return QColor(140, 140, 140); // No value here (e.g. an A/B difference where the shapes differ).
        return mapColor(m_scalar, scalePosition(m_scalar, v, a, b), m_palette);
    };

    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    core::BoundingBox box;
    auto pushVertex = [&](const core::Vec3& p, const core::Vec3& n, const QColor& c) {
        box.extend(p);
        vertices.insert(vertices.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z),
                                         static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z),
                                         static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                                         static_cast<float>(c.blueF()), 1.0F});
    };

    if (m_kind == QLatin1String("streamlines")) {
        // Tubes: a ring of kSides vertices per kept point, framed by parallel transport so
        // the tube does not twist; points closer than a radius to the last kept one are skipped.
        core::BoundingBox extent;
        for (const auto& p : m_data.points)
            extent.extend(p);
        const double radius = m_tubeRadius > 0 ? m_tubeRadius : std::max(1e-3, 0.004 * extent.diagonal());
        constexpr int kSides = 8;
        for (const auto& line : m_data.lines) {
            std::vector<std::uint32_t> kept;
            for (const auto idx : line) {
                if (idx >= m_data.points.size())
                    continue;
                if (kept.empty() || (m_data.points[idx] - m_data.points[kept.back()]).length() >= radius * 1.5)
                    kept.push_back(idx);
            }
            if (kept.size() < 2)
                continue;
            core::Vec3 normal;
            for (std::size_t i = 0; i < kept.size(); ++i) {
                const core::Vec3& p = m_data.points[kept[i]];
                const core::Vec3& prev = m_data.points[kept[i == 0 ? 0 : i - 1]];
                const core::Vec3& next = m_data.points[kept[std::min(i + 1, kept.size() - 1)]];
                core::Vec3 t = next - prev;
                if (t.length() <= 0)
                    continue;
                t = t * (1.0 / t.length());
                if (i == 0) {
                    const core::Vec3 up = std::abs(t.z) < 0.9 ? core::Vec3{0, 0, 1} : core::Vec3{1, 0, 0};
                    normal = t.cross(up);
                } else {
                    normal = normal - t * normal.dot(t); // Parallel transport.
                }
                if (normal.length() < 1e-12)
                    normal = t.cross(std::abs(t.z) < 0.9 ? core::Vec3{0, 0, 1} : core::Vec3{1, 0, 0});
                normal = normal * (1.0 / normal.length());
                const core::Vec3 binormal = t.cross(normal);
                const QColor c = colorOf(kept[i]);
                const auto base = static_cast<std::uint32_t>(vertices.size() / kFloatsPerVertex);
                for (int k = 0; k < kSides; ++k) {
                    const double ang = 2.0 * std::numbers::pi * k / kSides;
                    const core::Vec3 n = normal * std::cos(ang) + binormal * std::sin(ang);
                    pushVertex(p + n * radius, n, c);
                }
                if (i > 0) {
                    const std::uint32_t prevBase = base - kSides;
                    for (std::uint32_t k = 0; k < kSides; ++k) {
                        const std::uint32_t k1 = (k + 1) % kSides;
                        indices.insert(indices.end(), {prevBase + k, prevBase + k1, base + k1, prevBase + k, base + k1, base + k});
                    }
                }
            }
        }
    } else {
        // Indexed surface: one vertex per point with an area-weighted normal.
        const std::size_t n = m_data.points.size();
        std::vector<core::Vec3> normals(n);
        for (const auto& poly : m_data.polygons) {
            if (poly.size() < 3 || std::any_of(poly.begin(), poly.end(), [n](std::uint32_t i) { return i >= n; }))
                continue;
            core::Vec3 area; // Newell's method.
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const core::Vec3& p = m_data.points[poly[i]];
                const core::Vec3& q = m_data.points[poly[(i + 1) % poly.size()]];
                area = area + p.cross(q);
            }
            for (const auto i : poly)
                normals[i] = normals[i] + area;
            for (std::size_t i = 2; i < poly.size(); ++i) // Fan triangulation (convex faces).
                indices.insert(indices.end(), {poly[0], poly[i - 1], poly[i]});
        }
        // The part's surface is closed around its centre: orient the normals outwards
        // (OpenFOAM's patch faces may point either way depending on the writer).
        bool flip = false;
        if (m_kind == QLatin1String("surface") && n > 0) {
            core::Vec3 centre;
            for (const auto& p : m_data.points)
                centre = centre + p;
            centre = centre * (1.0 / static_cast<double>(n));
            double outward = 0.0;
            for (std::size_t i = 0; i < n; ++i)
                outward += normals[i].dot(m_data.points[i] - centre);
            flip = outward < 0.0;
        }
        // Qt Quick 3D lights the back of a two-sided face with the reversed normal, so the
        // triangles must wind counter-clockwise around the outward normal too.
        if (flip)
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
                std::swap(indices[i + 1], indices[i + 2]);
        for (std::size_t i = 0; i < n; ++i) {
            core::Vec3 nn = normals[i];
            const double len = nn.length();
            nn = len > 0 ? nn * ((flip ? -1.0 : 1.0) / len) : core::Vec3{0, 0, 1};
            pushVertex(m_data.points[i], nn, colorOf(static_cast<std::uint32_t>(i)));
        }
    }

    setVertexData(QByteArray(reinterpret_cast<const char*>(vertices.data()),
                             static_cast<qsizetype>(vertices.size() * sizeof(float))));
    setIndexData(QByteArray(reinterpret_cast<const char*>(indices.data()),
                            static_cast<qsizetype>(indices.size() * sizeof(std::uint32_t))));
    if (!box.isEmpty())
        setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
                  QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
    update();
    emit dataChanged();
}

QVariantMap FieldGeometry::pick(const QVector3D& origin, const QVector3D& direction) const
{
    if (m_kind == QLatin1String("streamlines") || m_data.polygons.empty())
        return {};
    const core::Vec3 o{origin.x(), origin.y(), origin.z()};
    const core::Vec3 dir{direction.x(), direction.y(), direction.z()};
    double bestT = std::numeric_limits<double>::max();
    std::array<std::uint32_t, 3> bestTri{};
    double bestU = 0, bestV = 0;
    const std::size_t n = m_data.points.size();
    for (const auto& poly : m_data.polygons) {
        for (std::size_t i = 2; i < poly.size(); ++i) {
            const std::uint32_t ia = poly[0], ib = poly[i - 1], ic = poly[i];
            if (ia >= n || ib >= n || ic >= n)
                continue;
            // Möller-Trumbore, both sides.
            const core::Vec3 e1 = m_data.points[ib] - m_data.points[ia];
            const core::Vec3 e2 = m_data.points[ic] - m_data.points[ia];
            const core::Vec3 pv = dir.cross(e2);
            const double det = e1.dot(pv);
            if (std::abs(det) < 1e-18)
                continue;
            const double inv = 1.0 / det;
            const core::Vec3 tv = o - m_data.points[ia];
            const double u = tv.dot(pv) * inv;
            if (u < 0.0 || u > 1.0)
                continue;
            const core::Vec3 qv = tv.cross(e1);
            const double v = dir.dot(qv) * inv;
            if (v < 0.0 || u + v > 1.0)
                continue;
            const double t = e2.dot(qv) * inv;
            if (t > 0.0 && t < bestT) {
                bestT = t;
                bestTri = {ia, ib, ic};
                bestU = u;
                bestV = v;
            }
        }
    }
    if (bestT == std::numeric_limits<double>::max())
        return {};
    const double w = 1.0 - bestU - bestV;
    const core::Vec3 p = o + dir * bestT;
    QVariantMap values;
    for (const auto& [name, column] : m_data.scalars) {
        if (bestTri[0] >= column.size() || bestTri[1] >= column.size() || bestTri[2] >= column.size())
            continue;
        const double v = w * column[bestTri[0]] + bestU * column[bestTri[1]] + bestV * column[bestTri[2]];
        if (std::isfinite(v))
            values.insert(QString::fromStdString(name), v);
    }
    return {{QStringLiteral("point"), QVector3D(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z))},
            {QStringLiteral("distance"), bestT},
            {QStringLiteral("kind"), m_kind},
            {QStringLiteral("values"), values}};
}

} // namespace aerocad::app
