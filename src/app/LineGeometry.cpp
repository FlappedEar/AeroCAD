// SPDX-License-Identifier: GPL-3.0-or-later
#include "LineGeometry.hpp"

#include "AppController.hpp"

#include <QByteArray>
#include <QVector3D>

namespace aerocad::app {

LineGeometry::LineGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent) {}

void LineGeometry::setController(AppController* controller)
{
    if (m_controller == controller)
        return;
    if (m_connection)
        disconnect(m_connection);
    m_controller = controller;
    if (m_controller) {
        m_connection = connect(m_controller, &AppController::partChanged, this, [this](quint64 id) {
            if (id == m_partId)
                rebuild();
        });
    }
    m_builtRevision = ~0ULL;
    rebuild();
    emit controllerChanged();
}

void LineGeometry::setPartId(quint64 id)
{
    if (m_partId == id)
        return;
    m_partId = id;
    m_builtRevision = ~0ULL;
    rebuild();
    emit partIdChanged();
}

void LineGeometry::rebuild()
{
    const model::Part* part = m_controller ? m_controller->part(m_partId) : nullptr;
    if (part != nullptr && part->revision == m_builtRevision)
        return;

    clear();
    const bool empty = part == nullptr || part->edgeSegments.empty();
    setStride(3 * static_cast<int>(sizeof(float)));
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Lines);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    if (empty) {
        m_builtRevision = ~0ULL;
    } else {
        const auto& segs = part->edgeSegments;
        QByteArray buffer(static_cast<qsizetype>(segs.size()) * 3 * static_cast<qsizetype>(sizeof(float)),
                          Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(buffer.data());
        core::BoundingBox box;
        for (const auto& p : segs) {
            *out++ = static_cast<float>(p.x);
            *out++ = static_cast<float>(p.y);
            *out++ = static_cast<float>(p.z);
            box.extend(p);
        }
        setVertexData(buffer);
        setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
                  QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
        m_builtRevision = part->revision;
    }
    update();
    if (empty != m_empty) {
        m_empty = empty;
        emit emptyChanged();
    }
}

} // namespace aerocad::app
