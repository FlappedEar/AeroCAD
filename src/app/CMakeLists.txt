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
        qml/TransformGizmo.qml
        qml/TransformPanel.qml
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
        qml/CommandSearch.qml
        qml/AppearancePopup.qml
        qml/ScanPanel.qml
        qml/SimulationPanel.qml
        qml/CfdResultsOverlay.qml
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
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Quick3D Qt6::Test
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
    # CAD-96: Create Sketch does not snap to geometry removed by Section Analysis.
    add_test(NAME app_section_sketch_snap_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/section_sketch_snap.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_section_sketch_snap_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-109: the Wind tunnel panel shows the part size and warns about implausible units.
    add_test(NAME app_cfd_size_warning_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_size_warning.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_size_warning_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-145: the Wind tunnel panel warns about a part that is not closed (raw scan shell).
    add_test(NAME app_cfd_open_warning_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_open_warning.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_open_warning_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-147: the road height under a moving floor, typed in the Wind tunnel panel.
    add_test(NAME app_cfd_floor_height_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_floor_height.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_floor_height_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-floor-height-runs")
    # CAD-150: no quality verdict while a run is still running (stand-in docker, no OpenFOAM).
    add_test(NAME app_cfd_running_checks_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_running_checks.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_running_checks_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-running-checks-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh")
    # CAD-184: Results names downforce and shows L/D and CdA / ClA (injected results, no engine).
    add_test(NAME app_cfd_efficiency_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_efficiency.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_efficiency_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-efficiency-runs")
    # CAD-191: the iteration swing in Results and "noise" in the A/B table (injected data, no engine).
    add_test(NAME app_cfd_noise_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_noise.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_noise_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-noise-runs")
    # CAD-194: aero balance in Results and the axle lift rows in A/B (injected data, no engine).
    add_test(NAME app_cfd_balance_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_balance.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_balance_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-balance-runs")
    # CAD-188: yaw (crosswind) in the New run form (prepared cases, no engine).
    add_test(NAME app_cfd_yaw_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_yaw.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_yaw_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-yaw-runs")
    # CAD-158: a running run shows its current (provisional) values (stand-in docker, no OpenFOAM).
    add_test(NAME app_cfd_running_values_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_running_values.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_running_values_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-running-values-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_HISTORY=1")
    # CAD-174: rename a run in the run list (prepared case, no engine).
    add_test(NAME app_cfd_rename_run_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_rename_run.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_rename_run_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-rename-run-runs")
    # CAD-167: Results > Export CSV… (stand-in docker, no OpenFOAM).
    add_test(NAME app_cfd_export_csv_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_export_csv.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_export_csv_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-export-csv-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_HISTORY=1;QML_XHR_ALLOW_FILE_READ=1")
    # CAD-205: the run list shows a finished run's final result (stand-in docker).
    add_test(NAME app_cfd_run_list_final_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_run_list_final.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_run_list_final_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-run-list-final-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_HISTORY=1;FAKE_DOCKER_FINISH=1")
    # CAD-207: rank the run list by the final drag, Cd or downforce (injected runs).
    add_test(NAME app_cfd_run_sort_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_run_sort.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_run_sort_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-run-sort-runs")
    # CAD-208: Results > At speed scales the final forces by speed² (injected results).
    add_test(NAME app_cfd_scale_speed_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_scale_speed.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_scale_speed_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-scale-speed-runs")
    # CAD-211: File > New clears the CFD panel's At speed text and B results.
    add_test(NAME app_cfd_new_resets_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_new_resets.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_new_resets_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-new-resets-runs")
    # CAD-193: A/B > Export CSV… (prepared runs, no engine).
    add_test(NAME app_cfd_export_ab_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_export_ab.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_export_ab_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-export-ab-runs;QML_XHR_ALLOW_FILE_READ=1")
    # CAD-204: A/B > Swap A ↔ B (prepared runs, no engine).
    add_test(NAME app_cfd_swap_ab_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_swap_ab.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_swap_ab_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-swap-ab-runs")
    # CAD-177: hovering a Results chart shows the values at the nearest iteration (stand-in docker).
    add_test(NAME app_cfd_chart_readout_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_chart_readout.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_chart_readout_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-chart-readout-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_HISTORY=1")
    # CAD-180: the wind tunnel speed in m/s, km/h or mph, with a decimal comma (no engine).
    add_test(NAME app_cfd_speed_units_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_speed_units.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_speed_units_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-speed-units-runs")
    # CAD-181: each run in the run list names its conditions (prepared cases, no engine).
    add_test(NAME app_cfd_run_conditions_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_run_conditions.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_run_conditions_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-run-conditions-runs")
    # CAD-182: the reference area of Cd/Cl/Cs in the New run form (prepared cases, no engine).
    add_test(NAME app_cfd_ref_area_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_ref_area.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_ref_area_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-ref-area-runs")
    # CAD-162: the New run form takes the settings of the selected run (prepared cases, no engine).
    add_test(NAME app_cfd_use_settings_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_use_settings.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_use_settings_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-use-settings-runs")
    # CAD-151: stopping or deleting a run is confirmed first (stand-in docker, no OpenFOAM).
    add_test(NAME app_cfd_confirm_run_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_confirm_run.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_confirm_run_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-confirm-run-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh")
    # CAD-105: Simplify a scan, then Ctrl+Z restores the full mesh (also with a reduced display).
    add_test(NAME app_simplify_undo_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/simplify_undo.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_simplify_undo_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-110: Fit smooth surface reports the deviation its tolerance verdict uses.
    add_test(NAME app_fit_deviation_message_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fit_deviation_message.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fit_deviation_message_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-114: the outline step on a fitted surface (empty sketch, rectangle drawn by dragging).
    add_test(NAME app_surface_outline_drag_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/surface_outline_drag.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_surface_outline_drag_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-122: in a sketch Esc ends the tool, a click selects a curve and Delete removes it.
    add_test(NAME app_sketch_select_delete_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_select_delete.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_select_delete_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-123: dragging a sketch point moves it through the solver.
    add_test(NAME app_sketch_drag_point_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_drag_point.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_drag_point_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-199: Center Rectangle and Center Point Arc in the sketch.
    add_test(NAME app_sketch_center_tools_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_center_tools.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_center_tools_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-203: 3-Point Rectangle (at any angle) in the sketch.
    add_test(NAME app_sketch_rectangle_3point_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_rectangle_3point.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_rectangle_3point_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-200: 2-Point and 3-Point Circle in the sketch.
    add_test(NAME app_sketch_circle_tools_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_circle_tools.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_circle_tools_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-195: Fit Point Spline tool in the sketch.
    add_test(NAME app_sketch_spline_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_spline.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_spline_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-160: dragging timeline chips reorders features, dragging the marker rolls back.
    add_test(NAME app_timeline_reorder_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/timeline_reorder.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_timeline_reorder_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-161: Edit Profile Sketch in the timeline menu opens the extrude's sketch.
    add_test(NAME app_timeline_profile_sketch_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/timeline_profile_sketch.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_timeline_profile_sketch_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-164: editing an earlier sketch rolls the timeline back, Finish rolls forward.
    add_test(NAME app_sketch_edit_rollback_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_edit_rollback.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_edit_rollback_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-131, CAD-132: the guide leads from an outline drawn on a scan to the part and to printing.
    add_test(NAME app_scan_to_part_next_steps_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/scan_to_part_next_steps.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_scan_to_part_next_steps_test PROPERTIES LABELS gui TIMEOUT 90)
    # CAD-133: a construction plane has its own guide (not the scan's).
    add_test(NAME app_construction_plane_guide_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/construction_plane_guide.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_construction_plane_guide_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-139: a scan above the triangle budget is drawn simplified and smooth.
    add_test(NAME app_scan_lod_display_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/scan_lod_display.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_scan_lod_display_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-140: a scan saved in metres is offered a one-click conversion.
    add_test(NAME app_scan_units_check_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/scan_units_check.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_scan_units_check_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-152: drag arrows of Fillet, Chamfer and Shell; Shell of a filleted body.
    add_test(NAME app_feature_size_arrows_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/feature_size_arrows.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_feature_size_arrows_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_fillet_arrow_cone_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fillet_arrow_cone.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fillet_arrow_cone_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: Project body edges into a sketch.
    add_test(NAME app_sketch_project_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_project.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_project_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: Sketch Offset of a selected chain.
    add_test(NAME app_sketch_offset_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_offset.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_offset_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: Sketch Fillet rounds a corner of two lines.
    add_test(NAME app_sketch_fillet_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_fillet.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_fillet_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: Sketch Mirror copies curves about a line, held symmetric.
    add_test(NAME app_sketch_mirror_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_mirror.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_mirror_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: Trim removes the piece of a sketch curve between crossings.
    add_test(NAME app_sketch_trim_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_trim.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_trim_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: X turns sketch curves into construction geometry.
    add_test(NAME app_sketch_construction_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_construction.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_construction_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-206: Symmetric constraint (two points, lines or circles about a line).
    add_test(NAME app_sketch_symmetric_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_symmetric.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_symmetric_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-125: Extrude up to an object (To Object), following its curved shape.
    add_test(NAME app_extrude_to_object_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/extrude_to_object.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_extrude_to_object_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-124: Revolve about a sketch line clicked in the view (through the live preview).
    add_test(NAME app_revolve_axis_pick_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/revolve_axis_pick.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_revolve_axis_pick_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_revolve_axis_edge_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/revolve_axis_edge.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_revolve_axis_edge_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-125: drag the Extrude distance arrow (distance, Flip below the sketch).
    add_test(NAME app_extrude_drag_arrow_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/extrude_drag_arrow.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_extrude_drag_arrow_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-169: Extrude Taper Angle drafts the side faces.
    add_test(NAME app_extrude_taper_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/extrude_taper.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_extrude_taper_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-115: Export suggests a file name.
    add_test(NAME app_export_file_name_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/export_file_name.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_export_file_name_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-116: the Wind Tunnel panel's quality lines and dimmed tabs.
    add_test(NAME app_cfd_quality_lines_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_quality_lines.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # An empty run folder: no earlier runs, so Results and A/B start disabled.
    set_tests_properties(app_cfd_quality_lines_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-quality-lines-runs")
    # CAD-127, CAD-128: colour bars and forces of the wind tunnel results in the viewport.
    add_test(NAME app_cfd_results_overlay_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_results_overlay.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_results_overlay_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-results-overlay-runs")
    # CAD-127..129 on a real coarse OpenFOAM run (not in CI: label openfoam).
    add_test(NAME app_cfd_results_3d_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_results_3d.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # A layout polish loop leaves rows of the results panel unplaced (CAD-173).
    set_tests_properties(app_cfd_results_3d_test PROPERTIES LABELS "gui;openfoam" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "Polish loop detected"
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-results-3d-runs")
    # CAD-141: the A/B pressure difference on two real coarse runs (not in CI: label openfoam).
    add_test(NAME app_cfd_difference_3d_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_difference_3d.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_difference_3d_test PROPERTIES LABELS "gui;openfoam" TIMEOUT 420
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-difference-3d-runs")
    # CAD-117: the A/B table (run names, quality note, no percentage at noise level).
    add_test(NAME app_cfd_ab_table_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_ab_table.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_ab_table_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-ab-table-runs")
    # CAD-136: the run list belongs to the open design (other designs' runs folded away).
    add_test(NAME app_cfd_run_list_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_run_list.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_run_list_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-run-list-runs")
    # Live Move / Rotate / Scale (handles, panel, Enter / Esc / click away) and free orbit.
    add_test(NAME app_transform_tool_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/transform_tool.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_transform_tool_test PROPERTIES LABELS gui TIMEOUT 90)
    # Lay on Face / Face Front: orienting a part for a wind tunnel run (box and a real scan).
    add_test(NAME app_orient_for_cfd_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/orient_for_cfd.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_orient_for_cfd_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_camera_feel_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/camera_feel.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_camera_feel_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_zoom_selection_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/zoom_selection.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_zoom_selection_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_sketch_right_button_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_right_button.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_sketch_right_button_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_shortcuts_fusion_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/shortcuts_fusion.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_shortcuts_fusion_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_copy_paste_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/copy_paste.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_copy_paste_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_hover_highlight_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/hover_highlight.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_hover_highlight_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_browser_rename_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/browser_rename.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_browser_rename_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_undo_order_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/undo_order.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_undo_order_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_isolate_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/isolate.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_isolate_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_multi_select_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/multi_select.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_multi_select_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_multi_select_many_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/multi_select_many.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_multi_select_many_test PROPERTIES LABELS gui TIMEOUT 120)
    add_test(NAME app_isolate_scan_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/isolate_scan.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_isolate_scan_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_isolate_history_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/isolate_history.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_isolate_history_test PROPERTIES LABELS gui TIMEOUT 120)
    add_test(NAME app_command_search_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/command_search.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_command_search_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_appearance_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/appearance.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_appearance_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_large_scale_preview_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/large_scale_preview.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_large_scale_preview_test PROPERTIES LABELS gui TIMEOUT 60
                         FAIL_REGULAR_EXPRESSION "TypeError|ReferenceError")
    add_test(NAME app_shell_closed_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/shell_closed.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_shell_closed_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_press_pull_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/press_pull.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_press_pull_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_split_body_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/split_body.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_split_body_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_delete_face_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/delete_face.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_delete_face_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_draft_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/draft.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_draft_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_sphere_torus_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sphere_torus.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_sphere_torus_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_midplane_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/midplane.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_midplane_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_body_properties_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/body_properties.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_body_properties_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_transform_status_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/transform_status.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_transform_status_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_cfd_setup_preview_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_setup_preview.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_cfd_setup_preview_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_orient_scan_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/orient_scan.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_orient_scan_test PROPERTIES LABELS gui TIMEOUT 60)
    # Touchpad gestures from a fresh start (sideways first, diagonal applied once).
    add_test(NAME app_touchpad_gestures_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/touchpad_gestures.ui)
    set_tests_properties(app_touchpad_gestures_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-85: a fit that misses its tolerance is marked as an approximation in the UI.
    add_test(NAME app_fit_tolerance_warning_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fit_tolerance_warning.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fit_tolerance_warning_test PROPERTIES LABELS gui TIMEOUT 120)
    # CAD-86: Cancel / OK of a modelling dialog touch only the dialog's own undo steps.
    add_test(NAME app_feature_dialog_owned_history_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome.step
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/feature_dialog_owned_history.ui)
    set_tests_properties(app_feature_dialog_owned_history_test PROPERTIES LABELS gui TIMEOUT 90)
    # Runs a coarse OpenFOAM case, so it needs the engine (label openfoam, not in CI). The
    # runs directory is in the build tree: under the home directory on macOS, which the
    # engine's VM can mount.
    add_test(NAME app_new_project_reset_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/new_project_reset.ui)
    set_tests_properties(app_new_project_reset_test PROPERTIES LABELS "gui;openfoam" TIMEOUT 300
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/gui-test-runs")

    # GUI tests share one desktop (or one Xvfb display): a window that opens in another test
    # takes the focus and cancels the mouse grab of a drag under way (CAD-201). They run one
    # at a time, also under `ctest -j`; the unit tests still run in parallel next to them.
    get_property(_app_tests DIRECTORY PROPERTY TESTS)
    foreach(_test IN LISTS _app_tests)
        get_test_property(${_test} LABELS _labels)
        if("gui" IN_LIST _labels)
            set_tests_properties(${_test} PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()
endif()
