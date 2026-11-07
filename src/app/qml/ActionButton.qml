// SPDX-License-Identifier: GPL-3.0-or-later
// A button for a ContextActions action: icon and text, accent when it is the
// recommended next step, a tooltip with what it does, or why it is disabled (the
// wrapper keeps receiving hover while the button itself is disabled).
import QtQuick
import QtQuick.Controls
import AeroCAD

Item {
    id: root

    property var act: ({})
    /// Smaller padding (the context bar).
    property bool compact: false
    property bool flat: false
    signal clicked()

    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    Button {
        id: button
        anchors.fill: parent
        text: root.act.text ?? ""
        enabled: root.act.enabled ?? false
        highlighted: (root.act.primary ?? false) && enabled
        icon.source: Theme.iconUrl(root.act.icon ?? "")
        icon.width: Theme.iconSmall
        icon.height: Theme.iconSmall
        flat: root.flat
        display: AbstractButton.TextBesideIcon
        horizontalPadding: root.compact ? Theme.space2 : Theme.space3
        focusPolicy: Qt.NoFocus
        onClicked: { root._tipDone = true; root.clicked() }
    }

    // After a click the tooltip would cover the popup it opened; it comes back on the
    // next hover.
    property bool _tipDone: false
    HoverHandler { id: hover; onHoveredChanged: if (!hovered) root._tipDone = false }
    ToolTip.visible: hover.hovered && !_tipDone
    ToolTip.delay: 400
    ToolTip.text: ((act.enabled ?? false) ? (act.tip ?? "") : (act.reason ?? act.tip ?? ""))
}
