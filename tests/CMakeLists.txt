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
    SOURCES core/test_mesh.cpp core/test_selection.cpp core/test_mesh_check.cpp core/test_scan.cpp
        core/test_mesh_inspect.cpp core/test_measure.cpp
    LIBS AeroCAD::core)

aerocad_add_test(test_geometry
    SOURCES geometry/test_geometry.cpp geometry/test_modeling.cpp geometry/test_surface_fit.cpp geometry/test_region_fit.cpp
        geometry/test_inspect.cpp
    LIBS AeroCAD::geometry)

aerocad_add_test(test_io
    SOURCES io/test_import.cpp io/test_export.cpp
    LIBS AeroCAD::io AeroCAD::geometry_occt miniz::miniz)

aerocad_add_test(test_model
    SOURCES model/test_document.cpp
    LIBS AeroCAD::model)

aerocad_add_test(test_commands
    SOURCES commands/test_commands.cpp commands/test_canards.cpp commands/test_export_commands.cpp commands/test_design_commands.cpp
        commands/test_project_commands.cpp commands/test_scan_commands.cpp commands/test_inspect_commands.cpp
        commands/test_simulation_commands.cpp
    LIBS AeroCAD::commands)

aerocad_add_test(test_mcp
    SOURCES mcp/test_mcp_server.cpp
    LIBS AeroCAD::mcp)

if(AEROCAD_BUILD_APP)
    aerocad_add_test(test_mcp_ipc
        SOURCES mcp/test_mcp_ipc.cpp
        LIBS AeroCAD::mcp_ipc)

    # The real aerocad-mcp executable over stdio, in headless mode (no GUI needed).
    add_test(NAME mcp_stdio_smoke_test
        COMMAND ${CMAKE_COMMAND} -DSERVER=$<TARGET_FILE:aerocad_mcp_server>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/mcp/stdio_session.jsonl
                -P ${CMAKE_CURRENT_SOURCE_DIR}/mcp/stdio_smoke_test.cmake)
    set_tests_properties(mcp_stdio_smoke_test PROPERTIES TIMEOUT 60)
endif()

aerocad_add_test(test_cad
    SOURCES cad/test_design.cpp cad/test_sketch_solver.cpp
    LIBS AeroCAD::cad)

aerocad_add_test(test_project
    SOURCES project/test_project_file.cpp
    LIBS AeroCAD::project miniz::miniz)

aerocad_add_test(test_cfd
    SOURCES cfd/test_cfd.cpp
    LIBS AeroCAD::cfd)
# Real OpenFOAM run (needs Docker or a local OpenFOAM; skipped otherwise). Not in CI.
add_test(NAME cfd_openfoam_wind_tunnel COMMAND test_cfd "[openfoam]")
set_tests_properties(cfd_openfoam_wind_tunnel PROPERTIES LABELS openfoam TIMEOUT 1500 SKIP_RETURN_CODE 4)

aerocad_add_test(test_e2e
    SOURCES e2e/test_workflow.cpp
    LIBS AeroCAD::commands)
