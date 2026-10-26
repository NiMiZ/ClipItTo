# The macOS app bundle's layout (install into a staging prefix):
#   <prefix>/ClipItTo.app/Contents/{Info.plist, MacOS/{clipitto-daemon,clipitto}, Resources/...}
# packaging/macos/package.sh installs there, bundles Qt with macdeployqt, signs the result
# ad hoc and builds the .dmg. clipitto-daemon is the app's executable (LSUIElement: a menu-bar
# app, no Dock icon); the CLI sits beside it for `--toggle`, `--status`, ...

set(CLIPITTO_APP ClipItTo.app)
install(TARGETS clipitto-daemon clipitto RUNTIME DESTINATION ${CLIPITTO_APP}/Contents/MacOS)

configure_file(packaging/macos/Info.plist.in ${PROJECT_BINARY_DIR}/macos/Info.plist @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/macos/Info.plist DESTINATION ${CLIPITTO_APP}/Contents)
install(FILES packaging/macos/ClipItTo.icns DESTINATION ${CLIPITTO_APP}/Contents/Resources)
install(FILES data/config.example.toml README.md LICENSE THIRD-PARTY-NOTICES.md
        DESTINATION ${CLIPITTO_APP}/Contents/Resources/doc)
install(FILES licenses/LGPL-3.0.txt DESTINATION ${CLIPITTO_APP}/Contents/Resources/doc/licenses)
