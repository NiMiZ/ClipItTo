#include "FileThumbnails.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::ui::FileThumbnails;
using Outcome = FileThumbnails::Outcome;

namespace {

QStringList g_messages;

void collect(QtMsgType, const QMessageLogContext&, const QString& message)
{
    g_messages << message;
}

bool makeImage(const QString& path, int w, int h, const char* format)
{
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(Qt::red);
    return image.save(path, format);
}

} // namespace

class TestFileThumbnails : public QObject {
    Q_OBJECT

private slots:
    void init() { QVERIFY(m_dir.isValid()); }

    void aPngAndAJpegGetAThumbnailNoLargerThanTheLimit()
    {
        const QString png = m_dir.filePath(QStringLiteral("a.png"));
        const QString jpg = m_dir.filePath(QStringLiteral("b.JPG"));
        QVERIFY(makeImage(png, 640, 320, "PNG"));
        QVERIFY(makeImage(jpg, 100, 300, "JPG"));
        FileThumbnails thumbs;
        QSignalSpy ready(&thumbs, &FileThumbnails::ready);
        QVERIFY(thumbs.thumbnail(png).isNull()); // asked for: made in the background
        QVERIFY(thumbs.thumbnail(jpg).isNull());
        QTRY_COMPARE(ready.size(), 2);
        const QPixmap a = thumbs.thumbnail(png);
        const QPixmap b = thumbs.thumbnail(jpg);
        QCOMPARE(a.size(), QSize(128, 64)); // aspect kept
        QCOMPARE(b.height(), 128);
        QVERIFY(b.width() <= 128);
        QCOMPARE(thumbs.outcomeCount(Outcome::Ok), 2);
        QCOMPARE(thumbs.pendingCount(), 0);
    }

    void aSmallPictureIsNotEnlarged()
    {
        const QString png = m_dir.filePath(QStringLiteral("small.png"));
        QVERIFY(makeImage(png, 20, 10, "PNG"));
        QImage out;
        QCOMPARE(FileThumbnails::load(png, {}, &out), Outcome::Ok);
        QCOMPARE(out.size(), QSize(20, 10));
    }

    void aMissingFileKeepsTheIcon()
    {
        FileThumbnails thumbs;
        QSignalSpy ready(&thumbs, &FileThumbnails::ready);
        const QString gone = m_dir.filePath(QStringLiteral("moved.png"));
        QVERIFY(thumbs.thumbnail(gone).isNull());
        QTRY_COMPARE(thumbs.outcomeCount(Outcome::Missing), 1);
        QVERIFY(thumbs.thumbnail(gone).isNull());
        QCOMPARE(thumbs.outcomeCount(Outcome::Missing), 1); // remembered: not looked at again at every paint
        QCOMPARE(ready.size(), 0);
    }

    void aFileThatIsNotAPictureIsNeverOpened()
    {
        const QString txt = m_dir.filePath(QStringLiteral("notes.txt"));
        QFile f(txt);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
        QVERIFY(!FileThumbnails::looksLikeImage(txt));
        QVERIFY(!FileThumbnails::looksLikeImage(QStringLiteral("/home/me/.png"))); // a name that is only a suffix
        QVERIFY(!FileThumbnails::looksLikeImage(QStringLiteral("/a/b.svg"))); // drawn code, not a photo
        FileThumbnails thumbs;
        QVERIFY(thumbs.thumbnail(txt).isNull());
        QCOMPARE(thumbs.pendingCount(), 0); // not even asked for
        QImage out;
        QCOMPARE(FileThumbnails::load(txt, {}, &out), Outcome::NotAPicture);
        // Named like a picture, but not one.
        const QString fake = m_dir.filePath(QStringLiteral("fake.png"));
        QFile g(fake);
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write("not a png at all");
        g.close();
        QCOMPARE(FileThumbnails::load(fake, {}, &out), Outcome::NotAPicture);
        // A directory with a picture's name.
        QVERIFY(QDir(m_dir.path()).mkdir(QStringLiteral("dir.png")));
        QCOMPARE(FileThumbnails::load(m_dir.filePath(QStringLiteral("dir.png")), {}, &out), Outcome::NotAPicture);
    }

