cmake_minimum_required(VERSION 3.20)
project(StickPoint C)

# Windows 10+ only
if(NOT WIN32)
    message(FATAL_ERROR "StickPoint targets Windows only.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- executable -----------------------------------------------------------

add_executable(StickPoint WIN32          # WIN32 → WinMain entry, no console
    src/main.c
    src/gamepad.c
    src/mouse.c
    src/tray.c
)

target_include_directories(StickPoint PRIVATE src)

# xinput and user32 are available on every Windows installation.
target_link_libraries(StickPoint PRIVATE
    xinput
    user32
    shell32
)

# ---- compiler warnings ----------------------------------------------------

if(MSVC)
    target_compile_options(StickPoint PRIVATE
        /W4         # high warning level
        /WX         # warnings as errors
        /wd4100     # suppress "unreferenced formal parameter" (intentional (void) casts)
    )
    # Use the secure CRT — suppress deprecation warnings for standard C functions.
    target_compile_definitions(StickPoint PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # MinGW / Clang-cl
    target_compile_options(StickPoint PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
endif()

# ---- DPI manifest (Windows 10+) ------------------------------------------
#
# SetProcessDPIAware() in main.c covers the runtime side.
# The manifest below sets the per-monitor DPI-aware level so the OS does
# not virtualise coordinates for SendInput on multi-monitor setups.
#
# Uncomment if you add a StickPoint.manifest to the project root.
#
# if(MSVC)
#     set_target_properties(StickPoint PROPERTIES
#         LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/StickPoint.manifest"
#     )
# endif()
