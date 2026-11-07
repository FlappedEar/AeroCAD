// SPDX-License-Identifier: GPL-3.0-or-later
// A ribbon command: large icon over a short label, as in Fusion's toolbar. Used through
// Main.qml's ToolCommand { action: ...; text: ...; iconName: ... }. Without iconName the
// icon is looked up from the label (Theme.commandIcon), falling back to a generic glyph.
import QtQuick
import QtQuick.Controls
import AeroCAD

ToolButton {
    id: control

    property string iconName: Theme.commandIcon(text)
    /// Explanation under the name in the tooltip; looked up from the label by default.
    property string help: Theme.commandHelp(text)
    /// Accent-coloured command (e.g. Finish Sketch).
    property bool primary: false

    implicitWidth: Math.max(52, label.implicitWidth + 2 * Theme.space2)
    implicitHeight: 54
    padding: 0
    focusPolicy: Qt.NoFocus

    contentItem: Item {
        AppIcon {
            id: glyph
            anchors.horizontalCenter: parent.horizontalCenter
            y: 7
            name: control.iconName
            size: Theme.iconLarge
            color: control.primary ? Theme.success : control.checked ? Theme.accent : Theme.icon
        }
        Text {
            id: label
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: glyph.bottom
            anchors.topMargin: 5
            text: control.text.replace("&", "")
            color: control.checked ? Theme.accent : Theme.text
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.pressed : control.checked ? Theme.accentSoft
             : control.hovered ? Theme.hover : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.animationFast } }
    }

    opacity: enabled ? 1 : 0.38
    ToolTip.visible: hovered
    ToolTip.delay: 600
    ToolTip.text: {
        const t = action ? action.text.replace("&", "") : text
        const s = action && action.shortcut ? Preferences.shortcutText(action.shortcut) : ""
        const title = s.length > 0 ? t + "  (" + s + ")" : t
        return help.length > 0 ? title + "\n" + help : title
    }
}