    void anOversizedPictureIsRefusedBeforeDecoding()
    {
        const QString png = m_dir.filePath(QStringLiteral("big.png"));
        QVERIFY(makeImage(png, 400, 300, "PNG"));
        QImage out;
        FileThumbnails::Limits limits;
        limits.maxPixels = 100 * 100;
        QCOMPARE(FileThumbnails::load(png, limits, &out), Outcome::TooLarge);
        QVERIFY(out.isNull());
        limits = {};
        limits.maxBytes = 10; // the file is larger than this many bytes
        QCOMPARE(FileThumbnails::load(png, limits, &out), Outcome::TooLarge);
        QCOMPARE(FileThumbnails::load(png, {}, &out), Outcome::Ok);

        FileThumbnails::Limits small;
        small.maxPixels = 100;
        FileThumbnails thumbs(nullptr, small);
        QVERIFY(thumbs.thumbnail(png).isNull());
        QTRY_COMPARE(thumbs.outcomeCount(Outcome::TooLarge), 1);
        QVERIFY(thumbs.thumbnail(png).isNull());
    }

    void networkPathsAreNotRead()
    {
        QVERIFY(FileThumbnails::isNetworkPath(QStringLiteral("//server/share/a.png")));
        QVERIFY(FileThumbnails::isNetworkPath(QStringLiteral("\\\\server\\share\\a.png")));
        QVERIFY(FileThumbnails::isNetworkPath(QStringLiteral("smb://server/share/a.png")));
        QVERIFY(FileThumbnails::isNetworkPath(QStringLiteral("/run/user/1000/gvfs/smb-share:server=x,share=y/a.png")));
        QVERIFY(!FileThumbnails::isNetworkPath(QStringLiteral("/home/me/a.png")));
        QVERIFY(!FileThumbnails::isNetworkPath(QStringLiteral("C:\\Users\\me\\a.png")));
        QImage out;
        QCOMPARE(FileThumbnails::load(QStringLiteral("//server/share/a.png"), {}, &out), Outcome::Network);
    }

    void aSymlinkToADirectoryOrBrokenIsNotAPicture()
    {
        const QString real = m_dir.filePath(QStringLiteral("real.png"));
        QVERIFY(makeImage(real, 10, 10, "PNG"));
        const QString link = m_dir.filePath(QStringLiteral("link.png"));
        if (!QFile::link(real, link)) {
            QSKIP("no symbolic links here");
        }
        QImage out;
        QCOMPARE(FileThumbnails::load(link, {}, &out), Outcome::Ok); // the one file it names
        QVERIFY(QDir(m_dir.path()).mkdir(QStringLiteral("d")));
        QVERIFY(QFile::link(m_dir.filePath(QStringLiteral("d")), m_dir.filePath(QStringLiteral("dl.png"))));
        QCOMPARE(FileThumbnails::load(m_dir.filePath(QStringLiteral("dl.png")), {}, &out), Outcome::NotAPicture);
        QVERIFY(QFile::link(m_dir.filePath(QStringLiteral("nowhere")), m_dir.filePath(QStringLiteral("broken.png"))));
        QCOMPARE(FileThumbnails::load(m_dir.filePath(QStringLiteral("broken.png")), {}, &out), Outcome::Missing);
    }

