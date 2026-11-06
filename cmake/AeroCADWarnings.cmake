# SPDX-License-Identifier: GPL-3.0-or-later

function(_aerocad_warning_flags out)
    if(MSVC)
        set(flags /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(AEROCAD_WARNINGS_AS_ERRORS)
            list(APPEND flags /WX)
        endif()
    else()
        set(flags -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
        if(AEROCAD_WARNINGS_AS_ERRORS)
            list(APPEND flags -Werror)
        endif()
    endif()
    set(${out} ${flags} PARENT_SCOPE)
endfunction()

# aerocad_set_warnings(<target>): project warning flags for all sources of a target.
function(aerocad_set_warnings target)
    _aerocad_warning_flags(flags)
    target_compile_options(${target} PRIVATE ${flags})
endfunction()

# aerocad_set_source_warnings(<source>...): warning flags for selected sources only.
# Used where a target also compiles generated code (e.g. Qt's QML cache), which we do
# not control and which does not build cleanly with our flags.
function(aerocad_set_source_warnings)
    _aerocad_warning_flags(flags)
    set_source_files_properties(${ARGN} DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTIES COMPILE_OPTIONS "${flags}")
endfunction()
