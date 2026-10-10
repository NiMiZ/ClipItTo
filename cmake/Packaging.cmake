# Installation layout and the Debian package (Linux).
#   Package:  cmake --preset deb && cmake --build --preset deb && cpack --config build/deb/CPackConfig.cmake
#   Per user: cmake --preset release -DCMAKE_INSTALL_PREFIX=$HOME/.local && cmake --install build/release

install(TARGETS clipitto clipitto-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/clipitto-setup scripts/clipitto-shortcut DESTINATION ${CMAKE_INSTALL_BINDIR})

# systemd searches <datadir>/systemd/user for every prefix (/usr/local, ~/.local); distro
# packages use /usr/lib/systemd/user.
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    set(CLIPITTO_UNIT_DIR_DEFAULT "/usr/lib/systemd/user")
else()
    set(CLIPITTO_UNIT_DIR_DEFAULT "${CMAKE_INSTALL_FULL_DATADIR}/systemd/user")
endif()
set(CLIPITTO_SYSTEMD_USER_UNIT_DIR "${CLIPITTO_UNIT_DIR_DEFAULT}" CACHE PATH "Where to install the systemd user unit")

configure_file(data/clipitto.service.in ${PROJECT_BINARY_DIR}/data/clipitto.service @ONLY)
configure_file(data/io.github.nimiz.ClipItTo.service.in ${PROJECT_BINARY_DIR}/data/io.github.nimiz.ClipItTo.service @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/data/clipitto.service DESTINATION ${CLIPITTO_SYSTEMD_USER_UNIT_DIR})
install(FILES ${PROJECT_BINARY_DIR}/data/io.github.nimiz.ClipItTo.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES data/io.github.nimiz.ClipItTo.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/config.example.toml README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/clipitto)

# GNOME Shell extension, with its settings schema compiled next to it.
set(CLIPITTO_EXT_UUID clipitto@nimiz.github.io)
set(CLIPITTO_EXT_SRC ${PROJECT_SOURCE_DIR}/gnome-extension/${CLIPITTO_EXT_UUID})
set(CLIPITTO_EXT_DEST ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${CLIPITTO_EXT_UUID})
install(FILES ${CLIPITTO_EXT_SRC}/metadata.json ${CLIPITTO_EXT_SRC}/extension.js DESTINATION ${CLIPITTO_EXT_DEST})
install(FILES ${CLIPITTO_EXT_SRC}/schemas/org.gnome.shell.extensions.clipitto.gschema.xml
    DESTINATION ${CLIPITTO_EXT_DEST}/schemas)
find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS)
    set(CLIPITTO_EXT_COMPILED ${PROJECT_BINARY_DIR}/gnome-extension/schemas/gschemas.compiled)
    add_custom_command(
        OUTPUT ${CLIPITTO_EXT_COMPILED}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/gnome-extension/schemas
        COMMAND ${GLIB_COMPILE_SCHEMAS} --strict --targetdir=${PROJECT_BINARY_DIR}/gnome-extension/schemas
                ${CLIPITTO_EXT_SRC}/schemas
        DEPENDS ${CLIPITTO_EXT_SRC}/schemas/org.gnome.shell.extensions.clipitto.gschema.xml
        COMMENT "Compiling the GNOME extension's settings schema")
    add_custom_target(clipitto_extension_schemas ALL DEPENDS ${CLIPITTO_EXT_COMPILED})
    install(FILES ${CLIPITTO_EXT_COMPILED} DESTINATION ${CLIPITTO_EXT_DEST}/schemas)
else()
    message(WARNING "glib-compile-schemas not found: the GNOME extension's settings will not load "
                    "(install libglib2.0-bin)")
endif()

# --- Debian package ---------------------------------------------------------------------
set(CLIPITTO_MAINTAINER "NiMiZ" CACHE STRING "Debian Maintainer field, e.g. 'Name <email>'")
option(CLIPITTO_DEB_SHLIBDEPS "Compute library dependencies with dpkg-shlibdeps" ON)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME clipitto)
set(CPACK_PACKAGE_VENDOR "NiMiZ")
set(CPACK_PACKAGE_CONTACT "${CLIPITTO_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_DESCRIPTION
    "Keyboard-driven clipboard history with search, favorites, groups and notes. Works on GNOME (Wayland, with the bundled GNOME Shell extension, and X11) and on other Wayland compositors that support the data-control protocol.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/NiMiZ/ClipItTo")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}") # paths in the unit files must match
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ${CLIPITTO_DEB_SHLIBDEPS})
# qt6-wayland and the gjs-run extension are loaded at runtime, invisible to dpkg-shlibdeps.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-wayland")
if(NOT CLIPITTO_DEB_SHLIBDEPS)
    string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libqt6core6t64, libqt6gui6t64, libqt6widgets6t64, "
                  "libqt6dbus6t64, libsqlite3-0, libtomlplusplus3t64, libx11-6, libxtst6")
endif()
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gnome-shell | wl-clipboard")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    ${PROJECT_SOURCE_DIR}/packaging/debian/postinst ${PROJECT_SOURCE_DIR}/packaging/debian/prerm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
