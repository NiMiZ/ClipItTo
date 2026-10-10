# Windows installation layout: a self-contained folder (Qt DLLs via windeployqt, the MSVC
# runtime next to the executables), which packaging/windows/clipitto.iss turns into an installer.
#   cmake --install build/windows --prefix C:/absolute/path/stage   (Qt's deploy step needs an absolute path)

install(TARGETS clipitto clipitto-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The daemon links every Qt module the CLI needs too, so deploying it covers both.
qt_generate_deploy_app_script(
    TARGET clipitto-daemon
    OUTPUT_SCRIPT CLIPITTO_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${CLIPITTO_DEPLOY_SCRIPT})

# App-local MSVC runtime (msvcp140.dll, vcruntime140*.dll): no separate redistributable needed.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
include(InstallRequiredSystemLibraries)

install(FILES data/config.example.toml README.md DESTINATION doc)
