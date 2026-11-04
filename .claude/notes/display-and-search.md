# Display and search (popup UI)

Files: `src/ui/PopupWindow.{h,cpp}` (1.3k lines: menus, key handling, Delete/Clear), `HistoryModel`
(rows: pinned, Groups tree, Recent), `HistoryDelegate` (painting, lock/●●●● rows, thumbnails),
`HistoryView`, `SearchBar` (+ masked `p[] ` mode), `SearchController` (debounced search),
`Prompts.h` (injectable TextPrompt/PasswordPrompt/FilePrompt/PartsPrompt so tests never block).

## Layout of the list
Search box, then Pinned (max 10, above Recent), a collapsed **Groups** row (tree: sub-groups, then
copies; Favorites = first group), then **Recent**. Starred-in-group copies are listed first in the group (max 10).

## Recent (rule lives in SQL macro `IN_RECENT` in Repository.cpp)
Visible if `is_saved = 0` OR (`off_recent = 0` AND not hidden by group/favorites rule). Pinned are
excluded from Recent (listed above). Hidden when: copy's group or an ancestor has `hide_from_recent`,
or (no group) it is starred and meta `favorites_hide_from_recent = 1`.
- **Delete** (`PopupWindow::remove`) on a top-level, non-pinned, starred/grouped copy in Recent only
  sets `off_recent` (stays in group). In a group / unfiled copy: really deletes.
- **Clear** button = `clearRecent`: deletes unfiled, sets off_recent on filed, pinned stay. Menu has
  "Clear everything, also in groups". Both confirm.
- Group menu: checkable **Keep out of Recent** (after "Move down"; tests index actions by position,
  keep it there). Favorites row has the same via `createFavoritesMenu`.
- Re-copying a copy resets `off_recent = 0` (ingest ON CONFLICT).
- `latest_after_paste`: choosing a copy bumps it, except saved ones (`Daemon::onEntryActivated`).

## Search
- Results of every kind (pinned/grouped/unsaved) are listed newest first (`last_used DESC`), not by relevance or saved-first. Start focus "Unsaved" = top of Recent (first entry after the Pinned section), or the top search result.
- Ordinary: FTS5 trigram over content + note + group (`Repository::search`). Pictures match note/group/"image".
  Passwords match by note/group only (content is the label "Password").
- Password search: typing the keyword (default `p[]`) + space switches `SearchBar` to a masked mode:
  real text kept in a buffer, display `p[] ****`; Backspace on empty leaves the mode; buffer wiped on
  hide/Esc. `SearchController::setPasswordQuery` -> `PasswordKeeper::search`: unlocked = decrypt and
  match part, case-insensitive; locked = exact only (keyed hash lookup, needs keyring), else a
  "Unlock to search inside passwords" hint.
- `ignore_case` only changes the *dedupe hash* (`Repository::textHash`, case-folded), not FTS.

## Search inside pictures
Migration 007 `clip_text(clip_id, text)`, cascade-deleted with the picture; not in the FTS index. `Repository::search`
merges `searchPictureText` (LIKE over image rows) into the normal result (`mergeNewest`); `searchFiltered` adds an EXISTS per
word. `Daemon::indexNextPicture` reads one unread picture at a time on `m_helloPool` (after each stored copy, 3 s after start,
and when the setting changes); an engine error sets `m_indexFailed` until the settings change; "" = read, nothing found.
`index_pictures` off -> `clearPictureText`. Never log the text. Only Windows tests ran the OCR path; Tesseract not tried here.

## Copy with formatting
Migration 008 `clip_rich(clip_id, html)`, cascade-deleted, not in FTS; `Entry::rich` is an EXISTS in `CLIP_COLUMNS`.
Capture: `CaptureBackend::captured(text, mimeTypes, html)` (3rd arg new): `QtClipboardBackend::htmlOf` (only when the
clipboard's text equals the native read's text), `WlPasteBackend::fetchHtml` (second wl-paste call), GNOME extension ->
D-Bus `IngestRich` (older daemon: extension falls back to `Ingest`). `CapturePipeline::process` keeps it only when
`richtext::hasFormatting` (tags b/i/a/lists/tables/headings/img or a style with weight/style/decoration/background; a
colour or font family alone is not formatting), under `max_item_bytes`, with `keep_formatting`. Never for marked or
secret-scanned copies. The newest copy of the same text decides (plain re-copy clears it); editing the text or
Mark as password drops it; turning the setting off deletes all (`clearAllRich`). Paste: Paste as › `rich`
(`pasteas::kRich`, last in the default list, only offered when `entry.rich`) -> `richActivated` -> `Daemon::onRichActivated`
-> `ClipboardWriter::setRich` (Qt clipboard with text + html, even with the GNOME integration, which takes one format per
write). Not in backups. Windows/macOS read path compiled by CI only.

## QR code view
Right-click a text copy (not a password, not a picture) › "Show as QR code…" -> `PopupWindow::m_qrView` (injectable `QrView`,
`defaultQrView()` in Prompts.cpp: modal dialog, Esc closes). `ui::qrImage(text, minSide, &error)` (QrImage.cpp) renders
with Nayuki's qrcodegen 1.8.0 (`clipitto_qrcodegen`, FetchContent in cmake/Dependencies.cmake, sha pinned; offline:
FETCHCONTENT_SOURCE_DIR_QRCODEGEN): ECC medium, falling back to low, 4-module quiet zone, whole pixels per module;
over 2953 UTF-8 bytes -> null + message. Nothing is logged or stored.

## Popup placement on Wayland
Wayland clients can't place windows; the GNOME extension does it: the daemon calls `ActivateWindowAt(position)`
(falls back to `ActivateWindow` = "center" for an older extension, which stays loaded until re-login), the
extension computes the spot (`popupPlacement`, mirrors `PopupWindow::placeOnScreen`), remembers "last" itself from
the window's position-changed, and centers any transient ClipItTo dialog (Settings) over the popup. Tests:
`gnome-extension/tests/test_extension.js` (fake shell, run by `ctest -R gnome_extension`) and `tst_dbus`.
Not verified on a real GNOME session. KDE / wlroots: no placement.

## Text from pictures (OCR)
Right-click a picture › "Paste text from picture". `src/capture/Ocr.h`: `available()` / `recognize(png)`, blocking,
one file per platform (`Ocr_win.cpp` Windows.Media.Ocr via C++/WinRT, `Ocr_mac.mm` Vision, `Ocr_tesseract.cpp` on
Linux when pkg-config finds tesseract+lept, else `Ocr_none.cpp`). The Daemon injects it into the popup
(`setTextFromPicture`, runs on `m_ocrPool`, DB read on the GUI thread) and the result goes out like "Paste as…"
(`textActivated`). Linux data: system tessdata, or `~/.local/share/clipitto/tessdata`; AppImage bundles
`eng.traineddata` (tessdata_fast 4.1.0, sha pinned in build-appimage.sh) and sets TESSDATA_PREFIX; snap stages
libtesseract5 + tesseract-ocr-eng; .deb Depends libtesseract5, Recommends tesseract-ocr-eng. Never use
showMessage() for progress (it is a modal box and hangs tests): use setBusy(). Only the Linux build without
Tesseract was compiled locally; Windows, macOS and the Tesseract path are compiled by CI only, never run.

## Tests
`tests/tst_popup.cpp` (menus, Delete keeps group, Clear counts: "Clear Recent (N copies)", N excludes
pinned), `tst_repository.cpp` (recent rules), `tst_settings.cpp`. Run: `ctest --preset debug`.
Drive the real app headlessly with the `run-clipitto` skill (screenshots only in its xvfb session).
