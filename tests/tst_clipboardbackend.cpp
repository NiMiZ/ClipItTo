#include "QtClipboardBackend.h"

#include <QGuiApplication>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>

#include <memory>

using clipitto::capture::NativeClipboardRead;
using clipitto::capture::QtClipboardBackend;

namespace {

// A native reader that returns the scripted results in turn (the last one repeats).
struct ScriptedReader {
    std::shared_ptr<QList<NativeClipboardRead>> script = std::make_shared<QList<NativeClipboardRead>>();
    std::shared_ptr<int> calls = std::make_shared<int>(0);

    NativeClipboardRead operator()() const
    {
        ++*calls;
        return script->size() > 1 ? script->takeFirst() : script->first();
    }
};

NativeClipboardRead busy()
{
    return {NativeClipboardRead::Status::Busy, {}, {}};
}

NativeClipboardRead ok(const QString& text, const QStringList& formats = {QStringLiteral("text/plain")})
{
    return {NativeClipboardRead::Status::Ok, text, formats};
}

} // namespace

// Runs on the offscreen platform, whose in-process clipboard emits the same
// QClipboard signals the xcb and windows platforms emit for external changes.
class TestClipboardBackend : public QObject {
    Q_OBJECT

private slots:
    void emitsTextAndMimeTypes()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();

        auto* data = new QMimeData;
        data->setText(QStringLiteral("copied text"));
        data->setData(QStringLiteral("x-kde-passwordManagerHint"), "secret");
        QGuiApplication::clipboard()->setMimeData(data);

        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("copied text"));
        QVERIFY(captured.at(0).at(1).toStringList().contains(QStringLiteral("x-kde-passwordManagerHint")));
    }

    void ignoresNonText()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();

        auto* data = new QMimeData;
        data->setData(QStringLiteral("image/png"), QByteArray(16, '\0'));
        QGuiApplication::clipboard()->setMimeData(data);
        QCoreApplication::processEvents();
        QCOMPARE(captured.size(), 0);
    }

    void nativeTextReplacesQtsText()
    {
        // Windows: the native reader's CF_UNICODETEXT wins over what Qt would report.
        ScriptedReader reader;
        *reader.script << ok(QStringLiteral("שלום עולם"));
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        QGuiApplication::clipboard()->setText(QStringLiteral("?????"));
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("שלום עולם"));
    }

    void busyTextIsReadAgainShortly()
    {
        ScriptedReader reader;
        *reader.script << busy() << busy() << ok(QStringLiteral("ready now"));
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        QGuiApplication::clipboard()->setText(QStringLiteral("x"));
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("ready now"));
        QCOMPARE(*reader.calls, 3);
    }

    void textThatNeverArrivesIsSkippedNotGuessed()
    {
        ScriptedReader reader;
        *reader.script << busy();
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        QGuiApplication::clipboard()->setText(QStringLiteral("x"));
        QTRY_COMPARE(*reader.calls, 1 + QtClipboardBackend::kMaxRetries);
        QTest::qWait(700); // past the last retry (20+40+80+160+320 ms)
        QCOMPARE(*reader.calls, 1 + QtClipboardBackend::kMaxRetries);
        QCOMPARE(captured.size(), 0); // no fallback to Qt's text
    }

    void unsupportedNativeReaderFallsBackToQt()
    {
        ScriptedReader reader;
        *reader.script << NativeClipboardRead{};
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        QGuiApplication::clipboard()->setText(QStringLiteral("via Qt"));
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("via Qt"));
        QGuiApplication::clipboard()->setText(QStringLiteral("again"));
        QTRY_COMPARE(captured.size(), 2);
        QCOMPARE(*reader.calls, 1); // asked once, then Qt only
    }

    void stopDisconnects()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        backend.stop();
        QGuiApplication::clipboard()->setText(QStringLiteral("after stop"));
        QCoreApplication::processEvents();
        QCOMPARE(captured.size(), 0);
    }
};

QTEST_MAIN(TestClipboardBackend)
#include "tst_clipboardbackend.moc"
