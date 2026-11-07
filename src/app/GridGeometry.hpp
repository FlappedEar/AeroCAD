// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QQuick3DGeometry>
#include <QtQml/qqmlregistration.h>

namespace aerocad::app {

/// Ground grid on the model's XY plane (Z = 0) for the viewport: lines every `step`
/// millimetres out to `extent` in each direction. With `majorEvery` > 1 only every n-th
/// line (`major`: true) or only the others (`major`: false) are emitted, so minor and
/// major lines can use different colours.
class GridGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double step READ step WRITE setStep NOTIFY changed)
    Q_PROPERTY(double extent READ extent WRITE setExtent NOTIFY changed)
    Q_PROPERTY(int majorEvery READ majorEvery WRITE setMajorEvery NOTIFY changed)
    Q_PROPERTY(bool major READ major WRITE setMajor NOTIFY changed)

public:
    explicit GridGeometry(QQuick3DObject* parent = nullptr);

    [[nodiscard]] double step() const { return m_step; }
    void setStep(double step);
    [[nodiscard]] double extent() const { return m_extent; }
    void setExtent(double extent);
    [[nodiscard]] int majorEvery() const { return m_majorEvery; }
    void setMajorEvery(int n);
    [[nodiscard]] bool major() const { return m_major; }
    void setMajor(bool major);

signals:
    void changed();

private:
    void rebuild();

    double m_step = 10.0;
    double m_extent = 100.0;
    int m_majorEvery = 5;
    bool m_major = false;
};

} // namespace aerocad::app
