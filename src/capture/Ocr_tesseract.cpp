// Text recognition with Tesseract (Apache-2.0) and Leptonica (BSD-2-Clause), both used as shared
// libraries, offline. Languages: the traineddata files that are installed, restricted to English
// plus the language of the user's locale, so a long list of languages doesn't slow every read.
// A folder ~/.local/share/clipitto/tessdata with *.traineddata files, when it exists, is used
// instead of the system's (how an AppImage or snap user adds a language).
#include "Ocr.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QStandardPaths>

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <memory>

namespace clipitto::ocr {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::ocr", text);
}

// Tesseract's language code for the user's locale ("he_IL" -> "heb"); empty when unknown.
QString localeLanguage()
{
    static const struct {
        const char* iso;
        const char* tess;
    } map[] = {{"he", "heb"}, {"ar", "ara"}, {"de", "deu"}, {"fr", "fra"}, {"es", "spa"}, {"it", "ita"},
               {"pt", "por"}, {"ru", "rus"}, {"ja", "jpn"}, {"ko", "kor"}, {"nl", "nld"}, {"pl", "pol"},
               {"tr", "tur"}, {"uk", "ukr"}, {"sv", "swe"}, {"cs", "ces"}, {"el", "ell"}, {"hi", "hin"}};
    const QString iso = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
    for (const auto& m : map) {
        if (iso == QLatin1String(m.iso)) {
            return QString::fromLatin1(m.tess);
        }
    }
    if (iso == QLatin1String("zh")) {
        return QStringLiteral("chi_sim");
    }
    return {};
}

// The user's own tessdata folder, when it holds traineddata; else empty (Tesseract's default).
QByteArray userDataPath()
{
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tessdata"));
    if (!dir.entryList({QStringLiteral("*.traineddata")}, QDir::Files).isEmpty()) {
        return dir.absolutePath().toUtf8();
    }
    return {};
}

struct Engine {
    tesseract::TessBaseAPI api;
    bool ready = false;
    QString problem;

    // Tries "<locale language>+eng", then either alone, and keeps the first the installed data allows.
    Engine()
    {
        const QByteArray dataPath = userDataPath();
        const char* datapath = dataPath.isEmpty() ? nullptr : dataPath.constData();
        const QString local = localeLanguage();
        QStringList candidates;
        if (!local.isEmpty()) {
            candidates << local + QStringLiteral("+eng") << local;
        }
        candidates << QStringLiteral("eng");
        for (const QString& languages : candidates) {
            const QByteArray code = languages.toLatin1();
            if (api.Init(datapath, code.constData()) == 0) {
                ready = true;
                return;
            }
            api.End();
        }
        problem = tr("No Tesseract language data found. Install a package such as tesseract-ocr-eng, or put "
                     "*.traineddata files in ~/.local/share/clipitto/tessdata.");
    }
};

} // namespace

bool available(QString* why)
{
    Engine engine;
    if (!engine.ready && why) {
        *why = engine.problem;
    }
    return engine.ready;
}

std::optional<QString> recognize(const QByteArray& png, QString* error)
{
    Engine engine;
    if (!engine.ready) {
        if (error) {
            *error = engine.problem;
        }
        return std::nullopt;
    }
    Pix* pix = pixReadMem(reinterpret_cast<const l_uint8*>(png.constData()), size_t(png.size()));
    if (!pix) {
        if (error) {
            *error = tr("The picture could not be read.");
        }
        return std::nullopt;
    }
    engine.api.SetImage(pix);
    std::unique_ptr<char[]> text(engine.api.GetUTF8Text());
    pixDestroy(&pix);
    if (!text) {
        if (error) {
            *error = tr("Text recognition failed.");
        }
        return std::nullopt;
    }
    return QString::fromUtf8(text.get()).trimmed();
}

} // namespace clipitto::ocr
