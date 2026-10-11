#include "ClipFilter.h"

#include <QTest>

using clipitto::ClipFilter;
using Verdict = ClipFilter::Verdict;

class TestClipFilter : public QObject {
    Q_OBJECT

private slots:
    void acceptsOrdinaryText()
    {
        const ClipFilter f(1024);
        QVERIFY(f.check(QStringLiteral("hello"), {QStringLiteral("text/plain")}) == Verdict::Accept);
    }

    void rejectsBlankText()
    {
        const ClipFilter f(1024);
        QVERIFY(f.check(QString(), {}) == Verdict::Empty);
        QVERIFY(f.check(QStringLiteral(" \n\t "), {}) == Verdict::Empty);
    }

    void measuresSizeInUtf8Bytes()
    {
        const ClipFilter f(10);
        QVERIFY(f.check(QStringLiteral("0123456789"), {}) == Verdict::Accept);
        QVERIFY(f.check(QStringLiteral("0123456789A"), {}) == Verdict::TooLarge);
        // 4 characters, 12 UTF-8 bytes.
        QVERIFY(f.check(QStringLiteral("日本語字"), {}) == Verdict::TooLarge);
        QVERIFY(f.check(QStringLiteral("日本語"), {}) == Verdict::Accept);
    }

    void rejectsPasswordManagerHints()
    {
        const ClipFilter f(1024);
        const QStringList keepass = {QStringLiteral("text/plain"), QStringLiteral("x-kde-passwordManagerHint")};
        QVERIFY(f.check(QStringLiteral("hunter2"), keepass) == Verdict::Sensitive);
        QVERIFY(ClipFilter::isSensitive({QStringLiteral("application/x-nspasteboard-concealed-type")}));
        QVERIFY(!ClipFilter::isSensitive({QStringLiteral("text/plain"), QStringLiteral("text/html")}));
        // Windows markers, as NativeClipboard reports them.
        QVERIFY(ClipFilter::isSensitive({QStringLiteral("text/plain"), QStringLiteral("ExcludeClipboardContentFromMonitorProcessing")}));
        QVERIFY(ClipFilter::isSensitive({QStringLiteral("text/plain"), QStringLiteral("CanIncludeInClipboardHistory=0")}));
        QVERIFY(ClipFilter::isSensitive({QStringLiteral("text/plain"), QStringLiteral("CanUploadToCloudClipboard=0")}));
        QVERIFY(ClipFilter::isSensitive({QStringLiteral("Clipboard Viewer Ignore")}));
        QVERIFY(ClipFilter::isSensitive(
            {QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\"")}));
        QVERIFY(!ClipFilter::isSensitive({QStringLiteral("application/x-qt-windows-mime;value=\"Rich Text Format\"")}));
        // An empty text with a marker is still Sensitive (the Windows reader skips the text).
        QVERIFY(ClipFilter(1024).check(QString(), {QStringLiteral("CanIncludeInClipboardHistory=0")}) == Verdict::Sensitive);
    }

    void ignoresOwnWriteEchoWithinWindow()
    {
        qint64 now = 1000;
        ClipFilter f(1024, [&now] { return now; });
        f.noteOwnWrite(QStringLiteral("pasted"));

        now += 100;
        QVERIFY(f.check(QStringLiteral("pasted"), {}) == Verdict::OwnWrite);
        QVERIFY(f.check(QStringLiteral("something else"), {}) == Verdict::Accept);

        now += ClipFilter::ownWriteWindowMs;
        QVERIFY(f.check(QStringLiteral("pasted"), {}) == Verdict::Accept);
    }
};

QTEST_GUILESS_MAIN(TestClipFilter)
#include "tst_clipfilter.moc"
