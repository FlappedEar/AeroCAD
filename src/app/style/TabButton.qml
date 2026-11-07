// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.TabButton {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 6
    horizontalPadding: Theme.space3
    spacing: 6
    font.pixelSize: Theme.fontBody
    icon.width: Theme.iconSmall
    icon.height: Theme.iconSmall
    icon.color: control.checked ? Theme.accent : Theme.icon

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.checked ? Theme.accent : control.hovered ? Theme.text : Theme.textMuted
    }

    background: Item {
        implicitHeight: Theme.controlHeight + 4
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - Theme.space2
            height: 2
            radius: 1
            color: Theme.accent
            visible: control.checked
        }
    }
}
