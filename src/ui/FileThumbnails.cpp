#include "FileThumbnails.h"

#include <QFileInfo>
#include <QImageReader>
#include <QLoggingCategory>
#include <QSet>
#include <QStorageInfo>

namespace clipitto::ui {

Q_LOGGING_CATEGORY(lcThumbs, "clipitto.thumbnails")

namespace {

constexpr int kScanFiles = 8; // how many paths of a list are looked at for a picture

const QSet<QString>& pictureSuffixes()
{
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        for (const QByteArray& format : QImageReader::supportedImageFormats()) {
            set.insert(QString::fromLatin1(format).toLower());
        }
        set.insert(QStringLiteral("jpg")); // Qt lists "jpeg" only on some builds
        set.remove(QStringLiteral("svg")); // drawn code, not a photo
        set.remove(QStringLiteral("svgz"));
        return set;
    }();
    return suffixes;
}

bool networkFileSystem(const QString& path)
{
    static const QSet<QString> types = {
        QStringLiteral("nfs"), QStringLiteral("nfs4"), QStringLiteral("cifs"), QStringLiteral("smb"),
        QStringLiteral("smb2"), QStringLiteral("smb3"), QStringLiteral("smbfs"), QStringLiteral("afs"),
        QStringLiteral("ncpfs"), QStringLiteral("davfs"), QStringLiteral("fuse.sshfs"), QStringLiteral("sshfs"),
        QStringLiteral("fuse.davfs2"), QStringLiteral("fuse.gvfsd-fuse"), QStringLiteral("fuse.rclone"),
        QStringLiteral("afpfs"), QStringLiteral("webdav"), QStringLiteral("macfuse"), QStringLiteral("osxfuse"),
        QStringLiteral("ceph"), QStringLiteral("glusterfs"), QStringLiteral("lustre"),
    };
    const QStorageInfo info(QFileInfo(path).absolutePath());
    return info.isValid() && types.contains(QString::fromLatin1(info.fileSystemType()).toLower());
}

} // namespace

FileThumbnails::FileThumbnails(QObject* parent, const Limits& limits)
    : QObject(parent)
    , m_limits(limits)
    , m_cache(qMax(1, limits.cacheSize))
{
    m_pool.setMaxThreadCount(2);
    pictureSuffixes(); // the reader plugins are listed here, not first thing in a worker
}

FileThumbnails::~FileThumbnails()
{
    ++m_generation; // queued work does nothing
    m_pool.waitForDone(); // before the object is gone: a worker posts its result to it
}

bool FileThumbnails::looksLikeImage(const QString& path)
{
    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
    const qsizetype slash = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return dot > slash + 1 && pictureSuffixes().contains(path.mid(dot + 1).toLower());
}

QString FileThumbnails::pictureAmong(const QStringList& paths)
{
    for (int i = 0; i < paths.size() && i < kScanFiles; ++i) {
        if (looksLikeImage(paths.at(i))) {
            return paths.at(i);
        }
    }
    return {};
}

bool FileThumbnails::isNetworkPath(const QString& path)
{
    // A slow share would stall whatever opens it: only local files are read without being asked.
    if (path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("\\\\"))
        || path.startsWith(QLatin1String("smb:"), Qt::CaseInsensitive) || path.contains(QLatin1String("://"))) {
        return true;
    }
    return path.contains(QLatin1String("/gvfs/")) || path.contains(QLatin1String("/.gvfs/"));
}

const char* FileThumbnails::outcomeName(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Ok: return "ok";
    case Outcome::Missing: return "missing";
    case Outcome::NotAPicture: return "not-a-picture";
    case Outcome::TooLarge: return "too-large";
    case Outcome::Network: return "network";
    case Outcome::Unreadable: return "unreadable";
    }
    return "?";
}

FileThumbnails::Outcome FileThumbnails::load(const QString& path, const Limits& limits, QImage* image)
{
    if (!looksLikeImage(path)) {
        return Outcome::NotAPicture; // by the name: other files are never opened
    }
    if (isNetworkPath(path)) {
        return Outcome::Network;
    }
    if (!path.startsWith(QLatin1Char('/')) && !(path.size() > 2 && path.at(1) == QLatin1Char(':'))) {
        return Outcome::NotAPicture; // absolute local paths only (not a Qt resource like ":/x.png")
    }
    // The file systems are asked first (they do not touch the file), for the link's folder and, for a symlink, the
    // folder of what it names (readlink only), so a link on this disk to a share is not followed onto it.
    if (networkFileSystem(path)) {
        return Outcome::Network;
    }
    const QString target = QFileInfo(path).symLinkTarget();
    if (!target.isEmpty() && (isNetworkPath(target) || networkFileSystem(target))) {
        return Outcome::Network;
    }
    // exists/isFile follow a symlink to the one file it names (a link to a directory, a pipe or a device is not a picture).
    const QFileInfo info(path);
    if (!info.exists()) {
        return Outcome::Missing;
    }
    if (!info.isFile()) {
        return Outcome::NotAPicture;
    }
    if (info.size() > limits.maxBytes) {
        return Outcome::TooLarge;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        return Outcome::NotAPicture;
    }
    const QSize size = reader.size(); // from the header, nothing decoded yet
    if (!size.isValid()) {
        return Outcome::Unreadable;
    }
    if (qint64(size.width()) * size.height() > limits.maxPixels) {
        return Outcome::TooLarge;
    }
    if (size.width() > limits.maxSide || size.height() > limits.maxSide) {
        reader.setScaledSize(size.scaled(limits.maxSide, limits.maxSide, Qt::KeepAspectRatio)); // JPEG decodes small
    }
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        return Outcome::Unreadable;
    }
    if (decoded.width() > limits.maxSide || decoded.height() > limits.maxSide) {
        decoded = decoded.scaled(limits.maxSide, limits.maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    *image = decoded;
    return Outcome::Ok;
}

QPixmap FileThumbnails::thumbnail(const QString& path)
{
    if (!looksLikeImage(path)) {
        return {};
    }
    if (const Entry* entry = m_cache.object(path)) { // also marks it as recently used
        return entry->pixmap;
    }
    if (m_inFlight.contains(path)) {
        return {};
    }
    const quint64 generation = m_generation.load();
    m_inFlight.insert(path, generation);
    m_pool.start([this, generation, path, limits = m_limits] {
        if (m_generation.load() != generation) {
            return;
        }
        QImage image;
        const Outcome outcome = load(path, limits, &image);
        QMetaObject::invokeMethod(this, [this, generation, path, outcome, image] { finished(generation, path, outcome, image); },
                                  Qt::QueuedConnection);
    });
    return {};
}

void FileThumbnails::finished(quint64 generation, const QString& path, Outcome outcome, const QImage& image)
{
    if (generation != m_generation.load()) {
        return;
    }
    m_inFlight.remove(path);
    ++m_counts[int(outcome)];
    qCDebug(lcThumbs) << "file thumbnail:" << outcomeName(outcome) << "(" << m_counts[int(outcome)] << "so far )"; // never the path
    auto* entry = new Entry;
    if (outcome == Outcome::Ok) {
        entry->pixmap = QPixmap::fromImage(image);
    }
    m_cache.insert(path, entry);
    if (outcome == Outcome::Ok) {
        emit ready(path);
    }
}

void FileThumbnails::clear()
{
    ++m_generation;
    m_inFlight.clear();
    m_cache.clear();
}

} // namespace clipitto::ui
