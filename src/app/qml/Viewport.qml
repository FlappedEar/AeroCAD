// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick3D
import AeroCAD

Item {
    id: root

    required property AppController controller
    property var selectedPartId: 0
    signal partPicked(var id)

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
        const r = Math.max(controller.sceneRadius, 1)
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
                    property var pickPartId: partId

                    visible: partVisible
                    pickable: true
                    geometry: MeshGeometry {
                        controller: root.controller
                        partId: partModel.partId
                    }
                    materials: PrincipledMaterial {
                        baseColor: partModel.partId === root.selectedPartId ? "#7fb2ff" : "#c8ccd2"
                        metalness: 0.0
                        roughness: 0.55
                        cullMode: Material.NoCulling
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
            root.partPicked(hit.objectHit ? hit.objectHit.pickPartId : 0)
        }
        onDoubleTapped: root.fitAll()
    }

    Connections {
        target: root.controller
        function onBoundsChanged() { root.updateClipping() }
    }
}
