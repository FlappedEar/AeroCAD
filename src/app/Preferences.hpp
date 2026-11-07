// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QSettings;

namespace aerocad::app {

/// User preferences, persisted with QSettings. Available in QML as the `Preferences`
/// singleton. The model is always in millimetres; `lengthUnit` only changes how lengths
/// are shown.
class Preferences : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString lengthUnit READ lengthUnit WRITE setLengthUnit NOTIFY lengthUnitChanged)
    Q_PROPERTY(QString lengthSymbol READ lengthSymbol NOTIFY lengthUnitChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString mouseProfile READ mouseProfile WRITE setMouseProfile NOTIFY mouseProfileChanged)
    Q_PROPERTY(bool invertZoom READ invertZoom WRITE setInvertZoom NOTIFY invertZoomChanged)
    Q_PROPERTY(bool invertOrbit READ invertOrbit WRITE setInvertOrbit NOTIFY invertOrbitChanged)
    Q_PROPERTY(int autosaveSeconds READ autosaveSeconds WRITE setAutosaveSeconds NOTIFY autosaveSecondsChanged)
    Q_PROPERTY(double minWallThickness READ minWallThickness WRITE setMinWallThickness NOTIFY minWallThicknessChanged)
    Q_PROPERTY(QVariantList lengthUnits READ lengthUnits CONSTANT)
    Q_PROPERTY(QVariantList themes READ themes CONSTANT)
    Q_PROPERTY(QVariantList mouseProfiles READ mouseProfiles CONSTANT)

public:
    explicit Preferences(QObject* parent = nullptr);

    [[nodiscard]] QString lengthUnit() const { return m_lengthUnit; }
    void setLengthUnit(const QString& unit);
    [[nodiscard]] QString lengthSymbol() const;

    /// "system", "light" or "dark". The colours themselves live in Theme.qml.
    [[nodiscard]] QString theme() const { return m_theme; }
    void setTheme(const QString& theme);

    /// Mouse navigation: "fusion", "solidworks" or "blender".
    [[nodiscard]] QString mouseProfile() const { return m_mouseProfile; }
    void setMouseProfile(const QString& profile);

    [[nodiscard]] bool invertZoom() const { return m_invertZoom; }
    [[nodiscard]] bool invertOrbit() const { return m_invertOrbit; }
    void setInvertZoom(bool invert);
    void setInvertOrbit(bool invert);

    /// Project autosave interval in seconds, 0 = off. Stored under the key the project
    /// autosave reads ("project/autosaveSeconds").
    [[nodiscard]] int autosaveSeconds() const { return m_autosaveSeconds; }
    void setAutosaveSeconds(int seconds);

    /// Thinnest wall the export check accepts for 3D printing, mm (0 = no wall check).
    [[nodiscard]] double minWallThickness() const { return m_minWallThickness; }
    void setMinWallThickness(double mm);

    /// [{value, label}] lists for the preferences dialog.
    [[nodiscard]] static QVariantList lengthUnits();
    [[nodiscard]] static QVariantList themes();
    [[nodiscard]] static QVariantList mouseProfiles();

    /// Millimetres to the display unit and back.
    Q_INVOKABLE [[nodiscard]] double toDisplay(double mm) const;
    Q_INVOKABLE [[nodiscard]] double fromDisplay(double value) const;
    /// "12.50 mm", "1.25 cm", "0.492 in": value in `unit` (empty = the display unit) with
    /// its symbol. decimals < 0 picks a precision suited to the unit.
    /// QML bindings pass Preferences.lengthUnit so they re-evaluate when the unit changes:
    /// `Preferences.formatLength(size, Preferences.lengthUnit)`.
    Q_INVOKABLE [[nodiscard]] QString formatLength(double mm, const QString& unit = QString(), int decimals = -1) const;

    /// Human-readable shortcut, as shown in menus on this platform (e.g. "⌘O" on macOS).
    /// Accepts a QKeySequence::StandardKey value or a key sequence string.
    Q_INVOKABLE [[nodiscard]] static QString shortcutText(const QVariant& shortcut);

    Q_INVOKABLE void resetToDefaults();


    /// Millimetres per unit, 0 for unknown units.
    [[nodiscard]] static double millimetresPer(const QString& unit);

signals:
    void lengthUnitChanged();
    void themeChanged();
    void mouseProfileChanged();
    void invertZoomChanged();
    void invertOrbitChanged();
    void autosaveSecondsChanged();
    void minWallThicknessChanged();

private:

    QString m_lengthUnit;
    QString m_theme;
    QString m_mouseProfile;
    bool m_invertZoom = false;
    bool m_invertOrbit = false;
    int m_autosaveSeconds = 120;
    double m_minWallThickness = 0.8;
};

} // namespace aerocad::app
