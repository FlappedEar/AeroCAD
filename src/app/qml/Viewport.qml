// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick3D
import AeroCAD

Item {
    id: root

    required property AppController controller
    property var selectedPartId: 0
    signal partPicked(var id)
    /// Right-click on a part (id 0: on empty space), at x, y in this item.
    signal partContextRequested(var id, real x, real y)
    /// Left click on a part: its id and the hit point in model coordinates (mm, Z up).
    signal pointPicked(var id, vector3d point)

    /// Active sketch ({id, origin, normal, xDir, yDir} in model space) or null; while set,
    /// left clicks draw with `sketchTool` instead of selecting.
    property var sketch: null
    /// Region selection tools (toolbar, keys 1/2/3); off while sketching or while a
    /// modelling dialog uses the top-left corner.
    property bool selectionToolsEnabled: true
    readonly property bool selectionToolsAvailable: selectionToolsEnabled && !sketch
    onSelectionToolsAvailableChanged: if (!selectionToolsAvailable) tool = "navigate"

    /// Returns true when a selection tool was active and is now left (Esc).
    function leaveSelectionTool() {
        if (tool === "navigate")
            return false
        controller.brushCancel()
        tool = "navigate"
        return true
    }
    property string sketchTool: ""
    property var sketchSnapPoints: []
    /// Only parts of this kind ("body", "sketch") can be clicked; "" = all.
    property string pickKind: ""
    /// Sketch feature shown even when hidden (the sketch a feature dialog works on).
    property var shownSketch: -1
    property var sketchCurves: []
    property var sketchConstraints: []
    property int sketchDegreesOfFreedom: -1
    signal curveDrawn(var curve)
    signal constraintAdded(var constraint)
    signal constraintEdited(int index, var value)
    signal constraintRemoved(int index)
    signal sketchToolRequested(string tool)
    /// Model-space points to mark (selections of the open feature dialog).
    property var markers: []
    readonly property alias sketchOverlay: sketchOverlay

    property bool fitPending: false

    /// Camera projection (Fusion ViewCube menu: Orthographic / Perspective).
    property bool orthographic: false
    property bool viewCubeVisible: true
    /// Ground grid on the XY plane, spaced to suit the scene size (View > Show Grid).
    property bool gridVisible: true
    property bool navigationBarVisible: true
    /// Home view orientation; "Set current view as Home" changes it, "Reset Home" restores it.
    readonly property real defaultHomePitch: -30
    readonly property real defaultHomeYaw: 45
    property real homePitch: defaultHomePitch
    property real homeYaw: defaultHomeYaw
    readonly property alias pitch: cameraInput.pitch
    readonly property alias yaw: cameraInput.yaw
    property alias leftDragMode: cameraInput.leftDragMode
    // Region selection tools (see SelectionToolbar.qml). Keys: 1 smooth region (click),
    // 2 lasso, 3 brush, Esc navigate; [ and ] change the brush size.
    property string tool: "navigate" // navigate | region | lasso | brush
    property string selectMode: "replace" // replace | add | subtract
    property real regionAngle: 20 // degrees between neighbouring triangles
    property real regionSeedAngle: 180 // degrees from the clicked triangle; 180 = off
    property real brushRadius: 0 // mm; 0 = set from the part size on first use
    property bool brushConnected: true
    property bool lassoThrough: false

    onToolChanged: {
        root.controller.brushCancel()
        lassoCanvas.points = []
        lassoCanvas.requestPaint()
        brushCursor.visible = false
        if (tool === "brush" && root.selectedPartId)
            ensureBrushRadius(root.selectedPartId)
    }

    /// Surface hit at viewport position (x, y): {partId, point (model mm), distance from the
    /// camera}, or null. Ray-cast against the mesh triangles in C++: Qt Quick 3D picks
    /// custom geometry by its bounding volume only.
    function pickPart(x, y) {
        const near = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 0)))
        const far = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 100)))
        const hit = controller.pickSurface(near, far.minus(near))
        if (!hit.partId)
            return null
        const eye = modelSpace.mapPositionFromScene(view.camera.scenePosition)
        return { partId: hit.partId, point: hit.point, distance: hit.point.minus(eye).length() }
    }

    /// Point on part `id` under view position (x, y) (model mm), or null.
    function pickOnPart(x, y, id) {
        const near = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 0)))
        const far = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 100)))
        const hit = controller.pickPartSurface(id, near, far.minus(near))
        return hit.partId ? hit.point : null
    }

    function modelPoint(hit) {
        return hit.point
    }

    function ensureBrushRadius(id) {
        if (brushRadius > 0)
            return
        const info = controller.partInfo(id)
        brushRadius = Math.max(0.1, Number(((info.diagonal ?? 100) * 0.03).toPrecision(2)))
    }

    function brushStrokeMode() {
        return selectMode
    }

    function pickRegion(hit) {
        const p = modelPoint(hit)
        controller.execute("selection.pick_region", {
            id: hit.partId,
            point: [p.x, p.y, p.z],
            maxAngle: regionAngle,
            maxSeedAngle: regionSeedAngle,
            mode: selectMode
        })
    }

    /// Part the lasso applies to: the selected one, or the only part in the document.
    function lassoPartId() {
        if (selectedPartId)
            return selectedPartId
        const r = controller.execute("document.parts")
        const parts = r.ok ? r.result.parts : []
        return parts.length === 1 ? parts[0].id : 0
    }

    function finishLasso(points) {
        if (points.length < 3)
            return
        const id = lassoPartId()
        if (!id) {
            controller.showStatus(qsTr("Lasso: select a part first."))
            return
        }
        const active = view.camera
        const eye = modelSpace.mapPositionFromScene(active.scenePosition)
        const forward = modelSpace.mapDirectionFromScene(active.forward)
        const up = modelSpace.mapDirectionFromScene(active.up)
        const viewParams = {
            eye: [eye.x, eye.y, eye.z],
            forward: [forward.x, forward.y, forward.z],
            up: [up.x, up.y, up.z],
            fov: camera.fieldOfView,
            aspect: view.width / Math.max(1, view.height)
        }
        if (root.orthographic) {
            viewParams.orthographic = true
            viewParams.height = view.height / orthoCamera.verticalMagnification
        }
        controller.execute("selection.lasso", {
            id: id,
            polygon: points.map(p => [p.x / view.width, p.y / view.height]),
            view: viewParams,
            visibleOnly: !lassoThrough,
            mode: selectMode
        })
        partPicked(id)
    }

    /// Frames all visible parts. Deferred until the view has a size.
    function fitAll() {
        if (width <= 0 || height <= 0) {
            fitPending = true
            return
        }
        fitPending = false
        // An empty scene (e.g. a new sketch) gets a 100 mm view.
        const r = controller.sceneRadius > 1e-6 ? Math.max(controller.sceneRadius, 1) : 50
        cameraInput.pivot.position = controller.sceneCenter
        const halfFov = camera.fieldOfView * Math.PI / 360
        const aspect = Math.max(0.1, width / Math.max(1, height))
        const fitFov = aspect < 1 ? Math.atan(Math.tan(halfFov) * aspect) : halfFov
        cameraInput.distance = r / Math.sin(fitFov) * 1.1
        updateClipping()
    }

    /// Camera state stored in project files.
    function cameraState() {
        const p = cameraInput.pivot.position
        return { pivot: [p.x, p.y, p.z], pitch: cameraInput.pitch, yaw: cameraInput.yaw,
                 distance: cameraInput.distance, orthographic: orthographic }
    }

    /// Restores cameraState() output; frames everything when `state` has no camera.
    function restoreCameraState(state) {
        if (!state || typeof state.distance !== "number" || !state.pivot || state.pivot.length !== 3) {
            fitAll()
            return
        }
        cameraInput.pitch = state.pitch
        cameraInput.yaw = state.yaw
        cameraInput.distance = state.distance
        cameraInput.pivot.position = Qt.vector3d(state.pivot[0], state.pivot[1], state.pivot[2])
        orthographic = !!state.orthographic
        updateClipping()
    }

    onWidthChanged: if (fitPending) fitAll()
    onHeightChanged: if (fitPending) fitAll()

    /// Standard view (degrees, CameraController convention), framed to fit everything.
    function setView(pitch, yaw) {
        cameraInput.animateTo(pitch, yaw)
        fitAll()
    }

    /// Looks at the model from `direction` (model space, Z up): the ViewCube's face, edge and
    /// corner views. Keeps the current target and zoom.
    function setViewDirection(direction) {
        // Model (x, y, z) is (x, z, -y) in Qt Quick 3D's Y-up scene; the camera sits on the
        // pivot's +Z axis, i.e. at (cos p sin y, -sin p, cos p cos y) for pitch p and yaw y.
        const d = Qt.vector3d(direction.x, direction.z, -direction.y).normalized()
        const pitch = -Math.asin(Math.max(-1, Math.min(1, d.y))) * 180 / Math.PI
        const yaw = Math.abs(d.y) > 0.9999 ? 0 : Math.atan2(d.x, d.z) * 180 / Math.PI
        cameraInput.animateTo(pitch, yaw)
    }

    function goHome() {
        setView(homePitch, homeYaw)
    }

    function setCurrentViewAsHome() {
        homePitch = cameraInput.pitch
        homeYaw = cameraInput.yaw
    }

    function resetHome() {
        homePitch = defaultHomePitch
        homeYaw = defaultHomeYaw
    }

    function updateClipping() {
        const r = Math.max(controller.sceneRadius, 1)
        camera.clipFar = cameraInput.distance + r * 4
        camera.clipNear = Math.max(cameraInput.distance - r * 2, cameraInput.distance * 0.001, 0.001)
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            // Top-to-bottom gradient, as in Fusion and most CAD applications (Theme.qml).
            GradientStop { position: 0.0; color: Theme.viewportTop }
            GradientStop { position: 1.0; color: Theme.viewportBottom }
        }
    }

    View3D {
        id: view
        anchors.fill: parent
        camera: root.orthographic ? orthoCamera : camera

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        Node {
            id: pivot
            PerspectiveCamera {
                id: camera
                fieldOfView: 40
                clipNear: 1
                clipFar: 100000
                // Headlight: lighting follows the view, so every side is readable.
                DirectionalLight {
                    eulerRotation: Qt.vector3d(-20, -20, 0)
                    brightness: 1.0
                    ambientColor: Qt.rgba(0.35, 0.35, 0.38, 1)
                }
            }
            // Same pose as the perspective camera; the magnification matches its framing at the pivot.
            OrthographicCamera {
                id: orthoCamera
                z: cameraInput.distance
                clipNear: Math.max(1e-4, camera.clipNear * 0.5)
                clipFar: camera.clipFar
                horizontalMagnification: {
                    const w = cameraInput.worldPerPixel()
                    return isFinite(w) && w > 0 ? 1 / w : 1
                }
                verticalMagnification: horizontalMagnification
            }
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-60, 30, 0)
            brightness: 0.35
        }

        // Model space is Z-up (CAD convention); Qt Quick 3D is Y-up.
        Node {
            id: modelSpace
            eulerRotation.x: -90

            // Ground grid: minor lines every `step`, major lines every 5th.
            Node {
                id: grid
                visible: root.gridVisible
                // A round step (1, 2 or 5 x 10^n mm), about a fifth of the scene radius.
                readonly property real step: {
                    const target = Math.max(root.controller.sceneRadius, 1e-3) / 5
                    const power = Math.pow(10, Math.floor(Math.log10(target)))
                    const f = target / power
                    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * power
                }
                Model {
                    pickable: false
                    geometry: GridGeometry { step: grid.step; extent: grid.step * 25; majorEvery: 5; major: false }
                    materials: PrincipledMaterial {
                        lighting: PrincipledMaterial.NoLighting
                        baseColor: Theme.gridMinor
                        alphaMode: PrincipledMaterial.Blend
                    }
                }
                Model {
                    pickable: false
                    geometry: GridGeometry { step: grid.step; extent: grid.step * 25; majorEvery: 5; major: true }
                    materials: PrincipledMaterial {
                        lighting: PrincipledMaterial.NoLighting
                        baseColor: Theme.gridMajor
                        alphaMode: PrincipledMaterial.Blend
                    }
                }
            }

            Repeater3D {
                model: root.controller.parts
                delegate: Model {
                    id: partModel
                    required property var partId
                    required property bool partVisible
                    required property string kind
                    required property var featureId
                    property var pickPartId: partId
                    readonly property bool selected: partId === root.selectedPartId

                    visible: partVisible || (kind === "sketch" && featureId == root.shownSketch)
                    pickable: root.pickKind === "" || kind === root.pickKind
                    geometry: MeshGeometry {
                        controller: root.controller
                        partId: partModel.partId
                    }
                    // Fusion-like colours: grey bodies, translucent orange sketch profiles.
                    materials: PrincipledMaterial {
                        baseColor: partModel.selected ? Theme.selectedColor
                                 : partModel.kind === "sketch" ? Theme.sketchProfile
                                 : partModel.kind === "body" ? Theme.bodyColor : Theme.meshColor
                        alphaMode: partModel.kind === "sketch" ? PrincipledMaterial.Blend : PrincipledMaterial.Default
                        metalness: 0.0
                        roughness: 0.45
                        specularAmount: 0.35
                        cullMode: Material.NoCulling
                    }

                    // Region selection highlight.
                    Model {
                        visible: !selectionGeometry.empty
                        pickable: false
                        geometry: SelectionGeometry {
                            id: selectionGeometry
                            controller: root.controller
                            partId: partModel.partId
                        }
                        materials: PrincipledMaterial {
                            baseColor: Theme.regionColor
                            emissiveFactor: Qt.vector3d(0.35, 0.16, 0.0)
                            metalness: 0.0
                            roughness: 0.6
                            cullMode: Material.NoCulling
                        }
                    }

                    // Sketch curves and body edges.
                    Model {
                        visible: !lines.empty
                        pickable: false
                        geometry: LineGeometry {
                            id: lines
                            controller: root.controller
                            partId: partModel.partId
                        }
                        materials: PrincipledMaterial {
                            lighting: PrincipledMaterial.NoLighting
                            baseColor: partModel.kind === "sketch" ? Theme.sketchColor : Theme.edgeColor
                        }
                    }
                }
            }
        }
    }

    CameraController {
        id: cameraInput
        anchors.fill: parent
        pivot: pivot
        camera: camera
        onDistanceChanged: root.updateClipping()
    }

    ViewCube {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        visible: root.viewCubeVisible
        pitch: cameraInput.pitch
        yaw: cameraInput.yaw
        orthographic: root.orthographic
        onViewRequested: direction => root.setViewDirection(direction)
        onHomeRequested: root.goHome()
        onOrbitRequested: (dx, dy) => cameraInput.orbit(dx, dy)
        onSetHomeRequested: root.setCurrentViewAsHome()
        onResetHomeRequested: root.resetHome()
        onProjectionRequested: ortho => root.orthographic = ortho
    }

    NavigationBar {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 8
        visible: root.navigationBarVisible
        mode: cameraInput.leftDragMode
        orthographic: root.orthographic
        onModeSelected: mode => cameraInput.leftDragMode = mode
        onFitRequested: root.fitAll()
        onHomeRequested: root.goHome()
        onProjectionRequested: ortho => root.orthographic = ortho
        gridVisible: root.gridVisible
        onGridToggled: shown => root.gridVisible = shown
    }

    // Right-click: select the part under the cursor and open its context menu.
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: !root.sketch
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: eventPoint => {
            const hit = root.pickPart(eventPoint.position.x, eventPoint.position.y)
            const id = hit ? hit.partId : 0
            if (id)
                root.partPicked(id)
            root.partContextRequested(id, eventPoint.position.x, eventPoint.position.y)
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: eventPoint => {
            // Mesh ray cast: Qt Quick 3D picks custom geometry by its bounding volume only.
            const hit = root.pickPart(eventPoint.position.x, eventPoint.position.y)
            const id = hit ? hit.partId : 0
            root.partPicked(id)
            if (!hit)
                return
            if (root.tool === "region") {
                root.pickRegion(hit)
            } else if (root.tool === "brush") {
                root.ensureBrushRadius(id)
                if (root.controller.brushBegin(id, root.brushRadius, root.brushConnected, root.brushStrokeMode())) {
                    root.controller.brushAdd(root.modelPoint(hit))
                    root.controller.brushEnd()
                }
            } else {
                root.pointPicked(id, root.modelPoint(hit))
            }
        }
        onDoubleTapped: if (root.tool === "navigate") root.fitAll()
    }

    // Brush strokes and lasso drawing (left drag while a selection tool is active).
    DragHandler {
        id: selectionDrag
        target: null
        // Off while a navigation bar tool (orbit / pan / zoom) owns the left button.
        enabled: (root.tool === "brush" || root.tool === "lasso") && root.leftDragMode === ""
        acceptedButtons: Qt.LeftButton
        acceptedModifiers: Qt.NoModifier
        property var strokePartId: 0
        property vector3d lastPoint
        property bool hasLastPoint: false

        function brushAt(x, y) {
            const hit = root.pickPart(x, y)
            if (!hit || hit.partId !== strokePartId)
                return
            const p = root.modelPoint(hit)
            // Dabs closer than a quarter of the radius add nothing visible.
            if (hasLastPoint && p.minus(lastPoint).length() < root.brushRadius * 0.25)
                return
            root.controller.brushAdd(p)
            lastPoint = p
            hasLastPoint = true
        }

        onActiveChanged: {
            if (root.tool === "lasso") {
                if (active) {
                    lassoCanvas.points = [centroid.pressPosition]
                } else {
                    root.finishLasso(lassoCanvas.points)
                    lassoCanvas.points = []
                }
                lassoCanvas.requestPaint()
                return
            }
            if (active) {
                hasLastPoint = false
                lastScreen = centroid.pressPosition
                const hit = root.pickPart(centroid.pressPosition.x, centroid.pressPosition.y)
                strokePartId = hit ? hit.partId : 0
                if (!strokePartId)
                    return
                root.partPicked(strokePartId)
                root.ensureBrushRadius(strokePartId)
                if (root.controller.brushBegin(strokePartId, root.brushRadius, root.brushConnected, root.brushStrokeMode()))
                    brushAt(centroid.pressPosition.x, centroid.pressPosition.y)
            } else if (root.controller.brushActive()) {
                root.controller.brushEnd()
            }
        }
        property point lastScreen

        onCentroidChanged: {
            if (!active)
                return
            const pos = centroid.position
            if (root.tool === "lasso") {
                lassoCanvas.points.push(pos)
                lassoCanvas.requestPaint()
            } else if (root.controller.brushActive()) {
                // Fill the gap between pointer events so fast strokes stay continuous.
                const dx = pos.x - lastScreen.x, dy = pos.y - lastScreen.y
                const steps = Math.min(64, Math.max(1, Math.ceil(Math.hypot(dx, dy) / 4)))
                for (let i = 1; i <= steps; ++i)
                    brushAt(lastScreen.x + dx * i / steps, lastScreen.y + dy * i / steps)
            }
            lastScreen = pos
        }
    }

    // Brush outline under the pointer, sized from the surface depth.
    HoverHandler {
        id: brushHover
        enabled: root.tool === "brush"
        onPointChanged: {
            const hit = root.pickPart(point.position.x, point.position.y)
            if (!hit || root.brushRadius <= 0) {
                brushCursor.visible = false
                return
            }
            const worldPerPixel = root.orthographic
                ? 1 / orthoCamera.verticalMagnification
                : 2 * hit.distance * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, view.height)
            brushCursor.radius = root.brushRadius / worldPerPixel
            brushCursor.x = point.position.x - brushCursor.radius
            brushCursor.y = point.position.y - brushCursor.radius
            brushCursor.visible = true
        }
        onHoveredChanged: if (!hovered) brushCursor.visible = false
    }

    Rectangle {
        id: brushCursor
        visible: false
        width: radius * 2
        height: radius * 2
        color: "transparent"
        border.color: root.selectMode === "subtract" ? "#ff5a5a" : "#ffb066"
        border.width: 1.5
    }

    Canvas {
        id: lassoCanvas
        anchors.fill: parent
        property var points: []
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (points.length < 2)
                return
            ctx.strokeStyle = "#ffb066"
            ctx.fillStyle = "rgba(255, 154, 60, 0.12)"
            ctx.lineWidth = 1.5
            ctx.setLineDash([4, 3])
            ctx.beginPath()
            ctx.moveTo(points[0].x, points[0].y)
            for (let i = 1; i < points.length; ++i)
                ctx.lineTo(points[i].x, points[i].y)
            ctx.closePath()
            ctx.fill()
            ctx.stroke()
        }
    }

    SelectionToolbar {
        viewport: root
        visible: root.selectionToolsAvailable
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
    }

    // Not while sketching: digits are typed into sketch dimensions there. Esc is handled
    // by the window (it also closes dialogs and sketches) and calls leaveSelectionTool().
    Shortcut { sequence: "1"; enabled: root.selectionToolsAvailable; onActivated: root.tool = "region" }
    Shortcut { sequence: "2"; enabled: root.selectionToolsAvailable; onActivated: root.tool = "lasso" }
    Shortcut { sequence: "3"; enabled: root.selectionToolsAvailable; onActivated: root.tool = "brush" }
    Shortcut {
        sequence: "["
        enabled: root.tool === "brush"
        onActivated: root.brushRadius = Math.max(0.05, Number((root.brushRadius / 1.25).toPrecision(2)))
    }
    Shortcut {
        sequence: "]"
        enabled: root.tool === "brush"
        onActivated: root.brushRadius = Number((Math.max(root.brushRadius, 0.04) * 1.25).toPrecision(2))
    }

    /// Model-space point under a view position on the plane (origin, normal), or null.
    function planePoint(x, y, origin, normal) {
        const a = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 0)))
        const b = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 100)))
        const d = b.minus(a)
        const denom = d.dotProduct(normal)
        if (Math.abs(denom) < 1e-12)
            return null
        const t = origin.minus(a).dotProduct(normal) / denom
        return a.plus(d.times(t))
    }

    /// View position of a model-space point.
    function viewPoint(p) {
        return view.mapFrom3DScene(modelSpace.mapPositionToScene(p))
    }

    // Selection markers, re-projected whenever the camera moves.
    Repeater {
        model: {
            cameraInput.pitch; cameraInput.yaw; cameraInput.distance; pivot.position; root.width; root.height
            root.orthographic
            return root.markers.map(p => root.viewPoint(Qt.vector3d(p[0], p[1], p[2])))
        }
        delegate: Rectangle {
            required property var modelData
            x: modelData.x - width / 2
            y: modelData.y - height / 2
            width: 10
            height: 10
            radius: 5
            color: "#ff8a00"
            border.color: "white"
            border.width: 1.5
        }
    }

    SketchOverlay {
        id: sketchOverlay
        anchors.fill: parent
        viewport: root
        sketch: root.sketch
        tool: root.sketchTool
        snapPoints: root.sketchSnapPoints
        sketchCurves: root.sketchCurves
        sketchConstraints: root.sketchConstraints
        degreesOfFreedom: root.sketchDegreesOfFreedom
        onCurveDrawn: curve => root.curveDrawn(curve)
        onConstraintAdded: c => root.constraintAdded(c)
        onConstraintEdited: (i, v) => root.constraintEdited(i, v)
        onConstraintRemoved: i => root.constraintRemoved(i)
        onToolRequested: t => root.sketchToolRequested(t)
    }

    Connections {
        target: root.controller
        function onBoundsChanged() { root.updateClipping() }
    }
}
