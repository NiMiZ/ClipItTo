# Daemon logic as a library so integration tests can drive it in-process.
add_library(clipitto_daemon_lib STATIC
    Daemon.cpp Daemon.h
    UnixSignals.cpp UnixSignals.h
)
target_include_directories(clipitto_daemon_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_daemon_lib PUBLIC clipitto_core clipitto_capture clipitto_ipc Qt6::Gui)
clipitto_set_warnings(clipitto_daemon_lib)

qt_add_executable(clipitto-daemon main.cpp)
target_link_libraries(clipitto-daemon PRIVATE clipitto_daemon_lib Qt6::Widgets)
target_compile_definitions(clipitto-daemon PRIVATE CLIPITTO_VERSION="${PROJECT_VERSION}")
clipitto_set_warnings(clipitto-daemon)
