#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace clipitto {

// The keys ClipItTo types to paste. Config's `paste_keys` offers CtrlV and ShiftInsert; CtrlShiftV is what
// most terminals want and is chosen per app (`paste_keys_per_app`, roadmap 8).
enum class PasteKeys { CtrlV, ShiftInsert, CtrlShiftV };

// Which program will receive the paste, as far as the platform can tell. Names only: never a window title.
//   Unsupported  this session cannot tell (Wayland without the GNOME extension, macOS, builds without X11)
//   Unknown      it can, but there was no usable window (nothing remembered, our own window, a vanished window)
//   Known        ids lists what identifies the program: executable name, window class, app id, WM_CLASS ...
struct PasteTarget {
    enum class State { Unsupported, Unknown, Known };

    State state = State::Unsupported;
    QStringList ids;

    static PasteTarget unsupported() { return {}; }
    static PasteTarget unknown() { return {State::Unknown, {}}; }
    static PasteTarget known(const QStringList& ids) { return {State::Known, ids}; }

    bool operator==(const PasteTarget&) const = default;
};

// "app-pattern = chord": the keys to type when the paste goes to a program the pattern names.
struct PasteKeyRule {
    QString pattern;
    PasteKeys keys = PasteKeys::CtrlShiftV;

    bool operator==(const PasteKeyRule&) const = default;
};

namespace pastekeys {

constexpr int kMaxRules = 16;
constexpr int kMaxPatternLength = 64;

// "ctrl+v", "shift+insert", "ctrl+shift+v" (the only chords there are).
QString chordName(PasteKeys keys);
// Case and spaces around the plus signs are ignored.
bool parseChord(const QString& text, PasteKeys& out);
QStringList chordNames();

// Why a pattern cannot be used, empty when it is fine. A pattern names an executable ("WindowsTerminal.exe" or
// "windowsterminal"), a window class / WM_CLASS ("XTerm") or an app id ("org.gnome.Terminal"); letter case is
// ignored, a trailing ".exe" is ignored on both sides, and "*" / "?" match any run / one character.
QString validatePattern(const QString& pattern);
QString validate(const PasteKeyRule& rule);

// Config text form "pattern = chord" (the pattern has no '=' and no quote). parse() leaves `out` alone and returns
// false for anything malformed or invalid.
QString format(const PasteKeyRule& rule);
bool parse(const QString& text, PasteKeyRule& out);

// Does the pattern name one of the ids?
bool matches(const QString& pattern, const QStringList& ids);

// Terminals ClipItTo knows (used after the user's own rules, while paste_keys_per_app is on).
const QList<PasteKeyRule>& builtInRules();

struct Choice {
    PasteKeys keys = PasteKeys::CtrlV;
    QString rule; // the pattern that decided, "" when the global setting did; for the debug log only
    bool builtIn = false;
};

// The keys for a paste: with `perApp` off or no known target, `global`. Otherwise the first user rule that
// matches the target, then the first built-in one, then `global`.
Choice choose(PasteKeys global, bool perApp, const QList<PasteKeyRule>& userRules, const PasteTarget& target);

} // namespace pastekeys
} // namespace clipitto
