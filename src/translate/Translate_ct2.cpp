// The translation engine: CTranslate2 (MIT) runs the model, SentencePiece (Apache-2.0) cuts the text into
// the pieces the model knows. Both run in this process on the CPU; neither opens a network connection.
#include "Translate.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QElapsedTimer>
#include <QDateTime>
#include <algorithm>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <memory>
#include <string>
#include <vector>

namespace clipitto::translate {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::translate", text);
}

// A small log of what the engine is doing, for finding why a translation "gets stuck": the time, the pack, how many pieces
// and sentences, how long it took. Never the text (the log must not hold clipboard contents).
// <data folder>/translation/translation.log, cut back when it grows past 64 KiB.
void logLine(const QString& line)
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/translation/translation.log");
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (QFileInfo(path).size() > 64 * 1024) {
        QFile::remove(path);
    }
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + QLatin1Char(' ') + line + QLatin1Char('\n')).toUtf8());
    }
}

// One thread: a short text gains nothing from more (20 long sentences: 2.9 s on one thread, 1.4 s on four), and a
// freeze on Windows turned up that more threads could explain. CLIPITTO_TRANSLATE_THREADS=N overrides it.
int translationThreads()
{
    bool ok = false;
    const int fromEnvironment = qEnvironmentVariable("CLIPITTO_TRANSLATE_THREADS").toInt(&ok);
    return ok ? qBound(1, fromEnvironment, 16) : 1;
}

struct Loaded {
    QString packId;
    std::unique_ptr<sentencepiece::SentencePieceProcessor> sp;
    std::unique_ptr<ctranslate2::Translator> translator;
};

QMutex g_mutex; // one translation at a time; the loaded pack is kept for the next one
Loaded g_loaded;

bool load(const Pack& pack, QString* error)
{
    if (g_loaded.packId == pack.id && g_loaded.translator) {
        return true;
    }
    g_loaded = {};
    auto sp = std::make_unique<sentencepiece::SentencePieceProcessor>();
    const std::string spPath = QDir::toNativeSeparators(pack.path + QStringLiteral("/sentencepiece.model")).toStdString();
    if (!sp->Load(spPath).ok()) {
        if (error) {
            *error = tr("The language pack's text model could not be read.");
        }
        return false;
    }
    try {
        ctranslate2::models::ModelLoader loader(QDir::toNativeSeparators(pack.path + QStringLiteral("/model")).toStdString());
        loader.device = ctranslate2::Device::CPU;
        loader.compute_type = ctranslate2::ComputeType::DEFAULT;
        ctranslate2::ReplicaPoolConfig config;
        config.num_threads_per_replica = translationThreads();
        g_loaded.translator = std::make_unique<ctranslate2::Translator>(loader, config);
    } catch (const std::exception& e) {
        if (error) {
            *error = tr("The translation model could not be loaded: %1").arg(QString::fromUtf8(e.what()));
        }
        return false;
    }
    g_loaded.sp = std::move(sp);
    g_loaded.packId = pack.id;
    return true;
}

// The model's output pieces back into text: "▁" marks the start of a word.
QString joinPieces(const std::vector<std::string>& pieces)
{
    QString out;
    for (const std::string& p : pieces) {
        out += QString::fromStdString(p);
    }
    out.replace(QChar(0x2581), QLatin1Char(' '));
    return out.simplified();
}

