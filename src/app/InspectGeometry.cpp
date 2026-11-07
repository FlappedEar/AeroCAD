// SPDX-License-Identifier: GPL-3.0-or-later
#include "InspectGeometry.hpp"

#include "AppController.hpp"

#include "aerocad/core/MeshQuery.hpp"
#include "aerocad/core/MeshSection.hpp"
#include "aerocad/geometry/Inspect.hpp"

#include <QByteArray>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace aerocad::app {

namespace {

constexpr int kFloats = 10; // position, normal, colour

void put(float*& out, const core::Vec3& p, const core::Vec3& n, const float c[4])
{
    *out++ = static_cast<float>(p.x);
    *out++ = static_cast<float>(p.y);
    *out++ = static_cast<float>(p.z);
    *out++ = static_cast<float>(n.x);
    *out++ = static_cast<float>(n.y);
    *out++ = static_cast<float>(n.z);
    for (int i = 0; i < 4; ++i)
        *out++ = c[i];
}

} // namespace

InspectGeometry::InspectGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent)
{
    setEmptyGeometry();
}

void InspectGeometry::setController(AppController* controller)
{
    if (m_controller == controller)
        return;
    if (m_partConnection)
        disconnect(m_partConnection);
    if (m_inspectConnection)
        disconnect(m_inspectConnection);
    m_controller = controller;
    if (m_controller) {
        m_partConnection = connect(m_controller, &AppController::partChanged, this, [this](quint64 id) {
            if (id == m_partId)
                rebuild();
        });
        m_inspectConnection = connect(m_controller, &AppController::inspectChanged, this, &InspectGeometry::rebuild);
    }
    m_built = false;
    rebuild();
    emit controllerChanged();
}

void InspectGeometry::setPartId(quint64 id)
{
    if (m_partId == id)
        return;
    m_partId = id;
    m_built = false;
    rebuild();
    emit partIdChanged();
}

void InspectGeometry::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_built = false;
    rebuild();
    emit modeChanged();
}

void InspectGeometry::heatColor(double t, float rgba[4])
{
    // Blue (low) -> cyan -> green -> yellow -> red (high), as in common deviation maps.
    static constexpr std::array<std::array<float, 3>, 5> stops{{{0.13F, 0.29F, 0.85F},
                                                                 {0.10F, 0.75F, 0.90F},
                                                                 {0.20F, 0.80F, 0.25F},
                                                                 {0.98F, 0.85F, 0.15F},
                                                                 {0.88F, 0.18F, 0.15F}}};
    t = std::clamp(t, 0.0, 1.0) * 4.0;
    const auto i = std::min<std::size_t>(3, static_cast<std::size_t>(t));
    const auto f = static_cast<float>(t - static_cast<double>(i));
    for (std::size_t k = 0; k < 3; ++k)
        rgba[k] = stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f;
    rgba[3] = 1.0F;
}

void InspectGeometry::setEmpty(bool empty)
{
    if (m_empty == empty)
        return;
    m_empty = empty;
    emit emptyChanged();
}

void InspectGeometry::setEmptyGeometry()
{
    clear();
    // Keep a valid layout: Qt Quick 3D rejects a zero stride.
    setStride(kFloats * static_cast<int>(sizeof(float)));
    setPrimitiveType(m_mode == Outline ? PrimitiveType::Lines : PrimitiveType::Triangles);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    update();
    setEmpty(true);
}

void InspectGeometry::rebuild()
{
    const model::Part* part = m_controller ? m_controller->part(m_partId) : nullptr;
    const commands::InspectState* inspect = m_controller ? &m_controller->session().inspect() : nullptr;
    if (part == nullptr || inspect == nullptr || !part->mesh || part->mesh->isEmpty()) {
        m_built = false;
        setEmptyGeometry();
        return;
    }
    // Section geometry depends on the section only, the heat map on the heat map only; both
    // are compared through the state revision plus the mesh, which is cheap and safe.
    if (m_built && m_builtMesh == part->mesh && m_builtInspectRevision == inspect->revision())
        return;
    m_built = true;
    m_builtMesh = part->mesh;
    m_builtInspectRevision = inspect->revision();

    const core::TriangleMesh& mesh = *part->mesh;
    const bool smooth = part->shape.has_value();

    if (m_mode == HeatMap) {
        const commands::HeatMap& h = inspect->heatMap();
        if (!h.enabled || h.part != m_partId || h.mesh != part->mesh || !h.values ||
            h.values->size() != mesh.vertexCount()) {
            setEmptyGeometry();
            return;
        }
        setTriangles(mesh, smooth, h.values.get(), h.rangeMin, h.rangeMax, h.kind == commands::HeatMap::Kind::Deviation);
        return;
    }

    const commands::SectionAnalysis& s = inspect->section();
    if (!s.cuts(m_partId) || part->kind == model::PartKind::Sketch) {
        setEmptyGeometry();
        return;
    }
    if (m_mode == Clipped) {
        const core::TriangleMesh clipped = core::clipMesh(mesh, s.plane);
        if (clipped.isEmpty())
            setEmptyGeometry();
        else
            setTriangles(clipped, smooth, nullptr, 0, 0, false);
        return;
    }
    const auto curves = core::sectionCurves(mesh, s.plane);
    if (m_mode == Outline) {
        std::vector<core::Vec3> segments;
        for (const auto& c : curves) {
            for (std::size_t i = 1; i < c.points.size(); ++i) {
                segments.push_back(c.points[i - 1]);
                segments.push_back(c.points[i]);
            }
            if (c.closed && c.points.size() > 2) {
                segments.push_back(c.points.back());
                segments.push_back(c.points.front());
            }
        }
        setLines(segments);
        return;
    }
    // Cap.
    core::TriangleMesh cap;
    if (s.cap) {
        // Drawn while rendering: a kernel failure must only drop the cap, never end the app.
        try {
            cap = geometry::triangulateRegion(curves, s.plane);
        } catch (...) {
            cap = core::TriangleMesh{};
        }
    }
    if (cap.isEmpty())
        setEmptyGeometry();
    else
        setTriangles(cap, false, nullptr, 0, 0, false);
}

