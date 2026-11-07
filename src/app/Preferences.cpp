// SPDX-License-Identifier: GPL-3.0-or-later
#include "Preferences.hpp"

#include <QKeySequence>
#include <QLocale>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <utility>

namespace aerocad::app {

namespace {

const QString kUnitKey = QStringLiteral("display/lengthUnit");
const QString kThemeKey = QStringLiteral("display/theme");
const QString kMouseKey = QStringLiteral("navigation/mouseProfile");
const QString kInvertZoomKey = QStringLiteral("navigation/invertZoom");
const QString kInvertOrbitKey = QStringLiteral("navigation/invertOrbit");
const QString kAutosaveKey = QStringLiteral("project/autosaveSeconds");
const QString kMinWallKey = QStringLiteral("export/minWallThickness");

const QString kDefaultUnit = QStringLiteral("mm");
const QString kDefaultTheme = QStringLiteral("system");
const QString kDefaultMouse = QStringLiteral("fusion");
constexpr int kDefaultAutosave = 120;
constexpr int kMaxAutosave = 3600;
constexpr double kDefaultMinWall = 0.8;
constexpr double kMaxMinWall = 20.0;

QVariantMap option(const QString& value, const QString& label)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
}

bool contains(const QVariantList& options, const QString& value)
{
    return std::any_of(options.begin(), options.end(),
                       [&](const QVariant& o) { return o.toMap().value(QStringLiteral("value")).toString() == value; });
}

} // namespace

Preferences::Preferences(QObject* parent) : QObject(parent)
{
    const QSettings s;
    m_lengthUnit = s.value(kUnitKey, kDefaultUnit).toString();
    if (millimetresPer(m_lengthUnit) <= 0.0)
        m_lengthUnit = kDefaultUnit;
    m_theme = s.value(kThemeKey, kDefaultTheme).toString();
    if (!contains(themes(), m_theme))
        m_theme = kDefaultTheme;
    m_mouseProfile = s.value(kMouseKey, kDefaultMouse).toString();
    if (!contains(mouseProfiles(), m_mouseProfile))
        m_mouseProfile = kDefaultMouse;
    m_invertZoom = s.value(kInvertZoomKey, false).toBool();
    m_invertOrbit = s.value(kInvertOrbitKey, false).toBool();
    m_autosaveSeconds = std::clamp(s.value(kAutosaveKey, kDefaultAutosave).toInt(), 0, kMaxAutosave);
    m_minWallThickness = std::clamp(s.value(kMinWallKey, kDefaultMinWall).toDouble(), 0.0, kMaxMinWall);

}

void Preferences::setLengthUnit(const QString& unit)
{
    if (unit == m_lengthUnit || millimetresPer(unit) <= 0.0)
        return;
    m_lengthUnit = unit;
    QSettings().setValue(kUnitKey, unit);
    emit lengthUnitChanged();
}

QString Preferences::lengthSymbol() const
{
    return m_lengthUnit;
}

void Preferences::setTheme(const QString& theme)
{
    if (theme == m_theme || !contains(themes(), theme))
        return;
    m_theme = theme;
    QSettings().setValue(kThemeKey, theme);
    emit themeChanged();
}

void Preferences::setMouseProfile(const QString& profile)
{
    if (profile == m_mouseProfile || !contains(mouseProfiles(), profile))
        return;
    m_mouseProfile = profile;
    QSettings().setValue(kMouseKey, profile);
    emit mouseProfileChanged();
}

void Preferences::setInvertOrbit(bool invert)
{
    if (invert == m_invertOrbit)
        return;
    m_invertOrbit = invert;
    QSettings().setValue(kInvertOrbitKey, invert);
    emit invertOrbitChanged();
}

void Preferences::setInvertZoom(bool invert)
{
    if (invert == m_invertZoom)
        return;
    m_invertZoom = invert;
    QSettings().setValue(kInvertZoomKey, invert);
    emit invertZoomChanged();
}

void Preferences::setAutosaveSeconds(int seconds)
{
    seconds = std::clamp(seconds, 0, kMaxAutosave);
    if (seconds == m_autosaveSeconds)
        return;
    m_autosaveSeconds = seconds;
    QSettings().setValue(kAutosaveKey, seconds);
    emit autosaveSecondsChanged();
}

void Preferences::setMinWallThickness(double mm)
{
    mm = std::clamp(std::isfinite(mm) ? mm : kDefaultMinWall, 0.0, kMaxMinWall);
    if (mm == m_minWallThickness)
        return;
    m_minWallThickness = mm;
    QSettings().setValue(kMinWallKey, mm);
    emit minWallThicknessChanged();
}

QVariantList Preferences::lengthUnits()
{
    return {option(QStringLiteral("mm"), tr("Millimetres (mm)")), option(QStringLiteral("cm"), tr("Centimetres (cm)")),
            option(QStringLiteral("m"), tr("Metres (m)")), option(QStringLiteral("in"), tr("Inches (in)"))};
}

QVariantList Preferences::themes()
{
    return {option(QStringLiteral("system"), tr("Follow system")), option(QStringLiteral("light"), tr("Light")),
            option(QStringLiteral("dark"), tr("Dark"))};
}

QVariantList Preferences::mouseProfiles()
{
    return {option(QStringLiteral("fusion"), tr("Fusion: middle drag pans, Shift+middle orbits")),
            option(QStringLiteral("solidworks"), tr("SolidWorks: middle drag orbits, Ctrl+middle pans")),
            option(QStringLiteral("blender"), tr("Blender: middle drag orbits, Shift+middle pans"))};
}

double Preferences::millimetresPer(const QString& unit)
{
    if (unit == QLatin1String("mm"))
        return 1.0;
    if (unit == QLatin1String("cm"))
        return 10.0;
    if (unit == QLatin1String("m"))
        return 1000.0;
    if (unit == QLatin1String("in"))
        return 25.4;
    return 0.0;
}

double Preferences::toDisplay(double mm) const
{
    return mm / millimetresPer(m_lengthUnit);
}

double Preferences::fromDisplay(double value) const
{
    return value * millimetresPer(m_lengthUnit);
}

QString Preferences::formatLength(double mm, const QString& unit, int decimals) const
{
    const QString u = millimetresPer(unit) > 0.0 ? unit : m_lengthUnit;
    const double per = millimetresPer(u);
    if (decimals < 0) {
        // About 0.01 mm resolution in every unit.
        decimals = std::clamp(static_cast<int>(std::ceil(std::log10(per / 0.01))), 0, 6);
    }
    return QLocale().toString(mm / per, 'f', decimals) + QLatin1Char(' ') + u;
}

QString Preferences::shortcutText(const QVariant& shortcut)
{
    QKeySequence seq;
    if (shortcut.typeId() == QMetaType::QString)
        seq = QKeySequence(shortcut.toString());
    else if (shortcut.canConvert<int>())
        seq = QKeySequence(static_cast<QKeySequence::StandardKey>(shortcut.toInt()));
    else if (shortcut.canConvert<QKeySequence>())
        seq = shortcut.value<QKeySequence>();
    return seq.toString(QKeySequence::NativeText);
}

void Preferences::resetToDefaults()
{
    setLengthUnit(kDefaultUnit);
    setTheme(kDefaultTheme);
    setMouseProfile(kDefaultMouse);
    setInvertZoom(false);
    setInvertOrbit(false);
    setAutosaveSeconds(kDefaultAutosave);
    setMinWallThickness(kDefaultMinWall);
}

} // namespace aerocad::app
