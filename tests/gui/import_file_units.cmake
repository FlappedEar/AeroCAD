# SPDX-License-Identifier: GPL-3.0-or-later
# Two real application sessions prove that a persisted unitless-file preference does
# not override the next STEP/3MF file's units, including a mixed background import queue.
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
foreach(script import_units_remember.ui import_file_units.ui)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${script}" "${OUTPUT_DIR}/${script}" @ONLY)
    execute_process(COMMAND "${APP}" --ui-script "${OUTPUT_DIR}/${script}"
        WORKING_DIRECTORY "${OUTPUT_DIR}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${script} failed: ${result}")
    endif()
endforeach()
