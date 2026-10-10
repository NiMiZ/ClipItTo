qt_add_executable(clipitto main.cpp)
target_compile_definitions(clipitto PRIVATE CLIPITTO_VERSION="${PROJECT_VERSION}")
# Deliberately no Gui/Widgets dependency: the shortcut path must start fast.
if(CLIPITTO_DBUS)
    target_compile_definitions(clipitto PRIVATE CLIPITTO_USE_DBUS)
    target_include_directories(clipitto PRIVATE ${PROJECT_SOURCE_DIR}/src/ipc) # header-only DbusNames.h
    target_link_libraries(clipitto PRIVATE Qt6::Core Qt6::DBus)
else()
    target_link_libraries(clipitto PRIVATE clipitto_localipc_client)
endif()
clipitto_set_warnings(clipitto)
