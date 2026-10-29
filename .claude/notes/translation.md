# Offline translation (src/translate)

Engine: CTranslate2 (MIT) + SentencePiece (Apache-2.0), static, built by `scripts/build-translation-libs.sh|.ps1`
into a prefix (`include/`, `lib/libctranslate2_all.a` = CT2 + ruy + cpu_features + cpuinfo merged, `lib/libsentencepiece.a`,
`licenses/`). CMake finds it through `-DCLIPITTO_TRANSLATE_PREFIX` or the env var of that name (CI sets the env var via
`.github/actions/translation-libs`, cached per OS). Without it the build has `Translate_none.cpp` and no Translation tab.
Pins: CTranslate2 v4.5.0 (383d063d...), SentencePiece v0.2.0 (17d7580d...). Verified: no network symbols imported
(`nm -D --undefined-only`), works inside `unshare -rn`.

Packs = Argos `.argosmodel` zips (OPUS-MT, CC BY 4.0), NOT shipped; the user adds them in Settings > Translation.
`ZipReader` is our own minimal zip + inflate (no zip lib); `PackStore` installs only metadata.json, sentencepiece.model
and model/* into `<AppData>/clipitto/translation/<from>-<to>/`. Output pieces are joined and "▁" -> space.
UI hooks: `ui::TranslationSupport` (Translation.h) handed to PopupWindow and SettingsDialog by `Daemon::translationSupport()`;
translation runs on `m_translatePool`. Pictures: OCR first (m_textFromPicture), then the fitting pack (`fits` by script).
Tests: `tst_translate` (zip, inflate, packs, sentence splitting; a real-model test when `CLIPITTO_TEST_PACK=<zh->en .argosmodel>`),
fake engine tests in tst_popup / tst_settings.
Licence checklist for a future relicence: THIRD-PARTY-NOTICES.md ("If ClipItTo's own licence ever changes").
Windows (MSVC, ruy) and macOS (universal = arm64 + x86_64 built separately with CMAKE_SYSTEM_PROCESSOR forced, lipo'd) were
written without being run: expect CI iterations there.
