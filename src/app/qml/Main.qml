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

    width: 1400
    height: 860
    visible: true
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

    function startCreateSketch() {
        finishSketch()
        featureDialog.close()
        pickingSketchPlane = true
    }

    function createSketch(params) {
        pickingSketchPlane = false
        const r = controller.execute("sketch.create", params)
        if (r.ok)
            enterSketch(r.result.id)
    }

    /// Snap points of a sketch: ends of lines, rectangle corners, centres.
    function sketchSnapPoints(feature) {
        const out = []
        const num = v => typeof v === "number"
        const pt = p => p && num(p[0]) && num(p[1])
        for (const c of (feature && feature.params.curves) || []) {
            for (const k of ["start", "end", "through", "center", "corner", "opposite"]) {
                if (pt(c[k]))
                    out.push(c[k])
            }
            if (c.type === "rectangle" && pt(c.corner) && num(c.width) && num(c.height)) {
                const [x, y] = c.corner
                out.push([x + c.width, y], [x + c.width, y + c.height], [x, y + c.height])
            }
        }
        return out
    }

    function refreshSketchSnaps() {
        if (!activeSketch)
            return
        const r = controller.execute("timeline.list")
        if (r.ok)
            viewport.sketchSnapPoints = sketchSnapPoints(r.result.features.find(f => f.id === activeSketch.id))
    }

    function enterSketch(id) {
        const r = controller.execute("sketch.profiles", { sketch: id })
        if (!r.ok)
            return
        const pl = r.result.plane
        activeSketch = { id: id, origin: pl.origin, normal: pl.normal, xDir: pl.xDir, yDir: pl.yDir }
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
        viewport.fitAll()
    }

    function finishSketch() {
        activeSketch = null
        sketchTool = ""
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
    Action { id: finishSketchAction; text: qsTr("Finish Sketch"); enabled: !!window.activeSketch; onTriggered: window.finishSketch() }
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
    Action { id: slotAction; text: qsTr("Sl&ot"); enabled: !!window.activeSketch; checkable: true; checked: window.sketchTool === "slot"; onTriggered: window.sketchTool_("slot") }

    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (featureDialog.visible)
                featureDialog.close()
            else if (window.pickingSketchPlane)
                window.pickingSketchPlane = false
            else if (window.activeSketch && viewport.sketchOverlay.points.length > 0)
                viewport.sketchOverlay.reset()
            else if (window.activeSketch)
                window.finishSketch()
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
    }

    // --- Toolbar: grouped commands with captions, as in Fusion's toolbar ---------------

    component ToolGroup: ColumnLayout {
        property string caption
        default property alias buttons: row.data
        spacing: 0
        RowLayout {
            id: row
            spacing: 0
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: parent.caption
            font.pixelSize: 10
            font.bold: true
            opacity: 0.6
        }
    }

    component ToolCommand: ToolButton {
        display: AbstractButton.TextOnly
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            spacing: 6
            ToolGroup {
                caption: qsTr("EDIT")
                ToolCommand { action: undoAction; text: qsTr("Undo") }
                ToolCommand { action: redoAction; text: qsTr("Redo") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("INSERT")
                ToolCommand { action: importAction; text: qsTr("Import") }
                ToolCommand { action: convertAction; text: qsTr("To Body") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("CREATE")
                visible: !window.activeSketch
                ToolCommand { action: createSketchAction; text: qsTr("Sketch") }
                ToolCommand { action: extrudeAction; text: qsTr("Extrude") }
                ToolCommand { action: revolveAction; text: qsTr("Revolve") }
                ToolCommand { action: boxAction; text: qsTr("Box") }
                ToolCommand { action: cylinderAction; text: qsTr("Cylinder") }
                ToolCommand { action: holeAction; text: qsTr("Hole") }
            }
            ToolGroup {
                caption: qsTr("SKETCH")
                visible: !!window.activeSketch
                ToolCommand { action: lineAction; text: qsTr("Line") }
                ToolCommand { action: rectangleAction; text: qsTr("Rectangle") }
                ToolCommand { action: circleAction; text: qsTr("Circle") }
                ToolCommand { action: arcAction; text: qsTr("Arc") }
                ToolCommand { action: polygonAction; text: qsTr("Polygon") }
                ToolCommand { action: slotAction; text: qsTr("Slot") }
                ToolCommand { action: extrudeAction; text: qsTr("Extrude") }
                ToolCommand { action: finishSketchAction; text: qsTr("Finish Sketch") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("MODIFY")
                ToolCommand { action: filletAction; text: qsTr("Fillet") }
                ToolCommand { action: chamferAction; text: qsTr("Chamfer") }
                ToolCommand { action: shellAction; text: qsTr("Shell") }
                ToolCommand { action: combineAction; text: qsTr("Combine") }
                ToolCommand { action: parametersAction; text: qsTr("Parameters") }
                ToolCommand { action: visibilityAction; text: qsTr("Show/Hide") }
                ToolCommand { action: deleteAction; text: qsTr("Delete") }
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
                caption: qsTr("VIEW")
                ToolCommand { action: fitAction; text: qsTr("Fit") }
                ToolCommand { action: homeAction; text: qsTr("Home") }
                ToolCommand { action: orthoAction; text: qsTr("Ortho") }
            }
            Item { Layout.fillWidth: true }
        }
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
        }

        Viewport {
            id: viewport
            objectName: "viewport"
            SplitView.fillWidth: true
            controller: window.controller
            selectedPartId: window.selectedPartId
            sketch: window.activeSketch
            sketchTool: window.sketchTool
            markers: featureDialog.visible ? featureDialog.pickedPoints : []
            onPartPicked: id => window.selectPart(id)
            onPointPicked: (id, point) => {
                const info = window.controller.partInfo(id)
                if (window.pickingSketchPlane) {
                    if (info.kind === "body")
                        window.createSketch({ face: { body: info.featureId, point: [point.x, point.y, point.z] } })
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
                padding: 12
                ColumnLayout {
                    Label { text: qsTr("Create Sketch"); font.bold: true; font.pixelSize: 15 }
                    Label { text: qsTr("Click a planar face of a body, or pick an origin plane:"); opacity: 0.75 }
                    RowLayout {
                        Button { text: qsTr("XY (Top)"); onClicked: window.createSketch({ plane: "XY" }) }
                        Button { text: qsTr("XZ (Front)"); onClicked: window.createSketch({ plane: "XZ" }) }
                        Button { text: qsTr("YZ (Right)"); onClicked: window.createSketch({ plane: "YZ" }) }
                    }
                    Button { text: qsTr("Cancel"); onClicked: window.pickingSketchPlane = false }
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
        ToolBar {
        Layout.fillWidth: true
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            Label {
                Layout.fillWidth: true
                text: window.controller.statusMessage
                elide: Text.ElideRight
            }
            Label {
                text: qsTr("Pan: middle drag · Orbit: Shift+middle drag · Zoom: wheel · Touchpad: two-finger pan, Shift+two-finger orbit, pinch zoom · Fit: F6")
                opacity: 0.6
                elide: Text.ElideLeft
            }
        }
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
