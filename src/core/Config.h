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

// User settings from ~/.config/clipitto/config.toml. Missing or invalid keys fall back to defaults.
struct Config {
    int maxHistory = 1000; // unsaved items kept; saved items don't count
    int recentCount = 20;
    bool showSavedOnTop = true;
    int savedCap = 15;
    qint64 maxItemBytes = 1024 * 1024;
    bool capturePrimary = false;
    bool pasteOnEnter = false;
    PasteKeys pasteKeys = PasteKeys::CtrlV;
    ClickMode clickToCopy = ClickMode::Single;
    bool hideOnFocusLoss = true;
    int searchDebounceMs = 100;
    BackendChoice backend = BackendChoice::Auto;
    QString hotkey = QStringLiteral("Ctrl+Alt+V"); // global shortcut registered by the daemon (Windows)

    // Parse errors, unknown keys and out-of-range values are reported in warnings (if given)
    // and never fail: the affected values keep their defaults.
    static Config fromToml(std::string_view text, QStringList* warnings = nullptr);
    // A missing file yields defaults without a warning.
    static Config load(const QString& path, QStringList* warnings = nullptr);

    std::string toToml() const;
    // Atomic write; creates the parent directory.
    bool save(const QString& path, QString* error = nullptr) const;

    bool operator==(const Config&) const = default;
};

} // namespace clipitto
