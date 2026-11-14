// SPDX-License-Identifier: GPL-3.0-or-later
// Step-by-step guide for the selected part, at the top of Properties: what the part is,
// what can be done with it, numbered steps with a sentence each and their buttons, and
// why a button is disabled. Content comes from ContextActions.guide().
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

ColumnLayout {
    id: root

    required property ContextActions registry
    required property ActionPopup popup
    property var info: ({})

    readonly property var guide: registry.guide(info)

    spacing: Theme.space2

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        AppIcon {
            name: root.guide.icon || root.registry.roleIcon(root.registry.role(root.info))
            size: Theme.iconMedium
            color: Theme.accent
        }
        Label {
            Layout.fillWidth: true
            text: root.info.id && !root.guide.plainTitle ? root.guide.title + " · " + root.info.name : root.guide.title
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
    }
    Label {
        Layout.fillWidth: true
        text: root.guide.intro
        color: Theme.textMuted
        wrapMode: Text.WordWrap
    }

    Repeater {
        model: root.guide.sections
        delegate: ColumnLayout {
            id: section
            required property var modelData
            /// A collapsed section (rarely used tools) opens on a click on its title.
            property bool open: false
            Layout.fillWidth: true
            Layout.topMargin: Theme.space2
            spacing: Theme.space2

            Text {
                visible: section.modelData.title.length > 0
                Layout.fillWidth: true
                text: (section.modelData.collapsed ? (section.open ? "▾ " : "▸ ") : "") + section.modelData.title.toUpperCase()
                MouseArea {
                    anchors.fill: parent
                    enabled: !!section.modelData.collapsed
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: section.open = !section.open
                }
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.captionSpacing
                elide: Text.ElideRight
            }

            Repeater {
                model: section.modelData.steps
                delegate: RowLayout {
                    id: step
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: Theme.space2

                    // Step number, or a check mark once done.
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        implicitWidth: 22
                        implicitHeight: 22
                        radius: 11
                        color: step.modelData.done ? Theme.success : Theme.accentSoft
                        Text {
                            anchors.centerIn: parent
                            visible: !step.modelData.done
                            text: step.index + 1
                            color: Theme.accent
                            font.pixelSize: Theme.fontSmall
                            font.weight: Font.Bold
                        }
                        AppIcon {
                            anchors.centerIn: parent
                            visible: step.modelData.done
                            name: "check"
                            size: 14
                            color: "#ffffff"
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            Layout.fillWidth: true
                            text: step.modelData.title
                            font.weight: Font.DemiBold
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: step.modelData.text
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space1
                            Repeater {
                                model: step.modelData.actions
                                delegate: ActionButton {
                                    required property var modelData
                                    act: modelData
                                    onClicked: root.popup.request(modelData, root.info, this)
                                }
                            }
                        }
                        // Why the main button of this step cannot be used yet.
                        Label {
                            readonly property var blocked: step.modelData.actions.find(x => !x.enabled && x.reason)
                            visible: !!blocked
                            Layout.fillWidth: true
                            text: blocked ? "ⓘ " + blocked.reason : ""
                            color: Theme.warning
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            Flow {
                visible: section.modelData.actions.length > 0 && (!section.modelData.collapsed || section.open)
                Layout.fillWidth: true
                spacing: Theme.space1
                Repeater {
                    model: section.modelData.actions
                    delegate: ActionButton {
                        required property var modelData
                        act: modelData
                        onClicked: root.popup.request(modelData, root.info, this)
                    }
                }
            }
        }
    }
}
