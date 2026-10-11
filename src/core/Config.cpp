#include "Config.h"

#include "Hotkey.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <toml++/toml.hpp>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace clipitto {

namespace {

template <typename Enum, std::size_t N>
using EnumNames = std::array<std::pair<Enum, const char*>, N>;

constexpr EnumNames<PasteKeys, 2> kPasteKeys{{{PasteKeys::CtrlV, "ctrl+v"}, {PasteKeys::ShiftInsert, "shift+insert"}}};
constexpr EnumNames<ClickMode, 2> kClickModes{{{ClickMode::Single, "single"}, {ClickMode::Double, "double"}}};
// The first name of a value is the one written back; later ones are accepted aliases.
constexpr EnumNames<PopupPosition, 7> kPopupPositions{{{PopupPosition::Cursor, "cursor"},
                                                       {PopupPosition::Center, "center"},
                                                       {PopupPosition::TopLeft, "top-left"},
                                                       {PopupPosition::TopRight, "top-right"},
                                                       {PopupPosition::BottomLeft, "bottom-left"},
                                                       {PopupPosition::BottomRight, "bottom-right"},
                                                       {PopupPosition::Last, "last"}}};
constexpr EnumNames<Theme, 3> kThemes{{{Theme::System, "system"}, {Theme::Light, "light"}, {Theme::Dark, "dark"}}};
constexpr EnumNames<BackendChoice, 5> kBackends{{{BackendChoice::Auto, "auto"},
                                                 {BackendChoice::GnomeExt, "gnome-ext"},
                                                 {BackendChoice::WlPaste, "wl-paste"},
                                                 {BackendChoice::QtClipboard, "qt-clipboard"},
                                                 {BackendChoice::QtClipboard, "x11"}}};

template <typename Enum, std::size_t N>
const char* nameOf(const EnumNames<Enum, N>& names, Enum value)
{
    for (const auto& [v, name] : names) {
        if (v == value) {
            return name;
        }
    }
    return names.front().second;
}

class Reader {
public:
    Reader(const toml::table& table, QStringList* warnings) : m_table(table), m_warnings(warnings) {}

    template <typename T>
    void integer(const char* key, T& out, qint64 min, qint64 max)
    {
        const toml::node* node = find(key);
        if (!node) {
            return;
        }
        const auto value = node->value_exact<int64_t>();
        if (!value) {
            warn(QStringLiteral("%1: expected an integer").arg(QLatin1String(key)));
        } else if (*value < min || *value > max) {
            warn(QStringLiteral("%1: %2 is outside %3..%4").arg(QLatin1String(key)).arg(*value).arg(min).arg(max));
        } else {
            out = T(*value);
        }
    }

    void boolean(const char* key, bool& out)
    {
        const toml::node* node = find(key);
        if (!node) {
            return;
        }
        if (const auto value = node->value_exact<bool>()) {
            out = *value;
        } else {
            warn(QStringLiteral("%1: expected true or false").arg(QLatin1String(key)));
        }
    }

    template <typename Enum, std::size_t N>
    void choice(const char* key, Enum& out, const EnumNames<Enum, N>& names)
    {
        const toml::node* node = find(key);
        if (!node) {
            return;
        }
        if (const auto value = node->value_exact<std::string>()) {
            for (const auto& [v, name] : names) {
                if (*value == name) {
                    out = v;
                    return;
                }
            }
        }
        QStringList allowed;
        for (const auto& entry : names) {
            if (nameOf(names, entry.first) == entry.second) { // skip aliases
                allowed << QString::fromLatin1(entry.second);
            }
        }
        warn(QStringLiteral("%1: expected one of %2").arg(QLatin1String(key), allowed.join(QLatin1String(", "))));
    }

    void hotkey(const char* key, QString& out)
    {
        const toml::node* node = find(key);
        if (!node) {
            return;
        }
        const auto value = node->value_exact<std::string>();
        const auto parsed = value ? parseHotkey(QString::fromStdString(*value)) : std::nullopt;
        if (parsed) {
            out = parsed->canonical;
        } else {
            warn(QStringLiteral("%1: expected a shortcut like \"Ctrl+Alt+V\"").arg(QLatin1String(key)));
        }
    }

