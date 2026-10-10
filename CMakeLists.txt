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

include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

qt_standard_project_setup()

function(clipitto_set_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(CLIPITTO_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
