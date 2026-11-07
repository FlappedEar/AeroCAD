// SPDX-License-Identifier: GPL-3.0-or-later
// Inspect > Measure (Fusion: I). Click one or two selections in the viewport; the panel shows
// their properties (and the wall thickness under one face or mesh point), the minimum
// distance with X/Y/Z deltas and the angle between planar faces, straight edges and axes.
// Sketch lines can be picked too. Body selection measures the clearance between parts (with interference
// for B-rep solids). Everything runs through inspect.measure / inspect.clearance.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    /// Function returning the current pick radius in mm (Viewport.pickTolerance).
    property var pickTolerance: () => 0.5
    property bool active: false
    /// Selection filter (Fusion's Measure "Selection Filter").
    property string filter: "auto"
    property int precision: 2
    property var selA: null
    property var selB: null
    property var result: null
    property string error: ""

    /// Dimensions kept in the view (inspect.dimensions), measured again after every change.
    property var kept: []
    /// Kept dimensions as viewport overlay lines; shown whether the panel is open or not.
    readonly property var keptLines: kept.filter(d => d.value !== undefined).map(d => ({
        a: d.pointA, b: d.pointB, label: dimensionLabel(d), color: "#1f6fd1"
    }))

    function refreshKept() {
        const r = controller.execute("inspect.dimensions", {})
        kept = r.ok ? r.result.dimensions : []
    }

    function dimensionLabel(d) {
        if (d.value === undefined)
            return ""
        if (d.show === "angle")
            return fmt(d.value) + "°"
        const prefix = d.show === "thickness" ? "t " : d.show === "radius" ? "R " : d.show === "length" ? "L " : ""
        return prefix + fmt(d.value) + " mm"
    }

    function dimensionName(d) {
        const what = d.show === "distance" ? qsTr("Distance") : d.show === "angle" ? qsTr("Angle")
                   : d.show === "thickness" ? qsTr("Thickness") : d.show === "radius" ? qsTr("Radius") : qsTr("Length")
        return what + ": " + (d.value !== undefined ? dimensionLabel(d) : qsTr("not found"))
    }

    /// What "Keep in view" keeps for the current result: distance for two selections,
    /// otherwise the thickness, radius or length of the one.
    readonly property string keepShow: {
        if (!result)
            return ""
        if (selB)
            return result.distance !== undefined ? "distance" : ""
        if (result.thickness)
            return "thickness"
        if (result.a && result.a.radius !== undefined)
            return "radius"
        if (result.a && result.a.length !== undefined)
            return "length"
        return ""
    }

    function keep(show) {
        if (!selA || !show)
            return
        const params = { a: strip(selA), show: show }
        if (selB)
            params.b = strip(selB)
        const r = controller.execute("inspect.add_dimension", params)
        if (r.ok) {
            refreshKept()
            restart()
        } else {
            error = r.error
        }
    }

    function removeKept(id) {
        const params = id ? { id: id } : { all: true }
        controller.execute("inspect.remove_dimension", params)
        refreshKept()
    }

    Connections {
        target: root.controller
        function onInspectChanged() { root.refreshKept() }
        function onHistoryChanged() { root.refreshKept() }
        function onBoundsChanged() { root.refreshKept() }
    }
    Component.onCompleted: refreshKept()

    /// Lines for the viewport overlay.
    readonly property var overlayLines: {
        if (!active || !result)
            return []
        if (result.pointA && result.pointB)
            return [{ a: result.pointA, b: result.pointB, label: fmt(result.distance) + " mm" }]
        if (result.thickness)
            return [{ a: result.thickness.pointA, b: result.thickness.pointB,
                      label: qsTr("t %1 mm").arg(fmt(result.thickness.value)) }]
        return []
    }
    /// Points to mark for single selections.
    readonly property var markers: {
        if (!active)
            return []
        const out = []
        for (const s of [selA, selB])
            if (s && s.point && !(result && (result.pointA || result.thickness)))
                out.push(s.point)
        return out
    }

    visible: active
    padding: 12
    width: 300

    function open(initialFilter) {
        filter = initialFilter || "auto"
        restart()
        active = true
    }

    function close() {
        active = false
        restart()
    }

    function restart() {
        selA = null
        selB = null
        result = null
        error = ""
    }

    function fmt(v) {
        return Number(v).toFixed(precision)
    }

    function fmtVec(v) {
        return v ? v.map(x => fmt(x)).join(", ") : ""
    }

    function selection(info, point) {
        const p = [point.x, point.y, point.z]
        if (filter === "body")
            return { part: info.id, pick: "body", point: p, name: info.name }
        if (filter === "point")
            return { part: info.id, pick: "point", point: p, name: info.name }
        return { part: info.id, pick: "auto", point: p, tolerance: pickTolerance(), name: info.name }
    }

    /// Viewport click on a part.
    function addPick(info, point) {
        if (!active || !info.id)
            return
        const s = selection(info, point)
        if (!selA || selB) {
            selA = s
            selB = null
        } else {
            selB = s
        }
        run()
    }

    function strip(s) {
        const out = { part: s.part, pick: s.pick, point: s.point }
        if (s.tolerance)
            out.tolerance = s.tolerance
        return out
    }

    function run() {
        error = ""
        if (!selA) {
            result = null
            return
        }
        let r
        if (selB && filter === "body" && selA.part !== selB.part) {
            r = controller.execute("inspect.clearance", { a: selA.part, b: selB.part })
            if (r.ok) {
                const m = controller.execute("inspect.measure", { a: strip(selA), b: strip(selB) })
                r.result.a = m.ok ? m.result.a : null
                r.result.b = m.ok ? m.result.b : null
                r.result.delta = [0, 1, 2].map(i => r.result.pointB[i] - r.result.pointA[i])
            }
        } else {
            const params = { a: strip(selA) }
            if (selB)
                params.b = strip(selB)
            r = controller.execute("inspect.measure", params)
        }
        if (r.ok) {
            result = r.result
        } else {
            result = null
            error = r.error
        }
    }

    function describe(e) {
        if (!e)
            return ""
        const kind = e.kind === "point" ? qsTr("Point") : e.kind === "vertex" ? qsTr("Vertex")
                   : e.kind === "edge" ? qsTr("Edge") : e.kind === "face" ? qsTr("Face") : qsTr("Body")
        const parts = [kind + (e.type && e.type !== "vertex" ? " (" + e.type + ")" : "")]
        if (e.partName)
            parts.push(e.partName)
        return parts.join(" · ")
    }

    function details(e) {
        if (!e)
            return []
        const out = []
        if (e.length !== undefined) out.push([qsTr("Length"), fmt(e.length) + " mm"])
        if (e.radius !== undefined) out.push([qsTr("Radius"), fmt(e.radius) + " mm"])
        if (e.area !== undefined) out.push([qsTr("Area"), fmt(e.area) + " mm²"])
        if (e.volume !== undefined) out.push([qsTr("Volume"), fmt(e.volume) + " mm³"])
        if (e.position !== undefined) out.push([qsTr("Position"), fmtVec(e.position)])
        return out
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Label { text: qsTr("Measure"); font.bold: true; font.pixelSize: 15; Layout.fillWidth: true }
            ToolButton { text: "✕"; onClicked: root.close() }
        }
        GridLayout {
            columns: 2
            columnSpacing: 8
            Label { text: qsTr("Selection filter") }
            ComboBox {
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                model: [
                    { value: "auto", text: qsTr("Face / Edge / Vertex") },
                    { value: "body", text: qsTr("Body (clearance)") },
                    { value: "point", text: qsTr("Point") }
                ]
                currentIndex: Math.max(0, ["auto", "body", "point"].indexOf(root.filter))
                onActivated: { root.filter = currentValue; root.restart() }
            }
            Label { text: qsTr("Precision") }
            ComboBox {
                Layout.fillWidth: true
                model: ["0", "0.0", "0.00", "0.000", "0.0000"]
                currentIndex: root.precision
                onActivated: root.precision = currentIndex
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: !root.selA ? qsTr("Click the first selection.")
                 : !root.selB ? qsTr("Click a second selection to measure between them.")
                 : qsTr("Click again to start a new measurement.")
        }

        // Selections.
        Repeater {
            model: root.result ? [["1", root.result.a], ["2", root.result.b]].filter(x => !!x[1]) : []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 2
                Label { text: modelData[0] + ": " + root.describe(modelData[1]); font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                Repeater {
                    model: root.details(modelData[1])
                    delegate: RowLayout {
                        required property var modelData
                        Label { text: modelData[0]; opacity: 0.7; Layout.preferredWidth: 70 }
                        Label { text: modelData[1]; Layout.fillWidth: true; elide: Text.ElideRight }
                    }
                }
            }
        }

        // Wall thickness under a single selection (a face of a solid, a closed mesh).
        RowLayout {
            visible: !!root.result && !!root.result.thickness
            Label { text: qsTr("Thickness"); font.bold: true; Layout.preferredWidth: 70 }
            Label {
                text: root.result && root.result.thickness ? root.fmt(root.result.thickness.value) + " mm" : ""
                font.bold: true
            }
        }

        // Results.
        GridLayout {
            visible: !!root.result && root.result.distance !== undefined
            columns: 2
            columnSpacing: 8
            Label { text: qsTr("Distance"); font.bold: true }
            Label {
                text: root.result && root.result.distance !== undefined ? root.fmt(root.result.distance) + " mm" : ""
                font.bold: true
            }
            Label { text: "ΔX"; opacity: 0.7 }
            Label { text: root.result && root.result.delta ? root.fmt(root.result.delta[0]) + " mm" : "" }
            Label { text: "ΔY"; opacity: 0.7 }
            Label { text: root.result && root.result.delta ? root.fmt(root.result.delta[1]) + " mm" : "" }
            Label { text: "ΔZ"; opacity: 0.7 }
            Label { text: root.result && root.result.delta ? root.fmt(root.result.delta[2]) + " mm" : "" }
            Label { text: qsTr("Angle"); visible: !!root.result && root.result.angle !== undefined; font.bold: true }
            Label {
                visible: !!root.result && root.result.angle !== undefined
                text: root.result && root.result.angle !== undefined ? root.fmt(root.result.angle) + "°" : ""
                font.bold: true
            }
            Label { text: qsTr("Interference"); visible: !!root.result && root.result.interference !== undefined }
            Label {
                visible: !!root.result && root.result.interference !== undefined
                color: root.result && root.result.interference === true ? "#c62828" : palette.text
                text: !root.result || root.result.interference === undefined ? ""
                    : root.result.interference === null ? qsTr("not checked (mesh)")
                    : root.result.interference ? qsTr("yes, %1 mm³").arg(root.fmt(root.result.interferenceVolume))
                    : qsTr("none")
            }
            Label { text: qsTr("Method"); opacity: 0.7; visible: !!root.result && !!root.result.method }
            Label {
                visible: !!root.result && !!root.result.method
                opacity: 0.7
                text: root.result && root.result.method === "mesh" ? qsTr("mesh surface") : qsTr("exact (B-rep)")
            }
        }

        Label {
            visible: root.error !== ""
            text: root.error
            color: "#c62828"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            visible: root.keepShow !== ""
            Button {
                text: qsTr("Keep in view")
                highlighted: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Keep this %1 as a dimension in the view; it updates when the model changes and is saved with the project").arg(root.keepShow)
                onClicked: root.keep(root.keepShow)
            }
            Button {
                visible: !!root.result && root.result.angle !== undefined
                text: qsTr("Keep angle")
                onClicked: root.keep("angle")
            }
        }

        // Dimensions kept in the view.
        Label {
            visible: root.kept.length > 0
            text: qsTr("In the view")
            font.bold: true
        }
        // Scrolls when many are kept, so every remove button stays reachable.
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 170)
            visible: root.kept.length > 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            model: root.kept
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: root.dimensionName(modelData)
                    color: modelData.value !== undefined ? palette.text : "#c62828"
                }
                ToolButton {
                    text: "✕"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Remove from the view")
                    onClicked: root.removeKept(modelData.id)
                }
            }
        }
        Button {
            visible: root.kept.length > 1
            text: qsTr("Remove all")
            flat: true
            onClicked: root.removeKept(0)
        }

        RowLayout {
            Button { text: qsTr("Restart"); onClicked: root.restart() }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Close"); onClicked: root.close() }
        }
    }
}
