// SPDX-License-Identifier: GPL-3.0-or-later
// A monochrome icon from src/app/icons, tinted with the theme's icon colour.
import QtQuick
import QtQuick.Controls.impl
import AeroCAD

Item {
    id: root

    /// File name in src/app/icons without ".svg".
    property string name
    property int size: Theme.iconMedium
    property color color: Theme.icon

    implicitWidth: size
    implicitHeight: size

    IconImage {
        anchors.fill: parent
        source: Theme.iconUrl(root.name)
        sourceSize: Qt.size(root.size, root.size)
        color: root.color
    }
}
