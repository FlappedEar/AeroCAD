// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.Button {
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
    icon.color: control.highlighted ? Theme.accentText : Theme.icon

    readonly property bool primary: control.highlighted || (control.DialogButtonBox.buttonRole === T.DialogButtonBox.AcceptRole)

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.primary ? Theme.accentText : Theme.text
    }

    background: Rectangle {
        implicitWidth: 72
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        visible: !control.flat || control.down || control.checked || control.hovered
        color: control.primary ? (control.down ? Theme.accentHover : control.hovered ? Theme.accentHover : Theme.accent)
             : control.down ? Theme.pressed
             : control.checked ? Theme.accentSoft
             : control.flat ? (control.hovered ? Theme.hover : "transparent")
             : control.hovered ? Qt.tint(Theme.surfaceRaised, Theme.hover) : Theme.surfaceRaised
        border.width: control.flat || control.primary ? 0 : 1
        border.color: control.visualFocus ? Theme.accent : Theme.border
    }

    opacity: enabled ? 1 : 0.45
}
