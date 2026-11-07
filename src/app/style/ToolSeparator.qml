// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.ToolSeparator {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: vertical ? 6 : 2
    verticalPadding: vertical ? 4 : 6
    contentItem: Rectangle {
        implicitWidth: control.vertical ? 1 : 24
        implicitHeight: control.vertical ? 20 : 1
        color: Theme.separator
    }
}
