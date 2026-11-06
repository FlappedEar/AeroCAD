// SPDX-License-Identifier: GPL-3.0-or-later
// Right-click menu for a part (3D view, Browser, context bar "More"): every action of
// ContextActions for it, with the recommended one first.
import QtQuick
import QtQuick.Controls
import AeroCAD

Menu {
    id: root

    required property ContextActions registry
    required property ActionPopup popupHelper
    property var info: ({})

    readonly property var items: {
        const all = registry.actions(info)
        const next = registry.nextAction(info)
        const seen = {}
        const out = []
        for (const a of (next ? [next] : []).concat(all)) {
            if (seen[a.id])
                continue
            seen[a.id] = true
            out.push(a)
        }
        return out
    }

    Instantiator {
        model: root.items
        delegate: MenuItem {
            required property var modelData
            text: modelData.text + (modelData.primary && modelData.enabled ? "  ★" : "")
            enabled: modelData.enabled
            icon.source: Theme.iconUrl(modelData.icon)
            onTriggered: root.popupHelper.request(modelData, root.info, null)
        }
        onObjectAdded: (index, object) => root.insertItem(index, object)
        onObjectRemoved: (index, object) => root.removeItem(object)
    }
}
