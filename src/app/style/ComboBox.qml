// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 4
    leftPadding: Theme.space2
    rightPadding: Theme.space2 + 16 + spacing
    spacing: Theme.space1
    font.pixelSize: Theme.fontBody

    delegate: T.ItemDelegate {
        id: item
        required property int index
        width: ListView.view.width
        implicitHeight: Theme.controlHeight
        leftPadding: Theme.space2
        rightPadding: Theme.space2
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
        // textAt() resolves textRole for every model kind (string list, object list,
        // C++ list of maps, ListModel). The old Array.isArray(control.model) check is
        // false on newer Qt for a JS object list, which left every row empty.
        text: control.textAt(index)
        contentItem: Text {
            text: item.text
            font.pixelSize: Theme.fontBody
            font.weight: control.currentIndex === item.index ? Font.DemiBold : Font.Normal
            color: Theme.text
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: item.highlighted ? Theme.accentSoft : "transparent"
        }
    }

    indicator: AppIcon {
        x: control.width - width - Theme.space2
        y: (control.height - height) / 2
        name: "chevron-down"
        size: 14
        color: Theme.textMuted
    }

    contentItem: T.TextField {
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        font: control.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: 140
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSmall
        color: control.editable ? Theme.field : control.down ? Theme.pressed
             : control.hovered ? Qt.tint(Theme.surfaceRaised, Theme.hover) : Theme.surfaceRaised
        border.width: control.visualFocus || (control.editable && control.activeFocus) ? 2 : 1
        border.color: control.visualFocus || (control.editable && control.activeFocus) ? Theme.accent : Theme.border
        visible: !control.flat || control.down
    }

    popup: T.Popup {
        y: control.height + 2
        width: control.width
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, control.Window.height - topMargin - bottomMargin)
        topMargin: 6
        bottomMargin: 6
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: PopupBackground {}
    }

    opacity: enabled ? 1 : 0.45
}
