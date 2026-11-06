// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    property var selectedPartId: 0
    signal partSelected(var id)

    padding: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            Layout.fillWidth: true
            Layout.margins: 8
            text: qsTr("Parts")
            font.bold: true
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller.parts

            delegate: ItemDelegate {
                id: row
                required property var partId
                required property string name
                required property bool partVisible
                required property var triangles
                required property string kind

                width: ListView.view.width
                highlighted: row.partId === root.selectedPartId
                onClicked: root.partSelected(row.partId)

                contentItem: RowLayout {
                    CheckBox {
                        checked: row.partVisible
                        onToggled: root.controller.execute("part.set_visible", { id: row.partId, visible: checked })
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: row.name
                            elide: Text.ElideRight
                        }
                        Label {
                            text: row.kind === "body" ? qsTr("Body (timeline)")
                                : row.kind === "sketch" ? qsTr("Sketch (timeline)")
                                : qsTr("%1 triangles").arg(Number(row.triangles).toLocaleString(Qt.locale(), "f", 0))
                            opacity: 0.6
                            font.pointSize: 9
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 32
                visible: list.count === 0
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                opacity: 0.6
                text: qsTr("No parts yet.\nFile > Import or drop a 3D file onto the view.")
            }
        }
    }
}
