# Wire protocol and blocking client: QtCore + QtNetwork only, so the CLI stays lean.
add_library(clipitto_localipc_client STATIC
    LocalClient.cpp LocalClient.h
    LocalProtocol.cpp LocalProtocol.h
)
target_include_directories(clipitto_localipc_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_localipc_client PUBLIC Qt6::Core Qt6::Network)
clipitto_set_warnings(clipitto_localipc_client)

# The daemon's endpoint.
add_library(clipitto_localipc STATIC
    LocalControlChannel.cpp LocalControlChannel.h
)
target_link_libraries(clipitto_localipc PUBLIC clipitto_localipc_client clipitto_core)
clipitto_set_warnings(clipitto_localipc)
