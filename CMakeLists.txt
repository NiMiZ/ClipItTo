cmake_minimum_required(VERSION 3.22)

project(ClipItTo
    VERSION 0.9.2
    DESCRIPTION "Clipboard manager with history, groups and favorites for Linux, Windows and macOS"
    LANGUAGES C CXX)

if(APPLE)
    # The macOS pasteboard, source-app and (later) hotkey code is Objective-C++ (.mm).
    # macOS 11 is the first release for Apple Silicon Macs (the late-2020 M1 ones); Qt 6.5
    # supports it. Both architectures in one binary unless the caller chose.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
    endif()
    # Xcode 15's linker warns about every library listed twice, as static-library cycles
    # (core <-> db, ui <-> output) make CMake do; the repeat is harmless.
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" CLIPITTO_LD_NO_WARN_DUPLICATES)
    if(CLIPITTO_LD_NO_WARN_DUPLICATES)
        add_link_options("LINKER:-no_warn_duplicate_libraries")
    endif()
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTING "Build unit tests and benchmarks" ON)
option(CLIPITTO_WERROR "Treat compiler warnings as errors" OFF)
if(UNIX AND NOT APPLE)
    set(CLIPITTO_DBUS_DEFAULT ON)
else()
    set(CLIPITTO_DBUS_DEFAULT OFF)
endif()
option(CLIPITTO_DBUS "Build the D-Bus control channel, Linux daemon entry point and clipitto CLI"
    ${CLIPITTO_DBUS_DEFAULT})

if(MSVC)
    add_compile_options(/utf-8) # sources and test literals are UTF-8
    if(MSVC_VERSION GREATER_EQUAL 1950)
        # Qt 6.5's headers still use stdext iterators, which MSVC 14.50 removed.
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/FI${PROJECT_SOURCE_DIR}/cmake/msvc_stdext_shim.h>")
    endif()
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

qt_standard_project_setup()

function(clipitto_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(CLIPITTO_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(CLIPITTO_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_subdirectory(src)

if(CLIPITTO_DBUS)
    include(cmake/Packaging.cmake)
elseif(WIN32)
    include(cmake/PackagingWindows.cmake)
elseif(APPLE)
    include(cmake/PackagingMac.cmake)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
