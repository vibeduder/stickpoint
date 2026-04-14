cmake_minimum_required(VERSION 3.20)
project(StickPoint CXX)

# Windows 10+ only
if(NOT WIN32)
    message(FATAL_ERROR "StickPoint targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- executable -----------------------------------------------------------

add_executable(StickPoint WIN32          # WIN32 → WinMain entry, no console
    src/main.cpp
    src/gamepad.cpp
    src/mouse.cpp
    src/tray.cpp
    src/StickPoint.rc                    # embeds icon into the .exe
)

target_include_directories(StickPoint PRIVATE src)

# xinput and user32 are available on every Windows installation.
# ole32 + windowscodecs: COM + WIC (PNG loading in tray.cpp)
# setupapi + hid:        HID device enumeration for controller name/image detection
# msimg32:               AlphaBlend for PNG rendering with transparency
target_link_libraries(StickPoint PRIVATE
    xinput
    user32
    shell32
    ole32
    windowscodecs
    setupapi
    hid
    msimg32
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

# ---- Copy assets to output directory --------------------------------------
#
# The assets folder must sit next to the exe so build_image_path() finds it.
#
add_custom_command(TARGET StickPoint POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:StickPoint>/assets"
    COMMENT "Copying assets to output directory"
)

# ---- DPI manifest (Windows 10+) ------------------------------------------
#
# SetProcessDPIAware() in main.cpp covers the runtime side.
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
