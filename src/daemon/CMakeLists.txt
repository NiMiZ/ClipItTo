# Daemon logic as a portable library, so tests can drive it in-process on every platform.
add_library(clipitto_daemon_lib STATIC
    Daemon.cpp Daemon.h
)
target_include_directories(clipitto_daemon_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_daemon_lib PUBLIC clipitto_core clipitto_capture Qt6::Gui)
clipitto_set_warnings(clipitto_daemon_lib)

# The Linux entry point (D-Bus control channel, POSIX signals). The Windows entry point
# arrives with its own control channel (global hotkey + named pipe).
if(CLIPITTO_DBUS)
    qt_add_executable(clipitto-daemon main.cpp UnixSignals.cpp UnixSignals.h)
    target_link_libraries(clipitto-daemon PRIVATE clipitto_daemon_lib clipitto_ipc Qt6::Widgets)
    target_compile_definitions(clipitto-daemon PRIVATE CLIPITTO_VERSION="${PROJECT_VERSION}")
    clipitto_set_warnings(clipitto-daemon)
endif()
