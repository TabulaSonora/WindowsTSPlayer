# The engine is inert without a licensed SCCore.dll, so every test that renders is conditional on
# one being available. A checkout with no ROM configures and builds; it just has nothing to assert
# against.
#
# Note the variable, which is not the one the engine's own Catch2 suite reads. This and the
# `tabula-sonora` CLI take TS_SCCORE_DLL; NativeTS/tests takes TS_SCCORE. Setting only one is the
# usual reason a third of a run silently skips.
set(TSGUI_TEST_ROM "$ENV{TS_SCCORE_DLL}" CACHE FILEPATH
    "SCCore.dll to run the engine tests against; defaults to $TS_SCCORE_DLL")

set(TSGUI_TEST_MIDI "" CACHE FILEPATH "A MIDI file to render in the engine tests")

add_executable(ts-export-tool ts_export_tool.cpp)
target_link_libraries(ts-export-tool PRIVATE tsgui_host tsgui_warnings)

# The CLI comes from the engine tree this program is linked against, not from whatever happens to be
# on PATH or left over in a sibling checkout's build directory. Comparing our render against a
# *different* build of the engine would fail whenever upstream changed the voice, which says nothing
# about whether this program renders it faithfully -- the only question the test is asking.
#
# The engine is added with EXCLUDE_FROM_ALL, so the target has to be pulled in explicitly.
if(TARGET tabula-sonora)
    add_dependencies(ts-export-tool tabula-sonora)
endif()

add_executable(ts-host-smoke ts_host_smoke.cpp)
target_link_libraries(ts-host-smoke PRIVATE tsgui_host tsgui_warnings)

# --- Metadata, which needs no ROM ---------------------------------------------------------------

# What a file says about itself, over files this test assembles byte by byte. No ROM and no corpus:
# the cases worth pinning are the malformed and the unusual, which is exactly what nobody can be
# relied on to have a copy of.
#
# This is the test to run first on a new toolchain -- it exercises the whole metadata reader with
# nothing to install and nothing to license.
add_executable(ts-song-info-test ts_song_info_test.cpp)
target_link_libraries(ts-song-info-test PRIVATE tsgui_host tsgui_warnings)
add_test(NAME song-info COMMAND ts-song-info-test)

# The Linux front end also validates its desktop entry, its AppStream metainfo and its
# shared-mime-info definitions here. None of those exists on Windows: the equivalents live in
# Package.appxmanifest, which MSBuild validates against its schema at build time, so there is
# nothing for ctest to add.

# --- The engine, which does ----------------------------------------------------------------------

if(NOT TSGUI_TEST_ROM OR NOT EXISTS "${TSGUI_TEST_ROM}")
    message(STATUS "No SCCore.dll configured; engine tests will not be registered. "
                   "Set TS_SCCORE_DLL or -DTSGUI_TEST_ROM=...")
    return()
endif()

if(NOT TSGUI_TEST_MIDI OR NOT EXISTS "${TSGUI_TEST_MIDI}")
    message(STATUS "No test MIDI configured; engine tests will not be registered. "
                   "Set -DTSGUI_TEST_MIDI=...")
    return()
endif()

# Producer and consumer on two threads across the ring.
#
# The "ring" label is kept for continuity with the other ports, but it means less here: it exists
# because this is the case LinuxTSPlayer runs under ThreadSanitizer, and there is no TSan on
# Windows -- MSVC has none and clang-cl does not support -fsanitize=thread. This still proves the
# ring carries audio across two threads; it no longer proves the absence of races. A change to
# ts_player.cpp's synchronisation has to be checked in the Linux tree.
add_test(NAME host-smoke
    COMMAND ts-host-smoke "${TSGUI_TEST_ROM}" "${TSGUI_TEST_MIDI}")
set_tests_properties(host-smoke PROPERTIES LABELS "ring" TIMEOUT 120)

if(TARGET tabula-sonora)
    add_test(NAME export-matches-cli
        COMMAND "${CMAKE_COMMAND}"
                -DTOOL=$<TARGET_FILE:ts-export-tool>
                -DCLI=$<TARGET_FILE:tabula-sonora>
                -DROM=${TSGUI_TEST_ROM}
                -DMIDI=${TSGUI_TEST_MIDI}
                -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/export
                -P "${CMAKE_CURRENT_SOURCE_DIR}/CompareExport.cmake")
    set_tests_properties(export-matches-cli PROPERTIES TIMEOUT 600)
else()
    message(WARNING
        "The engine's CLI target is absent, so the export byte-comparison -- the test that proves "
        "this program renders the engine faithfully -- will not run.")
endif()
