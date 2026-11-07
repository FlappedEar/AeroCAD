// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.SpinBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitBackgroundHeight)

    padding: 4
    leftPadding: Theme.space2
    rightPadding: 22 + Theme.space1
    font.pixelSize: Theme.fontBody

    validator: IntValidator {
        locale: control.locale.name
        bottom: Math.min(control.from, control.to)
        top: Math.max(control.from, control.to)
    }

    contentItem: TextInput {
        text: control.displayText
        font: control.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        horizontalAlignment: Qt.AlignLeft
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        clip: true
    }

    up.indicator: Rectangle {
        x: control.width - width - 2
        y: 2
        implicitWidth: 20
        implicitHeight: (control.height - 4) / 2
        radius: 3
        color: control.up.pressed ? Theme.pressed : control.up.hovered ? Theme.hover : "transparent"
        AppIcon {
            anchors.centerIn: parent
            name: "chevron-down"
            size: 12
            rotation: 180
            color: Theme.textMuted
        }
    }

    down.indicator: Rectangle {
        x: control.width - width - 2
        y: control.height / 2
        implicitWidth: 20
        implicitHeight: (control.height - 4) / 2
        radius: 3
        color: control.down.pressed ? Theme.pressed : control.down.hovered ? Theme.hover : "transparent"
        AppIcon {
            anchors.centerIn: parent
            name: "chevron-down"
            size: 12
            color: Theme.textMuted
        }
    }

    background: Rectangle {
        implicitWidth: 100
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSmall
        color: Theme.field
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent : control.hovered ? Theme.borderStrong : Theme.border
    }

    opacity: enabled ? 1 : 0.45
}
