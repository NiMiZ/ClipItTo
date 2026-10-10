add_library(clipitto_ui STATIC
    AppIcon.cpp AppIcon.h
    HistoryDelegate.cpp HistoryDelegate.h
    HistoryModel.cpp HistoryModel.h
    HistoryView.cpp HistoryView.h
    PopupWindow.cpp PopupWindow.h
    Prompts.cpp Prompts.h
    SearchBar.cpp SearchBar.h
    SearchController.cpp SearchController.h
    SettingsDialog.cpp SettingsDialog.h
    Theme.cpp Theme.h
)

target_include_directories(clipitto_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_ui PUBLIC clipitto_core Qt6::Widgets)
clipitto_set_warnings(clipitto_ui)
