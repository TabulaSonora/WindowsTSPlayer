# The warning set, as an interface target so nothing has to repeat it.
#
# Compiler-conditional, unlike the Linux front end's, which applies the GCC spelling unguarded --
# every one of those flags is an error on MSVC. The engine already does it this way; see
# NativeTS/CMakeLists.txt.

add_library(tsgui_warnings INTERFACE)

if(MSVC)
    # /W4 rather than /Wall. MSVC's /Wall includes per-instantiation noise from <system_error> and
    # the rest of the STL that no project keeps clean, so it would train the habit of ignoring
    # warnings -- which is the opposite of what a warning set is for.
    #
    # /permissive- is the load-bearing half: without it MSVC still accepts two-phase-lookup errors
    # and other non-conformances that would compile here and fail on the other two ports.
    target_compile_options(tsgui_warnings INTERFACE /W4 /permissive-)
else()
    # Deliberately without -Wold-style-cast: the Linux front end needs C casts for GTK's macros, and
    # keeping the sets aligned matters more than the flag does.
    target_compile_options(tsgui_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wdouble-promotion
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>)
endif()
