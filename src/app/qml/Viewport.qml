// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick3D
import AeroCAD

Item {
    id: root

    required property AppController controller
    property var selectedPartId: 0
    signal partPicked(var id)
    /// Left click on a part: its id and the hit point in model coordinates (mm, Z up).
    signal pointPicked(var id, vector3d point)

    /// Active sketch ({id, origin, normal, xDir, yDir} in model space) or null; while set,
    /// left clicks draw with `sketchTool` instead of selecting.
    property var sketch: null
    property string sketchTool: ""
    property var sketchSnapPoints: []
    signal curveDrawn(var curve)
    /// Model-space points to mark (selections of the open feature dialog).
    property var markers: []
    readonly property alias sketchOverlay: sketchOverlay

    property bool fitPending: false

    /// Camera projection (Fusion ViewCube menu: Orthographic / Perspective).
    property bool orthographic: false
    property bool viewCubeVisible: true
    property bool navigationBarVisible: true
    /// Home view orientation; "Set current view as Home" changes it, "Reset Home" restores it.
    readonly property real defaultHomePitch: -30
    readonly property real defaultHomeYaw: 45
    property real homePitch: defaultHomePitch
    property real homeYaw: defaultHomeYaw
    readonly property alias pitch: cameraInput.pitch
    readonly property alias yaw: cameraInput.yaw
    property alias leftDragMode: cameraInput.leftDragMode

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
            // Light top-to-bottom gradient, as in Fusion and most CAD applications.
            GradientStop { position: 0.0; color: "#fbfcfd" }
            GradientStop { position: 1.0; color: "#c3cbd4" }
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

            Repeater3D {
                model: root.controller.parts
                delegate: Model {
                    id: partModel
                    required property var partId
                    required property bool partVisible
                    required property string kind
                    property var pickPartId: partId
                    readonly property bool selected: partId === root.selectedPartId

                    visible: partVisible
                    pickable: true
                    geometry: MeshGeometry {
                        controller: root.controller
                        partId: partModel.partId
                    }
                    // Fusion-like colours: grey bodies, translucent orange sketch profiles.
                    materials: PrincipledMaterial {
                        baseColor: partModel.selected ? "#7fb2ff"
                                 : partModel.kind === "sketch" ? Qt.rgba(1.0, 0.78, 0.45, 0.45)
                                 : partModel.kind === "body" ? "#b8bec6" : "#c8ccd2"
                        alphaMode: partModel.kind === "sketch" ? PrincipledMaterial.Blend : PrincipledMaterial.Default
                        metalness: 0.0
                        roughness: 0.55
                        cullMode: Material.NoCulling
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
                            baseColor: partModel.kind === "sketch" ? "#1f5fbf" : "#262a30"
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
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: eventPoint => {
            const hit = view.pick(eventPoint.position.x, eventPoint.position.y)
            const id = hit.objectHit ? hit.objectHit.pickPartId : 0
            root.partPicked(id)
            if (id)
                root.pointPicked(id, modelSpace.mapPositionFromScene(hit.scenePosition))
        }
        onDoubleTapped: root.fitAll()
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
        onCurveDrawn: curve => root.curveDrawn(curve)
    }

    Connections {
        target: root.controller
        function onBoundsChanged() { root.updateClipping() }
    }
}
