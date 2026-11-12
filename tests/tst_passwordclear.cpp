// Taking a pasted password off the clipboard again (ClipboardWriter::clearPassword) against the Qt clipboard of the
// offscreen platform: only when the clipboard still holds that very paste. No clipboard contents are printed.
// What these cannot show (the real Windows, X11, Wayland and macOS clipboards) is listed in docs/untested.md.
#include "ClipFilter.h"
#include "ClipboardWriter.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QTest>

using clipitto::ClipboardWriter;
using clipitto::ClipFilter;
using Result = ClipboardWriter::ClearResult;

class TestPasswordClear : public QObject {
    Q_OBJECT

    static QClipboard* clip() { return QGuiApplication::clipboard(); }

private slots:
    void init() { clip()->clear(); }

    void clearsWhatItPasted()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        QCOMPARE(clip()->text(), QStringLiteral("hunter2-pasted"));
        QVERIFY(filter.ownPasswordPending());
        QVERIFY(writer.clearPassword() == Result::Cleared);
        QVERIFY(clip()->text().isEmpty());
        QVERIFY(!filter.ownPasswordPending());
        // Once is enough: a second call finds nothing waiting.
        QVERIFY(writer.clearPassword() == Result::NothingPending);
    }

    void theClearIsNotACopy()
    {
        // What the capture would report after the clear: an empty clipboard. Never a history entry, and the late
        // report of the paste itself is still taken for our own.
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        QVERIFY(writer.clearPassword() == Result::Cleared);
        QVERIFY(filter.check(QString(), {}) == ClipFilter::Verdict::Empty);
        QVERIFY(filter.check(QStringLiteral("hunter2-pasted"), {}) == ClipFilter::Verdict::OwnWrite);
        // The user's own next copy is a copy as usual.
        QVERIFY(filter.check(QStringLiteral("the next thing"), {}) == ClipFilter::Verdict::Accept);
    }

    void leavesANewerCopyAlone_theCaptureSawIt()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        clip()->setText(QStringLiteral("copied after"));
        QVERIFY(filter.check(QStringLiteral("copied after"), {}) == ClipFilter::Verdict::Accept); // the backend reports it
        QVERIFY(writer.clearPassword() == Result::NothingPending);
        QCOMPARE(clip()->text(), QStringLiteral("copied after"));
    }

    void leavesANewerCopyAlone_theCaptureHasNotSeenItYet()
    {
        // The clear fires between the user's copy and the capture's report of it: the clipboard itself says no.
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        clip()->setText(QStringLiteral("copied after"));
        QVERIFY(filter.ownPasswordPending());
        QVERIFY(writer.clearPassword() == Result::Changed);
        QCOMPARE(clip()->text(), QStringLiteral("copied after"));
        QVERIFY(!filter.ownPasswordPending()); // given up on: not tried again
    }

    void leavesANewerPictureAlone()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(Qt::red);
        clip()->setImage(image);
        QVERIFY(writer.clearPassword() == Result::Changed);
        QVERIFY(!clip()->image().isNull());
    }

    void leavesANewerCopyOfTheSameLookingNothingButOtherText()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        clip()->setText(QStringLiteral("hunter2-pasted-but-longer"));
        QVERIFY(writer.clearPassword() == Result::Changed);
        QCOMPARE(clip()->text(), QStringLiteral("hunter2-pasted-but-longer"));
    }

    void plainTextPastesAreNeverCleared()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setText(QStringLiteral("an ordinary copy"));
        QVERIFY(writer.clearPassword() == Result::NothingPending);
        QCOMPARE(clip()->text(), QStringLiteral("an ordinary copy"));
        // And an ordinary paste after a password paste ends the wait for the password.
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        writer.setText(QStringLiteral("an ordinary copy"));
        QVERIFY(writer.clearPassword() == Result::Changed); // the clipboard says it is not the password any more
        QCOMPARE(clip()->text(), QStringLiteral("an ordinary copy"));
    }

    void nothingPastedNothingToClear()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        clip()->setText(QStringLiteral("something"));
        QVERIFY(writer.clearPassword() == Result::NothingPending);
        QCOMPARE(clip()->text(), QStringLiteral("something"));
    }

    void pastingTheSamePasswordAgainRestartsTheWait()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        QVERIFY(writer.clearPassword() == Result::Cleared);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        QVERIFY(filter.ownPasswordPending());
        QVERIFY(writer.clearPassword() == Result::Cleared);
        QVERIFY(clip()->text().isEmpty());
    }

    void thePasteStillCarriesTheMarkers()
    {
        ClipFilter filter(1024 * 1024);
        ClipboardWriter writer(clip(), filter);
        writer.setPassword(QStringLiteral("hunter2-pasted"));
        // Marked the platform's way, so other managers and Windows' own history skip it (as before).
        QVERIFY(ClipFilter::isSensitive(clip()->mimeData()->formats()));
    }
};

QTEST_MAIN(TestPasswordClear)
#include "tst_passwordclear.moc"
