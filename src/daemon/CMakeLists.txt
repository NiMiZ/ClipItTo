# Daemon logic as a portable library, so tests can drive it in-process on every platform.
add_library(clipitto_daemon_lib STATIC
    Daemon.cpp Daemon.h
    GnomeShortcut.cpp GnomeShortcut.h
    LoginItem.cpp LoginItem.h
)
target_include_directories(clipitto_daemon_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_daemon_lib PUBLIC clipitto_core clipitto_capture clipitto_output clipitto_ui Qt6::Concurrent)
clipitto_set_warnings(clipitto_daemon_lib)
if(APPLE)
    target_compile_options(clipitto_daemon_lib PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

# WIN32: a GUI-subsystem program on Windows (no console window); ignored elsewhere.
qt_add_executable(clipitto-daemon WIN32 main.cpp)
target_link_libraries(clipitto-daemon PRIVATE clipitto_daemon_lib Qt6::Widgets)
target_compile_definitions(clipitto-daemon PRIVATE CLIPITTO_VERSION="${PROJECT_VERSION}")
# Windows Hello for password copies (see WindowsHello.h): C++/WinRT on Windows.
target_sources(clipitto_daemon_lib PRIVATE WindowsHello.h)
if(WIN32)
    target_sources(clipitto_daemon_lib PRIVATE WindowsHello_win.cpp)
    target_link_libraries(clipitto_daemon_lib PRIVATE WindowsApp)
else()
    target_sources(clipitto_daemon_lib PRIVATE WindowsHello_none.cpp)
endif()
# The history key's keyring, one implementation per platform (see SystemKeyStore.h).
target_sources(clipitto_daemon_lib PRIVATE SystemKeyStore.h)
if(WIN32)
    target_sources(clipitto_daemon_lib PRIVATE SystemKeyStore_win.cpp)
    target_link_libraries(clipitto_daemon_lib PRIVATE Advapi32) # Cred*W
elseif(APPLE)
    # The macOS Keychain (Security.framework): no extra library.
    target_sources(clipitto_daemon_lib PRIVATE SystemKeyStore_mac.mm)
    target_link_libraries(clipitto_daemon_lib PRIVATE "-framework Security" "-framework Foundation")
elseif(Qt6Keychain_FOUND)
    target_sources(clipitto_daemon_lib PRIVATE SystemKeyStore_linux.cpp KeychainKeyStore.cpp KeychainKeyStore.h)
    target_link_libraries(clipitto_daemon_lib PRIVATE Qt6Keychain::Qt6Keychain)
else()
    target_sources(clipitto_daemon_lib PRIVATE SystemKeyStore_none.cpp)
endif()
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
elseif(APPLE)
    target_sources(clipitto-daemon PRIVATE MacHotkey.mm MacHotkey.h)
    target_link_libraries(clipitto-daemon PRIVATE "-framework Carbon")
endif()
target_sources(clipitto-daemon PRIVATE GlobalHotkey.h)
clipitto_set_warnings(clipitto-daemon)
