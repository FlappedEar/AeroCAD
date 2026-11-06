// SPDX-License-Identifier: GPL-3.0-or-later
// Shown in the middle of the empty 3D view: how to start.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property ContextActions registry
    required property ActionPopup popup

    padding: Theme.space6
    width: 480
    background: Rectangle {
        color: Theme.overlay
        border.color: Theme.border
        radius: Theme.radiusLarge
    }

    contentItem: ColumnLayout {
        spacing: Theme.space3
        Label {
            text: qsTr("Start a design")
            font.pixelSize: Theme.fontTitle + 3
            font.weight: Font.DemiBold
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: qsTr("Import a scan (STL, OBJ, PLY) to design parts that fit it, or start from a sketch. You can also drop a file here.")
        }
        Repeater {
            model: [root.registry.action("import", {}), root.registry.action("newSketch", {}), root.registry.action("box", {})]
            delegate: RowLayout {
                id: row
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: Theme.space3
                ActionButton {
                    Layout.preferredWidth: 200
                    act: Object.assign({}, row.modelData, { primary: row.index === 0 })
                    onClicked: root.popup.request(row.modelData, {}, this)
                }
                Label {
                    Layout.fillWidth: true
                    text: row.modelData.tip
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
