function(clipitto_add_test name)
    add_executable(${name} ${name}.cpp TestDb.h)
    target_link_libraries(${name} PRIVATE clipitto_core Qt6::Test ${ARGN})
    clipitto_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endfunction()

# Portable tests: run on Linux and Windows.
clipitto_add_test(tst_config)
clipitto_add_test(tst_ftsquery)
clipitto_add_test(tst_migrator)
clipitto_add_test(tst_repository)
clipitto_add_test(tst_clipfilter)
clipitto_add_test(tst_pipeline)
clipitto_add_test(tst_clipboardbackend clipitto_capture)
clipitto_add_test(tst_backendselector clipitto_capture)
clipitto_add_test(tst_daemon clipitto_daemon_lib)
clipitto_add_test(tst_historymodel clipitto_ui)
clipitto_add_test(tst_searchcontroller clipitto_ui)
clipitto_add_test(tst_popup clipitto_ui)

if(UNIX)
    # Drives the backend against fake-wl-paste.sh (POSIX shell).
    clipitto_add_test(tst_wlpaste clipitto_capture)
    target_compile_definitions(tst_wlpaste PRIVATE CLIPITTO_FAKE_WLPASTE="${CMAKE_CURRENT_SOURCE_DIR}/fake-wl-paste.sh")

    # The GNOME Shell extension's logic, run by gjs against fake shell modules.
    find_program(GJS gjs)
    if(GJS)
        add_test(NAME gnome_extension
            COMMAND sh ${PROJECT_SOURCE_DIR}/gnome-extension/tests/run-tests.sh ${CMAKE_CURRENT_BINARY_DIR}/gnome-extension)
    endif()
endif()

# Not part of the default ctest run: ctest -C bench -L bench (or run bench_search directly).
add_executable(bench_search bench_search.cpp TestDb.h)
target_link_libraries(bench_search PRIVATE clipitto_core Qt6::Test)
clipitto_set_warnings(bench_search)
add_test(NAME bench_search COMMAND bench_search CONFIGURATIONS bench)
set_tests_properties(bench_search PROPERTIES LABELS bench)

# D-Bus integration (Linux): daemon in-process + the real CLI binary, on a private session bus.
if(CLIPITTO_DBUS)
    add_executable(tst_dbus tst_dbus.cpp)
    target_link_libraries(tst_dbus PRIVATE clipitto_daemon_lib clipitto_ipc Qt6::Test)
    target_compile_definitions(tst_dbus PRIVATE CLIPITTO_CLI_PATH="$<TARGET_FILE:clipitto>")
    add_dependencies(tst_dbus clipitto)
    clipitto_set_warnings(tst_dbus)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(DBUS_RUN_SESSION)
        add_test(NAME tst_dbus COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_dbus>)
    else()
        add_test(NAME tst_dbus COMMAND tst_dbus) # skips itself without a session bus
    endif()
    set_tests_properties(tst_dbus PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
endif()
