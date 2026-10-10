add_library(clipitto_core STATIC
    CapturePipeline.cpp CapturePipeline.h
    ClipFilter.cpp ClipFilter.h
    Config.cpp Config.h
    ControlChannel.h
    Entry.h
    Paths.cpp Paths.h
    SessionEnv.cpp SessionEnv.h
    db/FtsQuery.cpp db/FtsQuery.h
    db/Migrator.cpp db/Migrator.h
    db/Repository.cpp db/Repository.h
    db/Sqlite.cpp db/Sqlite.h
)

qt_add_resources(clipitto_core "clipitto_migrations"
    PREFIX "/clipitto"
    BASE db
    FILES db/migrations/001_init.sql
)

target_include_directories(clipitto_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_core
    PUBLIC Qt6::Core
    PRIVATE SQLite::SQLite3 tomlplusplus::tomlplusplus
)
clipitto_set_warnings(clipitto_core)
