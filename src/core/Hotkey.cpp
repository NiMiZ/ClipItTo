#include "Hotkey.h"

#include <QStringList>

namespace clipitto {

namespace {

struct NamedKey {
    const char* name; // canonical spelling
    quint32 vk; // Win32 virtual-key code
    const char* gtk; // GDK key name for GNOME accelerators
    quint32 mac; // macOS kVK_* virtual key code
};

constexpr NamedKey kNamedKeys[] = {
    {"Space", 0x20, "space", 0x31},   {"Tab", 0x09, "Tab", 0x30},        {"Enter", 0x0D, "Return", 0x24},
    {"Insert", 0x2D, "Insert", 0x72}, {"Delete", 0x2E, "Delete", 0x75},  {"Home", 0x24, "Home", 0x73},
    {"End", 0x23, "End", 0x77},       {"PageUp", 0x21, "Page_Up", 0x74}, {"PageDown", 0x22, "Page_Down", 0x79},
};

// Punctuation keys by their US-layout character (VK_OEM_* codes).
constexpr NamedKey kPunctuation[] = {
    {"`", 0xC0, "grave", 0x32},       {"-", 0xBD, "minus", 0x1B},        {"=", 0xBB, "equal", 0x18},
    {"[", 0xDB, "bracketleft", 0x21}, {"]", 0xDD, "bracketright", 0x1E}, {"\\", 0xDC, "backslash", 0x2A},
    {";", 0xBA, "semicolon", 0x29},   {"'", 0xDE, "apostrophe", 0x27},   {",", 0xBC, "comma", 0x2B},
    {".", 0xBE, "period", 0x2F},      {"/", 0xBF, "slash", 0x2C},
};

// kVK_ANSI_A..Z and 0..9 are positions on the keyboard, not alphabetical.
constexpr quint32 kMacLetters[26] = {0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                     0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06};
constexpr quint32 kMacDigits[10] = {0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};
// kVK_F1..F20; a Mac keyboard has no F21..F24.
constexpr quint32 kMacFunction[20] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D,
                                      0x67, 0x6F, 0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A};

constexpr quint32 kCarbonCmd = 0x0100;
constexpr quint32 kCarbonShift = 0x0200;
constexpr quint32 kCarbonOption = 0x0800;
constexpr quint32 kCarbonControl = 0x1000;

#ifdef Q_OS_MACOS
constexpr const char* kWinKeyName = "Cmd";
#else
constexpr const char* kWinKeyName = "Win";
#endif

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
        } else if (mod == QLatin1String("alt") || mod == QLatin1String("option") || mod == QLatin1String("opt")) {
            flag = win32::ModAlt;
        } else if (mod == QLatin1String("shift")) {
            flag = win32::ModShift;
        } else if (mod == QLatin1String("win") || mod == QLatin1String("super") || mod == QLatin1String("meta")
                   || mod == QLatin1String("cmd") || mod == QLatin1String("command")) {
            flag = win32::ModWin;
        }
        if (!flag || (hotkey.modifiers & flag)) {
            return std::nullopt; // unknown or repeated modifier
        }
        hotkey.modifiers |= flag;
    }
    // Canonical order, independent of how it was written (on a Mac that is Apple's own: ⌃⌥⇧⌘).
    for (const auto& [flag, name] : {std::pair{win32::ModControl, "Ctrl"}, std::pair{win32::ModAlt, "Alt"},
                                     std::pair{win32::ModShift, "Shift"}, std::pair{win32::ModWin, kWinKeyName}}) {
        if (hotkey.modifiers & flag) {
            canonicalModifiers << QString::fromLatin1(name);
        }
    }

    const QString key = parts.last().trimmed();
    QString keyName;
    QString gtkKey;
    if (key.size() == 1 && key.at(0).isLetterOrNumber() && key.at(0).unicode() < 128) {
        const QChar upper = key.at(0).toUpper();
        hotkey.virtualKey = upper.unicode(); // VK_A..VK_Z / VK_0..VK_9 equal their ASCII codes
        hotkey.macKeyCode = upper.isDigit() ? kMacDigits[upper.unicode() - '0'] : kMacLetters[upper.unicode() - 'A'];
        keyName = upper;
        gtkKey = upper.toLower();
    } else if (key.compare(QLatin1String("grave"), Qt::CaseInsensitive) == 0
               || key.compare(QLatin1String("backtick"), Qt::CaseInsensitive) == 0) {
        hotkey.virtualKey = 0xC0;
        hotkey.macKeyCode = 0x32;
        keyName = QStringLiteral("`");
        gtkKey = QStringLiteral("grave");
    } else if (key.size() == 1 && key.at(0).unicode() < 128 && !key.at(0).isLetterOrNumber()) {
        for (const NamedKey& named : kPunctuation) {
            if (key == QLatin1String(named.name)) {
                hotkey.virtualKey = named.vk;
                hotkey.macKeyCode = named.mac;
                keyName = key;
                gtkKey = QString::fromLatin1(named.gtk);
            }
        }
        if (!hotkey.virtualKey) {
            return std::nullopt;
        }
    } else if (key.size() >= 2 && key.at(0).toUpper() == QLatin1Char('F')) {
        bool ok = false;
        const int n = key.mid(1).toInt(&ok);
        if (!ok || n < 1 || n > 24) {
            return std::nullopt;
        }
        hotkey.virtualKey = 0x70 + quint32(n - 1); // VK_F1..VK_F24
        hotkey.macKeyCode = n <= 20 ? kMacFunction[n - 1] : NativeHotkey::kNoMacKey;
        keyName = QStringLiteral("F%1").arg(n);
        gtkKey = keyName;
    } else {
        for (const NamedKey& named : kNamedKeys) {
            if (key.compare(QLatin1String(named.name), Qt::CaseInsensitive) == 0) {
                hotkey.virtualKey = named.vk;
                hotkey.macKeyCode = named.mac;
                keyName = QString::fromLatin1(named.name);
                gtkKey = QString::fromLatin1(named.gtk);
            }
        }
        if (!hotkey.virtualKey) {
            return std::nullopt;
        }
    }

    for (const auto& [flag, gtk] : {std::pair{win32::ModControl, "<Control>"}, std::pair{win32::ModAlt, "<Alt>"},
                                    std::pair{win32::ModShift, "<Shift>"}, std::pair{win32::ModWin, "<Super>"}}) {
        if (hotkey.modifiers & flag) {
            hotkey.gtkAccelerator += QString::fromLatin1(gtk);
        }
    }
    hotkey.gtkAccelerator += gtkKey;
    for (const auto& [flag, carbon] : {std::pair{win32::ModControl, kCarbonControl}, std::pair{win32::ModAlt, kCarbonOption},
                                       std::pair{win32::ModShift, kCarbonShift}, std::pair{win32::ModWin, kCarbonCmd}}) {
        if (hotkey.modifiers & flag) {
            hotkey.macModifiers |= carbon;
        }
    }
    hotkey.canonical = (canonicalModifiers << keyName).join(QLatin1Char('+'));
    return hotkey;
}

} // namespace clipitto
