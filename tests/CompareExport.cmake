# Renders the same song twice -- once through the host layer's export path, once through
# `tabula-sonora render` -- and requires the bytes to match.
#
# Expects: TOOL, CLI, ROM, MIDI, OUT_DIR.

file(MAKE_DIRECTORY "${OUT_DIR}")
set(_ours "${OUT_DIR}/ours.wav")
set(_theirs "${OUT_DIR}/theirs.wav")

execute_process(COMMAND "${TOOL}" "${ROM}" "${MIDI}" "${_ours}" RESULT_VARIABLE _ours_result)
if(NOT _ours_result EQUAL 0)
    message(FATAL_ERROR "The host layer's export failed (${_ours_result})")
endif()

# The flags that reproduce TSEngineSettingsDefault, which is what the session exports with:
# SC-8820, two ports, the hardware's 64 voices, every effect on, unity gain. The CLI's own default
# polyphony is 0 -- grow on demand -- so it has to be said explicitly.
execute_process(
    COMMAND "${CLI}" render --dll "${ROM}" --map 4 --ports 2 --polyphony 64 "${MIDI}" "${_theirs}"
    RESULT_VARIABLE _theirs_result
    OUTPUT_QUIET)
if(NOT _theirs_result EQUAL 0)
    message(FATAL_ERROR "tabula-sonora render failed (${_theirs_result})")
endif()

file(SIZE "${_ours}" _ours_size)
file(SIZE "${_theirs}" _theirs_size)

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${_ours}" "${_theirs}"
    RESULT_VARIABLE _differ)

if(NOT _differ EQUAL 0)
    message(FATAL_ERROR
        "Export does not match tabula-sonora render.\n"
        "  ours:   ${_ours} (${_ours_size} bytes)\n"
        "  theirs: ${_theirs} (${_theirs_size} bytes)\n"
        "The vendored session has changed the engine's audio behaviour.")
endif()

message(STATUS "Export matches tabula-sonora render byte for byte (${_ours_size} bytes)")