void InspectGeometry::setTriangles(const core::TriangleMesh& mesh, bool smooth, const std::vector<double>* values,
                                   double lo, double hi, bool deviation)
{
    clear();
    const auto& verts = mesh.vertices();
    static constexpr float white[4] = {1, 1, 1, 1};
    static constexpr float noValue[4] = {0.62F, 0.64F, 0.67F, 1};
    auto colorOf = [&](std::uint32_t v, float out[4]) {
        if (values == nullptr) {
            std::memcpy(out, white, sizeof(white));
            return;
        }
        const double x = (*values)[v];
        if (!std::isfinite(x)) {
            std::memcpy(out, noValue, sizeof(noValue));
            return;
        }
        // Thickness: thin is the problem, so low values are red.
        const double t = hi > lo ? (x - lo) / (hi - lo) : 0.5;
        heatColor(deviation ? t : 1.0 - t, out);
    };

    float c[4];
    if (smooth) {
        const std::vector<core::Vec3> normals = core::vertexNormals(mesh);
        QByteArray vb(static_cast<qsizetype>(verts.size()) * kFloats * static_cast<qsizetype>(sizeof(float)),
                      Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(vb.data());
        for (std::size_t i = 0; i < verts.size(); ++i) {
            colorOf(static_cast<std::uint32_t>(i), c);
            put(out, verts[i], normals[i], c);
        }
        QByteArray ib(static_cast<qsizetype>(mesh.triangleCount()) * 3 * static_cast<qsizetype>(sizeof(quint32)),
                      Qt::Uninitialized);
        std::memcpy(ib.data(), mesh.triangles().data(), static_cast<std::size_t>(ib.size()));
        setVertexData(vb);
        setIndexData(ib);
        addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
    } else {
        QByteArray vb(static_cast<qsizetype>(mesh.triangleCount()) * 3 * kFloats * static_cast<qsizetype>(sizeof(float)),
                      Qt::Uninitialized);
        auto* out = reinterpret_cast<float*>(vb.data());
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            const core::Vec3 n = mesh.faceNormal(t);
            for (const std::uint32_t v : mesh.triangles()[t]) {
                colorOf(v, c);
                put(out, verts[v], n, c);
            }
        }
        setVertexData(vb);
    }
    const core::BoundingBox box = mesh.bounds();
    setStride(kFloats * static_cast<int>(sizeof(float)));
    setPrimitiveType(PrimitiveType::Triangles);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    addAttribute(Attribute::NormalSemantic, 3 * static_cast<int>(sizeof(float)), Attribute::F32Type);
    addAttribute(Attribute::ColorSemantic, 6 * static_cast<int>(sizeof(float)), Attribute::F32Type);
    setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
              QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
    update();
    setEmpty(false);
}

void InspectGeometry::setLines(const std::vector<core::Vec3>& segments)
{
    if (segments.empty()) {
        setEmptyGeometry();
        return;
    }
    clear();
    static constexpr float white[4] = {1, 1, 1, 1};
    QByteArray vb(static_cast<qsizetype>(segments.size()) * kFloats * static_cast<qsizetype>(sizeof(float)),
                  Qt::Uninitialized);
    auto* out = reinterpret_cast<float*>(vb.data());
    core::BoundingBox box;
    for (const auto& p : segments) {
        put(out, p, {0, 0, 1}, white);
        box.extend(p);
    }
    setVertexData(vb);
    setStride(kFloats * static_cast<int>(sizeof(float)));
    setPrimitiveType(PrimitiveType::Lines);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    setBounds(QVector3D(static_cast<float>(box.min.x), static_cast<float>(box.min.y), static_cast<float>(box.min.z)),
              QVector3D(static_cast<float>(box.max.x), static_cast<float>(box.max.y), static_cast<float>(box.max.z)));
    update();
    setEmpty(false);
}

} // namespace aerocad::app
