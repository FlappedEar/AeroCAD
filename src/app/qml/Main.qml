// SPDX-License-Identifier: GPL-3.0-or-later
// Main window, laid out like common CAD applications (Autodesk Fusion in particular):
// toolbar on top, Browser on the left, canvas with ViewCube (top right) and navigation bar
// (bottom centre), properties on the right, status bar at the bottom.
// Keyboard shortcuts follow Fusion's defaults, https://www.autodesk.com/shortcuts/fusion-360
// (Qt maps Ctrl to Command and Alt to Option on macOS).
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import AeroCAD

ApplicationWindow {
    id: window

    required property AppController controller
    required property ScanController scan
    property var selectedPartId: 0
    /// Every selected part (Fusion: Ctrl / Shift + click adds or takes out one, Ctrl+A takes
    /// all). `selectedPartId` is the last one picked: tools for one part work on it, Delete,
    /// Hide (V) and Export on all of them.
    property var selectedPartIds: []
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"

    property bool browserVisible: true
    property bool propertiesVisible: true
    /// Wind tunnel panel (replaces Properties while open).
    property bool simulationVisible: false

    /// Sketch being edited ({id, origin, normal, xDir, yDir}) or null.
    property var activeSketch: null
    property string sketchTool: ""
    /// True while Create Sketch waits for a plane or a face.
    property bool pickingSketchPlane: false
    /// Sketch drawn over a surface or scan, just finished: offers "Part on surface".
    property var partOnSurfaceSketch: null

    width: 1400
    height: 860
    visible: true

    // One palette for every control, built from the Theme tokens (Preferences > Theme:
    // follow the system, light or dark). The Basic style draws everything from the palette,
    // so menus, dialogs, fields and panels all switch together. Popups inherit it.
    // Groups are set one by one, because palette.<role> would also overwrite the disabled
    // colours.
    function applyTheme() {
        const c = {
            window: Theme.surface, windowText: Theme.text, base: Theme.field,
            alternateBase: Theme.surfaceRaised, toolTipBase: Theme.tooltip, toolTipText: Theme.tooltipText,
            placeholderText: Theme.textFaint, text: Theme.text, button: Theme.surfaceRaised,
            buttonText: Theme.text, brightText: Theme.accentText, light: Theme.surfaceRaised,
            midlight: Theme.border, mid: Theme.borderStrong, dark: Theme.borderStrong, shadow: Theme.shadow,
            highlight: Theme.accent, highlightedText: Theme.accentText, link: Theme.accent
        }
        const dimmed = ["windowText", "text", "buttonText"]
        for (const role in c) {
            palette.active[role] = c[role]
            palette.inactive[role] = c[role]
            palette.disabled[role] = dimmed.includes(role) ? Theme.textFaint : c[role]
        }
    }

    font.pixelSize: Theme.fontBody
    color: Theme.chrome
    Component.onCompleted: applyTheme()
    Connections {
        target: Theme
        function onDarkChanged() { window.applyTheme() }
    }

    title: (controller.projectDirty ? "*" : "") + controller.projectName
           + (controller.variants.length > 1 ? " [" + controller.activeVariantName + "]" : "")
           + " — " + qsTr("AeroCAD %1").arg(controller.version)

    /// Set once the user agreed to close (saved or discarded changes).
    property bool closeConfirmed: false
    /// Runs after the unsaved-changes question was answered with Save (and saving worked)
    /// or Discard.
    property var afterUnsavedCheck: null

    function selectPart(id, additive) {
        // Picking another part (or empty space) applies a running Move / Rotate / Scale.
        if (viewport.transformActive && id !== viewport.transformTarget.partId)
            viewport.finishTransform(true)
        if (additive && !id)
            return // Ctrl / Shift + click on empty space keeps the selection.
        if (additive && selectedPartIds.indexOf(id) >= 0) {
            const rest = selectedPartIds.filter(p => p !== id)
            selectedPartIds = rest
            selectedPartId = rest.length ? rest[rest.length - 1] : 0
            return
        }
        selectedPartIds = !id ? [] : additive ? selectedPartIds.concat([id]) : [id]
        selectedPartId = id
        if (additive)
            controller.showStatus(qsTr("%1 parts selected: Delete, Hide (V) and Export act on all").arg(selectedPartIds.length))
    }

    /// A right-click on a part keeps a selection it is part of (the menu then acts on all).
    function selectPartForMenu(id) {
        if (!id || selectedPartIds.indexOf(id) < 0)
            selectPart(id)
        else if (selectedPartId !== id)
            selectedPartId = id
    }

    /// Ctrl+A (Fusion: Select All): every visible part.
    function selectAllParts() {
        const r = controller.execute("document.parts")
        const ids = r.ok ? r.result.parts.filter(p => p.visible).map(p => p.id) : []
        if (viewport.transformActive)
            viewport.finishTransform(true)
        selectedPartIds = ids
        selectedPartId = ids.length ? ids[ids.length - 1] : 0
        if (ids.length > 1)
            controller.showStatus(qsTr("%1 parts selected").arg(ids.length))
    }

    /// Selected parts that still exist, the last picked one last.
    function selectedExisting() {
        return selectedPartIds.filter(id => controller.partInfo(id).id)
    }

    /// Runs `step(id)` for every selected part and joins what they did into one undo step.
    /// The steps are joined every 50 parts on the way, so a large selection never pushes
    /// its first steps out of the 200-step history before they are joined.
    function forEachSelected(label, step, items) {
        let tokens = []
        let done = 0
        const join = () => {
            if (tokens.length > 1 && controller.mergeHistoryIds(tokens, label.arg(done)))
                tokens = [controller.historyTopId()]
        }
        for (const id of (items ?? selectedExisting())) {
            const before = controller.historyTopId()
            step(id)
            const after = controller.historyTopId()
            if (after && after !== before) {
                tokens.push(after)
                ++done
            }
            if (tokens.length >= 50)
                join()
        }
        join()
    }

    /// A tool that picks in the view or edits the design: opening one applies a running
    /// Move / Rotate / Scale first (Measure and selection tools would see the old geometry).
    readonly property bool otherToolActive: measurePanel.active || heatMapPanel.active || scanPanel.visible
                                            || featureDialog.visible || !!activeSketch || pickingSketchPlane
                                            || viewport.tool !== "navigate"
    onOtherToolActiveChanged: if (otherToolActive) viewport.finishTransform(true)

    /// Move / Rotate / Scale (Fusion: Move/Copy, M): handles in the view and a small panel,
    /// live preview, one undo step when done.
    function startTransform(id) {
        if (activeSketch)
            return null
        // Everything that may still change the geometry ends first (a running transform, a
        // feature dialog's preview), so the centre read below is the one on screen.
        const partId = id || selectedPartId
        viewport.finishTransform(true)
        closeToolPanels()
        featureDialog.close()
        pickingSketchPlane = false
        viewport.leaveSelectionTool()
        const info = controller.partInfo(partId)
        if (!info.id)
            return null
        if (selectedPartId !== info.id)
            selectPart(info.id)
        if (!viewport.startTransform(info)) {
            controller.showStatus(qsTr("Sketches and planes move with the body they belong to."))
            return null
        }
        viewport.transformStatus(qsTr("Drag an arrow to move, a ring to rotate, the square to scale (Shift snaps). Enter applies, Esc cancels."))
        return null
    }

    // --- Guided actions (ContextActions.qml) --------------------------------------------

    function openImport() { importDialog.open() }

    /// Region selection tool: "region", "lasso", "brush" or "navigate".
    function setSelectionTool(tool) { viewport.tool = tool }

    /// Selects the body a timeline feature made (e.g. after Convert to body).
    function selectBodyOfFeature(featureId) {
        const r = controller.execute("document.parts")
        const part = r.ok ? r.result.parts.find(p => p.kind === "body" && p.featureId === featureId) : null
        if (part)
            selectPart(part.id)
    }

    ContextActions {
        id: contextActions
        controller: window.controller
        host: window
    }

    ActionPopup {
        id: actionPopup
        registry: contextActions
    }

    // Right click in a sketch (Fusion's sketch marking menu): end the curve, the drawing
    // tools, Look At, Undo, Finish Sketch. Right drag still orbits.
    Menu {
        id: sketchMenu
        objectName: "sketchMenu"
        property bool drawing: false
        component SketchMenuItem: MenuItem {
            height: visible ? implicitHeight : 0
        }
        SketchMenuItem {
            objectName: "endCurve"
            text: qsTr("End Curve (Esc)")
            visible: sketchMenu.drawing
            font.bold: true
            onTriggered: viewport.endSketchCurve()
        }
        MenuSeparator { visible: sketchMenu.drawing; height: visible ? implicitHeight : 0 }
        SketchMenuItem {
            objectName: "deleteCurves"
            text: qsTr("Delete (Del)")
            icon.source: Theme.iconUrl("delete")
            visible: !sketchMenu.drawing && viewport.sketchOverlay.selectedCurves.length > 0
            font.bold: true
            onTriggered: viewport.sketchOverlay.deleteSelection()
        }
        SketchMenuItem {
            objectName: "constructionCurves"
            text: qsTr("Construction (X)")
            icon.source: Theme.iconUrl("construction")
            visible: !sketchMenu.drawing && viewport.sketchOverlay.selectedCurves.length > 0
            onTriggered: viewport.sketchOverlay.toggleConstruction()
        }
        MenuSeparator {
            visible: !sketchMenu.drawing && viewport.sketchOverlay.selectedCurves.length > 0
            height: visible ? implicitHeight : 0
        }
        SketchMenuItem { action: lineAction; icon.source: Theme.iconUrl("line") }
        SketchMenuItem { action: rectangleAction; icon.source: Theme.iconUrl("rectangle") }
        SketchMenuItem { action: circleAction; icon.source: Theme.iconUrl("circle") }
        SketchMenuItem { action: arcAction; icon.source: Theme.iconUrl("arc") }
        SketchMenuItem { action: polygonAction; icon.source: Theme.iconUrl("polygon") }
        SketchMenuItem { action: slotAction; icon.source: Theme.iconUrl("slot") }
        SketchMenuItem { action: dimensionAction; icon.source: Theme.iconUrl("dimension") }
        MenuSeparator {}
        SketchMenuItem {
            objectName: "lookAt"
            text: qsTr("Look At")
            icon.source: Theme.iconUrl("plane")
            onTriggered: window.lookAtSketch(null)
        }
        SketchMenuItem {
            text: qsTr("Fit All (F6)")
            icon.source: Theme.iconUrl("fit")
            onTriggered: viewport.fitAll()
        }
        SketchMenuItem {
            text: controller.canUndo ? qsTr("Undo %1").arg(controller.undoText) : qsTr("Undo")
            icon.source: Theme.iconUrl("undo")
            enabled: controller.canUndo
            onTriggered: controller.execute("edit.undo")
        }
        MenuSeparator {}
        SketchMenuItem {
            objectName: "finishSketch"
            action: finishSketchAction
            icon.source: Theme.iconUrl("finish-sketch")
            font.bold: !sketchMenu.drawing
        }
    }

    PartContextMenu {
        id: partMenu
        registry: contextActions
        popupHelper: actionPopup
    }

    function toggleSelectedVisibility() {
        const ids = selectedExisting()
        if (!ids.length)
            return
        // Several parts: hide them all if any is shown, otherwise show them all.
        const show = !ids.some(id => controller.partInfo(id).visible)
        forEachSelected(show ? qsTr("Show %1 parts") : qsTr("Hide %1 parts"), id => {
            if (controller.partInfo(id).visible !== show)
                controller.execute("part.set_visible", { id: id, visible: show })
        })
    }

    /// Isolate (Fusion): only part `id` is shown, and Unisolate brings back what it hid.
    /// `isolationRecords` lists the Isolates still in the history, oldest first, each
    /// { id, name, hidden: [ids], step, exitStep }: step / exitStep are the history tokens of
    /// the Isolate and of the Unisolate that ended it. `isolation` ({ id, name, hidden, step }
    /// or null) is the newest record whose Isolate is done (applied, or aged out of the
    /// history) and whose Unisolate is not, so
    /// Undo / Redo bring the Exit controls back with the hidden parts, also across several
    /// Isolates. Hiding or deleting the isolated part keeps it on, so the parts it hid can
    /// still be brought back.
    property var isolationRecords: []
    property var isolation: null
    function isolatePart(id) {
        if (!id)
            return
        if (isolation)
            unisolate() // isolating another part starts from the scene as it was
        const r = controller.execute("part.isolate", { id: id })
        if (!r.ok)
            return
        // Undone Isolates past the last step can never be redone again: forget them, and keep
        // the list short.
        const kept = isolationRecords.filter(rec => controller.historyApplied(rec.step)
                                                    || controller.historyExpired(rec.step)).slice(-19)
        kept.push({ id: id, name: controller.partInfo(id).name, hidden: r.result.hidden,
                    step: controller.historyTopId(), exitStep: 0 })
        isolationRecords = kept
        checkIsolation()
        selectPart(id)
    }
    /// Appearance (A): swatches for part `id`, at the pointer when it is over the view.
    function openAppearance(id) {
        const p = viewHover.hovered ? viewport.mapToItem(Overlay.overlay, viewHover.point.position.x, viewHover.point.position.y)
                                    : viewport.mapToItem(Overlay.overlay, 24, 60)
        return appearancePopup.openFor(id, p.x, p.y)
    }
    function unisolate() {
        const iso = isolation
        if (!iso)
            return
        const r = controller.execute("part.unisolate", { ids: iso.hidden })
        if (r.ok) {
            const exit = controller.historyTopId()
            isolationRecords = isolationRecords.map(rec => rec.step === iso.step
                                                    ? Object.assign({}, rec, { exitStep: exit }) : rec)
        }
        checkIsolation()
    }
    /// An isolation is on while its Isolate is applied, its Unisolate is not, and a part it
    /// hid is still hidden (it ends when they have all been shown again or deleted by hand).
    function checkIsolation() {
        // A step is done while applied, and also once it has aged out of the history (its
        // change is kept for good: an Isolate 200 edits ago still hides the other parts).
        const done = step => controller.historyApplied(step) || controller.historyExpired(step)
        let found = null
        for (let i = isolationRecords.length - 1; i >= 0 && !found; --i) {
            const rec = isolationRecords[i]
            if (!done(rec.step) || done(rec.exitStep))
                continue
            if (rec.hidden.length === 0
                    || rec.hidden.some(h => { const info = controller.partInfo(h); return info.id && !info.visible }))
                found = rec
        }
        if (!found) {
            if (isolation)
                isolation = null
            return
        }
        if (!isolation || isolation.step !== found.step)
            isolation = { id: found.id, name: found.name, hidden: found.hidden, step: found.step }
    }
    Connections {
        target: controller
        function onHistoryChanged() { window.checkIsolation() }
    }

    function deleteSelected() {
        if (activeSketch) {
            viewport.sketchOverlay.deleteSelection()
            return
        }
        // What to delete is read before anything goes: deleting a sketch makes the body
        // extruded from it fail and vanish from the parts, yet its feature stays in the
        // timeline. Bodies, sketches and planes come from the timeline, so their features
        // are deleted, the last in the timeline first (a feature never needs a later one).
        const tl = controller.execute("timeline.list")
        const order = tl.ok ? tl.result.features.map(f => f.id) : []
        const features = [], parts = []
        for (const id of selectedExisting()) {
            const info = controller.partInfo(id)
            if (info.kind === "body" || info.kind === "sketch" || info.kind === "plane") {
                if (features.indexOf(info.featureId) < 0)
                    features.push(info.featureId)
            } else {
                parts.push(id)
            }
        }
        features.sort((a, b) => order.indexOf(b) - order.indexOf(a))
        const items = features.map(f => ({ feature: f })).concat(parts.map(p => ({ part: p })))
        forEachSelected(qsTr("Delete %1 parts"), item => {
            if (item.feature !== undefined)
                controller.execute("feature.delete", { id: item.feature })
            else if (controller.partInfo(item.part).id)
                controller.execute("part.remove", { id: item.part })
        }, items)
    }

    // --- View (right-click menu on empty space) ----------------------------------------

    function fitView() { viewport.fitAll() }
    function homeView() { viewport.goHome() }
    function isOrthographic() { return viewport.orthographic }
    function setOrthographic(ortho) { viewport.orthographic = ortho }

    function hiddenPartCount() {
        const r = controller.execute("document.parts")
        return r.ok ? r.result.parts.filter(p => !p.visible).length : 0
    }

    function showAllParts() {
        const r = controller.execute("document.parts")
        for (const p of (r.ok ? r.result.parts : []))
            if (!p.visible)
                controller.execute("part.set_visible", { id: p.id, visible: true })
    }

    /// Zoom to Selection: fills the view with the selected area of part `id` (default: the
    /// selected part) when it has one, else with the whole part; nothing selected fits all.
    function zoomToSelection(id) {
        const pid = id || selectedPartId
        if (!pid) {
            viewport.fitAll()
            return
        }
        const sel = controller.execute("selection.get", { id: pid })
        const b = sel.ok ? sel.result.bounds : null
        if (b)
            frameOnPart(pid, b.center, b.diagonal / 2)
        else
            zoomToPart(pid)
    }

    /// Parts that Copy takes: bodies and imported parts, not sketches or planes.
    function canCopyPart(id) {
        const kind = controller.partInfo(id).kind
        return kind === "body" || kind === "imported"
    }

    /// Paste: a copy of the copied part, selected and open in Move / Rotate / Scale (Fusion).
    function pastePart() {
        viewport.finishTransform(true)
        const r = controller.execute("part.duplicate", { id: copiedPartId })
        if (!r.ok) {
            controller.showStatus(qsTr("Nothing to paste: %1").arg(r.error))
            copiedPartId = 0
            return
        }
        selectPart(r.result.id)
        startTransform(r.result.id)
        if (viewport.transformActive)
            viewport.transformPasted = true
    }

    /// Grow / shrink / invert the area selected on the selected scan.
    function changeSelection(command, params) {
        const info = selectedInfo()
        if (info.kind !== "imported" || !(info.triangles > 0)) {
            controller.showStatus(qsTr("Select a scan first."))
            return
        }
        if (command === "selection.grow" && !(info.selectedTriangles > 0)) {
            controller.showStatus(qsTr("Select an area of the scan first (1, 2 or 3)."))
            return
        }
        const r = controller.execute(command, Object.assign({ id: info.id }, params))
        if (!r.ok)
            controller.showStatus(r.error)
    }

    /// Renames part `id` in place in the Browser (Fusion: Rename in the Browser).
    function renamePart(id) {
        if (!id)
            return
        window.browserVisible = true
        selectPart(id)
        browser.startRename(id)
    }

    /// Fills the view with part `id`.
    function zoomToPart(id) {
        const info = controller.partInfo(id)
        if (info.center && info.center.length === 3)
            frameOnPart(id, info.center, (info.diagonal ?? 0) / 2)
    }

    /// Frames a sphere of part `id` (model mm) where it is shown: a part being moved is
    /// framed at its preview.
    function frameOnPart(id, center, radius) {
        let c = Qt.vector3d(center[0], center[1], center[2])
        let r = radius
        if (viewport.transformActive && viewport.transformTarget.partId === id) {
            c = viewport.previewPoint(id, c)
            r *= viewport.transformScale
        }
        viewport.frameAround([c.x, c.y, c.z], Math.max(r, 1))
    }

    function selectedInfo() {
        return selectedPartId ? controller.partInfo(selectedPartId) : ({})
    }

    // --- Modelling ----------------------------------------------------------------------

    /// Create Sketch: pick a plane, face or surface. `across` (optional) presets "Fin
    /// across the surface".
    function startCreateSketch(across) {
        viewport.finishTransform(true)
        if (across !== undefined)
            sketchAcross.checked = across
        partOnSurfaceSketch = null
        finishSketch()
        featureDialog.close()
        pickingSketchPlane = true
    }

    /// Fin angle from the Create Sketch pane, degrees.
    function finAngle() {
        let a = 0
        try { a = Number.fromLocaleString(Qt.locale(), sketchAngle.text) } catch (e) { a = Number(sketchAngle.text.replace(",", ".")) }
        return isFinite(a) ? a : 0
    }

    function createSketch(params) {
        pickingSketchPlane = false
        const r = controller.execute("sketch.create", params)
        if (r.ok)
            enterSketch(r.result.id)
    }

    /// Reloads the solved curves and constraints of the active sketch; snap points are
    /// the curve ends and centres.
    function refreshSketchSnaps() {
        if (!activeSketch)
            return
        const r = controller.execute("sketch.curves", { sketch: activeSketch.id })
        if (!r.ok)
            return
        const snaps = []
        for (const c of r.result.curves) {
            if (c.kind === "circle")
                snaps.push(c.points[0])
            else
                snaps.push(c.points[0], c.points[c.points.length - 1])
            if (c.center)
                snaps.push(c.center)
        }
        viewport.sketchSnapPoints = snaps
        viewport.sketchCurves = r.result.curves
        viewport.sketchConstraints = r.result.constraints
        viewport.sketchDegreesOfFreedom = r.result.degreesOfFreedom
    }

    /// Editing an earlier sketch rolls the timeline back to just after it (Fusion): what came
    /// later is hidden and cannot be snapped to or projected. Finishing rolls forward again;
    /// the command layer keeps track of it (sketch.begin_edit / sketch.end_edit).
    /// Only called when it changes something, so opening a sketch at the end does not mark
    /// the project modified.
    function enterSketch(id) {
        const tl = controller.execute("timeline.list")
        if (tl.ok && (tl.result.sketchEdit || tl.result.features.findIndex(f => f.id === id) + 1 < tl.result.marker))
            controller.execute("sketch.begin_edit", { id: id })
        const r = controller.execute("sketch.profiles", { sketch: id, surfaceLine: true })
        if (!r.ok) {
            endSketchEdit()
            return
        }
        const pl = r.result.plane
        const on = r.result.onSurface
        activeSketch = { id: id, origin: pl.origin, normal: pl.normal, xDir: pl.xDir, yDir: pl.yDir,
                         surfacePart: on ? on.part : 0, across: !!(on && on.across),
                         surfaceLine: r.result.surfaceLine || [], onPoint: on && on.point ? on.point : null }
        sketchTool = "line"
        refreshSketchSnaps()
        // Show the sketch while editing it (Fusion hides sketches once they are used).
        const parts = controller.execute("document.parts")
        if (parts.ok) {
            const part = parts.result.parts.find(p => p.kind === "sketch" && p.featureId === id)
            if (part && !part.visible)
                controller.execute("part.set_visible", { id: part.id, visible: true })
        }
        // Show the sketch while editing it, and look at it.
        for (let row = 0; row < controller.parts.count; ++row) {
            }
        lookAtSketch(on && on.point ? on.point : null)
    }

    /// Look At (Fusion): the view turns to face the sketch plane. On a scan it frames the
    /// place the sketch was started (`point`, or the sketch's own), else everything.
    function lookAtSketch(point) {
        if (!activeSketch)
            return
        const n = activeSketch.normal
        viewport.setViewDirection(Qt.vector3d(n[0], n[1], n[2]))
        const at = point || activeSketch.onPoint
        if (at && controller.sceneRadius > 0)
            viewport.frameAround(at, controller.sceneRadius * 0.35)
        else
            viewport.fitAll()
    }

    /// Ends sketch editing. With `offerPart` (the user finished it), a closed outline drawn
    /// over a surface or scan offers to make the part on that surface right away.
    function endSketchEdit() {
        const tl = controller.execute("timeline.list")
        if (tl.ok && tl.result.sketchEdit)
            controller.execute("sketch.end_edit")
    }

    function finishSketch(offerPart) {
        endSketchEdit()
        // Any other way out (new project, variant switch, a feature dialog) drops the offer.
        partOnSurfaceSketch = null
        if (offerPart && activeSketch) {
            const r = controller.execute("sketch.profiles", { sketch: activeSketch.id })
            if (r.ok && r.result.onSurface && r.result.profiles.length > 0)
                partOnSurfaceSketch = { id: activeSketch.id, part: r.result.onSurface.part,
                                        across: !!r.result.onSurface.across }
        }
        activeSketch = null
        sketchTool = ""
        viewport.sketchCurves = []
        viewport.sketchConstraints = []
        viewport.sketchDegreesOfFreedom = -1
    }

    function sketchTool_(tool) {
        if (activeSketch)
            sketchTool = tool
    }

    /// Opens a feature dialog, pre-filled from the selection (Fusion: select, then press E).
    function openFeature(type) {
        closeToolPanels()
        if (featureDialog.visible)
            featureDialog.close() // an open dialog is cancelled, as in Fusion
        const initial = {}
        const info = selectedInfo()
        if (activeSketch) {
            initial.sketch = activeSketch.id
            finishSketch()
        } else if (info.kind === "sketch") {
            initial.sketch = info.featureId
        }
        // An exact solid outside the timeline (a fin, a fitted solid, a STEP import) is made
        // editable first, so its edges can be rounded or bevelled in one step. The dialog
        // owns the conversion: Cancel undoes it, OK makes it one undo step with the feature.
        const convert = info.kind !== "body" && info.kind !== "sketch" && info.hasBrep && (info.solids ?? 0) > 0
                        && ["fillet", "chamfer", "shell", "press_pull", "hole", "combine", "split", "delete_faces", "draft"].includes(type)
        if (convert) {
            const b = featureDialog.convertToBody(info.id)
            if (b) {
                info.kind = "body"
                info.featureId = b.featureId
                selectBodyOfFeature(b.featureId)
            }
        }
        if (type === "combine" && info.kind === "imported" && !((info.solids ?? 0) > 0)) {
            // A surface or the scan selected: it is the tool that cuts a body to its shape.
            initial.tools = [{ part: info.id }]
            initial.operation = "cut"
        }
        if (info.kind === "body") {
            if (type === "combine")
                initial.target = info.featureId
            else if (["fillet", "chamfer", "shell", "press_pull", "hole", "move", "mirror", "rectangular_pattern",
                      "circular_pattern", "split", "delete_faces", "draft"].includes(type))
                initial.body = info.featureId
        }
        pickingSketchPlane = false
        featureDialog.open(type, initial, 0)
    }

    function editFeature(feature) {
        if (feature.type === "sketch") {
            featureDialog.close()
            enterSketch(feature.id)
        } else {
            finishSketch()
            featureDialog.open(feature.type, feature.params, feature.id)
        }
    }

    function resetLayout() {
        browserVisible = true
        propertiesVisible = true
        viewport.viewCubeVisible = true
        viewport.navigationBarVisible = true
        browser.SplitView.preferredWidth = 260
        properties.SplitView.preferredWidth = 280
    }

    // --- Project files ----------------------------------------------------------------------

    /// Runs `fn` now, or after asking to save unsaved changes.
    function withUnsavedCheck(fn) {
        viewport.finishTransform(true) // a move still on screen counts as a change
        if (!controller.projectDirty) {
            fn()
            return
        }
        afterUnsavedCheck = fn
        unsavedDialog.open()
    }

    /// Dropped or imported files. A project file replaces the current project, so it goes
    /// through the unsaved-changes check like File > Open; meshes and CAD files are imported
    /// into the project that is open afterwards (only if opening succeeded).
    function openOrImport(urls) {
        const isProject = url => /\.aerocad$/i.test(url.toString())
        const projects = []
        const files = []
        for (const url of urls)
            (isProject(url) ? projects : files).push(url)
        const importFiles = () => {
            for (const url of files)
                scan.importFile(url)
        }
        if (projects.length === 0) {
            importFiles()
            return
        }
        if (projects.length > 1)
            controller.showStatus(qsTr("Opened only the first of %1 dropped projects").arg(projects.length))
        withUnsavedCheck(() => {
            if (controller.openFile(projects[0]))
                importFiles()
        })
    }

    function saveProject(then) {
        if (controller.saveProject(viewport.cameraState())) {
            if (then)
                then()
            return
        }
        if (!controller.projectPath) {
            saveAsDialog.then = then || null
            saveAsDialog.open()
        }
    }

    /// Variant commands replace the design: leave sketch / feature editing first, their
    /// cached planes and ids belong to the previous variant.
    function switchVariant(command, params) {
        viewport.finishTransform(true) // applies to the variant it was started in
        featureDialog.close()
        finishSketch()
        pickingSketchPlane = false
        controller.execute(command, params)
    }

    /// Called by main() once the window is up (not in smoke tests).
    function offerRecovery() {
        const files = controller.autosaveFiles()
        if (files.length === 0)
            return
        recoveryDialog.file = files[0]
        recoveryDialog.open()
    }

    onClosing: close => {
        if (!closeConfirmed)
            viewport.finishTransform(true) // a move still on screen counts as a change
        if (closeConfirmed || !controller.projectDirty) {
            controller.discardAutosaves(true)
            return
        }
        close.accepted = false
        withUnsavedCheck(() => {
            controller.discardAutosaves(true)
            closeConfirmed = true
            window.close()
        })
    }

    MessageDialog {
        id: unsavedDialog
        title: qsTr("Unsaved changes")
        text: qsTr("Save changes to %1?").arg(window.controller.projectName)
        informativeText: qsTr("Your changes will be lost if you don't save them.")
        buttons: MessageDialog.Save | MessageDialog.Discard | MessageDialog.Cancel
        onButtonClicked: (button, role) => {
            // Discard has the destructive role, which does not close a non-native dialog
            // (Linux): close it explicitly, or it stays on screen over the opened project.
            close()
            const next = window.afterUnsavedCheck
            window.afterUnsavedCheck = null
            if (button === MessageDialog.Save)
                window.saveProject(next)
            else if (button === MessageDialog.Discard && next)
                next()
        }
    }

    MessageDialog {
        id: recoveryDialog
        property string file
        title: qsTr("Recover unsaved changes")
        text: qsTr("AeroCAD did not close properly. Recover the unsaved changes?")
        informativeText: file
        buttons: MessageDialog.Yes | MessageDialog.No
        onButtonClicked: (button, role) => {
            if (button === MessageDialog.Yes) {
                window.controller.recoverAutosave(file)
            } else {
                window.controller.discardAutosaveFile(file)
                // Other projects may have left copies too: offer them one by one.
                Qt.callLater(window.offerRecovery)
            }
        }
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open")
        nameFilters: window.controller.openNameFilters
        onAccepted: window.withUnsavedCheck(() => window.controller.openFile(selectedFile))
    }

    FileDialog {
        id: saveAsDialog
        property var then: null
        title: qsTr("Save As")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "aerocad"
        nameFilters: [qsTr("AeroCAD projects (*.aerocad)")]
        onAccepted: {
            if (window.controller.saveProjectAs(selectedFile, viewport.cameraState()) && then)
                then()
            then = null
        }
        onRejected: then = null
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import 3D file")
        nameFilters: window.controller.importNameFilters
        onAccepted: window.openOrImport([selectedFile])
    }

    FileDialog {
        id: exportDialog
        // Part ids to export; empty = all visible parts.
        property var partIds: []
        title: partIds.length ? qsTr("Export selected part") : qsTr("Export visible parts")
        fileMode: FileDialog.SaveFile
        acceptLabel: qsTr("Export")
        nameFilters: window.controller.exportNameFilters
        defaultSuffix: selectedNameFilter.extensions.length ? selectedNameFilter.extensions[0] : "stl"
        onAccepted: {
            // Check first; write straight away when everything is fine.
            const file = selectedFile
            const ids = partIds
            const name = file.toString().toLowerCase()
            const format = name.endsWith(".3mf") ? "3mf" : (name.endsWith(".step") || name.endsWith(".stp")) ? "step" : "stl"
            const params = { format: format, minWall: Preferences.minWallThickness }
            if (ids.length)
                params.ids = ids
            const c = window.controller.execute("document.export_check", params)
            if (c.ok && !c.result.passed)
                exportCheckDialog.show(c.result, () => window.writeExport(file, ids))
            else
                window.writeExport(file, ids)
        }
    }

    function writeExport(file, ids) {
        const r = window.controller.exportFile(file, ids)
        if (!r.ok) {
            exportMessage.title = qsTr("Export failed")
            exportMessage.text = r.error
            exportMessage.open()
        }
    }

    ExportCheckDialog {
        id: exportCheckDialog
        controller: window.controller
    }

    MessageDialog {
        id: exportMessage
        buttons: MessageDialog.Ok
    }

    // Export for printing: each visible solid to its own STL and STEP in a folder.
    FolderDialog {
        id: printFolderDialog
        title: qsTr("Folder for the print files")
        onAccepted: {
            const url = selectedFolder.toString()
            const folder = decodeURIComponent(url.replace(/^file:\/\/\/(?=[A-Za-z]:)|^file:\/\/(?=\/)/, ""))
            const c = window.controller.execute("document.export_check", { print: true, minWall: Preferences.minWallThickness })
            if (c.ok && !c.result.passed)
                exportCheckDialog.show(c.result, () => window.writePrintFiles(folder))
            else
                window.writePrintFiles(folder)
        }
    }

    function writePrintFiles(folder) {
        const r = window.controller.execute("document.export_for_print", { folder: folder })
        if (!r.ok) {
            exportMessage.title = qsTr("Export failed")
            exportMessage.text = r.error
            exportMessage.open()
            return
        }
        window.controller.showStatus(qsTr("%1 part(s) written to %2 (STL and STEP each)")
                                     .arg(r.result.parts).arg(r.result.folder))
    }

    function openPrintExport() {
        printFolderDialog.open()
    }

    function openExport(ids) {
        exportDialog.partIds = ids
        // Suggest a file name as Fusion does: the part's name, or the project's for all
        // visible parts (CAD-115).
        const parts = window.controller.execute("document.parts")
        const part = ids.length === 1 && parts.ok ? parts.result.parts.find(p => p.id === ids[0]) : null
        const base = (part ? part.name : window.controller.projectName) || qsTr("export")
        const safe = base.replace(/[\\/:*?"<>|]+/g, "_").trim()
        const folder = exportDialog.currentFolder.toString().replace(/\/$/, "")
        exportDialog.selectedFile = folder + "/" + encodeURIComponent(safe) + "." + exportDialog.defaultSuffix
        exportDialog.open()
    }

    // --- Actions (shared by the menu bar and the toolbar) -------------------------------

    Action {
        id: newAction
        text: qsTr("&New Design")
        shortcut: StandardKey.New // Fusion: Ctrl+N / Cmd+N
        onTriggered: window.withUnsavedCheck(() => window.controller.newProject())
    }
    Action {
        id: openAction
        text: qsTr("&Open...")
        // Fusion: Ctrl+O / Cmd+O. Opens projects and, like Fusion's Open, also 3D files.
        shortcut: StandardKey.Open
        onTriggered: openDialog.open()
    }
    Action {
        id: saveAction
        text: qsTr("&Save")
        shortcut: StandardKey.Save // Fusion: Ctrl+S / Cmd+S
        onTriggered: window.saveProject()
    }
    Action {
        id: saveAsAction
        text: qsTr("Save &As...")
        // No shortcut: Fusion uses Ctrl+Shift+S for Recovery Save.
        onTriggered: saveAsDialog.open()
    }
    Action {
        id: recoverySaveAction
        text: qsTr("Recovery Sa&ve")
        shortcut: "Ctrl+Shift+S" // Fusion: Recovery Save, Ctrl+Shift+S / Cmd+Shift+S
        onTriggered: {
            if (!window.controller.autosaveNow())
                window.controller.setStatus(qsTr("Nothing to recover: no unsaved changes"))
        }
    }
    Action {
        id: importAction
        text: qsTr("&Import...")
        onTriggered: importDialog.open()
    }

    // Variants (A/B comparison). Fusion has no shortcuts for these.
    Action {
        id: duplicateVariantAction
        text: qsTr("&Duplicate as New Variant")
        onTriggered: window.switchVariant("variant.create", {})
    }
    Action {
        id: toggleVariantAction
        text: qsTr("&Toggle A/B")
        enabled: window.controller.variants.length > 1
        onTriggered: window.switchVariant("variant.toggle", {})
    }
    Action {
        id: deleteVariantAction
        text: qsTr("Delete &Other Variant...")
        enabled: window.controller.variants.length > 1
        onTriggered: {
            const other = window.controller.variants.find(v => !v.active)
            if (other)
                window.controller.execute("variant.delete", { variant: other.id })
        }
    }
    Action {
        id: exportSelectedAction
        text: qsTr("&Export Selected...")
        enabled: !!window.selectedPartId
        shortcut: "Ctrl+E" // Fusion has no default export shortcut
        onTriggered: window.openExport(window.selectedExisting())
    }
    Action {
        id: exportAllAction
        text: qsTr("Export &All Visible...")
        enabled: window.controller.parts.count > 0
        shortcut: "Ctrl+Shift+E"
        onTriggered: window.openExport([])
    }
    Action {
        id: exportPrintAction
        text: qsTr("Export for &Printing...")
        enabled: window.controller.parts.count > 0
        onTriggered: window.openPrintExport()
    }
    Action {
        id: preferencesAction
        text: qsTr("&Preferences...")
        // Cmd+, on macOS (Qt moves the item into the application menu there).
        shortcut: StandardKey.Preferences
        onTriggered: preferencesDialog.open()
    }
    Action {
        id: shortcutsAction
        text: qsTr("&Keyboard Shortcuts...")
        shortcut: "Ctrl+/"
        onTriggered: shortcutsDialog.open()
    }
    Action {
        id: undoAction
        text: viewport.transformActive ? qsTr("&Undo Move / Rotate / Scale")
              : window.controller.canUndo ? qsTr("&Undo %1").arg(window.controller.undoText) : qsTr("&Undo")
        // A running preview can always be undone, also with an empty history (just opened).
        enabled: viewport.transformActive || window.controller.canUndo
        shortcut: StandardKey.Undo // Fusion: Ctrl+Z / Cmd+Z
        onTriggered: {
            // Undo inside Move / Rotate / Scale drops the preview first (as Fusion's Esc).
            if (viewport.transformActive)
                viewport.cancelTransform()
            else
                window.controller.execute("edit.undo")
        }
    }
    Action {
        id: redoAction
        text: window.controller.canRedo ? qsTr("&Redo %1").arg(window.controller.redoText) : qsTr("&Redo")
        enabled: window.controller.canRedo
        // Fusion: Ctrl+Y on Windows, Cmd+Shift+Z on macOS.
        shortcut: window.isMac ? "Ctrl+Shift+Z" : "Ctrl+Y"
        onTriggered: window.controller.execute("edit.redo")
    }
    Action {
        id: deleteAction
        text: qsTr("&Delete")
        // In a sketch, Delete removes the selected sketch curves, never a part.
        enabled: window.activeSketch ? viewport.sketchOverlay.selectedCurves.length > 0 : !!window.selectedPartId
        shortcut: "Delete" // Fusion: Del
        onTriggered: window.deleteSelected()
    }
    Action {
        id: selectAllAction
        text: qsTr("Select &All")
        // In a sketch the keys stay with the sketch; text fields keep their own Ctrl+A.
        enabled: !window.activeSketch
        shortcut: StandardKey.SelectAll // Fusion: Ctrl+A / Cmd+A
        onTriggered: window.selectAllParts()
    }
    Action {
        id: visibilityAction
        text: qsTr("Toggle &Visibility")
        enabled: !!window.selectedPartId
        shortcut: "V" // Fusion: V
        onTriggered: window.toggleSelectedVisibility()
    }
    Action {
        id: appearanceAction
        text: qsTr("&Appearance...")
        enabled: !!window.selectedPartId && !window.activeSketch
        shortcut: "A" // Fusion: A
        onTriggered: window.openAppearance(window.selectedPartId)
    }
    Action {
        id: isolateAction
        text: qsTr("&Isolate")
        enabled: !!window.selectedPartId
        onTriggered: window.isolatePart(window.selectedPartId)
    }
    Action {
        id: unisolateAction
        text: qsTr("&Unisolate")
        enabled: !!window.isolation
        onTriggered: window.unisolate()
    }
    Action {
        id: renameAction
        text: qsTr("Re&name")
        enabled: !!window.selectedPartId
        shortcut: "F2" // the system rename key; Fusion renames from the Browser
        onTriggered: window.renamePart(window.selectedPartId)
    }
    Action {
        id: moveAction
        text: qsTr("&Move / Rotate / Scale")
        enabled: !!window.selectedPartId && !window.activeSketch
        shortcut: "M" // Fusion: Move/Copy, M
        onTriggered: window.startTransform(window.selectedPartId)
    }
    Action {
        id: fitAction
        text: qsTr("&Fit")
        // Fusion lists F6 for Zoom to Fit (Electronics workspace); F is Fillet in Fusion.
        shortcut: "F6"
        onTriggered: viewport.fitAll()
    }
    Action {
        id: zoomSelectionAction
        text: qsTr("&Zoom to Selection")
        enabled: !!window.selectedPartId
        onTriggered: window.zoomToSelection(0)
    }
    // Copy / Paste a part (Fusion: Ctrl+C / Ctrl+V; the pasted copy opens in Move, as there).
    // Text fields keep Ctrl+C / Ctrl+V for their text.
    property var copiedPartId: 0
    Action {
        id: copyAction
        text: qsTr("&Copy")
        shortcut: StandardKey.Copy
        enabled: !!window.selectedPartId && !window.activeSketch && window.canCopyPart(window.selectedPartId)
        onTriggered: {
            window.copiedPartId = window.selectedPartId
            window.controller.showStatus(qsTr("Copied %1: Ctrl+V pastes it").arg(window.controller.partInfo(window.copiedPartId).name))
        }
    }
    Action {
        id: pasteAction
        text: qsTr("&Paste")
        shortcut: StandardKey.Paste
        enabled: !!window.copiedPartId && !window.activeSketch
        onTriggered: window.pastePart()
    }
    Action {
        id: zoomInAction
        text: qsTr("Zoom &In")
        shortcut: StandardKey.ZoomIn // Fusion (Windows): Ctrl++
        onTriggered: viewport.zoomView(1 / 1.25)
    }
    Action {
        id: zoomOutAction
        text: qsTr("Zoom &Out")
        shortcut: StandardKey.ZoomOut // Fusion (Windows): Ctrl+-
        onTriggered: viewport.zoomView(1.25)
    }
    // On most layouts + shares a key with =: Ctrl+= (no Shift) zooms in too.
    Shortcut {
        sequence: "Ctrl+="
        onActivated: zoomInAction.trigger()
    }
    // Area selection on a scan (Fusion Direct Mesh: Shift+Up / Shift+Down grow / shrink,
    // Alt+N invert).
    Action {
        id: growSelectionAction
        text: qsTr("&Grow Selection")
        shortcut: "Shift+Up"
        enabled: !!window.selectedPartId && !window.activeSketch
        onTriggered: window.changeSelection("selection.grow", { steps: 1 })
    }
    Action {
        id: shrinkSelectionAction
        text: qsTr("S&hrink Selection")
        shortcut: "Shift+Down"
        enabled: !!window.selectedPartId && !window.activeSketch
        onTriggered: window.changeSelection("selection.grow", { steps: -1 })
    }
    Action {
        id: invertSelectionAction
        text: qsTr("&Invert Selection")
        shortcut: "Alt+N"
        enabled: !!window.selectedPartId && !window.activeSketch
        onTriggered: window.changeSelection("selection.invert", {})
    }
    Action {
        id: homeAction
        text: qsTr("&Home")
        onTriggered: viewport.goHome()
    }
    Action {
        id: orthoAction
        text: qsTr("&Orthographic")
        checkable: true
        checked: viewport.orthographic
        onTriggered: {
            viewport.orthographic = checked
            checked = Qt.binding(() => viewport.orthographic)
        }
    }

    Action {
        id: agentAccessAction
        text: qsTr("AI &Agent Access...")
        onTriggered: mcpPanel.open()
    }

    McpPanel {
        id: mcpPanel
        anchors.centerIn: Overlay.overlay
        access: window.controller.mcp
    }

    // Modelling tools. Single-letter shortcuts are Fusion's defaults
    // (https://www.autodesk.com/shortcuts/fusion-360): E Extrude, F Fillet, L Line,
    // R 2-point Rectangle, C Center Diameter Circle.
    Action { id: createSketchAction; text: qsTr("Create &Sketch"); onTriggered: window.startCreateSketch() }
    Action { id: finishSketchAction; text: qsTr("Finish Sketch"); enabled: !!window.activeSketch; onTriggered: window.finishSketch(true) }
    Action { id: extrudeAction; text: qsTr("&Extrude"); shortcut: "E"; onTriggered: window.openFeature("extrude") }
    Action { id: revolveAction; text: qsTr("Re&volve"); onTriggered: window.openFeature("revolve") }
    Action { id: boxAction; text: qsTr("&Box"); onTriggered: window.openFeature("box") }
    Action { id: cylinderAction; text: qsTr("C&ylinder"); onTriggered: window.openFeature("cylinder") }
    Action { id: sphereAction; text: qsTr("Sp&here"); onTriggered: window.openFeature("sphere") }
    Action { id: torusAction; text: qsTr("T&orus"); onTriggered: window.openFeature("torus") }
    // Fusion: Press Pull, Q (moves faces in or out; a curved face is offset).
    Action { id: pressPullAction; text: qsTr("&Press Pull"); shortcut: "Q"; enabled: !window.activeSketch; onTriggered: window.openFeature("press_pull") }
    Action { id: filletAction; text: qsTr("&Fillet"); shortcut: "F"; enabled: !window.activeSketch; onTriggered: window.openFeature("fillet") }
    Action { id: holeAction; text: qsTr("H&ole"); shortcut: "H"; enabled: !window.activeSketch; onTriggered: window.openFeature("hole") }
    Action { id: chamferAction; text: qsTr("C&hamfer"); onTriggered: window.openFeature("chamfer") }
    Action { id: shellAction; text: qsTr("S&hell"); onTriggered: window.openFeature("shell") }
    // Fusion: Modify > Draft (tilt faces by an angle from a fixed plane).
    Action { id: draftAction; text: qsTr("D&raft"); enabled: !window.activeSketch; onTriggered: window.openFeature("draft") }
    // Fusion: Modify > Delete on faces (the gap heals from the neighbours).
    Action { id: deleteFacesAction; text: qsTr("&Delete Face"); enabled: !window.activeSketch; onTriggered: window.openFeature("delete_faces") }
    // Fusion: Modify > Split Body (by a plane: origin plane, construction plane or flat face).
    Action { id: splitAction; text: qsTr("Spli&t Body"); enabled: !window.activeSketch; onTriggered: window.openFeature("split") }
    Action { id: combineAction; text: qsTr("Co&mbine"); onTriggered: window.openFeature("combine") }
    Action { id: planeAction; text: qsTr("Construction &Plane"); enabled: !window.activeSketch; onTriggered: window.openFeature("plane") }
    // Fusion: Construct > Midplane (halfway between two parallel faces or planes).
    Action { id: midplaneAction; text: qsTr("&Midplane"); enabled: !window.activeSketch; onTriggered: window.openFeature("midplane") }
    Action { id: loftAction; text: qsTr("&Loft"); enabled: !window.activeSketch; onTriggered: window.openFeature("loft") }
    Action { id: sweepAction; text: qsTr("S&weep"); enabled: !window.activeSketch; onTriggered: window.openFeature("sweep") }
    Action { id: mirrorAction; text: qsTr("M&irror"); enabled: !window.activeSketch; onTriggered: window.openFeature("mirror") }
    Action { id: rectPatternAction; text: qsTr("&Rectangular Pattern"); enabled: !window.activeSketch; onTriggered: window.openFeature("rectangular_pattern") }
    Action { id: circPatternAction; text: qsTr("C&ircular Pattern"); enabled: !window.activeSketch; onTriggered: window.openFeature("circular_pattern") }
    Action {
        id: convertAction
        text: qsTr("Convert to &Body")
        enabled: !!window.selectedPartId && window.selectedInfo().kind === "imported"
        onTriggered: window.controller.execute("feature.base_body", { part: window.selectedPartId })
    }
    Action {
        id: scanToolsAction
        text: qsTr("Scan &Tools...")
        checkable: true
        checked: scanPanel.visible
        onTriggered: {
            if (scanPanel.visible) {
                scanPanel.close()
            } else {
                featureDialog.close()
                closeToolPanels()
                scanPanel.open()
            }
        }
    }
    Action {
        id: cfdAction
        text: qsTr("&Wind Tunnel (CFD)")
        checkable: true
        checked: window.simulationVisible
        onTriggered: {
            window.simulationVisible = checked
            checked = Qt.binding(() => window.simulationVisible)
        }
    }
    Action { id: parametersAction; text: qsTr("Change &Parameters..."); onTriggered: parametersDialog.open() }
    Action { id: lineAction; text: qsTr("&Line"); shortcut: "L"; enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "line"; onTriggered: window.sketchTool_("line") }
    Action { id: rectangleAction; text: qsTr("&Rectangle"); shortcut: "R"; enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "rectangle"; onTriggered: window.sketchTool_("rectangle") }
    Action { id: circleAction; text: qsTr("&Circle"); shortcut: "C"; enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "circle"; onTriggered: window.sketchTool_("circle") }
    Action { id: arcAction; text: qsTr("&Arc"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "arc"; onTriggered: window.sketchTool_("arc") }
    Action { id: polygonAction; text: qsTr("&Polygon"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "polygon"; onTriggered: window.sketchTool_("polygon") }
    Action { id: dimensionAction; text: qsTr("Sketch &Dimension"); shortcut: "D"; enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "dimension"; onTriggered: window.sketchTool_("dimension") }
    Action { id: horizontalAction; text: qsTr("Horizontal"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "horizontal"; onTriggered: window.sketchTool_("horizontal") }
    Action { id: verticalAction; text: qsTr("Vertical"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "vertical"; onTriggered: window.sketchTool_("vertical") }
    Action { id: coincidentAction; text: qsTr("Coincident"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "coincident"; onTriggered: window.sketchTool_("coincident") }
    Action { id: tangentAction; text: qsTr("Tangent"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "tangent"; onTriggered: window.sketchTool_("tangent") }
    Action { id: equalAction; text: qsTr("Equal"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "equal"; onTriggered: window.sketchTool_("equal") }
    Action { id: parallelAction; text: qsTr("Parallel"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "parallel"; onTriggered: window.sketchTool_("parallel") }
    Action { id: perpendicularAction; text: qsTr("Perpendicular"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "perpendicular"; onTriggered: window.sketchTool_("perpendicular") }
    Action { id: concentricAction; text: qsTr("Concentric"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "concentric"; onTriggered: window.sketchTool_("concentric") }
    Action { id: midpointAction; text: qsTr("Midpoint"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "midpoint"; onTriggered: window.sketchTool_("midpoint") }
    Action { id: collinearAction; text: qsTr("Collinear"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "collinear"; onTriggered: window.sketchTool_("collinear") }
    // Fusion: Construction (X) switches the selected sketch curves.
    Action { id: constructionAction; text: qsTr("Construction"); shortcut: "X"; enabled: !!window.activeSketch && viewport.sketchOverlay.selectedCurves.length > 0; onTriggered: viewport.sketchOverlay.toggleConstruction() }
    Action { id: fixAction; text: qsTr("Fix/Unfix"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "fix"; onTriggered: window.sketchTool_("fix") }
    Action { id: trimAction; text: qsTr("&Trim"); shortcut: "T"; enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "trim"; onTriggered: window.sketchTool_("trim") }
    Action { id: sketchFilletAction; text: qsTr("&Fillet"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "fillet"; onTriggered: window.sketchTool_("fillet") }
    Action { id: sketchProjectAction; text: qsTr("&Project"); shortcut: "P"; enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "project"; onTriggered: window.sketchTool_("project") }
    Action { id: sketchOffsetAction; text: qsTr("&Offset"); shortcut: "O"; enabled: !!window.activeSketch && (viewport.sketchOverlay.selectedCurves.length > 0 || window.sketchTool === "offset"); checkable: true; checked: window.sketchTool === "offset"; onTriggered: { if (window.sketchTool !== "offset") viewport.sketchOverlay.startOffset(); else window.sketchTool_("") } }
    Action { id: sketchMirrorAction; text: qsTr("&Mirror"); enabled: !!window.activeSketch && (viewport.sketchOverlay.selectedCurves.length > 0 || window.sketchTool === "mirror"); checkable: true; checked: window.sketchTool === "mirror"; onTriggered: { if (window.sketchTool !== "mirror") viewport.sketchOverlay.startMirror(); else window.sketchTool_("") } }
    Action { id: slotAction; text: qsTr("Sl&ot"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "slot"; onTriggered: window.sketchTool_("slot") }

    // Scan area selection tools (shortcuts 1/2/3 are in Viewport.qml).
    Action {
        id: regionToolAction
        text: qsTr("Select &Area")
        enabled: viewport.selectionToolsAvailable
        checkable: true
        checked: viewport.tool === "region"
        onTriggered: {
            viewport.tool = viewport.tool === "region" ? "navigate" : "region"
            checked = Qt.binding(() => viewport.tool === "region")
        }
    }
    Action {
        id: lassoToolAction
        text: qsTr("&Lasso Selection")
        enabled: viewport.selectionToolsAvailable
        checkable: true
        checked: viewport.tool === "lasso"
        onTriggered: {
            viewport.tool = viewport.tool === "lasso" ? "navigate" : "lasso"
            checked = Qt.binding(() => viewport.tool === "lasso")
        }
    }
    Action {
        id: brushToolAction
        text: qsTr("&Paint Selection")
        enabled: viewport.selectionToolsAvailable
        checkable: true
        checked: viewport.tool === "brush"
        onTriggered: {
            viewport.tool = viewport.tool === "brush" ? "navigate" : "brush"
            checked = Qt.binding(() => viewport.tool === "brush")
        }
    }

    /// New or opened project: nothing of the previous one stays on screen (tools, picks,
    /// sections, measurements, heat maps, scan picks, simulation results, dialogs holding
    /// ids). The command layer clears the document, undo history and analyses itself.
    function resetWorkspace() {
        viewport.finishTransform(false) // the preview belongs to the previous document
        featureDialog.close() // may hold a feature id of the previous project
        parametersDialog.close()
        exportCheckDialog.close()
        viewport.leaveSelectionTool()
        window.pickingSketchPlane = false
        window.partOnSurfaceSketch = null
        window.finishSketch()
        window.selectPart(0)
        window.copiedPartId = 0 // part ids restart in the next project: never paste another part
        window.isolationRecords = [] // the hidden parts belong to the previous project
        window.isolation = null
        closeToolPanels()
        scanPanel.reset()
        heatMapPanel.result = null
        sectionPanel.close()
        simulationPanel.clearSelection()
    }

    // Inspect (Fusion: I = Measure).
    /// Closes the panels that share the top-left corner of the view (Measure, heat maps, Scan Tools).
    function closeToolPanels() {
        viewport.finishTransform(true)
        measurePanel.close()
        heatMapPanel.close()
        scanPanel.close()
    }
    Action {
        id: measureAction
        text: qsTr("&Measure")
        shortcut: "I"
        onTriggered: {
            featureDialog.close()
            heatMapPanel.close()
            scanPanel.close()
            measurePanel.open("auto")
        }
    }
    Action {
        id: clearanceAction
        text: qsTr("&Interference / Clearance")
        onTriggered: {
            featureDialog.close()
            heatMapPanel.close()
            scanPanel.close()
            measurePanel.open("body")
        }
    }
    Action {
        id: sectionAction
        text: qsTr("&Section Analysis")
        checkable: true
        checked: sectionPanel.active
        onTriggered: {
            if (sectionPanel.active)
                sectionPanel.close()
            else
                sectionPanel.open()
            checked = Qt.binding(() => sectionPanel.active)
        }
    }
    Action {
        id: deviationAction
        text: qsTr("&Deviation Map (Scan vs Part)")
        onTriggered: {
            measurePanel.close()
            scanPanel.close()
            heatMapPanel.open("deviation", window.selectedPartId)
        }
    }
    Action {
        id: thicknessAction
        text: qsTr("&Thickness Map")
        onTriggered: {
            measurePanel.close()
            scanPanel.close()
            heatMapPanel.open("thickness", window.selectedPartId)
        }
    }
    Action {
        id: clearAnalysesAction
        text: qsTr("&Clear Analyses")
        onTriggered: {
            sectionPanel.close()
            window.controller.execute("inspect.clear")
        }
    }

    Shortcut {
        sequence: "Esc"
        // A name being edited in the Browser takes its own Esc (keep the old name).
        enabled: !browser.renamingId
        onActivated: {
            if (viewport.transformPickMode) {
                viewport.transformPickMode = ""
                return
            }
            if (viewport.transformActive) {
                viewport.cancelTransform()
                return
            }
            if (viewport.leaveSelectionTool())
                return
            if (measurePanel.active)
                measurePanel.close()
            else if (heatMapPanel.active)
                heatMapPanel.close()
            else if (featureDialog.visible)
                featureDialog.close()
            else if (scanPanel.visible)
                scanPanel.close()
            else if (window.pickingSketchPlane)
                window.pickingSketchPlane = false
            // In a sketch, as in Fusion: Esc ends what is being drawn, then the tool, then
            // clears the curve selection; only then does it leave the sketch.
            else if (window.activeSketch && viewport.sketchOverlay.pointDragging)
                viewport.sketchOverlay.cancelPointDrag() // the drag is dropped, the point goes back
            else if (window.activeSketch && viewport.sketchOverlay.drawing)
                viewport.sketchOverlay.endCurrent()
            else if (window.activeSketch && window.sketchTool !== "")
                window.sketchTool = ""
            else if (window.activeSketch && viewport.sketchOverlay.selectedCurves.length > 0)
                viewport.sketchOverlay.clearSelection()
            else if (window.activeSketch)
                window.finishSketch(true)
            else if (window.partOnSurfaceSketch)
                window.partOnSurfaceSketch = null
        }
    }

    // Enter applies Move / Rotate / Scale; in one of its fields Enter only takes the value.
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: viewport.transformActive && !(window.activeFocusItem instanceof TextInput)
        onActivated: viewport.finishTransform(true)
    }

    // macOS keyboards label Backspace "delete".
    Shortcut {
        enabled: window.isMac && deleteAction.enabled
        sequence: "Backspace"
        onActivated: window.deleteSelected()
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            MenuItem { action: newAction }
            MenuItem { action: openAction }
            Menu {
                id: recentMenu
                title: qsTr("Open &Recent")
                enabled: window.controller.recentProjects.length > 0
                Instantiator {
                    model: window.controller.recentProjects
                    delegate: MenuItem {
                        required property string modelData
                        text: modelData
                        onTriggered: window.withUnsavedCheck(() => window.controller.openPath(modelData))
                    }
                    onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => recentMenu.removeItem(object)
                }
                MenuSeparator {}
                Action { text: qsTr("Clear List"); onTriggered: window.controller.clearRecentProjects() }
            }
            MenuItem { action: saveAction }
            MenuItem { action: saveAsAction }
            MenuItem { action: recoverySaveAction }
            MenuSeparator {}
            MenuItem { action: importAction }
            MenuItem { action: exportSelectedAction }
            MenuItem { action: exportAllAction }
            MenuItem { action: exportPrintAction }
            MenuItem { action: convertAction }
            MenuSeparator {}
            Action {
                text: qsTr("E&xit")
                shortcut: StandardKey.Quit
                onTriggered: Qt.quit()
            }
        }
        Menu {
            title: qsTr("&Edit")
            MenuItem { action: undoAction }
            MenuItem { action: redoAction }
            MenuSeparator {}
            MenuItem { action: copyAction }
            MenuItem { action: pasteAction }
            MenuSeparator {}
            MenuItem { action: visibilityAction }
            MenuItem { action: renameAction }
            MenuItem { action: appearanceAction }
            MenuItem { action: deleteAction }
            MenuSeparator {}
            MenuItem { action: growSelectionAction }
            MenuItem { action: shrinkSelectionAction }
            MenuItem { action: invertSelectionAction }
            MenuSeparator {}
            MenuItem { action: agentAccessAction }
            MenuItem { action: preferencesAction }
        }
        Menu {
            title: qsTr("&Create")
            MenuItem { action: createSketchAction }
            MenuItem { action: finishSketchAction }
            MenuSeparator {}
            MenuItem { action: extrudeAction }
            MenuItem { action: revolveAction }
            MenuItem { action: sweepAction }
            MenuItem { action: loftAction }
            MenuItem { action: boxAction }
            MenuItem { action: cylinderAction }
            MenuItem { action: sphereAction }
            MenuItem { action: torusAction }
            MenuItem { action: holeAction }
            MenuSeparator {}
            MenuItem { action: mirrorAction }
            Menu {
                title: qsTr("&Pattern")
                MenuItem { action: rectPatternAction }
                MenuItem { action: circPatternAction }
            }
        }
        Menu {
            title: qsTr("C&onstruct")
            MenuItem { action: planeAction }
            MenuItem { action: midplaneAction }
        }
        Menu {
            title: qsTr("&Modify")
            MenuItem { action: pressPullAction }
            MenuItem { action: filletAction }
            MenuItem { action: chamferAction }
            MenuItem { action: shellAction }
            MenuItem { action: combineAction }
            MenuItem { action: splitAction }
            MenuItem { action: deleteFacesAction }
            MenuItem { action: draftAction }
            MenuSeparator {}
            MenuItem { action: parametersAction }
            MenuSeparator {}
            MenuItem { action: duplicateVariantAction }
            MenuItem { action: toggleVariantAction }
            MenuItem { action: deleteVariantAction }
            MenuSeparator {}
            MenuItem { action: scanToolsAction }
        }
        Menu {
            title: qsTr("&Sketch")
            MenuItem { action: lineAction }
            MenuItem { action: rectangleAction }
            MenuItem { action: circleAction }
            MenuItem { action: arcAction }
            MenuItem { action: polygonAction }
            MenuItem { action: slotAction }
            MenuSeparator {}
            MenuItem { action: sketchFilletAction }
            MenuItem { action: trimAction }
            MenuItem { action: sketchOffsetAction }
            MenuItem { action: sketchProjectAction }
            MenuItem { action: sketchMirrorAction }
            MenuItem { action: constructionAction }
            MenuSeparator {}
            MenuItem { action: dimensionAction }
            Menu {
                title: qsTr("&Constraints")
                MenuItem { action: horizontalAction }
                MenuItem { action: verticalAction }
                MenuItem { action: coincidentAction }
                MenuItem { action: tangentAction }
                MenuItem { action: equalAction }
                MenuItem { action: parallelAction }
                MenuItem { action: perpendicularAction }
                MenuItem { action: concentricAction }
                MenuItem { action: midpointAction }
                MenuItem { action: collinearAction }
                MenuItem { action: fixAction }
            }
            MenuSeparator {}
            MenuItem { action: finishSketchAction }
        }
        Menu {
            title: qsTr("&Inspect")
            MenuItem { action: measureAction }
            MenuItem { action: clearanceAction }
            MenuSeparator {}
            MenuItem { action: sectionAction }
            MenuItem { action: deviationAction }
            MenuItem { action: thicknessAction }
            MenuSeparator {}
            MenuItem { action: clearAnalysesAction }
        }
        Menu {
            title: qsTr("S&imulate")
            MenuItem { action: cfdAction }
        }
        Menu {
            title: qsTr("&View")
            MenuItem { action: fitAction }
            MenuItem { action: zoomSelectionAction }
            MenuItem { action: zoomInAction }
            MenuItem { action: zoomOutAction }
            MenuItem { action: homeAction }
            MenuSeparator {}
            MenuItem { action: isolateAction }
            MenuItem { action: unisolateAction }
            MenuSeparator {}
            Action { text: qsTr("Front"); onTriggered: viewport.setView(0, 0) }
            Action { text: qsTr("Back"); onTriggered: viewport.setView(0, 180) }
            Action { text: qsTr("Left"); onTriggered: viewport.setView(0, -90) }
            Action { text: qsTr("Right"); onTriggered: viewport.setView(0, 90) }
            Action { text: qsTr("Top"); onTriggered: viewport.setView(-90, 0) }
            Action { text: qsTr("Bottom"); onTriggered: viewport.setView(90, 0) }
            MenuSeparator {}
            MenuItem { action: orthoAction }
            MenuSeparator {}
            Action {
                text: qsTr("Show ViewCube")
                checkable: true
                checked: viewport.viewCubeVisible
                shortcut: "Ctrl+Alt+V" // Fusion: Ctrl+Alt+V / Option+Cmd+V
                onTriggered: {
                    viewport.viewCubeVisible = checked
                    checked = Qt.binding(() => viewport.viewCubeVisible)
                }
            }
            Action {
                text: qsTr("Show Browser")
                checkable: true
                checked: window.browserVisible
                shortcut: "Ctrl+Alt+B" // Fusion: Ctrl+Alt+B / Option+Cmd+B
                onTriggered: {
                    window.browserVisible = checked
                    checked = Qt.binding(() => window.browserVisible)
                }
            }
            Action {
                text: qsTr("Show Navigation Bar")
                checkable: true
                checked: viewport.navigationBarVisible
                shortcut: "Ctrl+Alt+N" // Fusion: Ctrl+Alt+N / Option+Cmd+N
                onTriggered: {
                    viewport.navigationBarVisible = checked
                    checked = Qt.binding(() => viewport.navigationBarVisible)
                }
            }
            Action {
                text: qsTr("Show Properties")
                checkable: true
                checked: window.propertiesVisible
                onTriggered: {
                    window.propertiesVisible = checked
                    checked = Qt.binding(() => window.propertiesVisible)
                }
            }
            MenuSeparator {}
            Action {
                text: qsTr("Reset to Default Layout")
                shortcut: "Ctrl+Alt+R" // Fusion: Ctrl+Alt+R / Option+Cmd+R
                onTriggered: window.resetLayout()
            }
        }
        Menu {
            title: qsTr("&Help")
            MenuItem { action: shortcutsAction }
        }
    }

    // --- Toolbar: tabs and grouped commands, as in Fusion's toolbar -------------------
    // ToolGroup / ToolCommand are the ribbon's building blocks (RibbonGroup.qml,
    // RibbonButton.qml): a group has a caption with a drop-down of all its commands and
    // belongs to a tab (tab: "solid" by default). See docs/ui.md, "Toolbar".

    component ToolGroup: RibbonGroup {}
    component ToolCommand: RibbonButton {}
    /// A ribbon command for a guided action (ContextActions.qml) on the selected part, the
    /// same one the context bar and the right-click menu offer: enabled when the action is
    /// offered for the selection, and values it needs are asked for in the action popup.
    /// `needs` says what to select when the action is not offered.
    component GuidedCommand: RibbonButton {
        required property string actionId
        property string needs: ""
        readonly property var offered: window.selectionActions.find(a => a.id === actionId)
        // Not offered: only its icon and tip are needed, so no part is passed (the action
        // would otherwise query e.g. a body as a sketch and report errors in the status bar).
        readonly property var act: offered ?? contextActions.action(actionId, ({}))
        iconName: act.icon ?? "tool"
        enabled: !!offered && !!offered.enabled && !window.activeSketch
        help: (!offered ? needs : !offered.enabled ? (offered.reason ?? "") : "")
              + (act.tip ? (!offered || !offered.enabled ? "\n" : "") + act.tip : "")
        onClicked: actionPopup.request(act, window.selectionInfo, this)
    }
    component ToolSeparator: Rectangle {
        property string tab: "solid"
        visible: !parent || parent.currentTab === undefined || parent.currentTab === tab
        Layout.fillHeight: true
        Layout.topMargin: 6
        Layout.bottomMargin: 6
        implicitWidth: 1
        color: Theme.separator
    }

    /// The selected part (controller.partInfo) and what can be done with it
    /// (ContextActions.actions), shared by the context bar and the ribbon's guided commands.
    readonly property var selectionInfo: {
        void contextActions.revision
        return window.selectedPartId ? window.controller.partInfo(window.selectedPartId) : ({})
    }
    readonly property var selectionActions: window.selectedPartId ? contextActions.actions(window.selectionInfo) : []

    /// Ribbon tab the user picked; the SKETCH tab shows while a sketch is edited.
    property string ribbonTab: "solid"
    readonly property string currentRibbonTab: activeSketch ? "sketch" : ribbonTab

    header: Rectangle {
        implicitHeight: tabRow.height + ribbon.implicitHeight + 1
        color: Theme.chrome

        // Tabs and quick-access commands.
        RowLayout {
            id: tabRow
            anchors.left: parent.left
            anchors.right: parent.right
            height: 32
            spacing: 0

            Repeater {
                model: [
                    { tab: "solid", text: qsTr("SOLID") },
                    { tab: "surface", text: qsTr("SURFACE") },
                    { tab: "mesh", text: qsTr("MESH") },
                    { tab: "inspect", text: qsTr("INSPECT") },
                    { tab: "simulate", text: qsTr("SIMULATE") },
                    { tab: "tools", text: qsTr("TOOLS") },
                    { tab: "sketch", text: qsTr("SKETCH") }
                ]
                delegate: RibbonTab {
                    required property var modelData
                    Layout.fillHeight: true
                    Layout.leftMargin: index === 0 ? Theme.space2 : 0
                    required property int index
                    text: modelData.text
                    // The SKETCH tab is contextual: shown only while editing a sketch.
                    visible: modelData.tab !== "sketch" || !!window.activeSketch
                    current: window.currentRibbonTab === modelData.tab
                    onClicked: {
                        // SKETCH is contextual (it follows activeSketch), so it is never
                        // stored: the ribbon returns to the user's tab when the sketch ends.
                        if (modelData.tab === "sketch")
                            return
                        if (window.activeSketch)
                            window.finishSketch()
                        window.ribbonTab = modelData.tab
                    }
                }
            }

            Item { Layout.fillWidth: true }

            IconButton { action: undoAction; iconName: "undo"; tip: undoAction.text.replace("&", "") }
            IconButton { action: redoAction; iconName: "redo"; tip: redoAction.text.replace("&", "") }
            Rectangle { implicitWidth: 1; implicitHeight: 16; color: Theme.separator; Layout.leftMargin: 4; Layout.rightMargin: 4 }
            IconButton {
                iconName: Theme.dark ? "theme-light" : "theme-dark"
                tip: Theme.dark ? qsTr("Switch to light theme") : qsTr("Switch to dark theme")
                onClicked: Preferences.theme = Theme.dark ? "light" : "dark"
            }
            IconButton {
                Layout.rightMargin: Theme.space2
                action: preferencesAction
                iconName: "settings"
                tip: qsTr("Preferences")
            }
        }

        Rectangle {
            id: ribbonBackground
            anchors.top: tabRow.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            color: Theme.surface
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }
        }

        RowLayout {
            id: ribbon
            /// Read by RibbonGroup to show the groups of the current tab only.
            property string currentTab: window.currentRibbonTab
            anchors.top: tabRow.bottom
            anchors.left: parent.left
            anchors.leftMargin: Theme.space2
            height: implicitHeight
            spacing: Theme.space1

            // SOLID tab: parametric modelling in the timeline.
            ToolGroup {
                caption: qsTr("CREATE")
                maxVisible: 6
                ToolCommand { action: createSketchAction; text: qsTr("Sketch"); iconName: "sketch" }
                ToolCommand { action: extrudeAction; text: qsTr("Extrude"); iconName: "extrude" }
                ToolCommand { action: revolveAction; text: qsTr("Revolve"); iconName: "revolve" }
                ToolCommand { action: holeAction; text: qsTr("Hole"); iconName: "hole" }
                ToolCommand { action: boxAction; text: qsTr("Box"); iconName: "box" }
                ToolCommand { action: cylinderAction; text: qsTr("Cylinder"); iconName: "cylinder" }
                ToolCommand { action: sphereAction; text: qsTr("Sphere"); iconName: "sphere" }
                ToolCommand { action: torusAction; text: qsTr("Torus"); iconName: "torus" }
                ToolCommand { action: sweepAction; text: qsTr("Sweep"); iconName: "sweep" }
                ToolCommand { action: loftAction; text: qsTr("Loft"); iconName: "loft" }
                ToolCommand { action: mirrorAction; text: qsTr("Mirror"); iconName: "mirror" }
                ToolCommand { action: rectPatternAction; text: qsTr("Pattern"); iconName: "pattern" }
                ToolCommand { action: circPatternAction; text: qsTr("Circular Pattern"); iconName: "circular-pattern" }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("MODIFY")
                maxVisible: 6
                ToolCommand { action: moveAction; text: qsTr("Move/Rotate"); iconName: "move" }
                ToolCommand { action: pressPullAction; text: qsTr("Press Pull"); iconName: "extrude" }
                ToolCommand { action: filletAction; text: qsTr("Fillet"); iconName: "fillet" }
                ToolCommand { action: chamferAction; text: qsTr("Chamfer"); iconName: "chamfer" }
                ToolCommand { action: shellAction; text: qsTr("Shell"); iconName: "shell" }
                ToolCommand { action: combineAction; text: qsTr("Combine"); iconName: "combine" }
                ToolCommand { action: splitAction; text: qsTr("Split Body"); iconName: "section" }
                ToolCommand { action: parametersAction; text: qsTr("Parameters"); iconName: "parameters" }
                ToolCommand { action: deleteAction; text: qsTr("Delete"); iconName: "delete" }
                ToolCommand { action: visibilityAction; text: qsTr("Show/Hide"); iconName: "visible" }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("CONSTRUCT")
                ToolCommand { action: planeAction; text: qsTr("Plane"); iconName: "plane" }
                ToolCommand { action: midplaneAction; text: qsTr("Midplane"); iconName: "plane" }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("INSERT")
                ToolCommand { action: importAction; text: qsTr("Import"); iconName: "import" }
                GuidedCommand { actionId: "solidToBody"; text: qsTr("Make Editable"); needs: qsTr("Select a solid from a file or a fit first.") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("VARIANT")
                ComboBox {
                    id: variantBox
                    implicitWidth: 90
                    model: window.controller.variants
                    textRole: "name"
                    currentIndex: window.controller.variants.findIndex(v => v.active)
                    onActivated: index => window.switchVariant("variant.activate",
                                                                { variant: window.controller.variants[index].id })
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Active design variant")
                }
                ToolCommand { action: toggleVariantAction; text: qsTr("A/B") }
                ToolCommand { action: duplicateVariantAction; text: qsTr("New") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("EXPORT")
                ToolCommand { action: exportSelectedAction; text: qsTr("Selected"); iconName: "export" }
                ToolCommand { action: exportAllAction; text: qsTr("All Visible"); iconName: "export" }
                ToolCommand { action: exportPrintAction; text: qsTr("For Printing"); iconName: "export" }
            }

            // SURFACE tab: smooth (NURBS) surfaces fitted to a scan, and parts made on them.
            ToolGroup {
                tab: "surface"
                caption: qsTr("CREATE")
                GuidedCommand { actionId: "fitSurface"; text: qsTr("Fit Surface"); needs: qsTr("Select an area of a scan first (MESH tab, SELECT).") }
                GuidedCommand { actionId: "fitWhole"; text: qsTr("Scan to Solid"); needs: qsTr("Select a scan first.") }
                GuidedCommand { actionId: "offset"; text: qsTr("Offset"); needs: qsTr("Select a surface first.") }
            }
            ToolSeparator { tab: "surface" }
            ToolGroup {
                tab: "surface"
                caption: qsTr("SKETCH")
                GuidedCommand { actionId: "sketchOnSurface"; text: qsTr("Outline"); needs: qsTr("Select a surface first.") }
                GuidedCommand { actionId: "sketchFin"; text: qsTr("Fin Sketch"); needs: qsTr("Select a scan or a surface first.") }
            }
            ToolSeparator { tab: "surface" }
            ToolGroup {
                tab: "surface"
                caption: qsTr("MODIFY")
                GuidedCommand { actionId: "trim"; text: qsTr("Cut Out"); needs: qsTr("Select a surface first.") }
                GuidedCommand { actionId: "thicken"; text: qsTr("Thicken"); needs: qsTr("Select a surface first.") }
                GuidedCommand { actionId: "cutWithThis"; text: qsTr("Cut to Shape"); needs: qsTr("Select a surface or a scan first.") }
            }
            ToolSeparator { tab: "surface" }
            ToolGroup {
                tab: "surface"
                caption: qsTr("PART")
                GuidedCommand { actionId: "partOnSurface"; text: qsTr("Part on Surface"); needs: qsTr("Select a sketch drawn on a surface or a scan first.") }
                GuidedCommand { actionId: "finOnSurface"; text: qsTr("Fin"); needs: qsTr("Select a fin sketch (Fin Sketch) first.") }
            }
            ToolSeparator { tab: "surface" }
            ToolGroup {
                tab: "surface"
                caption: qsTr("CHECK")
                GuidedCommand { actionId: "fitDeviation"; text: qsTr("Fit Deviation"); needs: qsTr("Select a surface or solid fitted to a scan first.") }
            }

            // MESH tab: scans (triangle meshes): select, prepare, convert.
            ToolGroup {
                tab: "mesh"
                caption: qsTr("INSERT")
                ToolCommand { action: importAction; text: qsTr("Import"); iconName: "import" }
                ToolCommand { action: scanToolsAction; text: qsTr("Scan Tools"); iconName: "mesh" }
            }
            ToolSeparator { tab: "mesh" }
            ToolGroup {
                tab: "mesh"
                caption: qsTr("SELECT")
                maxVisible: 6
                ToolCommand { action: regionToolAction; text: qsTr("Area"); iconName: "select-click" }
                ToolCommand { action: lassoToolAction; text: qsTr("Lasso"); iconName: "lasso" }
                ToolCommand { action: brushToolAction; text: qsTr("Paint"); iconName: "brush" }
                GuidedCommand { actionId: "growSelection"; text: qsTr("Grow"); needs: qsTr("Select an area of a scan first.") }
                GuidedCommand { actionId: "shrinkSelection"; text: qsTr("Shrink"); needs: qsTr("Select an area of a scan first.") }
                GuidedCommand { actionId: "clearSelection"; text: qsTr("Clear"); needs: qsTr("Nothing is selected.") }
            }
            ToolSeparator { tab: "mesh" }
            ToolGroup {
                tab: "mesh"
                caption: qsTr("PREPARE")
                GuidedCommand { actionId: "repair"; text: qsTr("Repair"); needs: qsTr("Select a scan first.") }
                GuidedCommand { actionId: "simplify"; text: qsTr("Simplify"); needs: qsTr("Select a scan first.") }
                GuidedCommand { actionId: "moveRotate"; text: qsTr("Move/Rotate"); needs: qsTr("Select a part first.") }
                GuidedCommand { actionId: "scale"; text: qsTr("Scale"); needs: qsTr("Select a part first.") }
            }
            ToolSeparator { tab: "mesh" }
            ToolGroup {
                tab: "mesh"
                caption: qsTr("DESIGN ON SCAN")
                GuidedCommand { actionId: "sketchOnScan"; text: qsTr("Sketch on Scan"); needs: qsTr("Select a scan first.") }
                GuidedCommand { actionId: "sketchFin"; text: qsTr("Fin Sketch"); needs: qsTr("Select a scan or a surface first.") }
            }
            ToolSeparator { tab: "mesh" }
            ToolGroup {
                tab: "mesh"
                caption: qsTr("CONVERT")
                GuidedCommand { actionId: "fitSurface"; text: qsTr("Fit Surface"); needs: qsTr("Select an area of a scan first.") }
                GuidedCommand { actionId: "fitWhole"; text: qsTr("Scan to Solid"); needs: qsTr("Select a scan first.") }
                GuidedCommand { actionId: "toBody"; text: qsTr("To Body"); needs: qsTr("Select a scan or mesh first.") }
            }
            ToolSeparator { tab: "mesh" }
            ToolGroup {
                tab: "mesh"
                caption: qsTr("EXPORT")
                ToolCommand { action: exportSelectedAction; text: qsTr("Selected"); iconName: "export" }
            }

            // INSPECT tab.
            ToolGroup {
                tab: "inspect"
                caption: qsTr("MEASURE")
                ToolCommand { action: measureAction; text: qsTr("Measure"); iconName: "measure" }
                ToolCommand { action: clearanceAction; text: qsTr("Clearance"); iconName: "clearance" }
            }
            ToolSeparator { tab: "inspect" }
            ToolGroup {
                tab: "inspect"
                caption: qsTr("ANALYSIS")
                ToolCommand { action: sectionAction; text: qsTr("Section"); iconName: "section" }
                ToolCommand { action: deviationAction; text: qsTr("Deviation"); iconName: "deviation" }
                ToolCommand { action: thicknessAction; text: qsTr("Thickness"); iconName: "thickness" }
                GuidedCommand { actionId: "fitDeviation"; text: qsTr("Fit Deviation"); needs: qsTr("Select a surface or solid fitted to a scan first.") }
                ToolCommand { action: clearAnalysesAction; text: qsTr("Clear"); iconName: "close" }
            }

            // SIMULATE tab.
            ToolGroup {
                tab: "simulate"
                caption: qsTr("AERODYNAMICS")
                ToolCommand { action: cfdAction; text: qsTr("Wind Tunnel"); iconName: "wind-tunnel" }
            }

            // TOOLS tab.
            ToolGroup {
                tab: "tools"
                caption: qsTr("AI")
                ToolCommand {
                    action: agentAccessAction
                    text: window.controller.mcp.clients.length > 0 ? qsTr("Agent ●")
                          : window.controller.mcp.listening ? qsTr("Agent ○") : qsTr("Agent")
                }
            }
            ToolSeparator { tab: "tools" }
            ToolGroup {
                tab: "tools"
                caption: qsTr("UTILITIES")
                ToolCommand { action: parametersAction; text: qsTr("Parameters"); iconName: "parameters" }
                ToolCommand { action: shortcutsAction; text: qsTr("Shortcuts"); iconName: "keyboard" }
                ToolCommand { action: preferencesAction; text: qsTr("Preferences"); iconName: "settings" }
            }

            // SKETCH tab (contextual, while a sketch is edited).
            ToolGroup {
                tab: "sketch"
                caption: qsTr("CREATE")
                ToolCommand { action: lineAction; text: qsTr("Line"); iconName: "line" }
                ToolCommand { action: rectangleAction; text: qsTr("Rectangle"); iconName: "rectangle" }
                ToolCommand { action: circleAction; text: qsTr("Circle"); iconName: "circle" }
                ToolCommand { action: arcAction; text: qsTr("Arc"); iconName: "arc" }
                ToolCommand { action: polygonAction; text: qsTr("Polygon"); iconName: "polygon" }
                ToolCommand { action: slotAction; text: qsTr("Slot"); iconName: "slot" }
                ToolCommand { action: dimensionAction; text: qsTr("Dimension"); iconName: "dimension" }
            }
            ToolSeparator { tab: "sketch" }
            ToolGroup {
                tab: "sketch"
                caption: qsTr("MODIFY")
                ToolCommand { action: sketchFilletAction; text: qsTr("Fillet"); iconName: "fillet" }
                ToolCommand { action: trimAction; text: qsTr("Trim"); iconName: "trim" }
                ToolCommand { action: sketchOffsetAction; text: qsTr("Offset"); iconName: "offset" }
                ToolCommand { action: sketchProjectAction; text: qsTr("Project"); iconName: "project" }
                ToolCommand { action: sketchMirrorAction; text: qsTr("Mirror"); iconName: "mirror" }
                ToolCommand { action: constructionAction; text: qsTr("Construction"); iconName: "construction" }
            }
            ToolSeparator { tab: "sketch" }
            ToolGroup {
                tab: "sketch"
                caption: qsTr("CONSTRAINTS")
                maxVisible: 6
                ToolCommand { action: horizontalAction; text: qsTr("Horizontal"); iconName: "horizontal" }
                ToolCommand { action: verticalAction; text: qsTr("Vertical"); iconName: "vertical" }
                ToolCommand { action: coincidentAction; text: qsTr("Coincident"); iconName: "coincident" }
                ToolCommand { action: tangentAction; text: qsTr("Tangent"); iconName: "tangent" }
                ToolCommand { action: perpendicularAction; text: qsTr("Perpendicular"); iconName: "perpendicular" }
                ToolCommand { action: parallelAction; text: qsTr("Parallel"); iconName: "parallel" }
                ToolCommand { action: equalAction; text: qsTr("Equal"); iconName: "equal" }
                ToolCommand { action: concentricAction; text: qsTr("Concentric"); iconName: "concentric" }
                ToolCommand { action: midpointAction; text: qsTr("Midpoint"); iconName: "midpoint" }
                ToolCommand { action: collinearAction; text: qsTr("Collinear"); iconName: "collinear" }
                ToolCommand { action: fixAction; text: qsTr("Fix/Unfix"); iconName: "fix" }
            }
            ToolSeparator { tab: "sketch" }
            ToolGroup {
                tab: "sketch"
                caption: qsTr("SOLID")
                ToolCommand { action: extrudeAction; text: qsTr("Extrude"); iconName: "extrude" }
                ToolCommand { action: revolveAction; text: qsTr("Revolve"); iconName: "revolve" }
            }
            ToolSeparator { tab: "sketch" }
            ToolGroup {
                tab: "sketch"
                caption: qsTr("FINISH")
                ToolCommand { action: finishSketchAction; text: qsTr("Finish Sketch"); iconName: "finish-sketch"; primary: true }
            }
        }

        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        PartsPanel {
            id: browser
            visible: window.browserVisible
            SplitView.preferredWidth: 260
            SplitView.minimumWidth: 180
            controller: window.controller
            selectedPartId: window.selectedPartId
            selectedPartIds: window.selectedPartIds
            isolation: window.isolation
            onUnisolateRequested: window.unisolate()
            onPartSelected: id => window.selectPart(id, window.controller.addToSelectionHeld())
            onPartContextRequested: (id, item, x, y) => {
                window.selectPartForMenu(id)
                partMenu.info = window.controller.partInfo(id)
                partMenu.popup(item, x, y)
            }
        }

        Viewport {
            id: viewport
            objectName: "viewport"
            HoverHandler { id: viewHover } // where the command search (S) opens
            SplitView.fillWidth: true
            controller: window.controller
            selectedPartId: window.selectedPartId
            selectedPartIds: window.selectedPartIds
            sketch: window.activeSketch
            sketchTool: window.sketchTool
            selectionToolsEnabled: !featureDialog.visible && !window.pickingSketchPlane && !measurePanel.active
                                   && !scanPanel.pickMode
            snapToNearestSurface: window.pickingSketchPlane
            property bool snappedClick: false
            onSnappedToNearestSurface: snappedClick = true
            markers: featureDialog.visible ? featureDialog.pickedPoints
                   : scanPanel.visible ? scanPanel.pickedPoints : measurePanel.markers
            pickKind: featureDialog.visible ? featureDialog.pickKind : ""
            shownSketch: featureDialog.shownSketch
            unpickablePlane: featureDialog.visible ? featureDialog.ownPlane : -1
            unpickableBody: featureDialog.visible ? featureDialog.previewFeatureId : -1
            distanceArrow: featureDialog.visible ? featureDialog.distanceArrow : null
            onDistanceArrowDragged: t => featureDialog.dragDistance(t)
            simulationFields: window.simulationVisible ? simulationPanel.viewportFields : []
            hiddenPartIds: window.simulationVisible ? simulationPanel.hiddenPartIds : []
            overlayLines: measurePanel.keptLines.concat(measurePanel.overlayLines)
            pickSketchLines: measurePanel.active
            onSectionDragged: delta => sectionPanel.moveBy(delta)
            onPartPicked: (id, additive, forMenu) => {
                if (forMenu)
                    window.selectPartForMenu(id)
                else
                    window.selectPart(id, additive)
                // A click through a hole of a scan, or next to the model, while choosing where to sketch.
                if (!id && window.pickingSketchPlane)
                    window.controller.showStatus(qsTr("Nothing there to sketch on: click on or next to the surface"))
            }
            onZoomToPartRequested: id => window.zoomToSelection(id)
            cfdPreview: simulationPanel.visible ? simulationPanel.proposal : null
            onAxisClicked: (x, y) => {
                if (featureDialog.visible)
                    featureDialog.pickAxisAt(viewport, x, y)
            }
            onSketchContextRequested: (x, y) => {
                sketchMenu.drawing = viewport.sketchDrawing
                sketchMenu.popup(viewport, x, y)
            }
            onPartContextRequested: (id, x, y) => {
                partMenu.info = id ? window.controller.partInfo(id) : ({})
                partMenu.popup(viewport, x, y)
            }
            onPointPicked: (id, point) => {
                const info = window.controller.partInfo(id)
                if (window.pickingSketchPlane) {
                    const snapped = viewport.snappedClick
                    viewport.snappedClick = false
                    const at = [point.x, point.y, point.z]
                    if (info.kind === "body")
                        window.createSketch({ face: { body: info.featureId, point: at } })
                    else if (info.kind === "plane")
                        window.createSketch({ plane: { plane: info.featureId } })
                    else if (info.kind === "imported")
                        // A fitted surface or a scan: sketch over its best-fit plane, then
                        // trim the surface with the outline (Properties > Surface).
                        window.createSketch({ surface: sketchAcross.checked
                                                  ? { part: id, point: at, across: true, angle: window.finAngle() }
                                                  : { part: id, point: at } })
                    if (snapped && window.activeSketch)
                        window.controller.showStatus(qsTr("That was a hole in the surface: sketching on the surface next to it"))
                } else if (featureDialog.visible) {
                    featureDialog.addPick(info, point)
                } else if (scanPanel.visible && scanPanel.pickMode) {
                    scanPanel.addPick(id, point)
                } else if (measurePanel.active) {
                    measurePanel.addPick(info, point)
                }
            }
            onCurveDrawn: curve => {
                if (!window.activeSketch)
                    return
                window.controller.execute("sketch.add_curves", { sketch: window.activeSketch.id, curves: [curve] })
                window.refreshSketchSnaps()
            }
            onConstraintAdded: c => {
                if (!window.activeSketch)
                    return
                // A conflicting constraint is rejected; the error shows in the status bar.
                window.controller.execute("sketch.add_constraints", { sketch: window.activeSketch.id, constraints: [c] })
                window.refreshSketchSnaps()
            }
            onConstraintEdited: (index, value) => {
                if (window.activeSketch)
                    window.controller.execute("sketch.edit_constraint", { sketch: window.activeSketch.id, index: index, value: value })
                window.refreshSketchSnaps()
            }
            onSketchToolRequested: tool => window.sketchTool_(tool)
            onConstraintRemoved: index => {
                if (window.activeSketch)
                    window.controller.execute("sketch.edit_constraint", { sketch: window.activeSketch.id, index: index, remove: true })
                window.refreshSketchSnaps()
            }
            // Dragging a sketch point (CAD-123): a live preview while moving, one undo step
            // on release; the solver keeps the constraints and dimensions.
            onSketchPointDragCancelled: {
                window.controller.cancelPreview()
                window.refreshSketchSnaps()
            }
            onSketchPointDragged: (point, to, done) => {
                if (!window.activeSketch)
                    return
                const args = { sketch: window.activeSketch.id, point: point, to: to }
                if (done) {
                    window.controller.cancelPreview()
                    const r = window.controller.execute("sketch.move_point", args)
                    if (!r.ok)
                        window.controller.showStatus(r.error)
                } else {
                    window.controller.preview("sketch.move_point", args)
                }
                window.refreshSketchSnaps()
            }
            onSketchConstructionToggled: (list, on) => {
                if (window.activeSketch)
                    window.controller.execute("sketch.set_construction", { sketch: window.activeSketch.id, curves: list, construction: on })
                window.refreshSketchSnaps()
            }
            onSketchProjectRequested: (body, point) => {
                if (!window.activeSketch)
                    return
                const r = window.controller.execute("sketch.project", { sketch: window.activeSketch.id, body: body, edges: [{ point: point.map(x => Math.round(x * 1e4) / 1e4) }] })
                if (!r.ok)
                    window.controller.showStatus(r.error)
                window.refreshSketchSnaps()
            }
            onSketchOffsetRequested: (curves, distance) => {
                if (!window.activeSketch)
                    return
                const r = window.controller.execute("sketch.offset", { sketch: window.activeSketch.id, curves: curves, distance: distance })
                if (!r.ok)
                    window.controller.showStatus(r.error)
                window.refreshSketchSnaps()
            }
            onSketchFilletRequested: (curves, radius) => {
                if (!window.activeSketch)
                    return
                const r = window.controller.execute("sketch.fillet", { sketch: window.activeSketch.id, curves: curves, radius: radius })
                if (!r.ok)
                    window.controller.showStatus(r.error)
                else
                    window.controller.showStatus(qsTr("Fillet R%1: edit its radius under Constraints").arg(radius))
                window.refreshSketchSnaps()
            }
            onSketchMirrorRequested: (curves, line) => {
                if (!window.activeSketch)
                    return
                const r = window.controller.execute("sketch.mirror", { sketch: window.activeSketch.id, curves: curves, line: line })
                if (!r.ok)
                    window.controller.showStatus(r.error)
                window.refreshSketchSnaps()
            }
            onSketchTrimRequested: (curve, at) => {
                if (!window.activeSketch)
                    return
                const r = window.controller.execute("sketch.trim", { sketch: window.activeSketch.id, curve: curve, at: at })
                if (!r.ok)
                    window.controller.showStatus(r.error)
                window.refreshSketchSnaps()
            }
            onSketchCurvesDeleted: list => {
                if (window.activeSketch)
                    window.controller.execute("sketch.delete_curves", { sketch: window.activeSketch.id, curves: list })
                window.refreshSketchSnaps()
            }

            // What can be done with the selection, next to it (ContextActions.qml).
            ContextBar {
                id: contextBar
                // Above the navigation bar, clear of the selection tools and ViewCube.
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 60
                maximumWidth: parent.width - 2 * Theme.space4
                width: Math.min(implicitWidth, maximumWidth)
                visible: !!window.selectedPartId && !window.activeSketch && !featureDialog.visible
                         && !window.pickingSketchPlane && !cfdOverlay.visible
                registry: contextActions
                popup: actionPopup
                info: window.selectionInfo
            }

            StartCard {
                anchors.centerIn: parent
                visible: window.controller.parts.count === 0 && !window.activeSketch && !featureDialog.visible
                         && !window.pickingSketchPlane
                registry: contextActions
                popup: actionPopup
            }

            FeatureDialog {
                id: featureDialog
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                controller: window.controller
            }

            TransformPanel {
                id: transformPanel
                objectName: "transformPanel"
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                viewport: viewport
            }
            MeasurePanel {
                id: measurePanel
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                controller: window.controller
                pickTolerance: () => viewport.pickTolerance()
            }
            HeatMapPanel {
                id: heatMapPanel
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                controller: window.controller
            }
            SectionPanel {
                id: sectionPanel
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 150 // below the ViewCube
                anchors.rightMargin: 8
                controller: window.controller
            }
            HeatMapLegend {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
                controller: window.controller
            }
            // Wind tunnel results: colour bars and the forces of the run shown in 3D.
            CfdResultsOverlay {
                id: cfdOverlay
                objectName: "cfdOverlay"
                anchors.fill: parent
                controller: window.controller
                fields: viewport.simulationFields
                results: simulationPanel.displayedRun === simulationPanel.activeRun ? simulationPanel.results : null
                runTitle: simulationPanel.displayedRunTitle
                comparison: simulationPanel.differenceShown ? simulationPanel.comparison : null
            }

            ScanPanel {
                id: scanPanel
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                controller: window.controller
                scan: window.scan
                partId: window.selectedPartId
            }

            // Import progress (files are parsed in the background).
            Pane {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 56
                visible: window.scan.busy
                padding: 10
                RowLayout {
                    spacing: 10
                    Label { text: window.scan.progressText }
                    ProgressBar { from: 0; to: 1; value: window.scan.progress; Layout.preferredWidth: 220 }
                    Label { text: Math.round(window.scan.progress * 100) + " %" }
                    Button { text: qsTr("Cancel"); onClicked: window.scan.cancelImport() }
                }
            }

            // Create Sketch: pick a plane or a planar face (Fusion shows the origin planes).
            Pane {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                visible: window.pickingSketchPlane
                padding: Theme.space3
                background: Rectangle {
                    color: Theme.overlay
                    border.color: Theme.border
                    radius: Theme.radius
                }
                ColumnLayout {
                    Label { text: qsTr("Create Sketch"); font.weight: Font.DemiBold; font.pixelSize: Theme.fontTitle }
                    Label {
                        text: qsTr("Click a planar face of a body, a fitted surface or a scan\n(sketch over its best-fit plane), or pick an origin plane:")
                        color: Theme.textMuted
                    }
                    RowLayout {
                        Button { text: qsTr("XY (Top)"); onClicked: window.createSketch({ plane: "XY" }) }
                        Button { text: qsTr("XZ (Front)"); onClicked: window.createSketch({ plane: "XZ" }) }
                        Button { text: qsTr("YZ (Right)"); onClicked: window.createSketch({ plane: "YZ" }) }
                    }
                    // A fin (canard, winglet) stands across the surface instead of lying on it.
                    RowLayout {
                        CheckBox {
                            id: sketchAcross
                            objectName: "sketchAcross"
                            text: qsTr("Fin across the surface")
                        }
                        Label { text: qsTr("Angle"); enabled: sketchAcross.checked }
                        TextField {
                            id: sketchAngle
                            enabled: sketchAcross.checked
                            Layout.preferredWidth: 48
                            text: "0"
                            validator: DoubleValidator { bottom: -90; top: 90; decimals: 1; notation: DoubleValidator.StandardNotation }
                        }
                        Label { text: qsTr("°"); enabled: sketchAcross.checked }
                    }
                    Label {
                        visible: sketchAcross.checked
                        text: qsTr("Click where the fin stands. Draw its side view above the surface line,\nwith the root a little below it; the root is cut to the surface.")
                        color: Theme.textMuted
                    }
                    Button {
                        readonly property var info: window.pickingSketchPlane ? window.selectedInfo() : ({})
                        visible: (info.selectedTriangles ?? 0) > 0
                        text: qsTr("Over selection on %1").arg(info.name ?? "")
                        onClicked: window.createSketch({ surface: sketchAcross.checked
                            ? { part: window.selectedPartId, useSelection: true, across: true, angle: window.finAngle() }
                            : { part: window.selectedPartId, useSelection: true } })
                    }
                    Button { text: qsTr("Cancel"); onClicked: window.pickingSketchPlane = false }
                }
            }

            // After a sketch over a surface or scan: cut the outline out of the surface and
            // thicken it, in one step.
            Pane {
                objectName: "partOnSurfacePane"
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 8
                visible: !!window.partOnSurfaceSketch && !window.activeSketch && !window.pickingSketchPlane
                         && !featureDialog.visible
                padding: Theme.space3
                background: Rectangle {
                    color: Theme.overlay
                    border.color: Theme.border
                    radius: Theme.radius
                }
                ColumnLayout {
                    readonly property bool fin: !!window.partOnSurfaceSketch && window.partOnSurfaceSketch.across
                    Label {
                        text: parent.fin ? qsTr("Fin on surface") : qsTr("Part on surface")
                        font.weight: Font.DemiBold
                        font.pixelSize: Theme.fontTitle
                    }
                    Label {
                        text: parent.fin
                              ? qsTr("Gives the outline a thickness (half each side of the sketch)\nand cuts its root to the surface, so it sits flush on it.\nThe root fillet rounds the joint to the surface.")
                              : qsTr("Cuts the outline out of the surface under it and gives it a thickness.\nThe edges follow the outline; the part follows the surface.")
                        color: Theme.textMuted
                    }
                    RowLayout {
                        Label { text: qsTr("Thickness") }
                        TextField {
                            id: partThickness
                            Layout.preferredWidth: 56
                            text: "2"
                            validator: DoubleValidator { bottom: 0.001; decimals: 3; notation: DoubleValidator.StandardNotation }
                        }
                        Label { text: qsTr("mm") }
                        ComboBox {
                            id: partSide
                            visible: !(window.partOnSurfaceSketch && window.partOnSurfaceSketch.across)
                            Layout.preferredWidth: 120
                            model: [qsTr("Inward"), qsTr("Outward"), qsTr("Both sides")]
                        }
                    }
                    RowLayout {
                        visible: !!window.partOnSurfaceSketch && window.partOnSurfaceSketch.across
                        Label { text: qsTr("Root fillet") }
                        TextField {
                            id: finRootFillet
                            objectName: "finRootFillet"
                            Layout.preferredWidth: 56
                            text: "2"
                            validator: DoubleValidator { bottom: 0; decimals: 3; notation: DoubleValidator.StandardNotation }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Rounds the joint between the fin and the surface (0 for a sharp joint).")
                        }
                        Label { text: qsTr("mm") }
                    }
                    RowLayout {
                        visible: !!window.partOnSurfaceSketch && window.partOnSurfaceSketch.across
                        Label { text: qsTr("Fins") }
                        SpinBox {
                            id: finCount
                            objectName: "finCount"
                            from: 1
                            to: 6
                            value: 1
                            Layout.preferredWidth: 96
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("More than one: copies of the fin stacked across the sketch, each set on the surface where it lands.")
                        }
                        Label { text: qsTr("spaced"); visible: finCount.value > 1 }
                        TextField {
                            id: finSpacing
                            objectName: "finSpacing"
                            visible: finCount.value > 1
                            Layout.preferredWidth: 56
                            text: "40"
                            validator: DoubleValidator { decimals: 2; notation: DoubleValidator.StandardNotation }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Distance between the fins, mm. Negative puts the copies on the other side.")
                        }
                        Label { text: qsTr("mm"); visible: finCount.value > 1 }
                    }
                    RowLayout {
                        visible: !!window.partOnSurfaceSketch && window.partOnSurfaceSketch.across
                        CheckBox {
                            id: finMirror
                            objectName: "finMirror"
                            text: qsTr("Both sides")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Also makes the fins on the other side of the car, mirrored in its centre plane and set on the surface there.")
                        }
                        ComboBox {
                            id: finMirrorPlane
                            visible: finMirror.checked
                            Layout.preferredWidth: 200
                            model: [qsTr("Scan's centre plane"), qsTr("YZ plane"), qsTr("XZ plane"), qsTr("XY plane")]
                        }
                    }
                    RowLayout {
                        Button {
                            text: qsTr("Create")
                            highlighted: true
                            onClicked: {
                                let t = NaN
                                try { t = Number.fromLocaleString(Qt.locale(), partThickness.text) } catch (e) { t = Number(partThickness.text.replace(",", ".")) }
                                if (!(t > 0))
                                    return window.controller.showStatus(qsTr("Enter the thickness in mm"))
                                const fin = window.partOnSurfaceSketch.across
                                let rf = 0
                                try { rf = Number.fromLocaleString(Qt.locale(), finRootFillet.text) } catch (e) { rf = Number(finRootFillet.text.replace(",", ".")) }
                                if (fin && !(rf >= 0))
                                    return window.controller.showStatus(qsTr("Enter the root fillet in mm (0 for none)"))
                                let spacing = 40
                                try { spacing = Number.fromLocaleString(Qt.locale(), finSpacing.text) } catch (e) { spacing = Number(finSpacing.text.replace(",", ".")) }
                                if (fin && finCount.value > 1 && !(Math.abs(spacing) > 0))
                                    return window.controller.showStatus(qsTr("Enter the spacing between the fins in mm"))
                                const r = fin
                                    ? window.controller.execute("surface.fin_from_sketch",
                                        { sketch: window.partOnSurfaceSketch.id, thickness: t, rootFillet: rf,
                                          count: finCount.value, spacing: spacing, mirror: finMirror.checked,
                                          mirrorPlane: ["auto", "yz", "xz", "xy"][finMirrorPlane.currentIndex] })
                                    : window.controller.execute("surface.part_from_sketch",
                                        { sketch: window.partOnSurfaceSketch.id, thickness: t,
                                          side: ["inside", "outside", "both"][partSide.currentIndex] })
                                if (!r.ok)
                                    return
                                window.partOnSurfaceSketch = null
                                window.selectPart(r.result.id)
                                if (fin && r.result.warnings.length > 0)
                                    window.controller.showStatus(qsTr("Fin \"%1\": %2").arg(r.result.name).arg(r.result.warnings[0]))
                                else if (fin && r.result.mirror)
                                    window.controller.showStatus(qsTr("%1 fins, %2 mm thick, on both sides of the %3")
                                        .arg(r.result.fins.length).arg(t)
                                        .arg(r.result.mirror.source === "scan" ? qsTr("scan's centre plane") : r.result.mirror.source.toUpperCase() + qsTr(" plane")))
                                else if (fin && r.result.fins.length > 1)
                                    window.controller.showStatus(qsTr("%1 fins, %2 mm thick").arg(r.result.fins.length).arg(t))
                                else if (fin)
                                    window.controller.showStatus(qsTr("Fin \"%1\": %2 mm thick, %3 cm³")
                                        .arg(r.result.name).arg(t).arg((r.result.volume / 1000).toFixed(1)))
                                else if (r.result.warnings.length > 0)
                                    window.controller.showStatus(qsTr("Part \"%1\": %2").arg(r.result.name).arg(r.result.warnings[0]))
                                else
                                    window.controller.showStatus(qsTr("Part \"%1\": %2 mm thick, %3 cm²")
                                        .arg(r.result.name).arg(t).arg((r.result.area / 100).toFixed(1)))
                            }
                        }
                        Button { text: qsTr("Not now"); onClicked: window.partOnSurfaceSketch = null }
                    }
                }
            }

            DropArea {
                anchors.fill: parent
                onDropped: drop => window.openOrImport(drop.urls)
            }
        }

        PropertiesPanel {
            id: properties
            visible: window.propertiesVisible && !window.simulationVisible
            busy: viewport.transformActive
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 200
            controller: window.controller
            registry: contextActions
            popup: actionPopup
            partId: window.selectedPartId
        }

        SimulationPanel {
            id: simulationPanel
            objectName: "simulationPanel"
            visible: window.simulationVisible
            SplitView.preferredWidth: 400
            SplitView.minimumWidth: 300
            controller: window.controller
            selectedPartId: window.selectedPartId
        }
    }

    footer: ColumnLayout {
        spacing: 0
        TimelineBar {
            id: timelineBar
            Layout.fillWidth: true
            controller: window.controller
            onEditRequested: feature => window.editFeature(feature)
        }
        // Status bar: last command result on the left, navigation hint and units on the right.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.statusBarHeight
            color: Theme.chrome
            Rectangle { width: parent.width; height: 1; color: Theme.border }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space3
                anchors.rightMargin: Theme.space3
                spacing: Theme.space3
                Rectangle {
                    implicitWidth: 6
                    implicitHeight: 6
                    radius: 3
                    color: window.controller.statusMessage.length > 0 ? Theme.success : Theme.textFaint
                }
                Text {
                    Layout.fillWidth: true
                    text: window.controller.statusMessage
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
                Text {
                    Layout.maximumWidth: window.width * 0.5
                    text: Preferences.mouseProfile === "solidworks"
                          ? qsTr("Orbit: right or middle drag · Pan: Ctrl+middle · Zoom: wheel · Fit: F6")
                          : Preferences.mouseProfile === "blender"
                            ? qsTr("Orbit: right or middle drag · Pan: Shift+middle · Zoom: wheel · Fit: F6")
                            : qsTr("Orbit: right drag or Shift+middle · Pan: middle drag · Zoom: wheel · Fit: F6")
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideLeft
                }
                Rectangle { implicitWidth: 1; implicitHeight: 14; color: Theme.border }
                AbstractButton {
                    implicitWidth: shortcutsLabel.implicitWidth
                    implicitHeight: shortcutsLabel.implicitHeight
                    hoverEnabled: true
                    onClicked: shortcutsAction.trigger()
                    contentItem: Text {
                        id: shortcutsLabel
                        text: qsTr("Shortcuts %1").arg(Preferences.shortcutText(shortcutsAction.shortcut))
                        color: parent.hovered ? Theme.accent : Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                    }
                }
                Rectangle { implicitWidth: 1; implicitHeight: 14; color: Theme.border }
                Text {
                    text: Preferences.lengthUnit
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    PreferencesDialog {
        id: preferencesDialog
        anchors.centerIn: Overlay.overlay
    }

    ShortcutsDialog {
        id: shortcutsDialog
        anchors.centerIn: Overlay.overlay
        menuBar: window.menuBar
        extraRows: {
            const g = qsTr("Viewport")
            const rows = [
                { group: g, name: qsTr("Cancel tool / clear selection"), keys: "Esc" },
                { group: g, name: qsTr("Search commands"), keys: "S" },
                { group: g, name: qsTr("Region select tool"), keys: "1" },
                { group: g, name: qsTr("Lasso select tool"), keys: "2" },
                { group: g, name: qsTr("Brush select tool"), keys: "3" },
                { group: g, name: qsTr("Brush smaller / larger"), keys: "[  /  ]" }
            ]
            if (window.isMac)
                rows.push({ group: g, name: qsTr("Delete"), keys: "⌫" })
            return rows
        }
    }

    // S (Fusion): search the commands and run one, in a box at the pointer. Text fields keep
    // the key (they accept it as a ShortcutOverride).
    AppearancePopup {
        id: appearancePopup
        objectName: "appearancePopup"
        parent: Overlay.overlay
        controller: window.controller
    }
    CommandSearch {
        id: commandSearch
        objectName: "commandSearch"
        parent: Overlay.overlay
        menuBar: window.menuBar
    }
    Shortcut {
        sequence: "S"
        enabled: !commandSearch.visible
        onActivated: {
            const p = viewHover.hovered ? viewport.mapToItem(Overlay.overlay, viewHover.point.position.x, viewHover.point.position.y)
                                        : viewport.mapToItem(Overlay.overlay, viewport.width / 2 - commandSearch.width / 2, 60)
            commandSearch.openAt(p.x, p.y)
        }
    }

    ParametersDialog {
        id: parametersDialog
        anchors.centerIn: Overlay.overlay
        controller: window.controller
    }

    Connections {
        target: window.controller
        function onScriptFinished() { viewport.fitAll() }
        function onProjectOpened(view) {
            window.resetWorkspace()
            viewport.restoreCameraState(view)
        }
        // Undo/redo and edits elsewhere (timeline, parameters) change the sketch too.
        function onHistoryChanged() {
            window.refreshSketchSnaps()
        }
        function onPartImported(id) {
            window.selectPart(id)
            viewport.fitAll()
        }
        function onPartChanged(id) {
            if (window.controller.parts.rowOf(id) >= 0)
                return
            if (window.selectedPartIds.indexOf(id) >= 0)
                window.selectedPartIds = window.selectedPartIds.filter(p => p !== id)
            if (window.selectedPartId === id)
                window.selectedPartId = window.selectedPartIds.length
                        ? window.selectedPartIds[window.selectedPartIds.length - 1] : 0
        }
    }
}
