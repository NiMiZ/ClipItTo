# Windows executables carry an icon and a VERSIONINFO resource (ProductName, version, copyright). Code signing
# services such as the SignPath Foundation require that metadata. The version is the project() VERSION, so the
# release version bump (docs/releasing.md) needs no change here.
#   clipitto_add_windows_resource(<target> "<file description>")
function(clipitto_add_windows_resource target description)
    set(CLIPITTO_RC_NAME "${target}")
    set(CLIPITTO_RC_DESCRIPTION "${description}")
    set(CLIPITTO_RC_VERSION "${PROJECT_VERSION}")
    set(CLIPITTO_RC_ICON "${PROJECT_SOURCE_DIR}/packaging/windows/ClipItTo.ico") # forward slashes work in .rc
    # FILEVERSION wants four numbers: 0.17.0 -> 0,17,0,0
    set(parts ${PROJECT_VERSION_MAJOR} ${PROJECT_VERSION_MINOR} ${PROJECT_VERSION_PATCH} 0)
    list(JOIN parts "," CLIPITTO_RC_VERSION_COMMA)
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/clipitto.rc.in" "${rc}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/packaging/windows/ClipItTo.ico")
    target_sources(${target} PRIVATE "${rc}")
endfunction()
