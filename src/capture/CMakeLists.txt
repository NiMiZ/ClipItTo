add_library(clipitto_capture STATIC
    BackendSelector.cpp BackendSelector.h
    CaptureBackend.h
    GnomeExtBackend.h
    QtClipboardBackend.cpp QtClipboardBackend.h
    WlPasteBackend.cpp WlPasteBackend.h
    NativeClipboard.h
    SourceApp.h
    ImageCodec.cpp ImageCodec.h
    Ocr.h
)
# Per platform: the native clipboard reader and the app a password copy came from.
#   Windows: Win32 (clipboard owner's process); macOS: AppKit (NSPasteboard, frontmost app);
#   X11: Xlib (already needed for auto-paste); elsewhere unknown.
if(WIN32)
    target_sources(clipitto_capture PRIVATE NativeClipboard_win.cpp SourceApp_win.cpp Ocr_win.cpp)
    target_link_libraries(clipitto_capture PRIVATE Version WindowsApp) # GetFileVersionInfoW; Windows.Media.Ocr
elseif(APPLE)
    target_sources(clipitto_capture PRIVATE NativeClipboard_mac.mm SourceApp_mac.mm Ocr_mac.mm)
    target_link_libraries(clipitto_capture PRIVATE "-framework AppKit" "-framework Vision" "-framework Foundation")
    target_compile_options(clipitto_capture PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
elseif(UNIX AND X11_FOUND)
    target_sources(clipitto_capture PRIVATE NativeClipboard_none.cpp SourceApp_x11.cpp)
    target_link_libraries(clipitto_capture PRIVATE X11::X11)
else()
    target_sources(clipitto_capture PRIVATE NativeClipboard_none.cpp SourceApp_none.cpp)
endif()

# Text in pictures on Linux: Tesseract and Leptonica (Apache-2.0 / BSD-2-Clause) when their development
# files are installed; otherwise a build without it (Ocr_none.cpp). Windows and macOS use the system's.
if(NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TESSERACT QUIET IMPORTED_TARGET tesseract lept)
    endif()
    if(TARGET PkgConfig::TESSERACT)
        message(STATUS "Text recognition: Tesseract ${TESSERACT_tesseract_VERSION}")
        set(CLIPITTO_HAVE_TESSERACT ON CACHE INTERNAL "Built with Tesseract text recognition")
        target_sources(clipitto_capture PRIVATE Ocr_tesseract.cpp)
        target_link_libraries(clipitto_capture PRIVATE PkgConfig::TESSERACT)
    else()
        message(STATUS "Text recognition: off (install libtesseract-dev and libleptonica-dev to build it)")
        target_sources(clipitto_capture PRIVATE Ocr_none.cpp)
    endif()
endif()

target_include_directories(clipitto_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_capture PUBLIC clipitto_core Qt6::Gui Qt6::Concurrent)
clipitto_set_warnings(clipitto_capture)
