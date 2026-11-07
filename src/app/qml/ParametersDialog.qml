// SPDX-License-Identifier: GPL-3.0-or-later
// User parameters (Fusion: Modify > Change Parameters). Expressions may use other
// parameters and units; every change regenerates the model.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Dialog {
    id: root

    required property AppController controller
    property var parameters: []
    property string error: ""

    title: qsTr("Change Parameters")
    modal: true
    standardButtons: Dialog.Close
    width: 520

    function refresh() {
        const r = controller.execute("timeline.list")
        if (r.ok)
            parameters = r.result.parameters
    }

    function setParameter(name, expression, comment) {
        const r = controller.execute("parameter.set", { name: name, expression: expression, comment: comment || "" })
        error = r.ok ? "" : r.error
        refresh()
        return r.ok
    }

    onAboutToShow: { error = ""; refresh() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("Name"); font.bold: true; Layout.preferredWidth: 100 }
            Label { text: qsTr("Expression"); font.bold: true; Layout.fillWidth: true }
            Label { text: qsTr("Value (mm / deg)"); font.bold: true }
            Item { Layout.preferredWidth: 40 }
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(240, contentHeight + 4)
            clip: true
            model: root.parameters
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                Label { text: modelData.name; Layout.preferredWidth: 100; elide: Text.ElideRight }
                TextField {
                    Layout.fillWidth: true
                    text: modelData.expression
                    selectByMouse: true
                    onEditingFinished: if (text !== modelData.expression) root.setParameter(modelData.name, text, modelData.comment)
                }
                Label {
                    Layout.preferredWidth: 80
                    text: modelData.value !== undefined ? Number(modelData.value).toFixed(3) : "⚠"
                    horizontalAlignment: Text.AlignRight
                }
                ToolButton {
                    text: "✕"
                    onClicked: {
                        root.controller.execute("parameter.remove", { name: modelData.name })
                        root.refresh()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            TextField { id: newName; placeholderText: qsTr("name, e.g. width"); Layout.preferredWidth: 140; selectByMouse: true }
            TextField {
                id: newExpr
                placeholderText: qsTr("expression, e.g. 40 mm")
                Layout.fillWidth: true
                selectByMouse: true
                onAccepted: addButton.clicked()
            }
            Button {
                id: addButton
                text: qsTr("Add")
                enabled: newName.text.length > 0 && newExpr.text.length > 0
                onClicked: if (root.setParameter(newName.text, newExpr.text)) { newName.text = ""; newExpr.text = "" }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.error !== ""
            text: root.error
            color: Theme.danger
            wrapMode: Text.WordWrap
        }
    }
}
