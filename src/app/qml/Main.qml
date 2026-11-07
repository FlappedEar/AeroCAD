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
    property var selectedPartId: 0
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"

    property bool browserVisible: true
    property bool propertiesVisible: true

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

    function selectPart(id) {
        selectedPartId = id
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

    PartContextMenu {
        id: partMenu
        registry: contextActions
        popupHelper: actionPopup
    }

    function toggleSelectedVisibility() {
        if (!selectedPartId)
            return
        const info = controller.partInfo(selectedPartId)
        if (info.id)
            controller.execute("part.set_visible", { id: selectedPartId, visible: !info.visible })
    }

    function deleteSelected() {
        if (!selectedPartId)
            return
        const info = controller.partInfo(selectedPartId)
        // Bodies and sketches come from the timeline: delete the feature that made them.
        if (info.kind === "body" || info.kind === "sketch")
            controller.execute("feature.delete", { id: info.featureId })
        else
            controller.execute("part.remove", { id: selectedPartId })
    }

    function selectedInfo() {
        return selectedPartId ? controller.partInfo(selectedPartId) : ({})
    }

    // --- Modelling ----------------------------------------------------------------------

    /// Create Sketch: pick a plane, face or surface. `across` (optional) presets "Fin
    /// across the surface".
    function startCreateSketch(across) {
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

    function enterSketch(id) {
        const r = controller.execute("sketch.profiles", { sketch: id, surfaceLine: true })
        if (!r.ok)
            return
        const pl = r.result.plane
        const on = r.result.onSurface
        activeSketch = { id: id, origin: pl.origin, normal: pl.normal, xDir: pl.xDir, yDir: pl.yDir,
                         surfacePart: on ? on.part : 0, across: !!(on && on.across),
                         surfaceLine: r.result.surfaceLine || [] }
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
        viewport.setViewDirection(Qt.vector3d(pl.normal[0], pl.normal[1], pl.normal[2]))
        // On a scan, look at the clicked place: the whole model would leave it tiny.
        if (on && on.point && controller.sceneRadius > 0)
            viewport.frameAround(on.point, controller.sceneRadius * 0.35)
        else
            viewport.fitAll()
    }

    /// Ends sketch editing. With `offerPart` (the user finished it), a closed outline drawn
    /// over a surface or scan offers to make the part on that surface right away.
    function finishSketch(offerPart) {
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
        const initial = {}
        const info = selectedInfo()
        if (activeSketch) {
            initial.sketch = activeSketch.id
            finishSketch()
        } else if (info.kind === "sketch") {
            initial.sketch = info.featureId
        }
        if (info.kind === "body") {
            if (type === "combine")
                initial.target = info.featureId
            else if (type === "fillet" || type === "chamfer" || type === "shell" || type === "hole")
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
        if (!controller.projectDirty) {
            fn()
            return
        }
        afterUnsavedCheck = fn
        unsavedDialog.open()
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
        onAccepted: window.controller.importFile(selectedFile)
    }

    FileDialog {
        id: exportDialog
        // Part ids to export; empty = all visible parts.
        property var partIds: []
        title: partIds.length ? qsTr("Export selected part") : qsTr("Export visible parts")
        fileMode: FileDialog.SaveFile
        nameFilters: window.controller.exportNameFilters
        defaultSuffix: selectedNameFilter.extensions.length ? selectedNameFilter.extensions[0] : "stl"
        onAccepted: {
            const r = window.controller.exportFile(selectedFile, partIds)
            if (!r.ok) {
                exportMessage.title = qsTr("Export failed")
                exportMessage.text = r.error
                exportMessage.open()
            } else if (r.warnings.length) {
                exportMessage.title = qsTr("Exported with warnings")
                exportMessage.text = qsTr("The file was written, but some meshes are not watertight solids. " +
                                          "Slicers and CFD meshers may reject or repair them.\n\n") +
                                     r.warnings.join("\n")
                exportMessage.open()
            }
        }
    }

    MessageDialog {
        id: exportMessage
        buttons: MessageDialog.Ok
    }

    function openExport(ids) {
        exportDialog.partIds = ids
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
        onTriggered: window.openExport([window.selectedPartId])
    }
    Action {
        id: exportAllAction
        text: qsTr("Export &All Visible...")
        enabled: window.controller.parts.count > 0
        shortcut: "Ctrl+Shift+E"
        onTriggered: window.openExport([])
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
        text: window.controller.canUndo ? qsTr("&Undo %1").arg(window.controller.undoText) : qsTr("&Undo")
        enabled: window.controller.canUndo
        shortcut: StandardKey.Undo // Fusion: Ctrl+Z / Cmd+Z
        onTriggered: window.controller.execute("edit.undo")
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
        enabled: !!window.selectedPartId
        shortcut: "Delete" // Fusion: Del
        onTriggered: window.deleteSelected()
    }
    Action {
        id: visibilityAction
        text: qsTr("Toggle &Visibility")
        enabled: !!window.selectedPartId
        shortcut: "V" // Fusion: V
        onTriggered: window.toggleSelectedVisibility()
    }
    Action {
        id: fitAction
        text: qsTr("&Fit")
        // Fusion lists F6 for Zoom to Fit (Electronics workspace); F is Fillet in Fusion.
        shortcut: "F6"
        onTriggered: viewport.fitAll()
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

    // Modelling tools. Single-letter shortcuts are Fusion's defaults
    // (https://www.autodesk.com/shortcuts/fusion-360): E Extrude, F Fillet, L Line,
    // R 2-point Rectangle, C Center Diameter Circle.
    Action { id: createSketchAction; text: qsTr("Create &Sketch"); onTriggered: window.startCreateSketch() }
    Action { id: finishSketchAction; text: qsTr("Finish Sketch"); enabled: !!window.activeSketch; onTriggered: window.finishSketch(true) }
    Action { id: extrudeAction; text: qsTr("&Extrude"); shortcut: "E"; onTriggered: window.openFeature("extrude") }
    Action { id: revolveAction; text: qsTr("Re&volve"); onTriggered: window.openFeature("revolve") }
    Action { id: boxAction; text: qsTr("&Box"); onTriggered: window.openFeature("box") }
    Action { id: cylinderAction; text: qsTr("C&ylinder"); onTriggered: window.openFeature("cylinder") }
    Action { id: filletAction; text: qsTr("&Fillet"); shortcut: "F"; enabled: !window.activeSketch; onTriggered: window.openFeature("fillet") }
    Action { id: holeAction; text: qsTr("H&ole"); shortcut: "H"; enabled: !window.activeSketch; onTriggered: window.openFeature("hole") }
    Action { id: chamferAction; text: qsTr("C&hamfer"); onTriggered: window.openFeature("chamfer") }
    Action { id: shellAction; text: qsTr("S&hell"); onTriggered: window.openFeature("shell") }
    Action { id: combineAction; text: qsTr("Co&mbine"); onTriggered: window.openFeature("combine") }
    Action {
        id: convertAction
        text: qsTr("Convert to &Body")
        enabled: !!window.selectedPartId && window.selectedInfo().kind === "imported"
        onTriggered: window.controller.execute("feature.base_body", { part: window.selectedPartId })
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
    Action { id: fixAction; text: qsTr("Fix/Unfix"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "fix"; onTriggered: window.sketchTool_("fix") }
    Action { id: slotAction; text: qsTr("Sl&ot"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "slot"; onTriggered: window.sketchTool_("slot") }

    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (viewport.leaveSelectionTool())
                return
            if (featureDialog.visible)
                featureDialog.close()
            else if (window.pickingSketchPlane)
                window.pickingSketchPlane = false
            else if (window.activeSketch && viewport.sketchOverlay.points.length > 0)
                viewport.sketchOverlay.reset()
            else if (window.activeSketch)
                window.finishSketch(true)
            else if (window.partOnSurfaceSketch)
                window.partOnSurfaceSketch = null
        }
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
            MenuItem { action: visibilityAction }
            MenuItem { action: deleteAction }
            MenuSeparator {}
            MenuItem { action: preferencesAction }
        }
        Menu {
            title: qsTr("&Create")
            MenuItem { action: createSketchAction }
            MenuItem { action: finishSketchAction }
            MenuSeparator {}
            MenuItem { action: extrudeAction }
            MenuItem { action: revolveAction }
            MenuItem { action: boxAction }
            MenuItem { action: cylinderAction }
            MenuItem { action: holeAction }
        }
        Menu {
            title: qsTr("&Modify")
            MenuItem { action: filletAction }
            MenuItem { action: chamferAction }
            MenuItem { action: shellAction }
            MenuItem { action: combineAction }
            MenuSeparator {}
            MenuItem { action: parametersAction }
            MenuSeparator {}
            MenuItem { action: duplicateVariantAction }
            MenuItem { action: toggleVariantAction }
            MenuItem { action: deleteVariantAction }
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
            title: qsTr("&View")
            MenuItem { action: fitAction }
            MenuItem { action: homeAction }
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
    component ToolSeparator: Rectangle {
        property string tab: "solid"
        visible: !parent || parent.currentTab === undefined || parent.currentTab === tab
        Layout.fillHeight: true
        Layout.topMargin: 6
        Layout.bottomMargin: 6
        implicitWidth: 1
        color: Theme.separator
    }

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

            ToolGroup {
                caption: qsTr("CREATE")
                maxVisible: 6
                ToolCommand { action: createSketchAction; text: qsTr("Sketch"); iconName: "sketch" }
                ToolCommand { action: extrudeAction; text: qsTr("Extrude"); iconName: "extrude" }
                ToolCommand { action: revolveAction; text: qsTr("Revolve"); iconName: "revolve" }
                ToolCommand { action: holeAction; text: qsTr("Hole"); iconName: "hole" }
                ToolCommand { action: boxAction; text: qsTr("Box"); iconName: "box" }
                ToolCommand { action: cylinderAction; text: qsTr("Cylinder"); iconName: "cylinder" }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("MODIFY")
                maxVisible: 6
                ToolCommand { action: filletAction; text: qsTr("Fillet"); iconName: "fillet" }
                ToolCommand { action: chamferAction; text: qsTr("Chamfer"); iconName: "chamfer" }
                ToolCommand { action: shellAction; text: qsTr("Shell"); iconName: "shell" }
                ToolCommand { action: combineAction; text: qsTr("Combine"); iconName: "combine" }
                ToolCommand { action: parametersAction; text: qsTr("Parameters"); iconName: "parameters" }
                ToolCommand { action: deleteAction; text: qsTr("Delete"); iconName: "delete" }
                ToolCommand { action: visibilityAction; text: qsTr("Show/Hide"); iconName: "visible" }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("INSERT")
                ToolCommand { action: importAction; text: qsTr("Import"); iconName: "import" }
                ToolCommand { action: convertAction; text: qsTr("To Body"); iconName: "mesh-to-body" }
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
            onPartSelected: id => window.selectPart(id)
            onPartContextRequested: (id, item, x, y) => {
                window.selectPart(id)
                partMenu.info = window.controller.partInfo(id)
                partMenu.popup(item, x, y)
            }
        }

        Viewport {
            id: viewport
            objectName: "viewport"
            SplitView.fillWidth: true
            controller: window.controller
            selectedPartId: window.selectedPartId
            sketch: window.activeSketch
            sketchTool: window.sketchTool
            selectionToolsEnabled: !featureDialog.visible && !window.pickingSketchPlane
            snapToNearestSurface: window.pickingSketchPlane
            property bool snappedClick: false
            onSnappedToNearestSurface: snappedClick = true
            markers: featureDialog.visible ? featureDialog.pickedPoints : []
            pickKind: featureDialog.visible ? featureDialog.pickKind : ""
            shownSketch: featureDialog.shownSketch
            onPartPicked: id => {
                window.selectPart(id)
                // A click through a hole of a scan, or next to the model, while choosing where to sketch.
                if (!id && window.pickingSketchPlane)
                    window.controller.showStatus(qsTr("Nothing there to sketch on: click on or next to the surface"))
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
                         && !window.pickingSketchPlane
                registry: contextActions
                popup: actionPopup
                info: { void contextActions.revision; return window.selectedPartId ? window.controller.partInfo(window.selectedPartId) : ({}) }
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
                              ? qsTr("Gives the outline a thickness (half each side of the sketch)\nand cuts its root to the surface, so it sits flush on it.")
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
                        Button {
                            text: qsTr("Create")
                            highlighted: true
                            onClicked: {
                                let t = NaN
                                try { t = Number.fromLocaleString(Qt.locale(), partThickness.text) } catch (e) { t = Number(partThickness.text.replace(",", ".")) }
                                if (!(t > 0))
                                    return window.controller.showStatus(qsTr("Enter the thickness in mm"))
                                const fin = window.partOnSurfaceSketch.across
                                const r = fin
                                    ? window.controller.execute("surface.fin_from_sketch",
                                        { sketch: window.partOnSurfaceSketch.id, thickness: t })
                                    : window.controller.execute("surface.part_from_sketch",
                                        { sketch: window.partOnSurfaceSketch.id, thickness: t,
                                          side: ["inside", "outside", "both"][partSide.currentIndex] })
                                if (!r.ok)
                                    return
                                window.partOnSurfaceSketch = null
                                window.selectPart(r.result.id)
                                if (fin)
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
                onDropped: drop => {
                    for (const url of drop.urls)
                        window.controller.importFile(url)
                }
            }
        }

        PropertiesPanel {
            id: properties
            visible: window.propertiesVisible
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 200
            controller: window.controller
            registry: contextActions
            popup: actionPopup
            partId: window.selectedPartId
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
                          ? qsTr("Orbit: middle drag · Pan: Ctrl+middle · Zoom: wheel")
                          : Preferences.mouseProfile === "blender"
                            ? qsTr("Orbit: middle drag · Pan: Shift+middle · Zoom: wheel")
                            : qsTr("Pan: middle drag · Orbit: Shift+middle · Zoom: wheel · Fit: F6")
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

    ParametersDialog {
        id: parametersDialog
        anchors.centerIn: Overlay.overlay
        controller: window.controller
    }

    Connections {
        target: window.controller
        function onScriptFinished() { viewport.fitAll() }
        function onProjectOpened(view) {
            // The dialog may hold a feature id of the previous project.
            featureDialog.close()
            window.selectPart(0)
            window.finishSketch()
            viewport.restoreCameraState(view)
        }
        // Undo/redo and edits elsewhere (timeline, parameters) change the sketch too.
        function onHistoryChanged() { window.refreshSketchSnaps() }
        function onPartImported(id) {
            window.selectPart(id)
            viewport.fitAll()
        }
        function onPartChanged(id) {
            if (window.selectedPartId === id && window.controller.parts.rowOf(id) < 0)
                window.selectPart(0)
        }
    }
}
