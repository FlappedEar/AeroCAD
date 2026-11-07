// SPDX-License-Identifier: GPL-3.0-or-later
// AI agent access (MCP): turn access on/off, see connected agents and every operation
// they perform, disconnect them. There is no chat here: the agent is an external MCP
// client such as Claude Desktop or Claude Code.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Dialog {
    id: root

    required property McpAccess access

    title: qsTr("AI agent access (MCP)")
    modal: false
    width: Math.min(640, parent ? parent.width - 40 : 640)
    height: Math.min(680, parent ? parent.height - 40 : 680)
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: qsTr("Project: %1").arg(root.access.projectTarget)
            wrapMode: Text.WrapAnywhere
            font.bold: true
        }

        Switch {
            text: qsTr("Allow AI agents to use this project")
            checked: root.access.enabled
            onToggled: root.access.enabled = checked
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.8
            text: !root.access.enabled
                  ? qsTr("Off. Agents cannot read or change the project.")
                  : root.access.listening
                    ? qsTr("On for this project until New, Open, recovery or app exit. Agents can edit the project, read/write files and run programs. Undo applies only to undoable project edits.")
                    : qsTr("Could not start: %1").arg(root.access.errorString)
            color: root.access.enabled && !root.access.listening ? "#d9534f" : palette.windowText
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.access.enabled
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: root.access.clients.length === 0
                      ? qsTr("No agent connected")
                      : qsTr("Connected: %1").arg(root.access.clients.join(", "))
            }
            Button {
                text: qsTr("Disconnect")
                enabled: root.access.clients.length > 0
                onClicked: root.access.disconnectClients()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Operations (%1)").arg(root.access.operationCount)
                font.bold: true
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Clear")
                flat: true
                enabled: root.access.log.length > 0
                onClicked: root.access.clearLog()
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 2

            ListView {
                id: logView
                anchors.fill: parent
                clip: true
                model: root.access.log
                ScrollBar.vertical: ScrollBar {}

                Label {
                    anchors.centerIn: parent
                    visible: logView.count === 0
                    text: qsTr("Nothing yet")
                    opacity: 0.5
                }

                delegate: ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    hoverEnabled: true
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: modelData.client + "\n" + modelData.project + "\n" + modelData.effects + "\n" + modelData.params
                    contentItem: RowLayout {
                        spacing: 8
                        Label {
                            text: modelData.ok ? "✓" : "✗"
                            color: modelData.ok ? "#4caf50" : "#d9534f"
                            font.bold: true
                        }
                        Label { text: modelData.time; opacity: 0.6; font.family: "monospace" }
                        Label { text: modelData.tool; font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.message
                            elide: Text.ElideRight
                            opacity: 0.8
                        }
                        Label { text: qsTr("%1 ms").arg(modelData.durationMs); opacity: 0.5 }
                    }
                }
            }
        }

        Label {
            text: qsTr("Connect Claude")
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.8
            text: qsTr("Claude Desktop: add this to claude_desktop_config.json (Settings > Developer > Edit Config) and restart Claude. Claude Code: run the command below.")
        }
        RowLayout {
            Layout.fillWidth: true
            TextArea {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                font.family: "monospace"
                font.pixelSize: 11
                wrapMode: TextEdit.WrapAnywhere
                text: root.access.claudeDesktopConfig
            }
            Button {
                text: qsTr("Copy")
                onClicked: root.access.copyToClipboard(root.access.claudeDesktopConfig)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                font.family: "monospace"
                font.pixelSize: 11
                text: root.access.claudeCodeCommand
                cursorPosition: 0
            }
            Button {
                text: qsTr("Copy")
                onClicked: root.access.copyToClipboard(root.access.claudeCodeCommand)
            }
        }
    }
}
