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

const std::array<std::array<int, 3>, 9>& mapFor(const QString& scalar)
{
    return scalar == QLatin1String("speed") ? kSequential : kDiverging;
}

} // namespace

QColor FieldGeometry::mapColor(const QString& scalar, double t)
{
    const auto& map = mapFor(scalar);
    if (!std::isfinite(t))
        t = 0.5;
    t = std::clamp(t, 0.0, 1.0) * static_cast<double>(map.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(t), map.size() - 2);
    const double f = t - static_cast<double>(i);
    auto mix = [&](int c) {
        return static_cast<int>(std::lround(map[i][static_cast<std::size_t>(c)] * (1 - f) +
                                            map[i + 1][static_cast<std::size_t>(c)] * f));
    };
    return QColor(mix(0), mix(1), mix(2));
}

FieldGeometry::FieldGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent) {}

QVariantList FieldGeometry::colormap() const
{
    QVariantList out;
    for (const auto& c : mapFor(m_scalar))
        out.append(QColor(c[0], c[1], c[2]));
    return out;
}

void FieldGeometry::setController(AppController* controller)
{
    if (m_controller == controller)
        return;
    m_controller = controller;
    reload();
    emit changed();
}

void FieldGeometry::setRun(const QString& run)
{
    if (m_run == run)
        return;
    m_run = run;
    reload();
    emit changed();
}

void FieldGeometry::setKind(const QString& kind)
{
    if (m_kind == kind)
        return;
    m_kind = kind;
    reload();
    emit changed();
}

void FieldGeometry::setScalar(const QString& scalar)
{
    if (m_scalar == scalar)
        return;
    m_scalar = scalar;
    rebuild();
    emit changed();
}

void FieldGeometry::setRangeMin(double v)
{
    if (m_rangeMin == v)
        return;
    m_rangeMin = v;
    rebuild();
    emit changed();
}

void FieldGeometry::setRangeMax(double v)
{
    if (m_rangeMax == v)
        return;
    m_rangeMax = v;
    rebuild();
    emit changed();
}

void FieldGeometry::setRevision(int revision)
{
    if (m_revision == revision)
        return;
    m_revision = revision;
    reload();
    emit changed();
}

void FieldGeometry::reload()
{
    m_data = {};
    if (m_controller && !m_run.isEmpty()) {
        try {
            m_data = m_controller->session().simulations().field(m_run.toStdString(), m_kind.toStdString());
        } catch (const std::exception&) {
            m_data = {};
        }
    }
    rebuild();
}

void FieldGeometry::rebuild()
{
    clear();
    constexpr int floatsPerVertex = 7; // position + RGBA
    setStride(floatsPerVertex * static_cast<int>(sizeof(float)));
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::ColorSemantic, 3 * static_cast<int>(sizeof(float)),
                 QQuick3DGeometry::Attribute::F32Type);
    const bool lines = m_kind == QLatin1String("streamlines");
    setPrimitiveType(lines ? QQuick3DGeometry::PrimitiveType::Lines : QQuick3DGeometry::PrimitiveType::Triangles);

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
    const double span = b > a ? b - a : 1.0;

    std::vector<std::uint32_t> order; // Vertex indices to emit.
    if (lines) {
        for (const auto& l : m_data.lines)
            for (std::size_t i = 1; i < l.size(); ++i) {
                order.push_back(l[i - 1]);
                order.push_back(l[i]);
            }
    } else {
        for (const auto& poly : m_data.polygons)
            for (std::size_t i = 2; i < poly.size(); ++i) { // Fan triangulation (convex faces).
                order.push_back(poly[0]);
                order.push_back(poly[i - 1]);
                order.push_back(poly[i]);
            }
    }

    QByteArray buffer(static_cast<qsizetype>(order.size()) * floatsPerVertex * static_cast<qsizetype>(sizeof(float)),
                      Qt::Uninitialized);
    auto* out = reinterpret_cast<float*>(buffer.data());
    core::BoundingBox box;
    for (const std::uint32_t idx : order) {
        const core::Vec3& p = m_data.points[idx];
        box.extend(p);
        const double v = values != nullptr && idx < values->size() ? (*values)[idx] : std::nan("");
        const QColor c = mapColor(m_scalar, (v - a) / span);
        *out++ = static_cast<float>(p.x);
        *out++ = static_cast<float>(p.y);
        *out++ = static_cast<float>(p.z);
        *out++ = static_cast<float>(c.redF());
        *out++ = static_cast<float>(c.greenF());
        *out++ = static_cast<float>(c.blueF());
        *out++ = 1.0F;
    }
    setVertexData(buffer);
    if (!box.isEmpty())
        setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
                  QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
    update();
    emit dataChanged();
}

} // namespace aerocad::app
