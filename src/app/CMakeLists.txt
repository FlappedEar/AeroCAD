# SPDX-License-Identifier: GPL-3.0-or-later
# The Qt Quick desktop application.
qt_standard_project_setup()

add_subdirectory(style)

set(AEROCAD_APP_SOURCES
    main.cpp
    AppController.cpp
    AppController.hpp
    PickFilter.hpp
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
    AgentView.cpp
    AgentView.hpp
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
    # CAD-12: a persisted unitless-file setting cannot rescale embedded file units.
    set(_import_units ${CMAKE_CURRENT_BINARY_DIR}/gui-import-units)
    add_test(NAME app_import_file_units_test
        COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:aerocad> -DOUTPUT_DIR=${_import_units}
                -P ${PROJECT_SOURCE_DIR}/tests/gui/import_file_units.cmake)
    set_tests_properties(app_import_file_units_test PROPERTIES LABELS gui TIMEOUT 90
        ENVIRONMENT "XDG_DATA_HOME=${_import_units}/data;XDG_CONFIG_HOME=${_import_units}/config")
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
    # An MCP agent's view tools: camera, screenshot, pick, results (view.* commands).
    add_test(NAME app_agent_view_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/agent_view.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_agent_view_test PROPERTIES LABELS gui TIMEOUT 60)
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
    # CAD-213: Run CFD and the run list on one screen; less used settings under "More settings".
    add_test(NAME app_cfd_run_tab_fold_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_run_tab_fold.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_run_tab_fold_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-run-tab-fold-runs")
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
    # CAD-215: deleting run A clears B and the old pair's field data (fake engine).
    add_test(NAME app_cfd_delete_a_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_delete_a.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_delete_a_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-delete-a-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_HISTORY=1;FAKE_DOCKER_FINISH=1")
    # CAD-220: a run that fails before the first iteration shows its error, not empty results.
    add_test(NAME app_cfd_failed_run_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_failed_run.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_failed_run_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-failed-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_FAIL=1")
    # CAD-221: a stopped run keeps the values it computed so far.
    add_test(NAME app_cfd_stopped_values_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/cfd_stopped_values.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_cfd_stopped_values_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "AEROCAD_CFD_DIR=${CMAKE_CURRENT_BINARY_DIR}/cfd-stopped-runs;AEROCAD_DOCKER=${PROJECT_SOURCE_DIR}/tests/gui/fake-docker.sh;FAKE_DOCKER_HISTORY=1")
    # CAD-224: an isolation is saved with the project and can be exited after reopening.
    add_test(NAME app_project_isolation_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/box_200x80x60.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/project_isolation.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # Its own settings and autosave folder: a recovery copy left by another run would open the
    # recovery question and take the keyboard (Ctrl+Z) away from the window.
    set_tests_properties(app_project_isolation_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/project-isolation-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/project-isolation-data")
    # CAD-226: crash recovery: the question names the design, the copy keeps the view.
    add_test(NAME app_project_recovery_test
        COMMAND sh ${PROJECT_SOURCE_DIR}/tests/gui/project_recovery.sh $<TARGET_FILE:aerocad> ${PROJECT_SOURCE_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/project-recovery)
    set_tests_properties(app_project_recovery_test PROPERTIES LABELS gui TIMEOUT 120)
    # CAD-227 / CAD-228: a STEP or 3MF file with several bodies imports as one named, coloured part
    # per body.
    add_test(NAME app_step_bodies_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/step_bodies.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_step_bodies_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/step-bodies-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/step-bodies-data")
    # CAD-236: the Scan Tools panel fits its width, lists open boundaries, Repair reports plainly.
    add_test(NAME app_scan_panel_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/scan_panel.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_scan_panel_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/scan-panel-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/scan-panel-data")
    add_test(NAME app_scan_panel_open_chain_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/open_chain.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/scan_panel_open_chain.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_scan_panel_open_chain_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/scan-panel-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/scan-panel-data")
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
    # CAD-259: large fits run in the background with progress, Cancel and stale-result checks.
    add_test(NAME app_fit_background_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fit_background.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fit_background_test PROPERTIES LABELS gui TIMEOUT 90)
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
    # CAD-276: the scan's Details are not shown in the Properties panel while a sketch is drawn.
    add_test(NAME app_sketch_details_hidden_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_details_hidden.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_details_hidden_test PROPERTIES LABELS gui TIMEOUT 90)
    # CAD-246, CAD-247: Both sides refuses a fin across the mirror plane; no selection toolbar under the pane.
    add_test(NAME app_fin_both_sides_centre_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fin_both_sides_centre.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fin_both_sides_centre_test PROPERTIES LABELS gui TIMEOUT 90
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/fin-centre-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/fin-centre-data")
    # CAD-250: a background recovery copy keeps the status line.
    add_test(NAME app_autosave_keeps_status_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/tetrahedron.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/autosave_keeps_status.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_autosave_keeps_status_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/autosave-status-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/autosave-status-data")
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
    set_tests_properties(app_scan_units_check_test PROPERTIES LABELS gui TIMEOUT 60
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/scan-units-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/scan-units-data")
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
    # CAD-225: Extend lengthens a sketch line or arc to the next curve.
    add_test(NAME app_sketch_extend_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_extend.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_extend_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-229: Break splits a sketch curve where other curves cross it.
    add_test(NAME app_sketch_break_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_break.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_break_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-235: Point places a sketch point that snaps, is drawn and can be deleted.
    add_test(NAME app_sketch_point_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_point.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_point_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-254: Sketch Chamfer with real clicks.
    add_test(NAME app_sketch_chamfer_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_chamfer.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_chamfer_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_sketch_ellipse_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_ellipse.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_ellipse_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-269: Polygon and Slot types with real clicks.
    add_test(NAME app_sketch_polygon_slot_types_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_polygon_slot_types.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_polygon_slot_types_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-270: Circle 2-Tangent / 3-Tangent with real clicks.
    add_test(NAME app_sketch_circle_tangent_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_circle_tangent.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_circle_tangent_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-258: Trim an ellipse into an elliptical arc with real clicks.
    add_test(NAME app_sketch_ellipse_trim_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_ellipse_trim.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_ellipse_trim_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-262: Sketch Conic Curve with real clicks.
    add_test(NAME app_sketch_conic_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_conic.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_conic_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-267: draw an elliptical arc directly with real clicks.
    add_test(NAME app_sketch_elliptical_arc_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_elliptical_arc.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_elliptical_arc_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-263: Sketch Text with real clicks.
    add_test(NAME app_sketch_text_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_text.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_text_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-260: Trim and Break a spline with real clicks.
    add_test(NAME app_sketch_spline_trim_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_spline_trim.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_spline_trim_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-253: Sketch Scale with real clicks and keys.
    add_test(NAME app_sketch_scale_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_scale.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_scale_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-251: Sketch Circular / Rectangular Pattern with real clicks.
    add_test(NAME app_sketch_pattern_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_pattern.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_pattern_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-249: an empty sketch shows no "Fully constrained" status.
    add_test(NAME app_sketch_empty_status_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_empty_status.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_empty_status_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-126: X turns sketch curves into construction geometry.
    add_test(NAME app_sketch_construction_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_construction.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_construction_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-212: Tangent Arc from the end of a line or arc.
    add_test(NAME app_sketch_tangent_arc_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_tangent_arc.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_tangent_arc_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-216: a line drawn from a dimensioned end starts where it is shown and stays joined.
    add_test(NAME app_sketch_draw_on_dimensioned_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_draw_on_dimensioned.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_draw_on_dimensioned_test PROPERTIES LABELS gui TIMEOUT 60)
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
    # CAD-268: Extrude Start Offset, Revolve Symmetric / Two Sides.
    add_test(NAME app_extent_options_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/extent_options.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_extent_options_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-271: Scale a body, non-uniform.
    add_test(NAME app_scale_body_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/scale_body.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_scale_body_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-271: Pattern on Path.
    add_test(NAME app_path_pattern_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/path_pattern.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_path_pattern_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-271: Pipe.
    add_test(NAME app_pipe_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/pipe.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_pipe_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-271: Rib.
    add_test(NAME app_rib_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/rib.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_rib_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-271: Coil.
    add_test(NAME app_coil_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/coil.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_coil_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-274: Sketch Intersect.
    add_test(NAME app_sketch_intersect_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_intersect.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_intersect_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-273: Arc Slot.
    add_test(NAME app_sketch_arc_slot_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_arc_slot.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_arc_slot_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-274: Stretch.
    add_test(NAME app_sketch_stretch_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_stretch.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_stretch_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-274: Control Point Spline.
    add_test(NAME app_sketch_cp_spline_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_cp_spline.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_cp_spline_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-273: 3-Point Arc Slot.
    add_test(NAME app_sketch_arc_slot_3pt_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_arc_slot_3pt.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_arc_slot_3pt_test PROPERTIES LABELS gui TIMEOUT 60)
    # Thread on a cylindrical face from the SOLID tab.
    add_test(NAME app_thread_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/thread.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_thread_test PROPERTIES LABELS gui TIMEOUT 60)
    # Variable radius fillet from the SOLID tab.
    add_test(NAME app_fillet_variable_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/fillet_variable.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_fillet_variable_test PROPERTIES LABELS gui TIMEOUT 60)
    # Circular Pattern axis picked from a cylindrical face or an edge in the view.
    add_test(NAME app_circular_pattern_axis_pick_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/circular_pattern_axis_pick.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_circular_pattern_axis_pick_test PROPERTIES LABELS gui TIMEOUT 60)
    # CAD-273: Circle 2-Tangent / 3-Tangent also to arcs and circles.
    add_test(NAME app_sketch_circle_tangent_arcs_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_circle_tangent_arcs.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_sketch_circle_tangent_arcs_test PROPERTIES LABELS gui TIMEOUT 60)
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
    # Navigation on a 1.5 M triangle scan: pick, camera move, wheel and ViewCube costs on the
    # UI thread, and no model hover picks under the ViewCube.
    add_executable(make_dense_mesh ${PROJECT_SOURCE_DIR}/tests/perf/make_dense_mesh.cpp)
    aerocad_set_warnings(make_dense_mesh)
    set(_dense ${CMAKE_CURRENT_BINARY_DIR}/gui-dense.stl)
    add_test(NAME app_large_model_setup COMMAND make_dense_mesh ${_dense})
    set_tests_properties(app_large_model_setup PROPERTIES LABELS gui FIXTURES_SETUP gui_dense)
    add_test(NAME app_large_model_navigation_test
        COMMAND aerocad ${_dense} --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/large_model_navigation.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_large_model_navigation_test PROPERTIES LABELS gui TIMEOUT 180
                         FIXTURES_REQUIRED gui_dense FAIL_REGULAR_EXPRESSION "TypeError|ReferenceError")
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
    add_test(NAME app_feature_dialog_checkbox_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/feature_dialog_checkbox.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_feature_dialog_checkbox_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_feature_dialog_required_picks_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/feature_dialog_required_picks.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_feature_dialog_required_picks_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_hole_sketch_points_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/hole_sketch_points.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_hole_sketch_points_test PROPERTIES LABELS gui TIMEOUT 60)
    add_test(NAME app_sketch_move_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/sketch_move.ui
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(app_sketch_move_test PROPERTIES LABELS gui TIMEOUT 60)
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
    # CAD-94: profile clicks reach the shown sketch behind the modelling preview.
    add_test(NAME app_profile_picking_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/profile_picking.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_profile_picking_test PROPERTIES LABELS gui TIMEOUT 90)
    add_test(NAME app_picking_filters_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/picking_filters.ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(app_picking_filters_test PROPERTIES LABELS gui TIMEOUT 120)
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
    # CAD-90: MCP/script/Undo context changes close editors before stale previews rebuild.
    set(_context ${CMAKE_CURRENT_BINARY_DIR}/gui-context)
    file(MAKE_DIRECTORY ${_context})
    file(WRITE ${_context}/reset.json "[{\"name\":\"project.new\"}]\n")
    file(WRITE ${_context}/open.json "[{\"name\":\"project.open\",\"params\":{\"path\":\"saved.aerocad\"}}]\n")
    file(WRITE ${_context}/missing.json "[{\"name\":\"project.open\",\"params\":{\"path\":\"missing.aerocad\"}}]\n")
    file(WRITE ${_context}/variants.json
        "[{\"name\":\"variant.activate\",\"params\":{\"variant\":\"A\"}},"
        "{\"name\":\"variant.activate\",\"params\":{\"variant\":\"B\"}}]\n")
    add_test(NAME app_editing_context_reset_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/editing_context_reset.ui
        WORKING_DIRECTORY ${_context})
    set_tests_properties(app_editing_context_reset_test PROPERTIES LABELS gui TIMEOUT 90)
    # CAD-42: migrate the old global grant, then exercise consent across project changes.
    set(_mcp ${CMAKE_CURRENT_BINARY_DIR}/gui-mcp-access)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gui-mcp-setup.cmake
        "file(REMOVE_RECURSE \"${_mcp}\")\n"
        "file(MAKE_DIRECTORY \"${_mcp}/config/FlappedEar\" \"${_mcp}/data\")\n"
        "file(WRITE \"${_mcp}/config/FlappedEar/AeroCAD.conf\" \"[mcp]\\nenabled=true\\n\")\n")
    add_test(NAME app_mcp_access_setup
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/gui-mcp-setup.cmake)
    set_tests_properties(app_mcp_access_setup PROPERTIES LABELS gui FIXTURES_SETUP gui_mcp)
    add_test(NAME app_mcp_project_access_test
        COMMAND aerocad --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/mcp_project_access.ui
        WORKING_DIRECTORY ${_mcp})
    set_tests_properties(app_mcp_project_access_test PROPERTIES LABELS gui TIMEOUT 90 FIXTURES_REQUIRED gui_mcp
        ENVIRONMENT "XDG_DATA_HOME=${_mcp}/data;XDG_CONFIG_HOME=${_mcp}/config")
    # CAD-92: an MCP edit while the warning modal is open needs a fresh export approval.
    set(CAD92_DIR ${CMAKE_CURRENT_BINARY_DIR}/gui-export-preflight)
    foreach(_file export_preflight.ui export_preflight_setup.cmake export_preflight_verify.cmake export_open_scan.ui)
        configure_file(${PROJECT_SOURCE_DIR}/tests/gui/${_file}.in ${CMAKE_CURRENT_BINARY_DIR}/${_file} @ONLY)
    endforeach()
    add_test(NAME app_export_preflight_setup
        COMMAND ${CMAKE_COMMAND} -DCMD=$<TARGET_FILE:aerocad_cmd>
                -P ${CMAKE_CURRENT_BINARY_DIR}/export_preflight_setup.cmake)
    set_tests_properties(app_export_preflight_setup PROPERTIES LABELS gui FIXTURES_SETUP gui_export_preflight)
    add_test(NAME app_export_preflight_test
        COMMAND aerocad ${CAD92_DIR}/fixture.aerocad --ui-script ${CMAKE_CURRENT_BINARY_DIR}/export_preflight.ui)
    set_tests_properties(app_export_preflight_test PROPERTIES LABELS gui TIMEOUT 90
        FIXTURES_REQUIRED gui_export_preflight
        RESOURCE_LOCK gui_export_preflight_dir
        ENVIRONMENT "XDG_DATA_HOME=${CAD92_DIR}/data;XDG_CONFIG_HOME=${CAD92_DIR}/data")
    add_test(NAME app_export_preflight_verify
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/export_preflight_verify.cmake)
    set_tests_properties(app_export_preflight_verify PROPERTIES LABELS gui
        FIXTURES_REQUIRED gui_export_preflight DEPENDS app_export_preflight_test)
    add_test(NAME app_export_open_scan_test
        COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome_scan.stl
                --ui-script ${CMAKE_CURRENT_BINARY_DIR}/export_open_scan.ui)
    set_tests_properties(app_export_open_scan_test PROPERTIES LABELS gui TIMEOUT 90
        FIXTURES_REQUIRED gui_export_preflight
        RESOURCE_LOCK gui_export_preflight_dir
        ENVIRONMENT "XDG_DATA_HOME=${CAD92_DIR}/data;XDG_CONFIG_HOME=${CAD92_DIR}/data")
    # CAD-78: fitted surfaces remain editable through the timeline and modelling tools.
    foreach(_surface_test surface_timeline surface_timeline_move surface_timeline_combine)
        add_test(NAME app_${_surface_test}_test
            COMMAND aerocad ${PROJECT_SOURCE_DIR}/tests/data/models/dome.stl
                    --ui-script ${PROJECT_SOURCE_DIR}/tests/gui/${_surface_test}.ui)
        set_tests_properties(app_${_surface_test}_test PROPERTIES LABELS gui TIMEOUT 180
            RESOURCE_LOCK gui_surface_timeline)
    endforeach()
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
