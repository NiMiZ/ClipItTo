add_library(clipitto_capture STATIC
    BackendSelector.cpp BackendSelector.h
    CaptureBackend.h
    GnomeExtBackend.h
    QtClipboardBackend.cpp QtClipboardBackend.h
    WlPasteBackend.cpp WlPasteBackend.h
    NativeClipboard.h
    ImageCodec.cpp ImageCodec.h
)
if(WIN32)
    target_sources(clipitto_capture PRIVATE NativeClipboard_win.cpp)
else()
    target_sources(clipitto_capture PRIVATE NativeClipboard_none.cpp)
endif()

target_include_directories(clipitto_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_capture PUBLIC clipitto_core Qt6::Gui Qt6::Concurrent)
clipitto_set_warnings(clipitto_capture)
