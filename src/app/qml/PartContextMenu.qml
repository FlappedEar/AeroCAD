// SPDX-License-Identifier: GPL-3.0-or-later
// Right-click menu (3D view, Browser, context bar "More"). It offers only what fits what
// was clicked, as in Fusion:
//   on a part:  its name, the recommended next step (★), the other steps that can be done
//               now (at most `maxSteps`, the rest under "More"), then the commands every
//               part has: Move / Rotate, Zoom to this, Hide, Delete;
//   on nothing: the view (Fit, Home, Orthographic, Show all), Undo / Redo, Import, New sketch.
// Disabled actions are left out: a menu lists what can be done, the guide (Properties)
// explains what cannot be done yet and why.
import QtQuick
import QtQuick.Controls
import AeroCAD

Menu {
    id: root

    required property ContextActions registry
    required property ActionPopup popupHelper
    property var info: ({})
    /// Steps shown at the top level; the rest go to "More".
    property int maxSteps: 5

    /// Commands every part gets at the bottom, in this order.
    readonly property var commonIds: ["moveRotate", "zoomTo", "hide", "delete"]
    /// Commands that make no sense from a right-click on a part (they start something new).
    readonly property var startIds: ["import", "newSketch", "box"]
    /// Tool switches: under "More", the toolbar and keys 1-3 are the quicker way.
    readonly property var toolIds: ["regionTool", "lassoTool", "brushTool"]

    /// {header, steps: [action], more: [action], common: [action]} for a part, or null.
    function partSections(info) {
        if (!info || !info.id)
            return null
        const all = registry.actions(info)
        const next = registry.nextAction(info)
        const seen = {}
        const usable = []
        for (const a of (next ? [next] : []).concat(all)) {
            if (seen[a.id] || !a.enabled || commonIds.includes(a.id) || startIds.includes(a.id))
                continue
            seen[a.id] = true
            usable.push(a)
        }
        const common = []
        for (const id of commonIds) {
            const a = registry.action(id, info)
            if (a.enabled)
                common.push(a)
        }
        const main = usable.filter(a => !toolIds.includes(a.id))
        const tools = usable.filter(a => toolIds.includes(a.id))
        return { header: registry.roleName(registry.role(info)) + ": " + (info.name || ""),
                 primaryId: next ? next.id : "",
                 steps: main.slice(0, maxSteps), more: main.slice(maxSteps).concat(tools), common: common }
    }

    readonly property Component itemComponent: Component {
        MenuItem {
            property var act: null
            property bool isPrimary: false
            property var run: null
            font.bold: isPrimary
            onTriggered: {
                if (run)
                    run()
                else if (act)
                    root.popupHelper.request(act, root.info, null)
            }
        }
    }
    readonly property Component separatorComponent: Component { MenuSeparator {} }
    // One "More" submenu, reused: destroying a submenu while it closes breaks its background.
    Menu {
        id: moreMenu
        title: qsTr("More")
    }

    property var _created: []

    function _clear() {
        while (moreMenu.count > 0)
            moreMenu.takeItem(0)
        while (count > 0) {
            if (menuAt(0) === moreMenu)
                takeMenu(0)
            else
                takeItem(0)
        }
        for (const o of _created)
            o.destroy()
        _created = []
    }

    function _add(menu, props) {
        const o = itemComponent.createObject(null)
        for (const k in props) {
            if (k === "icon")
                o.icon.source = props[k]
            else
                o[k] = props[k]
        }
        _created.push(o)
        menu.addItem(o)
        return o
    }

    function _addAction(menu, a, primary) {
        return _add(menu, { act: a, isPrimary: !!primary, text: a.text + (primary ? "  ★" : ""),
                            icon: Theme.iconUrl(a.icon) })
    }

    function _separator(menu) {
        const o = separatorComponent.createObject(null)
        _created.push(o)
        menu.addItem(o)
    }

    function _build() {
        _clear()
        const host = registry.host
        const s = partSections(info)
        if (s) {
            const title = _add(root, { text: s.header })
            title.enabled = false
            for (const a of s.steps)
                _addAction(root, a, a.id === s.primaryId)
            if (s.more.length) {
                for (const a of s.more)
                    _addAction(moreMenu, a, false)
                root.addMenu(moreMenu)
            }
            if (s.common.length) {
                _separator(root)
                for (const a of s.common)
                    _addAction(root, a, false)
            }
            return
        }
        // Nothing under the pointer: the view and the document.
        _add(root, { text: qsTr("Fit All (F6)"), icon: Theme.iconUrl("fit"), run: () => host.fitView() })
        _add(root, { text: qsTr("Home View"), icon: Theme.iconUrl("home"), run: () => host.homeView() })
        const ortho = host.isOrthographic()
        _add(root, { text: ortho ? qsTr("Perspective") : qsTr("Orthographic"),
                     icon: Theme.iconUrl(ortho ? "perspective" : "ortho"),
                     run: () => host.setOrthographic(!ortho) })
        if (host.hiddenPartCount() > 0)
            _add(root, { text: qsTr("Show All Parts"), icon: Theme.iconUrl("visible"), run: () => host.showAllParts() })
        const c = registry.controller
        if (c.canUndo || c.canRedo) {
            _separator(root)
            if (c.canUndo)
                _add(root, { text: qsTr("Undo %1").arg(c.undoText), icon: Theme.iconUrl("undo"),
                             run: () => c.execute("edit.undo") })
            if (c.canRedo)
                _add(root, { text: qsTr("Redo %1").arg(c.redoText), icon: Theme.iconUrl("redo"),
                             run: () => c.execute("edit.redo") })
        }
        _separator(root)
        for (const id of startIds.slice(0, 2))
            _addAction(root, registry.action(id, info || {}), false)
    }

    // Built as soon as the info is set: a Menu without items does not open at all.
    onInfoChanged: _build()
    onAboutToShow: _build()
    Component.onCompleted: _build()
}
