add_library(clipitto_output STATIC
    ClipboardWriter.cpp ClipboardWriter.h
    X11Paster.cpp X11Paster.h
)

target_include_directories(clipitto_output PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_output PUBLIC clipitto_core Qt6::Gui)
clipitto_set_warnings(clipitto_output)

# X11 auto-paste (paste_on_enter outside GNOME Wayland). Needs libx11-dev and libxtst-dev.
if(CLIPITTO_X11_PASTE)
    target_compile_definitions(clipitto_output PRIVATE CLIPITTO_HAVE_XTEST)
    target_link_libraries(clipitto_output PRIVATE X11::X11 X11::Xtst)
endif()
