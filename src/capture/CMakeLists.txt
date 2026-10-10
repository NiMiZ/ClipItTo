add_library(clipitto_capture STATIC
    BackendSelector.cpp BackendSelector.h
    CaptureBackend.h
    QtClipboardBackend.cpp QtClipboardBackend.h
)

target_include_directories(clipitto_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_capture PUBLIC clipitto_core Qt6::Gui)
clipitto_set_warnings(clipitto_capture)
