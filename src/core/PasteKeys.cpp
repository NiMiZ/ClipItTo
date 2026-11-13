#include "PasteKeys.h"

#include <QRegularExpression>

#include <initializer_list>

namespace clipitto::pastekeys {

namespace {

QString normalized(const QString& text)
{
    QString s = text.trimmed().toLower();
    if (s.endsWith(QLatin1String(".exe"))) {
        s.chop(4);
    }
    return s;
}

struct ChordName {
    PasteKeys keys;
    const char* name;
};
constexpr ChordName kChords[] = {
    {PasteKeys::CtrlV, "ctrl+v"},
    {PasteKeys::ShiftInsert, "shift+insert"},
    {PasteKeys::CtrlShiftV, "ctrl+shift+v"},
};

} // namespace

QString chordName(PasteKeys keys)
{
    for (const ChordName& chord : kChords) {
        if (chord.keys == keys) {
            return QString::fromLatin1(chord.name);
        }
    }
    return QStringLiteral("ctrl+v");
}

bool parseChord(const QString& text, PasteKeys& out)
{
    QString s = text.toLower();
    s.remove(QLatin1Char(' '));
    for (const ChordName& chord : kChords) {
        if (s == QLatin1String(chord.name)) {
            out = chord.keys;
            return true;
        }
    }
    return false;
}

QStringList chordNames()
{
    QStringList names;
    for (const ChordName& chord : kChords) {
        names << QString::fromLatin1(chord.name);
    }
    return names;
}

QString validatePattern(const QString& pattern)
{
    QString p = pattern.trimmed();
    if (p.isEmpty()) {
        return QStringLiteral("the program name is empty");
    }
    if (p.size() > kMaxPatternLength) {
        return QStringLiteral("the program name is longer than %1 characters").arg(kMaxPatternLength);
    }
    for (const QChar c : p) {
        if (c == QLatin1Char('=') || c == QLatin1Char('"') || c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return QStringLiteral("the program name has a character that is not allowed (= or a quote)");
        }
    }
    if (p.count(QLatin1Char('*')) + p.count(QLatin1Char('?')) > 3) {
        return QStringLiteral("the program name has more than 3 wildcards (* or ?)");
    }
    if (p.remove(QLatin1Char('*')).remove(QLatin1Char('?')).trimmed().isEmpty()) {
        return QStringLiteral("the program name must have a letter or digit besides * and ?");
    }
    return {};
}

QString validate(const PasteKeyRule& rule)
{
    return validatePattern(rule.pattern);
}

QString format(const PasteKeyRule& rule)
{
    return rule.pattern.trimmed() + QStringLiteral(" = ") + chordName(rule.keys);
}

bool parse(const QString& text, PasteKeyRule& out)
{
    const qsizetype eq = text.lastIndexOf(QLatin1Char('='));
    if (eq < 0) {
        return false;
    }
    PasteKeyRule rule;
    rule.pattern = text.left(eq).trimmed();
    if (!parseChord(text.mid(eq + 1).trimmed(), rule.keys) || !validate(rule).isEmpty()) {
        return false;
    }
    out = rule;
    return true;
}

bool matches(const QString& pattern, const QStringList& ids)
{
    const QString p = normalized(pattern);
    if (p.isEmpty()) {
        return false;
    }
    const bool wild = p.contains(QLatin1Char('*')) || p.contains(QLatin1Char('?'));
    const QRegularExpression re = wild ? QRegularExpression(QRegularExpression::wildcardToRegularExpression(p))
                                       : QRegularExpression();
    for (const QString& id : ids) {
        const QString n = normalized(id);
        if (wild ? re.match(n).hasMatch() : n == p) {
            return true;
        }
    }
    return false;
}

const QList<PasteKeyRule>& builtInRules()
{
    // Windows consoles take Shift+Insert whatever their settings; the others take Ctrl+Shift+V by default.
    // Names: Windows executables and window classes, X11 WM_CLASS (instance and class) and Wayland app ids.
    static const QList<PasteKeyRule> rules = [] {
        QList<PasteKeyRule> list;
        const auto add = [&list](PasteKeys keys, std::initializer_list<const char*> names) {
            for (const char* name : names) {
                list.append({QString::fromLatin1(name), keys});
            }
        };
        add(PasteKeys::ShiftInsert,
            {"ConsoleWindowClass", "conhost", "openconsole", "cmd", "powershell", "pwsh", "mintty"});
        add(PasteKeys::CtrlShiftV,
            {"WindowsTerminal", "wt", "CASCADIA_HOSTING_WINDOW_CLASS", "alacritty", "kitty", "wezterm", "wezterm-gui",
             "org.wezfurlong.wezterm", "gnome-terminal", "gnome-terminal-server", "org.gnome.Terminal", "konsole",
             "org.kde.konsole", "xterm", "uxterm", "tilix", "com.gexperts.Tilix", "terminator", "foot", "footclient",
             "ghostty", "com.mitchellh.ghostty", "xfce4-terminal", "mate-terminal", "lxterminal", "qterminal",
             "terminology", "sakura", "guake", "yakuake", "tabby", "st", "st-256color", "urxvt", "rxvt",
             "org.gnome.Console", "kgx", "ptyxis", "org.gnome.Ptyxis", "contour"});
        return list;
    }();
    return rules;
}

Choice choose(PasteKeys global, bool perApp, const QList<PasteKeyRule>& userRules, const PasteTarget& target)
{
    Choice choice;
    choice.keys = global;
    if (!perApp || target.state != PasteTarget::State::Known) {
        return choice;
    }
    for (const PasteKeyRule& rule : userRules) {
        if (matches(rule.pattern, target.ids)) {
            return {rule.keys, rule.pattern, false};
        }
    }
    for (const PasteKeyRule& rule : builtInRules()) {
        if (matches(rule.pattern, target.ids)) {
            return {rule.keys, rule.pattern, true};
        }
    }
    return choice;
}

} // namespace clipitto::pastekeys
