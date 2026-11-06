// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QQuick3DGeometry>
#include <QtQml/qqmlregistration.h>

namespace aerocad::app {

class AppController;

/// Line segments of one document part (sketch curves, body edges) for the viewport.
/// Rebuilds itself when the part changes.
class LineGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(aerocad::app::AppController* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(quint64 partId READ partId WRITE setPartId NOTIFY partIdChanged)
    Q_PROPERTY(bool empty READ isEmpty NOTIFY emptyChanged)

public:
    explicit LineGeometry(QQuick3DObject* parent = nullptr);

    [[nodiscard]] AppController* controller() const { return m_controller; }
    void setController(AppController* controller);

    [[nodiscard]] quint64 partId() const { return m_partId; }
    void setPartId(quint64 id);

    [[nodiscard]] bool isEmpty() const { return m_empty; }

signals:
    void controllerChanged();
    void partIdChanged();
    void emptyChanged();

private:
    void rebuild();

    QPointer<AppController> m_controller;
    quint64 m_partId = 0;
    quint64 m_builtRevision = ~0ULL;
    bool m_empty = true;
    QMetaObject::Connection m_connection;
};

} // namespace aerocad::app
