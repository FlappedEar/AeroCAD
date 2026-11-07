// SPDX-License-Identifier: GPL-3.0-or-later
// Properties-panel section: caption in small capitals over a subtle card.
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.GroupBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitLabelWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    spacing: 4
    padding: Theme.space3
    topPadding: padding + (implicitLabelWidth > 0 ? implicitLabelHeight + spacing : 0)
    font.pixelSize: Theme.fontBody

    label: Text {
        x: control.leftPadding
        y: Theme.space2
        width: control.availableWidth
        text: control.title.toUpperCase()
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.captionSpacing
        elide: Text.ElideRight
        color: Theme.textMuted
    }

    background: Rectangle {
        width: parent.width
        height: parent.height
        radius: Theme.radius
        color: Theme.surfaceRaised
        border.color: Theme.border
    }
}
