// SPDX-License-Identifier: GPL-3.0-or-later
#include "Preferences.hpp"

#include <QKeySequence>
#include <QLocale>
#include <QPalette>
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
const QString kAutosaveKey = QStringLiteral("project/autosaveSeconds");

const QString kDefaultUnit = QStringLiteral("mm");
const QString kDefaultTheme = QStringLiteral("system");
const QString kDefaultMouse = QStringLiteral("fusion");
constexpr int kDefaultAutosave = 120;
constexpr int kMaxAutosave = 3600;

QVariantMap option(const QString& value, const QString& label)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
}

bool contains(const QVariantList& options, const QString& value)
{
    return std::any_of(options.begin(), options.end(),
                       [&](const QVariant& o) { return o.toMap().value(QStringLiteral("value")).toString() == value; });
}

QPalette darkPalette()
{
    QPalette p;
    const QColor window(0x2b, 0x2d, 0x30);
    const QColor base(0x1f, 0x20, 0x23);
    const QColor text(0xe6, 0xe6, 0xe6);
    const QColor disabled(0x80, 0x80, 0x80);
    const QColor highlight(0x2f, 0x7e, 0xd8);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x3a, 0x3c, 0x40));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, QColor(0x4a, 0x4c, 0x50));
    p.setColor(QPalette::Midlight, QColor(0x42, 0x44, 0x48));
    p.setColor(QPalette::Mid, QColor(0x33, 0x35, 0x38));
    p.setColor(QPalette::Dark, QColor(0x18, 0x19, 0x1b));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, highlight.lighter(130));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

QPalette lightPalette()
{
    QPalette p;
    const QColor window(0xf0, 0xf0, 0xf0);
    const QColor text(0x1d, 0x1d, 0x1f);
    const QColor disabled(0x9a, 0x9a, 0x9a);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf7, 0xf7));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, Qt::white);
    p.setColor(QPalette::Midlight, QColor(0xe3, 0xe3, 0xe3));
    p.setColor(QPalette::Mid, QColor(0xb8, 0xb8, 0xb8));
    p.setColor(QPalette::Dark, QColor(0xa0, 0xa0, 0xa0));
    p.setColor(QPalette::Shadow, QColor(0x50, 0x50, 0x50));
    p.setColor(QPalette::Highlight, QColor(0x2f, 0x7e, 0xd8));
    p.setColor(QPalette::HighlightedText, Qt::white);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    return p;
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
    m_autosaveSeconds = std::clamp(s.value(kAutosaveKey, kDefaultAutosave).toInt(), 0, kMaxAutosave);

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
    setAutosaveSeconds(kDefaultAutosave);
}

QVariantMap Preferences::colors() const
{
    if (m_theme == QLatin1String("system"))
        return {};
    const QPalette p = m_theme == QLatin1String("dark") ? darkPalette() : lightPalette();
    const std::pair<const char*, QPalette::ColorRole> roles[] = {
        {"window", QPalette::Window},
        {"windowText", QPalette::WindowText},
        {"base", QPalette::Base},
        {"alternateBase", QPalette::AlternateBase},
        {"toolTipBase", QPalette::ToolTipBase},
        {"toolTipText", QPalette::ToolTipText},
        {"placeholderText", QPalette::PlaceholderText},
        {"text", QPalette::Text},
        {"button", QPalette::Button},
        {"buttonText", QPalette::ButtonText},
        {"brightText", QPalette::BrightText},
        {"light", QPalette::Light},
        {"midlight", QPalette::Midlight},
        {"mid", QPalette::Mid},
        {"dark", QPalette::Dark},
        {"shadow", QPalette::Shadow},
        {"highlight", QPalette::Highlight},
        {"highlightedText", QPalette::HighlightedText},
        {"link", QPalette::Link},
    };
    QVariantMap out;
    for (const auto& [name, role] : roles)
        out.insert(QString::fromLatin1(name), p.color(QPalette::Active, role));
    out.insert(QStringLiteral("disabledText"), p.color(QPalette::Disabled, QPalette::Text));
    return out;
}

} // namespace aerocad::app
