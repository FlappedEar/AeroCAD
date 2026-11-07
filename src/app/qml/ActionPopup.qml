// SPDX-License-Identifier: GPL-3.0-or-later
// Asks for the parameters of a ContextActions action (thickness, tolerance, ...) in a
// small popup with an explanation, then runs it. Enter applies, Esc cancels.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Popup {
    id: root

    required property ContextActions registry
    property var action: null
    property var info: ({})
    property var values: ({})

    /// Runs `act` on `partInfo` right away, or asks for its parameters first. `anchor`
    /// (optional) is the item the popup opens under.
    function request(act, partInfo, anchor) {
        if (!act || !act.enabled)
            return
        if (!act.params || act.params.length === 0) {
            registry.run(act.id, partInfo, {})
            return
        }
        // Rebuild the form even for the same action object, so the fields show the
        // defaults that `values` is reset to (not text left from a cancelled edit).
        action = null
        action = act
        info = partInfo
        const v = {}
        for (const p of act.params)
            v[p.key] = p.value
        values = v
        _anchor = anchor && anchor.Window.window ? anchor : null
        _place()
        open()
    }

    // The form's height is known only once its fields are laid out.
    onHeightChanged: if (visible) _place()
    onImplicitHeightChanged: if (visible) _place()

    property Item _anchor: null

    /// Below the anchor, or above it when there is no room below (the context bar sits
    /// at the bottom of the 3D view); always inside the window.
    function _place() {
        const ov = Overlay.overlay
        const h = Math.max(height, implicitHeight)
        if (!_anchor) {
            x = (ov.width - width) / 2
            y = (ov.height - h) / 3
            return
        }
        const below = _anchor.mapToItem(ov, 0, _anchor.height + 4)
        const above = _anchor.mapToItem(ov, 0, -4)
        x = Math.max(8, Math.min(below.x, ov.width - width - 8))
        y = below.y + h <= ov.height - 8 ? below.y : Math.max(8, above.y - h)
    }

    function apply() {
        const act = action
        close()
        registry.run(act.id, info, values)
    }

    // Enter applies wherever the focus is in the form (a text field or a choice).
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: root.opened
        onActivated: root.apply()
    }

    parent: Overlay.overlay
    modal: false
    focus: true
    width: 360
    padding: Theme.space4
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    contentItem: ColumnLayout {
        spacing: Theme.space2

        RowLayout {
            spacing: Theme.space2
            AppIcon { name: root.action ? root.action.icon : ""; size: Theme.iconMedium; color: Theme.accent }
            Label {
                Layout.fillWidth: true
                text: root.action ? root.action.text.replace("…", "") : ""
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.action ? root.action.tip : ""
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSmall
        }

        Repeater {
            model: root.action ? root.action.params : []
            delegate: ColumnLayout {
                id: field
                required property var modelData
                required property int index
                Layout.fillWidth: true
                Layout.topMargin: Theme.space1
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    Label { text: field.modelData.label; Layout.preferredWidth: 80 }
                    TextField {
                        id: numberField
                        visible: field.modelData.type === "number"
                        Layout.fillWidth: true
                        text: field.modelData.type === "number" ? String(field.modelData.value) : ""
                        validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                        selectByMouse: true
                        Component.onCompleted: if (field.index === 0 && visible) { forceActiveFocus(); selectAll() }
                        onTextChanged: {
                            const n = Number(String(text).replace(",", "."))
                            if (!isNaN(n))
                                root.values[field.modelData.key] = n
                        }
                        onAccepted: root.apply()
                    }
                    Label {
                        visible: field.modelData.type === "number" && (field.modelData.unit ?? "").length > 0
                        text: field.modelData.unit ?? ""
                        color: Theme.textMuted
                    }
                    ComboBox {
                        visible: field.modelData.type !== "number"
                        Layout.fillWidth: true
                        model: field.modelData.options ?? []
                        textRole: "label"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = Math.max(0, indexOfValue(field.modelData.value))
                        onActivated: root.values[field.modelData.key] = currentValue
                    }
                }
                Label {
                    visible: (field.modelData.help ?? "").length > 0
                    Layout.fillWidth: true
                    Layout.leftMargin: 80 + Theme.space2
                    text: field.modelData.help ?? ""
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space2
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: root.close() }
            Button { text: qsTr("Apply"); highlighted: true; onClicked: root.apply() }
        }
    }
}
