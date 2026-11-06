// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.MenuBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    leftPadding: Theme.space1
    topPadding: 2
    bottomPadding: 2
    spacing: 0
    font.pixelSize: Theme.fontBody

    delegate: MenuBarItem {}

    contentItem: Row {
        spacing: control.spacing
        Repeater { model: control.contentModel }
    }

    background: Rectangle {
        implicitHeight: 28
        color: Theme.chrome
    }
}
