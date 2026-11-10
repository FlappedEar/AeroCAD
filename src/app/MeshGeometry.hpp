// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <QPointer>
#include <QQuick3DGeometry>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace aerocad::app {

class AppController;

/// GPU geometry for one document part. Rebuilds itself when the part changes.
/// Meshes are drawn flat shaded (facets visible); tessellated B-rep parts smooth shaded.
class MeshGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(aerocad::app::AppController* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(quint64 partId READ partId WRITE setPartId NOTIFY partIdChanged)

public:
    explicit MeshGeometry(QQuick3DObject* parent = nullptr);

    [[nodiscard]] AppController* controller() const { return m_controller; }
    void setController(AppController* controller);

    [[nodiscard]] quint64 partId() const { return m_partId; }
    void setPartId(quint64 id);

signals:
    void controllerChanged();
    void partIdChanged();

private:
    void rebuild();
    /// Moves the uploaded corners when `display` is the built mesh after one affine
    /// transform (a moved, turned or scaled scan). False when it is not; nothing changes then.
    bool moveBuilt(const std::shared_ptr<const core::TriangleMesh>& display);
    /// Smaller meshes are simply built again.
    static constexpr std::size_t kMoveInPlaceTriangles = 50000;
    void onPartChanged(quint64 id);

    QPointer<AppController> m_controller;
    quint64 m_partId = 0;
    /// Held (not just its address) so a new mesh can never alias a freed one.
    std::shared_ptr<const core::TriangleMesh> m_builtMesh;
    bool m_builtWithShape = false;
    QMetaObject::Connection m_connection;
    QMetaObject::Connection m_lodConnection;
};

} // namespace aerocad::app
