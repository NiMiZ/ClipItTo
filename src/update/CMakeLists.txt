# "Check for updates": one small HTTPS request to cohenzco.com, made only when the user asks (or has turned on the automatic
# check, which is off by default). QtNetwork (part of Qt itself) is the only dependency.
add_library(clipitto_update STATIC
    UpdateCheck.cpp UpdateCheck.h
)
target_include_directories(clipitto_update PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_update PUBLIC clipitto_core Qt6::Network)
clipitto_set_warnings(clipitto_update)
