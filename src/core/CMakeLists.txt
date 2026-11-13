add_library(clipitto_core STATIC
    AutoForget.cpp AutoForget.h
    AutoGroup.cpp AutoGroup.h
    CapturePipeline.cpp CapturePipeline.h
    ClipFilter.cpp ClipFilter.h
    Backup.cpp Backup.h
    Config.cpp Config.h
    DebugLog.cpp DebugLog.h
    ControlChannel.h
    DesktopIntegration.h
    Entry.h
    ImageClip.h
    GroupPath.cpp GroupPath.h
    Hotkey.cpp Hotkey.h
    KeyStore.h
    PasswordKeeper.cpp PasswordKeeper.h
    PasswordPolicy.cpp PasswordPolicy.h
    PasswordRules.cpp PasswordRules.h
    NumberText.cpp NumberText.h
    PasteAs.cpp PasteAs.h
    PasteKeys.cpp PasteKeys.h
    TextTransform.cpp TextTransform.h
    SecretScan.cpp SecretScan.h
    SearchFilter.cpp SearchFilter.h
    PasswordVault.cpp PasswordVault.h
    Paths.cpp Paths.h
    Previews.cpp Previews.h
    RichText.cpp RichText.h
    SessionEnv.cpp SessionEnv.h
    db/FtsQuery.cpp db/FtsQuery.h
    db/Migrator.cpp db/Migrator.h
    db/Repository.cpp db/Repository.h
    db/Sqlite.cpp db/Sqlite.h
)

qt_add_resources(clipitto_core "clipitto_migrations"
    PREFIX "/clipitto"
    BASE db
    FILES db/migrations/001_init.sql db/migrations/002_groups.sql db/migrations/003_pins_and_order.sql db/migrations/004_images.sql db/migrations/005_passwords.sql db/migrations/006_recent_rules.sql db/migrations/007_picture_text.sql db/migrations/008_rich_text.sql db/migrations/009_auto_grouping.sql db/migrations/010_paste_once.sql
)

target_include_directories(clipitto_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_core
    PUBLIC Qt6::Core Qt6::Concurrent
    PRIVATE clipitto_sqlite tomlplusplus::tomlplusplus clipitto_monocypher
)
clipitto_set_warnings(clipitto_core)
