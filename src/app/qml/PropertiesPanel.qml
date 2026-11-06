// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    property var partId: 0
    property var info: ({})

    function refresh() {
        info = partId ? controller.partInfo(partId) : ({})
    }

    onPartIdChanged: refresh()
    Connections {
        target: root.controller
        function onPartChanged(id) {
            if (id === root.partId)
                root.refresh()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 8

        Label {
            text: qsTr("Properties")
            font.bold: true
        }

        Label {
            visible: !root.info.id
            text: qsTr("Select a part.")
            opacity: 0.6
        }

        GridLayout {
            visible: !!root.info.id
            columns: 2
            columnSpacing: 12
            Label { text: qsTr("Name"); opacity: 0.7 }
            TextField {
                Layout.fillWidth: true
                text: root.info.name ?? ""
                onEditingFinished: {
                    if (text.length > 0 && text !== root.info.name)
                        root.controller.execute("part.rename", { id: root.partId, name: text })
                }
            }
            Label { text: qsTr("Size X"); opacity: 0.7 }
            Label { text: qsTr("%1 mm").arg(Number(root.info.sizeX ?? 0).toFixed(2)) }
            Label { text: qsTr("Size Y"); opacity: 0.7 }
            Label { text: qsTr("%1 mm").arg(Number(root.info.sizeY ?? 0).toFixed(2)) }
            Label { text: qsTr("Size Z"); opacity: 0.7 }
            Label { text: qsTr("%1 mm").arg(Number(root.info.sizeZ ?? 0).toFixed(2)) }
            Label { text: qsTr("Triangles"); opacity: 0.7 }
            Label { text: Number(root.info.triangles ?? 0).toLocaleString(Qt.locale(), "f", 0) }
            Label { text: qsTr("Exact (B-rep)"); opacity: 0.7 }
            Label { text: root.info.hasBrep ? qsTr("yes") : qsTr("no (mesh)") }
        }

        GroupBox {
            Layout.fillWidth: true
            visible: !!root.info.id
            title: qsTr("Scale")
            RowLayout {
                anchors.fill: parent
                SpinBox {
                    id: scalePercent
                    Layout.fillWidth: true
                    from: 1
                    to: 100000
                    value: 100
                    editable: true
                    textFromValue: (value, locale) => value + " %"
                    valueFromText: (text, locale) => parseInt(text)
                }
                Button {
                    text: qsTr("Apply")
                    enabled: scalePercent.value !== 100
                    onClicked: {
                        root.controller.execute("part.scale", { id: root.partId, factor: scalePercent.value / 100 })
                        scalePercent.value = 100
                    }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            visible: !!root.info.id
            title: qsTr("Region selection")
            ColumnLayout {
                anchors.fill: parent
                Label {
                    text: (root.info.selectedTriangles ?? 0) > 0
                          ? qsTr("%1 triangles, %2 mm²")
                                .arg(Number(root.info.selectedTriangles).toLocaleString(Qt.locale(), "f", 0))
                                .arg(Number(root.info.selectedArea ?? 0).toLocaleString(Qt.locale(), "f", 1))
                          : qsTr("Nothing selected. Use Region (1), Lasso (2) or Brush (3) in the view.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    opacity: (root.info.selectedTriangles ?? 0) > 0 ? 1.0 : 0.6
                }
                GridLayout {
                    columns: 2
                    Layout.fillWidth: true
                    enabled: (root.info.selectedTriangles ?? 0) > 0
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Grow")
                        onClicked: root.controller.execute("selection.grow", { id: root.partId, steps: 1 })
                    }
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Shrink")
                        onClicked: root.controller.execute("selection.grow", { id: root.partId, steps: -1 })
                    }
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Invert")
                        onClicked: root.controller.execute("selection.invert", { id: root.partId })
                    }
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Clear")
                        onClicked: root.controller.execute("selection.clear", { id: root.partId })
                    }
                }
            }
        }

        Button {
            visible: !!root.info.id
            text: qsTr("Remove part")
            onClicked: root.controller.execute("part.remove", { id: root.partId })
        }

        Item { Layout.fillHeight: true }
    }
}
