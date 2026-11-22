#include "FileThumbnails.h"
#include "StallWatchdog.h"

#include <QImage>
#include <QTemporaryDir>
#include <QTest>

using clipitto::ui::FileThumbnails;

// A list of 200 picture files must not make the GUI thread wait for the disk or the decoder.
class TestPerfThumbnails : public QObject {
    Q_OBJECT

private slots:
    void twoHundredPicturesDoNotStallTheEventLoop()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage image(1600, 1200, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixel(x, y, qRgb(x % 256, y % 256, (x ^ y) % 256)); // not trivial: a real decode
            }
        }
        QStringList paths;
        for (int i = 0; i < 200; ++i) {
            paths << dir.filePath(QStringLiteral("p%1.png").arg(i));
            QVERIFY(image.save(paths.last(), "PNG"));
        }

        FileThumbnails::Limits limits;
        limits.cacheSize = 200;
        FileThumbnails thumbs(nullptr, limits);
        StallWatchdog wd;
        for (const QString& path : std::as_const(paths)) {
            thumbs.thumbnail(path); // what painting 200 rows does
        }
        QTRY_COMPARE_WITH_TIMEOUT(thumbs.cachedCount(), 200, 60000);
        QVERIFY_NO_STALL(wd, "200 picture files");
    }
};

QTEST_MAIN(TestPerfThumbnails)
#include "tst_perf_thumbnails.moc"
