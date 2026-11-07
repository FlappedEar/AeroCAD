// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.ItemDelegate {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 4
    horizontalPadding: Theme.space3
    spacing: Theme.space2
    font.pixelSize: Theme.fontBody
    icon.width: Theme.iconSmall
    icon.height: Theme.iconSmall
    icon.color: Theme.icon

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: control.display === IconLabel.IconOnly || control.display === IconLabel.TextUnderIcon ? Qt.AlignCenter : Qt.AlignLeft
        icon: control.icon
        text: control.text
        font: control.font
        color: Theme.text
    }

    background: Rectangle {
        implicitWidth: 100
        implicitHeight: Theme.rowHeight
        radius: Theme.radiusSmall
        color: control.down ? Theme.pressed : control.highlighted ? Theme.accentSoft
             : control.hovered ? Theme.hover : "transparent"
    }
}
