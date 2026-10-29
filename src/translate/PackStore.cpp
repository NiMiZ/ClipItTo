#include "Translate.h"
#include "ZipReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

namespace clipitto::translate {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::translate", text);
}

bool fail(QString* error, const QString& why)
{
    if (error) {
        *error = why;
    }
    return false;
}

// The files of a pack that the engine reads; everything else in the zip (a sentence-splitting model for
// Python, the README) is left out.
bool wanted(const QString& relative)
{
    return relative == QLatin1String("metadata.json") || relative == QLatin1String("sentencepiece.model")
        || relative == QLatin1String("README.md") || relative == QLatin1String("model/model.bin")
        || relative == QLatin1String("model/config.json") || relative == QLatin1String("model/shared_vocabulary.json")
        || relative == QLatin1String("model/shared_vocabulary.txt") || relative == QLatin1String("model/source_vocabulary.json")
        || relative == QLatin1String("model/target_vocabulary.json");
}

bool validCode(const QString& code)
{
    static const QRegularExpression re(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2,4})?$"));
    return re.match(code).hasMatch();
}

std::optional<Pack> readPack(const QString& dir)
{
    QFile meta(dir + QStringLiteral("/metadata.json"));
    if (!meta.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonObject o = QJsonDocument::fromJson(meta.readAll()).object();
    Pack p;
    p.from = o.value(QStringLiteral("from_code")).toString();
    p.to = o.value(QStringLiteral("to_code")).toString();
    p.fromName = o.value(QStringLiteral("from_name")).toString(p.from);
    p.toName = o.value(QStringLiteral("to_name")).toString(p.to);
    if (!validCode(p.from) || !validCode(p.to) || !QFileInfo::exists(dir + QStringLiteral("/model/model.bin"))
        || !QFileInfo::exists(dir + QStringLiteral("/sentencepiece.model"))) {
        return std::nullopt;
    }
    p.id = p.from + QLatin1Char('-') + p.to;
    p.path = dir;
    return p;
}

} // namespace

QString PackStore::defaultRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/translation");
}

PackStore::PackStore(QString root) : m_root(std::move(root)) {}

QList<Pack> PackStore::installed() const
{
    QList<Pack> out;
    const QDir root(m_root);
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (const auto pack = readPack(root.filePath(name))) {
            out.push_back(*pack);
        }
    }
    return out;
}

bool PackStore::install(const QString& file, QString* error)
{
    ZipReader zip;
    if (!zip.open(file, error)) {
        return false;
    }
    // Entries are "<top folder>/<relative path>"; find the folder that holds metadata.json.
    QString top;
    for (const QString& name : zip.names()) {
        if (name.endsWith(QStringLiteral("/metadata.json")) && name.count(QLatin1Char('/')) == 1) {
            top = name.section(QLatin1Char('/'), 0, 0);
            break;
        }
    }
    if (top.isEmpty()) {
        return fail(error, tr("This is not a language pack (.argosmodel)."));
    }
    const QByteArray metaBytes = zip.read(top + QStringLiteral("/metadata.json"), error);
    const QJsonObject meta = QJsonDocument::fromJson(metaBytes).object();
    const QString from = meta.value(QStringLiteral("from_code")).toString();
    const QString to = meta.value(QStringLiteral("to_code")).toString();
    if (!validCode(from) || !validCode(to)) {
        return fail(error, tr("The language pack has no valid languages."));
    }
    if (zip.sizeOf(top + QStringLiteral("/model/model.bin")) < 0 || zip.sizeOf(top + QStringLiteral("/sentencepiece.model")) < 0) {
        return fail(error, tr("The language pack does not hold a translation model."));
    }

    // Into a temporary folder first, then in place: a failed install leaves the old pack alone.
    const QString id = from + QLatin1Char('-') + to;
    const QString finalDir = m_root + QLatin1Char('/') + id;
    const QString tempDir = m_root + QStringLiteral("/.installing-") + id;
    QDir(tempDir).removeRecursively();
    QDir().mkpath(tempDir);
    const QString prefix = top + QLatin1Char('/');
    for (const QString& name : zip.names()) {
        if (!name.startsWith(prefix) || name.endsWith(QLatin1Char('/'))) {
            continue;
        }
        const QString relative = name.mid(prefix.size());
        if (!wanted(relative) || relative.contains(QStringLiteral(".."))) {
            continue;
        }
        if (!zip.extract(name, tempDir + QLatin1Char('/') + relative, error)) {
            QDir(tempDir).removeRecursively();
            return false;
        }
    }
    if (!readPack(tempDir)) {
        QDir(tempDir).removeRecursively();
        return fail(error, tr("The language pack is incomplete."));
    }
    QDir(finalDir).removeRecursively();
    if (!QDir().rename(tempDir, finalDir)) {
        QDir(tempDir).removeRecursively();
        return fail(error, tr("Could not install the language pack into %1.").arg(finalDir));
    }
    return true;
}

bool PackStore::remove(const QString& id)
{
    static const QRegularExpression safe(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2,4})?-[a-z]{2,3}(-[A-Za-z]{2,4})?$"));
    if (!safe.match(id).hasMatch()) {
        return false;
    }
    return QDir(m_root + QLatin1Char('/') + id).removeRecursively();
}

} // namespace clipitto::translate
