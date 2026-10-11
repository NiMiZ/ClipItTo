#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <string>
#include <string_view>

namespace clipitto {

enum class PasteKeys { CtrlV, ShiftInsert };
enum class ClickMode { Single, Double };
enum class BackendChoice { Auto, GnomeExt, WlPaste, QtClipboard };
enum class Theme { System, Light, Dark };
enum class PopupPosition { Cursor, Center };

// User settings from ~/.config/clipitto/config.toml. Missing or invalid keys fall back to defaults.
struct Config {
    int maxHistory = 1000; // unsaved items kept; pinned, starred and grouped ones don't count
    int recentCount = 20; // copies listed when the search box is empty
    int previewLines = 1; // text lines shown per copy
    bool showSavedOnTop = true; // the Pinned list above recent copies
    int savedCap = 15; // at most this many pinned copies listed (pins themselves are capped at 10)
    int favoritesLimit = 10; // starred copies without a group (1..20); 10 per group otherwise
    qint64 maxItemBytes = 1024 * 1024;
    bool capturePrimary = false;
    bool pasteOnEnter = true; // choosing a copy also pastes it into the window you came from
    PasteKeys pasteKeys = PasteKeys::CtrlV;
    ClickMode clickToCopy = ClickMode::Single;
    bool hideOnFocusLoss = true;
    int searchDebounceMs = 100;
    BackendChoice backend = BackendChoice::Auto;
    QString hotkey = QStringLiteral("Ctrl+Alt+V"); // Windows: registered by the daemon; GNOME: custom shortcut
    Theme theme = Theme::System;
    PopupPosition popupPosition = PopupPosition::Cursor; // X11/Windows; Wayland compositors place it
    bool encryptHistory = false; // history.db encrypted at rest, key in the system keyring

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