    void warnUnknownKeys()
    {
        for (const auto& [key, node] : m_table) {
            if (!m_known.count(std::string(key.str()))) {
                warn(QStringLiteral("unknown key '%1'").arg(QString::fromUtf8(key.str().data(), qsizetype(key.str().size()))));
            }
        }
    }

private:
    const toml::node* find(const char* key)
    {
        m_known.insert(key);
        return m_table.get(key);
    }

    void warn(const QString& message)
    {
        if (m_warnings) {
            *m_warnings << message;
        }
    }

    const toml::table& m_table;
    QStringList* m_warnings;
    std::set<std::string> m_known;
};

} // namespace

Config Config::fromToml(std::string_view text, QStringList* warnings)
{
    Config config;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        if (warnings) {
            *warnings << QStringLiteral("parse error at line %1: %2")
                             .arg(e.source().begin.line)
                             .arg(QString::fromUtf8(e.description().data(), qsizetype(e.description().size())));
        }
        return config;
    }

    Reader r(table, warnings);
    r.integer("max_history", config.maxHistory, 10, 1'000'000);
    r.integer("recent_count", config.recentCount, 1, 500);
    r.integer("preview_lines", config.previewLines, 1, 10);
    r.boolean("show_saved_on_top", config.showSavedOnTop);
    r.integer("saved_cap", config.savedCap, 0, 500);
    r.integer("favorites_limit", config.favoritesLimit, 1, 20);
    r.integer("max_item_bytes", config.maxItemBytes, 1, 64LL * 1024 * 1024);
    r.boolean("capture_primary", config.capturePrimary);
    r.boolean("paste_on_enter", config.pasteOnEnter);
    r.boolean("latest_after_paste", config.latestAfterPaste);
    r.choice("paste_keys", config.pasteKeys, kPasteKeys);
    r.choice("click_to_copy", config.clickToCopy, kClickModes);
    r.boolean("hide_on_focus_loss", config.hideOnFocusLoss);
    r.integer("search_debounce_ms", config.searchDebounceMs, 0, 2000);
    r.choice("backend", config.backend, kBackends);
    r.hotkey("hotkey", config.hotkey);
    r.choice("theme", config.theme, kThemes);
    r.choice("popup_position", config.popupPosition, kPopupPositions);
    r.boolean("encrypt_history", config.encryptHistory);
    r.warnUnknownKeys();
    return config;
}

Config Config::load(const QString& path, QStringList* warnings)
{
    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (warnings) {
            *warnings << QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        }
        return {};
    }
    // Binary read: TOML is UTF-8 by definition, and the parser accepts LF and CRLF files.
    const QByteArray data = file.readAll();
    return fromToml(std::string_view(data.constData(), std::size_t(data.size())), warnings);
}

