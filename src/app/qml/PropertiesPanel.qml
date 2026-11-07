// SPDX-License-Identifier: GPL-3.0-or-later
// Properties of the selected part: a step-by-step guide of what can be done with it
// (GuidePanel, from ContextActions) and its details (name, size, ...).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    required property ContextActions registry
    required property ActionPopup popup
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
        // A part removed elsewhere (undo, delete) leaves the panel on a stale id.
        function onHistoryChanged() { root.refresh() }
    }

    padding: 0

    PanelHeader {
        id: header
        width: parent.width
        title: qsTr("PROPERTIES")
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.topMargin: header.height
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: Theme.space2

            GuidePanel {
                Layout.fillWidth: true
                Layout.margins: Theme.space3
                registry: root.registry
                popup: root.popup
                info: root.info
            }

            Rectangle {
                visible: !!root.info.id
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.separator
            }

            Text {
                visible: !!root.info.id
                Layout.leftMargin: Theme.space3
                Layout.topMargin: Theme.space1
                text: qsTr("DETAILS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.captionSpacing
            }

            GridLayout {
                visible: !!root.info.id
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space3
                Layout.rightMargin: Theme.space3
                Layout.bottomMargin: Theme.space3
                columns: 2
                columnSpacing: 12
                rowSpacing: 6
                Label { text: qsTr("Name"); color: Theme.textMuted }
                TextField {
                    Layout.fillWidth: true
                    text: root.info.name ?? ""
                    onEditingFinished: {
                        if (text.length > 0 && text !== root.info.name)
                            root.controller.execute("part.rename", { id: root.partId, name: text })
                    }
                }
                Label { text: qsTr("Size X"); color: Theme.textMuted }
                Label { text: Preferences.formatLength(Number(root.info.sizeX ?? 0), Preferences.lengthUnit) }
                Label { text: qsTr("Size Y"); color: Theme.textMuted }
                Label { text: Preferences.formatLength(Number(root.info.sizeY ?? 0), Preferences.lengthUnit) }
                Label { text: qsTr("Size Z"); color: Theme.textMuted }
                Label { text: Preferences.formatLength(Number(root.info.sizeZ ?? 0), Preferences.lengthUnit) }
                Label { visible: root.info.kind === "imported" && !root.info.hasBrep; text: qsTr("Triangles"); color: Theme.textMuted }
                Label { visible: root.info.kind === "imported" && !root.info.hasBrep; text: Number(root.info.triangles ?? 0).toLocaleString(Qt.locale(), "f", 0) }
                Label { text: qsTr("Type"); color: Theme.textMuted }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.info.kind === "sketch" ? qsTr("Sketch")
                          : root.info.kind === "body" ? qsTr("Body (editable, from the timeline)")
                          : root.info.hasBrep
                          ? ((root.info.solids ?? 0) > 0 ? qsTr("Exact solid") : ((root.info.faces ?? 0) === 1 ? qsTr("Exact surface (1 face)") : qsTr("Exact surface (%1 faces)").arg(root.info.faces ?? 0)))
                          : qsTr("Triangle mesh")
                }
            }
        }
    }
}
