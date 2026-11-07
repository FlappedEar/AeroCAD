# SPDX-License-Identifier: GPL-3.0-or-later
# The Qt Quick desktop application.
qt_standard_project_setup()

add_subdirectory(style)

set(AEROCAD_APP_SOURCES
    main.cpp
    AppController.cpp
    AppController.hpp
    PartListModel.cpp
    PartListModel.hpp
    MeshGeometry.cpp
    MeshGeometry.hpp
    SelectionGeometry.cpp
    SelectionGeometry.hpp
    LineGeometry.cpp
    LineGeometry.hpp
    InspectGeometry.cpp
    InspectGeometry.hpp
    GridGeometry.cpp
    GridGeometry.hpp
    FieldGeometry.cpp
    FieldGeometry.hpp
    JsonBridge.cpp
    JsonBridge.hpp
    McpAccess.cpp
    McpAccess.hpp
    Preferences.cpp
    Preferences.hpp
    ScanController.cpp
    ScanController.hpp
    UiScript.cpp
    UiScript.hpp
)
qt_add_executable(aerocad ${AEROCAD_APP_SOURCES})

# Design tokens (colours, sizes, icons) shared by every QML file, see docs/ui.md.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

file(GLOB AEROCAD_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS icons/*.svg)

qt_add_qml_module(aerocad
    URI AeroCAD
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/AppIcon.qml
        qml/IconButton.qml
        qml/RibbonButton.qml
        qml/RibbonGroup.qml
        qml/RibbonTab.qml
        qml/PanelHeader.qml
        qml/ContextActions.qml
        qml/ActionPopup.qml
        qml/ActionButton.qml
        qml/GuidePanel.qml
        qml/ContextBar.qml
        qml/PartContextMenu.qml
        qml/StartCard.qml
        qml/Viewport.qml
        qml/SelectionToolbar.qml
        qml/CameraController.qml
        qml/PartsPanel.qml
        qml/PropertiesPanel.qml
        qml/ViewCube.qml
        qml/NavigationBar.qml
        qml/McpPanel.qml
        qml/SketchOverlay.qml
        qml/SketchConstraintLayer.qml
        qml/ExportCheckDialog.qml
        qml/FeatureDialog.qml
        qml/TimelineBar.qml
        qml/ParametersDialog.qml
        qml/MeasurePanel.qml
        qml/SectionPanel.qml
        qml/HeatMapPanel.qml
        qml/HeatMapLegend.qml
        qml/PreferencesDialog.qml
        qml/ShortcutsDialog.qml
        qml/ScanPanel.qml
        qml/SimulationPanel.qml
        qml/LineChart.qml
    RESOURCES
        ${AEROCAD_ICONS}
)

string(TIMESTAMP AEROCAD_BUNDLE_VERSION "%Y.%m.%d")
set_target_properties(aerocad PROPERTIES
    OUTPUT_NAME AeroCAD
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.flappedear.aerocad
    MACOSX_BUNDLE_BUNDLE_NAME AeroCAD
    # macOS needs a numeric bundle version: the configure date (the full build stamp is in the app).
    MACOSX_BUNDLE_BUNDLE_VERSION ${AEROCAD_BUNDLE_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${AEROCAD_BUNDLE_VERSION}
)

target_include_directories(aerocad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aerocad PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Quick3D
    AeroCAD::buildinfo AeroCAD::commands AeroCAD::io AeroCAD::mcp_ipc aerocad_styleplugin)

# Bundle the MCP server with the application: next to the executable, and inside the
# macOS app bundle (Contents/MacOS) so the app can show its path to the user.
add_dependencies(aerocad aerocad_mcp_server)
if(APPLE)
    add_custom_command(TARGET aerocad POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:aerocad_mcp_server>
                $<TARGET_BUNDLE_CONTENT_DIR:aerocad>/MacOS/
        VERBATIM)
endif()
if(MSVC)
    target_compile_options(aerocad PRIVATE /utf-8 /Zc:__cplusplus)
endif()
aerocad_set_source_warnings(${AEROCAD_APP_SOURCES})

install(TARGETS aerocad
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Smoke test: start the app headless, import a file through the command layer,
# render a frame and quit. Labelled "gui" so it can be selected/excluded with ctest -L/-LE.
if(AEROCAD_BUILD_TESTS)
    add_test(NAME app_smoke_test
        COMMAND aerocad --smoke-test ${PROJECT_SOURCE_DIR}/tests/data/tetrahedron.stl)
    set_tests_properties(app_smoke_test PROPERTIES LABELS gui TIMEOUT 60)
    # Every ComboBox popup row shows its text, for each kind of model the app uses.
    add_test(NAME app_combobox_popup_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/combobox_popup.ui)
    set_tests_properties(app_combobox_popup_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-79: dropping a project file asks about unsaved changes like File > Open. The
    # fixture writes project B and a broken project file next to a copy of a mesh; the
    # recovery copies go to a private data directory.
    set(_drop ${CMAKE_CURRENT_BINARY_DIR}/gui-drop)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gui-drop-setup.json
        "[{\"name\": \"feature.box\", \"params\": {\"size\": [30, 20, 10]}},\n"
        " {\"name\": \"project.save\", \"params\": {\"path\": \"${_drop}/drop/B.aerocad\"}}]\n")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gui-drop-setup.cmake
        "file(REMOVE_RECURSE \"${_drop}\")\n"
        "file(MAKE_DIRECTORY \"${_drop}/drop\" \"${_drop}/data\")\n"
        "file(COPY_FILE \"${PROJECT_SOURCE_DIR}/tests/data/tetrahedron.stl\" \"${_drop}/drop/tetrahedron.stl\")\n"
        "file(WRITE \"${_drop}/drop/broken.aerocad\" \"not a project\")\n"
        "execute_process(COMMAND \"\${CMD}\" \"${CMAKE_CURRENT_BINARY_DIR}/gui-drop-setup.json\" RESULT_VARIABLE rc)\n"
        "if(NOT rc EQUAL 0 OR NOT EXISTS \"${_drop}/drop/B.aerocad\")\n  message(FATAL_ERROR \"could not write B.aerocad\")\nendif()\n")
    add_test(NAME app_drop_project_setup
        COMMAND ${CMAKE_COMMAND} -DCMD=$<TARGET_FILE:aerocad_cmd> -P ${CMAKE_CURRENT_BINARY_DIR}/gui-drop-setup.cmake)
    set_tests_properties(app_drop_project_setup PROPERTIES LABELS gui FIXTURES_SETUP gui_drop)
    add_test(NAME app_drop_project_unsaved_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/tetrahedron.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/drop_project_unsaved.ui
        WORKING_DIRECTORY ${_drop})
    set_tests_properties(app_drop_project_unsaved_test PROPERTIES LABELS gui TIMEOUT 90 FIXTURES_REQUIRED gui_drop RESOURCE_LOCK gui_drop_dir
        ENVIRONMENT "XDG_DATA_HOME=${_drop}/data;XDG_CONFIG_HOME=${_drop}/data")
    # CAD-80: imports queued before New / Open are discarded and keep their import units.
    add_test(NAME app_import_after_project_replaced_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/import_after_project_replaced.ui
        WORKING_DIRECTORY ${_drop})
    set_tests_properties(app_import_after_project_replaced_test PROPERTIES LABELS gui TIMEOUT 90
        FIXTURES_REQUIRED gui_drop RESOURCE_LOCK gui_drop_dir
        ENVIRONMENT "XDG_DATA_HOME=${_drop}/data;XDG_CONFIG_HOME=${_drop}/data")
    # Wheel zoom towards the pointer, right-drag orbit, right-click menus, touchpad, Move/Rotate.
    add_test(NAME app_navigation_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/navigation.ui)
    set_tests_properties(app_navigation_test PROPERTIES LABELS gui TIMEOUT 60)
    # Touchpad gestures from a fresh start (sideways first, diagonal applied once).
    add_test(NAME app_touchpad_gestures_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/touchpad_gestures.ui)
    set_tests_properties(app_touchpad_gestures_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-85: a fit that misses its tolerance is marked as an approximation in the UI.
    add_test(NAME app_fit_tolerance_warning_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fit_tolerance_warning.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fit_tolerance_warning_test PROPERTIES LABELS gui TIMEOUT 120)
    # Runs a coarse OpenFOAM case, so it needs the engine (label openfoam, not in CI). The
    # runs directory is in the build tree: under the home directory on macOS, which the
    # engine's VM can mount.
    add_test(NAME app_new_project_reset_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/new_project_reset.ui)
    set_tests_properties(app_new_project_reset_test PROPERTIES LABELS "gui;openfoam" TIMEOUT 300
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/gui-test-runs")
endif()
