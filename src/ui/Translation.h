#pragma once

#include <QList>
#include <QString>

#include <functional>

namespace clipitto::ui {

struct TranslationPackInfo {
    QString id; // "zh-en"
    QString label; // "Chinese → English"
};

// What the popup and Settings need from the offline translation engine (src/translate), handed over by the
// Daemon like the Windows Hello and text-recognition hooks. Everything runs on this computer.
struct TranslationSupport {
    bool builtIn = false; // the build has the engine; otherwise the menus don't mention translation
    // Installed language packs; with a text, only those that translate from the language it is written in.
    std::function<QList<TranslationPackInfo>(const QString& text)> packsFor;
    // Translates `text` with a pack (off the GUI thread); `done(translation, error)` runs on the GUI thread.
    std::function<void(const QString& packId, const QString& text,
                       std::function<void(const QString& translation, const QString& error)> done)>
        translate;
    // Whether the pack translates from the language `text` is written in.
    std::function<bool(const QString& packId, const QString& text)> fits;
    // Settings: add a pack from a file, or remove one.
    std::function<bool(const QString& file, QString* error)> install;
    std::function<bool(const QString& id)> remove;
};

} // namespace clipitto::ui
