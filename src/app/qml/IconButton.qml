// SPDX-License-Identifier: GPL-3.0-or-later
// Small icon-only button with a tooltip (quick-access bar, timeline, panel headers,
// navigation bar). The tooltip shows `tip`, or the action's text, and its shortcut.
import QtQuick
import QtQuick.Controls
import AeroCAD

ToolButton {
    id: control

    property string iconName
    property string tip
    property int iconSize: Theme.iconSmall

    implicitWidth: Math.max(Theme.controlHeight, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.controlHeight
    padding: Theme.space1
    focusPolicy: Qt.NoFocus
    display: AbstractButton.IconOnly

    contentItem: Row {
        spacing: Theme.space1
        AppIcon {
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            size: control.iconSize
            color: control.checked ? Theme.accent : Theme.icon
            visible: control.iconName.length > 0
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: control.display !== AbstractButton.IconOnly
            text: control.text.replace("&", "")
            color: control.checked ? Theme.accent : Theme.text
            font.pixelSize: Theme.fontSmall
        }
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: control.down ? Theme.pressed : control.checked ? Theme.accentSoft
             : control.hovered ? Theme.hover : "transparent"
    }

    opacity: enabled ? 1 : 0.38
    ToolTip.visible: hovered && ToolTip.text.length > 0
    ToolTip.delay: 500
    ToolTip.text: {
        const t = tip.length > 0 ? tip : (action ? action.text.replace("&", "") : "")
        const s = action && action.shortcut ? Preferences.shortcutText(action.shortcut) : ""
        return s.length > 0 ? t + "  (" + s + ")" : t
    }
}
