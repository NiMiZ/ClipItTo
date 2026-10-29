#pragma once

#include <QList>
#include <QString>
#include <QStringList>

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
// the GUI thread). nullopt with `error` when it can't.
std::optional<QString> translate(const Pack& pack, const QString& text, QString* error = nullptr);

} // namespace clipitto::translate
