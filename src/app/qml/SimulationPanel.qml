// SPDX-License-Identifier: GPL-3.0-or-later
// Wind tunnel panel: start a CFD run for the selected part, follow its progress, read the
// results (forces in N, Cd/Cl, charts, quality) and compare two runs (A/B). Everything
// goes through the simulation.* commands, the same ones an AI agent uses over MCP.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property AppController controller
    property var selectedPartId: 0

    /// Fields to draw in the viewport: [{run, kind, scalar, min, max}].
    readonly property var viewportFields: {
        const out = []
        const run = displayedRun
        if (!run)
            return out
        for (const f of fieldChoices) {
            if (!f.on || !fieldStats[run + "/" + f.kind])
                continue
            const r = scaleFor(f.kind, f.scalar)
            out.push({ run: run, kind: f.kind, scalar: f.scalar, min: r[0], max: r[1] })
        }
        return out
    }
    /// Parts hidden while their pressure map is shown (they would overlap).
    readonly property var hiddenPartIds: {
        const f = fieldChoices.find(c => c.kind === "surface")
        const run = runs.find(r => r.id === displayedRun)
        // Only when the open document still has that part (ids are per document).
        return f && f.on && run && run.source && run.sourceInDocument && fieldStats[run.id + "/surface"]
            ? [run.source.partId] : []
    }
    /// Run whose fields are drawn: B while "Show B in 3D" is on, otherwise A.
    readonly property string displayedRun: showB && compareRun ? compareRun : activeRun

    property var environment: null
    property var runs: []
    property string activeRun: ""
    property string compareRun: ""
    property bool showB: false
    property var results: null
    property var comparison: null
    property var fieldStats: ({})
    property string error: ""
    /// True after "Install for me" until the engine is ready (the environment is polled).
    property bool installing: false
    property string setupMessage: ""
    /// 0 = Run (setup and runs), 1 = Results, 2 = A/B.
    property alias tab: tabBar.currentIndex
    property var fieldChoices: [
        { kind: "surface", scalar: "Cp", label: qsTr("Surface pressure (Cp)"), on: true },
        { kind: "sectionHorizontal", scalar: "speed", label: qsTr("Horizontal section (speed)"), on: false },
        { kind: "sectionVertical", scalar: "speed", label: qsTr("Vertical section (speed)"), on: false },
        { kind: "streamlines", scalar: "speed", label: qsTr("Streamlines (speed)"), on: true }
    ]

    readonly property bool anyRunning: runs.some(r => r.state === "running")
    readonly property var activeStatus: runs.find(r => r.id === activeRun) ?? null
    readonly property var partInfo: selectedPartId ? controller.partInfo(selectedPartId) : ({})

    function num(v, digits) {
        return v === null || v === undefined || !isFinite(v) ? "–" : Number(v).toFixed(digits)
    }

    function checkEnvironment() {
        const r = controller.execute("simulation.environment")
        environment = r.ok ? r.result : null
        if (!installing || !environment)
            return
        const pull = environment.imagePull
        if (pull && pull.state === "running")
            return
        if (pull && pull.state === "failed") {
            // Stop here and show why; pressing the button again retries.
            installing = false
            setupMessage = pull.message
        } else if (environment.setup.action === "pull" && !pull) {
            // Docker came up while installing: go on with the image download (once).
            installEngine()
        } else if (environment.setup.action === "none") {
            installing = false
            setupMessage = ""
        }
    }

    /// One "Install for me" step (simulation.install_engine), then keep checking.
    function installEngine() {
        const r = controller.execute("simulation.install_engine")
        if (!r.ok) {
            setupMessage = r.error
            return
        }
        installing = true
        setupMessage = r.result.message
        checkEnvironment()
    }

    function refreshRuns() {
        const r = controller.execute("simulation.list")
        if (!r.ok)
            return
        const stateOf = id => {
            const s = runs.find(x => x.id === id)
            return s ? s.state : ""
        }
        const beforeA = stateOf(activeRun), beforeB = stateOf(compareRun)
        const signature = list => list.map(x => x.id + ":" + x.state).join(",")
        const beforeAll = signature(runs)
        runs = r.result.runs
        if (!activeRun && runs.length > 0) {
            selectRun(runs[runs.length - 1].id)
            return
        }
        const nowA = stateOf(activeRun), nowB = stateOf(compareRun)
        const changedA = nowA === "running" || nowA !== beforeA
        const changedB = !!compareRun && (nowB === "running" || nowB !== beforeB)
        if (changedA)
            loadResults() // Also reloads the field statistics of A and B.
        else if (changedB)
            loadFieldStats()
        if (changedA || changedB || (compareRun && signature(runs) !== beforeAll))
            loadComparison() // Also when runs on other mesh levels finish (mesh check).
    }

    function selectRun(id) {
        activeRun = id
        showB = false
        loadResults()
        loadComparison()
    }

    function loadResults() {
        if (!activeRun) {
            results = null
            return
        }
        const r = controller.execute("simulation.results", { run: activeRun, maxPoints: 300 })
        results = r.ok ? r.result : null
        loadFieldStats()
    }

    function loadFieldStats() {
        const stats = {}
        for (const id of [activeRun, compareRun]) {
            if (!id)
                continue
            const status = runs.find(r => r.id === id)
            if (!status || status.state !== "finished")
                continue // Fields are written at the end of a run.
            for (const f of fieldChoices) {
                const r = controller.execute("simulation.field", { run: id, kind: f.kind })
                if (r.ok && r.result.available)
                    stats[id + "/" + f.kind] = r.result.scalars
            }
        }
        fieldStats = stats
    }

    function loadComparison() {
        if (!activeRun || !compareRun || activeRun === compareRun) {
            comparison = null
            return
        }
        const r = controller.execute("simulation.compare", { a: activeRun, b: compareRun })
        comparison = r.ok ? r.result : null
    }

    /// Colour scale for a field: shared between A and B in compare mode. Pressure maps are
    /// symmetric around zero, clipped to Cp in [-2, 1] where the data allows.
    function scaleFor(kind, scalar) {
        let lo = Infinity, hi = -Infinity
        for (const id of comparison ? [activeRun, compareRun] : [activeRun]) {
            const s = fieldStats[id + "/" + kind]
            if (s && s[scalar]) {
                lo = Math.min(lo, s[scalar].min)
                hi = Math.max(hi, s[scalar].max)
            }
        }
        if (!isFinite(lo))
            return [0, 0]
        if (scalar === "Cp")
            return [Math.max(lo, -2), Math.min(hi, 1)]
        return [0, hi]
    }

    function startRun() {
        error = ""
        if (!partInfo.id) {
            error = qsTr("Select a part first.")
            return
        }
        const params = {
            id: partInfo.id,
            speed: Number(speedField.text),
            flowDirection: flowBox.currentValue,
            mesh: meshBox.currentValue,
            ground: groundBox.checked
        }
        if (labelField.text.length > 0)
            params.label = labelField.text
        if (contextBox.checked) {
            const parts = controller.execute("document.parts")
            if (parts.ok)
                params.context = parts.result.parts
                    .filter(p => p.id !== partInfo.id && p.visible && p.kind !== "sketch").map(p => p.id)
        }
        const r = controller.execute("simulation.run", params)
        if (!r.ok) {
            error = r.error
            return
        }
        refreshRuns()
        selectRun(r.result.id)
        tab = 1
    }

    /// Runs A and B again on another mesh level, for the mesh check of the comparison.
    function refineComparison(mesh) {
        error = ""
        for (const id of [activeRun, compareRun]) {
            const r = controller.execute("simulation.refine", { run: id, mesh: mesh })
            if (!r.ok) {
                error = r.error
                controller.setStatus(r.error)
                return
            }
        }
        refreshRuns()
        loadComparison()
    }

    function setField(index, on) {
        const copy = fieldChoices.slice()
        copy[index] = Object.assign({}, copy[index], { on: on })
        fieldChoices = copy
    }

    function series(name, color, x, y) {
        return { name: name, color: color, x: x || [], y: y || [] }
    }

    onVisibleChanged: if (visible) { if (!environment) checkEnvironment(); refreshRuns() }
    Component.onCompleted: if (visible) { checkEnvironment(); refreshRuns() }

    Timer {
        interval: 4000
        repeat: true
        running: root.visible && root.installing
        onTriggered: root.checkEnvironment()
    }

    Timer {
        interval: root.anyRunning ? 1500 : 5000
        repeat: true
        running: root.visible || root.anyRunning
        onTriggered: root.refreshRuns()
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 10

            Label { text: qsTr("Wind tunnel (CFD)"); font.bold: true; font.pixelSize: 15 }

            // --- Engine ---------------------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    width: 10; height: 10; radius: 5
                    color: !root.environment ? Theme.textFaint : root.environment.recommendedBackend ? Theme.success : Theme.danger
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    text: root.environment ? root.environment.message : qsTr("Checking the simulation engine...")
                }
                Button { text: qsTr("Check"); onClicked: root.checkEnvironment() }
            }

            // "Install for me": the next setup step (download the image, start or install
            // Colima (the Docker engine) with Homebrew, or open an install page), then follow it until ready.
            ColumnLayout {
                readonly property var setup: root.environment ? root.environment.setup : null
                readonly property var pull: root.environment ? root.environment.imagePull : null
                readonly property bool pulling: !!pull && pull.state === "running"
                id: setupBox
                Layout.fillWidth: true
                visible: !!setup && (setup.action !== "none" || pulling || root.installing)
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        visible: !!setupBox.setup && setupBox.setup.action !== "none" && setupBox.setup.action !== "manual"
                                 && !setupBox.pulling
                        highlighted: true
                        text: qsTr("Install for me")
                        onClicked: root.installEngine()
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: Theme.textMuted
                        text: setupBox.pulling || (setupBox.pull && setupBox.pull.state === "failed") ? setupBox.pull.message
                            : root.setupMessage.length > 0 ? root.setupMessage
                            : setupBox.setup ? setupBox.setup.message : ""
                    }
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: setupBox.pulling
                    from: 0; to: 1
                    value: setupBox.pull ? setupBox.pull.progress : 0
                    indeterminate: !!setupBox.pull && setupBox.pull.progress === 0
                }
            }

            TabBar {
                id: tabBar
                Layout.fillWidth: true
                TabButton { text: qsTr("Run") }
                TabButton { text: qsTr("Results"); enabled: !!root.activeRun }
                TabButton { text: qsTr("A/B"); enabled: !!root.comparison }
            }

            // --- New run --------------------------------------------------------------
            GroupBox {
                Layout.fillWidth: true
                visible: root.tab === 0
                title: qsTr("New run")
                GridLayout {
                    anchors.fill: parent
                    columns: 2
                    columnSpacing: 8
                    Label { text: qsTr("Part"); opacity: 0.7 }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: root.partInfo.id ? root.partInfo.name : qsTr("select a part in the browser")
                        font.italic: !root.partInfo.id
                    }
                    Label { text: qsTr("Speed (m/s)"); opacity: 0.7 }
                    TextField {
                        id: speedField
                        Layout.fillWidth: true
                        text: "20"
                        validator: DoubleValidator { bottom: 0.1; top: 150; locale: "C" }
                    }
                    Label { text: qsTr("Air flows"); opacity: 0.7 }
                    ComboBox {
                        id: flowBox
                        Layout.fillWidth: true
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: qsTr("+Y (onto the front)"), value: "+Y" },
                            { text: qsTr("-Y (onto the back)"), value: "-Y" },
                            { text: qsTr("+X (onto the left side)"), value: "+X" },
                            { text: qsTr("-X (onto the right side)"), value: "-X" }
                        ]
                    }
                    Label { text: qsTr("Mesh"); opacity: 0.7 }
                    ComboBox {
                        id: meshBox
                        Layout.fillWidth: true
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: qsTr("Coarse (fast)"), value: "coarse" },
                            { text: qsTr("Medium"), value: "medium" },
                            { text: qsTr("Fine (slow)"), value: "fine" }
                        ]
                    }
                    Label { text: qsTr("Label"); opacity: 0.7 }
                    TextField { id: labelField; Layout.fillWidth: true; placeholderText: qsTr("e.g. A, v2") }
                    CheckBox { id: groundBox; Layout.columnSpan: 2; text: qsTr("Moving floor under the geometry") }
                    CheckBox {
                        id: contextBox
                        Layout.columnSpan: 2
                        text: qsTr("Other visible parts in the flow (not in the forces)")
                    }
                    Button {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: qsTr("Run CFD")
                        highlighted: true
                        enabled: !!root.partInfo.id && root.partInfo.kind !== "sketch"
                        onClicked: root.startRun()
                    }
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: root.error.length > 0
                        text: root.error
                        color: Theme.danger
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // --- Runs -----------------------------------------------------------------
            Label { text: qsTr("Runs"); font.bold: true; visible: root.tab === 0 && root.runs.length > 0 }
            Label {
                visible: root.tab === 0 && root.runs.length > 0
                text: qsTr("Click a run to show it (A). B: compare with A.")
                opacity: 0.6
                font.pixelSize: 11
            }
            Repeater {
                model: root.runs.slice().reverse()
                delegate: Rectangle {
                    id: runRow
                    required property var modelData
                    visible: root.tab === 0
                    readonly property bool isA: modelData.id === root.activeRun
                    readonly property bool isB: modelData.id === root.compareRun
                    Layout.fillWidth: true
                    implicitHeight: rowContent.implicitHeight + 10
                    radius: 4
                    color: isA ? Theme.compareA : isB ? Theme.compareB : "transparent"
                    border.color: Theme.border

                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.selectRun(runRow.modelData.id)
                        onDoubleClicked: root.tab = 1
                    }

                    ColumnLayout {
                        id: rowContent
                        anchors.fill: parent
                        anchors.margins: 5
                        spacing: 2
                        RowLayout {
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.bold: true
                                text: (runRow.isA ? "[A] " : runRow.isB ? "[B] " : "") +
                                      (runRow.modelData.source.label ? runRow.modelData.source.label + " · " : "") +
                                      (runRow.modelData.source.partName || runRow.modelData.id)
                            }
                            ToolButton {
                                text: "B"
                                checkable: true
                                checked: runRow.isB
                                enabled: !runRow.isA
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Compare with A")
                                onClicked: {
                                    root.compareRun = checked ? runRow.modelData.id : ""
                                    root.loadFieldStats()
                                    root.loadComparison()
                                    if (root.comparison)
                                        root.tab = 2
                                }
                            }
                            ToolButton {
                                visible: runRow.modelData.state === "running"
                                text: qsTr("Cancel")
                                onClicked: { root.controller.execute("simulation.cancel", { run: runRow.modelData.id }); root.refreshRuns() }
                            }
                            ToolButton {
                                visible: runRow.modelData.state !== "running"
                                text: "✕"
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Delete run")
                                onClicked: {
                                    const id = runRow.modelData.id
                                    root.controller.execute("simulation.delete", { run: id })
                                    if (root.activeRun === id) root.activeRun = ""
                                    if (root.compareRun === id) root.compareRun = ""
                                    root.refreshRuns()
                                    root.loadResults()
                                    root.loadComparison()
                                }
                            }
                        }
                        Label {
                            font.pixelSize: 11
                            opacity: 0.75
                            text: {
                                const m = runRow.modelData
                                const l = m.latest || {}
                                let s = m.state
                                if (m.state === "running")
                                    s += " · " + m.stage + (m.stage === "solve" ? " " + m.iteration + "/" + m.maxIterations : "")
                                if (l.cd !== null && l.cd !== undefined)
                                    s += " · Cd " + root.num(l.cd, 3) + " · drag " + root.num(l.dragN, 3) + " N"
                                return s
                            }
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            visible: runRow.modelData.state === "running"
                            value: runRow.modelData.progress
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: !!runRow.modelData.message
                            text: runRow.modelData.message || ""
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            color: runRow.modelData.state === "failed" ? Theme.danger : Theme.text
                        }
                    }
                }
            }

            // --- Results --------------------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === 1 && !!root.results
                spacing: 8

                RowLayout {
                    Label {
                        Layout.fillWidth: true
                        font.bold: true
                        elide: Text.ElideRight
                        text: root.activeStatus ? (root.activeStatus.source.label ? root.activeStatus.source.label + " · " : "") +
                                                  (root.activeStatus.source.partName || root.activeStatus.id) : ""
                    }
                    Rectangle {
                        readonly property string status: root.results ? root.results.quality.status : "unknown"
                        radius: 3
                        implicitWidth: qualityLabel.implicitWidth + 10
                        implicitHeight: qualityLabel.implicitHeight + 4
                        color: status === "ok" ? Theme.success : status === "warning" ? Theme.warning : Theme.textFaint
                        Label {
                            id: qualityLabel
                            anchors.centerIn: parent
                            color: "white"
                            font.pixelSize: 11
                            text: qsTr("checks: %1").arg(parent.status)
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !!root.activeStatus && root.activeStatus.state !== "finished"
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: {
                            const m = root.activeStatus
                            if (!m)
                                return ""
                            const step = m.stage === "solve" ? qsTr("solving, iteration %1 of max %2").arg(m.iteration).arg(m.maxIterations)
                                       : m.stage === "mesh" ? qsTr("meshing") : m.stage
                            return m.state === "running" ? qsTr("Running: %1").arg(step) : m.state + (m.message ? ": " + m.message : "")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: !!root.activeStatus && root.activeStatus.state === "running"
                        ProgressBar { Layout.fillWidth: true; value: root.activeStatus ? root.activeStatus.progress : 0 }
                        Button {
                            text: qsTr("Cancel")
                            onClicked: { root.controller.execute("simulation.cancel", { run: root.activeRun }); root.refreshRuns() }
                        }
                    }
                }

                GridLayout {
                    columns: 4
                    columnSpacing: 10
                    rowSpacing: 2
                    readonly property var s: root.results ? root.results.summary : ({})
                    Label { text: qsTr("Drag"); opacity: 0.7 }
                    Label { text: root.num(parent.s.dragN, 3) + " N"; font.bold: true }
                    Label { text: "Cd"; opacity: 0.7 }
                    Label { text: root.num(parent.s.cd, 3); font.bold: true }
                    Label { text: qsTr("Lift"); opacity: 0.7 }
                    Label { text: root.num(parent.s.liftN, 3) + " N"; font.bold: true }
                    Label { text: "Cl"; opacity: 0.7 }
                    Label { text: root.num(parent.s.cl, 3); font.bold: true }
                    Label { text: qsTr("Side"); opacity: 0.7 }
                    Label { text: root.num(parent.s.sideN, 3) + " N"; font.bold: true }
                    Label { text: "Cs"; opacity: 0.7 }
                    Label { text: root.num(parent.s.cs, 3); font.bold: true }
                    Label { text: qsTr("Pressure / friction"); opacity: 0.7; Layout.columnSpan: 2 }
                    Label {
                        Layout.columnSpan: 2
                        text: root.num(parent.s.pressureDragN, 3) + " / " + root.num(parent.s.viscousDragN, 3) + " N"
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    opacity: 0.7
                    text: {
                        const c = root.results ? root.results.conditions : null
                        if (!c)
                            return ""
                        return qsTr("%1 m/s, flow %2, %3 mesh%4 · Aref %5 cm² · OpenFOAM %6 · mean of the last 20 % of %7 iterations")
                            .arg(c.settings.speed).arg(c.settings.flowDirection).arg(c.settings.mesh)
                            .arg(!c.settings.ground ? "" : c.settings.groundMoving === false ? qsTr(", fixed floor") : qsTr(", moving floor"))
                            .arg(root.num(c.referenceArea * 1e4, 1)).arg(c.openfoam)
                            .arg(root.results.summary.iterations)
                    }
                }
                Repeater {
                    model: root.results ? root.results.quality.warnings : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: Theme.warning
                        text: "⚠ " + modelData
                    }
                }

                LineChart {
                    Layout.fillWidth: true
                    title: qsTr("Coefficients")
                    series: root.results ? [
                        root.series("Cd", Theme.chart1, root.results.history.iteration, root.results.history.cd),
                        root.series("Cl", Theme.chart2, root.results.history.iteration, root.results.history.cl),
                        root.series("Cs", Theme.chart3, root.results.history.iteration, root.results.history.cs)
                    ] : []
                }
                LineChart {
                    Layout.fillWidth: true
                    title: qsTr("Forces (N)")
                    series: root.results ? [
                        root.series(qsTr("drag"), Theme.chart1, root.results.history.forceIteration, root.results.history.dragN),
                        root.series(qsTr("lift"), Theme.chart2, root.results.history.forceIteration, root.results.history.liftN),
                        root.series(qsTr("side"), Theme.chart3, root.results.history.forceIteration, root.results.history.sideN)
                    ] : []
                }
                LineChart {
                    Layout.fillWidth: true
                    title: qsTr("Residuals")
                    logY: true
                    series: {
                        const r = root.results ? root.results.residuals : null
                        if (!r)
                            return []
                        const colors = { Ux: Theme.chart1, Uy: Theme.chart6, Uz: Theme.chart4, p: Theme.chart2, k: Theme.chart5, omega: Theme.chart3 }
                        return Object.keys(colors).filter(k => r[k]).map(k => root.series(k, colors[k], r.iteration, r[k]))
                    }
                }

                // --- 3D view ----------------------------------------------------------
                Label { text: qsTr("Show in 3D"); font.bold: true }
                Repeater {
                    model: root.fieldChoices
                    delegate: ColumnLayout {
                        id: fieldRow
                        required property var modelData
                        required property int index
                        readonly property var range: root.scaleFor(modelData.kind, modelData.scalar)
                        Layout.fillWidth: true
                        spacing: 2
                        CheckBox {
                            text: fieldRow.modelData.label
                            checked: fieldRow.modelData.on
                            enabled: !!(root.results && root.results.fields[fieldRow.modelData.kind])
                            onToggled: root.setField(fieldRow.index, checked)
                        }
                        RowLayout {
                            visible: fieldRow.modelData.on && fieldRow.enabled && fieldRow.range[1] > fieldRow.range[0]
                            Layout.leftMargin: 28
                            Label { text: root.num(fieldRow.range[0], 2); font.pixelSize: 10 }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 10
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    stops: {
                                        const map = root.controller.colormap(fieldRow.modelData.scalar)
                                        return map.map((c, i) => stopComponent.createObject(fieldRow, { position: i / (map.length - 1), color: c }))
                                    }
                                }
                            }
                            Label {
                                text: root.num(fieldRow.range[1], 2) + (fieldRow.modelData.scalar === "speed" ? " m/s" : "")
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }

            // --- A/B ------------------------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.tab === 2 && !!root.comparison
                spacing: 4
                Label { text: qsTr("A/B comparison"); font.bold: true }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.bold: true
                    color: !root.comparison ? Theme.text : root.comparison.verdict === "comparable" ? Theme.success : Theme.warning
                    text: root.comparison ? qsTr("Verdict: %1").arg(root.comparison.verdict) : ""
                }
                GridLayout {
                    columns: 5
                    columnSpacing: 8
                    rowSpacing: 2
                    Label { text: "" }
                    Label { text: "A"; opacity: 0.7 }
                    Label { text: "B"; opacity: 0.7 }
                    Label { text: "B − A"; opacity: 0.7 }
                    Label { text: "%"; opacity: 0.7 }
                    Repeater {
                        model: [["dragN", qsTr("Drag N")], ["liftN", qsTr("Lift N")], ["sideN", qsTr("Side N")]]
                        delegate: Repeater {
                            id: rowRep
                            required property var modelData
                            readonly property var d: root.comparison && root.comparison.forces[modelData[0]] ? root.comparison.forces[modelData[0]] : null
                            model: 5
                            delegate: Label {
                                required property int index
                                font.bold: index === 3
                                text: index === 0 ? rowRep.modelData[1]
                                    : !rowRep.d ? "–"
                                    : index === 1 ? root.num(rowRep.d.a, 3)
                                    : index === 2 ? root.num(rowRep.d.b, 3)
                                    : index === 3 ? (rowRep.d.difference >= 0 ? "+" : "") + root.num(rowRep.d.difference, 3)
                                    : rowRep.d.percent === null || rowRep.d.percent === undefined ? "–"
                                    : (rowRep.d.percent >= 0 ? "+" : "") + root.num(rowRep.d.percent, 1)
                            }
                        }
                    }
                }
                Repeater {
                    model: root.comparison ? root.comparison.differingConditions.map(c => qsTr("Different condition: %1").arg(c))
                                             .concat(root.comparison.notes) : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: Theme.warning
                        text: "⚠ " + modelData
                    }
                }
                // Does the difference hold on another mesh? (A and B run there too.)
                ColumnLayout {
                    id: meshCheck
                    readonly property var ms: root.comparison ? root.comparison.meshSensitivity : null
                    visible: !!ms && ms.status !== undefined && root.comparison.sameConditions
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 2
                    Label { text: qsTr("Mesh check"); font.bold: true; font.pixelSize: 12 }
                    Repeater {
                        model: meshCheck.ms && meshCheck.ms.levels ? meshCheck.ms.levels : []
                        delegate: Label {
                            required property var modelData
                            font.pixelSize: 11
                            color: Theme.textMuted
                            text: qsTr("%1 mesh: B − A drag %2 N, lift %3 N").arg(modelData.mesh)
                                .arg(modelData.dragN ? root.num(modelData.dragN.difference, 3) : "–")
                                .arg(modelData.liftN ? root.num(modelData.liftN.difference, 3) : "–")
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: !meshCheck.ms ? Theme.textMuted
                             : meshCheck.ms.status === "holds" ? Theme.success
                             : meshCheck.ms.status === "inconclusive" ? Theme.warning : Theme.textMuted
                        text: meshCheck.ms ? meshCheck.ms.message || "" : ""
                    }
                    Button {
                        visible: !!meshCheck.ms && meshCheck.ms.status === "not assessed" && !!meshCheck.ms.suggestedMesh
                        text: qsTr("Run A and B on the %1 mesh").arg(meshCheck.ms ? meshCheck.ms.suggestedMesh : "")
                        onClicked: root.refineComparison(meshCheck.ms.suggestedMesh)
                    }
                }
                Switch {
                    text: qsTr("Show B in 3D (same colour scale)")
                    checked: root.showB
                    onToggled: root.showB = checked
                }
            }
        }
    }

    Component {
        id: stopComponent
        GradientStop {}
    }
}
