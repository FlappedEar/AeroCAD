# SPDX-License-Identifier: GPL-3.0-or-later
# The Qt Quick desktop application.
qt_standard_project_setup()

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
    JsonBridge.cpp
    JsonBridge.hpp
    Preferences.cpp
    Preferences.hpp
)
qt_add_executable(aerocad ${AEROCAD_APP_SOURCES})

qt_add_qml_module(aerocad
    URI AeroCAD
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        qml/Main.qml
        qml/Viewport.qml
        qml/SelectionToolbar.qml
        qml/CameraController.qml
        qml/PartsPanel.qml
        qml/PropertiesPanel.qml
        qml/ViewCube.qml
        qml/NavigationBar.qml
        qml/SketchOverlay.qml
        qml/FeatureDialog.qml
        qml/TimelineBar.qml
        qml/ParametersDialog.qml
        qml/PreferencesDialog.qml
        qml/ShortcutsDialog.qml
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
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Quick3D
    AeroCAD::commands AeroCAD::io)
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
