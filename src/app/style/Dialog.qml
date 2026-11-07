// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import AeroCAD

T.Dialog {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth,
                            implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    padding: Theme.space4
    topPadding: Theme.space2
    font.pixelSize: Theme.fontBody

    background: PopupBackground { radius: Theme.radiusLarge }

    header: Text {
        text: control.title
        visible: control.title
        elide: Text.ElideRight
        color: Theme.text
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        padding: Theme.space4
        bottomPadding: Theme.space1
    }

    footer: DialogButtonBox {
        visible: count > 0
    }

    T.Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, Theme.dark ? 0.45 : 0.28) }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
