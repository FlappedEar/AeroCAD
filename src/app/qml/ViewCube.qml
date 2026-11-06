// SPDX-License-Identifier: GPL-3.0-or-later
// ViewCube in the style of Autodesk products (Fusion "ViewCube reference" in Autodesk help):
// click a face for an orthographic standard view, an edge or corner for the view between
// them, drag the cube to orbit, the house icon goes to the Home view, right-click opens the
// ViewCube menu. Directions are emitted in model space (Z up, Front = -Y, Right = +X).
import QtQuick
import QtQuick.Controls
import QtQuick3D
import AeroCAD

Item {
    id: root

    /// Orientation of the main camera (degrees, same convention as CameraController).
    property real pitch: 0
    property real yaw: 0
    property bool orthographic: false

    signal viewRequested(vector3d direction)
    signal homeRequested()
    signal orbitRequested(real dx, real dy)
    signal setHomeRequested()
    signal resetHomeRequested()
    signal projectionRequested(bool orthographic)

    implicitWidth: 130
    implicitHeight: 130

    property int hovered: -1

    // Cube of edge 100: faces are 64 x 64 plates, edges 64 x 18 x 18 bars, corners 18^3 blocks.
    readonly property real half: 50
    readonly property real inner: 32
    readonly property real band: 18
    readonly property real bandCenter: inner + band / 2

    // Face plates: normal, label, and the rotations that turn a #Rectangle (normal +Z, up +Y)
    // to face outwards with upright text (outer Z rotation applied after inner X rotation).
    readonly property var faces: [
        { dir: Qt.vector3d(0, -1, 0), label: qsTr("FRONT"), rz: 0, rx: 90 },
        { dir: Qt.vector3d(0, 1, 0), label: qsTr("BACK"), rz: 180, rx: 90 },
        { dir: Qt.vector3d(1, 0, 0), label: qsTr("RIGHT"), rz: 90, rx: 90 },
        { dir: Qt.vector3d(-1, 0, 0), label: qsTr("LEFT"), rz: -90, rx: 90 },
        { dir: Qt.vector3d(0, 0, 1), label: qsTr("TOP"), rz: 0, rx: 0 },
        { dir: Qt.vector3d(0, 0, -1), label: qsTr("BOTTOM"), rz: 0, rx: 180 }
    ]

    // Edges and corners: every direction with two or three non-zero components.
    readonly property var blocks: {
        const list = []
        for (let x = -1; x <= 1; ++x) {
            for (let y = -1; y <= 1; ++y) {
                for (let z = -1; z <= 1; ++z) {
                    const n = Math.abs(x) + Math.abs(y) + Math.abs(z)
                    if (n < 2)
                        continue
                    const c = root.bandCenter
                    list.push({
                        dir: Qt.vector3d(x, y, z),
                        position: Qt.vector3d(x * c, y * c, z * c),
                        scale: Qt.vector3d(x ? root.band / 100 : 2 * root.inner / 100,
                                           y ? root.band / 100 : 2 * root.inner / 100,
                                           z ? root.band / 100 : 2 * root.inner / 100)
                    })
                }
            }
        }
        return list
    }

    function pieceAt(x, y) {
        const hit = cubeView.pick(x, y)
        return hit.objectHit ? hit.objectHit : null
    }

    View3D {
        id: cubeView
        anchors.fill: parent
        anchors.margins: 6
        camera: cubeCamera
        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        Node {
            eulerRotation: Qt.vector3d(root.pitch, root.yaw, 0)
            OrthographicCamera {
                id: cubeCamera
                z: 400
                clipNear: 1
                clipFar: 1000
                // The cube's diagonal (~173) plus some margin fits the view.
                horizontalMagnification: Math.max(0.01, Math.min(cubeView.width, cubeView.height) / 185)
                verticalMagnification: horizontalMagnification
                DirectionalLight {
                    eulerRotation: Qt.vector3d(-25, -25, 0)
                    brightness: 0.6
                    ambientColor: Qt.rgba(0.75, 0.75, 0.78, 1)
                }
            }
        }

        // Same model-space convention as the main viewport: Z up.
        Node {
            eulerRotation.x: -90

            Repeater3D {
                model: root.faces
                delegate: Node {
                    id: faceNode
                    required property var modelData
                    required property int index
                    eulerRotation.z: modelData.rz
                    Node {
                        eulerRotation.x: faceNode.modelData.rx
                        Model {
                            property vector3d dir: faceNode.modelData.dir
                            property int pieceIndex: faceNode.index
                            source: "#Rectangle"
                            z: root.half
                            scale: Qt.vector3d(2 * root.inner / 100, 2 * root.inner / 100, 1)
                            pickable: true
                            materials: PrincipledMaterial {
                                roughness: 1
                                baseColor: root.hovered === faceNode.index ? Theme.hoverColor : "#ffffff"
                                baseColorMap: Texture {
                                    sourceItem: Rectangle {
                                        width: 128
                                        height: 128
                                        color: Theme.dark ? "#4a5059" : "#f1f3f6"
                                        border.color: Theme.dark ? "#6a717b" : "#a9b1bb"
                                        border.width: 2
                                        Text {
                                            anchors.centerIn: parent
                                            text: faceNode.modelData.label
                                            color: Theme.dark ? "#e7e9ec" : "#3a414b"
                                            font.pixelSize: 26
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 1
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Repeater3D {
                model: root.blocks
                delegate: Model {
                    id: block
                    required property var modelData
                    required property int index
                    property vector3d dir: modelData.dir
                    property int pieceIndex: 6 + index
                    source: "#Cube"
                    position: modelData.position
                    scale: modelData.scale
                    pickable: true
                    materials: PrincipledMaterial {
                        roughness: 1
                        baseColor: root.hovered === block.pieceIndex ? Theme.hoverColor : (Theme.dark ? "#5a6069" : "#d3d8de")
                    }
                }
            }
        }
    }

    HoverHandler {
        id: hover
        onPointChanged: {
            const piece = root.pieceAt(point.position.x - cubeView.x, point.position.y - cubeView.y)
            root.hovered = piece ? piece.pieceIndex : -1
        }
        onHoveredChanged: if (!hovered) root.hovered = -1
    }

    // Exclusive grab on press so clicks on the cube don't also reach the viewport's picking;
    // the DragHandler below may still take over to orbit.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: eventPoint => {
            const piece = root.pieceAt(eventPoint.position.x - cubeView.x, eventPoint.position.y - cubeView.y)
            if (piece)
                root.viewRequested(piece.dir)
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: eventPoint => menu.popup(eventPoint.position.x, eventPoint.position.y)
    }

    DragHandler {
        target: null
        acceptedButtons: Qt.LeftButton
        property point last
        onActiveChanged: last = centroid.position
        onCentroidChanged: {
            if (!active)
                return
            const p = centroid.position
            root.orbitRequested(p.x - last.x, p.y - last.y)
            last = p
        }
    }

    ToolButton {
        id: homeButton
        anchors.left: parent.left
        anchors.top: parent.top
        width: 26
        height: 26
        padding: 4
        horizontalPadding: 4
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Home")
        onClicked: root.homeRequested()
        contentItem: AppIcon {
            name: "home"
            size: 18
            color: homeButton.hovered ? Theme.accent : Theme.icon
        }
    }

    Menu {
        id: menu
        MenuItem {
            text: qsTr("Go Home")
            onTriggered: root.homeRequested()
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Orthographic")
            checkable: true
            checked: root.orthographic
            onTriggered: {
                root.projectionRequested(true)
                checked = Qt.binding(() => root.orthographic)
            }
        }
        MenuItem {
            text: qsTr("Perspective")
            checkable: true
            checked: !root.orthographic
            onTriggered: {
                root.projectionRequested(false)
                checked = Qt.binding(() => !root.orthographic)
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Set current view as Home")
            onTriggered: root.setHomeRequested()
        }
        MenuItem {
            text: qsTr("Reset Home")
            onTriggered: root.resetHomeRequested()
        }
    }
}
