cmake_minimum_required(VERSION 3.22)

project(ClipItTo
    VERSION 0.1.0
    DESCRIPTION "Clipboard manager with history, groups and favorites for Linux"
    LANGUAGES CXX)

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
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
