#include "QtClipboardBackend.h"

#include <QGuiApplication>
#include <QDir>
#include <QMimeData>
#include <QUrl>
#include <QSignalSpy>
#include <QBuffer>
#include <QImage>
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
    NativeClipboardRead read;
    read.status = NativeClipboardRead::Status::Busy;
    return read;
}

NativeClipboardRead ok(const QString& text, const QStringList& formats = {QStringLiteral("text/plain")})
{
    NativeClipboardRead read;
    read.status = NativeClipboardRead::Status::Ok;
    read.text = text;
    read.formats = formats;
    return read;
}

// A picture as the Windows native reader returns it (bytes plus their type).
NativeClipboardRead picture(const QByteArray& data, const QString& type, const QStringList& formats = {})
{
    NativeClipboardRead read;
    read.status = NativeClipboardRead::Status::Ok;
    read.image = data;
    read.imageType = type;
    read.formats = formats;
    return read;
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
        data->setData(QStringLiteral("text/html"), "<b>copied</b> text");
        QGuiApplication::clipboard()->setMimeData(data);

        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("copied text"));
        QVERIFY(captured.at(0).at(1).toStringList().contains(QStringLiteral("text/html")));
        QCOMPARE(captured.at(0).at(2).toString(), QStringLiteral("<b>copied</b> text")); // the formatted version too
    }

    void copiedFilesAreRecordedAsTheirPaths()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        auto* data = new QMimeData;
        data->setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a b.txt")), QUrl::fromLocalFile(QStringLiteral("/tmp/c.txt")),
                       QUrl(QStringLiteral("https://example.org/x"))});
        QGuiApplication::clipboard()->setMimeData(data);
        QTRY_COMPARE(captured.size(), 1);
        const QString expected = QDir::toNativeSeparators(QStringLiteral("/tmp/a b.txt")) + QLatin1Char('\n')
            + QDir::toNativeSeparators(QStringLiteral("/tmp/c.txt"));
        QCOMPARE(captured.at(0).at(0).toString(), expected); // web links are not files

        // A password manager's marker wins over everything.
        auto* marked = new QMimeData;
        marked->setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/secret.kdbx"))});
        marked->setData(QStringLiteral("x-kde-passwordManagerHint"), "secret");
        QGuiApplication::clipboard()->setMimeData(marked);
        QTRY_COMPARE(captured.size(), 2);
        QVERIFY(captured.at(1).at(0).toString().isEmpty()); // only the markers, as for any marked copy
    }

    void formattedVersionIsLeftOutWhenOffOrWhenItIsForAnotherText()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.setReadRichText(false);
        backend.start();
        auto* off = new QMimeData;
        off->setText(QStringLiteral("plain"));
        off->setHtml(QStringLiteral("<b>plain</b>"));
        QGuiApplication::clipboard()->setMimeData(off);
        QTRY_COMPARE(captured.size(), 1);
        QVERIFY(captured.at(0).at(2).toString().isEmpty());
        backend.stop();

        // A native read whose text isn't the one now on the clipboard (it changed in between): no HTML is
        // taken from the other copy.
        QtClipboardBackend native(QGuiApplication::clipboard(), false, nullptr, [] { return ok(QStringLiteral("older")); });
        QSignalSpy nativeCaptured(&native, &QtClipboardBackend::captured);
        native.start();
        auto* newer = new QMimeData;
        newer->setText(QStringLiteral("newer"));
        newer->setHtml(QStringLiteral("<b>newer</b>"));
        QGuiApplication::clipboard()->setMimeData(newer);
        QTRY_COMPARE(nativeCaptured.size(), 1);
        QCOMPARE(nativeCaptured.at(0).at(0).toString(), QStringLiteral("older"));
        QVERIFY(nativeCaptured.at(0).at(2).toString().isEmpty());
    }

    void markedTextIsReadOnlyWhenPasswordsAreKept()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        const auto copyMarked = [](const QString& text) {
            auto* data = new QMimeData;
            data->setText(text);
            data->setData(QStringLiteral("x-kde-passwordManagerHint"), "secret");
            QGuiApplication::clipboard()->setMimeData(data);
        };

        copyMarked(QStringLiteral("hunter2"));
        QTRY_COMPARE(captured.size(), 1);
        QVERIFY(captured.at(0).at(0).toString().isEmpty()); // only the markers
        QVERIFY(captured.at(0).at(1).toStringList().contains(QStringLiteral("x-kde-passwordManagerHint")));

        backend.setReadMarkedText(true);
        copyMarked(QStringLiteral("hunter3"));
        QTRY_COMPARE(captured.size(), 2);
        QCOMPARE(captured.at(1).at(0).toString(), QStringLiteral("hunter3"));
        QVERIFY(captured.at(1).at(1).toStringList().contains(QStringLiteral("x-kde-passwordManagerHint")));
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

    void aChangeCounterReplacesTheClipboardSignal()
    {
        // macOS: no change event reaches a background program, so a counter is polled. Only
        // changes after start count, and QClipboard's own signal is not used.
        ScriptedReader reader;
        *reader.script << ok(QStringLiteral("first")) << ok(QStringLiteral("second"));
        auto count = std::make_shared<qint64>(10);
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader,
                                   [count] { return *count; });
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();

        QTest::qWait(QtClipboardBackend::kPollMs * 2);
        QCOMPARE(captured.size(), 0); // what was there already is not a copy
        QCOMPARE(*reader.calls, 0);
        QGuiApplication::clipboard()->setText(QStringLiteral("qt signal"));
        QTest::qWait(QtClipboardBackend::kPollMs * 2);
        QCOMPARE(*reader.calls, 0); // the counter didn't move: nothing read

        ++*count;
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("first"));
        QTest::qWait(QtClipboardBackend::kPollMs * 3);
        QCOMPARE(captured.size(), 1); // an unchanged counter is not read again

        *count += 5; // several copies between two polls are one read of the latest
        QTRY_COMPARE(captured.size(), 2);
        QCOMPARE(captured.at(1).at(0).toString(), QStringLiteral("second"));
        QCOMPARE(*reader.calls, 2);

        backend.stop();
        ++*count;
        QTest::qWait(QtClipboardBackend::kPollMs * 2);
        QCOMPARE(*reader.calls, 2); // stopped: no polling
    }

    void aCounterThatSaysMinusOneUsesTheClipboardSignal()
    {
        ScriptedReader reader;
        *reader.script << ok(QStringLiteral("by signal"));
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader, [] { return qint64(-1); });
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        backend.start();
        QGuiApplication::clipboard()->setText(QStringLiteral("x"));
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(captured.at(0).at(0).toString(), QStringLiteral("by signal"));
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

    void nativePictureBytesAreEncodedOneAtATime()
    {
        // The Windows reader hands over the picture's bytes; a burst of copies keeps the
        // first (already encoding) and the newest, not every one in between.
        auto pngOf = [](QColor color) {
            QImage image(50, 40, QImage::Format_RGB32);
            image.fill(color);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return png;
        };
        ScriptedReader reader;
        *reader.script << picture(pngOf(Qt::red), QStringLiteral("image/png"), {QStringLiteral("PNG")})
                       << picture(pngOf(Qt::green), QStringLiteral("image/png"))
                       << picture(pngOf(Qt::blue), QStringLiteral("image/png"));
        QtClipboardBackend backend(QGuiApplication::clipboard(), false, nullptr, reader);
        QSignalSpy images(&backend, &QtClipboardBackend::capturedImage);
        backend.start();
        const QDateTime before = QDateTime::currentDateTimeUtc();
        for (int i = 0; i < 3; ++i) {
            QGuiApplication::clipboard()->setText(QStringLiteral("change %1").arg(i)); // any change
        }
        QTRY_COMPARE(images.size(), 2);
        QTest::qWait(200);
        QCOMPARE(images.size(), 2);
        const auto first = images.at(0).at(0).value<clipitto::ImageClip>();
        const auto last = images.at(1).at(0).value<clipitto::ImageClip>();
        QCOMPARE(QImage::fromData(first.png, "PNG").pixelColor(1, 1), QColor(Qt::red));
        QCOMPARE(QImage::fromData(last.png, "PNG").pixelColor(1, 1), QColor(Qt::blue));
        QCOMPARE(images.at(0).at(1).toStringList(), QStringList{QStringLiteral("PNG")});
        // Stamped when copied, not when the encode finished.
        QVERIFY(first.copiedAt >= before && first.copiedAt <= last.copiedAt);
    }

    void emitsCopiedPicturesWhenThereIsNoText()
    {
        QtClipboardBackend backend(QGuiApplication::clipboard(), false);
        QSignalSpy captured(&backend, &QtClipboardBackend::captured);
        QSignalSpy images(&backend, &QtClipboardBackend::capturedImage);
        backend.start();

        QImage picture(120, 80, QImage::Format_RGB32);
        picture.fill(QColor(10, 200, 30));
        QGuiApplication::clipboard()->setImage(picture);
        QTRY_COMPARE(images.size(), 1); // encoded off the GUI thread, then reported
        const auto clip = images.at(0).at(0).value<clipitto::ImageClip>();
        QCOMPARE(clip.width, 120);
        QCOMPARE(clip.height, 80);
        QCOMPARE(QImage::fromData(clip.png, "PNG").pixelColor(5, 5), QColor(10, 200, 30));
        QCOMPARE(captured.size(), 0);

        // Text and a picture together: text wins (e.g. spreadsheet cells).
        auto* both = new QMimeData;
        both->setText(QStringLiteral("cells"));
        both->setImageData(picture);
        QGuiApplication::clipboard()->setMimeData(both);
        QTRY_COMPARE(captured.size(), 1);
        QTest::qWait(200);
        QCOMPARE(images.size(), 1);
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
