// SPDX-License-Identifier: GPL-3.0-or-later
// Scan tools: mesh statistics and repair, units and scale calibration, alignment to the
// world axes, display level of detail. Every action runs a command (mesh.*, part.*), so
// it is undoable and available to an AI agent over MCP as well.
// Points are picked by clicking on the model while a pick mode is active: Main forwards
// viewport picks to addPick().
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    required property ScanController scan
    /// Part the tools act on (the selected part).
    property var partId: 0
    /// "" | "calibrate" | "align": what viewport clicks are used for.
    property string pickMode: ""
    property var calibratePoints: []
    property var alignPoints: []
    property var stats: null
    property string message: ""
    /// Bumped when a display level of detail finishes, so the "Displayed" count updates.
    property int lodRevision: 0

    readonly property var info: partId ? controller.partInfo(partId) : ({})
    readonly property bool editable: !!info.id && info.kind === "imported"
    readonly property var pickedPoints: pickMode === "calibrate" ? calibratePoints
                                      : pickMode === "align" ? alignPoints : []
    readonly property real measured: calibratePoints.length === 2
        ? Qt.vector3d(calibratePoints[1][0] - calibratePoints[0][0], calibratePoints[1][1] - calibratePoints[0][1],
                      calibratePoints[1][2] - calibratePoints[0][2]).length() : 0

    visible: false
    padding: 12
    width: 320

    function open() {
        visible = true
        refreshStats()
    }

    function close() {
        visible = false
        pickMode = ""
    }

    function reset() {
        calibratePoints = []
        alignPoints = []
        stats = null
        message = ""
        pickMode = ""
    }

    // Ids arrive as different numeric types (selection vs. viewport picks), so a
    // re-selection of the same part also signals a change; compare values.
    property real currentPartId: 0
    onPartIdChanged: {
        if (Number(partId) === currentPartId)
            return
        currentPartId = Number(partId)
        reset()
        if (visible)
            refreshStats()
    }

    function addPick(id, point) {
        if (!pickMode || Number(id) !== Number(partId))
            return
        const p = scan.snapToMesh(id, point)
        const xyz = [p.x, p.y, p.z]
        if (pickMode === "calibrate")
            calibratePoints = calibratePoints.length >= 2 ? [xyz] : calibratePoints.concat([xyz])
        else if (pickMode === "align")
            alignPoints = alignPoints.concat([xyz])
    }

    function run(name, params, done) {
        const r = controller.execute(name, params)
        if (!r.ok) {
            message = r.error
            return null
        }
        message = ""
        if (done)
            done(r.result)
        refreshStats()
        return r.result
    }

    function refreshStats() {
        if (!info.id || info.kind === "sketch") {
            stats = null
            return
        }
        const r = controller.execute("mesh.stats", { id: partId })
        stats = r.ok ? r.result : null
    }

    function fmt(n) {
        return Number(n).toLocaleString(Qt.locale(), "f", 0)
    }

    component Hint: Label {
        Layout.fillWidth: true
        Layout.maximumWidth: root.availableWidth
        wrapMode: Text.WordWrap
        opacity: 0.75
    }

    component Section: Label {
        Layout.topMargin: 8
        font.bold: true
        opacity: 0.8
    }

    ScrollView {
        anchors.fill: parent
        implicitHeight: Math.min(content.implicitHeight, root.parent ? root.parent.height - 40 : 600)
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            id: content
            width: root.availableWidth
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Scan Tools"); font.bold: true; font.pixelSize: 15; Layout.fillWidth: true }
                ToolButton { text: "✕"; onClicked: root.close() }
            }
            Hint {
                text: !root.info.id ? qsTr("Select an imported mesh in the browser or the canvas.")
                    : root.info.name + (root.editable ? "" : qsTr(" (timeline geometry: read only)"))
            }

            // --- Mesh statistics -------------------------------------------------------
            Section { text: qsTr("MESH") }
            GridLayout {
                visible: !!root.stats
                columns: 2
                columnSpacing: 12
                rowSpacing: 0
                Label { text: qsTr("Triangles") } Label { text: root.stats ? root.fmt(root.stats.triangles) : "" }
                Label { text: qsTr("Displayed") }
                Label {
                    text: {
                        root.stats; root.scan.triangleBudget; root.lodRevision
                        const n = root.info.id ? root.scan.displayedTriangles(root.partId) : 0
                        return n ? root.fmt(n) : qsTr("computing…")
                    }
                }
                Label { text: qsTr("Holes") } Label { text: root.stats ? root.fmt(root.stats.holes) : "" }
                Label { text: qsTr("Flipped edges") } Label { text: root.stats ? root.fmt(root.stats.inconsistentEdges) : "" }
                Label { text: qsTr("Non-manifold edges") } Label { text: root.stats ? root.fmt(root.stats.nonManifoldEdges) : "" }
                Label { text: qsTr("Duplicates / degenerate") }
                Label { text: root.stats ? root.fmt(root.stats.duplicateTriangles) + " / " + root.fmt(root.stats.degenerateTriangles) : "" }
                Label { text: qsTr("Shells") } Label { text: root.stats ? root.fmt(root.stats.components) : "" }
                Label { text: qsTr("Size (mm)") }
                Label {
                    text: root.stats && root.stats.bounds
                        ? root.stats.bounds.size.map(v => v.toFixed(2)).join(" × ") : ""
                }
            }

            // --- Repair ----------------------------------------------------------------
            Section { text: qsTr("REPAIR") }
            GridLayout {
                columns: 2
                enabled: root.editable && !root.info.hasBrep
                Label { text: qsTr("Merge distance (mm)") }
                TextField { id: mergeField; text: "0"; Layout.preferredWidth: 80; validator: DoubleValidator { bottom: 0 } }
                Label { text: qsTr("Fill holes up to (edges)") }
                SpinBox { id: holeEdges; from: 0; to: 100000; value: 200; editable: true; stepSize: 50 }
            }
            RowLayout {
                enabled: root.editable && !root.info.hasBrep
                Button {
                    text: qsTr("Repair")
                    onClicked: root.run("mesh.repair", {
                        id: root.partId, mergeDistance: Number(mergeField.text) || 0, maxHoleEdges: holeEdges.value
                    }, r => root.controller.showStatus(qsTr("Repaired: %1 holes filled, %2 triangles flipped, %3 duplicates and %4 degenerate removed")
                        .arg(r.filledHoles).arg(r.flippedTriangles).arg(r.removedDuplicates).arg(r.removedDegenerate)))
                }
                Button { text: qsTr("Flip Normals"); onClicked: root.run("mesh.flip_normals", { id: root.partId }) }
            }

            // --- Units and scale -------------------------------------------------------
            Section { text: qsTr("UNITS AND SCALE") }
            RowLayout {
                Label { text: qsTr("New imports in") }
                ComboBox {
                    model: root.scan.unitSymbols
                    currentIndex: Math.max(0, root.scan.unitSymbols.indexOf(root.scan.importUnits))
                    onActivated: root.scan.importUnits = currentText
                }
            }
            RowLayout {
                enabled: root.editable
                Label { text: qsTr("This part is in") }
                ComboBox { id: fromUnit; model: root.scan.unitSymbols; currentIndex: 1 }
                Button {
                    text: qsTr("Convert")
                    enabled: fromUnit.currentText !== "mm"
                    onClicked: root.run("part.convert_units", { id: root.partId, from: fromUnit.currentText })
                }
            }
            Hint {
                text: qsTr("Calibrate: pick two points of a known dimension, enter the true distance.")
            }
            RowLayout {
                enabled: root.editable
                Button {
                    text: root.pickMode === "calibrate" ? qsTr("Picking… (%1/2)").arg(root.calibratePoints.length) : qsTr("Pick 2 Points")
                    checkable: true
                    checked: root.pickMode === "calibrate"
                    onClicked: {
                        root.pickMode = checked ? "calibrate" : ""
                        if (checked)
                            root.calibratePoints = []
                    }
                }
                Label { text: root.measured > 0 ? qsTr("%1 mm").arg(root.measured.toFixed(3)) : "" }
            }
            RowLayout {
                enabled: root.editable && root.measured > 0
                TextField { id: trueDistance; placeholderText: qsTr("True distance"); Layout.preferredWidth: 110; validator: DoubleValidator { bottom: 0 } }
                ComboBox { id: distanceUnit; model: root.scan.unitSymbols; currentIndex: 1; Layout.preferredWidth: 70 }
                Button {
                    text: qsTr("Scale")
                    enabled: Number(trueDistance.text) > 0
                    onClicked: root.run("part.calibrate", {
                        id: root.partId, a: root.calibratePoints[0], b: root.calibratePoints[1],
                        distance: Number(trueDistance.text), units: distanceUnit.currentText
                    }, r => {
                        root.controller.showStatus(qsTr("Scaled by %1").arg(r.factor.toFixed(6)))
                        root.calibratePoints = []
                        root.pickMode = ""
                    })
                }
            }

            // --- Alignment -------------------------------------------------------------
            Section { text: qsTr("ORIENT") }
            Hint {
                text: qsTr("Pick 3 or more points on a flat reference area; it is turned onto the chosen world plane and moved to the origin.")
            }
            RowLayout {
                enabled: root.editable
                Button {
                    text: root.pickMode === "align" ? qsTr("Picking… (%1)").arg(root.alignPoints.length) : qsTr("Pick Points")
                    checkable: true
                    checked: root.pickMode === "align"
                    onClicked: {
                        root.pickMode = checked ? "align" : ""
                        if (checked)
                            root.alignPoints = []
                    }
                }
                ComboBox {
                    id: targetPlane
                    textRole: "label"
                    valueRole: "value"
                    model: [{ value: "xy", label: qsTr("XY (Top)") }, { value: "xz", label: qsTr("XZ (Front)") },
                            { value: "yz", label: qsTr("YZ (Right)") }]
                }
            }
            RowLayout {
                enabled: root.editable
                CheckBox { id: flipAlign; text: qsTr("Flip") }
                CheckBox { id: keepPosition; text: qsTr("Rotate only") }
                Button {
                    text: qsTr("Align")
                    enabled: root.alignPoints.length >= 3
                    onClicked: root.run("part.align", {
                        id: root.partId, points: root.alignPoints, target: targetPlane.currentValue,
                        flip: flipAlign.checked, keepPosition: keepPosition.checked
                    }, r => {
                        root.controller.showStatus(qsTr("Aligned (plane fit RMS %1 mm)").arg(r.fitRms.toFixed(4)))
                        root.alignPoints = []
                        root.pickMode = ""
                    })
                }
            }
            RowLayout {
                enabled: root.editable
                Button { text: qsTr("Place on Floor"); onClicked: root.run("part.place", { id: root.partId, onFloor: true }) }
                Button { text: qsTr("Center"); onClicked: root.run("part.place", { id: root.partId, onFloor: false }) }
            }

            // --- Display ---------------------------------------------------------------
            Section { text: qsTr("DISPLAY") }
            RowLayout {
                Label { text: qsTr("Max. triangles drawn") }
                ComboBox {
                    id: budget
                    textRole: "label"
                    valueRole: "value"
                    model: [{ value: 250000, label: "250 k" }, { value: 500000, label: "500 k" },
                            { value: 1000000, label: "1 M" }, { value: 2000000, label: "2 M" },
                            { value: 5000000, label: "5 M" }, { value: 0, label: qsTr("All") }]
                    currentIndex: Math.max(0, model.findIndex(m => m.value === root.scan.triangleBudget))
                    onActivated: root.scan.triangleBudget = currentValue
                }
            }
            Hint {
                opacity: 0.6
                font.pixelSize: 11
                text: qsTr("Bigger meshes are drawn simplified; selection, fitting and modelling always use the full mesh.")
            }

            Hint {
                visible: root.message !== ""
                opacity: 1
                color: "#c0392b"
                text: root.message
            }
        }
    }

    Connections {
        target: root.scan
        function onDisplayMeshReady(id) {
            if (Number(id) === Number(root.partId))
                root.lodRevision++
        }
    }

    Connections {
        target: root.controller
        function onPartChanged(id) {
            if (root.visible && id === root.partId)
                root.refreshStats()
        }
    }
}
