#include "Hotkey.h"

#include <QStringList>

namespace clipitto {

namespace {

struct NamedKey {
    const char* name;
    quint32 vk;
};

constexpr NamedKey kNamedKeys[] = {
    {"Space", 0x20}, {"Tab", 0x09},      {"Enter", 0x0D},  {"Insert", 0x2D},   {"Delete", 0x2E},
    {"Home", 0x24},  {"End", 0x23},      {"PageUp", 0x21}, {"PageDown", 0x22},
};

} // namespace

std::optional<NativeHotkey> parseHotkey(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('+'));
    if (parts.size() < 2) {
        return std::nullopt; // a bare key would swallow normal typing
    }

    NativeHotkey hotkey;
    QStringList canonicalModifiers;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString mod = parts[i].trimmed().toLower();
        quint32 flag = 0;
        if (mod == QLatin1String("ctrl") || mod == QLatin1String("control")) {
            flag = win32::ModControl;
        } else if (mod == QLatin1String("alt")) {
            flag = win32::ModAlt;
        } else if (mod == QLatin1String("shift")) {
            flag = win32::ModShift;
        } else if (mod == QLatin1String("win") || mod == QLatin1String("super") || mod == QLatin1String("meta")) {
            flag = win32::ModWin;
        }
        if (!flag || (hotkey.modifiers & flag)) {
            return std::nullopt; // unknown or repeated modifier
        }
        hotkey.modifiers |= flag;
    }
    // Canonical order, independent of how it was written.
    for (const auto& [flag, name] : {std::pair{win32::ModControl, "Ctrl"}, std::pair{win32::ModAlt, "Alt"},
                                     std::pair{win32::ModShift, "Shift"}, std::pair{win32::ModWin, "Win"}}) {
        if (hotkey.modifiers & flag) {
            canonicalModifiers << QString::fromLatin1(name);
        }
    }

    const QString key = parts.last().trimmed();
    QString keyName;
    if (key.size() == 1 && key.at(0).isLetterOrNumber() && key.at(0).unicode() < 128) {
        const QChar upper = key.at(0).toUpper();
        hotkey.virtualKey = upper.unicode(); // VK_A..VK_Z / VK_0..VK_9 equal their ASCII codes
        keyName = upper;
    } else if (key.size() >= 2 && key.at(0).toUpper() == QLatin1Char('F')) {
        bool ok = false;
        const int n = key.mid(1).toInt(&ok);
        if (!ok || n < 1 || n > 24) {
            return std::nullopt;
        }
        hotkey.virtualKey = 0x70 + quint32(n - 1); // VK_F1..VK_F24
        keyName = QStringLiteral("F%1").arg(n);
    } else {
        for (const NamedKey& named : kNamedKeys) {
            if (key.compare(QLatin1String(named.name), Qt::CaseInsensitive) == 0) {
                hotkey.virtualKey = named.vk;
                keyName = QString::fromLatin1(named.name);
            }
        }
        if (!hotkey.virtualKey) {
            return std::nullopt;
        }
    }

    hotkey.canonical = (canonicalModifiers << keyName).join(QLatin1Char('+'));
    return hotkey;
}

} // namespace clipitto
