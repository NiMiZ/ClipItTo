# Offline translation. The engine is CTranslate2 + SentencePiece, built by scripts/build-translation-libs.sh
# (or .ps1) into a prefix that CLIPITTO_TRANSLATE_PREFIX points at; without it the build has no translation
# (Translate_none.cpp) and the rest of ClipItTo is unchanged.
add_library(clipitto_translate STATIC
    Translate.h
    TextSplit.cpp PackStore.cpp ZipReader.cpp ZipReader.h
)

set(CLIPITTO_TRANSLATE_PREFIX "" CACHE PATH "Prefix holding CTranslate2 and SentencePiece (scripts/build-translation-libs.*)")
set(_have_translate OFF)
if(CLIPITTO_TRANSLATE_PREFIX)
    find_path(CLIPITTO_CT2_INCLUDE ctranslate2/translator.h PATHS "${CLIPITTO_TRANSLATE_PREFIX}/include" NO_DEFAULT_PATH)
    find_path(CLIPITTO_SP_INCLUDE sentencepiece_processor.h PATHS "${CLIPITTO_TRANSLATE_PREFIX}/include" NO_DEFAULT_PATH)
    find_library(CLIPITTO_CT2_LIB NAMES ctranslate2_all PATHS "${CLIPITTO_TRANSLATE_PREFIX}/lib" NO_DEFAULT_PATH)
    find_library(CLIPITTO_SP_LIB NAMES sentencepiece PATHS "${CLIPITTO_TRANSLATE_PREFIX}/lib" NO_DEFAULT_PATH)
    if(CLIPITTO_CT2_INCLUDE AND CLIPITTO_SP_INCLUDE AND CLIPITTO_CT2_LIB AND CLIPITTO_SP_LIB)
        set(_have_translate ON)
    else()
        message(WARNING "Translation libraries not found under ${CLIPITTO_TRANSLATE_PREFIX}: building without translation")
    endif()
endif()

if(_have_translate)
    message(STATUS "Translation: CTranslate2 + SentencePiece from ${CLIPITTO_TRANSLATE_PREFIX}")
    target_sources(clipitto_translate PRIVATE Translate_ct2.cpp)
    # System include paths: the libraries' headers are not ours to warn about.
    target_include_directories(clipitto_translate SYSTEM PRIVATE "${CLIPITTO_CT2_INCLUDE}" "${CLIPITTO_SP_INCLUDE}")
    target_link_libraries(clipitto_translate PRIVATE "${CLIPITTO_CT2_LIB}" "${CLIPITTO_SP_LIB}")
    if(APPLE)
        target_link_libraries(clipitto_translate PRIVATE "-framework Accelerate")
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(clipitto_translate PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    set(CLIPITTO_HAVE_TRANSLATION ON CACHE INTERNAL "Built with the translation engine")
else()
    message(STATUS "Translation: off (set CLIPITTO_TRANSLATE_PREFIX; see scripts/build-translation-libs.sh)")
    target_sources(clipitto_translate PRIVATE Translate_none.cpp)
endif()

target_include_directories(clipitto_translate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clipitto_translate PUBLIC Qt6::Core)
clipitto_set_warnings(clipitto_translate)
