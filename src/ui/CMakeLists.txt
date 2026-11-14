add_library(clipitto_ui STATIC
    AppIcon.cpp AppIcon.h
    HistoryDelegate.cpp HistoryDelegate.h
    HistoryModel.cpp HistoryModel.h
    HistoryView.cpp HistoryView.h
    PopupWindow.cpp PopupWindow.h
    QrImage.cpp QrImage.h
    Prompts.cpp Prompts.h
    SearchBar.cpp SearchBar.h
    SearchController.cpp SearchController.h
    SelfTestWindow.cpp SelfTestWindow.h
    SettingsDialog.cpp SettingsDialog.h
    Theme.cpp Theme.h
    MacApp.h PlatformKeys.h
)
# macOS: the menu-bar-only activation policy and bringing the popup to the front (AppKit).
if(APPLE)
    target_sources(clipitto_ui PRIVATE MacApp.mm)
    target_link_libraries(clipitto_ui PRIVATE "-framework AppKit")
    target_compile_options(clipitto_ui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
    target_sources(clipitto_ui PRIVATE MacApp_none.cpp)
endif()

target_include_directories(clipitto_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_ui PUBLIC clipitto_core Qt6::Widgets PRIVATE clipitto_qrcodegen)
clipitto_set_warnings(clipitto_ui)