    void theCacheIsALeastRecentlyUsedList()
    {
        FileThumbnails::Limits limits;
        limits.cacheSize = 3;
        FileThumbnails thumbs(nullptr, limits);
        QStringList paths;
        for (int i = 0; i < 5; ++i) {
            paths << m_dir.filePath(QStringLiteral("p%1.png").arg(i));
            QVERIFY(makeImage(paths.last(), 16, 16, "PNG"));
        }
        for (int i = 0; i < 3; ++i) {
            thumbs.thumbnail(paths.at(i));
        }
        QTRY_COMPARE(thumbs.cachedCount(), 3);
        QVERIFY(!thumbs.thumbnail(paths.at(0)).isNull()); // 0 is now the most recently used
        thumbs.thumbnail(paths.at(3)); // evicts 1, the least recently used
        QTRY_COMPARE(thumbs.outcomeCount(Outcome::Ok), 4);
        QCOMPARE(thumbs.cachedCount(), 3);
        QVERIFY(!thumbs.thumbnail(paths.at(0)).isNull());
        QVERIFY(!thumbs.thumbnail(paths.at(2)).isNull());
        QVERIFY(!thumbs.thumbnail(paths.at(3)).isNull());
        QVERIFY(thumbs.thumbnail(paths.at(1)).isNull()); // evicted: asked for again
        QCOMPARE(thumbs.pendingCount(), 1);
        QTRY_COMPARE(thumbs.outcomeCount(Outcome::Ok), 5);
        QCOMPARE(thumbs.limits().cacheSize, 3);
    }

    void clearMakesTheNextShowLookAtTheFileAgain()
    {
        const QString png = m_dir.filePath(QStringLiteral("c.png"));
        QVERIFY(makeImage(png, 16, 16, "PNG"));
        FileThumbnails thumbs;
        thumbs.thumbnail(png);
        QTRY_COMPARE(thumbs.cachedCount(), 1);
        QVERIFY(QFile::remove(png));
        QVERIFY(!thumbs.thumbnail(png).isNull()); // still the one from before
        thumbs.clear();
        QCOMPARE(thumbs.cachedCount(), 0);
        QVERIFY(thumbs.thumbnail(png).isNull());
        QTRY_COMPARE(thumbs.outcomeCount(Outcome::Missing), 1);
    }

    void pictureAmongTakesTheFirstPictureOfTheFirstFew()
    {
        QCOMPARE(FileThumbnails::pictureAmong({QStringLiteral("/a/x.txt"), QStringLiteral("/a/y.png")}),
                 QStringLiteral("/a/y.png"));
        QCOMPARE(FileThumbnails::pictureAmong({QStringLiteral("/a/x.txt")}), QString());
        QStringList many;
        for (int i = 0; i < 20; ++i) {
            many << QStringLiteral("/a/f%1.txt").arg(i);
        }
        many << QStringLiteral("/a/late.png");
        QCOMPARE(FileThumbnails::pictureAmong(many), QString()); // a long list is not searched
    }

    void nothingIsLoggedAboutPathsOrNames()
    {
        const QString secretName = QStringLiteral("zq-private-holiday-photo");
        const QString png = m_dir.filePath(secretName + QStringLiteral(".png"));
        QVERIFY(makeImage(png, 16, 16, "PNG"));
        const QString gone = m_dir.filePath(secretName + QStringLiteral("-gone.jpg"));
        g_messages.clear();
        QLoggingCategory::setFilterRules(QStringLiteral("clipitto.*=true"));
        const QtMessageHandler old = qInstallMessageHandler(&collect);
        {
            FileThumbnails thumbs;
            thumbs.thumbnail(png);
            thumbs.thumbnail(gone);
            QTRY_COMPARE(thumbs.outcomeCount(Outcome::Ok) + thumbs.outcomeCount(Outcome::Missing), 2);
        }
        qInstallMessageHandler(old);
        QLoggingCategory::setFilterRules(QString());
        QVERIFY(!g_messages.isEmpty()); // it does log the outcome class
        for (const QString& line : std::as_const(g_messages)) {
            QVERIFY2(!line.contains(secretName) && !line.contains(m_dir.path()) && !line.contains(QLatin1String(".png")),
                     qPrintable(line));
        }
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(TestFileThumbnails)
#include "tst_filethumbnails.moc"
