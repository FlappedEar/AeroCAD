# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_cfd: wind tunnel simulation. Generates OpenFOAM cases from parts, runs them in
# the background (Docker or a local OpenFOAM) and reads the results. No Qt, no OCCT.
add_library(aerocad_cfd STATIC
    src/Settings.cpp
    src/CaseWriter.cpp
    src/Results.cpp
    src/Process.cpp
    src/RunManager.cpp
)
add_library(AeroCAD::cfd ALIAS aerocad_cfd)
target_include_directories(aerocad_cfd PUBLIC include)
target_link_libraries(aerocad_cfd PUBLIC AeroCAD::core nlohmann_json::nlohmann_json)
aerocad_set_warnings(aerocad_cfd)
