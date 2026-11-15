# Third-party dependencies.
#   Ubuntu 24.04: sudo apt install qt6-base-dev libgl-dev libsqlite3-dev libtomlplusplus-dev
#   Windows:      Qt 6.5+ (MSVC) plus vcpkg in manifest mode (see vcpkg.json and the
#                 "windows" preset), which provides SQLite (with FTS5) and toml++.

# Concurrent: encoding copied pictures off the GUI thread. Network: the local socket of the control channel (Windows) and
# "Receive from phone", which listens on the local network only while the user has its window open.
set(CLIPITTO_QT_COMPONENTS Core Gui Widgets Concurrent Network)
if(CLIPITTO_DBUS)
    list(APPEND CLIPITTO_QT_COMPONENTS DBus)
endif()
if(BUILD_TESTING)
    list(APPEND CLIPITTO_QT_COMPONENTS Test)
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS ${CLIPITTO_QT_COMPONENTS})

# SQLite. With CLIPITTO_ENCRYPTION (default) it is SQLite3 Multiple Ciphers, built from its
# release amalgamation: SQLite plus page encryption (ChaCha20-Poly1305), with FTS5. Distro
# SQLCipher builds lack FTS5, which the search needs. Offline builds can point
# FETCHCONTENT_SOURCE_DIR_SQLITE3MC at an unpacked copy of the same zip.
option(CLIPITTO_ENCRYPTION "Bundle SQLite3 Multiple Ciphers so the history can be encrypted" ON)
add_library(clipitto_sqlite INTERFACE)
if(CLIPITTO_ENCRYPTION)
    include(FetchContent)
    FetchContent_Declare(sqlite3mc
        URL https://github.com/utelle/SQLite3MultipleCiphers/releases/download/v2.5.1/sqlite3mc-2.5.1-sqlite-3.53.4-amalgamation.zip
        URL_HASH SHA256=4125f8ff275ea953dabb3289331b20a0e76d4fc060f57148f4a5df3bf3b0d5e0
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR no-cmake-project) # sources only: no add_subdirectory
    FetchContent_MakeAvailable(sqlite3mc)
    add_library(clipitto_sqlite3mc STATIC ${sqlite3mc_SOURCE_DIR}/sqlite3mc_amalgamation.c)
    target_include_directories(clipitto_sqlite3mc PUBLIC ${sqlite3mc_SOURCE_DIR})
    target_compile_definitions(clipitto_sqlite3mc
        PRIVATE SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5 SQLITE_DQS=0 SQLITE_TEMP_STORE=3
        PUBLIC CLIPITTO_HAVE_ENCRYPTION)
    find_package(Threads REQUIRED)
    target_link_libraries(clipitto_sqlite3mc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    if(UNIX)
        target_link_libraries(clipitto_sqlite3mc PUBLIC m)
    endif()
    if(MSVC)
        target_compile_options(clipitto_sqlite3mc PRIVATE /w)
    else()
        target_compile_options(clipitto_sqlite3mc PRIVATE -w)
    endif()
    target_link_libraries(clipitto_sqlite INTERFACE clipitto_sqlite3mc)
else()
    find_package(SQLite3 3.35 REQUIRED) # 3.35+: UPSERT ... RETURNING; FTS5 trigram needs 3.34+
    target_link_libraries(clipitto_sqlite INTERFACE SQLite::SQLite3)
endif()

# Monocypher (CC0 / BSD-2-Clause): the password copies' crypto (X25519 sealed boxes,
# XChaCha20-Poly1305, Argon2id, keyed BLAKE2b). Two files, built in on every platform, so
# there is no extra DLL on Windows and no extra package on Linux. Offline builds can point
# FETCHCONTENT_SOURCE_DIR_MONOCYPHER at an unpacked copy of the same tarball.
include(FetchContent)
FetchContent_Declare(monocypher
    # The tag's GitHub archive: the same code as monocypher.org's 4.0.2 tarball, without
    # that tarball's documentation symlinks, which fail to extract on Windows and /mnt drives.
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
    URL_HASH SHA256=bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR no-cmake-project) # sources only: its Makefile is not used
FetchContent_MakeAvailable(monocypher)
add_library(clipitto_monocypher STATIC ${monocypher_SOURCE_DIR}/src/monocypher.c)
target_include_directories(clipitto_monocypher SYSTEM PUBLIC ${monocypher_SOURCE_DIR}/src)
set_target_properties(clipitto_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(clipitto_monocypher PRIVATE /w)
else()
    target_compile_options(clipitto_monocypher PRIVATE -w)
endif()

# QR Code generator library by Project Nayuki (MIT): the QR view of a copy (Show as QR code). Two files
# (cpp/qrcodegen.cpp and .hpp), built in on every platform. Offline builds can point
# FETCHCONTENT_SOURCE_DIR_QRCODEGEN at an unpacked copy of the same tarball.
FetchContent_Declare(qrcodegen
    URL https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz
    URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR no-cmake-project) # sources only: its Makefile is not used
FetchContent_MakeAvailable(qrcodegen)
add_library(clipitto_qrcodegen STATIC ${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp)
target_include_directories(clipitto_qrcodegen SYSTEM PUBLIC ${qrcodegen_SOURCE_DIR}/cpp)
set_target_properties(clipitto_qrcodegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(clipitto_qrcodegen PRIVATE /w)
else()
    target_compile_options(clipitto_qrcodegen PRIVATE -w)
endif()

# The history key's keyring: Windows uses the Credential Manager API directly; Linux uses
# Secret Service through qtkeychain (distro package), optional.
if(UNIX AND NOT APPLE)
    find_package(Qt6Keychain CONFIG QUIET)
    if(Qt6Keychain_FOUND)
        message(STATUS "qtkeychain found: the history key can be kept in the system keyring")
    else()
        message(STATUS "qtkeychain not found: history encryption cannot be turned on (install qtkeychain-qt6-dev)")
    endif()
endif()

# XTest for X11 auto-paste: optional, Linux only.
if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND AND X11_XTest_FOUND)
        set(CLIPITTO_X11_PASTE_DEFAULT ON)
    else()
        set(CLIPITTO_X11_PASTE_DEFAULT OFF)
        message(STATUS "libXtst not found: building without X11 auto-paste (install libxtst-dev)")
    endif()
    option(CLIPITTO_X11_PASTE "Auto-paste on X11 via the XTest extension" ${CLIPITTO_X11_PASTE_DEFAULT})
else()
    set(CLIPITTO_X11_PASTE OFF)
endif()

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
