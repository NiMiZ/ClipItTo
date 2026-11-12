#pragma once

#include "Translate.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// The translation quality check (clipitto-daemon --translate-check <word list>, wrapped by scripts/translate-check.sh):
// runs every line of a word list through the language packs the user has installed and writes
//   input -> output   (milliseconds, pack)
// so that words that translate badly or stall can be seen. It needs the real engine and real packs, so CI cannot run it;
// the engine and the packs are parameters so that the plumbing is tested with fakes.
namespace clipitto::translatecheck {

// The entries of a word list file (UTF-8): one per line; empty lines and lines starting with "#" are skipped, surrounding
// white space is cut off. nullopt with `error` when the file cannot be read.
std::optional<QStringList> readWordList(const QString& file, QString* error = nullptr);

struct Engine {
    std::function<QList<translate::Pack>()> packs; // null: the installed packs
    std::function<std::optional<QString>(const translate::Pack&, const QString& text, QString* error)> translate; // null: the real engine
    int slowMs = 3000; // taking longer than this is flagged SLOW
};

struct Summary {
    int total = 0;
    int translated = 0;
    int unchanged = 0; // the output equals the input (an unknown word often comes back as it is)
    int failed = 0; // an error, or no installed pack for the text's language
    int slow = 0;
};

// Writes the report to `out` (a callback per line, so a file or the console can be used) and returns the summary.
// `packId` restricts the check to one pack (empty: for each text, the first installed pack that fits its language).
Summary run(const QStringList& words, const QString& packId, const std::function<void(const QString& line)>& out,
            const Engine& engine = {});

// The whole command: reads `listFile`, runs it, writes the report to `outFile` (empty: the console). Returns the exit code
// (0: done, 2: no list, no packs or a pack missing; failed words do not make the exit code non-zero).
int runCommand(const QString& listFile, const QString& packId, const QString& outFile, const Engine& engine = {});

} // namespace clipitto::translatecheck
