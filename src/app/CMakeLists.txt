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
    JsonBridge.cpp
    JsonBridge.hpp
    Preferences.cpp
    Preferences.hpp
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
        qml/SketchOverlay.qml
        qml/SketchConstraintLayer.qml
        qml/FeatureDialog.qml
        qml/TimelineBar.qml
        qml/ParametersDialog.qml
        qml/MeasurePanel.qml
        qml/SectionPanel.qml
        qml/HeatMapPanel.qml
        qml/HeatMapLegend.qml
        qml/PreferencesDialog.qml
        qml/ShortcutsDialog.qml
    RESOURCES
        ${AEROCAD_ICONS}
)

set_target_properties(aerocad PROPERTIES
    OUTPUT_NAME AeroCAD
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.flappedear.aerocad
    MACOSX_BUNDLE_BUNDLE_NAME AeroCAD
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

target_compile_definitions(aerocad PRIVATE AEROCAD_VERSION="${PROJECT_VERSION}")
target_include_directories(aerocad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aerocad PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Quick3D
    AeroCAD::commands AeroCAD::io aerocad_styleplugin)
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
endif()
