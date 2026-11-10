#include "DebugLog.h"

#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QTest>

Q_LOGGING_CATEGORY(lcTestLog, "clipitto.test")

class TestDebugLog : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { clipitto::debuglog::stop(); }

    void writesTimestampedLinesAndStopsWhenAsked()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("debug.log"));
        QVERIFY(clipitto::debuglog::start(path));
        qCInfo(lcTestLog) << "first line";
        qCDebug(lcTestLog) << "second line";
        clipitto::debuglog::stop();
        qCInfo(lcTestLog) << "after stop";

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QLatin1String("clipitto.test: first line")));
        QVERIFY(text.contains(QLatin1String("clipitto.test: second line")));
        QVERIFY(!text.contains(QLatin1String("after stop")));
        // A time stamp first: 2026-11-10 12:00:00.123
        QVERIFY(QRegularExpression(QStringLiteral(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} )")).match(text).hasMatch());
    }

    void startingAgainEmptiesTheFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("debug.log"));
        QVERIFY(clipitto::debuglog::start(path));
        qCInfo(lcTestLog) << "old run";
        QVERIFY(clipitto::debuglog::start(path));
        qCInfo(lcTestLog) << "new run";
        clipitto::debuglog::stop();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(!text.contains(QLatin1String("old run")));
        QVERIFY(text.contains(QLatin1String("new run")));
    }

    void rotatesAtTheLimitKeepingTheNewestLines()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("debug.log"));
        QVERIFY(clipitto::debuglog::start(path, 400));
        for (int i = 0; i < 200; ++i) {
            qCInfo(lcTestLog) << "a line of about this length" << i;
        }
        clipitto::debuglog::stop();
        QFile current(path);
        QVERIFY(current.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(current.readAll());
        QVERIFY(current.size() <= 400);
        QVERIFY2(text.contains(QLatin1String("this length 199")), "the newest line is in the current file");
        QVERIFY(QFile::exists(path + QLatin1String(".1"))); // one older part, no more
        QVERIFY(!QFile::exists(path + QLatin1String(".2")));
        current.close(); // Windows cannot delete a file that is open
        clipitto::debuglog::remove(path);
        QVERIFY(!QFile::exists(path) && !QFile::exists(path + QLatin1String(".1")));
    }

    void doesNotChangeTheMessageFormatOfTheProcess()
    {
        // The time stamps are added by the handler itself: Qt's own pattern (QT_MESSAGE_PATTERN, the test runner's) is left alone.
        QTemporaryDir dir;
        QVERIFY(clipitto::debuglog::start(dir.filePath(QStringLiteral("debug.log"))));
        clipitto::debuglog::stop();
        QCOMPARE(qFormatLogMessage(QtInfoMsg, QMessageLogContext(), QStringLiteral("x")), QStringLiteral("x"));
    }

#if !defined(Q_OS_WIN)
    void isPrivateFromTheFirstMomentAndRefusesALink()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("debug.log"));
        QVERIFY(clipitto::debuglog::start(path));
        const QFileDevice::Permissions mode = QFile::permissions(path);
        clipitto::debuglog::stop();
        QCOMPARE(mode & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        QVERIFY(mode & QFileDevice::ReadOwner);

        // A symbolic link at the path is not followed (it would overwrite the target).
        const QString target = dir.filePath(QStringLiteral("precious.txt"));
        QFile precious(target);
        QVERIFY(precious.open(QIODevice::WriteOnly));
        precious.write("keep me");
        precious.close();
        const QString link = dir.filePath(QStringLiteral("link.log"));
        QVERIFY(QFile::link(target, link));
        QVERIFY(!clipitto::debuglog::start(link));
        QVERIFY(precious.open(QIODevice::ReadOnly));
        QCOMPARE(precious.readAll(), QByteArray("keep me"));
    }
#endif

    void failsOnAFileThatCannotBeOpened()
    {
        QVERIFY(!clipitto::debuglog::start(QStringLiteral("/nonexistent-directory-for-test/debug.log")));
    }
};

QTEST_GUILESS_MAIN(TestDebugLog)
#include "tst_debuglog.moc"
