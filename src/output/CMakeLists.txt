add_library(clipitto_output STATIC
    ClipboardWriter.cpp ClipboardWriter.h
)

target_include_directories(clipitto_output PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_output PUBLIC clipitto_core Qt6::Gui)
clipitto_set_warnings(clipitto_output)
