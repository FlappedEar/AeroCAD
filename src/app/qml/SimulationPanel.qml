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
        // Only while the open document still has the run's part, unchanged: the results are
        // in model coordinates, so without the part they float in empty space.
        if (!run || !displayedSourcePresent)
            return out
        for (const f of fieldChoices) {
            if (!f.on || !fieldStats[run + "/" + f.kind])
                continue
            if (f.kind === "surface" && differenceShown) {
                // The pressure difference B - A on A's part replaces the pressure map (CAD-141).
                const d = differenceScale()
                out.push({ run: activeRun, compareRun: compareRun, kind: "surface", scalar: "dCp", min: d[0], max: d[1],
                           tubeRadius: tubeRadius, palette: "coolwarm" })
                continue
            }
            const r = scaleFor(f.kind, f.scalar)
            out.push({ run: run, kind: f.kind, scalar: f.scalar, min: r[0], max: r[1], tubeRadius: tubeRadius,
                       palette: paletteFor(f.scalar) })
        }
        return out
    }
    /// "Difference B - A" ticked in Show in 3D (CAD-141).
    property bool showDifference: false
    /// Statistics of the pressure difference B - A on A's part (simulation.field with
    /// compare), or null.
    property var differenceStats: null
    /// The difference is drawn: ticked, A and B comparable, both surfaces written, and A's
    /// part in the open design (the difference lies on it).
    readonly property bool differenceShown: {
        if (!showDifference || !finalComparisonAvailable || !differenceStats)
            return false
        const a = runs.find(r => r.id === activeRun)
        return !!a && (!a.source || a.source.partFingerprint === "0" || a.sourceInDocument)
    }
    function loadDifferenceStats() {
        differenceStats = null
        // Read comparison itself: this runs from onComparisonChanged, before the
        // finalComparisonAvailable binding has been updated.
        if (!comparison || comparison.reasons.length > 0)
            return
        const r = controller.execute("simulation.field", { run: activeRun, kind: "surface", compare: compareRun })
        differenceStats = r.ok && r.result.available && r.result.scalars.dCp ? r.result.scalars.dCp : null
    }
    /// Symmetric ΔCp scale around zero over the 2nd-98th percentile, so blue and red mean
    /// the same amount of change.
    function differenceScale() {
        const s = differenceStats
        if (!s)
            return [0, 0]
        const m = Math.max(Math.abs(s.p02 ?? s.min), Math.abs(s.p98 ?? s.max), 1e-3)
        return [-m, m]
    }
    onComparisonChanged: loadDifferenceStats()
    /// Streamline tube radius in mm: 0.08 % of the part's largest dimension, thin enough
    /// for the dense rakes of CAD-135 (0 = automatic).
    readonly property real tubeRadius: {
        const c = results ? results.conditions : null
        return c && c.characteristicLength > 0 ? c.characteristicLength * 1000 * 0.0008 : 0
    }
    /// Name of the run drawn in 3D, for the viewport card.
    readonly property string displayedRunTitle: {
        if (!displayedRun)
            return ""
        return (showB && finalComparisonAvailable ? "B: " : differenceShown ? "A: " : "") + runTitle(displayedRun, "")
    }
    /// Parts hidden while their pressure map is shown (they would overlap).
    readonly property var hiddenPartIds: {
        const f = fieldChoices.find(c => c.kind === "surface")
        const run = runs.find(r => r.id === displayedRun)
        // Only when the open document still has that part (ids are per document).
        if (f && f.on && differenceShown) {
            const a = runs.find(r => r.id === activeRun)
            return a && a.source && a.sourceInDocument ? [a.source.partId] : []
        }
        return f && f.on && run && run.source && run.sourceInDocument && fieldStats[run.id + "/surface"]
            ? [run.source.partId] : []
    }
    /// Run whose fields are drawn: B while "Show B in 3D" is on, otherwise A.
    readonly property string displayedRun: showB && finalComparisonAvailable ? compareRun : activeRun
    /// The part the displayed run was made from is in the open document, unchanged (runs that
    /// record no part, e.g. imported from elsewhere, are always drawn).
    readonly property bool displayedSourcePresent: {
        const run = runs.find(r => r.id === displayedRun)
        if (!run)
            return false
        return !run.source || run.source.partFingerprint === "0" || run.sourceInDocument
    }

    property var environment: null
    property var runs: []
    /// Runs made from a part in the open document, and the others (CAD-136).
    readonly property var ownRuns: runs.filter(r => r.sourceInDocument).reverse()
    readonly property var otherRuns: runs.filter(r => !r.sourceInDocument).reverse()
    readonly property var shownOtherRuns: showOtherRuns ? otherRuns
                                                        : otherRuns.filter(r => r.id === activeRun || r.id === compareRun)
    property bool showOtherRuns: false
    property string activeRun: ""
    /// Selects the newest run on first show; off after a new or opened project.
    property bool autoSelect: true
    property string compareRun: ""
    property bool showB: false
    property var results: null
    property var comparison: null
    readonly property bool finalComparisonAvailable: !!comparison && comparison.reasons.length === 0
    property var fieldStats: ({})
    property string error: ""
    /// True after "Install for me" until the engine is ready (the environment is polled).
    property bool installing: false
    /// Remote engine: connecting (testing the SSH connection) and the last error.
    property bool connecting: false
    property string engineError: ""
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
    // Re-read after any change of the parts (a Scale changes the size shown, CAD-109).
    property int partRevision: 0
    readonly property Connections _partWatch: Connections {
        target: root.controller
        function onPartChanged() { root.partRevision++ }
        function onHistoryChanged() { root.partRevision++ }
    }
    readonly property var partInfo: {
        void partRevision
        return selectedPartId ? controller.partInfo(selectedPartId) : ({})
    }

    /// The quality report as short lines (CAD-116): the per-field residual warnings and the
    /// solver's "not converged" line become one line with the final residuals, and a steady
    /// drag coefficient is said too, so a usual coarse run does not show seven warnings.
    function qualityLines(q) {
        if (!q)
            return []
        const lines = []
        const residuals = []
        const warnings = q.warnings || []
        const residualLine = /^(\w+): residual has not reached the preset target/
        for (const w of warnings) {
            const m = residualLine.exec(w)
            if (m)
                residuals.push(m[1])
        }
        for (const w of warnings) {
            const solverLine = w.indexOf("solver: finished without reaching") === 0
            if (!residualLine.test(w) && !(solverLine && residuals.length > 0))
                lines.push({ text: w, ok: false })
        }
        if (residuals.length > 0) {
            const fr = q.finalResiduals || {}
            const values = residuals.map(f => fr[f] !== undefined ? f + " " + Number(fr[f]).toExponential(1) : f)
            lines.unshift({ text: qsTr("Not converged to the preset targets (1e-5, p 1e-4); final residuals %1").arg(values.join(", ")),
                            ok: false })
        }
        if (q.checks && q.checks.forceStability === "passed" && q.cdFluctuation !== undefined)
            lines.push({ text: qsTr("Cd is steady: it changes %1 % over the last 20 % of the iterations")
                                   .arg(root.num(100 * q.cdFluctuation, 2)), ok: true })
        return lines
    }

    /// Why the verdict is "check quality": the selected runs themselves, or a run of the mesh check (CAD-117).
    function checkQualityText(c) {
        if (!c || c.verdict !== "check quality")
            return ""
        const q = c.quality || {}
        const okA = q.a && q.a.status === "ok", okB = q.b && q.b.status === "ok"
        if (!okA && !okB)
            return qsTr("A and B have quality warnings (see Results), so the difference may not be reliable.")
        if (!okA || !okB)
            return qsTr("%1 has quality warnings (see Results), so the difference may not be reliable.").arg(okA ? "B" : "A")
        const m = c.meshSensitivity && c.meshSensitivity.message
        return m ? qsTr("Mesh check: %1").arg(m)
                 : qsTr("A run of the mesh check has quality warnings, so the difference may not be reliable.")
    }

    /// A run's name for the A/B table: its label (or part name) and start time, plus its state
    /// when it is not finished (CAD-117).
    function runTitle(id, state) {
        const r = runs.find(x => x.id === id)
        if (!r)
            return id
        const t = r.created ? new Date(r.created) : null
        const time = t && !isNaN(t) ? t.toLocaleTimeString(Qt.locale(), "HH:mm") : ""
        const name = r.source && r.source.label ? r.source.label : (r.source && r.source.partName) || id
        return name + (time ? " (" + time + ")" : "") + (state && state !== "finished" ? ", " + state : "")
    }

    /// A force too small next to the run's largest force for a percentage to mean anything
    /// (e.g. a side force of 0.002 N on a 3 N drag: +316 % is noise) (CAD-117).
    function noiseLevel(a) {
        const f = comparison ? comparison.forces : null
        if (!f)
            return false
        const largest = Math.max(...["dragN", "liftN", "sideN"].map(k => f[k] ? Math.abs(f[k].a) : 0))
        return Math.abs(a) < 0.02 * largest
    }

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

    /// Chooses the engine: "" = this computer, "ssh://user@host" = Docker there.
    function setEngine(host) {
        connecting = true
        engineError = ""
        const r = controller.execute("simulation.set_engine", { remoteHost: host })
        connecting = false
        if (!r.ok) {
            engineError = r.error
            return
        }
        checkEnvironment()
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
        // On first show, the newest run of this design (never one of another design) (CAD-136).
        const own = runs.filter(x => x.sourceInDocument)
        if (!activeRun && autoSelect && own.length > 0) {
            selectRun(own[own.length - 1].id)
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

    /// New or opened project: nothing selected, nothing drawn, until a run is picked.
    function clearSelection() {
        autoSelect = false
        activeRun = ""
        compareRun = ""
        showB = false
        results = null
        comparison = null
        fieldStats = ({})
        rangeOverrides = ({})
        showDifference = false
        differenceStats = null
        cpMin.text = ""
        cpMax.text = ""
        error = ""
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
        if (!finalComparisonAvailable)
            showB = false
    }

    /// Colour scale for a field: shared between A and B in compare mode, and between all
    /// fields of a quantity. Pressure uses the 2nd to 98th percentile (CAD-135), so a few
    /// extreme points at the stagnation point or a sharp edge do not wash out the map; a
    /// range typed in the panel (rangeOverrides) wins.
    function scaleFor(kind, scalar) {
        const o = rangeOverrides[scalar]
        if (o && o[1] > o[0])
            return [o[0], o[1]]
        return autoScale(scalar)
    }
    function autoScale(scalar) {
        let lo = Infinity, hi = -Infinity
        const robust = scalar === "Cp" || scalar === "pressure"
        for (const id of finalComparisonAvailable ? [activeRun, compareRun] : [displayedRun]) {
            for (const f of fieldChoices) {
                if (f.scalar !== scalar)
                    continue
                const s = fieldStats[id + "/" + f.kind]
                if (s && s[scalar]) {
                    lo = Math.min(lo, robust && s[scalar].p02 !== undefined ? s[scalar].p02 : s[scalar].min)
                    hi = Math.max(hi, robust && s[scalar].p98 !== undefined ? s[scalar].p98 : s[scalar].max)
                }
            }
        }
        if (!isFinite(lo))
            return [0, 0]
        if (scalar === "Cp")
            return [Math.max(lo, -2), Math.min(hi, 1)]
        return [0, hi]
    }
    /// Colour scales typed in the panel: {scalar: [min, max]}; absent = automatic.
    property var rangeOverrides: ({})
    function setRangeOverride(scalar, min, max) {
        const copy = Object.assign({}, rangeOverrides)
        if (isFinite(min) && isFinite(max) && max > min)
            copy[scalar] = [min, max]
        else
            delete copy[scalar]
        rangeOverrides = copy
    }
    /// Colour map of the pressure fields: "rainbow" (default, high contrast) or "coolwarm".
    property string pressurePalette: "rainbow"
    function paletteFor(scalar) {
        return scalar === "Cp" || scalar === "pressure" ? pressurePalette : ""
    }

    /// Settings of a new run as they stand in the panel (simulation.run / propose_run).
    function runParams() {
        const params = {
            id: partInfo.id,
            speed: Number(speedField.text),
            flowDirection: flowBox.currentValue,
            mesh: meshBox.currentValue,
            ground: groundBox.checked
        }
        if (contextBox.checked) {
            const parts = controller.execute("document.parts")
            if (parts.ok)
                params.context = parts.result.parts
                    .filter(p => p.id !== partInfo.id && p.visible && p.kind !== "sketch" && p.kind !== "plane").map(p => p.id)
        }
        return params
    }

    /// The planned run of the selected part (simulation.propose_run without the engine
    /// check): part size, tunnel and flow in model mm, size warnings. Drawn in the view
    /// while a new run is set up; null when there is nothing to plan.
    property var proposal: null
    function refreshProposal() {
        if (!visible || tab !== 0 || !partInfo.id || partInfo.kind === "sketch" || partInfo.kind === "plane") {
            proposal = null
            return
        }
        const params = runParams()
        // Preview only: a cleared or zero speed still shows the tunnel. A run with such a
        // speed is refused by simulation.run, never started at another speed.
        if (!(params.speed > 0))
            params.speed = 20
        params.checkEngine = false
        const r = controller.execute("simulation.propose_run", params)
        proposal = r.ok ? r.result : null
    }
    Timer {
        id: proposalTimer
        interval: 120
        onTriggered: root.refreshProposal()
    }
    onTabChanged: proposalTimer.restart()
    onPartInfoChanged: proposalTimer.restart()
    Connections {
        target: root.controller
        function onBoundsChanged() { proposalTimer.restart() }
    }

    function startRun() {
        error = ""
        if (!partInfo.id) {
            error = qsTr("Select a part first.")
            return
        }
        const params = runParams()
        if (labelField.text.length > 0)
            params.label = labelField.text
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

    onVisibleChanged: {
        proposalTimer.restart()
        if (visible) {
            if (!environment)
                checkEnvironment()
            refreshRuns()
        }
    }
    Component.onCompleted: if (visible) { checkEnvironment(); refreshRuns() }

    // A part deleted, changed or brought back by undo: show or hide its results at once.
    Connections {
        target: root.controller
        function onHistoryChanged() {
            if (root.visible)
                root.refreshRuns()
        }
    }

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
                // A "manual" step only repeats the engine line above: do not show it twice.
                visible: !!setup && ((setup.action !== "none" && setup.action !== "manual") || pulling || root.installing)
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

            // Where OpenFOAM runs: this computer, or Docker on another machine over SSH.
            ColumnLayout {
                id: engineBox
                readonly property string remoteHost: root.environment && root.environment.remoteHost
                                                     ? root.environment.remoteHost : ""
                property bool editing: false
                Layout.fillWidth: true
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Run on"); font.pixelSize: 11 }
                    ComboBox {
                        id: engineChoice
                        Layout.fillWidth: true
                        model: [qsTr("This computer"), qsTr("Another machine (Docker over SSH)")]
                        currentIndex: engineBox.remoteHost.length > 0 || engineBox.editing ? 1 : 0
                        onActivated: index => {
                            if (index === 0) {
                                engineBox.editing = false
                                if (engineBox.remoteHost.length > 0)
                                    root.setEngine("")
                            } else {
                                engineBox.editing = true
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: engineChoice.currentIndex === 1
                    TextField {
                        id: hostField
                        Layout.fillWidth: true
                        placeholderText: "ssh://user@host"
                        text: engineBox.remoteHost
                        onAccepted: root.setEngine(text.trim())
                    }
                    Button {
                        text: qsTr("Connect")
                        enabled: hostField.text.trim().length > 0 && !root.connecting
                        onClicked: root.setEngine(hostField.text.trim())
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: engineChoice.currentIndex === 1
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: root.engineError.length > 0 ? Theme.danger : Theme.textMuted
                    text: root.connecting ? qsTr("Connecting...")
                        : root.engineError.length > 0 ? root.engineError
                        : qsTr("The case is sent to that machine, OpenFOAM runs there and the results come back. "
                               + "Needs SSH login with a key and Docker there.")
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
                    Label { text: qsTr("Size"); opacity: 0.7; visible: !!root.partInfo.id && root.partInfo.kind !== "sketch" }
                    Label {
                        Layout.fillWidth: true
                        visible: !!root.partInfo.id && root.partInfo.kind !== "sketch"
                        text: visible ? [root.partInfo.sizeX, root.partInfo.sizeY, root.partInfo.sizeZ]
                                        .map(v => Number(v).toLocaleString(Qt.locale(), "f", v < 10 ? 2 : 0)).join(" × ") + " mm"
                                      : ""
                    }
                    // A part far too small or large for a wind tunnel is most likely in the wrong unit (CAD-109).
                    Label {
                        id: sizeWarning
                        objectName: "cfdSizeWarning"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: root.partInfo.id ? controller.partSizeWarning(root.partInfo.id) : ""
                        visible: text.length > 0
                        color: Theme.warning
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                    Label { text: qsTr("Speed (m/s)"); opacity: 0.7 }
                    TextField {
                        id: speedField
                        objectName: "cfdSpeedField"
                        Layout.fillWidth: true
                        text: "20"
                        validator: DoubleValidator { bottom: 0.1; top: 150; locale: "C" }
                        onEditingFinished: proposalTimer.restart()
                    }
                    Label { text: qsTr("Air flows"); opacity: 0.7 }
                    ComboBox {
                        id: flowBox
                        objectName: "cfdFlowBox"
                        Layout.fillWidth: true
                        onActivated: proposalTimer.restart()
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
                        onActivated: proposalTimer.restart()
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { text: qsTr("Coarse (fast)"), value: "coarse" },
                            { text: qsTr("Medium"), value: "medium" },
                            { text: qsTr("Fine (slow)"), value: "fine" }
                        ]
                    }
                    Label { text: qsTr("Label"); opacity: 0.7 }
                    TextField { id: labelField; Layout.fillWidth: true; placeholderText: qsTr("e.g. v2, long splitter") }
                    CheckBox {
                        id: groundBox
                        Layout.columnSpan: 2
                        text: qsTr("Moving floor under the geometry")
                        onToggled: proposalTimer.restart()
                    }
                    CheckBox {
                        id: contextBox
                        Layout.columnSpan: 2
                        onToggled: proposalTimer.restart()
                        text: qsTr("Other visible parts in the flow (not in the forces)")
                    }
                    Button {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: qsTr("Run CFD")
                        highlighted: true
                        enabled: !!root.partInfo.id && root.partInfo.kind !== "sketch" && root.partInfo.kind !== "plane"
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
            // This design's runs first (newest first); runs of other designs (their part is
            // not in the open document) are folded away unless they are A or B (CAD-136).
            Repeater {
                model: root.ownRuns
                delegate: runRowDelegate
            }
            Label {
                visible: root.tab === 0 && root.otherRuns.length > 0
                Layout.topMargin: 4
                text: (root.showOtherRuns ? "▾ " : "▸ ") + qsTr("Runs from other designs (%1)").arg(root.otherRuns.length)
                color: Theme.textMuted
                font.pixelSize: 11
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showOtherRuns = !root.showOtherRuns
                }
            }
            Repeater {
                model: root.shownOtherRuns
                delegate: runRowDelegate
            }
            Component {
                id: runRowDelegate
                Rectangle {
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
                                objectName: "compareButton_" + runRow.modelData.id
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
                                    s += (m.state === "finished" ? " · latest Cd " : " · provisional Cd ")
                                        + root.num(l.cd, 3) + " · drag " + root.num(l.dragN, 3) + " N"
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

                Label {
                    text: root.results && root.results.finalResults.available
                        ? qsTr("Final results") : qsTr("Final results unavailable")
                    font.bold: true
                }
                Repeater {
                    model: root.results ? root.results.finalResults.reasons : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.warning
                        text: modelData
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
                        const conditions = qsTr("%1 m/s, flow %2, %3 mesh%4 · Aref %5 cm² · OpenFOAM %6")
                            .arg(c.settings.speed).arg(c.settings.flowDirection).arg(c.settings.mesh)
                            .arg(!c.settings.ground ? "" : c.settings.groundMoving === false ? qsTr(", fixed floor") : qsTr(", moving floor"))
                            .arg(root.num(c.referenceArea * 1e4, 1)).arg(c.openfoam)
                        return conditions + (root.results.provisional
                            ? qsTr(" · %1 recorded iterations; provisional data")
                            : qsTr(" · mean of the last 20 % of %1 iterations")).arg(root.results.summary.iterations)
                    }
                }
                Repeater {
                    model: root.results ? root.qualityLines(root.results.quality) : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: modelData.ok ? Theme.success : Theme.warning
                        text: (modelData.ok ? "✓ " : "⚠ ") + modelData.text
                    }
                }

                LineChart {
                    Layout.fillWidth: true
                    title: root.results && root.results.provisional ? qsTr("Coefficients (provisional)") : qsTr("Coefficients")
                    series: root.results ? [
                        root.series("Cd", Theme.chart1, root.results.history.iteration, root.results.history.cd),
                        root.series("Cl", Theme.chart2, root.results.history.iteration, root.results.history.cl),
                        root.series("Cs", Theme.chart3, root.results.history.iteration, root.results.history.cs)
                    ] : []
                }
                LineChart {
                    Layout.fillWidth: true
                    title: root.results && root.results.provisional ? qsTr("Forces (N, provisional)") : qsTr("Forces (N)")
                    series: root.results ? [
                        root.series(qsTr("drag"), Theme.chart1, root.results.history.forceIteration, root.results.history.dragN),
                        root.series(qsTr("lift"), Theme.chart2, root.results.history.forceIteration, root.results.history.liftN),
                        root.series(qsTr("side"), Theme.chart3, root.results.history.forceIteration, root.results.history.sideN)
                    ] : []
                }
                LineChart {
                    Layout.fillWidth: true
                    title: root.results && root.results.provisional ? qsTr("Residuals (provisional)") : qsTr("Residuals")
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
                Label {
                    Layout.fillWidth: true
                    visible: !!root.displayedRun && !root.displayedSourcePresent
                    text: qsTr("Not drawn in 3D: the part of this run was deleted or has changed since the run.")
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                }
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
                                        const map = root.controller.colormap(fieldRow.modelData.scalar, root.paletteFor(fieldRow.modelData.scalar))
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
                // A/B: the change of pressure on the part (CAD-141).
                CheckBox {
                    objectName: "differenceCheck"
                    visible: root.finalComparisonAvailable
                    enabled: !!root.differenceStats
                    text: qsTr("Difference B − A on the part (ΔCp)")
                    checked: root.showDifference
                    onToggled: root.showDifference = checked
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.differenceShown
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                    text: qsTr("On A's part: red where B has higher pressure, blue where lower; grey where B's part has another shape.")
                }
                // Colours of the pressure map and the Cp range (CAD-135).
                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 6
                    rowSpacing: 4
                    visible: !!(root.results && root.results.fields.surface)
                    Label { text: qsTr("Pressure colours") }
                    ComboBox {
                        id: paletteBox
                        objectName: "pressurePaletteBox"
                        Layout.columnSpan: 3
                        Layout.fillWidth: true
                        textRole: "text"
                        valueRole: "value"
                        model: [{ text: qsTr("Rainbow"), value: "rainbow" }, { text: qsTr("Blue–white–red"), value: "coolwarm" }]
                        Component.onCompleted: currentIndex = indexOfValue(root.pressurePalette)
                        onActivated: root.pressurePalette = currentValue
                    }
                    Label { text: qsTr("Cp range") }
                    TextField {
                        id: cpMin
                        objectName: "cpMinField"
                        Layout.preferredWidth: 64
                        placeholderText: root.num(root.autoScale("Cp")[0], 2)
                        // C locale: the text is read with parseFloat, so a decimal comma must not validate.
                        validator: DoubleValidator { locale: "C" }
                        onEditingFinished: root.setRangeOverride("Cp", parseFloat(cpMin.text), parseFloat(cpMax.text))
                    }
                    TextField {
                        id: cpMax
                        objectName: "cpMaxField"
                        Layout.preferredWidth: 64
                        placeholderText: root.num(root.autoScale("Cp")[1], 2)
                        // C locale: the text is read with parseFloat, so a decimal comma must not validate.
                        validator: DoubleValidator { locale: "C" }
                        onEditingFinished: root.setRangeOverride("Cp", parseFloat(cpMin.text), parseFloat(cpMax.text))
                    }
                    Button {
                        text: qsTr("Auto")
                        enabled: !!root.rangeOverrides.Cp
                        onClicked: { cpMin.text = ""; cpMax.text = ""; root.setRangeOverride("Cp", NaN, NaN) }
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
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                    visible: text.length > 0
                    text: root.checkQualityText(root.comparison)
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    // Which run is A and which is B, by label and start time (CAD-117).
                    text: root.comparison ? qsTr("A: %1 · B: %2").arg(root.runTitle(root.comparison.a, root.comparison.states.a))
                                                                 .arg(root.runTitle(root.comparison.b, root.comparison.states.b)) : ""
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
                                    : rowRep.d.percent === null || rowRep.d.percent === undefined || root.noiseLevel(rowRep.d.a) ? "–"
                                    : Math.abs(rowRep.d.percent) < 0.05 ? "0.0" // not "-0.0"
                                    : (rowRep.d.percent >= 0 ? "+" : "") + root.num(rowRep.d.percent, 1)
                            }
                        }
                    }
                }
                Repeater {
                    model: root.comparison ? root.comparison.differingConditions.map(c => qsTr("Different condition: %1").arg(c))
                                             .concat((root.comparison.unknownConditions || []).map(c => qsTr("Unknown condition: %1").arg(c)))
                                             .concat(root.comparison.reasons).concat(root.comparison.notes) : []
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
                    // Also for a target-part variant whose only difference is its own domain.
                    visible: !!ms && ms.status !== undefined && !root.comparison.reasons.length
                             && root.comparison.verdict !== "not comparable"
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
                    // Progress of the mesh-check runs still solving (CAD-120).
                    Repeater {
                        model: meshCheck.ms && meshCheck.ms.runningRuns ? meshCheck.ms.runningRuns : []
                        delegate: RowLayout {
                            id: pendingRow
                            required property string modelData
                            readonly property var run: root.runs.find(x => x.id === modelData)
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                font.pixelSize: 11
                                color: Theme.textMuted
                                text: pendingRow.run ? root.runTitle(pendingRow.run.id, "") : pendingRow.modelData
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                value: pendingRow.run ? pendingRow.run.progress || 0 : 0
                            }
                            Label {
                                font.pixelSize: 11
                                color: Theme.textMuted
                                text: Math.round(100 * (pendingRow.run ? pendingRow.run.progress || 0 : 0)) + " %"
                            }
                        }
                    }
                    Button {
                        visible: !!meshCheck.ms && meshCheck.ms.status === "not assessed" && !!meshCheck.ms.suggestedMesh
                        text: qsTr("Run A and B on the %1 mesh").arg(meshCheck.ms ? meshCheck.ms.suggestedMesh : "")
                        onClicked: root.refineComparison(meshCheck.ms.suggestedMesh)
                    }
                }
                Switch {
                    text: qsTr("Show B in 3D (same colour scale)")
                    enabled: root.finalComparisonAvailable
                    checked: root.showB && root.finalComparisonAvailable
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
