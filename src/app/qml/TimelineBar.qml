// SPDX-License-Identifier: GPL-3.0-or-later
// The design timeline at the bottom of the window, as in Fusion: one chip per feature
// in order, playback buttons to move the marker (roll back), double-click to edit,
// right-click for Edit / Rename / Suppress / Roll Back To Here / Delete. Features that
// fail are red, suppressed and rolled-back ones are faded.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Rectangle {
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

    implicitHeight: 40
    color: Theme.chrome

    Rectangle { width: parent.width; height: 1; color: Theme.border }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.space2
        anchors.rightMargin: Theme.space2
        spacing: 2

        IconButton { iconName: "timeline-start"; tip: qsTr("Go to beginning"); onClicked: root.setMarker(0) }
        IconButton { iconName: "timeline-back"; tip: qsTr("Step back"); onClicked: root.setMarker(root.timeline.marker - 1) }
        IconButton { iconName: "chevron-right"; tip: qsTr("Step forward"); onClicked: root.setMarker(root.timeline.marker + 1) }
        IconButton { iconName: "timeline-end"; tip: qsTr("Go to end"); onClicked: root.setMarker(root.timeline.features.length) }
        Rectangle { implicitWidth: 1; implicitHeight: 20; color: Theme.separator; Layout.leftMargin: Theme.space1; Layout.rightMargin: Theme.space1 }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            orientation: ListView.Horizontal
            spacing: 2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.timeline.features

            // Fusion-style feature chips: icon and name; the marker is a handle after the
            // last active feature.
            delegate: Item {
                id: chip
                required property var modelData
                required property int index
                readonly property var feature: modelData
                readonly property bool inactive: feature.status === "suppressed" || feature.status === "rolled_back"
                readonly property bool failed: feature.status === "error"
                readonly property bool atMarker: chip.index === root.timeline.marker - 1
                width: chipBody.width + (atMarker ? 10 : 0)
                height: 32

                Rectangle {
                    id: chipBody
                    width: chipRow.implicitWidth + 2 * Theme.space2
                    height: 28
                    anchors.verticalCenter: parent.verticalCenter
                    radius: Theme.radiusSmall
                    color: chip.failed ? Theme.dangerSoft
                         : chipMouse.pressed ? Theme.pressed
                         : chipMouse.containsMouse ? Theme.hover : "transparent"
                    border.width: chip.failed ? 1 : 0
                    border.color: Theme.danger
                    opacity: chip.inactive ? 0.4 : 1

                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        AppIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: chip.failed ? "warning" : Theme.featureIcon(chip.feature.type)
                            size: Theme.iconSmall
                            color: chip.failed ? Theme.danger : Theme.icon
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: chip.feature.name
                            color: chip.failed ? Theme.danger : Theme.text
                            font.pixelSize: Theme.fontSmall
                            font.strikeout: chip.feature.suppressed
                        }
                    }
                }

                // Timeline marker after the last active feature.
                Rectangle {
                    visible: chip.atMarker
                    x: chipBody.width + 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: 26
                    radius: 1.5
                    color: Theme.accent
                }

                MouseArea {
                    id: chipMouse
                    anchors.fill: chipBody
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton)
                            menu.popup()
                    }
                    onDoubleClicked: root.editRequested(chip.feature)
                }
                ToolTip.visible: chipMouse.containsMouse
                ToolTip.delay: 400
                ToolTip.text: chip.feature.status === "error" ? chip.feature.error
                            : chip.feature.report && chip.feature.report.warnings && chip.feature.report.warnings.length
                              ? chip.feature.report.warnings.join("\n")
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

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: list.count === 0
                color: Theme.textFaint
                font.pixelSize: Theme.fontSmall
                text: qsTr("Timeline: create a sketch or a primitive to start modelling")
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
