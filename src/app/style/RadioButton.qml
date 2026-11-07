// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.RadioButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 4
    spacing: Theme.space2
    font.pixelSize: Theme.fontBody

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: 8
        color: Theme.field
        border.width: control.checked ? 5 : 1
        border.color: control.checked || control.visualFocus ? Theme.accent : Theme.borderStrong
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
    }

    opacity: enabled ? 1 : 0.45
}
