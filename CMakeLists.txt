cmake_minimum_required(VERSION 3.24)

# The CMake half of the build, and only that half.
#
# This project has two build systems and one link. CMake owns the engine and the host layer and
# produces static libraries; MSBuild owns the WinUI 3 application -- XAML, MIDL and cppwinrt -- and
# links them. The direction is one-way on purpose: MSBuild reads the props file generated in
# src/CMakeLists.txt, and CMake never invokes MSBuild, because the reverse would be a cycle neither
# could break.
#
# So there is no add_executable here. `build.ps1` runs both halves in order.

project(WindowsTSPlayer VERSION 1.1.0 LANGUAGES CXX)

# Only C++: the Linux front end also enables C to compile glib-compile-resources output, and nothing
# here generates any.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(TSGUI_BUILD_TESTS "Build the host tests, and with them the engine's CLI" ON)
option(TSGUI_FAST_DEBUG "Compile the engine optimised even in Debug builds" ON)
option(TSGUI_USE_INSTALLED_NATIVETS "Use an installed TabulaSonora rather than a source tree" OFF)

include(CTest)
include(TsguiWarnings)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------------
# Where the engine comes from
# ---------------------------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nativets/CMakeLists.txt")
    set(_tsgui_default_nativets "${CMAKE_CURRENT_SOURCE_DIR}/nativets")
else()
    set(_tsgui_default_nativets "${CMAKE_CURRENT_SOURCE_DIR}/../NativeTS")
endif()

set(TSGUI_NATIVETS_DIR "${_tsgui_default_nativets}" CACHE PATH
    "Path to a NativeTS source tree, when not using an installed one")

if(NOT TSGUI_USE_INSTALLED_NATIVETS AND NOT EXISTS "${TSGUI_NATIVETS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "No NativeTS source tree at '${TSGUI_NATIVETS_DIR}'.\n"
        "Run 'git submodule update --init --recursive', or pass -DTSGUI_NATIVETS_DIR=<path>.")
endif()

# ---------------------------------------------------------------------------------------------
# Which engine commit this was built from, for the About dialog
#
# Empty is a supported outcome, not a failure: a source archive has no repository, and the dialog
# falls back to linking the project rather than a commit in it. Which is also why this is not
# FATAL_ERROR -- a missing hash must never be the reason someone cannot build the player.
# ---------------------------------------------------------------------------------------------
set(TSGUI_NATIVETS_COMMIT "")

if(NOT TSGUI_USE_INSTALLED_NATIVETS)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${TSGUI_NATIVETS_DIR}" rev-parse HEAD
            OUTPUT_VARIABLE _tsgui_nativets_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _tsgui_nativets_rev_parse)

        if(_tsgui_nativets_rev_parse EQUAL 0)
            set(TSGUI_NATIVETS_COMMIT "${_tsgui_nativets_commit}")

            # Configure again when the engine moves, or the hash goes stale the moment the submodule
            # is bumped: the engine's *sources* change, so the build recompiles them and the binary
            # really does carry the new voice, but a compile definition fixed at configure time
            # would still name the old commit -- a dialog confidently reporting the wrong answer,
            # which is worse than reporting none.
            #
            # Both files, because they go stale by different routes. A `git checkout` of another
            # commit rewrites HEAD, which is what `git submodule update` leaves behind; a
            # fast-forward of the branch that HEAD already points at never touches HEAD at all and
            # moves only the ref underneath it.
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${TSGUI_NATIVETS_DIR}" rev-parse --absolute-git-dir
                OUTPUT_VARIABLE _tsgui_nativets_git_dir
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)

            if(EXISTS "${_tsgui_nativets_git_dir}/HEAD")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${_tsgui_nativets_git_dir}/HEAD")

                file(READ "${_tsgui_nativets_git_dir}/HEAD" _tsgui_nativets_head)
                string(STRIP "${_tsgui_nativets_head}" _tsgui_nativets_head)
                # Anything else is a detached HEAD, which holds the hash itself and needs no ref.
                if(_tsgui_nativets_head MATCHES "^ref: (.+)$")
                    set(_tsgui_nativets_ref "${_tsgui_nativets_git_dir}/${CMAKE_MATCH_1}")
                    # A packed ref has no file of its own; the HEAD dependency is all there is, and
                    # a loose ref appears the moment the branch next moves anyway.
                    if(EXISTS "${_tsgui_nativets_ref}")
                        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                            "${_tsgui_nativets_ref}")
                    endif()
                endif()
            endif()
        endif()
    endif()

    if(TSGUI_NATIVETS_COMMIT)
        message(STATUS "NativeTS at ${TSGUI_NATIVETS_COMMIT}")
    else()
        message(STATUS "NativeTS commit unknown; About will link the project rather than a commit")
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------------------------
if(TSGUI_USE_INSTALLED_NATIVETS)
    find_package(TabulaSonora REQUIRED)
else()
    set(TS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # The CLI is what `export-matches-cli` compares against, so the tests need it and nothing else
    # does.
    set(TS_BUILD_CLI ${TSGUI_BUILD_TESTS} CACHE BOOL "" FORCE)
    set(TS_BUILD_PLAYER OFF CACHE BOOL "" FORCE)
    set(TS_BUILD_TUI OFF CACHE BOOL "" FORCE)
    set(TS_BUILD_WEB OFF CACHE BOOL "" FORCE)
    set(TS_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(TS_INSTALL OFF CACHE BOOL "" FORCE)

    # The engine links FLAC, Vorbis and Ogg whenever pkg-config finds them, purely for the
    # compressed SoundFont export this player never calls. The Linux front end hides them by
    # pointing PKG_CONFIG_LIBDIR at an empty directory for the duration of the add_subdirectory.
    #
    # Here there is normally no pkg-config to hide them from -- but vcpkg can put a pkgconf on PATH,
    # and then the codecs would reappear depending on how the developer's environment happens to be
    # set up. Saying so outright is deterministic where relying on its absence is not.
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
    add_subdirectory("${TSGUI_NATIVETS_DIR}" "${CMAKE_BINARY_DIR}/nativets" EXCLUDE_FROM_ALL)
    unset(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)
endif()

# ---------------------------------------------------------------------------------------------
# Debug builds that can still keep up
#
# At /Od the synth renders about 1.4x realtime, and no ring can absorb that -- a Debug build would
# stutter for reasons that have nothing to do with whatever is being debugged. So the engine is
# compiled optimised even in Debug, and only the host and application sources are not.
#
# This is more than one line on MSVC, unlike GCC. CMake puts both /RTC1 and /Od into the global
# CMAKE_CXX_FLAGS_DEBUG, where neither can be removed per target; and /RTC1 with /O2 is D8016, a
# hard error rather than a warning. So both are stripped from the global string here and /Od handed
# back to our own targets in src/.
#
# Note what this does *not* fix: /O2 leaves _ITERATOR_DEBUG_LEVEL=2 in place, which bounds-checks
# every std::vector and std::span access in the DSP inner loops. Changing that is an ABI change and
# would break linking against everything else built Debug, so it is not an option. If a Debug build
# still cannot keep up, the answer is a preset whose CMake half is RelWithDebInfo -- losing
# step-through of the host layer -- not a CRT flag.
# ---------------------------------------------------------------------------------------------
if(TSGUI_FAST_DEBUG AND MSVC)
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

if(TSGUI_FAST_DEBUG AND TARGET tabulasonora)
    target_compile_options(tabulasonora PRIVATE
        $<$<CONFIG:Debug>:$<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>>)
endif()

add_subdirectory(src)

if(TSGUI_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
