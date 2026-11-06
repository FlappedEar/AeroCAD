cmake_minimum_required(VERSION 3.20)
project(mesh_to_brep_spike CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCASCADE REQUIRED)

add_executable(mesh_to_brep main.cpp)
target_include_directories(mesh_to_brep PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(mesh_to_brep PRIVATE
    TKernel TKMath TKG3d TKGeomBase TKGeomAlgo TKBRep TKTopAlgo TKPrim TKBO TKBool
    TKOffset TKFillet TKShHealing TKMesh TKSTL TKSTEP TKXSBase)

enable_testing()
# Synthetic noisy mesh -> fitted surface -> trim, thicken, extrude, fuse, fillet, export.
add_test(NAME mesh_to_brep_synthetic COMMAND mesh_to_brep WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(mesh_to_brep_synthetic PROPERTIES TIMEOUT 300)
