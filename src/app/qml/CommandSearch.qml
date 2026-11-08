// SPDX-License-Identifier: GPL-3.0-or-later
// Command search (Fusion's S key): a small box at the pointer. Type part of a command's name
// (or of its menu), Up / Down pick a result, Enter runs it, Esc closes. The commands are read
// from the menu bar when the box opens, so it never goes stale; disabled ones are listed
// dimmed and cannot be run.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Popup {
    id: root

    /// The window's MenuBar.
    required property var menuBar
    property var commands: []
    property string filter: ""
    readonly property alias field: field
    readonly property alias results: results

    width: 340
    padding: Theme.space2
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function cleanText(text) {
        return String(text).replace(/&(.)/g, "$1") // menu mnemonics
    }

    function collect(menu, path, out) {
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i)
            if (!item)
                continue
            if (item.subMenu) {
                collect(item.subMenu, path + " › " + cleanText(item.subMenu.title), out)
                continue
            }
            if (!item.text || typeof item.triggered !== "function")
                continue // separators
            const action = item.action
            const shortcut = action && action.shortcut ? Preferences.shortcutText(action.shortcut) : ""
            out.push({ name: cleanText(item.text).replace(/\.\.\.$|…$/, ""), path: path, keys: shortcut, item: item })
        }
    }

    /// Opens the box at x, y (in the parent's coordinates), kept inside the parent.
    function openAt(x, y) {
        const out = []
        for (let i = 0; i < menuBar.count; ++i) {
            const menu = menuBar.menuAt(i)
            if (menu)
                collect(menu, cleanText(menu.title), out)
        }
        commands = out
        filter = ""
        field.text = ""
        root.x = Math.max(0, Math.min(x, parent.width - width))
        root.y = Math.max(0, Math.min(y, parent.height - 320))
        open()
        field.forceActiveFocus()
    }

    /// Commands matching `filter`, best first: name starts with it, a word of the name starts
    /// with it, the name contains it, then the menu path contains it.
    function matches() {
        const f = filter.trim().toLowerCase()
        if (f === "")
            return []
        const scored = []
        for (const c of commands) {
            const n = c.name.toLowerCase()
            let score = -1
            if (n.startsWith(f))
                score = 0
            else if (n.split(/[\s/›(-]+/).some(w => w.startsWith(f)))
                score = 1
            else if (n.includes(f))
                score = 2
            else if (c.path.toLowerCase().includes(f))
                score = 3
            if (score >= 0)
                scored.push({ c: c, score: score + (c.item.enabled ? 0 : 10) })
        }
        scored.sort((a, b) => a.score - b.score || a.c.name.localeCompare(b.c.name))
        return scored.slice(0, 12).map(s => s.c)
    }

    function run(index) {
        const c = results.model[index]
        if (!c || !c.item.enabled)
            return false
        close()
        // After the box has closed, so that a dialog the command opens gets the focus.
        Qt.callLater(() => c.item.action ? c.item.action.trigger() : c.item.triggered())
        return true
    }

    onFilterChanged: results.currentIndex = 0

    background: Rectangle {
        color: Theme.surfaceRaised
        border.color: Theme.border
        radius: 6
    }

    contentItem: ColumnLayout {
        spacing: Theme.space1

        TextField {
            id: field
            objectName: "commandSearchField"
            Layout.fillWidth: true
            placeholderText: qsTr("Search commands")
            onTextChanged: root.filter = text
            Keys.onDownPressed: results.currentIndex = Math.min(results.count - 1, results.currentIndex + 1)
            Keys.onUpPressed: results.currentIndex = Math.max(0, results.currentIndex - 1)
            onAccepted: root.run(results.currentIndex)
        }

        ListView {
            id: results
            objectName: "commandSearchResults"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 280)
            visible: count > 0
            clip: true
            model: root.matches()
            highlightMoveDuration: 0
            delegate: ItemDelegate {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                enabled: modelData.item.enabled
                onClicked: root.run(index)
                contentItem: RowLayout {
                    spacing: Theme.space2
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            elide: Text.ElideRight
                            opacity: row.enabled ? 1 : 0.45
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.path
                            elide: Text.ElideRight
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    Label {
                        text: row.modelData.keys
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }
            }
        }

        Label {
            visible: root.filter.trim() !== "" && results.count === 0
            text: qsTr("No command matches \"%1\"").arg(root.filter.trim())
            color: Theme.textFaint
            font.pixelSize: Theme.fontSmall
        }
    }
}
