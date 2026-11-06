// SPDX-License-Identifier: GPL-3.0-or-later
// The design timeline at the bottom of the window, as in Fusion: one chip per feature
// in order, playback buttons to move the marker (roll back), double-click to edit,
// right-click for Edit / Rename / Suppress / Roll Back To Here / Delete. Features that
// fail are red, suppressed and rolled-back ones are faded.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

ToolBar {
    id: root

    required property AppController controller
    property var timeline: ({ features: [], marker: 0, parameters: [], bodies: [] })
    signal editRequested(var feature)

    function refresh() {
        const r = controller.execute("timeline.list")
        if (r.ok)
            timeline = r.result
    }

    function setMarker(position) {
        controller.execute("timeline.set_marker", { position: Math.max(0, Math.min(position, timeline.features.length)) })
    }

    Component.onCompleted: refresh()
    Connections {
        target: root.controller
        function onHistoryChanged() { root.refresh() }
    }

    readonly property var symbols: ({
        sketch: "✎", extrude: "▮", revolve: "◔", fillet: "◜", chamfer: "◸", shell: "▢",
        combine: "⊕", box: "■", cylinder: "●"
    })

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 6
        spacing: 2

        ToolButton { text: "⏮"; ToolTip.text: qsTr("Go to beginning"); ToolTip.visible: hovered; onClicked: root.setMarker(0) }
        ToolButton { text: "◀"; ToolTip.text: qsTr("Step back"); ToolTip.visible: hovered; onClicked: root.setMarker(root.timeline.marker - 1) }
        ToolButton { text: "▶"; ToolTip.text: qsTr("Step forward"); ToolTip.visible: hovered; onClicked: root.setMarker(root.timeline.marker + 1) }
        ToolButton { text: "⏭"; ToolTip.text: qsTr("Go to end"); ToolTip.visible: hovered; onClicked: root.setMarker(root.timeline.features.length) }
        ToolSeparator {}

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            orientation: ListView.Horizontal
            spacing: 3
            clip: true
            model: root.timeline.features

            delegate: Rectangle {
                id: chip
                required property var modelData
                required property int index
                readonly property var feature: modelData
                readonly property bool inactive: feature.status === "suppressed" || feature.status === "rolled_back"
                width: chipLabel.implicitWidth + 16
                height: 26
                radius: 4
                color: feature.status === "error" ? "#f4c7c3" : chipMouse.containsMouse ? "#dfe8f5" : "#eef1f5"
                border.color: feature.status === "error" ? "#b3261e" : "#b7c0cc"
                opacity: inactive ? 0.45 : 1

                // Timeline marker after the last active feature.
                Rectangle {
                    visible: chip.index === root.timeline.marker - 1
                    anchors.left: parent.right
                    anchors.leftMargin: 0
                    width: 3
                    height: parent.height + 4
                    y: -2
                    color: "#1f5fbf"
                }

                Label {
                    id: chipLabel
                    anchors.centerIn: parent
                    text: (root.symbols[chip.feature.type] || "•") + " " + chip.feature.name
                    font.strikeout: chip.feature.suppressed
                }

                MouseArea {
                    id: chipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton)
                            menu.popup()
                    }
                    onDoubleClicked: root.editRequested(chip.feature)
                }
                ToolTip.visible: chipMouse.containsMouse
                ToolTip.text: chip.feature.status === "error" ? chip.feature.error
                            : chip.feature.type + " · " + chip.feature.status

                Menu {
                    id: menu
                    MenuItem { text: qsTr("Edit Feature"); onTriggered: root.editRequested(chip.feature) }
                    MenuItem {
                        text: qsTr("Rename")
                        onTriggered: { renameDialog.feature = chip.feature; renameDialog.open() }
                    }
                    MenuItem {
                        text: chip.feature.suppressed ? qsTr("Unsuppress Feature") : qsTr("Suppress Feature")
                        onTriggered: root.controller.execute("feature.suppress", { id: chip.feature.id, suppressed: !chip.feature.suppressed })
                    }
                    MenuItem { text: qsTr("Roll Back To Here"); onTriggered: root.setMarker(chip.index + 1) }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Delete"); onTriggered: root.controller.execute("feature.delete", { id: chip.feature.id }) }
                }
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                visible: list.count === 0
                opacity: 0.6
                text: qsTr("Timeline: create a sketch (Create Sketch) or a primitive to start modelling")
            }
        }
    }

    Dialog {
        id: renameDialog
        property var feature: null
        title: qsTr("Rename Feature")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: nameField.text = feature ? feature.name : ""
        onAccepted: if (feature && nameField.text.length > 0)
            root.controller.execute("feature.rename", { id: feature.id, name: nameField.text })
        TextField { id: nameField; width: 240; onAccepted: renameDialog.accept() }
    }
}
