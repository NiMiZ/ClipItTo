qt_add_executable(clipitto main.cpp)
# Only the header-only DbusNames.h from ipc; deliberately no Gui/Widgets dependency.
target_include_directories(clipitto PRIVATE ${CMAKE_SOURCE_DIR}/src/ipc)
target_link_libraries(clipitto PRIVATE Qt6::Core Qt6::DBus)
target_compile_definitions(clipitto PRIVATE CLIPITTO_VERSION="${PROJECT_VERSION}")
clipitto_set_warnings(clipitto)
