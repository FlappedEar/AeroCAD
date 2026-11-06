# SPDX-License-Identifier: GPL-3.0-or-later
include(Catch)

# aerocad_add_test(<name> SOURCES ... LIBS ...)
function(aerocad_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Catch2::Catch2WithMain)
    target_compile_definitions(${name} PRIVATE
        AEROCAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    aerocad_set_warnings(${name})
    catch_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

aerocad_add_test(test_core
    SOURCES core/test_mesh.cpp
    LIBS AeroCAD::core)

aerocad_add_test(test_geometry
    SOURCES geometry/test_geometry.cpp geometry/test_modeling.cpp
    LIBS AeroCAD::geometry)

aerocad_add_test(test_io
    SOURCES io/test_import.cpp
    LIBS AeroCAD::io AeroCAD::geometry_occt)

aerocad_add_test(test_model
    SOURCES model/test_document.cpp
    LIBS AeroCAD::model)

aerocad_add_test(test_commands
    SOURCES commands/test_commands.cpp commands/test_design_commands.cpp commands/test_simulation_commands.cpp
    LIBS AeroCAD::commands)

aerocad_add_test(test_cad
    SOURCES cad/test_design.cpp
    LIBS AeroCAD::cad)

aerocad_add_test(test_cfd
    SOURCES cfd/test_cfd.cpp
    LIBS AeroCAD::cfd)
# Real OpenFOAM run (needs Docker or a local OpenFOAM; skipped otherwise). Not in CI.
add_test(NAME cfd_openfoam_wind_tunnel COMMAND test_cfd "[openfoam]")
set_tests_properties(cfd_openfoam_wind_tunnel PROPERTIES LABELS openfoam TIMEOUT 1500 SKIP_RETURN_CODE 4)
