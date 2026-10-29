// The translation engine: CTranslate2 (MIT) runs the model, SentencePiece (Apache-2.0) cuts the text into
// the pieces the model knows. Both run in this process on the CPU; neither opens a network connection.
#include "Translate.h"

#include <QCoreApplication>
#include <QDir>
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
        config.num_threads_per_replica = qBound(1, QThread::idealThreadCount() / 2, 4);
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

} // namespace

bool builtIn()
{
    return true;
}

std::optional<QString> translate(const Pack& pack, const QString& text, QString* error)
{
    QMutexLocker lock(&g_mutex);
    if (!load(pack, error)) {
        return std::nullopt;
    }
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
        if (!batch.empty()) {
            ctranslate2::TranslationOptions options;
            options.beam_size = 4;
            options.max_decoding_length = 512;
            const auto results = g_loaded.translator->translate_batch(batch, options);
            for (std::size_t k = 0; k < results.size(); ++k) {
                translated[where[k]] = joinPieces(results[k].output());
            }
        }
        QString out;
        for (int i = 0; i < pieces.size(); ++i) {
            out += translated[i].isEmpty() && pieces[i].text.trimmed().isEmpty() ? pieces[i].text : translated[i];
            out += pieces[i].after;
        }
        return out.trimmed();
    } catch (const std::exception& e) {
        if (error) {
            *error = tr("Translation failed: %1").arg(QString::fromUtf8(e.what()));
        }
        return std::nullopt;
    }
}

} // namespace clipitto::translate
