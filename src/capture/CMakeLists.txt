add_library(clipitto_capture STATIC
    BackendSelector.cpp BackendSelector.h
    CaptureBackend.h
    GnomeExtBackend.h
    QtClipboardBackend.cpp QtClipboardBackend.h
    WlPasteBackend.cpp WlPasteBackend.h
    NativeClipboard.h
    SourceApp.h
    ImageCodec.cpp ImageCodec.h
)
if(WIN32)
    target_sources(clipitto_capture PRIVATE NativeClipboard_win.cpp SourceApp_win.cpp)
    target_link_libraries(clipitto_capture PRIVATE Version) # GetFileVersionInfoW
else()
    target_sources(clipitto_capture PRIVATE NativeClipboard_none.cpp)
endif()
# The app a password copy came from: Windows above; X11 through Xlib (already needed for
# auto-paste); elsewhere unknown.
if(UNIX AND X11_FOUND)
    target_sources(clipitto_capture PRIVATE SourceApp_x11.cpp)
    target_link_libraries(clipitto_capture PRIVATE X11::X11)
elseif(NOT WIN32)
    target_sources(clipitto_capture PRIVATE SourceApp_none.cpp)
endif()

target_include_directories(clipitto_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_capture PUBLIC clipitto_core Qt6::Gui Qt6::Concurrent)
clipitto_set_warnings(clipitto_capture)
