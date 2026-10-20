#include "ImageCodec.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QImageReader>
#include <QImageWriter>
#include <QLoggingCategory>
#include <QtConcurrent>

Q_LOGGING_CATEGORY(lcImage, "clipitto.capture.image")

namespace clipitto::capture {

namespace {

// A write buffer that fails past `limit` bytes, so a PNG that is going to be too large stops
// growing instead of being finished and thrown away.
class LimitedBuffer : public QBuffer {
public:
    LimitedBuffer(QByteArray* data, qint64 limit) : QBuffer(data), m_limit(limit) {}

protected:
    qint64 writeData(const char* data, qint64 length) override
    {
        if (size() + length > m_limit) {
            return -1;
        }
        return QBuffer::writeData(data, length);
    }

private:
    qint64 m_limit;
};

QByteArray toPng(const QImage& image, qint64 limit)
{
    QByteArray png;
    LimitedBuffer buffer(&png, limit);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    if (!writer.write(image)) {
        return {};
    }
    return png;
}

const char* qtFormat(const QString& mimeType)
{
    const QString type = mimeType.toLower();
    if (type == QLatin1String("image/png")) {
        return "png";
    }
    if (type == QLatin1String("image/bmp")) {
        return "bmp";
    }
    if (type == QLatin1String("image/jpeg") || type == QLatin1String("image/jpg")) {
        return "jpeg";
    }
    if (type == QLatin1String("image/gif")) {
        return "gif";
    }
    return nullptr;
}

QImage decode(const QByteArray& data, const QString& mimeType)
{
    const char* format = qtFormat(mimeType);
    if (!format) {
        return {};
    }
    // Qt refuses to allocate more than its limit (256 MB by default): match kMaxImagePixels.
    static const bool limitSet = [] {
        QImageReader::setAllocationLimit(int(kMaxImagePixels * 4 / (1024 * 1024)) + 1);
        return true;
    }();
    Q_UNUSED(limitSet);
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setDecideFormatFromContent(false); // only the declared format's decoder
    const QSize size = reader.size(); // from the header, before decoding anything
    if (!size.isValid() || qint64(size.width()) * size.height() > kMaxImagePixels) {
        qCDebug(lcImage) << "refused picture:" << format << size;
        return {};
    }
    QImage image;
    if (!reader.read(&image)) {
        qCDebug(lcImage) << "undecodable picture:" << format << data.size() << "bytes";
    }
    return image;
}

void runAsync(std::function<std::optional<ImageClip>()> work, QObject* context,
              std::function<void(std::optional<ImageClip>)> done)
{
    // The watcher belongs to `context`: destroying the context drops the result unseen.
    auto* watcher = new QFutureWatcher<std::optional<ImageClip>>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [watcher, done = std::move(done)] {
        done(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

} // namespace

bool isSupportedImageType(const QString& mimeType)
{
    return qtFormat(mimeType) != nullptr;
}

QString pixelHash(const QImage& image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint32 size[2] = {image.width(), image.height()};
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(size), sizeof(size)));
    // In strips of rows converted to one format: the same pixels hash the same whatever the
    // source format, without a full-size copy of a large picture. Row padding is skipped.
    const qsizetype rowBytes = qsizetype(image.width()) * 4;
    constexpr int kStrip = 64;
    for (int top = 0; top < image.height(); top += kStrip) {
        const int rows = qMin(kStrip, image.height() - top);
        const QImage strip = image.copy(0, top, image.width(), rows).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < rows; ++y) {
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(strip.constScanLine(y)), rowBytes));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

std::optional<ImageClip> encodeImage(const QImage& image, qint64 maxPngBytes)
{
    if (image.isNull() || qint64(image.width()) * image.height() > kMaxImagePixels) {
        return std::nullopt;
    }
    ImageClip clip;
    clip.width = image.width();
    clip.height = image.height();
    clip.png = toPng(image, maxPngBytes);
    if (clip.png.isEmpty()) {
        qCDebug(lcImage) << "picture not recorded:" << image.size() << "is over" << maxPngBytes << "bytes as PNG";
        return std::nullopt;
    }
    clip.pixelHash = pixelHash(image);
    const QImage small = image.width() > kThumbnailSize || image.height() > kThumbnailSize
        ? image.scaled(kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : image;
    clip.thumbnail = toPng(small, maxPngBytes);
    if (!clip.isValid() || clip.thumbnail.isEmpty()) {
        return std::nullopt;
    }
    return clip;
}

void encodeImageAsync(const QImage& image, qint64 maxPngBytes, QObject* context,
                      std::function<void(std::optional<ImageClip>)> done)
{
    runAsync([image, maxPngBytes] { return encodeImage(image, maxPngBytes); }, context, std::move(done));
}

void encodeImageDataAsync(const QByteArray& data, const QString& mimeType, qint64 maxPngBytes, QObject* context,
                          std::function<void(std::optional<ImageClip>)> done)
{
    runAsync([data, mimeType, maxPngBytes] { return encodeImage(decode(data, mimeType), maxPngBytes); }, context,
             std::move(done));
}

} // namespace clipitto::capture
