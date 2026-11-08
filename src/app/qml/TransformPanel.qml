// SPDX-License-Identifier: GPL-3.0-or-later
// Small, non-modal panel of the Move / Rotate / Scale tool: the exact values of what the
// manipulator in the view does. Typing a value previews it at once (Enter or leaving the
// field); Done (Enter) applies everything as one undo step, Cancel (Esc) puts the part back.
// Clicking another part or starting another command applies it too.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    /// The Viewport (transform state, finishTransform, rotation helpers).
    required property var viewport
    readonly property var t: viewport.transformTarget
    visible: !!t
    padding: 10

    function fmt(v, decimals) {
        return Number(v).toFixed(decimals).replace(/\.?0+$/, "") || "0"
    }

    /// Euler angles (degrees) shown for the current rotation: X, then Y, then Z, about the
    /// world axes.
    readonly property vector3d euler: viewport.quaternionToEuler(viewport.transformRotation)

    component ValueField: TextField {
        id: field
        property real value: 0
        property int decimals: 2
        signal edited(real value)
        Layout.preferredWidth: 72
        horizontalAlignment: TextInput.AlignRight
        selectByMouse: true
        validator: DoubleValidator { notation: DoubleValidator.StandardNotation; locale: "C" }
        /// Typed by the user and not applied yet. Only such text is applied, so a field
        /// left after a drag in the view never writes its old value back.
        property bool typed: false
        text: root.fmt(value, decimals)
        onTextEdited: typed = true
        onValueChanged: if (!typed) text = root.fmt(value, decimals)
        onActiveFocusChanged: {
            if (!activeFocus) {
                apply()
                text = root.fmt(value, decimals)
            }
        }
        onAccepted: apply()
        function apply() {
            if (!typed)
                return
            typed = false
            const v = Number(String(text).replace(",", "."))
            if (isFinite(v) && Math.abs(v - value) > 1e-9)
                edited(v)
            text = root.fmt(value, decimals)
        }
        Keys.onEscapePressed: {
            typed = false
            text = root.fmt(value, decimals)
            root.viewport.finishTransform(false)
        }
    }

    ColumnLayout {
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Move / Rotate / Scale")
                font.bold: true
                Layout.fillWidth: true
            }
            Label {
                text: root.t ? root.t.name : ""
                color: Theme.textMuted
                elide: Text.ElideRight
                Layout.maximumWidth: 140
            }
        }
        Label {
            text: qsTr("Drag the arrows, rings or the middle square in the view (Shift snaps), or type values.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
            Layout.preferredWidth: 300
        }

        GridLayout {
            columns: 4
            columnSpacing: 6
            rowSpacing: 4

            Label { text: "" }
            Label { text: "X"; color: "#e5484d"; font.bold: true; Layout.alignment: Qt.AlignHCenter }
            Label { text: "Y"; color: "#30a46c"; font.bold: true; Layout.alignment: Qt.AlignHCenter }
            Label { text: "Z"; color: "#3e63dd"; font.bold: true; Layout.alignment: Qt.AlignHCenter }

            Label { text: qsTr("Move (mm)") }
            ValueField {
                objectName: "moveX"
                value: root.viewport.transformMove.x
                onEdited: v => root.viewport.transformMove = Qt.vector3d(v, root.viewport.transformMove.y, root.viewport.transformMove.z)
            }
            ValueField {
                objectName: "moveY"
                value: root.viewport.transformMove.y
                onEdited: v => root.viewport.transformMove = Qt.vector3d(root.viewport.transformMove.x, v, root.viewport.transformMove.z)
            }
            ValueField {
                objectName: "moveZ"
                value: root.viewport.transformMove.z
                onEdited: v => root.viewport.transformMove = Qt.vector3d(root.viewport.transformMove.x, root.viewport.transformMove.y, v)
            }

            Label { text: qsTr("Rotate (°)") }
            ValueField {
                objectName: "rotateX"
                decimals: 1
                value: root.euler.x
                onEdited: v => root.viewport.transformRotation = root.viewport.eulerToQuaternion(Qt.vector3d(v, root.euler.y, root.euler.z))
            }
            ValueField {
                objectName: "rotateY"
                decimals: 1
                value: root.euler.y
                onEdited: v => root.viewport.transformRotation = root.viewport.eulerToQuaternion(Qt.vector3d(root.euler.x, v, root.euler.z))
            }
            ValueField {
                objectName: "rotateZ"
                decimals: 1
                value: root.euler.z
                onEdited: v => root.viewport.transformRotation = root.viewport.eulerToQuaternion(Qt.vector3d(root.euler.x, root.euler.y, v))
            }

            Label { text: qsTr("Scale (%)"); visible: root.t ? root.t.canScale : false }
            ValueField {
                objectName: "scale"
                visible: root.t ? root.t.canScale : false
                decimals: 1
                value: root.viewport.transformScale * 100
                onEdited: v => { if (v > 0) root.viewport.transformScale = v / 100 }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "layOnFace"
                text: qsTr("Lay on Face")
                checkable: true
                checked: root.viewport.transformPickMode === "lay"
                onClicked: root.viewport.transformPickMode = checked ? "lay" : ""
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Click a face of the part: it is laid down on that face, on the ground")
            }
            Button {
                objectName: "faceFront"
                text: qsTr("Face Front")
                checkable: true
                checked: root.viewport.transformPickMode === "front"
                onClicked: root.viewport.transformPickMode = checked ? "front" : ""
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Click a face of the part: it turns to the front, where the wind tunnel air comes from (+Y flow)")
            }
        }
        Label {
            visible: root.viewport.transformPickMode !== ""
            text: qsTr("Click a face of the part (Esc: back).")
            color: Theme.accent
            font.pixelSize: Theme.fontSmall
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                text: qsTr("Reset")
                flat: true
                onClicked: root.viewport.resetTransform()
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Cancel")
                onClicked: root.viewport.finishTransform(false)
            }
            Button {
                text: qsTr("Done")
                highlighted: true
                onClicked: root.viewport.finishTransform(true)
            }
        }
    }
}