// Does the text say the same word twice in a row ("שלום שלום", "peace Peace.")? Case and end punctuation don't count.
bool repeatsAWord(const QString& text)
{
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (qsizetype i = 1; i < words.size(); ++i) {
        const auto strip = [](QString w) {
            while (!w.isEmpty() && (w.back().isPunct() || w.back().isSpace())) {
                w.chop(1);
            }
            return w.toCaseFolded();
        };
        const QString a = strip(words[i - 1]);
        if (!a.isEmpty() && a == strip(words[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

bool builtIn()
{
    return true;
}

std::optional<QString> translate(const Pack& pack, const QString& text, QString* error)
{
    // A translation still running (or stuck) must not make the next one wait for ever.
    if (!g_mutex.tryLock(5000)) {
        logLine(QStringLiteral("busy: another translation is still running"));
        if (error) {
            *error = tr("Another translation is still running. Try again in a moment.");
        }
        return std::nullopt;
    }
    struct Unlock {
        ~Unlock() { g_mutex.unlock(); }
    } unlock; // held by tryLock above; let go on every way out
    QElapsedTimer timer;
    timer.start();
    if (!load(pack, error)) {
        logLine(QStringLiteral("load failed %1").arg(pack.id));
        return std::nullopt;
    }
    logLine(QStringLiteral("start %1 text=%2 chars, model ready after %3 ms, %4 thread(s)")
                .arg(pack.id).arg(text.size()).arg(timer.elapsed()).arg(translationThreads()));
    try {
        const QList<Piece> pieces = splitSentences(text);
        std::vector<std::vector<std::string>> batch;
        std::vector<int> where; // index of the piece each batch entry came from
        for (int i = 0; i < pieces.size(); ++i) {
            if (pieces[i].text.trimmed().isEmpty()) {
                continue;
            }
            std::vector<std::string> tokens;
            g_loaded.sp->Encode(pieces[i].text.toStdString(), &tokens);
            batch.push_back(std::move(tokens));
            where.push_back(i);
        }
        QStringList translated(pieces.size());
        // Sentences go through together, bounded by their length: a short text such as "pc enthusiast" must not
        // decode up to 512 pieces when the model starts repeating itself.
        // One to three pieces (a word or two) are different: the model was trained on sentences and, given "hello",
        // answers "שלום שלום" (and "שלום" comes back as "Peaceful peace."). For those the best few candidates are
        // looked at and the first that doesn't say the same word twice in a row is taken.
        constexpr std::size_t kShort = 3;
        std::vector<std::size_t> longIdx;
        std::vector<std::size_t> shortIdx;
        for (std::size_t k = 0; k < batch.size(); ++k) {
            (batch[k].size() <= kShort ? shortIdx : longIdx).push_back(k);
        }
        const auto take = [&batch](const std::vector<std::size_t>& idx) {
            std::vector<std::vector<std::string>> part;
            for (const std::size_t k : idx) {
                part.push_back(batch[k]);
            }
            return part;
        };
        const auto longest = [](const std::vector<std::vector<std::string>>& part) {
            std::size_t n = 0;
            for (const auto& tokens : part) {
                n = std::max(n, tokens.size());
            }
            return n;
        };
        if (!longIdx.empty()) {
            const auto part = take(longIdx);
            ctranslate2::TranslationOptions options;
            options.beam_size = 4;
            options.max_decoding_length = std::min<std::size_t>(512, longest(part) * 2 + 10);
            options.repetition_penalty = 1.2F;
            options.no_repeat_ngram_size = 3;
            const auto results = g_loaded.translator->translate_batch(part, options);
            for (std::size_t r = 0; r < results.size(); ++r) {
                translated[where[longIdx[r]]] = joinPieces(results[r].output());
            }
        }
        if (!shortIdx.empty()) {
            const auto part = take(shortIdx);
            ctranslate2::TranslationOptions options;
            options.beam_size = 6;
            options.num_hypotheses = 6;
            options.max_decoding_length = longest(part) * 3 + 4;
            options.repetition_penalty = 1.2F;
            const auto results = g_loaded.translator->translate_batch(part, options);
            for (std::size_t r = 0; r < results.size(); ++r) {
                QString chosen;
                for (const auto& hypothesis : results[r].hypotheses) {
                    const QString candidate = joinPieces(hypothesis);
                    if (chosen.isEmpty()) {
                        chosen = candidate; // the best one, if all repeat
                    }
                    if (!repeatsAWord(candidate)) {
                        chosen = candidate;
                        break;
                    }
                }
                translated[where[shortIdx[r]]] = chosen;
            }
        }
        QString out;
        for (int i = 0; i < pieces.size(); ++i) {
            out += translated[i].isEmpty() && pieces[i].text.trimmed().isEmpty() ? pieces[i].text : translated[i];
            out += pieces[i].after;
        }
        logLine(QStringLiteral("done in %1 ms").arg(timer.elapsed()));
        return out.trimmed();
    } catch (const std::exception& e) {
        logLine(QStringLiteral("failed after %1 ms: %2").arg(timer.elapsed()).arg(QString::fromUtf8(e.what())));
        if (error) {
            *error = tr("Translation failed: %1").arg(QString::fromUtf8(e.what()));
        }
        return std::nullopt;
    }
}

} // namespace clipitto::translate
