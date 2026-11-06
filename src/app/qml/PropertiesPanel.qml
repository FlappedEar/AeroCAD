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
            // Timeline bodies and sketches change through their features, not directly.
            visible: !!root.info.id && root.info.kind === "imported"
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
                RowLayout {
                    Layout.fillWidth: true
                    enabled: (root.info.selectedTriangles ?? 0) > 0
                    Label { text: qsTr("Tolerance") }
                    TextField {
                        id: fitTolerance
                        Layout.preferredWidth: 56
                        text: "0.25"
                        validator: DoubleValidator { bottom: 0.001; decimals: 3; notation: DoubleValidator.StandardNotation }
                    }
                    Label { text: qsTr("mm") }
                }
                Button {
                    Layout.fillWidth: true
                    enabled: (root.info.selectedTriangles ?? 0) > 0
                    text: qsTr("Fit surface to selection")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Creates a smooth NURBS surface (new part) from the selected area and checks how far it deviates from the mesh")
                    onClicked: {
                        const tol = Number.fromLocaleString(Qt.locale(), fitTolerance.text)
                        const r = root.controller.execute("surface.fit_selection",
                                                          { id: root.partId, tolerance: tol > 0 ? tol : 0.25 })
                        if (!r.ok)
                            return
                        const f = r.result.fit
                        let msg = qsTr("Surface \"%1\": deviation mesh→surface max %2 mm (RMS %3), surface→mesh max %4 mm, %5")
                            .arg(r.result.name)
                            .arg(Number(f.meshToSurfaceMax).toFixed(3))
                            .arg(Number(f.meshToSurfaceRms).toFixed(3))
                            .arg(Number(f.surfaceToMeshMax).toFixed(3))
                            .arg(f.withinTolerance ? qsTr("within tolerance") : qsTr("OUT OF TOLERANCE"))
                        if (r.result.warnings.length > 0)
                            msg += " · " + r.result.warnings.join(" ")
                        root.controller.showStatus(msg)
                    }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            // Exact surfaces only (for example a fitted base surface); solids have their own tools.
            visible: !!root.info.hasBrep && root.info.kind !== "sketch" && (root.info.faces ?? 0) > 0
                     && (root.info.solids ?? 0) === 0
            title: qsTr("Surface")
            GridLayout {
                anchors.fill: parent
                columns: 3
                Label { text: qsTr("Offset") }
                TextField {
                    id: offsetDistance
                    Layout.preferredWidth: 56
                    text: "2"
                    validator: DoubleValidator { decimals: 3; notation: DoubleValidator.StandardNotation }
                }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("Offset")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("New surface moved along the normal by this distance (mm, negative: the other side), e.g. a clearance")
                    onClicked: {
                        const d = Number.fromLocaleString(Qt.locale(), offsetDistance.text)
                        const r = root.controller.execute("surface.offset", { id: root.partId, distance: d })
                        if (r.ok)
                            root.controller.showStatus(qsTr("Offset surface \"%1\" at %2 mm").arg(r.result.name).arg(d))
                    }
                }
                Label { text: qsTr("Thickness") }
                TextField {
                    id: thickness
                    Layout.preferredWidth: 56
                    text: "2"
                    validator: DoubleValidator { bottom: 0.001; decimals: 3; notation: DoubleValidator.StandardNotation }
                }
                ComboBox {
                    id: thickenSide
                    Layout.fillWidth: true
                    model: [qsTr("Outside"), qsTr("Inside"), qsTr("Both")]
                }
                Button {
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                    text: qsTr("Thicken to solid")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("New closed solid of this thickness (mm) from the surface")
                    onClicked: {
                        const t = Number.fromLocaleString(Qt.locale(), thickness.text)
                        const side = ["outside", "inside", "both"][thickenSide.currentIndex]
                        const r = root.controller.execute("surface.thicken", { id: root.partId, thickness: t, side: side })
                        if (r.ok)
                            root.controller.showStatus(qsTr("Solid \"%1\": %2 mm thick, volume %3 cm³")
                                                       .arg(r.result.name).arg(t)
                                                       .arg((r.result.volume / 1000).toFixed(2)))
                    }
                }
            }
        }

        Button {
            visible: !!root.info.id && root.info.kind === "imported"
            text: qsTr("Remove part")
            onClicked: root.controller.execute("part.remove", { id: root.partId })
        }

        Item { Layout.fillHeight: true }
    }
}
