qt_add_dbus_adaptor(clipitto_adaptor_sources
    io.github.nimiz.ClipItTo.xml
    DbusControlChannel.h
    clipitto::ipc::DbusControlChannel
    ControlAdaptor
    ControlAdaptor
)

add_library(clipitto_ipc STATIC
    DbusControlChannel.cpp DbusControlChannel.h
    DbusNames.h
    ${clipitto_adaptor_sources}
)

target_include_directories(clipitto_ipc
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(clipitto_ipc PUBLIC clipitto_core Qt6::DBus)
clipitto_set_warnings(clipitto_ipc)
