// SPDX-License-Identifier: GPL-3.0-or-later
// A group of ribbon commands with its caption underneath, as in Fusion's toolbar
// (CREATE, MODIFY, ...). Clicking the caption opens a menu with every command of the
// group; commands past `maxVisible` are only in that menu, so a group can grow without
// widening the toolbar. The group shows only while the ribbon's current tab is `tab`.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

ColumnLayout {
    id: root

    property string caption
    /// Ribbon tab the group belongs to ("solid", "sketch", ...); see Ribbon.currentTab.
    property string tab: "solid"
    property int maxVisible: 8
    default property alias buttons: row.data

    /// Buttons with an action, in order.
    readonly property var commands: {
        const list = []
        for (let i = 0; i < row.children.length; ++i) {
            const c = row.children[i]
            if (c.action !== undefined)
                list.push(c)
        }
        return list
    }

    function updateOverflow() {
        const c = commands
        for (let i = 0; i < c.length; ++i)
            c[i].visible = i < maxVisible
    }

    onCommandsChanged: updateOverflow()
    onMaxVisibleChanged: updateOverflow()

    visible: !parent || parent.currentTab === undefined || parent.currentTab === tab
    spacing: 0
    Layout.fillHeight: true

    RowLayout {
        id: row
        Layout.topMargin: 3
        spacing: 1
    }

    Item { Layout.fillHeight: true }

    ToolButton {
        id: captionButton
        Layout.alignment: Qt.AlignHCenter
        Layout.bottomMargin: 2
        implicitHeight: 18
        padding: 0
        leftPadding: Theme.space2
        rightPadding: Theme.space1
        focusPolicy: Qt.NoFocus
        contentItem: Row {
            spacing: 2
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.caption
                color: captionButton.hovered || menu.visible ? Theme.text : Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.captionSpacing
            }
            AppIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"
                size: 12
                color: captionButton.hovered || menu.visible ? Theme.text : Theme.textMuted
            }
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: captionButton.down || menu.visible ? Theme.pressed : captionButton.hovered ? Theme.hover : "transparent"
        }
        onClicked: menu.popup(captionButton, 0, captionButton.height)
    }

    Menu {
        id: menu
        Instantiator {
            model: root.commands
            delegate: MenuItem {
                required property var modelData
                action: modelData.action
                text: modelData.action ? modelData.action.text : modelData.text
                icon.source: Theme.iconUrl(modelData.iconName)
                icon.color: Theme.icon
                icon.width: Theme.iconSmall
                icon.height: Theme.iconSmall
                // Guided commands (Main.qml GuidedCommand) have no Action: the item
                // follows the button and clicks it.
                Component.onCompleted: if (!modelData.action) enabled = Qt.binding(() => modelData.enabled)
                onTriggered: if (!modelData.action) modelData.clicked()
            }
            onObjectAdded: (index, object) => menu.insertItem(index, object)
            onObjectRemoved: (index, object) => menu.removeItem(object)
        }
    }
}
