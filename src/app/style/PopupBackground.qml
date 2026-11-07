// SPDX-License-Identifier: GPL-3.0-or-later
// Rounded panel with a soft drop shadow, shared by menus, popups, tooltips and dialogs.
// (Qt 6.4 has no MultiEffect, so the shadow is a few stacked translucent outlines.)
import QtQuick
import AeroCAD

Item {
    property color color: Theme.surfaceRaised
    property real radius: Theme.radius
    property bool shadow: true

    Repeater {
        model: parent.shadow ? 3 : 0
        Rectangle {
            required property int index
            anchors.fill: parent
            anchors.margins: -(index + 1)
            anchors.topMargin: -index
            anchors.bottomMargin: -(index + 2)
            radius: parent.radius + index + 1
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, (Theme.dark ? 0.30 : 0.07) / (index + 1))
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: parent.color
        border.width: 1
        border.color: Theme.border
    }
}
