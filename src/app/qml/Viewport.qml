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

    property bool fitPending: false

    // Region selection tools (see SelectionToolbar.qml). Keys: 1 smooth region (click),
    // 2 lasso, 3 brush, Esc navigate; [ and ] change the brush size.
    property string tool: "navigate" // navigate | region | lasso | brush
    property string selectMode: "replace" // replace | add | subtract
    property real regionAngle: 20 // degrees between neighbouring triangles
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

    /// Hit on a part at viewport position (x, y), or null.
    function pickPart(x, y) {
        const hit = view.pick(x, y)
        return hit.objectHit && hit.objectHit.pickPartId ? hit : null
    }

    function modelPoint(hit) {
        const p = modelSpace.mapPositionFromScene(hit.scenePosition)
        return Qt.vector3d(p.x, p.y, p.z)
    }

    function ensureBrushRadius(id) {
        if (brushRadius > 0)
            return
        const info = controller.partInfo(id)
        brushRadius = Math.max(0.1, Number(((info.diagonal ?? 100) * 0.03).toPrecision(2)))
    }

    function brushStrokeMode() {
        return selectMode === "subtract" ? "subtract" : "add"
    }

    function pickRegion(hit) {
        const p = modelPoint(hit)
        controller.execute("selection.pick_region", {
            id: hit.objectHit.pickPartId,
            point: [p.x, p.y, p.z],
            maxAngle: regionAngle,
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
        const eye = modelSpace.mapPositionFromScene(camera.scenePosition)
        const forward = modelSpace.mapDirectionFromScene(camera.forward)
        const up = modelSpace.mapDirectionFromScene(camera.up)
        controller.execute("selection.lasso", {
            id: id,
            polygon: points.map(p => [p.x / view.width, p.y / view.height]),
            view: {
                eye: [eye.x, eye.y, eye.z],
                forward: [forward.x, forward.y, forward.z],
                up: [up.x, up.y, up.z],
                fov: camera.fieldOfView,
                aspect: view.width / Math.max(1, view.height)
            },
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
                            baseColor: "#ff9a3c"
                            emissiveFactor: Qt.vector3d(0.35, 0.16, 0.0)
                            metalness: 0.0
                            roughness: 0.6
                            cullMode: Material.NoCulling
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
        leftOrbit: root.tool === "navigate"
        onDistanceChanged: root.updateClipping()
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: eventPoint => {
            const hit = root.pickPart(eventPoint.position.x, eventPoint.position.y)
            const id = hit ? hit.objectHit.pickPartId : 0
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
            }
        }
        onDoubleTapped: if (root.tool === "navigate") root.fitAll()
    }

    // Brush strokes and lasso drawing (left drag while a selection tool is active).
    DragHandler {
        id: selectionDrag
        target: null
        enabled: root.tool === "brush" || root.tool === "lasso"
        acceptedButtons: Qt.LeftButton
        acceptedModifiers: Qt.NoModifier
        property var strokePartId: 0
        property vector3d lastPoint
        property bool hasLastPoint: false

        function brushAt(x, y) {
            const hit = root.pickPart(x, y)
            if (!hit || hit.objectHit.pickPartId !== strokePartId)
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
                const hit = root.pickPart(centroid.pressPosition.x, centroid.pressPosition.y)
                strokePartId = hit ? hit.objectHit.pickPartId : 0
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
        onCentroidChanged: {
            if (!active)
                return
            if (root.tool === "lasso") {
                lassoCanvas.points.push(centroid.position)
                lassoCanvas.requestPaint()
            } else if (root.controller.brushActive()) {
                brushAt(centroid.position.x, centroid.position.y)
            }
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
            const worldPerPixel = 2 * hit.distance * Math.tan(camera.fieldOfView * Math.PI / 360) / Math.max(1, view.height)
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
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
    }

    Shortcut { sequence: "1"; onActivated: root.tool = "region" }
    Shortcut { sequence: "2"; onActivated: root.tool = "lasso" }
    Shortcut { sequence: "3"; onActivated: root.tool = "brush" }
    Shortcut { sequence: "Esc"; onActivated: root.tool = "navigate" }
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

    Connections {
        target: root.controller
        function onBoundsChanged() { root.updateClipping() }
    }
}
