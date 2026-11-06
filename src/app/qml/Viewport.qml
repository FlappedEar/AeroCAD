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

    function setView(pitch, yaw) {
        cameraInput.pitch = pitch
        cameraInput.yaw = yaw
        fitAll()
    }

    function updateClipping() {
        const r = Math.max(controller.sceneRadius, 1)
        camera.clipFar = cameraInput.distance + r * 4
        camera.clipNear = Math.max(cameraInput.distance - r * 2, cameraInput.distance * 0.001, 0.001)
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#3b4452" }
            GradientStop { position: 1.0; color: "#15181d" }
        }
    }

    View3D {
        id: view
        anchors.fill: parent
        camera: camera

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
