// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.MenuBarItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 3
    horizontalPadding: Theme.space2 + 2
    font.pixelSize: Theme.fontBody

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: Qt.AlignLeft
        icon: control.icon
        text: control.text.replace("&", "")
        font: control.font
        color: Theme.text
    }

    background: Rectangle {
        implicitHeight: 24
        radius: Theme.radiusSmall
        color: control.down || control.highlighted ? Theme.hover : "transparent"
    }
}
