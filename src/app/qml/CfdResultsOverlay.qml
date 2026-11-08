// SPDX-License-Identifier: GPL-3.0-or-later
// Wind tunnel results in the viewport (CAD-128): a vertical colour bar for each quantity
// shown in 3D (title with unit, ticks, zero marked on pressure scales) and a card with the
// forces and coefficients of the run, so a screenshot of the view can be read on its own.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Item {
    id: root

    required property AppController controller
    /// SimulationPanel.viewportFields: [{run, kind, scalar, min, max, palette}].
    property var fields: []
    /// SimulationPanel.results of the displayed run (summary, conditions), or null.
    property var results: null
    /// Run name shown on the card ("A: ..." / "B: ...").
    property string runTitle: ""
    /// simulation.compare result while the difference map B - A is shown (CAD-141), or null:
    /// the card then adds the change of the forces and coefficients.
    property var comparison: null

    /// One legend per distinct scale: [{scalar, min, max, palette, kinds}].
    readonly property var legends: {
        const out = []
        for (const f of fields) {
            if (!(f.max > f.min))
                continue
            const palette = f.palette || ""
            const same = out.find(l => l.scalar === f.scalar && l.min === f.min && l.max === f.max && l.palette === palette)
            if (same)
                same.kinds.push(f.kind)
            else
                out.push({ scalar: f.scalar, min: f.min, max: f.max, palette: palette, kinds: [f.kind] })
        }
        return out
    }

    function title(scalar) {
        return scalar === "Cp" ? qsTr("Pressure coefficient Cp")
             : scalar === "dCp" ? qsTr("Cp change, B − A")
             : scalar === "pressure" ? qsTr("Pressure (Pa)")
             : scalar === "speed" ? qsTr("Air speed (m/s)") : scalar
    }
    function kindName(kind) {
        return kind === "surface" ? qsTr("on the part")
             : kind === "streamlines" ? qsTr("streamlines")
             : kind === "sectionHorizontal" ? qsTr("horizontal section")
             : kind === "sectionVertical" || kind.indexOf("sectionY_") === 0 ? qsTr("vertical section")
             : kind.indexOf("sectionZ_") === 0 ? qsTr("horizontal section")
             : kind.indexOf("sectionX_") === 0 ? qsTr("section across the flow") : kind
    }
    /// Tick values from bottom to top: five evenly spaced, with zero on pressure scales.
    function ticks(l) {
        const pressure = l.scalar === "Cp" || l.scalar === "pressure" || l.scalar === "dCp"
        if (pressure && l.min < 0 && l.max > 0)
            return [l.min, l.min / 2, 0, l.max / 2, l.max]
        const out = []
        for (let i = 0; i <= 4; ++i)
            out.push(l.min + (l.max - l.min) * i / 4)
        return out
    }
    function fmt(v, span) {
        const digits = span >= 50 ? 0 : span >= 5 ? 1 : 2
        const s = Number(v).toFixed(digits)
        return Number(s) === 0 ? Number(0).toFixed(digits) : s // No "-0.00".
    }
    function signed(v, digits) {
        if (v === null || v === undefined || !isFinite(v))
            return "–"
        const s = Number(v).toFixed(digits)
        return (Number(s) > 0 ? "+" : "") + s
    }
    /// Force change "+3.00 N (+5.4 %)" from a simulation.compare entry {difference, percent}.
    function signedN(d) {
        if (!d)
            return "–"
        const pct = d.percent !== null && d.percent !== undefined && isFinite(d.percent)
                  ? " (" + signed(d.percent, 1) + " %)" : ""
        return signed(d.difference, 2) + " N" + pct
    }
    function num(v, digits) {
        return v === null || v === undefined || !isFinite(v) ? "–" : Number(v).toFixed(digits)
    }

    visible: fields.length > 0

    // --- Colour bars, left side, below the selection tools ---------------------------------
    Column {
        id: legendColumn
        objectName: "cfdLegends"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 12
        anchors.topMargin: 60
        spacing: 8

        Repeater {
            model: root.legends
            delegate: Pane {
                id: legend
                required property var modelData
                readonly property var tickValues: root.ticks(modelData)
                readonly property var colors: root.controller.colormap(modelData.scalar, modelData.palette)
                objectName: "cfdLegend_" + modelData.scalar
                padding: 8
                opacity: 0.94

                ColumnLayout {
                    spacing: 4
                    Label { text: root.title(legend.modelData.scalar); font.bold: true }
                    Label {
                        text: legend.modelData.scalar === "dCp" ? qsTr("on part A · grey: shapes differ")
                                                                : legend.modelData.kinds.map(k => root.kindName(k)).join(", ")
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }
                    Item {
                        implicitWidth: 18 + 6 + tickColumn.implicitWidth
                        implicitHeight: 130
                        Rectangle {
                            id: bar
                            width: 18
                            height: parent.height
                            border.color: Theme.borderStrong
                            // Top = high values; the stops follow the colour scale, so a value
                            // sits at the same height as its colour on the model.
                            gradient: Gradient {
                                stops: legend.colors.map((c, i) => stopComponent.createObject(bar, {
                                    position: 1 - i / (legend.colors.length - 1), color: c }))
                            }
                        }
                        Item {
                            id: tickColumn
                            x: bar.width + 6
                            height: parent.height
                            implicitWidth: 56
                            Repeater {
                                model: legend.tickValues
                                delegate: Item {
                                    required property real modelData
                                    readonly property real t: root.controller.fieldScalePosition(
                                        legend.modelData.scalar, modelData, legend.modelData.min, legend.modelData.max)
                                    y: (1 - t) * tickColumn.height
                                    Rectangle { x: -10; width: 6; height: 1; color: Theme.text }
                                    Label {
                                        anchors.verticalCenter: parent.top
                                        font.pixelSize: 11
                                        font.bold: parent.modelData === 0 && legend.modelData.scalar !== "speed"
                                        text: root.fmt(parent.modelData, legend.modelData.max - legend.modelData.min)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: stopComponent
        GradientStop {}
    }

    // --- Forces card, right side ------------------------------------------------------------
    Pane {
        id: card
        objectName: "cfdSummary"
        readonly property var s: root.results ? root.results.summary : null
        readonly property var c: root.results ? root.results.conditions : null
        visible: !!s
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 12
        anchors.bottomMargin: 12
        padding: 10
        opacity: 0.94

        ColumnLayout {
            spacing: 4
            Label {
                text: root.runTitle
                font.bold: true
                visible: text.length > 0
            }
            GridLayout {
                columns: 4
                columnSpacing: 10
                rowSpacing: 2
                Label { text: qsTr("Drag"); color: Theme.textMuted }
                Label { objectName: "cfdSummaryDrag"; text: root.num(card.s?.dragN, 2) + " N"; font.bold: true }
                Label { text: "Cd"; color: Theme.textMuted }
                Label { text: root.num(card.s?.cd, 3); font.bold: true }
                Label { text: qsTr("Lift"); color: Theme.textMuted }
                Label { text: root.num(card.s?.liftN, 2) + " N"; font.bold: true }
                Label { text: "Cl"; color: Theme.textMuted }
                Label { text: root.num(card.s?.cl, 3); font.bold: true }
                Label { text: qsTr("Side"); color: Theme.textMuted }
                Label { text: root.num(card.s?.sideN, 2) + " N"; font.bold: true }
                Label { text: "Cs"; color: Theme.textMuted }
                Label { text: root.num(card.s?.cs, 3); font.bold: true }
            }
            GridLayout {
                id: deltaGrid
                objectName: "cfdSummaryDelta"
                readonly property var f: root.comparison ? root.comparison.forces : null
                readonly property var k: root.comparison ? root.comparison.coefficientsOnReferenceAreaOfA : null
                visible: !!f && !!k
                columns: 4
                columnSpacing: 10
                rowSpacing: 2
                Label {
                    Layout.columnSpan: 4
                    text: qsTr("Change B − A")
                    font.bold: true
                    topPadding: 4
                }
                Label { text: qsTr("Drag"); color: Theme.textMuted }
                Label { objectName: "cfdSummaryDeltaDrag"; text: root.signedN(deltaGrid.f?.dragN); font.bold: true }
                Label { text: "Cd"; color: Theme.textMuted }
                Label { text: root.signed(deltaGrid.k?.cd?.difference, 3); font.bold: true }
                Label { text: qsTr("Lift"); color: Theme.textMuted }
                Label { text: root.signedN(deltaGrid.f?.liftN); font.bold: true }
                Label { text: "Cl"; color: Theme.textMuted }
                Label { text: root.signed(deltaGrid.k?.cl?.difference, 3); font.bold: true }
            }
            Label {
                visible: !!card.c
                font.pixelSize: 11
                color: Theme.textMuted
                text: card.c ? qsTr("%1 m/s · flow %2 · %3 mesh").arg(card.c.settings.speed)
                                   .arg(card.c.settings.flowDirection).arg(card.c.settings.mesh) : ""
            }
        }
    }
}
