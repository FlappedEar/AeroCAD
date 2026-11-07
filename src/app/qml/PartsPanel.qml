// SPDX-License-Identifier: GPL-3.0-or-later
// Browser (Fusion's name for the document tree): the parts of the document with their
// visibility.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    property var selectedPartId: 0
    signal partSelected(var id)
    /// Right-click on a row: open the part's context menu at x, y in `item`.
    signal partContextRequested(var id, Item item, real x, real y)

    padding: 0
    background: Rectangle { color: Theme.surface }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PanelHeader {
            Layout.fillWidth: true
            title: qsTr("BROWSER")
            Text {
                text: list.count > 0 ? list.count : ""
                color: Theme.textFaint
                font.pixelSize: Theme.fontSmall
                rightPadding: Theme.space2
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.space1
            clip: true
            model: root.controller.parts
            boundsBehavior: Flickable.StopAtBounds

            delegate: ItemDelegate {
                id: row
                required property var partId
                required property string name
                required property bool partVisible
                required property var triangles
                required property string kind
                required property string shapeKind
                required property bool frozen

                readonly property bool selected: row.partId === root.selectedPartId

                width: ListView.view.width
                height: 36
                leftPadding: Theme.space3
                rightPadding: Theme.space1
                onClicked: root.partSelected(row.partId)
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: eventPoint => root.partContextRequested(row.partId, row, eventPoint.position.x, eventPoint.position.y)
                }

                background: Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space1
                    anchors.rightMargin: Theme.space1
                    radius: Theme.radiusSmall
                    color: row.selected ? Theme.accentSoft : row.hovered ? Theme.hover : "transparent"
                }

                contentItem: RowLayout {
                    spacing: Theme.space2
                    AppIcon {
                        name: row.shapeKind === "surface" ? "surface" : Theme.featureIcon(row.kind)
                        size: Theme.iconSmall
                        color: row.selected ? Theme.accent : Theme.icon
                        opacity: row.partVisible ? 1 : 0.45
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: row.name
                            color: row.partVisible ? Theme.text : Theme.textFaint
                            font.pixelSize: Theme.fontBody
                            font.weight: row.selected ? Font.DemiBold : Font.Normal
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: row.frozen ? (row.shapeKind === "surface" ? qsTr("Frozen surface · no history") : qsTr("Frozen solid · no history"))
                                : row.shapeKind === "surface" ? qsTr("Surface")
                                : row.kind === "body" ? qsTr("Body")
                                : row.kind === "sketch" ? qsTr("Sketch")
                                : row.kind === "plane" ? qsTr("Construction plane")
                                : row.shapeKind === "solid" ? qsTr("Solid")
                                : qsTr("Mesh · %1 triangles").arg(Number(row.triangles).toLocaleString(Qt.locale(), "f", 0))
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    IconButton {
                        objectName: "visibilityToggle"
                        iconName: row.partVisible ? "visible" : "hidden"
                        tip: row.partVisible ? qsTr("Hide") : qsTr("Show")
                        // Hidden parts keep a visible toggle; visible ones show it on hover.
                        opacity: !row.partVisible || row.hovered || hovered ? 1 : 0.0
                        onClicked: root.controller.execute("part.set_visible", { id: row.partId, visible: !row.partVisible })
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.space6
                visible: list.count === 0
                spacing: Theme.space2
                AppIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "import"
                    size: 32
                    color: Theme.textFaint
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("No parts yet.\nImport a 3D file or drop one onto the view.")
                }
            }
        }
    }
}
