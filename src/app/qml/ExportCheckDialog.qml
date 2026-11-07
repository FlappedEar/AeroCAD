// SPDX-License-Identifier: GPL-3.0-or-later
// Check before export: shown when document.export_check finds problems in the parts about
// to be written. One row per part with what is wrong in plain words. Errors (not a closed
// solid, STEP without exact geometry) leave only Cancel; warnings (thin walls, B-rep
// defects) can be exported anyway. "Show Thin Walls" colours the thinnest part by wall
// thickness: red is thinner than the limit, blue twice the limit or more.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AeroCAD

Dialog {
    id: root

    required property AppController controller
    /// document.export_check result.
    property var check: ({ parts: [], summary: "", canExport: true, minWall: 0 })
    /// Called when the user exports anyway.
    property var onExport: null
    property bool changedSinceCheck: false

    readonly property var problemParts: (check.parts || []).filter(p => p.status !== "ok")
    readonly property var thinPart: {
        let best = null
        for (const p of (check.parts || []))
            if (p.minWall && p.minWall.thinnest < check.minWall && (!best || p.minWall.thinnest < best.minWall.thinnest))
                best = p
        return best
    }

    title: check.canExport ? qsTr("Check before export") : qsTr("Cannot export yet")
    anchors.centerIn: Overlay.overlay
    modal: true
    width: 560

    function show(result, exportAction, changed) {
        check = result
        onExport = exportAction
        changedSinceCheck = !!changed
        open()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space2

        Label {
            Layout.fillWidth: true
            visible: root.changedSinceCheck
            wrapMode: Text.Wrap
            text: qsTr("The design or export settings changed. Review this updated check before exporting.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.check.passed ? qsTr("The current export check passed.") : root.check.canExport
                  ? qsTr("The file can be written, but look at these first.")
                  : qsTr("Fix these first, or the file will not open or print correctly.")
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(rows.implicitHeight, 320)
            clip: true

            ColumnLayout {
                id: rows
                width: root.availableWidth
                spacing: Theme.space2

                Repeater {
                    model: root.problemParts
                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        readonly property bool failed: modelData.status === "error"
                        Layout.fillWidth: true
                        spacing: Theme.space2

                        AppIcon {
                            Layout.alignment: Qt.AlignTop
                            name: "warning"
                            color: row.failed ? Theme.danger : Theme.warning
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label { text: row.modelData.name; font.bold: true }
                            Repeater {
                                model: row.modelData.errors.concat(row.modelData.warnings)
                                delegate: Label {
                                    required property string modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: index < row.modelData.errors.length ? Theme.danger : Theme.text
                                    text: "• " + modelData
                                }
                            }
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            text: root.check.summary + (root.check.minWall > 0
                  ? " " + qsTr("Thinnest wall allowed: %1 mm (Preferences).").arg(root.check.minWall) : "")
        }
    }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Show Thin Walls")
            visible: !!root.thinPart
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                root.controller.execute("inspect.thickness", { id: root.thinPart.id, min: root.check.minWall, max: 2 * root.check.minWall })
                root.reject()
            }
        }
        Button {
            text: root.check.passed ? qsTr("Export") : qsTr("Export Anyway")
            visible: root.check.canExport
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: qsTr("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    onAccepted: {
        const action = onExport
        onExport = null
        if (action) action()
    }
    onRejected: onExport = null
}
