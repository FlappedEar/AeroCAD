// SPDX-License-Identifier: GPL-3.0-or-later
// Title bar of a side panel (BROWSER, PROPERTIES, ...): caption on the left, optional
// buttons on the right (put them in as children).
import QtQuick
import QtQuick.Layouts
import AeroCAD

Rectangle {
    id: root

    property string title
    default property alias actions: actionRow.data

    implicitHeight: Theme.panelHeaderHeight
    color: Theme.surface

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.space3
        anchors.rightMargin: Theme.space1
        spacing: Theme.space1

        Text {
            Layout.fillWidth: true
            text: root.title
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.captionSpacing
            elide: Text.ElideRight
        }
        RowLayout {
            id: actionRow
            spacing: 0
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.separator
    }
}