namespace {

struct Line {
    const char* key;
    std::string value; // already TOML-formatted
    const char* comment; // may be empty
};

std::string tomlString(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out + '"';
}

std::string tomlBool(bool v)
{
    return v ? "true" : "false";
}

std::vector<Line> lines(const Config& c)
{
    return {
        {"max_history", std::to_string(c.maxHistory), "unsaved copies kept; pinned, starred and grouped ones are kept"},
        {"recent_count", std::to_string(c.recentCount), "copies listed when the search box is empty"},
        {"preview_lines", std::to_string(c.previewLines), "text lines shown per copy"},
        {"show_saved_on_top", tomlBool(c.showSavedOnTop), "list pinned copies above recent ones"},
        {"saved_cap", std::to_string(c.savedCap), ""},
        {"favorites_limit", std::to_string(c.favoritesLimit), "starred copies without a group (1-20)"},
        {"max_item_bytes", std::to_string(c.maxItemBytes), ""},
        {"capture_primary", tomlBool(c.capturePrimary), ""},
        {"paste_on_enter", tomlBool(c.pasteOnEnter), "choosing a copy also pastes it"},
        {"latest_after_paste", tomlBool(c.latestAfterPaste), "a chosen copy becomes the latest"},
        {"paste_keys", tomlString(nameOf(kPasteKeys, c.pasteKeys)), "ctrl+v | shift+insert"},
        {"click_to_copy", tomlString(nameOf(kClickModes, c.clickToCopy)), "single | double"},
        {"hide_on_focus_loss", tomlBool(c.hideOnFocusLoss), ""},
        {"search_debounce_ms", std::to_string(c.searchDebounceMs), ""},
        {"backend", tomlString(nameOf(kBackends, c.backend)), "auto | gnome-ext | wl-paste | qt-clipboard"},
        {"hotkey", tomlString(c.hotkey.toStdString()), "global shortcut that opens the popup"},
        {"theme", tomlString(nameOf(kThemes, c.theme)), "system | light | dark"},
        {"popup_position", tomlString(nameOf(kPopupPositions, c.popupPosition)),
         "cursor | center | top-left | top-right | bottom-left | bottom-right | last"},
        {"encrypt_history", tomlBool(c.encryptHistory), "encrypt history.db; key kept in the system keyring"},
    };
}

constexpr std::size_t kCommentColumn = 32;

std::string render(const Line& line)
{
    std::string text = std::string(line.key) + " = " + line.value;
    if (*line.comment) {
        text.append(text.size() < kCommentColumn ? kCommentColumn - text.size() : 1, ' ');
        text += std::string("# ") + line.comment;
    }
    return text;
}

// End of the value that starts at `from`: a quoted string, or a bare token.
std::size_t valueEnd(const std::string& line, std::size_t from)
{
    if (from < line.size() && (line[from] == '"' || line[from] == '\'')) {
        const char quote = line[from];
        for (std::size_t i = from + 1; i < line.size(); ++i) {
            if (quote == '"' && line[i] == '\\') {
                ++i; // skip the escaped character
            } else if (line[i] == quote) {
                return i + 1;
            }
        }
        return line.size();
    }
    std::size_t i = from;
    while (i < line.size() && line[i] != '#' && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
        ++i;
    }
    return i;
}

} // namespace

std::string Config::toToml() const
{
    std::string out = "# ClipItTo configuration\n";
    for (const Line& line : lines(*this)) {
        out += render(line) + '\n';
    }
    return out;
}

std::string Config::mergeInto(std::string_view existing) const
{
    const std::vector<Line> wanted = lines(*this);
    std::vector<bool> seen(wanted.size(), false);
    const bool crlf = existing.find("\r\n") != std::string_view::npos;
    std::string out;
    // Notepad saves UTF-8 with a BOM: keep it, but don't let it glue onto the first key.
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    if (existing.substr(0, kBom.size()) == kBom) {
        out += kBom;
        existing.remove_prefix(kBom.size());
    }
    bool inTable = false; // keys after a [table] header are not ours
    std::size_t pos = 0;
    while (pos < existing.size()) {
        const std::size_t nl = existing.find('\n', pos);
        std::string line(existing.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? existing.size() : nl + 1;
        const bool hadNewline = nl != std::string_view::npos;

        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '[') {
            inTable = true;
        }
        const std::size_t eq = line.find('=');
        if (!inTable && first != std::string::npos && line[first] != '#' && eq != std::string::npos) {
            std::string key = line.substr(first, eq - first);
            key.erase(key.find_last_not_of(" \t") + 1);
            for (std::size_t i = 0; i < wanted.size(); ++i) {
                if (key == wanted[i].key) {
                    std::size_t start = line.find_first_not_of(" \t", eq + 1);
                    if (start == std::string::npos) {
                        start = line.size();
                    }
                    line.replace(start, valueEnd(line, start) - start, wanted[i].value);
                    seen[i] = true;
                }
            }
        }
        out += line;
        if (hadNewline) {
            out += '\n';
        }
    }
    bool appended = false;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (!seen[i]) {
            if (!appended && !out.empty() && out.back() != '\n') {
                out += crlf ? "\r\n" : "\n";
            }
            out += render(wanted[i]) + (crlf ? "\r\n" : "\n");
            appended = true;
        }
    }
    return out;
}

bool Config::save(const QString& path, QString* error) const
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("cannot create directory for %1").arg(path);
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    // Keep the user's file (comments, layout, unknown keys); only values change.
    std::string text;
    if (QFile existing(path); existing.exists() && existing.open(QIODevice::ReadOnly)) {
        const QByteArray old = existing.readAll();
        text = mergeInto(std::string_view(old.constData(), std::size_t(old.size())));
    } else {
        text = toToml(); // UTF-8, LF line endings on every platform
    }
    file.write(text.data(), qint64(text.size()));
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

} // namespace clipitto
