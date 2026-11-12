#include "TranslateCheck.h"

#include <QElapsedTimer>
#include <QFile>

#include <cstdio>

namespace clipitto::translatecheck {

std::optional<QStringList> readWordList(const QString& file, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot read %1").arg(file);
        }
        return std::nullopt;
    }
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1); // a byte-order mark from Windows editors
    }
    QStringList words;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed(); // also drops a trailing \r
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            words << line;
        }
    }
    return words;
}

Summary run(const QStringList& words, const QString& packId, const std::function<void(const QString&)>& out, const Engine& engine)
{
    const QList<translate::Pack> packs = engine.packs ? engine.packs() : translate::PackStore().installed();
    Summary sum;
    for (const QString& word : words) {
        ++sum.total;
        const translate::Pack* chosen = nullptr;
        for (const translate::Pack& p : packs) {
            if ((packId.isEmpty() || p.id == packId) && translate::fits(p, word)) {
                chosen = &p;
                break;
            }
        }
        if (!chosen) {
            ++sum.failed;
            out(QStringLiteral("%1 -> NO PACK for this text's language (script: %2)").arg(word, translate::scriptOfText(word)));
            continue;
        }
        QString error;
        QElapsedTimer timer;
        timer.start();
        const std::optional<QString> result = engine.translate ? engine.translate(*chosen, word, &error)
                                                                : translate::translate(*chosen, word, &error, 10);
        const qint64 ms = timer.elapsed();
        const QString flag = ms > engine.slowMs ? QStringLiteral(" SLOW") : QString();
        if (ms > engine.slowMs) {
            ++sum.slow;
        }
        if (!result) {
            ++sum.failed;
            out(QStringLiteral("%1 -> ERROR: %2 (%3 ms, %4)%5").arg(word, error, QString::number(ms), chosen->id, flag));
            continue;
        }
        ++sum.translated;
        QString note;
        if (result->trimmed() == word.trimmed()) {
            ++sum.unchanged;
            note = QStringLiteral(" UNCHANGED");
        }
        // One line each: line breaks inside a sentence are shown as "\n".
        QString shown = *result;
        shown.replace(QStringLiteral("\r"), QString()).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        out(QStringLiteral("%1 -> %2 (%3 ms, %4)%5%6").arg(word, shown, QString::number(ms), chosen->id, note, flag));
    }
    out(QStringLiteral("# %1 checked: %2 translated (%3 unchanged), %4 failed, %5 slow (over %6 ms)")
            .arg(sum.total).arg(sum.translated).arg(sum.unchanged).arg(sum.failed).arg(sum.slow).arg(engine.slowMs));
    return sum;
}

int runCommand(const QString& listFile, const QString& packId, const QString& outFile, const Engine& engine)
{
    QString error;
    const auto words = readWordList(listFile, &error);
    if (!words) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    const QList<translate::Pack> packs = engine.packs ? engine.packs() : translate::PackStore().installed();
    if (packs.isEmpty()) {
        std::fprintf(stderr, "No language pack is installed (Settings > Translation > Add language pack).\n");
        return 2;
    }
    if (!packId.isEmpty()) {
        bool found = false;
        for (const translate::Pack& p : packs) {
            found = found || p.id == packId;
        }
        if (!found) {
            std::fprintf(stderr, "The pack %s is not installed.\n", qPrintable(packId));
            return 2;
        }
    }
    QFile file(outFile);
    if (!outFile.isEmpty() && !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(outFile));
        return 2;
    }
    run(*words, packId, [&file, &outFile](const QString& line) {
        if (outFile.isEmpty()) {
            std::fprintf(stdout, "%s\n", line.toUtf8().constData());
            std::fflush(stdout); // a stalled word must not hide the lines before it
        } else {
            file.write(line.toUtf8() + '\n');
            file.flush();
        }
    }, engine);
    return 0;
}

} // namespace clipitto::translatecheck
