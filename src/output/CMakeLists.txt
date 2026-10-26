add_library(clipitto_output STATIC
    ClipboardWriter.cpp ClipboardWriter.h
    SyntheticPaste.cpp SyntheticPaste.h
)

target_include_directories(clipitto_output PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_output PUBLIC clipitto_core Qt6::Gui)
clipitto_set_warnings(clipitto_output)

# X11 auto-paste (paste_on_enter outside GNOME Wayland). Needs libx11-dev and libxtst-dev.
if(CLIPITTO_X11_PASTE)
    target_compile_definitions(clipitto_output PRIVATE CLIPITTO_HAVE_XTEST)
    target_link_libraries(clipitto_output PRIVATE X11::X11 X11::Xtst)
endif()

# macOS: a pasted password carries nspasteboard.org's ConcealedType, which Qt can't write;
# auto-paste presses ⌘V with Quartz events.
if(NOT APPLE)
    target_sources(clipitto_output PRIVATE MacPaste_none.cpp MacPaste.h) # stubs: the daemon's status asks
endif()
if(APPLE)
    target_sources(clipitto_output PRIVATE MacPasteboard.mm MacPasteboard.h MacPaste.mm MacPaste.h)
    target_link_libraries(clipitto_output PRIVATE "-framework AppKit" "-framework ApplicationServices")
    target_compile_options(clipitto_output PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()
