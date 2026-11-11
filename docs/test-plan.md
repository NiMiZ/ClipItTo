# Test plan: more checks on the current features

Goal: more automated checks on what exists, in this order. Each area is one PR; a bug found by a new test gets its own fix and
the PR says so. Counts are test cases (a data row counts as one). Real Windows and macOS clipboards cannot be driven from CI;
what the tests cannot reach goes to [untested.md](untested.md).

| PR | Area | Tests |
|---|---|---|
| 1 | Passwords and grouping, end to end | 33 |
| 2 | Copying from many apps and styles | 30 |
| 3 | Performance and stalling | 45 |
| 4 | GUI in general | 32 |
| 5 | Picture search and translation | 20 |
| | Total | 160 |

## 1. Passwords and grouping (33)
- **Detection (8):** password-manager marker; built-in generated-password rule; the user's own rules (including the 6-character
  minimum); API key, token and card patterns; each with a master password, with "use without one", and with neither.
- **The name box (3):** moving a copy to the Passwords group opens the note prompt, in both vault modes.
- **Lock and unlock (5):** wrong password, *Lock now*, auto-lock timer, change the master password, remove it.
- **`p[]` search (2):** locked and unlocked.
- **Never in clear (3):** the history file, `debug.log` and the paste-back markers never hold the password.
- **Backup (2):** passwords survive backup and restore, with and without encryption.
- **Grouping (10):** auto-grouping learns, uses a threshold, takes corrections, never touches passwords or pictures; Ctrl+G and the
  Group menu; nested groups; deleting a group; *Forget what was learned*.

## 2. Copying from many apps and styles (30)
Simulated copies (the data formats each app puts on the clipboard) through the Qt, `wl-paste` and Windows backends:
- Word, LibreOffice, Chrome, Firefox (HTML and RTF); Excel, VS Code, a terminal.
- KeePassXC, Bitwarden, 1Password (privacy markers).
- Explorer, Nautilus, Finder (files); a screenshot and a picture from a browser.
- Text styles: URLs, colours, Hebrew and right-to-left text, emoji, CRLF, whitespace only, empty, 1 MB, the same copy twice, a burst.
- Formatting (6): paste plain, paste with formatting, the `keep_formatting` setting.

## 3. Performance and stalling (45)
A shared watchdog fails a test when the GUI event loop is blocked longer than its budget (100 ms) and records the longest stall.
All tests carry the CTest label `perf` (`ctest -L perf`). Limits are loose on CI and tighter locally.
- **Watchdog (1).**
- **Popup (9):** open with 1k, 10k and 50k copies; reopen 50 times (no slowdown, no memory growth); fast scroll through 50k rows; mixed
  rows (pictures, long text, file lists); thumbnails off the GUI thread; hover preview of a big picture; hide and show during a search.
- **Search (6):** typing in 50k copies; the `img:`, `group:` and `since:` filters; `p[]` with 5k passwords unlocked; search during a
  copy burst; stale searches dropped when typing fast; index size after a large import.
- **Capture (9):** a burst of 200 copies, also with the popup open; 1 MB text, 20 MB picture, 5 MB HTML or RTF; a clipboard owner that is
  slow or never answers; the same copy in a flood; per-copy cost with every detection on (secret scan, user rules, auto-grouping);
  sealing a password while locked (Argon2).
- **Background work (5):** OCR, translation, vault unlock, auto-grouping learning, the picture-indexing queue during a burst.
- **Big data (7):** backup export and import with 50k copies; startup, and migration, with a big database; clear and prune; pasting a
  large copy back; encryption on a large database; the WAL file does not grow without bound.
- **System (8):** Settings opens fast; saving Settings does not stall; a slow or unreachable keyring does not stall; a D-Bus or CLI call
  while the daemon is busy; memory growth over 10k copies; idle CPU (no timers spinning); hotkey-to-popup latency; shutdown time.

## 4. GUI in general (32)
- **Fresh install (10):** with an empty config the popup shows the expected sections and each default does what its label says.
- **Every Settings tab (8):** open with defaults, change each control, check it saves and reloads, disabled states and tooltips.
- **Save with no change (1):** leaves the config identical.
- **Keyboard (8):** Enter, Shift+Enter, Esc, arrows, Ctrl+G and Ctrl+E, delete, pin and star, search filters.
- **Rendering (3):** long text, emoji and right-to-left text, theme switch.
- **Reopening (2):** the popup resets cleanly each time.

## 5. Picture search and translation (20)
- **Picture search (4):** regression tests (it works today).
- **Translation (14):** with a fake engine, since CI builds without translation: a word that exists and one that does not;
  number-like words; Hebrew words such as מכשול; the timeout; a missing pack; the wrong direction; long text; punctuation; mixed
  languages; two translations at once; hiding the popup mid-translation; translating a picture; the result going to the clipboard
  and into the paste.
- **Quality check (2):** a script that runs a word list through the user's real language packs and prints the results (CI cannot judge
  translation quality), and its word list.
