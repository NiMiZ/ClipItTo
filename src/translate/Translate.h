#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>

// Offline translation. Translation models ("language packs", one per direction: zh-en, en-zh, he-en, ...)
// are run on this computer by CTranslate2 with SentencePiece for the text pieces; nothing is sent
// anywhere, and ClipItTo itself never downloads a pack: the user picks a pack file (.argosmodel, the
// OPUS-MT based models of Argos Translate) in Settings.
namespace clipitto::translate {

struct Pack {
    QString id; // "zh-en"
    QString from; // "zh"
    QString to; // "en"
    QString fromName; // "Chinese"
    QString toName; // "English"
    QString path; // the folder of the installed pack

    QString label() const { return QStringLiteral("%1 → %2").arg(fromName, toName); }
};

// Whether this build has the translation engine (CTranslate2 + SentencePiece).
bool builtIn();

// Installed packs, and adding / removing them. The store is a folder (default: ClipItTo's data folder).
class PackStore {
public:
    explicit PackStore(QString root = defaultRoot());
    static QString defaultRoot();

    QList<Pack> installed() const;
    // Installs the pack in an .argosmodel file (a zip), replacing the same direction if it is there.
    // On failure `error` says why.
    bool install(const QString& file, QString* error = nullptr);
    bool remove(const QString& id);

private:
    QString m_root;
};

// Which script a language code is written in, and the dominant script of a text; used to offer the packs
// whose source language fits the text. Scripts: "latin", "han", "kana", "hangul", "hebrew", "cyrillic",
// "greek", "arabic", "devanagari", "" (no letters).
QString scriptOfLanguage(const QString& code);
QString scriptOfText(const QString& text);
// Whether `pack` translates from the language `text` is written in (Chinese pack for Chinese characters, ...).
bool fits(const Pack& pack, const QString& text);

// The text in sentences, kept apart from line breaks: each piece is {text, separator after it}. The model
// translates a sentence at a time; the separators are put back as they were.
struct Piece {
    QString text;
    QString after;
};
QList<Piece> splitSentences(const QString& text);

// Translates `text` with `pack` (blocking, a second or two the first time while the model loads; keep it off
// the GUI thread). nullopt with `error` when it can't, or when it takes longer than `timeoutSeconds` (1..10): the
// answer then comes back at once and the stuck work is left to end by itself.
std::optional<QString> translate(const Pack& pack, const QString& text, QString* error = nullptr, int timeoutSeconds = 5);

// The most characters one translation takes. The model decodes a sentence at a time (a second or two for twenty long
// ones on one thread), so a longer text can never finish inside the timeout, and its stuck work (it cannot be cancelled
// in the middle of a sentence) would keep every later translation waiting. Longer texts are refused at once.
constexpr qsizetype kMaxTranslateChars = 10000;

// Runs `work` (the engine) on a thread of its own so that a run that never ends cannot hold the caller; the
// caller gets its answer after `timeoutMs` and the stuck run is told to stop (`cancelled`), which it should check
// between sentences. One run at a time in this process: a run that was left behind keeps the next one waiting
// (up to `timeoutMs`), then it is refused with "Another translation is still running". `translate()` goes
// through this; it is public so that tests can drive the same plumbing with a fake engine.
using GuardedWork = std::function<std::optional<QString>(QString* error, const std::atomic<bool>& cancelled)>;
enum class GuardOutcome { Done, Busy, TimedOut, StartFailed };
std::optional<QString> runGuarded(GuardedWork work, QString* error, int timeoutMs, GuardOutcome* outcome = nullptr);
// Whether a run (possibly one that was given up on) is still going.
bool guardBusy();

} // namespace clipitto::translate
