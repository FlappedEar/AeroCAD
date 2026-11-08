// SPDX-License-Identifier: GPL-3.0-or-later
// The design timeline at the bottom of the window, as in Fusion: one chip per feature
// in order, playback buttons to move the marker (roll back), double-click to edit,
// right-click for Edit / Edit Profile Sketch / Rename / Suppress / Roll Back To Here /
// Delete. Features that fail are red, suppressed and rolled-back ones are faded. Dragging
// a chip moves the feature in the timeline, dragging the marker rolls back or forward.
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

    /// Drag in progress: "feature" (a chip, `dragFrom` its index) or "marker"; `dragSlot`
    /// is the gap it would drop into (0 = before the first feature, count = after the last).
    property string dragKind: ""
    /// Horizontal scroll of the chips (for UI scripts).
    readonly property real scrollX: list.contentX - list.originX
    property int dragFrom: -1
    property int dragSlot: -1

    /// The gap nearest x (in list.contentItem coordinates). Only chips the list has created
    /// (around the visible part) count: past the last of them is the gap right after it, not
    /// the end of a long timeline; dragging near an edge scrolls further (dragUpdate).
    function slotAt(cx) {
        let last = -1
        for (let i = 0; i < list.count; ++i) {
            const it = list.itemAtIndex(i)
            if (!it)
                continue
            if (cx < it.x + it.chipWidth / 2)
                return i
            last = i
        }
        return last + 1
    }
    /// x of gap `slot` in list.contentItem coordinates.
    function slotX(slot) {
        if (slot <= 0 && list.headerItem && list.headerItem.width > 0)
            return list.headerItem.x + 2
        const before = slot > 0 ? list.itemAtIndex(slot - 1) : null
        if (before)
            return before.x + before.chipWidth + 4
        const after = slot < list.count ? list.itemAtIndex(slot) : null
        if (after)
            return after.x - 2
        return list.headerItem ? list.headerItem.x + 2 : 0
    }

    /// The sketch a feature takes its profile from (Extrude, Revolve, Sweep, Loft: the first
    /// section), or null.
    function profileSketch(feature) {
        if (!feature || feature.type === "sketch" || !feature.params)
            return null
        let id = feature.params.sketch
        if (id === undefined && Array.isArray(feature.params.sections) && feature.params.sections.length > 0) {
            const sec = feature.params.sections[0]
            id = typeof sec === "number" ? sec : sec.sketch
        }
        return timeline.features.find(f => f.type === "sketch" && f.id === id) || null
    }

    /// Window coordinates of the middle of chip `index` / of gap `slot` (for UI scripts).
    function chipPoint(index) {
        const it = list.itemAtIndex(index)
        return it ? it.mapToItem(null, it.chipWidth / 2, it.height / 2) : null
    }
    function slotPoint(slot) {
        return list.contentItem.mapToItem(null, slotX(slot) + 1, list.height / 2)
    }
    function dragUpdate(item, x) {
        // Near an edge of a long timeline, scroll to bring more gaps into reach.
        const lx = item.mapToItem(list, x, 0).x
        const step = 24
        if (lx < step)
            list.contentX = Math.max(list.originX, list.contentX - step)
        else if (lx > list.width - step)
            list.contentX = Math.min(list.originX + Math.max(0, list.contentWidth - list.width), list.contentX + step)
        dragSlot = slotAt(item.mapToItem(list.contentItem, x, 0).x)
    }
    function dragFinish() {
        const kind = dragKind, from = dragFrom, slot = dragSlot
        dragKind = ""
        dragFrom = -1
        dragSlot = -1
        if (kind === "marker" && slot >= 0 && slot !== timeline.marker) {
            setMarker(slot)
        } else if (kind === "feature" && slot >= 0) {
            const to = slot > from ? slot - 1 : slot
            if (to !== from)
                controller.execute("timeline.move_feature", { id: timeline.features[from].id, position: to })
        }
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
            interactive: root.dragKind === ""

            // The marker before the first feature (everything rolled back).
            header: Item {
                width: root.timeline.marker === 0 && root.timeline.features.length > 0 ? 10 : 0
                height: 32
                Rectangle {
                    visible: parent.width > 0
                    x: 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: 26
                    radius: 1.5
                    color: Theme.accent
                    opacity: root.dragKind === "marker" ? 0.3 : 1
                }
                MouseArea {
                    id: headerHandle
                    objectName: "timelineMarkerHandle"
                    enabled: parent.width > 0
                    anchors.fill: parent
                    cursorShape: Qt.SizeHorCursor
                    preventStealing: true
                    onPressed: { root.dragKind = "marker"; root.dragSlot = 0 }
                    onPositionChanged: mouse => root.dragUpdate(headerHandle, mouse.x)
                    onReleased: root.dragFinish()
                    onCanceled: { root.dragKind = ""; root.dragSlot = -1 }
                }
            }

            // Fusion-style feature chips: icon and name; the marker is a handle after the
            // last active feature.
            delegate: Item {
                id: chip
                required property var modelData
                required property int index
                readonly property var feature: modelData
                readonly property bool inactive: feature.status === "suppressed" || feature.status === "rolled_back"
                readonly property bool failed: feature.status === "error"
                // Kept, but the surface fit missed its tolerance (see the tooltip).
                readonly property bool approximate: !failed && feature.approximate === true
                readonly property bool atMarker: chip.index === root.timeline.marker - 1
                readonly property real chipWidth: chipBody.width
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
                    opacity: root.dragKind === "feature" && root.dragFrom === chip.index ? 0.3 : chip.inactive ? 0.4 : 1

                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        AppIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: chip.failed || chip.approximate ? "warning" : Theme.featureIcon(chip.feature.type)
                            size: Theme.iconSmall
                            color: chip.failed ? Theme.danger : chip.approximate ? Theme.warning : Theme.icon
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

                // Timeline marker after the last active feature; drag it to roll back / forward.
                Rectangle {
                    visible: chip.atMarker
                    x: chipBody.width + 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: 26
                    radius: 1.5
                    color: Theme.accent
                    opacity: root.dragKind === "marker" ? 0.3 : 1
                }
                MouseArea {
                    id: markerHandle
                    objectName: chip.atMarker ? "timelineMarkerHandle" : ""
                    visible: chip.atMarker
                    x: chipBody.width
                    width: 10
                    height: parent.height
                    cursorShape: Qt.SizeHorCursor
                    preventStealing: true
                    onPressed: { root.dragKind = "marker"; root.dragSlot = chip.index + 1 }
                    onPositionChanged: mouse => root.dragUpdate(markerHandle, mouse.x)
                    onReleased: root.dragFinish()
                    onCanceled: { root.dragKind = ""; root.dragSlot = -1 }
                }

                MouseArea {
                    id: chipMouse
                    anchors.fill: chipBody
                    hoverEnabled: true
                    preventStealing: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property real pressX: 0
                    onPressed: mouse => { pressX = mouse.x }
                    onPositionChanged: mouse => {
                        if (!(mouse.buttons & Qt.LeftButton))
                            return
                        if (root.dragKind === "" && Math.abs(mouse.x - pressX) > 8) {
                            root.dragKind = "feature"
                            root.dragFrom = chip.index
                        }
                        if (root.dragKind === "feature")
                            root.dragUpdate(chipMouse, mouse.x)
                    }
                    onReleased: if (root.dragKind === "feature") root.dragFinish()
                    onCanceled: if (root.dragKind === "feature") { root.dragKind = ""; root.dragFrom = -1; root.dragSlot = -1 }
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton)
                            menu.popup(chipMouse, mouse.x, mouse.y)
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
                        // Fusion: open the sketch the profile comes from.
                        readonly property var sketch: root.profileSketch(chip.feature)
                        text: qsTr("Edit Profile Sketch")
                        visible: sketch !== null
                        height: visible ? implicitHeight : 0
                        onTriggered: root.editRequested(sketch)
                    }
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

            // Where a dragged feature or the dragged marker drops.
            Rectangle {
                parent: list.contentItem
                visible: root.dragSlot >= 0 && root.dragKind !== ""
                x: root.slotX(root.dragSlot) - (root.dragKind === "marker" ? 0 : 1)
                y: (list.height - height) / 2
                width: root.dragKind === "marker" ? 3 : 2
                height: 26
                radius: 1
                color: root.dragKind === "marker" ? Theme.accent : Theme.text
                z: 10
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
