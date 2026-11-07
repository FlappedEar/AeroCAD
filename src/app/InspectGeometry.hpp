// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <QPointer>
#include <QQuick3DGeometry>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>

namespace aerocad::app {

class AppController;

/// GPU geometry for the Inspect analyses of one part (see commands/Inspect.hpp):
/// - Clipped: the part cut by the section plane (the removed half is dropped).
/// - Cap: the filled section (closed section loops), facing the removed half.
/// - Outline: the section curves as lines.
/// - HeatMap: the part with per-vertex colours from the active heat map.
/// Rebuilds when the part or the analyses change; `empty` tells QML whether to draw it.
class InspectGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(aerocad::app::AppController* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(quint64 partId READ partId WRITE setPartId NOTIFY partIdChanged)
    Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool empty READ isEmpty NOTIFY emptyChanged)

public:
    enum Mode { Clipped, Cap, Outline, HeatMap };
    Q_ENUM(Mode)

    explicit InspectGeometry(QQuick3DObject* parent = nullptr);

    [[nodiscard]] AppController* controller() const { return m_controller; }
    void setController(AppController* controller);
    [[nodiscard]] quint64 partId() const { return m_partId; }
    void setPartId(quint64 id);
    [[nodiscard]] Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    [[nodiscard]] bool isEmpty() const { return m_empty; }

    /// Heat map colour for value t in [0, 1] (blue - cyan - green - yellow - red).
    static void heatColor(double t, float rgba[4]);

signals:
    void controllerChanged();
    void partIdChanged();
    void modeChanged();
    void emptyChanged();

private:
    void rebuild();
    void setTriangles(const core::TriangleMesh& mesh, bool smooth, const std::vector<double>* values, double lo,
                      double hi, bool deviation);
    void setLines(const std::vector<core::Vec3>& segments);
    void setEmptyGeometry();
    void setEmpty(bool empty);

    QPointer<AppController> m_controller;
    quint64 m_partId = 0;
    Mode m_mode = Clipped;
    bool m_empty = true;
    // What the current buffers were built from.
    std::shared_ptr<const core::TriangleMesh> m_builtMesh;
    std::uint64_t m_builtInspectRevision = ~0ULL;
    bool m_built = false;
    QMetaObject::Connection m_partConnection;
    QMetaObject::Connection m_inspectConnection;
};

} // namespace aerocad::app
