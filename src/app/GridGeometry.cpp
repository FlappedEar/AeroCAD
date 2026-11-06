// SPDX-License-Identifier: GPL-3.0-or-later
#include "GridGeometry.hpp"

#include <QByteArray>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <vector>

namespace aerocad::app {

GridGeometry::GridGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent)
{
    rebuild();
}

void GridGeometry::setStep(double step)
{
    if (!(step > 0.0) || step == m_step)
        return;
    m_step = step;
    rebuild();
    emit changed();
}

void GridGeometry::setExtent(double extent)
{
    if (!(extent > 0.0) || extent == m_extent)
        return;
    m_extent = extent;
    rebuild();
    emit changed();
}

void GridGeometry::setMajorEvery(int n)
{
    if (n < 1 || n == m_majorEvery)
        return;
    m_majorEvery = n;
    rebuild();
    emit changed();
}

void GridGeometry::setMajor(bool major)
{
    if (major == m_major)
        return;
    m_major = major;
    rebuild();
    emit changed();
}

void GridGeometry::rebuild()
{
    // Cap the line count so a tiny step on a large extent cannot explode the buffer.
    const int half = std::min(static_cast<int>(std::floor(m_extent / m_step)), 500);
    const double e = half * m_step;
    std::vector<float> data;
    data.reserve(static_cast<std::size_t>(2 * half + 1) * 12);
    for (int i = -half; i <= half; ++i) {
        const bool isMajor = m_majorEvery > 1 && i % m_majorEvery == 0;
        if (m_majorEvery > 1 && isMajor != m_major)
            continue;
        const auto t = static_cast<float>(i * m_step);
        const auto ef = static_cast<float>(e);
        data.insert(data.end(), {t, -ef, 0.0F, t, ef, 0.0F, -ef, t, 0.0F, ef, t, 0.0F});
    }

    clear();
    setStride(3 * static_cast<int>(sizeof(float)));
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Lines);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    setVertexData(QByteArray(reinterpret_cast<const char*>(data.data()),
                             static_cast<qsizetype>(data.size() * sizeof(float))));
    const auto ef = static_cast<float>(e);
    setBounds(QVector3D(-ef, -ef, 0.0F), QVector3D(ef, ef, 0.0F));
    update();
}

} // namespace aerocad::app
