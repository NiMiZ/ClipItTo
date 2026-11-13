#pragma once

#include "AutoForget.h"
#include "PasswordRules.h"
#include "PasteKeys.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <string>
#include <string_view>

namespace clipitto {

enum class ClickMode { Single, Double };
enum class BackendChoice { Auto, GnomeExt, WlPaste, QtClipboard };
enum class Theme { System, Light, Dark };
// Where the popup opens (X11 and Windows; Wayland compositors place windows themselves).
// Last: where it was when it last closed, e.g. after dragging it by its name.
enum class PopupPosition { Cursor, Center, TopLeft, TopRight, BottomLeft, BottomRight, Last };
// "cursor", "center", "top-left", ... as written in the config file.
QString popupPositionName(PopupPosition position);
// Where the selection starts when the popup opens and after each search: on the first copy
// of that kind in the list (the first copy, when there is none).
enum class StartFocus { Pinned, Grouped, Unsaved };

// User settings from ~/.config/clipitto/config.toml. Missing or invalid keys fall back to defaults.
struct Config {
    int maxHistory = 1000; // unsaved items kept; pinned, starred and grouped ones don't count
    int recentCount = 20; // copies listed when the search box is empty
    int previewLines = 1; // text lines shown per copy
    bool showSavedOnTop = true; // the Pinned list above recent copies
    int savedCap = 15; // at most this many pinned copies listed (pins themselves are capped at 10)
    int favoritesLimit = 10; // starred copies without a group (1..20); 10 per group otherwise
    qint64 maxItemBytes = 1024 * 1024;
    QString ocrLanguages; // languages to read in pictures, e.g. "en, he"; empty = automatic
    bool indexPictures = true; // read the text in copied pictures, so a search finds them by it
    bool debugLog = false; // write a diagnostic log file (what ClipItTo does, never what was copied) for troubleshooting
    bool autoGroup = false; // file new copies in the group they match best, learned from the user's groups (off by default)
    bool keepFormatting = true; // also keep the HTML of a copy with bold, links, tables… for "Paste with formatting"
    QString keyboardLayouts; // layouts offered by Paste as…: "he, ru, el"; "" = the system language's; "none"
    QString pasteAsText; // items of the Paste as… menu for text, see PasteAs.h; "" = the built-in list
    QString pasteAsPicture; // the same for pictures
    int translateTimeout = 5; // seconds (1..10) a translation may take before it is cancelled
    bool pasteAsOthers = true; // the Paste as… menu ends with "Others": the items left unticked
    int maxImages = 100; // unsaved pictures kept, apart from maxHistory; 0 = don't record pictures
    qint64 maxImageBytes = 50LL * 1024 * 1024; // larger pictures (PNG-encoded) are skipped
    bool capturePrimary = false;
    bool pasteOnEnter = true; // choosing a copy also pastes it into the window you came from
    bool latestAfterPaste = true; // a chosen copy becomes the latest in the history, as if copied
    PasteKeys pasteKeys = PasteKeys::CtrlV; // ctrl+v | shift+insert, for every program no rule names
    bool pasteKeysPerApp = true; // the keys follow the program that gets the paste (terminals: ctrl+shift+v)
    QList<PasteKeyRule> pasteKeysApps; // your own "program = keys" rules (up to 16), tried before the built-in terminals
    ClickMode clickToCopy = ClickMode::Single;
    bool hideOnFocusLoss = true;
    int searchDebounceMs = 100;
    BackendChoice backend = BackendChoice::Auto;
#ifdef Q_OS_MACOS
    QString hotkey = QStringLiteral("Alt+Cmd+V"); // ⌘⌥V in Apple's modifier order, registered by the daemon (Carbon)
#else
    QString hotkey = QStringLiteral("Ctrl+Alt+V"); // Windows: registered by the daemon; GNOME: custom shortcut
#endif
    Theme theme = Theme::System;
    PopupPosition popupPosition = PopupPosition::Cursor; // X11/Windows; Wayland compositors place it
    StartFocus startFocus = StartFocus::Pinned;
    bool ignoreCase = false; // "ABC" and "abc" are one copy (the one copied last is kept)
    bool encryptHistory = false; // history.db encrypted at rest, key in the system keyring
    // Password copies (copies marked by a password manager), once a master password is set.
    bool keepPasswords = true;
    bool passwordsInGroup = true; // new ones go to the "Passwords" group
    bool detectSecrets = true; // copies that look like keys/tokens/card numbers are kept as password copies
    bool detectPasswords = false; // copies that look like generated passwords are kept as password copies (guesses; off)
    QList<PasswordRule> passwordRules; // user rules (up to 5) for what else is kept as a password copy
    bool passwordsPerNote = true; // the same password under another note is another copy
    bool passwordAskNote = true; // after copying a new password, open ClipItTo to name it
    int passwordUnlockMinutes = 5; // an unlock lasts this long
    int passwordClearSeconds = 2; // a pasted password leaves the clipboard this long after the paste (0..60); 0 = stays
    int pasteOnceClearSeconds = 2; // a "Paste once" copy leaves the clipboard this long after its paste (0..60); 0 = stays
    QList<AutoForgetRule> autoForget; // rules (up to 8) for copies that remove themselves; none = nothing is removed
    // Sync through a folder the user syncs (Sync.h, docs/sync-design.md). The passphrase is not a setting: it is never stored.
    bool syncEnabled = false;
    QString syncFolder;
    QString syncDeviceName; // empty: the computer's name
    int syncMaxCopies = 2000; // unsaved copies in this PC's sync file; every saved copy is in it besides
    bool syncPictures = true;
    int syncPictureMaxKb = 2048; // larger pictures are not synced
    int syncTombstoneDays = 30; // a deletion is announced this long
    bool windowsHello = false; // Windows: unlock with Windows Hello too
    QString passwordSearchKeyword = QStringLiteral("p[]"); // "p[] " starts a password search
    // Panic key (roadmap 20): one shortcut that wipes the history, locks the passwords and empties the system clipboard,
    // with no question. Off by default; `panicKey` only gates the shortcut (`clipitto --panic` always works).
    bool panicKey = false;
    QString panicHotkey; // empty = none; same spelling as `hotkey`

    // Lock the password vault when the screen locks or the PC sleeps (default on: locking is the safe direction).
    bool lockOnScreenLock = true;

    // Parse errors, unknown keys and out-of-range values are reported in warnings (if given)
    // and never fail: the affected values keep their defaults.
    static Config fromToml(std::string_view text, QStringList* warnings = nullptr);
    // A missing file yields defaults without a warning.
    static Config load(const QString& path, QStringList* warnings = nullptr);

    std::string toToml() const; // a complete file, comments aligned
    // `existing` with each known key's value replaced in place (comments, unknown keys and
    // line endings kept) and missing keys appended.
    std::string mergeInto(std::string_view existing) const;
    // Atomic write; creates the parent directory. Updates an existing file via mergeInto().
    bool save(const QString& path, QString* error = nullptr) const;

    bool operator==(const Config&) const = default;
};

} // namespace clipitto
