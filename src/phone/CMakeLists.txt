# "Receive from phone" (roadmap 28): a tiny HTTP server for the local network, the address helpers and the phone's page.
# QtNetwork (part of Qt itself) is the only thing it needs; the server exists only while the user has the window open.
add_library(clipitto_phone STATIC
    PhoneNet.cpp PhoneNet.h
    PhonePage.cpp PhonePage.h
    PhoneServer.cpp PhoneServer.h
)
target_include_directories(clipitto_phone PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_phone PUBLIC clipitto_core Qt6::Network)
clipitto_set_warnings(clipitto_phone)
