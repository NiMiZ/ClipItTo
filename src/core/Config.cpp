#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <toml++/toml.hpp>

#include <array>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace clipitto {

namespace {

template <typename Enum, std::size_t N>
using EnumNames = std::array<std::pair<Enum, const char*>, N>;

constexpr EnumNames<PasteKeys, 2> kPasteKeys{{{PasteKeys::CtrlV, "ctrl+v"}, {PasteKeys::ShiftInsert, "shift+insert"}}};
constexpr EnumNames<ClickMode, 2> kClickModes{{{ClickMode::Single, "single"}, {ClickMode::Double, "double"}}};
constexpr EnumNames<BackendChoice, 4> kBackends{{{BackendChoice::Auto, "auto"},
                                                 {BackendChoice::GnomeExt, "gnome-ext"},
                                                 {BackendChoice::WlPaste, "wl-paste"},
                                                 {BackendChoice::X11, "x11"}}};

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
            allowed << QString::fromLatin1(entry.second);
        }
        warn(QStringLiteral("%1: expected one of %2").arg(QLatin1String(key), allowed.join(QLatin1String(", "))));
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
    r.boolean("show_saved_on_top", config.showSavedOnTop);
    r.integer("saved_cap", config.savedCap, 0, 500);
    r.integer("max_item_bytes", config.maxItemBytes, 1, 64LL * 1024 * 1024);
    r.boolean("capture_primary", config.capturePrimary);
    r.boolean("paste_on_enter", config.pasteOnEnter);
    r.choice("paste_keys", config.pasteKeys, kPasteKeys);
    r.choice("click_to_copy", config.clickToCopy, kClickModes);
    r.boolean("hide_on_focus_loss", config.hideOnFocusLoss);
    r.integer("search_debounce_ms", config.searchDebounceMs, 0, 2000);
    r.choice("backend", config.backend, kBackends);
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
    const QByteArray data = file.readAll();
    return fromToml(std::string_view(data.constData(), std::size_t(data.size())), warnings);
}

std::string Config::toToml() const
{
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::ostringstream out;
    out << "# ClipItTo configuration\n"
        << "max_history = " << maxHistory << "          # unsaved items kept; favorites/grouped never pruned\n"
        << "recent_count = " << recentCount << "\n"
        << "show_saved_on_top = " << b(showSavedOnTop) << "\n"
        << "saved_cap = " << savedCap << "\n"
        << "max_item_bytes = " << maxItemBytes << "\n"
        << "capture_primary = " << b(capturePrimary) << "\n"
        << "paste_on_enter = " << b(pasteOnEnter) << "\n"
        << "paste_keys = \"" << nameOf(kPasteKeys, pasteKeys) << "\"        # ctrl+v | shift+insert\n"
        << "click_to_copy = \"" << nameOf(kClickModes, clickToCopy) << "\"    # single | double\n"
        << "hide_on_focus_loss = " << b(hideOnFocusLoss) << "\n"
        << "search_debounce_ms = " << searchDebounceMs << "\n"
        << "backend = \"" << nameOf(kBackends, backend) << "\"            # auto | gnome-ext | wl-paste | x11\n";
    return out.str();
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
    const std::string text = toToml();
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
