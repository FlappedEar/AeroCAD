// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import AeroCAD

T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 0
    overlap: 4
    padding: 4
    font.pixelSize: Theme.fontBody

    delegate: MenuItem {}

    contentItem: ListView {
        implicitHeight: contentHeight
        // Wide enough for the longest item (text and shortcut), not a fixed width.
        implicitWidth: {
            let w = 0
            for (let i = 0; i < control.count; ++i) {
                const item = control.itemAt(i)
                if (item)
                    w = Math.max(w, item.implicitWidth)
            }
            return w
        }
        model: control.contentModel
        interactive: Window.window
                     ? contentHeight + control.topPadding + control.bottomPadding > Window.window.height
                     : false
        clip: true
        currentIndex: control.currentIndex
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    background: PopupBackground {
        implicitWidth: 220
        implicitHeight: 32
    }

    T.Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, Theme.dark ? 0.45 : 0.28) }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
