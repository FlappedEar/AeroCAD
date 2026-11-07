// SPDX-License-Identifier: GPL-3.0-or-later
// A tab of the ribbon (SOLID, SKETCH, ...): upper-case label, accent underline when current.
import QtQuick
import QtQuick.Controls
import AeroCAD

AbstractButton {
    id: control

    property bool current: false

    implicitWidth: label.implicitWidth + 2 * Theme.space3
    implicitHeight: 32
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    contentItem: Text {
        id: label
        text: control.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.current ? Theme.accent : control.hovered ? Theme.text : Theme.textMuted
        font.pixelSize: Theme.fontSmall
        font.weight: control.current ? Font.Bold : Font.DemiBold
        font.letterSpacing: Theme.captionSpacing
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 4
            radius: Theme.radiusSmall
            color: control.current ? Theme.surface : control.hovered ? Theme.hover : "transparent"
        }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 2 * Theme.space2
            height: 2
            radius: 1
            color: Theme.accent
            visible: control.current
        }
    }
}
