// SPDX-License-Identifier: GPL-3.0-or-later
// Menu item with icon, text and the action's shortcut on the right (Qt Quick's own
// menus show no shortcuts).
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.MenuItem {
    id: control

    readonly property string shortcutText: action && action.shortcut ? Preferences.shortcutText(action.shortcut) : ""

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 4
    leftPadding: Theme.space2 + 20
    rightPadding: Theme.space2
    spacing: Theme.space2
    font.pixelSize: Theme.fontBody

    icon.width: Theme.iconSmall
    icon.height: Theme.iconSmall
    icon.color: Theme.icon

    contentItem: Item {
        implicitWidth: label.implicitWidth + (control.shortcutText ? shortcut.implicitWidth + 32 : 0)
                       + (control.subMenu ? 20 : 0)
        implicitHeight: Math.max(label.implicitHeight, 20)
        IconLabel {
            id: label
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: control.spacing
            mirrored: control.mirrored
            display: control.display
            alignment: Qt.AlignLeft
            icon: control.icon
            text: control.text.replace("&", "")
            font: control.font
            color: control.highlighted ? Theme.text : Theme.text
        }
        Text {
            id: shortcut
            anchors.right: parent.right
            anchors.rightMargin: control.subMenu ? 20 : 0
            anchors.verticalCenter: parent.verticalCenter
            text: control.shortcutText
            visible: text.length > 0
            font.pixelSize: Theme.fontSmall
            color: Theme.textFaint
        }
    }

    indicator: AppIcon {
        x: Theme.space1 + 2
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.checked
        name: "check"
        size: 16
        color: Theme.accent
    }

    arrow: AppIcon {
        x: control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.subMenu
        name: "chevron-right"
        size: 14
        color: Theme.textMuted
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 28
        radius: Theme.radiusSmall
        color: control.down ? Theme.pressed : control.highlighted ? Theme.accentSoft : "transparent"
    }

    opacity: enabled ? 1 : 0.4
}
