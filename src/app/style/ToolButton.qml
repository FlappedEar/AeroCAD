// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 4
    horizontalPadding: Theme.space2
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
        color: control.checked ? Theme.accent : Theme.text
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSmall
        color: control.down ? Theme.pressed : control.checked ? Theme.accentSoft
             : control.hovered ? Theme.hover : "transparent"
    }

    opacity: enabled ? 1 : 0.4
}
