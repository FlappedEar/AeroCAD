// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QQuick3DGeometry>
#include <QtQml/qqmlregistration.h>

namespace aerocad::app {

class AppController;

/// GPU geometry of the selected triangles of one part (drawn as a highlight over the
/// part). Shows the live brush preview while a stroke is in progress.
class SelectionGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(aerocad::app::AppController* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(quint64 partId READ partId WRITE setPartId NOTIFY partIdChanged)
    Q_PROPERTY(bool empty READ isEmpty NOTIFY emptyChanged)

public:
    explicit SelectionGeometry(QQuick3DObject* parent = nullptr);

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
    void onPartChanged(quint64 id);

    QPointer<AppController> m_controller;
    quint64 m_partId = 0;
    bool m_empty = true;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace aerocad::app
