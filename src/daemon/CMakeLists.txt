# Daemon logic as a portable library, so tests can drive it in-process on every platform.
add_library(clipitto_daemon_lib STATIC
    Daemon.cpp Daemon.h
)
target_include_directories(clipitto_daemon_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_daemon_lib PUBLIC clipitto_core clipitto_capture clipitto_output clipitto_ui)
clipitto_set_warnings(clipitto_daemon_lib)

# WIN32: a GUI-subsystem program on Windows (no console window); ignored elsewhere.
qt_add_executable(clipitto-daemon WIN32 main.cpp)
target_link_libraries(clipitto-daemon PRIVATE clipitto_daemon_lib Qt6::Widgets)
target_compile_definitions(clipitto-daemon PRIVATE CLIPITTO_VERSION="${PROJECT_VERSION}")
if(CLIPITTO_DBUS)
    # Linux: D-Bus control channel and the GNOME Shell extension.
    target_compile_definitions(clipitto-daemon PRIVATE CLIPITTO_USE_DBUS)
    target_link_libraries(clipitto-daemon PRIVATE clipitto_ipc)
else()
    # Windows (and builds without D-Bus): local socket / named pipe.
    target_link_libraries(clipitto-daemon PRIVATE clipitto_localipc)
endif()
if(UNIX)
    target_sources(clipitto-daemon PRIVATE UnixSignals.cpp UnixSignals.h)
endif()
if(WIN32)
    target_sources(clipitto-daemon PRIVATE WindowsHotkey.cpp WindowsHotkey.h)
endif()
clipitto_set_warnings(clipitto-daemon)
