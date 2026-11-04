#include "QrImage.h"

#include <QTest>

using clipitto::ui::qrImage;

class TestQrImage : public QObject {
    Q_OBJECT

private slots:
    void drawsFinderPatternsWithAQuietZone()
    {
        QString error;
        const QImage image = qrImage(QStringLiteral("https://example.org"), 300, &error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QVERIFY(image.width() >= 300);
        QCOMPARE(image.width(), image.height());
        // The 4-module quiet zone is white all around.
        QCOMPARE(image.pixel(0, 0), qRgb(255, 255, 255)); // quiet zone
        QCOMPARE(image.pixel(image.width() - 1, image.height() - 1), qRgb(255, 255, 255));
        int black = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                black += image.pixel(x, y) == qRgb(0, 0, 0);
            }
        }
        QVERIFY(black > image.width() * image.height() / 10); // a real pattern, not a blank
        QVERIFY(black < image.width() * image.height() * 6 / 10);
        // The first dark pixel of the middle row is the finder pattern, after the quiet zone: at least 4 modules in.
        int firstDark = -1;
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, image.height() / 2) == qRgb(0, 0, 0)) {
                firstDark = x;
                break;
            }
        }
        QVERIFY(firstDark >= 4);
    }

    void holdsLongAndNonLatinText()
    {
        QString error;
        QVERIFY(!qrImage(QStringLiteral("שלום עולם — ✓"), 200, &error).isNull());
        QVERIFY2(!qrImage(QString(2900, QLatin1Char('a')), 200, &error).isNull(), qPrintable(error)); // low correction
    }

    void refusesWhatDoesNotFit()
    {
        QString error;
        QVERIFY(qrImage(QString(3000, QLatin1Char('a')), 200, &error).isNull());
        QVERIFY(error.contains(QLatin1String("too long")));
        error.clear();
        QVERIFY(qrImage(QString(), 200, &error).isNull());
        QVERIFY(!error.isEmpty());
    }

    void growsWithTheRequestedSize()
    {
        const QImage small = qrImage(QStringLiteral("x"), 100);
        const QImage big = qrImage(QStringLiteral("x"), 400);
        QVERIFY(small.width() >= 100 && big.width() >= 400);
        QVERIFY(big.width() > small.width());
    }
};

QTEST_MAIN(TestQrImage)
#include "tst_qrimage.moc"
