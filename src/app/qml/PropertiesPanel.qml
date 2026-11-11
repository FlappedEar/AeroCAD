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
    /// Physical properties (Fusion: Properties) of a closed body, solid or mesh:
    /// geometry.measure kind "part" (volume, surfaceArea, centerOfMass), or null.
    property var physical: null
    /// Measuring waits while this is true: geometry.measure would end a Move / Rotate /
    /// Scale preview (an explicit analysis commits it), so it is left until the tool ends.
    property bool busy: false
    /// While a sketch is being drawn the selection can still be the scan under it: its name
    /// and size are not what the panel is about then (the guide says "Drawing a sketch").
    readonly property bool detailsShown: !!info.id && !(registry.host.activeSketch && info.kind !== "sketch")

    function refresh() {
        info = partId ? controller.partInfo(partId) : ({})
        physical = null
        // Bodies and exact solids are cheap to measure; a big scan mesh is left to Measure.
        const measurable = info.kind === "body" || (info.kind === "imported" && (info.hasBrep ? (info.solids ?? 0) > 0
                                                                                    : (info.triangles ?? 0) <= 200000))
        // Measured only while the panel is shown (it runs on the GUI thread).
        if (!partId || !measurable || !visible || busy)
            return
        const r = controller.execute("geometry.measure", { kind: "part", id: partId })
        if (r.ok && r.result.closed && r.result.volume > 0)
            physical = r.result
    }
    function fixed(v, d) { return Number(v).toLocaleString(Qt.locale(), "f", d) }

    onPartIdChanged: refresh()
    onVisibleChanged: if (visible) refresh()
    onBusyChanged: if (!busy) refresh()
    Connections {
        target: root.controller
        function onPartChanged(id) {
            if (id === root.partId)
                root.refresh()
        }
        // A part removed elsewhere (undo, delete) leaves the panel on a stale id.
        function onHistoryChanged() { Qt.callLater(root.refresh) }
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
                visible: root.detailsShown
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.separator
            }

            Text {
                visible: root.detailsShown
                Layout.leftMargin: Theme.space3
                Layout.topMargin: Theme.space1
                text: qsTr("DETAILS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.captionSpacing
            }

            GridLayout {
                visible: root.detailsShown
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
                Label { visible: !!root.physical; text: qsTr("Volume"); color: Theme.textMuted }
                Label { visible: !!root.physical; objectName: "propVolume"; text: root.physical ? root.fixed(root.physical.volume / 1000, 3) + " cm³" : "" }
                Label { visible: !!root.physical; text: qsTr("Surface area"); color: Theme.textMuted }
                Label { visible: !!root.physical; objectName: "propArea"; text: root.physical ? root.fixed(root.physical.surfaceArea / 100, 2) + " cm²" : "" }
                Label { visible: !!root.physical && !!root.physical.centerOfMass; text: qsTr("Centre of mass"); color: Theme.textMuted }
                Label {
                    visible: !!root.physical && !!root.physical.centerOfMass
                    objectName: "propCentre"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.physical && root.physical.centerOfMass
                          ? root.physical.centerOfMass.map(v => Preferences.formatLength(v, Preferences.lengthUnit)).join(", ") : ""
                }
                Label { visible: !!root.physical; text: qsTr("Material"); color: Theme.textMuted }
                ComboBox {
                    visible: !!root.physical
                    objectName: "propMaterial"
                    Layout.fillWidth: true
                    model: Preferences.printMaterials.map(m => m.label + " (" + Number(m.density).toFixed(2) + " g/cm³)")
                    currentIndex: Preferences.printMaterials.findIndex(m => m.value === Preferences.printMaterial)
                    onActivated: index => Preferences.printMaterial = Preferences.printMaterials[index].value
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("The mass is for solid material (100 % infill).")
                }
                Label { visible: !!root.physical; text: qsTr("Mass"); color: Theme.textMuted }
                Label {
                    visible: !!root.physical
                    objectName: "propMass"
                    text: root.physical ? qsTr("%1 g (solid)").arg(root.fixed(root.physical.volume / 1000 * Preferences.printDensity, 1)) : ""
                }
                Label { text: qsTr("Type"); color: Theme.textMuted }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.info.kind === "sketch" ? qsTr("Sketch")
                          : root.info.kind === "body" ? qsTr("Body (editable, from the timeline)")
                          : root.info.kind === "plane" ? qsTr("Construction plane (reference only)")
                          : root.info.hasBrep
                          ? ((root.info.solids ?? 0) > 0 ? qsTr("Exact solid") : ((root.info.faces ?? 0) === 1 ? qsTr("Exact surface (1 face)") : qsTr("Exact surface (%1 faces)").arg(root.info.faces ?? 0)))
                          : qsTr("Triangle mesh")
                }
            }
        }
    }
}
