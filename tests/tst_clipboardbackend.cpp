#include "QtClipboardBackend.h"

#include <QGuiApplication>
#include <QMimeData>
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
