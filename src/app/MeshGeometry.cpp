// SPDX-License-Identifier: GPL-3.0-or-later
#include "MeshGeometry.hpp"

#include "AppController.hpp"
#include "aerocad/core/MeshSimplify.hpp"
#include "ScanController.hpp"

#include <QByteArray>
#include <QVector3D>

#include <cstring>
#include <limits>
#include <vector>

static_assert(sizeof(aerocad::core::TriangleMesh::Triangle) == 3 * sizeof(quint32));

namespace aerocad::app {

MeshGeometry::MeshGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent) {}

void MeshGeometry::setController(AppController* controller)
{
    if (m_controller == controller)
        return;
    if (m_connection)
        disconnect(m_connection);
    m_controller = controller;
    if (m_controller)
        m_connection = connect(m_controller, &AppController::partChanged, this, &MeshGeometry::onPartChanged);
    if (!m_lodConnection && ScanController::instance() != nullptr)
        m_lodConnection = connect(ScanController::instance(), &ScanController::displayMeshReady, this,
                                  &MeshGeometry::onPartChanged);
    m_builtMesh = nullptr;
    rebuild();
    emit controllerChanged();
}

void MeshGeometry::setPartId(quint64 id)
{
    if (m_partId == id)
        return;
    m_partId = id;
    m_builtMesh = nullptr;
    rebuild();
    emit partIdChanged();
}

void MeshGeometry::onPartChanged(quint64 id)
{
    if (id == m_partId)
        rebuild();
}

void MeshGeometry::rebuild()
{
    const model::Part* part = m_controller ? m_controller->part(m_partId) : nullptr;
    // Big meshes are drawn at a reduced level of detail (ScanController); the document
    // keeps the full mesh. nullptr while the reduced mesh is being computed.
    std::shared_ptr<const core::TriangleMesh> display;
    if (part != nullptr)
        display = ScanController::instance() != nullptr ? ScanController::instance()->displayMesh(*part) : part->mesh;
    // Only the drawn mesh (and whether it is a B-rep tessellation) affects the GPU data;
    // other part changes such as the selection or the name do not need a re-upload.
    if (part != nullptr && display == m_builtMesh && part->shape.has_value() == m_builtWithShape)
        return;

    clear();
    if (part == nullptr || !display || display->isEmpty()) {
        // Keep a valid (empty) layout: Qt Quick 3D rejects a zero stride, and sketches
        // without closed profiles have no triangles.
        setStride(6 * static_cast<int>(sizeof(float)));
        setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
        addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
        m_builtMesh = nullptr;
        update();
        return;
    }

    // Model coordinates are Z-up millimetres; the scene converts Z-up to Qt's Y-up with
    // a parent node rotation, so positions are uploaded unchanged.
    const core::TriangleMesh& mesh = *display;
    constexpr int floatsPerVertex = 6; // position + normal
    const auto& vertices = mesh.vertices();
    auto put = [](float*& out, const core::Vec3& v, const core::Vec3& n) {
        *out++ = static_cast<float>(v.x);
        *out++ = static_cast<float>(v.y);
        *out++ = static_cast<float>(v.z);
        *out++ = static_cast<float>(n.x);
        *out++ = static_cast<float>(n.y);
        *out++ = static_cast<float>(n.z);
    };

    // A scan drawn simplified (display level of detail) is drawn smooth, see below.
    const bool simplified = !part->shape && display != part->mesh;
    if (simplified) {
        // Its vertices are averages over the cells of a grid, so flat normals show bands
        // where the surface crosses a grid plane (lines that are not on the scan); averaged
        // normals draw it as the full scan looks (core::smoothCornerNormals).
        const std::vector<core::Vec3> normals = core::smoothCornerNormals(mesh);
        const qsizetype vertexCount = static_cast<qsizetype>(mesh.triangleCount()) * 3;
        QByteArray buffer(vertexCount * floatsPerVertex * static_cast<qsizetype>(sizeof(float)), Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(buffer.data());
        std::size_t corner = 0;
        for (const auto& tri : mesh.triangles())
            for (const std::uint32_t vi : tri)
                put(out, vertices[vi], normals[corner++]);
        setVertexData(buffer);
    } else if (part->shape) {
        // Tessellated B-rep: vertices are shared only within a face, so averaged vertex
        // normals give smooth curved faces and keep edges between faces sharp.
        std::vector<core::Vec3> normals(vertices.size());
        for (const auto& t : mesh.triangles()) {
            const core::Vec3 n = (vertices[t[1]] - vertices[t[0]]).cross(vertices[t[2]] - vertices[t[0]]);
            for (const std::uint32_t vi : t)
                normals[vi] = normals[vi] + n; // area weighted
        }
        QByteArray vertexBuffer(static_cast<qsizetype>(vertices.size()) * floatsPerVertex *
                                    static_cast<qsizetype>(sizeof(float)),
                                Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(vertexBuffer.data());
        for (std::size_t i = 0; i < vertices.size(); ++i)
            put(out, vertices[i], normals[i].normalized());

        QByteArray indexBuffer(static_cast<qsizetype>(mesh.triangleCount()) * 3 *
                                   static_cast<qsizetype>(sizeof(quint32)),
                               Qt::Uninitialized);
        std::memcpy(indexBuffer.data(), mesh.triangles().data(), static_cast<std::size_t>(indexBuffer.size()));
        setVertexData(vertexBuffer);
        setIndexData(indexBuffer);
        addAttribute(QQuick3DGeometry::Attribute::IndexSemantic, 0, QQuick3DGeometry::Attribute::U32Type);
    } else {
        // Plain mesh (STL, OBJ, 3MF, scans): flat normal per triangle shows the facets.
        const qsizetype vertexCount = static_cast<qsizetype>(mesh.triangleCount()) * 3;
        QByteArray buffer(vertexCount * floatsPerVertex * static_cast<qsizetype>(sizeof(float)), Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(buffer.data());
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            const core::Vec3 n = mesh.faceNormal(t);
            for (const std::uint32_t vi : mesh.triangles()[t])
                put(out, vertices[vi], n);
        }
        setVertexData(buffer);
    }

    const core::BoundingBox box = mesh.bounds();
    setStride(floatsPerVertex * static_cast<int>(sizeof(float)));
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, 0, QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::NormalSemantic, 3 * static_cast<int>(sizeof(float)),
                 QQuick3DGeometry::Attribute::F32Type);
    setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
              QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
    m_builtMesh = display;
    m_builtWithShape = part->shape.has_value();
    update();
}

} // namespace aerocad::app
