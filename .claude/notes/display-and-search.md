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
- Ordinary: FTS5 trigram over content + note + group (`Repository::search`). Pictures match note/group/"image".
  Passwords match by note/group only (content is the label "Password").
- Password search: typing the keyword (default `p[]`) + space switches `SearchBar` to a masked mode:
  real text kept in a buffer, display `p[] ****`; Backspace on empty leaves the mode; buffer wiped on
  hide/Esc. `SearchController::setPasswordQuery` -> `PasswordKeeper::search`: unlocked = decrypt and
  match part, case-insensitive; locked = exact only (keyed hash lookup, needs keyring), else a
  "Unlock to search inside passwords" hint.
- `ignore_case` only changes the *dedupe hash* (`Repository::textHash`, case-folded), not FTS.

## Popup placement on Wayland
Wayland clients can't place windows; the GNOME extension does it: the daemon calls `ActivateWindowAt(position)`
(falls back to `ActivateWindow` = "center" for an older extension, which stays loaded until re-login), the
extension computes the spot (`popupPlacement`, mirrors `PopupWindow::placeOnScreen`), remembers "last" itself from
the window's position-changed, and centers any transient ClipItTo dialog (Settings) over the popup. Tests:
`gnome-extension/tests/test_extension.js` (fake shell, run by `ctest -R gnome_extension`) and `tst_dbus`.
Not verified on a real GNOME session. KDE / wlroots: no placement.

## Tests
`tests/tst_popup.cpp` (menus, Delete keeps group, Clear counts: "Clear Recent (N copies)", N excludes
pinned), `tst_repository.cpp` (recent rules), `tst_settings.cpp`. Run: `ctest --preset debug`.
Drive the real app headlessly with the `run-clipitto` skill (screenshots only in its xvfb session).
