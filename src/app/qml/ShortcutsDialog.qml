// SPDX-License-Identifier: GPL-3.0-or-later
// Help > Keyboard Shortcuts: every menu command that has a shortcut, read from the menu
// bar at the time the dialog opens (so it never goes stale), plus viewport keys and mouse
// navigation for the selected profile.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Dialog {
    id: root

    /// The window's MenuBar.
    required property var menuBar
    /// Extra rows: [{group, name, keys}], e.g. viewport-only shortcuts.
    property var extraRows: []
    property var rows: []
    property string filter: ""

    title: qsTr("Keyboard Shortcuts")
    modal: true
    standardButtons: Dialog.Close
    width: 560
    height: 600

    function cleanText(text) {
        // Menu mnemonics: "&Import..." -> "Import..."
        return String(text).replace(/&(.)/g, "$1")
    }

    function collect(menu, group, out) {
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i)
            if (!item)
                continue
            if (item.subMenu) {
                collect(item.subMenu, group + " › " + cleanText(item.subMenu.title), out)
                continue
            }
            const action = item.action
            if (!action || action.shortcut === undefined || action.shortcut === "" || action.shortcut === null)
                continue
            const keys = Preferences.shortcutText(action.shortcut)
            if (keys !== "")
                out.push({ group: group, name: cleanText(action.text), keys: keys })
        }
    }

    function navigationRows() {
        const p = Preferences.mouseProfile
        const g = qsTr("Mouse (%1)").arg(p === "fusion" ? "Fusion" : p === "solidworks" ? "SolidWorks" : "Blender")
        const orbit = (p === "fusion" ? qsTr("Shift + middle drag") : qsTr("Middle drag")) + qsTr(", or right drag")
        const pan = (p === "fusion" ? qsTr("Middle drag") : p === "solidworks" ? qsTr("Ctrl + middle drag")
                                                                               : qsTr("Shift + middle drag"))
                    + qsTr(", or Shift + right drag")
        const scrollPans = Preferences.touchpadScrollPans
        const rows = [
            { group: g, name: qsTr("Orbit (around the point under the pointer)"), keys: orbit },
            { group: g, name: qsTr("Pan"), keys: pan },
            { group: g, name: qsTr("Zoom (towards the pointer)"), keys: qsTr("Wheel") },
            { group: g, name: qsTr("Select"), keys: qsTr("Left click") },
            { group: g, name: qsTr("Context menu"), keys: qsTr("Right click") },
            { group: g, name: qsTr("Touchpad zoom / pan / orbit"),
              keys: scrollPans ? qsTr("Pinch or Ctrl + two fingers / two fingers / Shift + two fingers")
                               : qsTr("Two fingers or pinch / Ctrl + two fingers / Shift + two fingers") }
        ]
        if (p !== "fusion")
            rows.splice(3, 0, { group: g, name: qsTr("Zoom (drag)"),
                                keys: p === "solidworks" ? qsTr("Shift + middle drag") : qsTr("Ctrl + middle drag") })
        return rows
    }

    function rebuild() {
        rowModel.clear()
        const f = filter.toLowerCase()
        for (const r of rows) {
            if (f === "" || (r.group + " " + r.name + " " + r.keys).toLowerCase().includes(f))
                rowModel.append(r)
        }
    }

    function refresh() {
        const out = []
        for (let i = 0; i < menuBar.count; ++i) {
            const menu = menuBar.menuAt(i)
            if (menu)
                collect(menu, cleanText(menu.title), out)
        }
        rows = out.concat(extraRows, navigationRows())
        rebuild()
    }

    onFilterChanged: rebuild()

    ListModel { id: rowModel }

    onAboutToShow: { filter = ""; refresh() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        TextField {
            Layout.fillWidth: true
            placeholderText: qsTr("Search commands or keys")
            text: root.filter
            onTextChanged: root.filter = text
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: rowModel
            section.property: "group"
            section.delegate: Label {
                required property string section
                text: section
                font.bold: true
                topPadding: 8
                bottomPadding: 2
            }
            delegate: RowLayout {
                required property string name
                required property string keys
                width: ListView.view.width
                Label { text: name; Layout.fillWidth: true; elide: Text.ElideRight }
                Label { text: keys; opacity: 0.85 }
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }
}
