#include "ImageCodec.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTest>

using clipitto::ImageClip;
using clipitto::kThumbnailSize;
namespace capture = clipitto::capture;

namespace {

// A recognizable picture: a gradient with a red square.
QImage picture(int w, int h, QImage::Format format = QImage::Format_RGB32)
{
    QImage image(w, h, format);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            image.setPixelColor(x, y, QColor((x * 255) / qMax(1, w - 1), (y * 255) / qMax(1, h - 1), 128));
        }
    }
    QPainter(&image).fillRect(w / 4, h / 4, w / 4, h / 4, Qt::red);
    return image;
}

} // namespace

class TestImageCodec : public QObject {
    Q_OBJECT

private slots:
    void encodesPngThumbnailAndLabel()
    {
        const auto clip = capture::encodeImage(picture(1600, 900), 50'000'000);
        QVERIFY(clip);
        QCOMPARE(clip->width, 1600);
        QCOMPARE(clip->height, 900);
        QCOMPARE(clip->label(), QStringLiteral("Image 1600×900"));
        const QImage png = QImage::fromData(clip->png, "PNG");
        QCOMPARE(png.size(), QSize(1600, 900));
        const QImage thumb = QImage::fromData(clip->thumbnail, "PNG");
        QCOMPARE(thumb.width(), kThumbnailSize); // the longer side
        QCOMPARE(thumb.height(), kThumbnailSize * 900 / 1600);

        // A small picture is its own thumbnail.
        const auto small = capture::encodeImage(picture(40, 20), 50'000'000);
        QCOMPARE(QImage::fromData(small->thumbnail, "PNG").size(), QSize(40, 20));
    }

    void samePixelsSameHashWhateverTheFormat()
    {
        const QImage rgb = picture(64, 48, QImage::Format_RGB32);
        const QImage argb = rgb.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage rgb888 = rgb.convertToFormat(QImage::Format_RGB888); // other row padding
        QCOMPARE(capture::pixelHash(argb), capture::pixelHash(rgb));
        QCOMPARE(capture::pixelHash(rgb888), capture::pixelHash(rgb));

        // Our own paste comes back as the PNG we stored: still the same picture.
        const auto clip = capture::encodeImage(rgb, 50'000'000);
        QCOMPARE(capture::pixelHash(QImage::fromData(clip->png, "PNG")), clip->pixelHash);

        QImage changed = rgb;
        changed.setPixelColor(0, 0, Qt::black);
        QVERIFY(capture::pixelHash(changed) != capture::pixelHash(rgb));
        QVERIFY(capture::pixelHash(picture(48, 64)) != capture::pixelHash(rgb)); // size counts
    }

    void nullOrUndecodablePicturesAreRefused()
    {
        QVERIFY(!capture::encodeImage(QImage(), 50'000'000));
        bool called = false;
        bool got = true;
        capture::encodeImageDataAsync(QByteArray("not a picture"), QStringLiteral("image/png"), 50'000'000, this, [&](std::optional<ImageClip> clip) {
            called = true;
            got = clip.has_value();
        });
        QTRY_VERIFY(called);
        QVERIFY(!got);
    }

    void limitsAreAppliedBeforeAndDuringTheWork()
    {
        // Only the formats with a known decoder; the declared format is used, not guessed.
        QVERIFY(capture::isSupportedImageType(QStringLiteral("image/png")));
        QVERIFY(capture::isSupportedImageType(QStringLiteral("image/JPEG")));
        QVERIFY(!capture::isSupportedImageType(QStringLiteral("image/svg+xml")));
        QVERIFY(!capture::isSupportedImageType(QStringLiteral("image/tiff")));
        const QByteArray png = capture::encodeImage(picture(64, 64), 50'000'000)->png;
        bool called = false;
        bool got = true;
        capture::encodeImageDataAsync(png, QStringLiteral("image/svg+xml"), 50'000'000, this,
                                      [&](std::optional<ImageClip> clip) { called = true; got = clip.has_value(); });
        QTRY_VERIFY(called);
        QVERIFY(!got);

        // A PNG over the byte limit is not finished.
        QVERIFY(!capture::encodeImage(picture(400, 300), 1000));
        QVERIFY(capture::encodeImage(picture(400, 300), 5'000'000));
    }

    void asyncEncodingReportsOnTheCallerThread()
    {
        const QByteArray png = capture::encodeImage(picture(100, 50), 50'000'000)->png;
        std::optional<ImageClip> result;
        Qt::HANDLE thread = nullptr;
        capture::encodeImageDataAsync(png, QStringLiteral("image/png"), 50'000'000, this, [&](std::optional<ImageClip> clip) {
            result = clip;
            thread = QThread::currentThreadId();
        });
        QTRY_VERIFY(result.has_value());
        QCOMPARE(thread, QThread::currentThreadId());
        QCOMPARE(result->width, 100);

        // A context destroyed before the work finishes never hears back.
        bool called = false;
        {
            QObject context;
            capture::encodeImageAsync(picture(800, 600), 50'000'000, &context, [&](std::optional<ImageClip>) { called = true; });
        }
        QTest::qWait(300);
        QVERIFY(!called);
    }
};

QTEST_MAIN(TestImageCodec)
#include "tst_imagecodec.moc"
