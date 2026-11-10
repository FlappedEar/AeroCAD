// SPDX-License-Identifier: GPL-3.0-or-later
#include "MeshGeometry.hpp"

#include "AppController.hpp"
#include "aerocad/core/Align.hpp"
#include "aerocad/core/MeshSimplify.hpp"
#include "ScanController.hpp"

#include <QByteArray>
#include <QVector3D>

#include <cmath>
#include <algorithm>
#include <thread>
#include <cstring>
#include <limits>
#include <vector>

static_assert(sizeof(aerocad::core::TriangleMesh::Triangle) == 3 * sizeof(quint32));

namespace aerocad::app {

namespace {

/// Runs work(first, end) over [0, count) on up to 8 threads (this one included).
template <typename Work>
void parallelFor(qsizetype count, const Work& work)
{
    constexpr qsizetype kMinPerThread = 20000;
    const qsizetype chunks = std::clamp<qsizetype>(static_cast<qsizetype>(std::thread::hardware_concurrency()), 1,
                                                   std::min<qsizetype>(8, std::max<qsizetype>(1, count / kMinPerThread)));
    std::vector<std::thread> threads;
    for (qsizetype c = 1; c < chunks; ++c)
        threads.emplace_back([&work, count, c, chunks] { work(count * c / chunks, count * (c + 1) / chunks); });
    work(0, count / chunks);
    for (auto& th : threads)
        th.join();
}

} // namespace

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

bool MeshGeometry::moveBuilt(const std::shared_ptr<const core::TriangleMesh>& display)
{
    const auto tf = core::affineBetween(*m_builtMesh, *display);
    if (!tf)
        return false;
    const QByteArray old = vertexData();
    constexpr int floatsPerVertex = 6;
    const qsizetype vertexCount = old.size() / static_cast<qsizetype>(floatsPerVertex * sizeof(float));
    if (vertexCount != static_cast<qsizetype>(display->triangleCount()) * 3)
        return false; // not the per-corner layout this mesh was built with
    // Normals turn with the inverse transpose; the cofactor matrix is that times the
    // determinant, so its sign keeps the normals on their side when the transform mirrors.
    const core::Mat3& l = tf->linear;
    const double det = l.determinant();
    if (det == 0.0 || !std::isfinite(det))
        return false;
    const double sign = det > 0.0 ? 1.0 : -1.0;
    const core::Mat3 cof = core::Mat3::fromRows(l.row(1).cross(l.row(2)) * sign, l.row(2).cross(l.row(0)) * sign,
                                                l.row(0).cross(l.row(1)) * sign);
    float nm[9];
    for (int i = 0; i < 9; ++i)
        nm[i] = static_cast<float>(cof.m[static_cast<std::size_t>(i)]);

    // A new buffer, written from the old one: the old one is still shared with the renderer,
    // so writing it in place would first copy tens of MB.
    QByteArray buffer(old.size(), Qt::Uninitialized);
    const auto* src = reinterpret_cast<const float*>(old.constData());
    auto* dst = reinterpret_cast<float*>(buffer.data());
    const bool mirrored = det < 0.0;
    const auto& vertices = display->vertices();
    const auto& tris = display->triangles();
    // Positions come from the mesh itself (doubles): transforming the uploaded floats would
    // keep their rounding, e.g. detail lost far from the origin would stay lost after a
    // move back. Only the normals are carried over. transformMesh reverses the winding of a
    // mirrored mesh, (a, b, c) -> (a, c, b): corner k then had its normal at (3 - k) % 3.
    auto work = [&](qsizetype firstTriangle, qsizetype endTriangle) {
        for (qsizetype t = firstTriangle; t < endTriangle; ++t) {
            const auto& tri = tris[static_cast<std::size_t>(t)];
            for (qsizetype k = 0; k < 3; ++k) {
                const qsizetype from = t * 3 + (mirrored ? (3 - k) % 3 : k);
                const float* v = src + from * floatsPerVertex;
                float* o = dst + (t * 3 + k) * floatsPerVertex;
                const core::Vec3& p = vertices[tri[static_cast<std::size_t>(k)]];
                o[0] = static_cast<float>(p.x);
                o[1] = static_cast<float>(p.y);
                o[2] = static_cast<float>(p.z);
                const float nx = nm[0] * v[3] + nm[1] * v[4] + nm[2] * v[5];
                const float ny = nm[3] * v[3] + nm[4] * v[4] + nm[5] * v[5];
                const float nz = nm[6] * v[3] + nm[7] * v[4] + nm[8] * v[5];
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                const float inv = len > 0.0f ? 1.0f / len : 0.0f;
                o[3] = nx * inv;
                o[4] = ny * inv;
                o[5] = nz * inv;
            }
        }
    };
    parallelFor(vertexCount / 3, work);
    setVertexData(buffer);
    const core::BoundingBox box = display->bounds();
    setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
              QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
    m_builtMesh = display;
    update();
    return true;
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

    // A big scan moved, turned or scaled (or its reduced mesh moved along with it): move the
    // uploaded corners instead of building them again; smoothing the normals of a
    // million-triangle mesh takes most of a 0.1 s.
    if (part != nullptr && display && !part->shape && !m_builtWithShape && m_builtMesh &&
        display->triangleCount() >= kMoveInPlaceTriangles && moveBuilt(display))
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
        const qsizetype vertexCount = static_cast<qsizetype>(mesh.triangleCount()) * 3;
        // Normally computed with the reduced mesh on its worker thread.
        std::shared_ptr<const std::vector<float>> given =
            ScanController::instance() != nullptr ? ScanController::instance()->takeDisplayNormals(display) : nullptr;
        if (!given || static_cast<qsizetype>(given->size()) != vertexCount * 3) {
            const std::vector<core::Vec3> smooth = core::smoothCornerNormals(mesh);
            auto packed = std::make_shared<std::vector<float>>(smooth.size() * 3);
            for (std::size_t i = 0; i < smooth.size(); ++i) {
                (*packed)[3 * i] = static_cast<float>(smooth[i].x);
                (*packed)[3 * i + 1] = static_cast<float>(smooth[i].y);
                (*packed)[3 * i + 2] = static_cast<float>(smooth[i].z);
            }
            given = std::move(packed);
        }
        QByteArray buffer(vertexCount * floatsPerVertex * static_cast<qsizetype>(sizeof(float)), Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(buffer.data());
        const float* normals = given->data();
        const auto& tris = mesh.triangles();
        parallelFor(static_cast<qsizetype>(tris.size()), [&](qsizetype first, qsizetype end) {
            for (qsizetype t = first; t < end; ++t) {
                for (qsizetype k = 0; k < 3; ++k) {
                    const core::Vec3& v = vertices[tris[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)]];
                    float* o = out + (t * 3 + k) * floatsPerVertex;
                    o[0] = static_cast<float>(v.x);
                    o[1] = static_cast<float>(v.y);
                    o[2] = static_cast<float>(v.z);
                    const float* n = normals + (t * 3 + k) * 3;
                    o[3] = n[0];
                    o[4] = n[1];
                    o[5] = n[2];
                }
            }
        });
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
        const auto& tris = mesh.triangles();
        parallelFor(static_cast<qsizetype>(tris.size()), [&](qsizetype first, qsizetype end) {
            float* o = out + first * 3 * floatsPerVertex;
            for (qsizetype t = first; t < end; ++t) {
                const core::Vec3 n = mesh.faceNormal(static_cast<std::size_t>(t));
                for (const std::uint32_t vi : tris[static_cast<std::size_t>(t)])
                    put(o, vertices[vi], n);
            }
        });
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
