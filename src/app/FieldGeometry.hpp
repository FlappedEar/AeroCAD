// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/cfd/Results.hpp"

#include <QColor>
#include <QPointer>
#include <QQuick3DGeometry>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace aerocad::app {

class AppController;

/// A simulation result field in the viewport, coloured by a scalar: pressure on the part
/// ("surface"), a velocity section, or streamlines. Positions are model coordinates (mm);
/// colours are baked into the vertices. The surface has smooth normals and streamlines are
/// built as tubes with normals, so both can be drawn lit and read as 3D shapes; sections
/// are flat (draw unlit so their colours match the legend exactly).
class FieldGeometry : public QQuick3DGeometry {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(aerocad::app::AppController* controller READ controller WRITE setController NOTIFY changed)
    Q_PROPERTY(QString run READ run WRITE setRun NOTIFY changed)
    /// surface, sectionHorizontal, sectionVertical, streamlines.
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY changed)
    /// Scalar to colour by: Cp, pressure, p, speed.
    Q_PROPERTY(QString scalar READ scalar WRITE setScalar NOTIFY changed)
    /// Colour scale. When rangeMin >= rangeMax the data range is used.
    Q_PROPERTY(double rangeMin READ rangeMin WRITE setRangeMin NOTIFY changed)
    Q_PROPERTY(double rangeMax READ rangeMax WRITE setRangeMax NOTIFY changed)
    /// Streamline tube radius in mm; 0 = 0.4 % of the streamlines' extent.
    Q_PROPERTY(double tubeRadius READ tubeRadius WRITE setTubeRadius NOTIFY changed)
    /// Bump to reload the files (e.g. when the run finishes).
    Q_PROPERTY(int revision READ revision WRITE setRevision NOTIFY changed)
    Q_PROPERTY(double dataMin READ dataMin NOTIFY dataChanged)
    Q_PROPERTY(double dataMax READ dataMax NOTIFY dataChanged)
    Q_PROPERTY(bool empty READ isEmpty NOTIFY dataChanged)
    /// Colour map stops from low to high (for legends), matching the vertex colours.
    Q_PROPERTY(QVariantList colormap READ colormap NOTIFY changed)

public:
    explicit FieldGeometry(QQuick3DObject* parent = nullptr);

    [[nodiscard]] AppController* controller() const { return m_controller; }
    void setController(AppController* controller);
    [[nodiscard]] QString run() const { return m_run; }
    void setRun(const QString& run);
    [[nodiscard]] QString kind() const { return m_kind; }
    void setKind(const QString& kind);
    [[nodiscard]] QString scalar() const { return m_scalar; }
    void setScalar(const QString& scalar);
    [[nodiscard]] double rangeMin() const { return m_rangeMin; }
    void setRangeMin(double v);
    [[nodiscard]] double rangeMax() const { return m_rangeMax; }
    void setRangeMax(double v);
    [[nodiscard]] double tubeRadius() const { return m_tubeRadius; }
    void setTubeRadius(double r);
    [[nodiscard]] int revision() const { return m_revision; }
    void setRevision(int revision);
    [[nodiscard]] double dataMin() const { return m_dataMin; }
    [[nodiscard]] double dataMax() const { return m_dataMax; }
    [[nodiscard]] bool isEmpty() const { return m_data.empty(); }
    [[nodiscard]] QVariantList colormap() const;

    /// Colour of t in [0, 1] on the map used for `scalar` (diverging for pressure,
    /// sequential for speed).
    [[nodiscard]] static QColor mapColor(const QString& scalar, double t);
    /// Position of `value` on the colour scale [lo, hi] as t in [0, 1]. Pressure scalars
    /// (Cp, pressure, p) whose range spans zero keep zero, the free-stream pressure, at the
    /// neutral middle of the diverging map; each side is scaled on its own (CAD-127).
    [[nodiscard]] static double scalePosition(const QString& scalar, double value, double lo, double hi);
    /// Number of floats per vertex: position, normal, RGBA.
    static constexpr int kFloatsPerVertex = 10;

signals:
    void changed();
    void dataChanged();

private:
    void reload();
    void rebuild();
    void schedule(bool reloadFiles);

    QPointer<AppController> m_controller;
    QString m_run;
    QString m_kind = QStringLiteral("surface");
    QString m_scalar = QStringLiteral("Cp");
    double m_rangeMin = 0.0;
    double m_rangeMax = 0.0;
    double m_tubeRadius = 0.0;
    int m_revision = 0;
    bool m_updatePending = false;
    bool m_reloadPending = false;
    double m_dataMin = 0.0;
    double m_dataMax = 0.0;
    cfd::PolyData m_data;
};

} // namespace aerocad::app
