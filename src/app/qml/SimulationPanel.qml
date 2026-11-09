// SPDX-License-Identifier: GPL-3.0-or-later
// Wind tunnel panel: start a CFD run for the selected part, follow its progress, read the
// results (forces in N, Cd/Cl, charts, quality) and compare two runs (A/B). Everything
// goes through the simulation.* commands, the same ones an AI agent uses over MCP.
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
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
            const kind = fieldKind(f, run)
            if (!f.on || !kind || !fieldStats[run + "/" + kind])
                continue
            if (f.kind === "surface" && differenceShown) {
                // The pressure difference B - A on A's part replaces the pressure map (CAD-141).
                const d = differenceScale()
                out.push({ run: activeRun, compareRun: compareRun, kind: "surface", scalar: "dCp", min: d[0], max: d[1],
                           tubeRadius: tubeRadius, palette: "coolwarm" })
                continue
            }
            const r = scaleFor(f.kind, f.scalar)
            const smoke = f.kind === "streamlines" && streamlineStyle === "smoke"
            out.push({ run: run, kind: kind, scalar: f.scalar, min: r[0], max: r[1],
                       tubeRadius: smoke ? tubeRadius / 2 : tubeRadius, palette: paletteFor(f.scalar, f.kind) })
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
        if (!comparison || comparison.reasons.length > 0 || !activeRun || !compareRun)
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
    onComparisonChanged: {
        // A/B is disabled without a comparison (CAD-214).
        if (!comparison && tab === 2)
            tab = activeRun ? 1 : 0
        loadDifferenceStats()
        // The export note belongs to one A/B pair (CAD-193), not to a refresh of the same pair.
        const pair = comparison ? comparison.a + "|" + comparison.b : ""
        if (pair !== abExportPair)
            abExportNote = ""
        abExportPair = pair
    }
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
    // The difference B - A lies on A's part, so A is the displayed run while it is shown.
    readonly property string displayedRun: showB && finalComparisonAvailable && !differenceShown ? compareRun : activeRun
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
    readonly property var ownRuns: sortedRuns(runs.filter(r => r.sourceInDocument).reverse())
    readonly property var otherRuns: sortedRuns(runs.filter(r => !r.sourceInDocument).reverse())
    /// Order of the run list (CAD-207): "newest", "drag", "cd" or "downforce".
    property string runSort: "newest"
    /// Ranks newest-first runs by their final result (CAD-205); runs without one (running,
    /// failed, prepared) follow, newest first. Equal values keep the newest first.
    function sortedRuns(newestFirst) {
        if (runSort === "newest")
            return newestFirst
        const key = r => !r.final ? null
                       : runSort === "drag" ? r.final.dragN : runSort === "cd" ? r.final.cd : r.final.liftN
        const ranked = newestFirst.filter(r => typeof key(r) === "number")
        const rest = newestFirst.filter(r => typeof key(r) !== "number")
        // Array.sort is stable, so ties stay newest first.
        ranked.sort((a, b) => key(a) - key(b))
        return ranked.concat(rest)
    }
    readonly property var shownOtherRuns: showOtherRuns ? otherRuns
                                                        : otherRuns.filter(r => r.id === activeRun || r.id === compareRun)
    property bool showOtherRuns: false
    property string activeRun: ""
    /// Selects the newest run on first show; off after a new or opened project.
    property bool autoSelect: true
    property string compareRun: ""
    property bool showB: false
    // "Show B in 3D" and "Difference B - A" exclude each other: ticking one unticks the other.
    onShowBChanged: if (showB) showDifference = false
    onShowDifferenceChanged: if (showDifference) showB = false
    property var results: null
    /// Results of B (history sampled like A's) for the A/B convergence chart (CAD-179).
    property var resultsB: null
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
        { kind: "sectionHorizontal", axis: "z", scalar: "speed", label: qsTr("Horizontal section (speed)"), on: false },
        { kind: "sectionVertical", axis: "y", scalar: "speed", label: qsTr("Vertical section (speed)"), on: false },
        { kind: "streamlines", scalar: "speed", label: qsTr("Streamlines (speed)"), on: true },
        { kind: "sectionCross", axis: "x", scalar: "speed", label: qsTr("Section across the flow (speed)"), on: false },
        // CAD-165: where the air has lost all of its total pressure, as in motorsport CFD.
        { kind: "wake", scalar: "speed", label: qsTr("Wake: total pressure lost, Cp0 = 0 (speed)"), on: false },
        { kind: "vortices", scalar: "speed", label: qsTr("Vortex cores, Q-criterion (speed)"), on: false }
    ]

    readonly property bool anyRunning: runs.some(r => r.state === "running")
    readonly property var activeStatus: runs.find(r => r.id === activeRun) ?? null
    /// The shown run is still running: its checks and final numbers come after the run (CAD-150).
    readonly property bool activeRunning: !!activeStatus && activeStatus.state === "running"
    // Re-read after any change of the parts (a Scale changes the size shown, CAD-109).
    property int partRevision: 0
    function bumpParts() { partRevision++ }
    readonly property Connections _partWatch: Connections {
        target: root.controller
        function onPartChanged() { Qt.callLater(root.bumpParts) }
        function onHistoryChanged() { Qt.callLater(root.bumpParts) }
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
            lines.unshift({ text: q.forcesSettled
                                  ? qsTr("Forces settled, but residuals are above 1e-3: %1").arg(values.join(", "))
                                  : qsTr("Not converged to the preset targets (1e-5, p 1e-4); final residuals %1").arg(values.join(", ")),
                            ok: false })
        }
        // CAD-155: the solver stopped early because Cd and Cl no longer changed.
        if (q.forcesSettled)
            lines.push({ text: q.iterations ? qsTr("Stopped after %1 iterations: Cd and Cl have settled").arg(q.iterations)
                                            : qsTr("Stopped early: Cd and Cl have settled"), ok: true })
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

    /// Deleting a run removes its files and stopping one cannot be undone: both are asked first
    /// (CAD-151). A run may take hours of computing.
    function confirmRunAction(action, id) {
        confirmRun.action = action
        confirmRun.runId = id
        confirmRun.open()
    }
    function doRunAction(action, id) {
        if (action === "cancel") {
            controller.execute("simulation.cancel", { run: id })
            refreshRuns()
            return
        }
        controller.execute("simulation.delete", { run: id })
        if (activeRun === id) {
            // B is compared with A: without A nothing is (CAD-215).
            activeRun = ""
            compareRun = ""
            showB = false
        }
        if (compareRun === id) compareRun = ""
        refreshRuns()
        loadResults()
        loadComparison()
    }
    Dialog {
        id: confirmRun
        objectName: "cfdConfirmRun"
        property string action: "delete"
        property string runId: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 420
        title: action === "delete" ? qsTr("Delete this run?") : qsTr("Stop this run?")
        Label {
            width: confirmRun.availableWidth
            wrapMode: Text.WordWrap
            text: confirmRun.action === "delete"
                  ? qsTr("%1 and its results are removed from this computer. This cannot be undone.").arg(root.runTitle(confirmRun.runId))
                  : qsTr("%1 stops now and cannot be resumed. What it computed so far stays visible.").arg(root.runTitle(confirmRun.runId))
        }
        footer: DialogButtonBox {
            Button {
                text: confirmRun.action === "delete" ? qsTr("Delete") : qsTr("Stop run")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Keep")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        onAccepted: root.doRunAction(action, runId)
    }

    /// A force too small next to the run's largest force for a percentage to mean anything
    /// (e.g. a side force of 0.002 N on a 3 N drag: +316 % is noise) (CAD-117).
    function noiseLevel(a, coefficient) {
        // Forces against the largest force, coefficients against the largest coefficient (CAD-171).
        const f = !comparison ? null : coefficient ? comparison.coefficientsOnReferenceAreaOfA : comparison.forces
        if (!f)
            return false
        const keys = coefficient ? ["cd", "cl", "cs"] : ["dragN", "liftN", "sideN"]
        const largest = Math.max(...keys.map(k => f[k] ? Math.abs(f[k].a) : 0))
        return Math.abs(a) < 0.02 * largest
    }
    /// One A/B table row: A, B, B − A and the change in % (null at noise level or for A = 0).
    function abRow(key) {
        // L/D of A and B (CAD-187): a ratio, so never at force noise level.
        if (key === "efficiency")
            return comparison && comparison.efficiency ? comparison.efficiency : null
        // Front share of lift in % (CAD-197): B − A in percentage points, no % of a %.
        if (key === "balance")
            return comparison && comparison.balance
                   ? { a: comparison.balance.a, b: comparison.balance.b, difference: comparison.balance.difference,
                       percent: null, withinNoise: false } : null
        const coefficient = ["cd", "cl", "cs"].indexOf(key) >= 0
        const src = !comparison ? null : coefficient ? comparison.coefficientsOnReferenceAreaOfA : comparison.forces
        const d = src && src[key] ? src[key] : null
        if (!d)
            return null
        const percent = d.percent !== undefined ? d.percent : Math.abs(d.a) > 1e-12 ? 100 * d.difference / Math.abs(d.a) : null
        // Inside the iteration swing of A plus B (CAD-191); equal steady values are no change.
        return { a: d.a, b: d.b, difference: d.difference,
                 percent: percent === null || noiseLevel(d.a, coefficient) ? null : percent,
                 withinNoise: d.withinNoise === true && d.noise > 0 }
    }
    /// " ± 0.012" (CAD-191): how far a final result still swings over the averaging window.
    function spreadText(summary, key, digits) {
        const v = summary && summary.spread ? summary.spread[key] : undefined
        return typeof v === "number" ? " ± " + num(v, digits) : ""
    }

    /// A run's conditions in one line for the run list (CAD-181): "35 m/s (126 km/h) · flow +X ·
    /// medium mesh · moving floor at Z -40 mm"; "" without case information.
    function runConditions(st) {
        if (!st)
            return ""
        const parts = [qsTr("%1 m/s (%2 km/h)").arg(Number(Number(st.speed).toFixed(2))).arg(Math.round(st.speed * 3.6)),
                       qsTr("flow %1").arg(st.flowDirection) + (st.yaw ? qsTr(", yaw %1°").arg(Number(st.yaw)) : ""),
                       qsTr("%1 mesh").arg(st.mesh)]
        if (st.ground) {
            let floor = st.rotatingWheels ? qsTr("moving floor, turning wheels")
                      : st.groundMoving === false ? qsTr("fixed floor") : qsTr("moving floor")
            if (st.groundHeight !== null && st.groundHeight !== undefined)
                floor += " " + qsTr("at Z %1 mm").arg(Number(st.groundHeight))
            parts.push(floor)
        }
        if (st.referenceArea) // A set reference area (CAD-182); none = the frontal area.
            parts.push(qsTr("Aref %1 cm²").arg(Number((st.referenceArea * 1e4).toPrecision(12))))
        return parts.join(" · ")
    }
    function num(v, digits) {
        return v === null || v === undefined || !isFinite(v) ? "–" : Number(v).toFixed(digits)
    }
    /// Power in W to overcome a drag force at the run speed (CAD-209), or null without both.
    function dragPowerW(dragN, results) {
        const v = results && results.conditions && results.conditions.settings ? results.conditions.settings.speed : undefined
        if (typeof dragN !== "number" || !isFinite(dragN) || !(v > 0))
            return null
        return dragN * v
    }
    /// A power for Results (CAD-218): watts below 1 kW, so a model-sized run does not read
    /// "0.03 kW", and hp (745.7 W) with three digits below 1 hp.
    function powerText(watts) {
        if (watts === null || watts === undefined || !isFinite(watts))
            return "–"
        const hp = watts / 745.7
        const w = Math.abs(watts) < 1000 ? num(watts, Math.abs(watts) < 10 ? 1 : 0) + " W" : num(watts / 1000, 2) + " kW"
        return w + " (" + num(hp, Math.abs(hp) < 1 ? 3 : 2) + " hp)"
    }
    /// A mass for Results (CAD-218): three decimals below 1 kg.
    function massText(kg) {
        return num(kg, Math.abs(kg) < 1 ? 3 : 2) + " kg"
    }
    /// Efficiency |lift| / drag (CAD-184), or null without a positive drag.
    function liftDragRatio(s) {
        if (!s || !(s.dragN > 0) || s.liftN === null || s.liftN === undefined || !isFinite(s.liftN))
            return null
        return Math.abs(s.liftN) / s.dragN
    }
    /// Coefficient times the reference area in cm² (CdA, ClA; CAD-184): does not depend on the area chosen.
    function coefficientArea(coefficient, conditions) {
        const a = conditions ? conditions.referenceArea : undefined
        if (coefficient === null || coefficient === undefined || !(a > 0))
            return null
        return coefficient * a * 1e4
    }
    /// "45 s", "12 min", "1 h 05 min" (CAD-149).
    function duration(seconds) {
        const s = Math.max(0, Math.round(seconds))
        if (s < 60)
            return qsTr("%1 s").arg(s)
        const min = Math.round(s / 60)
        if (min < 60)
            return qsTr("%1 min").arg(min)
        return qsTr("%1 h %2 min").arg(Math.floor(min / 60)).arg(String(min % 60).padStart(2, "0"))
    }
    /// Time so far and at most how long is left, of a running run; "" when unknown.
    function timeText(m) {
        if (!m || m.state !== "running")
            return ""
        const known = v => v !== null && v !== undefined
        const parts = []
        if (known(m.elapsedSeconds))
            parts.push(qsTr("%1 so far").arg(duration(m.elapsedSeconds)))
        if (known(m.remainingSeconds))
            parts.push(qsTr("at most %1 left").arg(duration(m.remainingSeconds)))
        return parts.join(", ")
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
        renamedRun = null
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
        // Copied run settings refer to the closed document's parts (CAD-162).
        copiedContext = null
        copiedContextForeign = 0
        renamingRun = ""
        renameText = ""
        copiedSettings = ({})
        groundFixed = false
        autoSelect = false
        activeRun = ""
        compareRun = ""
        showB = false
        results = null
        resultsB = null
        comparison = null
        fieldStats = ({})
        runSections = ({})
        sectionIndices = ({})
        rangeOverrides = ({})
        showDifference = false
        differenceStats = null
        cpMin.text = ""
        cpMax.text = ""
        // A speed typed for the closed project's run (CAD-211).
        scaleSpeedField.text = ""
        error = ""
        tab = 0 // Results and A/B are empty and disabled now (CAD-214).
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
            loadFieldStats() // Drops the field data of the run that was shown (CAD-215).
            return
        }
        const r = controller.execute("simulation.results", { run: activeRun, maxPoints: 300 })
        results = r.ok ? r.result : null
        loadFieldStats()
    }

    function loadFieldStats() {
        const stats = {}
        // Section stacks of each run (CAD-148): B may be a run made before them.
        const sections = {}
        let rb = null
        for (const id of [activeRun, compareRun]) {
            if (!id)
                continue
            if (id === activeRun) {
                sections[id] = results && results.fields ? results.fields.sections ?? [] : []
            } else {
                const r = controller.execute("simulation.results", { run: id, maxPoints: 300 })
                rb = r.ok ? r.result : null
                sections[id] = rb && rb.fields ? rb.fields.sections ?? [] : []
            }
        }
        resultsB = rb
        runSections = sections
        for (const id of [activeRun, compareRun]) {
            if (!id)
                continue
            const status = runs.find(r => r.id === id)
            if (!status || status.state !== "finished")
                continue // Fields are written at the end of a run.
            for (const f of fieldChoices) {
                const kind = fieldKind(f, id)
                if (!kind || stats[id + "/" + kind])
                    continue
                const r = controller.execute("simulation.field", { run: id, kind: kind })
                if (r.ok && r.result.available)
                    stats[id + "/" + kind] = r.result.scalars
            }
        }
        fieldStats = stats
    }

    /// Exchanges the baseline and the variant (CAD-204): B − A, the % changes and the
    /// balance shift flip sign; the A/B tab stays open.
    function swapAB() {
        if (!activeRun || !compareRun)
            return
        const a = activeRun
        activeRun = compareRun
        compareRun = a
        showB = false
        loadResults()
        loadComparison()
        if (comparison)
            tab = 2
    }

    function loadComparison() {
        if (!activeRun || !compareRun || activeRun === compareRun) {
            comparison = null
            // "Show B" belongs to a pair: unticking or deleting B must not leave it on for the
            // next B ticked (CAD-217).
            showB = false
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
                const s = fieldStats[id + "/" + fieldKind(f, id)]
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
    /// Streamline colours (CAD-170): "speed" (viridis) or "smoke" (thin grey lines, the
    /// slowest air in the strongest contrast to the background, as in pro F1 renders).
    property string streamlineStyle: "speed"
    function paletteFor(scalar, kind) {
        if (kind === "streamlines" && streamlineStyle === "smoke")
            return Theme.dark ? "smoke-dark" : "smoke-light"
        return scalar === "Cp" || scalar === "pressure" ? pressurePalette : ""
    }

    /// The run whose label is being edited in the run list (CAD-174); "" = none.
    property string renamingRun: ""
    property string renameText: ""
    /// Cursor and selection in the name field; null = select all (as it opens).
    property var renameCursor: null
    /// The name field lost focus. A refresh rebuilding the row hands it to the new field; a
    /// click elsewhere (another field, the view) applies the name, as in the Browser (CAD-108).
    function renameFocusLeft() {
        // Only the run whose field lost the focus: a click on another run's ✎ has already
        // applied it and opened the other one (startRename).
        const id = renamingRun
        Qt.callLater(() => {
            const f = root.Window.activeFocusItem
            if (!id || renamingRun !== id || !f || (f.objectName || "").indexOf("runRenameField_") === 0)
                return
            renameRun(id, renameText)
        })
    }
    function startRename(run) {
        // Switching to another run applies the pending edit, like any other click elsewhere.
        if (renamingRun && renamingRun !== run.id)
            renameRun(renamingRun, renameText)
        renameText = run.source.label || ""
        renameCursor = null
        renamingRun = run.id
    }
    function renameRun(id, label) {
        const r = controller.execute("simulation.rename", { run: id, label: label })
        renamingRun = ""
        if (!r.ok) {
            controller.showStatus(qsTr("Rename failed: %1").arg(r.error))
            return false
        }
        // The list is rebuilt a moment later, not now: the rename is often applied on the press
        // of another row button (focus leaves the field), and rebuilding the rows here would
        // destroy that button before its click. The row shows the new name meanwhile.
        renamedRun = { id: id, label: label.trim() }
        renameRefresh.restart()
        return true
    }
    property var renamedRun: null
    Timer {
        id: renameRefresh
        interval: 400
        onTriggered: root.refreshRuns()
    }

    /// Where the last results export went, or why it failed (CAD-167).
    property string exportNote: ""
    readonly property alias resultsDialog: resultsExportDialog
    onActiveRunChanged: {
        exportNote = ""
        // Results is disabled without a run: never leave the panel on it, empty (CAD-214).
        if (!activeRun && tab === 1)
            tab = 0
    }
    function openResultsExport() {
        const m = activeStatus
        // The run id already holds the part name and the time (CAD-167).
        const base = m ? (m.source.label ? m.source.label + " " : "") + m.id : "results"
        const safe = base.replace(/[\\/:*?"<>|]+/g, "_").replace(/\s+/g, " ").trim()
        const folder = resultsExportDialog.currentFolder.toString().replace(/\/$/, "")
        resultsExportDialog.selectedFile = folder + "/" + encodeURIComponent(safe) + ".csv"
        resultsExportDialog.open()
    }
    function exportResults(path) {
        const r = controller.execute("simulation.export_results", { run: activeRun, path: path })
        exportNote = r.ok ? qsTr("%n iteration(s) written to %1", "", r.result.rows).arg(path)
                          : qsTr("Export failed: %1").arg(r.error)
        return r.ok
    }
    FileDialog {
        id: resultsExportDialog
        title: qsTr("Export results")
        fileMode: FileDialog.SaveFile
        acceptLabel: qsTr("Export")
        defaultSuffix: "csv"
        nameFilters: [qsTr("CSV files (*.csv)")]
        onAccepted: root.exportResults(root.controller.localPath(selectedFile))
    }

    // The A/B table to a spreadsheet or report (CAD-193).
    property string abExportNote: ""
    property string abExportPair: ""
    readonly property alias comparisonDialog: comparisonExportDialog
    function openComparisonExport() {
        if (!comparison)
            return
        // e.g. "A 20261109-143131-box vs B 20261109-143217-box": the label, if any, and the run id.
        const title = id => { const r = runs.find(x => x.id === id); return (r && r.source && r.source.label ? r.source.label + " " : "") + id }
        const name = title(comparison.a) + " vs " + title(comparison.b)
        const safe = name.replace(/[\\/:*?"<>|]+/g, "_").replace(/\s+/g, " ").trim()
        const folder = comparisonExportDialog.currentFolder.toString().replace(/\/$/, "")
        comparisonExportDialog.selectedFile = folder + "/" + encodeURIComponent(safe) + ".csv"
        comparisonExportDialog.open()
    }
    function exportComparison(path) {
        if (!comparison)
            return
        const r = controller.execute("simulation.export_comparison", { a: comparison.a, b: comparison.b, path: path })
        abExportNote = r.ok ? qsTr("A/B table written to %1").arg(path) : qsTr("Export failed: %1").arg(r.error)
    }
    FileDialog {
        id: comparisonExportDialog
        title: qsTr("Export A/B comparison")
        fileMode: FileDialog.SaveFile
        acceptLabel: qsTr("Export")
        defaultSuffix: "csv"
        nameFilters: [qsTr("CSV files (*.csv)")]
        onAccepted: root.exportComparison(root.controller.localPath(selectedFile))
    }

    /// Settings of a new run as they stand in the panel (simulation.run / propose_run).
    /// A floor copied from a run with a fixed (not moving) floor (CAD-162); the form has no
    /// control for it, so it is kept until the floor box is changed.
    property bool groundFixed: false
    /// Conditions of the copied run that the form has no controls for (air properties,
    /// turbulence, reference area and length, preset), sent with the run unchanged (CAD-162).
    property var copiedSettings: ({})
    /// The surroundings of the copied run, by part id; null = use the visible parts.
    property var copiedContext: null
    /// Surrounding parts of a copied run from another design: part ids are per document,
    /// so they cannot be restored here; the count is shown as a warning instead.
    property int copiedContextForeign: 0
    /// Parts of the copied surroundings that are no longer in the document.
    readonly property int copiedContextMissing: {
        void partRevision // re-checked when parts are removed, undone or redone
        if (!copiedContext || !contextBox.checked)
            return 0
        const parts = controller.execute("document.parts")
        const ids = parts.ok ? parts.result.parts.map(p => p.id) : []
        return copiedContext.filter(id => ids.indexOf(id) < 0).length
    }
    /// Fill the New run form with the conditions of run `id` (CAD-162), so a variant B is
    /// run exactly as A was. Returns false when the run's conditions are not known.
    function useSettingsOf(id) {
        const r = controller.execute("simulation.results", { run: id, maxPoints: 2 })
        if (!r.ok || !r.result.conditions)
            return false
        const st = r.result.conditions.settings
        speedField.text = String(st.speed)
        const flow = flowBox.indexOfValue(st.flowDirection)
        if (flow >= 0)
            flowBox.currentIndex = flow
        yawField.text = st.yaw ? String(st.yaw) : "" // CAD-188; older runs: none.
        const mesh = meshBox.indexOfValue(st.mesh)
        if (mesh >= 0)
            meshBox.currentIndex = mesh
        groundBox.checked = !!st.ground
        groundFixed = !!st.ground && st.groundMoving === false
        wheelsBox.checked = !!st.ground && !!st.rotatingWheels
        floorField.text = st.ground && st.groundHeight !== null && st.groundHeight !== undefined ? String(st.groundHeight) : ""
        // The reference area has its own field (CAD-182): empty = the frontal area, as in that run.
        refAreaField.text = st.referenceArea ? String(Number((st.referenceArea * 1e4).toPrecision(12))) : ""
        const extra = {}
        for (const key of ["preset", "density", "kinematicViscosity", "turbulenceIntensity", "viscosityRatio",
                           "referenceLength", "iterations"])
            if (st[key] !== null && st[key] !== undefined)
                extra[key] = st[key]
        copiedSettings = extra
        const run = runs.find(x => x.id === id)
        const context = run && run.source ? (run.source.contextPartIds || []) : []
        // Only a run of this design names its surroundings by ids of this document; the same
        // ids in another design may be unrelated parts.
        const own = !!run && run.sourceInDocument
        contextBox.checked = own && context.length > 0
        copiedContext = own && context.length > 0 ? context.slice() : null
        copiedContextForeign = own ? 0 : context.length
        proposalTimer.restart()
        return true
    }

    /// The speed typed in the New run form in m/s, or NaN: a number (decimal point or comma)
    /// with an optional unit m/s, km/h (kmh, kph) or mph (CAD-180).
    function speedMs(text) {
        const m = /^\s*(\d+(?:[.,]\d*)?|[.,]\d+)\s*(m\/s|ms|km\/h|kmh|kph|mph)?\s*$/i.exec(text || "")
        if (!m)
            return NaN
        const v = Number(m[1].replace(",", "."))
        const unit = (m[2] || "m/s").toLowerCase()
        // A converted speed is kept to 4 decimals, so "Use the settings of" shows it short and
        // copies it exactly (same speed, same case).
        const round = x => Math.round(x * 1e4) / 1e4
        return unit === "km/h" || unit === "kmh" || unit === "kph" ? round(v / 3.6)
             : unit === "mph" ? round(v * 0.44704) : v
    }
    /// The reference area typed in the New run form in m², null when empty (the frontal area),
    /// or NaN when it is not a number above 0 (CAD-182).
    function typedRefArea() {
        const t = refAreaField.text.trim()
        if (t.length === 0)
            return null
        const v = /^(\d+(?:[.,]\d*)?|[.,]\d+)$/.test(t) ? Number(t.replace(",", ".")) : NaN
        return v > 0 ? v / 1e4 : NaN
    }
    readonly property bool refAreaValid: refAreaField.text.trim().length === 0 || typedRefArea() > 0
    /// The yaw typed in the New run form in degrees: 0 when empty, NaN when not a number
    /// (CAD-188). Valid from -30 to 30.
    function typedYaw() {
        const t = yawField.text.trim()
        if (t.length === 0)
            return 0
        return /^[+-]?(\d+(?:[.,]\d*)?|[.,]\d+)$/.test(t) ? Number(t.replace(",", ".")) : NaN
    }
    readonly property bool yawValid: Math.abs(typedYaw()) <= 30
    readonly property real typedSpeed: speedMs(speedField.text)
    readonly property bool speedValid: typedSpeed > 0 && typedSpeed <= 150
    /// Under the speed field: the speed in both units, or why it cannot be used.
    readonly property string speedHint: {
        const v = typedSpeed
        if (isNaN(v))
            return qsTr("Type a speed, e.g. 20 (m/s), 72 km/h or 45 mph.")
        if (!(v > 0))
            return qsTr("The speed must be above 0.")
        if (v > 150)
            return qsTr("%1 m/s is above 150 m/s, the limit of this incompressible-flow solver.").arg(v.toFixed(1))
        return qsTr("= %1 m/s = %2 km/h").arg(Number(v.toFixed(2))).arg(Number((v * 3.6).toFixed(1)))
    }

    function runParams() {
        const params = Object.assign({}, copiedSettings, {
            id: partInfo.id,
            speed: speedMs(speedField.text),
            flowDirection: flowBox.currentValue,
            mesh: meshBox.currentValue,
            ground: groundBox.checked
        })
        const yaw = typedYaw()
        if (yaw !== 0 && isFinite(yaw))
            params.yaw = yaw
        const area = typedRefArea()
        if (area > 0)
            params.referenceArea = area
        if (groundBox.checked && groundFixed)
            params.groundMoving = false
        // CAD-154: the tyres stand on the moving floor and turn with it.
        if (groundBox.checked && wheelsBox.checked)
            params.rotatingWheels = true
        // The road under a part mounted on a car (CAD-147); empty = just below the parts.
        if (groundBox.checked && !wheelsBox.checked && floorField.text.trim().length > 0)
            params.groundHeight = Number(floorField.text.replace(",", "."))
        if (contextBox.checked && copiedContext) {
            // The copied run's own surroundings, not whatever is visible now (CAD-162).
            const parts = controller.execute("document.parts")
            const ids = parts.ok ? parts.result.parts.map(p => p.id) : []
            params.context = copiedContext.filter(id => id !== partInfo.id && ids.indexOf(id) >= 0)
        } else if (contextBox.checked) {
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
    /// Why the planned run cannot be set up (for example a floor above the part); "" = fine.
    property string proposalError: ""
    function refreshProposal() {
        if (!visible || tab !== 0 || !partInfo.id || partInfo.kind === "sketch" || partInfo.kind === "plane") {
            proposal = null
            proposalError = ""
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
        proposalError = r.ok ? "" : r.error
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

    // --- Section positions (CAD-148) -------------------------------------------------------
    /// Section stacks of the loaded runs: {runId: fields.sections of simulation.results}.
    property var runSections: ({})
    /// Plane picked on each section slider: {axis: index in the stack}. Kept apart from
    /// fieldChoices, the model of the field rows, so moving a slider does not recreate the
    /// rows and drop the slider being dragged.
    property var sectionIndices: ({})
    /// A run's stack of sections normal to `axis` ("x" across the flow, "y" vertical,
    /// "z" horizontal), by default of the run drawn in 3D: {axis, defaultIndex, lengthMm,
    /// positions: [{kind, index, fraction, mm}]}, or null for runs made before the stacks.
    function sectionStack(axis, run) {
        const id = run ?? displayedRun
        const stacks = id ? runSections[id] : null
        return axis && stacks ? stacks.find(st => st.axis === axis) ?? null : null
    }
    /// Position of a choice in its stack: the slider value, or the stack's default.
    function sectionPosition(f, run) {
        const st = sectionStack(f.axis, run)
        if (!st)
            return null
        const want = f.pos ?? sectionIndices[f.axis] ?? st.defaultIndex
        let best = st.positions[0]
        for (const p of st.positions)
            if (Math.abs(p.index - want) < Math.abs(best.index - want))
                best = p
        return best
    }
    /// Field kind drawn for a choice: a plane of the run's section stack when it has one,
    /// else the fixed section through the part centre ("sectionCross" has none: null).
    function fieldKind(f, run) {
        const p = f.axis ? sectionPosition(f, run) : null
        if (p)
            return p.kind
        return f.kind === "sectionCross" ? null : f.kind
    }
    function setSectionIndex(axis, pos) {
        if (sectionIndices[axis] === pos)
            return
        const copy = Object.assign({}, sectionIndices)
        copy[axis] = pos
        sectionIndices = copy
        loadFieldStats()
    }
    /// "450 mm behind the front", "120 mm left of the centre", "80 mm above the bottom".
    function sectionPositionText(axis, p) {
        if (!p)
            return ""
        const mm = Math.round(p.mm)
        if (axis === "x")
            return mm < 0 ? qsTr("%1 mm ahead of the part").arg(-mm) : qsTr("%1 mm behind the part's front").arg(mm)
        if (axis === "y")
            return mm === 0 ? qsTr("through the centre") : qsTr("%1 mm from the centre").arg(Math.abs(mm))
        return qsTr("%1 mm above the part's bottom").arg(mm)
    }

    /// Cd of B per iteration on the reference area of A, like the A/B table (CAD-179).
    function cdOfBOnAreaOfA() {
        const h = resultsB && resultsB.history ? resultsB.history : null
        if (!h || !h.cd)
            return []
        const areaA = results && results.conditions ? results.conditions.referenceArea || 0 : 0
        const areaB = resultsB.conditions ? resultsB.conditions.referenceArea || 0 : 0
        const k = areaA > 0 && areaB > 0 ? areaB / areaA : 1
        return h.cd.map(v => v === null || v === undefined ? v : v * k)
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
                Qt.callLater(root.refreshRuns)
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
        objectName: "cfdPanelScroll"
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true
        // Nothing scrolls sideways; a horizontal bar would only cover the last row (the
        // "Runs from other designs" toggle could not be clicked once the panel scrolls).
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

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
                    // A variant B runs under A's conditions with one click (CAD-162).
                    Button {
                        objectName: "cfdUseSettingsOfA"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: !!root.activeRun
                        text: qsTr("Use the settings of %1").arg(root.activeRun ? root.runTitle(root.activeRun, "") : "A")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Speed, flow direction, mesh, floor and surroundings of the selected run (A), so a variant can be compared with it")
                        onClicked: root.useSettingsOf(root.activeRun)
                    }
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
                    // An open shell (raw scan, surface) has no inside to put in the flow (CAD-145).
                    Label {
                        objectName: "cfdOpenWarning"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: root.partInfo.id ? controller.partOpenWarning(root.partInfo.id) : ""
                        visible: text.length > 0
                        color: Theme.warning
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                    Label { text: qsTr("Speed"); opacity: 0.7 }
                    TextField {
                        id: speedField
                        objectName: "cfdSpeedField"
                        Layout.fillWidth: true
                        text: "20"
                        placeholderText: qsTr("m/s, or with a unit: 72 km/h, 45 mph")
                        onEditingFinished: proposalTimer.restart()
                    }
                    Item { implicitWidth: 1; implicitHeight: 1 }
                    Label {
                        objectName: "cfdSpeedHint"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        text: root.speedHint
                        color: root.speedValid ? Theme.textMuted : Theme.warning
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
                    // Crosswind (CAD-188): the air turned this far counter-clockwise, seen from above.
                    Label { text: qsTr("Yaw (°)"); opacity: 0.7 }
                    TextField {
                        id: yawField
                        objectName: "cfdYawField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("0 (straight); crosswind e.g. 5")
                        onTextEdited: proposalTimer.restart()
                    }
                    Label {
                        objectName: "cfdYawError"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: !root.yawValid
                        text: qsTr("The yaw must be a number from -30 to 30 degrees, or empty for 0.")
                        color: Theme.warning
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
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
                    // Cd/Cl/Cs on this area (CAD-182): empty = the frontal area of the part; a wing's
                    // lift is usually given on its planform area.
                    Label { text: qsTr("Ref. area (cm²)"); opacity: 0.7 }
                    TextField {
                        id: refAreaField
                        objectName: "cfdRefAreaField"
                        Layout.fillWidth: true
                        placeholderText: root.proposal && root.proposal.referenceArea > 0
                                         ? qsTr("auto: %1 cm² (frontal)").arg((root.proposal.referenceArea * 1e4).toFixed(1))
                                         : qsTr("auto: frontal area")
                        onTextEdited: proposalTimer.restart()
                    }
                    Label {
                        objectName: "cfdRefAreaError"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: !root.refAreaValid
                        text: qsTr("The reference area must be a number above 0 (cm²), or empty for the frontal area.")
                        color: Theme.warning
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                    Label { text: qsTr("Label"); opacity: 0.7 }
                    TextField { id: labelField; Layout.fillWidth: true; placeholderText: qsTr("e.g. v2, long splitter") }
                    CheckBox {
                        id: groundBox
                        objectName: "cfdGroundBox"
                        Layout.columnSpan: 2
                        text: root.groundFixed ? qsTr("Fixed floor under the geometry (as in A)")
                                               : qsTr("Moving floor under the geometry")
                        onToggled: { root.groundFixed = false; proposalTimer.restart() }
                    }
                    // A car (CAD-154): its wheels found on the part's bottom stand on the floor and turn.
                    CheckBox {
                        id: wheelsBox
                        objectName: "cfdRotatingWheels"
                        Layout.columnSpan: 2
                        Layout.leftMargin: 24
                        visible: groundBox.checked
                        text: qsTr("Rotating wheels (on the floor)")
                        // Rotating wheels need a moving floor: a fixed floor copied from A is dropped.
                        onToggled: { if (checked) root.groundFixed = false; proposalTimer.restart() }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("For a car: the wheels standing on its bottom are found, the floor touches the tyres and they turn with it.")
                    }
                    // Where the road is (CAD-147): a part mounted on a car sits well above it.
                    Label { text: qsTr("Floor at Z (mm)"); opacity: 0.7; visible: groundBox.checked && !wheelsBox.checked }
                    TextField {
                        id: floorField
                        objectName: "cfdFloorField"
                        Layout.fillWidth: true
                        visible: groundBox.checked && !wheelsBox.checked
                        placeholderText: qsTr("just below the parts")
                        validator: RegularExpressionValidator { regularExpression: /^-?\d*([.,]\d*)?$/ }
                        onTextEdited: proposalTimer.restart()
                    }
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: groundBox.checked && !wheelsBox.checked
                        text: qsTr("Where the road is. Empty puts it just below the parts; for a part on a car, give the road's Z so the gap is as on the car.")
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                    CheckBox {
                        id: contextBox
                        Layout.columnSpan: 2
                        onToggled: { root.copiedContext = null; root.copiedContextForeign = 0; proposalTimer.restart() }
                        text: root.copiedContext
                              ? qsTr("Surroundings of A in the flow: %n part(s) (not in the forces)", "", root.copiedContext.length)
                              : qsTr("Other visible parts in the flow (not in the forces)")
                    }
                    Label {
                        objectName: "cfdCopiedNote"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.copiedContextForeign > 0
                              ? qsTr("A ran with %n surrounding part(s) of another design; they cannot be restored here, so B's flow differs from A's.", "", root.copiedContextForeign)
                              : root.copiedContextMissing > 0
                              ? qsTr("%n part(s) of A's surroundings are no longer in the design, so B's flow differs from A's.", "", root.copiedContextMissing)
                              : ""
                        color: Theme.warning
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                    }
                    Button {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: qsTr("Run CFD")
                        highlighted: true
                        enabled: !!root.partInfo.id && root.partInfo.kind !== "sketch" && root.partInfo.kind !== "plane"
                                 && root.speedValid && root.refAreaValid && root.yawValid
                        onClicked: root.startRun()
                    }
                    // Why this run cannot be set up, before Run CFD is pressed (CAD-147).
                    Label {
                        objectName: "cfdProposalError"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.proposalError
                        color: Theme.warning
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
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
            RowLayout {
                Layout.fillWidth: true
                visible: root.tab === 0 && root.runs.length > 0
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Click a run to show it (A). B: compare with A.")
                    opacity: 0.6
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
                // Rank the variants by their final result (CAD-207).
                Label { text: qsTr("Sort"); opacity: 0.7; font.pixelSize: 11; visible: root.runs.length > 1 }
                ComboBox {
                    objectName: "cfdRunSort"
                    visible: root.runs.length > 1
                    textRole: "text"
                    valueRole: "value"
                    model: [{ value: "newest", text: qsTr("Newest") },
                            { value: "drag", text: qsTr("Lowest drag") },
                            { value: "cd", text: qsTr("Lowest Cd") },
                            { value: "downforce", text: qsTr("Most downforce") }]
                    currentIndex: Math.max(0, model.findIndex(m => m.value === root.runSort))
                    onActivated: root.runSort = currentValue
                }
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
                    readonly property string label: root.renamedRun && root.renamedRun.id === modelData.id ?
                                                       root.renamedRun.label : (modelData.source.label || "")
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
                                visible: root.renamingRun !== runRow.modelData.id
                                elide: Text.ElideRight
                                font.bold: true
                                text: (runRow.isA ? "[A] " : runRow.isB ? "[B] " : "") +
                                      (runRow.label ? runRow.label + " · " : "") +
                                      (runRow.modelData.source.partName || runRow.modelData.id)
                            }
                            // Rename in place (CAD-174): Enter or leaving the field applies, Esc cancels.
                            TextField {
                                id: renameField
                                objectName: "runRenameField_" + runRow.modelData.id
                                Layout.fillWidth: true
                                visible: root.renamingRun === runRow.modelData.id
                                placeholderText: qsTr("Label, e.g. v2, long splitter (empty: none)")
                                maximumLength: 80
                                // The run list is rebuilt on every refresh, so the typed text
                                // lives in root.renameText and a new row picks it up.
                                // The cursor and selection are kept there too, so a correction in
                                // the middle of the name goes on where it was.
                                property bool restoring: false
                                function begin() {
                                    if (!visible)
                                        return
                                    restoring = true
                                    text = root.renameText
                                    const c = root.renameCursor
                                    if (!c)
                                        selectAll()
                                    else if (c.start !== c.end)
                                        c.cursor === c.start ? select(c.end, c.start) : select(c.start, c.end)
                                    else
                                        cursorPosition = c.cursor
                                    restoring = false
                                    forceActiveFocus()
                                }
                                function keep() {
                                    if (!restoring && visible)
                                        root.renameCursor = { start: selectionStart, end: selectionEnd, cursor: cursorPosition }
                                }
                                Component.onCompleted: begin()
                                onVisibleChanged: begin()
                                onTextEdited: { root.renameText = text; keep() }
                                onActiveFocusChanged: if (!activeFocus) root.renameFocusLeft()
                                onCursorPositionChanged: keep()
                                onSelectionStartChanged: keep()
                                onSelectionEndChanged: keep()
                                onAccepted: root.renameRun(runRow.modelData.id, text)
                                // Esc belongs to the field, not to the window's Esc shortcut.
                                Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
                                Keys.onEscapePressed: root.renamingRun = ""
                            }
                            ToolButton {
                                objectName: "renameButton_" + runRow.modelData.id
                                visible: root.renamingRun !== runRow.modelData.id
                                text: "✎"
                                // Keeps the focus in an open name field while pressed (as on
                                // macOS), so the click reaches startRename, which applies that
                                // name first, instead of the field applying it on press and
                                // rebuilding the list under the button.
                                focusPolicy: Qt.NoFocus
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Rename run")
                                onClicked: root.startRename(runRow.modelData)
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
                                onClicked: root.confirmRunAction("cancel", runRow.modelData.id)
                            }
                            ToolButton {
                                visible: runRow.modelData.state !== "running"
                                text: "✕"
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Delete run")
                                onClicked: root.confirmRunAction("delete", runRow.modelData.id)
                            }
                        }
                        Label {
                            objectName: "runSummary_" + runRow.modelData.id
                            font.pixelSize: 11
                            opacity: 0.75
                            text: {
                                const m = runRow.modelData
                                const l = m.latest || {}
                                let s = m.state
                                if (m.state === "running")
                                    s += " · " + m.stage + (m.stage === "solve" ? " " + m.iteration + "/" + m.maxIterations : "")
                                        + (m.remainingSeconds !== null && m.remainingSeconds !== undefined
                                           ? " · " + qsTr("≤ %1 left").arg(root.duration(m.remainingSeconds)) : "")
                                // A finished run shows its final result, the same numbers as
                                // Results (CAD-205); a running one its latest iteration.
                                if (m.final)
                                    s += " · Cd " + root.num(m.final.cd, 3) + " · drag " + root.num(m.final.dragN, 3) + " N"
                                         // The value a downforce ranking goes by (CAD-207).
                                         + (root.runSort === "downforce" ? " · lift " + root.num(m.final.liftN, 3) + " N" : "")
                                else if (l.cd !== null && l.cd !== undefined && m.state !== "finished")
                                    s += " · provisional Cd " + root.num(l.cd, 3) + " · drag " + root.num(l.dragN, 3) + " N"
                                return s
                            }
                        }
                        Label {
                            objectName: "runConditions_" + runRow.modelData.id
                            Layout.fillWidth: true
                            visible: text.length > 0
                            font.pixelSize: 11
                            color: Theme.textMuted
                            wrapMode: Text.WordWrap
                            text: root.runConditions(runRow.modelData.settings)
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
                        // While the run runs every check is still missing: judged after the run (CAD-150).
                        readonly property bool running: root.activeRunning
                        readonly property string status: running ? "running"
                                                         : root.results ? root.results.quality.status : "unknown"
                        radius: 3
                        implicitWidth: qualityLabel.implicitWidth + 10
                        implicitHeight: qualityLabel.implicitHeight + 4
                        color: status === "ok" ? Theme.success : status === "warning" ? Theme.warning : Theme.textFaint
                        Label {
                            id: qualityLabel
                            anchors.centerIn: parent
                            color: "white"
                            font.pixelSize: 11
                            text: parent.running ? qsTr("checks: after the run") : qsTr("checks: %1").arg(parent.status)
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
                            const time = root.timeText(m)
                            return m.state === "running" ? qsTr("Running: %1").arg(step) + (time ? " · " + time : "")
                                                         : m.state + (m.message ? ": " + m.message : "")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: !!root.activeStatus && root.activeStatus.state === "running"
                        ProgressBar { Layout.fillWidth: true; value: root.activeStatus ? root.activeStatus.progress : 0 }
                        Button {
                            text: qsTr("Cancel")
                            onClicked: root.confirmRunAction("cancel", root.activeRun)
                        }
                    }
                }

                Label {
                    text: root.results && root.results.finalResults.available ? qsTr("Final results")
                          : root.activeRunning && root.results && root.results.current
                            ? qsTr("Current values (provisional): mean of the last 20 % so far")
                          : root.activeRunning ? qsTr("Final results when the run has finished")
                          : qsTr("Final results unavailable")
                    font.bold: true
                }
                Repeater {
                    model: root.results && !root.activeRunning ? root.results.finalResults.reasons : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.warning
                        text: modelData
                    }
                }
                GridLayout {
                    objectName: "cfdSummaryGrid"
                    columns: 4
                    columnSpacing: 10
                    rowSpacing: 2
                    // While running: the current means so far (CAD-158), dimmed as provisional.
                    readonly property bool current: !!root.results && !root.results.finalResults.available
                                                    && root.activeRunning && !!root.results.current
                    readonly property var s: !root.results ? ({}) : current ? root.results.current : root.results.summary
                    opacity: current ? 0.75 : 1.0
                    Label { text: qsTr("Drag"); opacity: 0.7 }
                    Label {
                        objectName: "cfdSummaryDrag"
                        text: root.num(parent.s.dragN, 3) + root.spreadText(parent.s, "dragN", 3) + " N"
                        font.bold: true
                    }
                    Label { text: "Cd"; opacity: 0.7 }
                    Label { text: root.num(parent.s.cd, 3); font.bold: true }
                    Label { text: qsTr("Lift"); opacity: 0.7 }
                    Label {
                        objectName: "cfdSummaryLift"
                        // Lift is along +Z: a negative lift is downforce (CAD-184).
                        text: root.num(parent.s.liftN, 3) + root.spreadText(parent.s, "liftN", 3) + " N"
                              + (parent.s.liftN < 0 ? qsTr(" (downforce)") : "")
                        font.bold: true
                    }
                    Label { text: "Cl"; opacity: 0.7 }
                    Label { text: root.num(parent.s.cl, 3); font.bold: true }
                    Label { text: qsTr("Side"); opacity: 0.7 }
                    Label { text: root.num(parent.s.sideN, 3) + root.spreadText(parent.s, "sideN", 3) + " N"; font.bold: true }
                    Label { text: "Cs"; opacity: 0.7 }
                    Label { text: root.num(parent.s.cs, 3); font.bold: true }
                    Label { text: qsTr("Efficiency L/D"); opacity: 0.7; Layout.columnSpan: 2 }
                    Label {
                        objectName: "cfdSummaryLD"
                        Layout.columnSpan: 2
                        readonly property var ratio: root.liftDragRatio(parent.s)
                        text: root.num(ratio, 2) + (ratio !== null && parent.s.liftN < 0 ? qsTr(" (downforce)") : "")
                    }
                    Label { text: qsTr("CdA / ClA"); opacity: 0.7; Layout.columnSpan: 2 }
                    Label {
                        objectName: "cfdSummaryCoefficientArea"
                        Layout.columnSpan: 2
                        readonly property var conditions: root.results ? root.results.conditions : null
                        text: root.num(root.coefficientArea(parent.s.cd, conditions), 1) + " / "
                              + root.num(root.coefficientArea(parent.s.cl, conditions), 1) + " cm²"
                    }
                    // Power to push the part through the air at the run speed, and downforce as a mass
                    // (CAD-209): the units motorsport aero is talked about in.
                    Label {
                        text: qsTr("Drag power")
                        opacity: 0.7
                        Layout.columnSpan: 2
                        visible: cfdDragPower.visible
                    }
                    Label {
                        id: cfdDragPower
                        objectName: "cfdSummaryDragPower"
                        Layout.columnSpan: 2
                        readonly property var watts: root.dragPowerW(parent.s.dragN, root.results)
                        visible: watts !== null
                        text: !visible ? "" : root.powerText(watts)
                    }
                    Label {
                        text: qsTr("Downforce")
                        opacity: 0.7
                        Layout.columnSpan: 2
                        visible: cfdDownforceMass.visible
                    }
                    Label {
                        id: cfdDownforceMass
                        objectName: "cfdSummaryDownforceKg"
                        Layout.columnSpan: 2
                        visible: typeof parent.s.liftN === "number" && isFinite(parent.s.liftN) && parent.s.liftN < 0
                        text: !visible ? "" : root.massText(-parent.s.liftN / 9.80665)
                    }
                    // Aero balance of a car with two axles of turning wheels (CAD-194), in two short
                    // rows: one long line would widen the grid and push the panel past its width.
                    Label {
                        text: qsTr("Front / rear axle")
                        opacity: 0.7
                        Layout.columnSpan: 2
                        visible: typeof parent.s.frontLiftN === "number"
                    }
                    Label {
                        objectName: "cfdSummaryBalance"
                        Layout.columnSpan: 2
                        visible: typeof parent.s.frontLiftN === "number"
                        text: !visible ? "" : root.num(parent.s.frontLiftN, 3) + " / " + root.num(parent.s.rearLiftN, 3) + " N"
                    }
                    Label {
                        text: qsTr("Aero balance (front)")
                        opacity: 0.7
                        Layout.columnSpan: 2
                        visible: typeof parent.s.frontLiftN === "number"
                    }
                    Label {
                        objectName: "cfdSummaryFrontShare"
                        Layout.columnSpan: 2
                        visible: typeof parent.s.frontLiftN === "number"
                        // No share when one axle is pushed down and the other lifted.
                        text: typeof parent.s.frontShare === "number" ? root.num(parent.s.frontShare, 1) + " %" : "–"
                    }
                    // The turning wheels' own forces (CAD-202); the body has the rest.
                    Label {
                        text: qsTr("Wheels drag / lift")
                        opacity: 0.7
                        Layout.columnSpan: 2
                        visible: typeof parent.s.wheelDragN === "number"
                    }
                    Label {
                        objectName: "cfdSummaryWheels"
                        Layout.columnSpan: 2
                        visible: typeof parent.s.wheelDragN === "number"
                        text: !visible ? "" : root.num(parent.s.wheelDragN, 3) + " / " + root.num(parent.s.wheelLiftN, 3) + " N"
                    }
                    Label {
                        text: qsTr("Wheels share of drag")
                        opacity: 0.7
                        Layout.columnSpan: 2
                        visible: typeof parent.s.wheelDragN === "number"
                    }
                    Label {
                        objectName: "cfdSummaryWheelShare"
                        Layout.columnSpan: 2
                        visible: typeof parent.s.wheelDragN === "number"
                        text: !visible ? "" : parent.s.dragN > 0 ? root.num(100 * parent.s.wheelDragN / parent.s.dragN, 1) + " %" : "–"
                    }
                    Label { text: qsTr("Pressure / friction"); opacity: 0.7; Layout.columnSpan: 2 }
                    Label {
                        Layout.columnSpan: 2
                        text: root.num(parent.s.pressureDragN, 3) + " / " + root.num(parent.s.viscousDragN, 3) + " N"
                    }
                }
                Label {
                    Layout.fillWidth: true
                    // The layout keeps the height of the first wrap otherwise, so a text
                    // that wraps to fewer or more lines later overlaps its neighbours. The
                    // cap is the panel's width, not the parent's: the parent's width comes
                    // from this layout, and Qt aborts that polish loop with later rows unplaced.
                    Layout.maximumWidth: scroll.availableWidth
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    opacity: 0.7
                    text: {
                        const c = root.results ? root.results.conditions : null
                        if (!c)
                            return ""
                        const conditions = qsTr("%1 m/s, flow %2, %3 mesh%4 · Aref %5 cm² · OpenFOAM %6")
                            .arg(c.settings.speed)
                            .arg(c.settings.flowDirection + (c.settings.yaw ? qsTr(" at yaw %1°").arg(Number(c.settings.yaw)) : ""))
                            .arg(c.settings.mesh)
                            .arg(!c.settings.ground ? "" : c.settings.groundMoving === false ? qsTr(", fixed floor")
                                 : c.settings.rotatingWheels ? qsTr(", moving floor, %n rotating wheel(s)", "", (c.wheels || []).length)
                                 : qsTr(", moving floor"))
                            .arg(root.num(c.referenceArea * 1e4, 1)).arg(c.openfoam)
                        return conditions + (root.results.provisional
                            ? qsTr(" · %1 recorded iterations; provisional data")
                            : qsTr(" · mean of the last 20 % of %1 iterations")).arg(root.results.summary.iterations)
                    }
                }
                // The final forces at another speed (CAD-208): same Cd/Cl, forces ∝ speed².
                RowLayout {
                    Layout.fillWidth: true
                    visible: !!root.results && !!root.results.conditions && !root.results.provisional
                             && typeof root.results.summary.dragN === "number"
                    Label { text: qsTr("At speed"); opacity: 0.7 }
                    TextField {
                        id: scaleSpeedField
                        objectName: "cfdScaleSpeed"
                        Layout.preferredWidth: 110
                        placeholderText: qsTr("e.g. 200 km/h")
                    }
                    Label {
                        objectName: "cfdScaleResult"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: {
                            const t = scaleSpeedField.text.trim()
                            if (t.length === 0 || !parent.visible)
                                return ""
                            const v = root.speedMs(t), v0 = root.results.conditions.settings.speed
                            if (!(v > 0) || !(v0 > 0))
                                return qsTr("a speed, e.g. 200 km/h, 55 m/s or 120 mph")
                            const k = (v / v0) * (v / v0), s = root.results.summary
                            // Drag power grows with speed³ (CAD-209).
                            const w = s.dragN * k * v
                            return qsTr("drag %1 N · lift %2 N · %4 (×%3)").arg(root.num(s.dragN * k, 1))
                                .arg(root.num(s.liftN * k, 1)).arg(root.num(k, 2))
                                .arg(Math.abs(w) < 1000 ? root.num(w, 0) + " W" : root.num(w / 1000, 2) + " kW")
                        }
                    }
                }
                Label {
                    visible: scaleSpeedField.text.trim().length > 0 && scaleSpeedField.parent.visible
                    Layout.fillWidth: true
                    Layout.maximumWidth: scroll.availableWidth
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    opacity: 0.7
                    text: qsTr("Scaled by (speed / run speed)² with this run's Cd and Cl; Reynolds-number effects are not included.")
                }
                // Forces and coefficients to a spreadsheet or report (CAD-167).
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "cfdExportCsv"
                        text: qsTr("Export CSV…")
                        onClicked: root.openResultsExport()
                    }
                    Label {
                        objectName: "cfdExportNote"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        font.pixelSize: 11
                        opacity: 0.7
                        text: root.exportNote
                    }
                }
                Repeater {
                    model: root.results && !root.activeRunning ? root.qualityLines(root.results.quality) : []
                    delegate: Label {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.maximumWidth: scroll.availableWidth // See the conditions above.
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: modelData.ok ? Theme.success : Theme.warning
                        text: (modelData.ok ? "✓ " : "⚠ ") + modelData.text
                    }
                }

                LineChart {
                    objectName: "cfdChartCoefficients"
                    Layout.fillWidth: true
                    title: root.results && root.results.provisional ? qsTr("Coefficients (provisional)") : qsTr("Coefficients")
                    series: root.results ? [
                        root.series("Cd", Theme.chart1, root.results.history.iteration, root.results.history.cd),
                        root.series("Cl", Theme.chart2, root.results.history.iteration, root.results.history.cl),
                        root.series("Cs", Theme.chart3, root.results.history.iteration, root.results.history.cs)
                    ] : []
                }
                LineChart {
                    objectName: "cfdChartForces"
                    Layout.fillWidth: true
                    title: root.results && root.results.provisional ? qsTr("Forces (N, provisional)") : qsTr("Forces (N)")
                    series: root.results ? [
                        root.series(qsTr("drag"), Theme.chart1, root.results.history.forceIteration, root.results.history.dragN),
                        root.series(qsTr("lift"), Theme.chart2, root.results.history.forceIteration, root.results.history.liftN),
                        root.series(qsTr("side"), Theme.chart3, root.results.history.forceIteration, root.results.history.sideN)
                    ] : []
                }
                LineChart {
                    objectName: "cfdChartResiduals"
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
                        readonly property var stack: root.sectionStack(modelData.axis)
                        readonly property var position: root.sectionPosition(modelData)
                        Layout.fillWidth: true
                        spacing: 2
                        CheckBox {
                            text: fieldRow.modelData.label
                            checked: fieldRow.modelData.on
                            enabled: !!(root.results && (root.results.fields[fieldRow.modelData.kind] || fieldRow.stack))
                            visible: fieldRow.modelData.kind !== "sectionCross" || !!fieldRow.stack
                            onToggled: root.setField(fieldRow.index, checked)
                        }
                        Label {
                            objectName: fieldRow.modelData.kind + "Missing"
                            visible: (fieldRow.modelData.kind === "wake" || fieldRow.modelData.kind === "vortices")
                                     && !!root.results && !root.results.fields[fieldRow.modelData.kind]
                            Layout.leftMargin: 28
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                            color: Theme.textMuted
                            text: fieldRow.modelData.kind === "wake"
                                  ? qsTr("This run was made before the wake surface was added: run it again to see the wake.")
                                  : qsTr("This run was made before the vortex cores were added: run it again to see them.")
                        }
                        // Where the section lies (CAD-148): steps through the planes the run wrote.
                        RowLayout {
                            visible: fieldRow.modelData.on && !!fieldRow.stack && fieldRow.stack.positions.length > 1
                            Layout.leftMargin: 28
                            Layout.fillWidth: true
                            Slider {
                                objectName: "sectionSlider_" + fieldRow.modelData.axis
                                Layout.fillWidth: true
                                from: 0
                                to: fieldRow.stack ? fieldRow.stack.positions.length - 1 : 0
                                stepSize: 1
                                snapMode: Slider.SnapAlways
                                value: fieldRow.stack && fieldRow.position ? fieldRow.stack.positions.indexOf(fieldRow.position) : 0
                                onMoved: root.setSectionIndex(fieldRow.modelData.axis, fieldRow.stack.positions[Math.round(value)].index)
                            }
                            Label {
                                objectName: "sectionPosition_" + fieldRow.modelData.axis
                                text: root.sectionPositionText(fieldRow.modelData.axis, fieldRow.position)
                                font.pixelSize: 11
                                color: Theme.textMuted
                            }
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
                                        const map = root.controller.colormap(fieldRow.modelData.scalar, root.paletteFor(fieldRow.modelData.scalar, fieldRow.modelData.kind))
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
                    Label { text: qsTr("Streamline colours"); visible: !!(root.results && root.results.fields.streamlines) }
                    ComboBox {
                        objectName: "streamlineStyleBox"
                        visible: !!(root.results && root.results.fields.streamlines)
                        Layout.columnSpan: 3
                        Layout.fillWidth: true
                        textRole: "text"
                        valueRole: "value"
                        model: [{ text: qsTr("Speed"), value: "speed" }, { text: qsTr("Smoke (grey)"), value: "smoke" }]
                        Component.onCompleted: currentIndex = indexOfValue(root.streamlineStyle)
                        onActivated: root.streamlineStyle = currentValue
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
                        objectName: "cfdAbRows"
                        // Forces, the drag split and the coefficients (B on A's reference area, CAD-171).
                        // The axle lifts (CAD-194) only when both runs are cars with two axles.
                        model: [["dragN", qsTr("Drag N")], ["liftN", qsTr("Lift N")], ["sideN", qsTr("Side N")],
                                ["pressureDragN", qsTr("Pressure drag N")], ["viscousDragN", qsTr("Friction drag N")]]
                               .concat(root.comparison && root.comparison.forces && root.comparison.forces.frontLiftN
                                       ? [["frontLiftN", qsTr("Front axle lift N")], ["rearLiftN", qsTr("Rear axle lift N")]] : [])
                               .concat(root.comparison && root.comparison.balance ? [["balance", qsTr("Balance front %")]] : [])
                               .concat(root.comparison && root.comparison.forces && root.comparison.forces.wheelDragN
                                       ? [["wheelDragN", qsTr("Wheels drag N")], ["wheelLiftN", qsTr("Wheels lift N")]] : [])
                               .concat([["efficiency", qsTr("L/D")], ["cd", "Cd"], ["cl", "Cl"], ["cs", "Cs"]])
                        delegate: Repeater {
                            id: rowRep
                            required property var modelData
                            readonly property var d: root.abRow(modelData[0])
                            model: 5
                            // Coefficients to 4 decimals, so a change of a few drag counts shows.
                            readonly property int digits: modelData[0].endsWith("N") ? 3
                                                          : modelData[0] === "efficiency" ? 2 : modelData[0] === "balance" ? 1 : 4
                            delegate: Label {
                                required property int index
                                font.bold: index === 3
                                color: index === 4 && rowRep.d && rowRep.d.withinNoise ? Theme.textMuted : Theme.text
                                text: index === 0 ? rowRep.modelData[1]
                                    : !rowRep.d ? "–"
                                    : index === 1 ? root.num(rowRep.d.a, rowRep.digits)
                                    : index === 2 ? root.num(rowRep.d.b, rowRep.digits)
                                    : index === 3 ? (rowRep.d.difference >= 0 ? "+" : "") + root.num(rowRep.d.difference, rowRep.digits)
                                                    + (rowRep.modelData[0] === "balance" ? " pp" : "")
                                    : rowRep.d.withinNoise ? qsTr("noise")
                                    : rowRep.d.percent === null ? "–"
                                    : Math.abs(rowRep.d.percent) < 0.05 ? "0.0" // not "-0.0"
                                    : (rowRep.d.percent >= 0 ? "+" : "") + root.num(rowRep.d.percent, 1)
                            }
                        }
                    }
                    Label {
                        Layout.columnSpan: 5
                        visible: !!root.comparison && !!root.comparison.coefficientsOnReferenceAreaOfA
                                 && Object.keys(root.comparison.coefficientsOnReferenceAreaOfA).length > 0
                        text: qsTr("Coefficients on the reference area of A")
                        font.pixelSize: 11
                        opacity: 0.7
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "cfdSwapAB"
                        text: qsTr("Swap A ↔ B")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Make B the baseline and A the variant")
                        onClicked: root.swapAB()
                    }
                    Button {
                        objectName: "cfdExportAB"
                        text: qsTr("Export CSV…")
                        onClicked: root.openComparisonExport()
                    }
                    Label {
                        objectName: "cfdExportABNote"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        font.pixelSize: 11
                        opacity: 0.7
                        text: root.abExportNote
                    }
                }
                Repeater {
                    // CAD-159: each differing condition in words with both values.
                    model: !root.comparison ? []
                           : (root.comparison.conditionChanges || root.comparison.differingConditions)
                               .map(c => qsTr("Different: %1").arg(c))
                               .concat((root.comparison.unknownConditions || []).map(c => qsTr("Unknown condition: %1").arg(c)))
                               .concat(root.comparison.reasons).concat(root.comparison.notes)
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
                // How A and B converged, on the same axes (CAD-179): is the difference larger
                // than the oscillation of Cd?
                LineChart {
                    objectName: "cfdChartAB"
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    visible: !!root.comparison
                    title: qsTr("Cd per iteration")
                    series: root.comparison && root.results && root.resultsB ? [
                        root.series(qsTr("Cd A"), Theme.chart1, root.results.history.iteration, root.results.history.cd),
                        root.series(qsTr("Cd B"), Theme.chart2, root.resultsB.history.iteration, root.cdOfBOnAreaOfA())
                    ] : []
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
