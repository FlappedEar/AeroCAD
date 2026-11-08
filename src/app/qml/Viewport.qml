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
    /// A left click went into a hole (or just past an edge) and was moved to the nearest
    /// surface; emitted before partPicked / pointPicked for that click.
    signal snappedToNearestSurface()
    /// Left clicks that miss the model (a hole in a scan) take the nearest surface instead,
    /// within a few degrees of the click: for choosing where to sketch.
    property bool snapToNearestSurface: false

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
    /// Only parts of this kind ("body", "sketch") can be clicked; "" = all. "solid": bodies
    /// and imported exact geometry (fins, fitted solids, STEP parts).
    property string pickKind: ""
    /// Clicks also pick sketch lines (Measure).
    property bool pickSketchLines: false
    /// Sketch feature shown even when hidden (the sketch a feature dialog works on).
    property var shownSketch: -1
    /// Construction plane that clicks pass through (the one a dialog is creating).
    property var unpickablePlane: -1
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
    readonly property alias transformGizmo: transformGizmo
    /// Simulation result fields to draw: [{run, kind, scalar, min, max}] (SimulationPanel).
    property var simulationFields: []
    /// Parts not drawn (e.g. while their pressure map is shown in their place).
    property var hiddenPartIds: []

    property bool fitPending: false

    /// Camera projection (Fusion ViewCube menu: Orthographic / Perspective).
    property bool orthographic: false
    onOrthographicChanged: cameraInput.invalidateZoomAnchor()
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
    /// Changes whenever the camera or the view size does: overlays drawn from projected
    /// points repaint on it.
    readonly property var viewKey: [cameraInput.pitch, cameraInput.yaw, cameraInput.distance, pivot.position,
                                    width, height, orthographic]
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
        const sceneNear = view.mapTo3DScene(Qt.vector3d(x, y, 0))
        const sceneFar = view.mapTo3DScene(Qt.vector3d(x, y, 100))
        const near = modelSpace.mapPositionFromScene(sceneNear)
        const far = modelSpace.mapPositionFromScene(sceneFar)
        const eye = modelSpace.mapPositionFromScene(view.camera.scenePosition)
        const node = transformActive ? previewNode(transformTarget.partId) : null
        if (!node) {
            const hit = controller.pickSurface(near, far.minus(near))
            if (!hit.partId)
                return null
            return { partId: hit.partId, point: hit.point, distance: hit.point.minus(eye).length() }
        }
        // Move / Rotate / Scale running: pick the moved part where it is shown (the ray taken
        // back into its own, unmoved coordinates), the other parts as they are.
        let best = null
        const others = controller.pickSurface(near, far.minus(near), transformTarget.partId, 0)
        if (others.partId)
            best = { partId: others.partId, point: others.point, distance: others.point.minus(eye).length() }
        const localNear = node.mapPositionFromScene(sceneNear)
        const localFar = node.mapPositionFromScene(sceneFar)
        const own = controller.pickSurface(localNear, localFar.minus(localNear), 0, transformTarget.partId)
        if (own.partId) {
            const p = modelSpace.mapPositionFromScene(node.mapPositionToScene(own.point))
            const d = p.minus(eye).length()
            if (!best || d < best.distance)
                best = { partId: own.partId, point: p, distance: d }
        }
        return best
    }

    /// Scene position under view position (x, y) for zooming and orbiting: the model
    /// surface, else (unless onModelOnly) the point at the camera pivot's depth; or null.
    function scenePointUnder(x, y, onModelOnly) {
        const hit = pickPart(x, y)
        if (hit)
            return modelSpace.mapPositionToScene(hit.point)
        if (onModelOnly)
            return null
        const near = view.mapTo3DScene(Qt.vector3d(x, y, 0))
        const dir = view.mapTo3DScene(Qt.vector3d(x, y, 100)).minus(near)
        const forward = view.camera.forward
        const along = dir.dotProduct(forward)
        if (Math.abs(along) < 1e-12)
            return null
        return near.plus(dir.times(pivot.position.minus(near).dotProduct(forward) / along))
    }

    /// Nearest surface to view position (x, y) for a click that misses, like pickPart, or null.
    function pickNearPart(x, y) {
        const near = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 0)))
        const far = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 100)))
        const eye = modelSpace.mapPositionFromScene(view.camera.scenePosition)
        // The angle is measured from the eye: `near` is on the near clipping plane, which
        // moves towards the model as the camera backs off. A parallel view has no eye; one
        // is placed behind the view as far as a perspective camera would see its height.
        let from = eye
        if (root.orthographic) {
            const dir = far.minus(near).normalized()
            const h = view.height / orthoCamera.verticalMagnification
            from = near.minus(dir.times(h / 2 / Math.tan(camera.fieldOfView * Math.PI / 360)))
        }
        const hit = controller.pickNearSurface(from, far.minus(from), 4)
        if (!hit.partId)
            return null
        return { partId: hit.partId, point: hit.point, distance: hit.point.minus(eye).length(), snapped: true }
    }

    /// Sketch line within the pick radius of view position (x, y), like pickPart, or null.
    function pickSketchLine(x, y) {
        const near = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 0)))
        const far = modelSpace.mapPositionFromScene(view.mapTo3DScene(Qt.vector3d(x, y, 100)))
        const hit = controller.pickSketchLine(near, far.minus(near), root.pickTolerance())
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

    /// Frames the area of `radius` (mm) around a model point (a place being worked on).
    function frameAround(point, radius) {
        // Model (x, y, z) is (x, z, -y) in the Y-up scene.
        cameraInput.pivot.position = Qt.vector3d(point[0], point[2], -point[1])
        const halfFov = camera.fieldOfView * Math.PI / 360
        cameraInput.distance = Math.max(radius, 1) / Math.sin(halfFov) * 1.1
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
                id: partRepeater
                model: root.controller.parts
                delegate: Model {
                    id: partModel
                    required property var partId
                    required property bool partVisible
                    required property string kind
                    required property var featureId
                    required property bool hasBrep
                    property var pickPartId: partId
                    readonly property bool selected: partId === root.selectedPartId
                    // Inspect: section view (cut + cap) and heat map replace the plain mesh.
                    readonly property bool sectioned: root.controller.inspectRevision >= 0
                                                      && root.controller.sectionCuts(partId)
                    readonly property bool heatMapped: root.controller.inspectRevision >= 0
                                                       && root.controller.showsHeatMap(partId) && !heatGeometry.empty
                    readonly property color capColor: kind === "body" ? "#d2584a" : "#3b82c4"
                    // Live preview of the Move / Rotate / Scale tool: p' = s·R(p − C) + C + T.
                    readonly property bool transforming: !!root.transformTarget && root.transformTarget.partId === partId
                    pivot: transforming ? root.transformTarget.center : Qt.vector3d(0, 0, 0)
                    position: transforming ? root.transformTarget.center.plus(root.transformMove) : Qt.vector3d(0, 0, 0)
                    rotation: transforming ? root.transformRotation : Qt.quaternion(1, 0, 0, 0)
                    scale: transforming ? Qt.vector3d(root.transformScale, root.transformScale, root.transformScale)
                                        : Qt.vector3d(1, 1, 1)

                    readonly property MeshGeometry plainGeometry: MeshGeometry {
                        controller: root.controller
                        partId: partModel.partId
                    }
                    readonly property InspectGeometry clippedGeometry: InspectGeometry {
                        controller: root.controller
                        partId: partModel.sectioned ? partModel.partId : 0
                        mode: InspectGeometry.Clipped
                    }
                    readonly property InspectGeometry heatGeometry: InspectGeometry {
                        controller: root.controller
                        partId: partModel.partId
                        mode: InspectGeometry.HeatMap
                    }

                    visible: (partVisible || (kind === "sketch" && featureId == root.shownSketch))
                             && root.hiddenPartIds.indexOf(partId) < 0
                    pickable: (root.pickKind === "" || kind === root.pickKind
                               || (root.pickKind === "solid" && (kind === "body" || (kind === "imported" && hasBrep))))
                              && !(kind === "plane" && featureId == root.unpickablePlane)
                    geometry: sectioned ? clippedGeometry : heatMapped ? heatGeometry : plainGeometry
                    // Fusion-like colours: grey bodies, translucent orange sketch profiles and planes.
                    materials: heatMapped && !sectioned ? [heatMaterial] : [plainMaterial]
                    PrincipledMaterial {
                        id: plainMaterial
                        baseColor: partModel.selected ? (partModel.kind === "plane" ? Theme.planeSelected : Theme.selectedColor)
                                 : partModel.kind === "sketch" ? Theme.sketchProfile
                                 : partModel.kind === "plane" ? Theme.planeFill
                                 : partModel.kind === "body" ? Theme.bodyColor : Theme.meshColor
                        alphaMode: partModel.kind === "sketch" || partModel.kind === "plane" ? PrincipledMaterial.Blend : PrincipledMaterial.Default
                        metalness: 0.0
                        roughness: 0.45
                        specularAmount: 0.35
                        // In a section the inside is drawn by the model below.
                        cullMode: partModel.sectioned ? Material.BackFaceCulling : Material.NoCulling
                    }
                    DefaultMaterial {
                        id: heatMaterial
                        vertexColorsEnabled: true
                        diffuseColor: "white"
                        specularAmount: 0.1
                        cullMode: Material.NoCulling
                    }

                    // Region selection highlight.
                    Model {
                        // Hidden in a section view: the highlight is not clipped.
                        visible: !selectionGeometry.empty && !partModel.sectioned
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


                    // Section: the inside of the cut part (back faces) in the cap colour, so
                    // open meshes such as scans still read as cut material.
                    Model {
                        visible: partModel.sectioned && !partModel.clippedGeometry.empty
                        pickable: false
                        geometry: partModel.clippedGeometry
                        materials: PrincipledMaterial {
                            lighting: PrincipledMaterial.NoLighting
                            baseColor: Qt.darker(partModel.capColor, 1.35)
                            cullMode: Material.FrontFaceCulling
                        }
                    }
                    // Section cap (closed section loops).
                    Model {
                        visible: partModel.sectioned && !capGeometry.empty
                        pickable: false
                        geometry: InspectGeometry {
                            id: capGeometry
                            controller: root.controller
                            partId: partModel.sectioned ? partModel.partId : 0
                            mode: InspectGeometry.Cap
                        }
                        materials: PrincipledMaterial {
                            baseColor: partModel.capColor
                            roughness: 0.8
                            cullMode: Material.NoCulling
                        }
                    }
                    // Section curves.
                    Model {
                        visible: partModel.sectioned && !outlineGeometry.empty
                        pickable: false
                        geometry: InspectGeometry {
                            id: outlineGeometry
                            controller: root.controller
                            partId: partModel.sectioned ? partModel.partId : 0
                            mode: InspectGeometry.Outline
                        }
                        materials: PrincipledMaterial {
                            lighting: PrincipledMaterial.NoLighting
                            baseColor: Qt.darker(partModel.capColor, 2.2)
                        }
                    }

                    // Sketch curves and body edges (hidden in a section: they are not clipped).
                    Model {
                        visible: !lines.empty && !partModel.sectioned
                        pickable: false
                        geometry: LineGeometry {
                            id: lines
                            controller: root.controller
                            partId: partModel.partId
                        }
                        materials: PrincipledMaterial {
                            lighting: PrincipledMaterial.NoLighting
                            baseColor: partModel.kind === "sketch" ? Theme.sketchColor
                                     : partModel.kind === "plane" ? Theme.planeEdge : Theme.edgeColor
                        }
                    }
                }
            }

            // Simulation results, coloured per vertex and unlit so colours match the legend.
            Repeater3D {
                model: root.simulationFields
                delegate: Model {
                    required property var modelData
                    pickable: false
                    geometry: FieldGeometry {
                        controller: root.controller
                        run: modelData.run
                        kind: modelData.kind
                        scalar: modelData.scalar
                        rangeMin: modelData.min
                        rangeMax: modelData.max
                    }
                    materials: DefaultMaterial {
                        vertexColorsEnabled: true
                        lighting: DefaultMaterial.NoLighting
                        cullMode: Material.NoCulling
                        lineWidth: 2
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
        pointUnder: (x, y, onModelOnly) => root.scenePointUnder(x, y, onModelOnly)
        // Right-click: select the part under the cursor and open its context menu.
        contextMenuEnabled: !root.sketch
        onContextClicked: (x, y) => {
            const hit = root.pickPart(x, y)
            const id = hit ? hit.partId : 0
            if (id)
                root.partPicked(id)
            root.partContextRequested(id, x, y)
        }
        onDistanceChanged: root.updateClipping()
    }

    // Move / Rotate / Scale handles; above the navigation, below the view's own controls.
    TransformGizmo {
        id: transformGizmo
        objectName: "transformGizmo"
        anchors.fill: parent
        viewport: root
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
        onStepRequested: (dPitch, dYaw) => cameraInput.stepView(dPitch, dYaw)
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

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: eventPoint => {
            // Mesh ray cast: Qt Quick 3D picks custom geometry by its bounding volume only.
            let hit = root.pickPart(eventPoint.position.x, eventPoint.position.y)
            if (root.pickSketchLines) {
                // Sketch lines have no surface: take one within the pick radius unless a
                // surface is clearly in front of it.
                const line = root.pickSketchLine(eventPoint.position.x, eventPoint.position.y)
                if (line && (!hit || line.distance <= hit.distance + root.pickTolerance()))
                    hit = line
            }
            if (!hit && root.snapToNearestSurface) {
                hit = root.pickNearPart(eventPoint.position.x, eventPoint.position.y)
                if (hit)
                    root.snappedToNearestSurface()
            }
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

    // --- Move / Rotate / Scale tool (Fusion: Move/Copy, M) --------------------------------
    // The part follows the manipulator (TransformGizmo) and the panel live, as a transform of
    // its 3D node; finishTransform(true) applies it as one command (one undo step):
    // feature.move for a timeline body, part.transform for everything else.

    /// Part being moved: {partId, kind, featureId, center (model mm), name, canScale} while the
    /// tool is on, or null. (Not "transform": that is Item.transform.)
    property var transformTarget: null
    property vector3d transformMove: Qt.vector3d(0, 0, 0)
    property quaternion transformRotation: Qt.quaternion(1, 0, 0, 0)
    property real transformScale: 1
    readonly property bool transformActive: !!transformTarget
    /// After the tool applied (committed) or was cancelled.
    signal transformFinished(bool committed)

    /// Starts the tool on a part (partInfo). Returns false for parts that cannot be moved.
    function startTransform(info) {
        if (transformTarget)
            finishTransform(true)
        if (!info || !info.id || info.kind === "sketch" || info.kind === "plane"
                || !info.center || info.center.length !== 3)
            return false
        transformMove = Qt.vector3d(0, 0, 0)
        transformRotation = Qt.quaternion(1, 0, 0, 0)
        transformScale = 1
        transformTarget = { partId: info.id, kind: info.kind, featureId: info.featureId,
                      center: Qt.vector3d(info.center[0], info.center[1], info.center[2]),
                      name: info.name || "", canScale: true }
        return true
    }

    function resetTransform() {
        transformMove = Qt.vector3d(0, 0, 0)
        transformRotation = Qt.quaternion(1, 0, 0, 0)
        transformScale = 1
    }

    /// Command and parameters that apply the current preview, or null when nothing changed.
    function transformCommand() {
        const t = transformTarget
        if (!t)
            return null
        const q = transformRotation
        const w = Math.max(-1, Math.min(1, q.scalar))
        const sinHalf = Math.sqrt(Math.max(0, 1 - w * w))
        let angle = 2 * Math.acos(Math.abs(w)) * 180 / Math.PI
        const sign = w < 0 ? -1 : 1
        const axis = sinHalf > 1e-9 ? [sign * q.x / sinHalf, sign * q.y / sinHalf, sign * q.z / sinHalf] : [0, 0, 1]
        if (angle < 1e-7)
            angle = 0
        const m = transformMove
        const moved = m.length() > 1e-9
        const scaled = Math.abs(transformScale - 1) > 1e-9
        if (!moved && !angle && !scaled)
            return null
        const params = { center: [t.center.x, t.center.y, t.center.z] }
        if (t.kind === "body") {
            // Always stored, so editing the step later shows this rotation (0 when it only
            // moves or scales) instead of the Move dialog's defaults; X / Y / Z by name.
            const named = ["X", "Y", "Z"].find((n, i) => Math.abs(Math.abs(axis[i]) - 1) < 1e-9)
            const i = named ? ["X", "Y", "Z"].indexOf(named) : -1
            params.axis = !angle ? "Z" : named ? named : axis
            params.angle = named && axis[i] < 0 ? -angle : angle
        } else if (angle) {
            params.axis = axis
            params.angle = angle
        }
        if (scaled)
            params.scale = transformScale
        if (moved)
            params.translate = [m.x, m.y, m.z]
        if (t.kind === "body") {
            params.body = t.featureId
            return { command: "feature.move", params: params }
        }
        params.id = t.partId
        return { command: "part.transform", params: params }
    }

    // Any other command that changes, saves, exports or analyses the document (from the UI or
    // the AI agent) applies the preview first, so it never works on geometry other than what
    // is shown; undo, redo and a new or opened project drop it instead. Listing queries the
    // panels make while the tool runs leave it alone.
    readonly property var transformKeepsFor: ["document.parts", "document.bounds", "timeline.list", "sketch.profiles",
        "sketch.curves", "mesh.stats", "simulation.list", "simulation.results", "simulation.field",
        "simulation.environment", "simulation.status", "project.info", "project.get_context", "edit.history",
        "inspect.state", "inspect.dimensions", "selection.get", "parameter.list"]
    Connections {
        target: root.controller
        enabled: root.transformActive
        function onCommandAboutToRun(command, modifiesDocument) {
            if (["edit.undo", "edit.redo", "project.new", "project.open", "project.recover"].includes(command))
                root.finishTransform(false)
            else if (modifiesDocument || !root.transformKeepsFor.includes(command))
                root.finishTransform(true)
        }
    }

    /// Ends the tool: commit applies the preview as one command, otherwise the part goes back.
    function finishTransform(commit) {
        if (!transformTarget)
            return null
        const c = commit ? transformCommand() : null
        transformTarget = null
        resetTransform()
        const r = c ? controller.execute(c.command, c.params) : null
        transformFinished(!!commit)
        return r
    }

    function quaternionMultiply(a, b) {
        return Qt.quaternion(a.scalar * b.scalar - a.x * b.x - a.y * b.y - a.z * b.z,
                             a.scalar * b.x + a.x * b.scalar + a.y * b.z - a.z * b.y,
                             a.scalar * b.y - a.x * b.z + a.y * b.scalar + a.z * b.x,
                             a.scalar * b.z + a.x * b.y - a.y * b.x + a.z * b.scalar)
    }

    function axisAngleQuaternion(axis, degrees) {
        const a = axis.normalized()
        const h = degrees * Math.PI / 360
        return Qt.quaternion(Math.cos(h), a.x * Math.sin(h), a.y * Math.sin(h), a.z * Math.sin(h))
    }

    /// Turns the preview further about a model axis through the part centre.
    function rotateTransform(axis, degrees) {
        if (Math.abs(degrees) < 1e-12)
            return
        const q = quaternionMultiply(axisAngleQuaternion(axis, degrees), transformRotation)
        const n = Math.hypot(q.scalar, q.x, q.y, q.z)
        transformRotation = Qt.quaternion(q.scalar / n, q.x / n, q.y / n, q.z / n)
    }

    /// Angles (degrees) about the X, then Y, then Z model axis: R = Rz·Ry·Rx.
    function quaternionToEuler(q) {
        const w = q.scalar, x = q.x, y = q.y, z = q.z
        const r20 = 2 * (x * z - w * y)
        const deg = 180 / Math.PI
        if (Math.abs(r20) > 1 - 1e-9) {
            const r01 = 2 * (x * y - w * z), r11 = 1 - 2 * (x * x + z * z)
            return Qt.vector3d(0, (r20 < 0 ? 90 : -90), Math.atan2(-r01, r11) * deg)
        }
        const r21 = 2 * (y * z + w * x), r22 = 1 - 2 * (x * x + y * y)
        const r10 = 2 * (x * y + w * z), r00 = 1 - 2 * (y * y + z * z)
        return Qt.vector3d(Math.atan2(r21, r22) * deg, Math.asin(-r20) * deg, Math.atan2(r10, r00) * deg)
    }

    function eulerToQuaternion(e) {
        return quaternionMultiply(axisAngleQuaternion(Qt.vector3d(0, 0, 1), e.z),
                                  quaternionMultiply(axisAngleQuaternion(Qt.vector3d(0, 1, 0), e.y),
                                                     axisAngleQuaternion(Qt.vector3d(1, 0, 0), e.x)))
    }

    /// View position (x, y) of a model-space point.
    function modelToScreen(p) {
        const v = view.mapFrom3DScene(modelSpace.mapPositionToScene(p))
        return Qt.point(v.x, v.y)
    }

    /// Model millimetres per screen pixel at a model-space point.
    function worldPerPixelAt(p) {
        if (root.orthographic)
            return 1 / orthoCamera.horizontalMagnification
        const s = modelSpace.mapPositionToScene(p)
        const depth = s.minus(camera.scenePosition).dotProduct(camera.forward)
        const w = 2 * Math.max(depth, camera.clipNear) * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, view.height)
        return isFinite(w) && w > 0 ? w : 1
    }

    /// Whether a model axis at point p points towards the viewer.
    function axisTowardsViewer(axis, p) {
        const a = modelSpace.mapDirectionToScene(axis)
        const s = modelSpace.mapPositionToScene(p)
        const toEye = root.orthographic ? view.camera.forward.times(-1) : view.camera.scenePosition.minus(s)
        return a.dotProduct(toEye) > 0
    }

    /// Where the preview puts a model point of the part being moved (for tests): model mm.
    function previewNode(partId) {
        for (let i = 0; i < partRepeater.count; ++i) {
            const node = partRepeater.objectAt(i)
            if (node && node.partId === partId)
                return node
        }
        return null
    }
    function previewPoint(partId, p) {
        const node = previewNode(partId)
        return node ? modelSpace.mapPositionFromScene(node.mapPositionToScene(p)) : p
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

    /// Measurement lines: [{a: [x, y, z], b: [x, y, z], label: string, color?}] in model space.
    property var overlayLines: []
    /// Pick radius for vertices and edges at the current zoom (mm), ~8 px.
    function pickTolerance() {
        const w = cameraInput.worldPerPixel()
        return isFinite(w) && w > 0 ? w * 8 : 0.5
    }

    Canvas {
        id: overlayCanvas
        anchors.fill: parent
        readonly property var projected: {
            cameraInput.pitch; cameraInput.yaw; cameraInput.distance; pivot.position; root.width; root.height
            root.orthographic
            return root.overlayLines.map(l => ({
                a: root.viewPoint(Qt.vector3d(l.a[0], l.a[1], l.a[2])),
                b: root.viewPoint(Qt.vector3d(l.b[0], l.b[1], l.b[2])),
                label: l.label || "",
                color: l.color || "#ff8a00"
            }))
        }
        onProjectedChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            for (const l of projected) {
                ctx.strokeStyle = l.color
                ctx.lineWidth = 2
                ctx.setLineDash([6, 4])
                ctx.beginPath()
                ctx.moveTo(l.a.x, l.a.y)
                ctx.lineTo(l.b.x, l.b.y)
                ctx.stroke()
                ctx.setLineDash([])
                for (const p of [l.a, l.b]) {
                    ctx.fillStyle = l.color
                    ctx.beginPath()
                    ctx.arc(p.x, p.y, 4.5, 0, 2 * Math.PI)
                    ctx.fill()
                }
                if (l.label) {
                    const mx = (l.a.x + l.b.x) / 2
                    const my = (l.a.y + l.b.y) / 2
                    ctx.font = "bold 13px sans-serif"
                    const w = ctx.measureText(l.label).width + 12
                    ctx.fillStyle = "rgba(255, 255, 255, 0.92)"
                    ctx.strokeStyle = l.color
                    ctx.lineWidth = 1
                    ctx.fillRect(mx - w / 2, my - 22, w, 20)
                    ctx.strokeRect(mx - w / 2, my - 22, w, 20)
                    ctx.fillStyle = "#1d2228"
                    ctx.fillText(l.label, mx - w / 2 + 6, my - 7)
                }
            }
        }
    }

    // Section plane manipulator (Fusion: drag the arrow to move the plane).
    Item {
        id: sectionHandle
        anchors.fill: parent
        readonly property var section: root.controller.inspectRevision >= 0
                                       ? root.controller.inspectInfo().section : ({ enabled: false })
        /// Arrow length in mm: a fifth of the model size.
        readonly property real arrowLength: Math.max(root.controller.sceneRadius * 0.4, 1)
        readonly property var ends: {
            cameraInput.pitch; cameraInput.yaw; cameraInput.distance; pivot.position; root.width; root.height
            root.orthographic
            if (!section.enabled)
                return null
            const o = Qt.vector3d(section.origin[0], section.origin[1], section.origin[2])
            const n = Qt.vector3d(section.normal[0], section.normal[1], section.normal[2])
            return { o: root.viewPoint(o), tip: root.viewPoint(o.plus(n.times(arrowLength))) }
        }
        visible: !!ends
        signal moved(real delta)

        Canvas {
            anchors.fill: parent
            readonly property var ends: sectionHandle.ends
            onEndsChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (!ends)
                    return
                const dx = ends.tip.x - ends.o.x
                const dy = ends.tip.y - ends.o.y
                const len = Math.hypot(dx, dy)
                ctx.strokeStyle = "#1f6fd1"
                ctx.fillStyle = "#1f6fd1"
                ctx.lineWidth = 3
                ctx.beginPath()
                ctx.moveTo(ends.o.x, ends.o.y)
                ctx.lineTo(ends.tip.x, ends.tip.y)
                ctx.stroke()
                if (len > 1) {
                    const ux = dx / len
                    const uy = dy / len
                    ctx.beginPath()
                    ctx.moveTo(ends.tip.x + ux * 12, ends.tip.y + uy * 12)
                    ctx.lineTo(ends.tip.x - uy * 7, ends.tip.y + ux * 7)
                    ctx.lineTo(ends.tip.x + uy * 7, ends.tip.y - ux * 7)
                    ctx.closePath()
                    ctx.fill()
                }
                ctx.beginPath()
                ctx.arc(ends.o.x, ends.o.y, 5, 0, 2 * Math.PI)
                ctx.fill()
            }
        }
        // Grab area around the arrow tip.
        Rectangle {
            visible: !!sectionHandle.ends
            x: sectionHandle.ends ? sectionHandle.ends.tip.x - width / 2 : 0
            y: sectionHandle.ends ? sectionHandle.ends.tip.y - height / 2 : 0
            width: 28
            height: 28
            radius: 14
            color: arrowDrag.active ? "#331f6fd1" : "transparent"
            border.color: "#1f6fd1"
            border.width: 1.5
            DragHandler {
                id: arrowDrag
                target: null
                property point last
                onActiveChanged: last = centroid.scenePosition
                onCentroidChanged: {
                    if (!active || !sectionHandle.ends)
                        return
                    const p = centroid.scenePosition
                    const e = sectionHandle.ends
                    const dx = e.tip.x - e.o.x
                    const dy = e.tip.y - e.o.y
                    const len2 = dx * dx + dy * dy
                    if (len2 < 1)
                        return
                    // Mouse motion along the projected arrow, in arrow lengths.
                    const t = ((p.x - last.x) * dx + (p.y - last.y) * dy) / len2
                    last = p
                    sectionHandle.moved(t * sectionHandle.arrowLength)
                }
            }
        }
    }
    /// Emitted while the section arrow is dragged: move the plane by `delta` mm along its normal.
    signal sectionDragged(real delta)
    Connections {
        target: sectionHandle
        function onMoved(delta) { root.sectionDragged(delta) }
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
