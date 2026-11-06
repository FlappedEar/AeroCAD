// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import AeroCAD

ApplicationWindow {
    id: window

    required property AppController controller
    property var selectedPartId: 0

    width: 1400
    height: 860
    visible: true
    title: qsTr("AeroCAD %1").arg(controller.version)

    function selectPart(id) {
        selectedPartId = id
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import 3D file")
        nameFilters: window.controller.importNameFilters
        onAccepted: window.controller.importFile(selectedFile)
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            Action {
                text: qsTr("&Import...")
                shortcut: StandardKey.Open
                onTriggered: importDialog.open()
            }
            MenuSeparator {}
            Action {
                text: qsTr("E&xit")
                shortcut: StandardKey.Quit
                onTriggered: Qt.quit()
            }
        }
        Menu {
            title: qsTr("&Edit")
            Action {
                text: window.controller.canUndo ? qsTr("&Undo %1").arg(window.controller.undoText) : qsTr("&Undo")
                enabled: window.controller.canUndo
                shortcut: StandardKey.Undo
                onTriggered: window.controller.execute("edit.undo")
            }
            Action {
                text: window.controller.canRedo ? qsTr("&Redo %1").arg(window.controller.redoText) : qsTr("&Redo")
                enabled: window.controller.canRedo
                shortcut: StandardKey.Redo
                onTriggered: window.controller.execute("edit.redo")
            }
        }
        Menu {
            title: qsTr("&View")
            Action {
                text: qsTr("&Fit all")
                shortcut: "F"
                onTriggered: viewport.fitAll()
            }
            Action {
                text: qsTr("&Top")
                onTriggered: viewport.setView(-90, 0)
            }
            Action {
                text: qsTr("F&ront")
                onTriggered: viewport.setView(0, 0)
            }
            Action {
                text: qsTr("&Right")
                onTriggered: viewport.setView(0, 90)
            }
            Action {
                text: qsTr("&Isometric")
                onTriggered: viewport.setView(-30, 45)
            }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        PartsPanel {
            SplitView.preferredWidth: 260
            SplitView.minimumWidth: 180
            controller: window.controller
            selectedPartId: window.selectedPartId
            onPartSelected: id => window.selectPart(id)
        }

        Viewport {
            id: viewport
            objectName: "viewport"
            SplitView.fillWidth: true
            controller: window.controller
            selectedPartId: window.selectedPartId
            onPartPicked: id => window.selectPart(id)

            DropArea {
                anchors.fill: parent
                onDropped: drop => {
                    for (const url of drop.urls)
                        window.controller.importFile(url)
                }
            }
        }

        PropertiesPanel {
            SplitView.preferredWidth: 280
            SplitView.minimumWidth: 200
            controller: window.controller
            partId: window.selectedPartId
        }
    }

    footer: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            Label {
                Layout.fillWidth: true
                text: window.controller.statusMessage
                elide: Text.ElideRight
            }
            Label {
                text: qsTr("Orbit: left drag · Pan: right/middle drag or Shift+drag · Zoom: wheel · Fit: F")
                opacity: 0.6
            }
        }
    }

    Connections {
        target: window.controller
        function onPartImported(id) {
            window.selectPart(id)
            viewport.fitAll()
        }
        function onPartChanged(id) {
            if (window.selectedPartId === id && window.controller.parts.rowOf(id) < 0)
                window.selectPart(0)
        }
    }
}
