// SPDX-License-Identifier: GPL-3.0-or-later
// Floating bar over the 3D view for the selected part: what it is, the recommended next
// step as a highlighted button, a few other actions, and a menu with all of them.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Pane {
    id: root

    required property ContextActions registry
    required property ActionPopup popup
    property var info: ({})

    readonly property var all: registry.actions(info)
    readonly property var next: registry.nextAction(info)
    /// Width the bar may take (set by the view); fewer quick actions fit when it is narrow.
    property real maximumWidth: 900
    /// A few other enabled actions, with their names (icons alone say too little).
    readonly property var quick: all.filter(a => a.enabled && (!next || a.id !== next.id))
                                    .slice(0, Math.max(0, Math.min(3, Math.floor((maximumWidth - 470) / 140))))

    padding: 4
    leftPadding: Theme.space3
    background: Rectangle {
        color: Theme.overlay
        border.color: Theme.border
        radius: Theme.radius
        // Keeps clicks on the bar from reaching the 3D view.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
    }

    contentItem: RowLayout {
        spacing: Theme.space1

        AppIcon {
            name: root.registry.roleIcon(root.registry.role(root.info))
            size: Theme.iconSmall
            color: Theme.accent
        }
        Label {
            Layout.maximumWidth: 150
            text: root.info.name ?? ""
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Label {
            text: root.registry.roleName(root.registry.role(root.info))
            color: Theme.textFaint
            font.pixelSize: Theme.fontSmall
            Layout.rightMargin: Theme.space1
        }
        Rectangle { implicitWidth: 1; implicitHeight: 20; color: Theme.separator }

        Label {
            visible: !!root.next
            text: qsTr("Next:")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            Layout.leftMargin: Theme.space1
        }
        ActionButton {
            visible: !!root.next
            act: root.next ?? ({})
            onClicked: root.popup.request(root.next, root.info, this)
        }
        Repeater {
            model: root.quick
            delegate: ActionButton {
                required property var modelData
                act: Object.assign({}, modelData, { primary: false })
                compact: true
                flat: true
                onClicked: root.popup.request(modelData, root.info, this)
            }
        }
        IconButton {
            id: moreButton
            iconName: "menu"
            tip: qsTr("All actions for this part")
            onClicked: menu.popup(moreButton, 0, moreButton.height + 4)
        }
    }

    PartContextMenu {
        id: menu
        registry: root.registry
        popupHelper: root.popup
        info: root.info
    }
}
