// Text recognition with Tesseract (Apache-2.0) and Leptonica (BSD-2-Clause), both used as shared
// libraries, offline. Languages: the traineddata files that are installed, restricted to English
// plus the language of the user's locale, so a long list of languages doesn't slow every read.
// A folder ~/.local/share/clipitto/tessdata with *.traineddata files, when it exists, is used
// instead of the system's (how an AppImage or snap user adds a language).
#include "Ocr.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QRegularExpression>
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

// Tesseract's code for a language tag: "he" or "he-IL" -> "heb"; a three-letter code ("heb", "chi_sim")
// stays as it is; empty when unknown.
QString tesseractCode(const QString& tag)
{
    static const struct {
        const char* iso;
        const char* tess;
    } map[] = {{"en", "eng"}, {"he", "heb"}, {"ar", "ara"}, {"de", "deu"}, {"fr", "fra"}, {"es", "spa"},
               {"it", "ita"}, {"pt", "por"}, {"ru", "rus"}, {"ja", "jpn"}, {"ko", "kor"}, {"nl", "nld"},
               {"pl", "pol"}, {"tr", "tur"}, {"uk", "ukr"}, {"sv", "swe"}, {"cs", "ces"}, {"el", "ell"},
               {"hi", "hin"}, {"fa", "fas"}, {"th", "tha"}, {"vi", "vie"}, {"hu", "hun"}, {"ro", "ron"},
               {"da", "dan"}, {"fi", "fin"}, {"nb", "nor"}, {"no", "nor"}, {"id", "ind"}, {"bg", "bul"},
               {"hr", "hrv"}, {"sk", "slk"}, {"sl", "slv"}, {"sr", "srp"}, {"lt", "lit"}, {"lv", "lav"},
               {"et", "est"}, {"ca", "cat"}};
    const QString lower = tag.trimmed().toLower();
    if (lower.startsWith(QLatin1String("zh"))) {
        return lower.contains(QLatin1String("tw")) || lower.contains(QLatin1String("hant")) || lower.contains(QLatin1String("hk"))
            ? QStringLiteral("chi_tra")
            : QStringLiteral("chi_sim");
    }
    const QString primary = lower.section(QRegularExpression(QStringLiteral("[-_]")), 0, 0);
    static const QRegularExpression own(QStringLiteral("^[a-z]{3}(_[a-z]{3,4})?$"));
    if (own.match(lower).hasMatch()) {
        return lower; // already Tesseract's own code ("heb", "chi_sim")
    }
    for (const auto& m : map) {
        if (primary == QLatin1String(m.iso)) {
            return QString::fromLatin1(m.tess);
        }
    }
    return {};
}

// Tesseract's code for the user's locale ("he_IL" -> "heb"); empty when unknown.
QString localeLanguage()
{
    return tesseractCode(QLocale::system().bcp47Name());
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

    // `chosen`: the user's languages (tags). Empty: "<locale language>+eng", then either alone, keeping the
    // first the installed data allows.
    explicit Engine(const QStringList& chosen = {})
    {
        const QByteArray dataPath = userDataPath();
        const char* datapath = dataPath.isEmpty() ? nullptr : dataPath.constData();
        QStringList candidates;
        if (!chosen.isEmpty()) {
            QStringList codes;
            for (const QString& tag : chosen) {
                const QString code = tesseractCode(tag);
                if (code.isEmpty()) {
                    problem = tr("Unknown language \"%1\". Use codes like en, he, fr, de.").arg(tag);
                    return;
                }
                codes << code;
            }
            candidates << codes.join(QLatin1Char('+'));
        } else {
            const QString local = localeLanguage();
            if (!local.isEmpty()) {
                candidates << local + QStringLiteral("+eng") << local;
            }
            candidates << QStringLiteral("eng");
        }
        for (const QString& languages : candidates) {
            const QByteArray code = languages.toLatin1();
            if (api.Init(datapath, code.constData()) == 0) {
                ready = true;
                return;
            }
            api.End();
        }
        problem = chosen.isEmpty()
            ? tr("No Tesseract language data found. Install a package such as tesseract-ocr-eng, or put "
                 "*.traineddata files in ~/.local/share/clipitto/tessdata.")
            : tr("The language data for \"%1\" is not installed (a package such as tesseract-ocr-%2, or "
                 "*.traineddata files in ~/.local/share/clipitto/tessdata).")
                  .arg(chosen.join(QStringLiteral(", ")), tesseractCode(chosen.first()));
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

std::optional<QString> recognize(const QByteArray& png, const QStringList& languages, QString* error)
{
    Engine engine(languages);
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
