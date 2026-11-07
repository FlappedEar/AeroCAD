// SPDX-License-Identifier: GPL-3.0-or-later
#include "SelectionGeometry.hpp"

#include "AppController.hpp"

#include <QByteArray>
#include <QVector3D>

namespace aerocad::app {

SelectionGeometry::SelectionGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent) {}

void SelectionGeometry::setController(AppController* controller)
{
    if (m_controller == controller)
        return;
    for (const auto& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    m_controller = controller;
    if (m_controller) {
        m_connections << connect(m_controller, &AppController::partChanged, this, &SelectionGeometry::onPartChanged);
        m_connections << connect(m_controller, &AppController::selectionPreviewChanged, this,
                                 &SelectionGeometry::onPartChanged);
    }
    rebuild();
    emit controllerChanged();
}

void SelectionGeometry::setPartId(quint64 id)
{
    if (m_partId == id)
        return;
    m_partId = id;
    rebuild();
    emit partIdChanged();
}

void SelectionGeometry::onPartChanged(quint64 id)
{
    if (id == m_partId)
        rebuild();
}

void SelectionGeometry::rebuild()
{
    clear();
    // A valid layout even without data: Qt divides by the stride.
    constexpr int floatsPerVertex = 6; // position + normal
    setStride(floatsPerVertex * static_cast<int>(sizeof(float)));
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::NormalSemantic, 3 * static_cast<int>(sizeof(float)),
                 QQuick3DGeometry::Attribute::F32Type);
    const model::Part* part = m_controller ? m_controller->part(m_partId) : nullptr;
    const core::TriangleSet* selection = m_controller ? m_controller->displayedSelection(m_partId) : nullptr;
    const bool empty = part == nullptr || !part->mesh || selection == nullptr || selection->empty();
    if (empty) {
        update();
        if (!m_empty) {
            m_empty = true;
            emit emptyChanged();
        }
        return;
    }

    // Each selected triangle is drawn twice, lifted slightly off both sides of the
    // surface, so the highlight shows from either side and with inconsistent winding.
    const core::TriangleMesh& mesh = *part->mesh;
    const core::BoundingBox box = mesh.bounds();
    const double lift = std::max(box.diagonal() * 4e-4, 1e-6);
    QByteArray buffer(static_cast<qsizetype>(selection->size()) * 2 * 3 * floatsPerVertex *
                          static_cast<qsizetype>(sizeof(float)),
                      Qt::Uninitialized);
    auto* out = reinterpret_cast<float*>(buffer.data());
    qsizetype written = 0;
    const auto& vertices = mesh.vertices();
    for (const auto t : *selection) {
        if (t >= mesh.triangleCount())
            continue;
        const core::Vec3 n = mesh.faceNormal(t);
        for (const double side : {1.0, -1.0}) {
            const core::Vec3 offset = n * (lift * side);
            const core::Vec3 sn = n * side;
            for (const std::uint32_t vi : mesh.triangles()[t]) {
                const core::Vec3 p = vertices[vi] + offset;
                *out++ = static_cast<float>(p.x);
                *out++ = static_cast<float>(p.y);
                *out++ = static_cast<float>(p.z);
                *out++ = static_cast<float>(sn.x);
                *out++ = static_cast<float>(sn.y);
                *out++ = static_cast<float>(sn.z);
            }
            ++written;
        }
    }
    buffer.resize(written * 3 * floatsPerVertex * static_cast<qsizetype>(sizeof(float)));
    setVertexData(buffer);
    setBounds(QVector3D(static_cast<float>(box.min.x - lift), static_cast<float>(box.min.y - lift),
                        static_cast<float>(box.min.z - lift)),
              QVector3D(static_cast<float>(box.max.x + lift), static_cast<float>(box.max.y + lift),
                        static_cast<float>(box.max.z + lift)));
    update();
    if (m_empty) {
        m_empty = false;
        emit emptyChanged();
    }
}

} // namespace aerocad::app
