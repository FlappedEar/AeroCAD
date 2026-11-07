// SPDX-License-Identifier: GPL-3.0-or-later
// Inspect > Section Analysis. Cuts the model with an origin plane at an offset (slider, value
// or by dragging the arrow in the viewport) and caps the cut. Per part it lists the section
// area and curve length, so a scan and the designed part can be compared at the same plane.
// Runs inspect.section; closing the panel turns the section off (inspect.section enabled:false).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    property bool active: false
    property string plane: "xz"
    property real offset: 0
    property bool flip: false
    property bool cap: true
    property real rangeMin: -50
    property real rangeMax: 50
    property var result: null
    property string error: ""

    visible: active
    padding: 12
    width: 300

    readonly property int axisIndex: plane === "xy" ? 2 : plane === "xz" ? 1 : 0

    function open() {
        updateRange()
        offset = (rangeMin + rangeMax) / 2
        active = true
        apply()
    }

    function close() {
        active = false
        result = null
        controller.execute("inspect.section", { enabled: false })
    }

    function updateRange() {
        const r = controller.execute("document.bounds")
        if (r.ok && r.result.bounds) {
            const lo = r.result.bounds.min[axisIndex]
            const hi = r.result.bounds.max[axisIndex]
            const margin = Math.max((hi - lo) * 0.02, 0.01)
            rangeMin = lo - margin
            rangeMax = hi + margin
        }
    }

    function setPlane(p) {
        plane = p
        updateRange()
        offset = (rangeMin + rangeMax) / 2
        apply()
    }

    /// Moves the plane along its normal (arrow drag in the viewport).
    function moveBy(delta) {
        if (!active)
            return
        // The default normals point along +Z, -Y and +X; flip reverses them.
        const sign = (plane === "xz" ? -1 : 1) * (flip ? -1 : 1)
        offset = Math.max(rangeMin, Math.min(rangeMax, offset + sign * delta))
        applyTimer.restart()
    }

    function apply() {
        if (!active)
            return
        const r = controller.execute("inspect.section", { plane: plane, offset: offset, flip: flip, cap: cap })
        if (r.ok) {
            result = r.result
            error = ""
        } else {
            error = r.error
        }
    }

    // Coalesces slider / drag updates (cutting large scans takes a moment).
    Timer {
        id: applyTimer
        interval: 30
        onTriggered: root.apply()
    }

    // Re-run when the model changes (results and range).
    Connections {
        target: root.controller
        enabled: root.active
        function onBoundsChanged() { root.updateRange(); applyTimer.restart() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Label { text: qsTr("Section Analysis"); font.bold: true; font.pixelSize: 15; Layout.fillWidth: true }
            ToolButton { text: "✕"; onClicked: root.close() }
        }
        GridLayout {
            columns: 2
            columnSpacing: 8
            Label { text: qsTr("Plane") }
            ComboBox {
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                model: [
                    { value: "xy", text: qsTr("XY (Top)") },
                    { value: "xz", text: qsTr("XZ (Front)") },
                    { value: "yz", text: qsTr("YZ (Right)") }
                ]
                currentIndex: ["xy", "xz", "yz"].indexOf(root.plane)
                onActivated: root.setPlane(currentValue)
            }
            Label { text: qsTr("Offset") + " (" + ["X", "Y", "Z"][root.axisIndex] + ")" }
            TextField {
                Layout.fillWidth: true
                text: root.offset.toFixed(2)
                validator: DoubleValidator {}
                onEditingFinished: {
                    const v = Number(text.replace(",", "."))
                    if (isFinite(v)) {
                        root.offset = v
                        root.apply()
                    }
                }
            }
        }
        Slider {
            Layout.fillWidth: true
            from: root.rangeMin
            to: root.rangeMax
            value: root.offset
            onMoved: {
                root.offset = value
                applyTimer.restart()
            }
        }
        RowLayout {
            CheckBox { text: qsTr("Flip"); checked: root.flip; onToggled: { root.flip = checked; root.apply() } }
            CheckBox { text: qsTr("Cap"); checked: root.cap; onToggled: { root.cap = checked; root.apply() } }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("Drag the blue arrow in the view to move the plane. Caps: red = bodies, blue = imported meshes / scans.")
        }

        // Per-part section results.
        Repeater {
            model: root.result ? root.result.parts : []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: modelData.name + (modelData.kind === "body" ? "" : " " + qsTr("(mesh)"))
                    font.bold: true
                    elide: Text.ElideRight
                }
                Label {
                    opacity: 0.8
                    text: (modelData.closedCurves === modelData.curves
                               ? qsTr("Area %1 mm²").arg(Number(modelData.area).toFixed(2))
                               : qsTr("%1 of %2 curves open").arg(modelData.curves - modelData.closedCurves).arg(modelData.curves))
                          + " · " + qsTr("length %1 mm").arg(Number(modelData.length).toFixed(2))
                }
            }
        }
        Label {
            visible: !!root.result && root.result.parts.length === 0
            text: qsTr("The plane does not cut any part.")
            opacity: 0.7
        }
        Label {
            visible: root.error !== ""
            text: root.error
            color: "#c62828"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Close"); onClicked: root.close() }
        }
    }
}
