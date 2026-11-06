// SPDX-License-Identifier: GPL-3.0-or-later
// Inspect > Deviation Map / Thickness Map. Deviation colours a part (usually a scan) by its
// signed distance to a reference (usually the designed body); Thickness colours a part by wall
// thickness. Runs inspect.deviation / inspect.thickness; Clear runs inspect.clear.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    property bool active: false
    property string kind: "deviation"
    property var parts: []
    property var measured: 0
    property var reference: 0
    property bool autoRange: true
    property real range: 1
    property real tolerance: 0.1
    property var result: null
    property string error: ""

    visible: active
    padding: 12
    width: 300

    function open(mode, selectedId) {
        kind = mode
        const r = controller.execute("document.parts")
        parts = r.ok ? r.result.parts.filter(p => p.kind !== "sketch" && p.triangles > 0) : []
        // Defaults: the selected part, else the first mesh is measured, the first body the reference.
        const mesh = parts.find(p => p.kind === "imported")
        const body = parts.find(p => p.kind === "body")
        measured = selectedId && parts.some(p => p.id === selectedId) ? selectedId : (mesh || parts[0] || { id: 0 }).id
        const ref = parts.find(p => p.id !== measured && p.kind === "body") || parts.find(p => p.id !== measured)
        reference = ref ? ref.id : 0
        if (kind === "thickness" && !selectedId && body)
            measured = body.id
        result = null
        error = ""
        active = true
    }

    function close() {
        active = false
    }

    function run() {
        let r
        if (kind === "deviation") {
            const params = { measured: measured, reference: reference, tolerance: tolerance }
            if (!autoRange)
                params.range = range
            r = controller.execute("inspect.deviation", params)
        } else {
            r = controller.execute("inspect.thickness", { id: measured })
        }
        if (r.ok) {
            result = r.result
            error = ""
        } else {
            result = null
            error = r.error
        }
    }

    function clear() {
        controller.execute("inspect.clear", { what: "heatmap" })
        result = null
    }

    function f(v) {
        return Number(v).toFixed(3)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Label {
                text: root.kind === "deviation" ? qsTr("Deviation Map") : qsTr("Thickness Map")
                font.bold: true
                font.pixelSize: 15
                Layout.fillWidth: true
            }
            ToolButton { text: "✕"; onClicked: root.close() }
        }
        GridLayout {
            columns: 2
            columnSpacing: 8
            Label { text: root.kind === "deviation" ? qsTr("Measured") : qsTr("Part") }
            ComboBox {
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "id"
                model: root.parts
                currentIndex: Math.max(0, root.parts.findIndex(p => p.id === root.measured))
                onActivated: root.measured = currentValue
            }
            Label { text: qsTr("Reference"); visible: root.kind === "deviation" }
            ComboBox {
                visible: root.kind === "deviation"
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "id"
                model: root.parts
                currentIndex: Math.max(0, root.parts.findIndex(p => p.id === root.reference))
                onActivated: root.reference = currentValue
            }
            Label { text: qsTr("Tolerance ±"); visible: root.kind === "deviation" }
            TextField {
                visible: root.kind === "deviation"
                Layout.fillWidth: true
                text: root.tolerance.toString()
                validator: DoubleValidator { bottom: 0 }
                onEditingFinished: root.tolerance = Math.max(1e-6, Number(text.replace(",", ".")) || 0.1)
            }
            CheckBox {
                visible: root.kind === "deviation"
                text: qsTr("Auto range")
                checked: root.autoRange
                onToggled: root.autoRange = checked
            }
            TextField {
                visible: root.kind === "deviation"
                enabled: !root.autoRange
                Layout.fillWidth: true
                text: root.range.toString()
                validator: DoubleValidator { bottom: 0 }
                onEditingFinished: root.range = Math.max(1e-6, Number(text.replace(",", ".")) || 1)
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: root.kind === "deviation"
                  ? qsTr("Red: measured surface outside the reference, blue: inside. Values in mm.")
                  : qsTr("Red: thin walls, blue: thick. Values in mm.")
        }
        GridLayout {
            visible: !!root.result
            columns: 2
            columnSpacing: 8
            Label { text: qsTr("Min"); opacity: 0.7 }
            Label { text: root.result ? root.f(root.result.stats.min) : "" }
            Label { text: qsTr("Max"); opacity: 0.7 }
            Label { text: root.result ? root.f(root.result.stats.max) : "" }
            Label { text: qsTr("Mean"); opacity: 0.7 }
            Label { text: root.result ? root.f(root.result.stats.mean) : "" }
            Label { text: qsTr("RMS"); opacity: 0.7; visible: root.kind === "deviation" }
            Label { text: root.result ? root.f(root.result.stats.rms) : ""; visible: root.kind === "deviation" }
            Label { text: qsTr("Within ±%1").arg(root.tolerance); opacity: 0.7; visible: !!root.result && root.result.withinTolerance !== undefined }
            Label {
                visible: !!root.result && root.result.withinTolerance !== undefined
                text: root.result && root.result.withinTolerance !== undefined ? Number(root.result.withinTolerance).toFixed(1) + " %" : ""
                font.bold: true
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
            Button {
                text: qsTr("Compute")
                highlighted: true
                enabled: !!root.measured && (root.kind !== "deviation" || (!!root.reference && root.reference !== root.measured))
                onClicked: root.run()
            }
            Button { text: qsTr("Clear"); onClicked: root.clear() }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Close"); onClicked: root.close() }
        }
    }
}
