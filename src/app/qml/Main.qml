// SPDX-License-Identifier: GPL-3.0-or-later
// Main window, laid out like common CAD applications (Autodesk Fusion in particular):
// toolbar on top, Browser on the left, canvas with ViewCube (top right) and navigation bar
// (bottom centre), properties on the right, status bar at the bottom.
// Keyboard shortcuts follow Fusion's defaults, https://www.autodesk.com/shortcuts/fusion-360
// (Qt maps Ctrl to Command and Alt to Option on macOS).
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import AeroCAD

ApplicationWindow {
    id: window

    required property AppController controller
    property var selectedPartId: 0
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"

    property bool browserVisible: true
    property bool propertiesVisible: true

    width: 1400
    height: 860
    visible: true
    title: qsTr("AeroCAD %1").arg(controller.version)

    function selectPart(id) {
        selectedPartId = id
    }

    function toggleSelectedVisibility() {
        if (!selectedPartId)
            return
        const info = controller.partInfo(selectedPartId)
        if (info.id)
            controller.execute("part.set_visible", { id: selectedPartId, visible: !info.visible })
    }

    function deleteSelected() {
        if (selectedPartId)
            controller.execute("part.remove", { id: selectedPartId })
    }

    function resetLayout() {
        browserVisible = true
        propertiesVisible = true
        viewport.viewCubeVisible = true
        viewport.navigationBarVisible = true
        browser.SplitView.preferredWidth = 260
        properties.SplitView.preferredWidth = 280
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import 3D file")
        nameFilters: window.controller.importNameFilters
        onAccepted: window.controller.importFile(selectedFile)
    }

    // --- Actions (shared by the menu bar and the toolbar) -------------------------------

    Action {
        id: importAction
        text: qsTr("&Import...")
        shortcut: StandardKey.Open // Fusion: Ctrl+O / Cmd+O
        onTriggered: importDialog.open()
    }
    Action {
        id: undoAction
        text: window.controller.canUndo ? qsTr("&Undo %1").arg(window.controller.undoText) : qsTr("&Undo")
        enabled: window.controller.canUndo
        shortcut: StandardKey.Undo // Fusion: Ctrl+Z / Cmd+Z
        onTriggered: window.controller.execute("edit.undo")
    }
    Action {
        id: redoAction
        text: window.controller.canRedo ? qsTr("&Redo %1").arg(window.controller.redoText) : qsTr("&Redo")
        enabled: window.controller.canRedo
        // Fusion: Ctrl+Y on Windows, Cmd+Shift+Z on macOS.
        shortcut: window.isMac ? "Ctrl+Shift+Z" : "Ctrl+Y"
        onTriggered: window.controller.execute("edit.redo")
    }
    Action {
        id: deleteAction
        text: qsTr("&Delete")
        enabled: !!window.selectedPartId
        shortcut: "Delete" // Fusion: Del
        onTriggered: window.deleteSelected()
    }
    Action {
        id: visibilityAction
        text: qsTr("Toggle &Visibility")
        enabled: !!window.selectedPartId
        shortcut: "V" // Fusion: V
        onTriggered: window.toggleSelectedVisibility()
    }
    Action {
        id: fitAction
        text: qsTr("&Fit")
        // Fusion lists F6 for Zoom to Fit (Electronics workspace); F is Fillet in Fusion.
        shortcut: "F6"
        onTriggered: viewport.fitAll()
    }
    Action {
        id: homeAction
        text: qsTr("&Home")
        onTriggered: viewport.goHome()
    }
    Action {
        id: orthoAction
        text: qsTr("&Orthographic")
        checkable: true
        checked: viewport.orthographic
        onTriggered: {
            viewport.orthographic = checked
            checked = Qt.binding(() => viewport.orthographic)
        }
    }

    // macOS keyboards label Backspace "delete".
    Shortcut {
        enabled: window.isMac && deleteAction.enabled
        sequence: "Backspace"
        onActivated: window.deleteSelected()
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            MenuItem { action: importAction }
            MenuSeparator {}
            Action {
                text: qsTr("E&xit")
                shortcut: StandardKey.Quit
                onTriggered: Qt.quit()
            }
        }
        Menu {
            title: qsTr("&Edit")
            MenuItem { action: undoAction }
            MenuItem { action: redoAction }
            MenuSeparator {}
            MenuItem { action: visibilityAction }
            MenuItem { action: deleteAction }
        }
        Menu {
            title: qsTr("&View")
            MenuItem { action: fitAction }
            MenuItem { action: homeAction }
            MenuSeparator {}
            Action { text: qsTr("Front"); onTriggered: viewport.setView(0, 0) }
            Action { text: qsTr("Back"); onTriggered: viewport.setView(0, 180) }
            Action { text: qsTr("Left"); onTriggered: viewport.setView(0, -90) }
            Action { text: qsTr("Right"); onTriggered: viewport.setView(0, 90) }
            Action { text: qsTr("Top"); onTriggered: viewport.setView(-90, 0) }
            Action { text: qsTr("Bottom"); onTriggered: viewport.setView(90, 0) }
            MenuSeparator {}
            MenuItem { action: orthoAction }
            MenuSeparator {}
            Action {
                text: qsTr("Show ViewCube")
                checkable: true
                checked: viewport.viewCubeVisible
                shortcut: "Ctrl+Alt+V" // Fusion: Ctrl+Alt+V / Option+Cmd+V
                onTriggered: {
                    viewport.viewCubeVisible = checked
                    checked = Qt.binding(() => viewport.viewCubeVisible)
                }
            }
            Action {
                text: qsTr("Show Browser")
                checkable: true
                checked: window.browserVisible
                shortcut: "Ctrl+Alt+B" // Fusion: Ctrl+Alt+B / Option+Cmd+B
                onTriggered: {
                    window.browserVisible = checked
                    checked = Qt.binding(() => window.browserVisible)
                }
            }
            Action {
                text: qsTr("Show Navigation Bar")
                checkable: true
                checked: viewport.navigationBarVisible
                shortcut: "Ctrl+Alt+N" // Fusion: Ctrl+Alt+N / Option+Cmd+N
                onTriggered: {
                    viewport.navigationBarVisible = checked
                    checked = Qt.binding(() => viewport.navigationBarVisible)
                }
            }
            Action {
                text: qsTr("Show Properties")
                checkable: true
                checked: window.propertiesVisible
                onTriggered: {
                    window.propertiesVisible = checked
                    checked = Qt.binding(() => window.propertiesVisible)
                }
            }
            MenuSeparator {}
            Action {
                text: qsTr("Reset to Default Layout")
                shortcut: "Ctrl+Alt+R" // Fusion: Ctrl+Alt+R / Option+Cmd+R
                onTriggered: window.resetLayout()
            }
        }
    }

    // --- Toolbar: grouped commands with captions, as in Fusion's toolbar ---------------

    component ToolGroup: ColumnLayout {
        property string caption
        default property alias buttons: row.data
        spacing: 0
        RowLayout {
            id: row
            spacing: 0
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: parent.caption
            font.pixelSize: 10
            font.bold: true
            opacity: 0.6
        }
    }

    component ToolCommand: ToolButton {
        display: AbstractButton.TextOnly
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            spacing: 6
            ToolGroup {
                caption: qsTr("EDIT")
                ToolCommand { action: undoAction; text: qsTr("Undo") }
                ToolCommand { action: redoAction; text: qsTr("Redo") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("INSERT")
                ToolCommand { action: importAction; text: qsTr("Import") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("MODIFY")
                ToolCommand { action: visibilityAction; text: qsTr("Show/Hide") }
                ToolCommand { action: deleteAction; text: qsTr("Delete") }
            }
            ToolSeparator {}
            ToolGroup {
                caption: qsTr("VIEW")
                ToolCommand { action: fitAction; text: qsTr("Fit") }
                ToolCommand { action: homeAction; text: qsTr("Home") }
                ToolCommand { action: orthoAction; text: qsTr("Ortho") }
            }
            Item { Layout.fillWidth: true }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        PartsPanel {
            id: browser
            visible: window.browserVisible
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
            id: properties
            visible: window.propertiesVisible
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
                text: qsTr("Pan: middle drag · Orbit: Shift+middle drag · Zoom: wheel · Touchpad: two-finger pan, Shift+two-finger orbit, pinch zoom · Fit: F6")
                opacity: 0.6
                elide: Text.ElideLeft
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
