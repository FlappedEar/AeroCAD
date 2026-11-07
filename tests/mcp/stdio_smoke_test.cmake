# SPDX-License-Identifier: GPL-3.0-or-later
# Runs aerocad-mcp --headless with a scripted MCP session on stdin and checks the replies.
# Usage: cmake -DSERVER=<aerocad-mcp> -DINPUT=<session.jsonl> -P stdio_smoke_test.cmake
execute_process(
    COMMAND "${SERVER}" --headless
    INPUT_FILE "${INPUT}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE rc
    TIMEOUT 30)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "aerocad-mcp exited with ${rc}\n${err}")
endif()

# One response per line; count them by their "jsonrpc" member (CMake lists do not cope
# with the brackets in JSON).
string(REGEX MATCHALL "\"jsonrpc\":\"2.0\"" responses "${out}")
list(LENGTH responses count)
if(NOT count EQUAL 4)
    message(FATAL_ERROR "expected 4 responses (notification has none), got ${count}:\n${out}\nstderr:\n${err}")
endif()

foreach(expected
        "\"protocolVersion\":\"2025-06-18\""
        "\"name\":\"document_import\""
        "\"name\":\"geometry_measure\""
        "\\\"distance\\\":5.0"
        "\\\"units\\\":")
    string(FIND "${out}" "${expected}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing ${expected} in output:\n${out}")
    endif()
endforeach()
message(STATUS "aerocad-mcp stdio session OK (${count} responses)")
