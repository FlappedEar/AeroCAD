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

    // Preferences > Theme: explicit light / dark colours, or the platform's palette.
    // Popups and dialogs inherit the window palette. Groups are set one by one, because
    // setting palette.<role> would also overwrite the disabled colours.
    SystemPalette { id: systemPalette; colorGroup: SystemPalette.Active }
    SystemPalette { id: disabledPalette; colorGroup: SystemPalette.Disabled }
    readonly property var paletteRoles: ["window", "windowText", "base", "alternateBase", "toolTipBase",
        "toolTipText", "placeholderText", "text", "button", "buttonText", "brightText", "light", "midlight",
        "mid", "dark", "shadow", "highlight", "highlightedText", "link"]
    readonly property var dimmedRoles: ["windowText", "text", "buttonText"]

    function applyTheme(initial) {
        const c = Preferences.colors
        const system = c.window === undefined
        if (system && initial)
            return // keep the platform palette untouched
        for (const role of paletteRoles) {
            const active = system ? systemPalette[role] : c[role]
            const disabled = system ? disabledPalette[role]
                                    : (dimmedRoles.includes(role) ? c.disabledText : c[role])
            if (active === undefined)
                continue
            palette.active[role] = active
            palette.inactive[role] = active
            if (disabled !== undefined)
                palette.disabled[role] = disabled
        }
    }

    Component.onCompleted: applyTheme(true)
    Connections {
        target: Preferences
        function onThemeChanged() { window.applyTheme(false) }
    }
    title: qsTr("AeroCAD %1").arg(controller.version)

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
        id: importAction
        text: qsTr("&Import...")
        shortcut: StandardKey.Open // Fusion: Ctrl+O / Cmd+O
        onTriggered: importDialog.open()
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
            if (viewport.leaveSelectionTool())
                return
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
        Menu {
            title: qsTr("&Help")
            MenuItem { action: shortcutsAction }
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
                caption: qsTr("EXPORT")
                ToolCommand { action: exportSelectedAction; text: qsTr("Selected") }
                ToolCommand { action: exportAllAction; text: qsTr("All") }
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
            selectionToolsEnabled: !featureDialog.visible && !window.pickingSketchPlane
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
                text: Preferences.mouseProfile === "solidworks"
                      ? qsTr("Orbit: middle drag · Pan: Ctrl+middle drag · Zoom: wheel, Shift+middle drag · Shortcuts: %1").arg(Preferences.shortcutText(shortcutsAction.shortcut))
                      : Preferences.mouseProfile === "blender"
                        ? qsTr("Orbit: middle drag · Pan: Shift+middle drag · Zoom: wheel, Ctrl+middle drag · Shortcuts: %1").arg(Preferences.shortcutText(shortcutsAction.shortcut))
                        : qsTr("Pan: middle drag · Orbit: Shift+middle drag · Zoom: wheel · Touchpad: two-finger pan, Shift+two-finger orbit, pinch zoom · Fit: F6 · Shortcuts: %1").arg(Preferences.shortcutText(shortcutsAction.shortcut))
                opacity: 0.6
                elide: Text.ElideLeft
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
