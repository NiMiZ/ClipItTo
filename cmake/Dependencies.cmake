# Third-party dependencies.
#   Ubuntu 24.04: sudo apt install qt6-base-dev libgl-dev libsqlite3-dev libtomlplusplus-dev
#   Windows:      Qt 6.5+ (MSVC) plus vcpkg in manifest mode (see vcpkg.json and the
#                 "windows" preset), which provides SQLite (with FTS5) and toml++.

set(CLIPITTO_QT_COMPONENTS Core Gui Widgets)
if(CLIPITTO_DBUS)
    list(APPEND CLIPITTO_QT_COMPONENTS DBus)
endif()
if(BUILD_TESTING)
    list(APPEND CLIPITTO_QT_COMPONENTS Test)
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS ${CLIPITTO_QT_COMPONENTS})

find_package(SQLite3 3.35 REQUIRED) # 3.35+: UPSERT ... RETURNING; FTS5 trigram needs 3.34+

# toml++: prefer the distro package (CMake config or pkg-config), else fetch the header-only release.
find_package(tomlplusplus 3 CONFIG QUIET)
if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(tomlplusplus QUIET IMPORTED_TARGET tomlplusplus)
    endif()
    if(TARGET PkgConfig::tomlplusplus)
        add_library(tomlplusplus::tomlplusplus ALIAS PkgConfig::tomlplusplus)
    else()
        message(STATUS "toml++ not found on the system; fetching v3.4.0")
        include(FetchContent)
        FetchContent_Declare(tomlplusplus
            URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
        FetchContent_MakeAvailable(tomlplusplus)
    endif()
endif()
