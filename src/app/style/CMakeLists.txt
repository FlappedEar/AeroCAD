# SPDX-License-Identifier: GPL-3.0-or-later
# AeroCADStyle: the Qt Quick Controls style of the application. Every Button, TextField,
# ComboBox, Menu, Dialog, ... in the app is drawn by these files with the Theme.qml tokens,
# so panels added by any feature look the same in light and dark. Controls not listed here
# fall back to the Basic style (IMPORTS), which draws from the palette Main.qml sets.
qt_add_qml_module(aerocad_style
    URI AeroCADStyle
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    IMPORTS QtQuick.Controls.Basic
    QML_FILES
        Button.qml
        CheckBox.qml
        ComboBox.qml
        Dialog.qml
        DialogButtonBox.qml
        Frame.qml
        GroupBox.qml
        ItemDelegate.qml
        Menu.qml
        MenuBar.qml
        MenuBarItem.qml
        MenuItem.qml
        MenuSeparator.qml
        Pane.qml
        Popup.qml
        PopupBackground.qml
        RadioButton.qml
        ScrollBar.qml
        Slider.qml
        SpinBox.qml
        SplitView.qml
        Switch.qml
        TabBar.qml
        TabButton.qml
        TextArea.qml
        TextField.qml
        ToolBar.qml
        ToolButton.qml
        ToolSeparator.qml
        ToolTip.qml
)
target_link_libraries(aerocad_style PRIVATE Qt6::Quick Qt6::QuickControls2)
