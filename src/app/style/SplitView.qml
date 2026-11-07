// SPDX-License-Identifier: GPL-3.0-or-later
// Panel divider: a 1 px line with a wider invisible grab area, accent while dragged.
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.SplitView {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    handle: Item {
        implicitWidth: control.orientation === Qt.Horizontal ? 1 : control.width
        implicitHeight: control.orientation === Qt.Horizontal ? control.height : 1
        Rectangle {
            anchors.centerIn: parent
            width: control.orientation === Qt.Horizontal ? (parent.T.SplitHandle.pressed || parent.T.SplitHandle.hovered ? 3 : 1) : parent.width
            height: control.orientation === Qt.Horizontal ? parent.height : (parent.T.SplitHandle.pressed || parent.T.SplitHandle.hovered ? 3 : 1)
            color: parent.T.SplitHandle.pressed || parent.T.SplitHandle.hovered ? Theme.accent : Theme.border
        }
        // Wider grab area than the visible line.
        containmentMask: Item {
            x: control.orientation === Qt.Horizontal ? -3 : 0
            y: control.orientation === Qt.Horizontal ? 0 : -3
            width: control.orientation === Qt.Horizontal ? 7 : control.width
            height: control.orientation === Qt.Horizontal ? control.height : 7
        }
    }
}
