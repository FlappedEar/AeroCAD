# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dependency resolution.
#
# Heavy dependencies (Qt 6, Open CASCADE) must be installed on the system:
#   - CI and Windows/macOS developers: Qt from the Qt online installer / aqtinstall,
#     Open CASCADE and the small libraries from vcpkg (vcpkg.json in the repo root).
#   - Linux developers: distribution packages are fine.
# Small libraries are looked up with find_package first and fetched with
# FetchContent as a fallback, so a fresh checkout builds with only Qt + OCCT installed.

include(FetchContent)

# --- Open CASCADE --------------------------------------------------------------
find_package(OpenCASCADE CONFIG REQUIRED)
message(STATUS "Open CASCADE ${OpenCASCADE_VERSION} (${OpenCASCADE_DIR})")

# Toolkit names changed in OCCT 7.8 (TKSTEP -> TKDESTEP, TKIGES -> TKDEIGES).
set(AEROCAD_OCCT_LIBS TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo
    TKTopAlgo TKPrim TKShHealing TKMesh TKBO TKBool TKFillet TKOffset TKXSBase)
if(TARGET TKDESTEP)
    list(APPEND AEROCAD_OCCT_LIBS TKDESTEP TKDEIGES)
else()
    list(APPEND AEROCAD_OCCT_LIBS TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKIGES)
endif()
add_library(aerocad_occt INTERFACE)
target_link_libraries(aerocad_occt INTERFACE ${AEROCAD_OCCT_LIBS})
target_include_directories(aerocad_occt SYSTEM INTERFACE ${OpenCASCADE_INCLUDE_DIR})

# --- nlohmann_json (command layer parameters / results) ------------------------
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, fetching")
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- pugixml (3MF model XML) ---------------------------------------------------
find_package(pugixml CONFIG QUIET)
if(NOT pugixml_FOUND)
    message(STATUS "pugixml not found, fetching")
    FetchContent_Declare(pugixml
        URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
        URL_HASH SHA256=2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(pugixml)
endif()
if(NOT TARGET pugixml::pugixml)
    if(TARGET pugixml::static)
        add_library(pugixml::pugixml ALIAS pugixml::static)
    elseif(TARGET pugixml)
        add_library(pugixml::pugixml ALIAS pugixml)
    endif()
endif()

# --- miniz (3MF zip container) -------------------------------------------------
find_package(miniz CONFIG QUIET)
if(NOT miniz_FOUND)
    # Bundled copy of the 3.0.2 release (miniz.c/miniz.h/LICENSE from miniz-3.0.2.zip,
    # SHA256 ada38db0...5332c5): the GitHub release download kept failing in CI (HTTP 504).
    message(STATUS "miniz not found, using third_party/miniz")
    set(miniz_SOURCE_DIR ${PROJECT_SOURCE_DIR}/third_party/miniz)
    add_library(miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
    target_include_directories(miniz SYSTEM PUBLIC ${miniz_SOURCE_DIR})
    set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(miniz::miniz ALIAS miniz)
endif()

# --- Catch2 (tests) ------------------------------------------------------------
if(AEROCAD_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        message(STATUS "Catch2 not found, fetching")
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.7.1
            GIT_SHALLOW ON)
        FetchContent_MakeAvailable(Catch2)
    endif()
endif()

# --- Qt 6 (application only) ---------------------------------------------------
if(AEROCAD_BUILD_APP)
    # Test: real key presses (through shortcuts) for the --ui-script GUI tests.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Quick3D Test)
    message(STATUS "Qt ${Qt6_VERSION}")
endif()
