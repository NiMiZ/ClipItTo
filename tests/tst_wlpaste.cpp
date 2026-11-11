#include "CapturePipeline.h"
#include "Previews.h"
#include "TestDb.h"
#include "WlPasteBackend.h"

#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::capture::WlPasteBackend;

// Drives WlPasteBackend against fake-wl-paste.sh, which emulates wl-paste's CLI.
class TestWlPaste : public QObject {
    Q_OBJECT

private:
    const QString m_program = QStringLiteral(CLIPITTO_FAKE_WLPASTE);
    QTemporaryDir m_dir;

    void write(const QString& name, const QByteArray& content = {})
    {
        QFile file(m_dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(content);
    }

    void clipboard(const QByteArray& text, const QByteArray& types = "text/plain;charset=utf-8\ntext/plain\n")
    {
        write(QStringLiteral("types"), types);
        write(QStringLiteral("text"), text);
    }

    QString lastText(const QSignalSpy& spy) const { return spy.isEmpty() ? QString() : spy.last().at(0).toString(); }

    // A backend wired to a pipeline and an in-memory database, as the daemon wires them.
    struct Rig {
        TestDb db;
        clipitto::CapturePipeline pipe;
        WlPasteBackend backend;
        QSignalSpy stored;
        QSignalSpy rejected;

        explicit Rig(const QString& program)
            : pipe(*db.repo, 1000, 1024 * 1024)
            , backend(program)
            , stored(&pipe, &clipitto::CapturePipeline::stored)
            , rejected(&pipe, &clipitto::CapturePipeline::rejected)
        {
            QObject::connect(&backend, &WlPasteBackend::captured, &pipe, &clipitto::CapturePipeline::process);
            QObject::connect(&backend, &WlPasteBackend::capturedImage, &pipe, &clipitto::CapturePipeline::processImage);
        }
        clipitto::Entry newest() const { return db.repo->recent(1, false).front(); }
        clipitto::ClipFilter::Verdict lastVerdict() const
        {
            return rejected.last().at(0).value<clipitto::ClipFilter::Verdict>();
        }
    };

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        for (const QString& f : QDir(m_dir.path()).entryList(QDir::Files)) {
            QFile::remove(m_dir.filePath(f));
        }
        qputenv("FAKE_WLPASTE_DIR", QFile::encodeName(m_dir.path()));
    }

    void requestsUtf8NotLatin1()
    {
        // An X11 app (through Xwayland) listing the Latin-1 STRING target first: asking
        // wl-paste for just "text" could pick it and turn Hebrew into '?'.
        QCOMPARE(WlPasteBackend::textType({"TEXT", "STRING", "UTF8_STRING", "text/plain"}), QStringLiteral("UTF8_STRING"));
        QCOMPARE(WlPasteBackend::textType({"text/plain", "text/plain;charset=utf-8"}), QStringLiteral("text/plain;charset=utf-8"));
        QCOMPARE(WlPasteBackend::textType({"STRING", "text/plain"}), QStringLiteral("text/plain"));
        QCOMPARE(WlPasteBackend::textType({"text/html", "STRING"}), QStringLiteral("text")); // let wl-paste decide

        clipboard("שלום", "STRING\nUTF8_STRING\n");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(lastText(captured), QStringLiteral("שלום"));
        QFile reads(m_dir.filePath(QStringLiteral("reads")));
        QVERIFY(reads.open(QIODevice::ReadOnly));
        QCOMPARE(reads.readAll().trimmed(), QByteArray("UTF8_STRING"));
    }

    void fetchesPicturesWhenThereIsNoPlainText()
    {
        QImage picture(64, 32, QImage::Format_RGB32);
        picture.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
        write(QStringLiteral("image"), png);
        // A browser's "Copy Image": text/html plus the picture, no plain text.
        write(QStringLiteral("types"), "text/html\nimage/jpeg\nimage/png\n");

        QCOMPARE(WlPasteBackend::imageType({"text/html", "image/jpeg", "image/png"}), QStringLiteral("image/png"));
        QVERIFY(WlPasteBackend::imageType({"text/plain"}).isEmpty());

        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        QSignalSpy images(&backend, &WlPasteBackend::capturedImage);
        backend.start();
        QTRY_COMPARE(images.size(), 1);
        const auto clip = images.at(0).at(0).value<clipitto::ImageClip>();
        QCOMPARE(QSize(clip.width, clip.height), QSize(64, 32));
        QCOMPARE(captured.size(), 0);
        QFile reads(m_dir.filePath(QStringLiteral("reads")));
        QVERIFY(reads.open(QIODevice::ReadOnly));
        QCOMPARE(reads.readAll().trimmed(), QByteArray("image/png")); // PNG preferred
    }

    void probeDetectsDataControl()
    {
        QVERIFY(WlPasteBackend::probe(m_program));
        write(QStringLiteral("unsupported"));
        QVERIFY(!WlPasteBackend::probe(m_program));
        QVERIFY(!WlPasteBackend::probe(QStringLiteral("/nonexistent/wl-paste")));
    }

    void capturesCurrentThenLaterChanges()
    {
        clipboard("first");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();

        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(lastText(captured), QStringLiteral("first"));
        QVERIFY(captured.last().at(1).toStringList().contains(QStringLiteral("text/plain")));

        clipboard("second\nline");
        write(QStringLiteral("event"));
        QTRY_COMPARE(captured.size(), 2);
        QCOMPARE(lastText(captured), QStringLiteral("second\nline"));
    }

    void neverReadsSecrets()
    {
        clipboard("hunter2", "text/plain\nx-kde-passwordManagerHint\n");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        // Only the markers are reported (so the daemon can say why nothing was kept).
        QTRY_COMPARE(captured.size(), 1);
        QVERIFY(captured.at(0).at(0).toString().isEmpty());
        QVERIFY(captured.at(0).at(1).toStringList().contains(QStringLiteral("x-kde-passwordManagerHint")));
        QTest::qWait(200);
        QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("reads"))));
    }

    void readsSecretsWhenPasswordsAreKept()
    {
        clipboard("hunter2", "text/plain\nx-kde-passwordManagerHint\n");
        WlPasteBackend backend(m_program);
        backend.setReadMarkedText(true);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(lastText(captured), QStringLiteral("hunter2"));
        QVERIFY(captured.at(0).at(1).toStringList().contains(QStringLiteral("x-kde-passwordManagerHint")));
    }

    void reportsTheFormattedVersionOfACopy()
    {
        clipboard("bold word", "text/plain;charset=utf-8\ntext/html\n");
        write(QStringLiteral("html"), "<b>bold</b> word");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(lastText(captured), QStringLiteral("bold word"));
        QCOMPARE(captured.at(0).at(2).toString(), QStringLiteral("<b>bold</b> word"));

        // Turned off (keep_formatting): the HTML is not even requested.
        QFile::remove(m_dir.filePath(QStringLiteral("reads")));
        backend.setReadRichText(false);
        write(QStringLiteral("text"), "second");
        write(QStringLiteral("event"));
        QTRY_COMPARE(captured.size(), 2);
        QVERIFY(captured.at(1).at(2).toString().isEmpty());
        QFile reads(m_dir.filePath(QStringLiteral("reads")));
        QVERIFY(reads.open(QIODevice::ReadOnly));
        QVERIFY(!reads.readAll().contains("text/html"));
    }

    void neverReadsTheFormattedVersionOfAPassword()
    {
        clipboard("hunter2", "text/plain\ntext/html\nx-kde-passwordManagerHint\n");
        write(QStringLiteral("html"), "<b>hunter2</b>");
        WlPasteBackend backend(m_program);
        backend.setReadMarkedText(true);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);
        QCOMPARE(lastText(captured), QStringLiteral("hunter2"));
        QVERIFY(captured.at(0).at(2).toString().isEmpty());
    }

    void skipsNonText()
    {
        clipboard("\x89PNG", "image/png\n");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTest::qWait(300);
        QCOMPARE(captured.size(), 0);
    }

    // ---- Copies from many apps, through wl-paste and the pipeline ----

    void firefoxAndLibreOfficeOnWaylandKeepTheirFormatting_data()
    {
        QTest::addColumn<QByteArray>("types");
        QTest::addColumn<QByteArray>("html");
        QTest::addColumn<QString>("text");
        QTest::newRow("Firefox (html with meta charset, moz types)")
            << QByteArray("text/html\ntext/_moz_htmlcontext\ntext/_moz_htmlinfo\ntext/plain;charset=utf-8\ntext/plain\n")
            << QByteArray("<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\"><b>Bold</b> and "
                          "<a href=\"https://example.org\">linked</a>")
            << QStringLiteral("Bold and linked");
        QTest::newRow("LibreOffice (rtf and html offered)")
            << QByteArray("text/rtf\napplication/rtf\ntext/html\ntext/plain;charset=utf-8\n")
            << QByteArray("<!DOCTYPE HTML><html><head><meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\"/></head>"
                          "<body><p style=\"font-style: italic\">Meeting notes: ship on Friday</p></body></html>")
            << QStringLiteral("Meeting notes: ship on Friday");
    }
    void firefoxAndLibreOfficeOnWaylandKeepTheirFormatting()
    {
        QFETCH(QByteArray, types);
        QFETCH(QByteArray, html);
        QFETCH(QString, text);
        clipboard(text.toUtf8(), types);
        write(QStringLiteral("html"), html);
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, text);
        QVERIFY(rig.newest().rich);
        QCOMPARE(rig.db.repo->richText(rig.newest().id), QString::fromUtf8(html));
        QFile reads(m_dir.filePath(QStringLiteral("reads")));
        QVERIFY(reads.open(QIODevice::ReadOnly));
        QVERIFY(!reads.readAll().contains("rtf")); // only plain text and html are fetched
    }

    void nautilusFileCopyIsStoredAsItsPaths()
    {
        // Nautilus offers x-special/gnome-copied-files and text/uri-list (CRLF separated), no plain text.
        clipboard("file:///tmp/ClipItTo%20a%20b.txt\r\nfile:///tmp/c.txt\r\n", "x-special/gnome-copied-files\ntext/uri-list\n");
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(clipitto::preview::filePaths(rig.newest().content),
                 (QStringList{QStringLiteral("/tmp/ClipItTo a b.txt"), QStringLiteral("/tmp/c.txt")}));
    }

    void aMarkedCopyThroughWlPasteIsNeverStoredOrRead()
    {
        clipboard("c0rrect-h0rse", "text/plain\ntext/html\nx-kde-passwordManagerHint\n");
        write(QStringLiteral("html"), "<b>c0rrect-h0rse</b>");
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.rejected.size(), 1);
        QCOMPARE(rig.lastVerdict(), clipitto::ClipFilter::Verdict::Sensitive);
        QCOMPARE(rig.db.repo->count(), 0);
        QVERIFY(!QFile::exists(m_dir.filePath(QStringLiteral("reads"))));
    }

    void aScreenshotWithOnlyAPngIsStored()
    {
        QImage picture(200, 100, QImage::Format_RGB32);
        picture.fill(QColor(30, 90, 200));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
        write(QStringLiteral("image"), png);
        write(QStringLiteral("types"), "image/png\n");
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.stored.size(), 1);
        QVERIFY(rig.newest().image);
        QCOMPARE(rig.newest().imageWidth, 200);
        QCOMPARE(rig.db.repo->count(), 1);
    }

    void terminalTextWithHebrewAndEmojiIsStoredIntact()
    {
        const QString text = QString::fromUtf8("$ echo \"שלום \xF0\x9F\x98\x80\"\nשלום \xF0\x9F\x98\x80");
        clipboard(text.toUtf8(), "text/plain;charset=utf-8\n");
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, text);
        QVERIFY(!rig.newest().rich);
    }

    void theOneMegabyteCapAppliesToWlPasteText_data()
    {
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("kept");
        QTest::newRow("1 MiB") << 1024 * 1024 << true;
        QTest::newRow("1 MiB and a byte") << 1024 * 1024 + 1 << false;
    }
    void theOneMegabyteCapAppliesToWlPasteText()
    {
        QFETCH(int, size);
        QFETCH(bool, kept);
        clipboard(QByteArray(size, 'a'));
        Rig rig(m_program);
        rig.backend.start();
        if (kept) {
            QTRY_COMPARE_WITH_TIMEOUT(rig.stored.size(), 1, 10000);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(rig.rejected.size(), 1, 10000);
            QCOMPARE(rig.lastVerdict(), clipitto::ClipFilter::Verdict::TooLarge);
            QCOMPARE(rig.db.repo->count(), 0);
        }
    }

    void blankTextAndOurOwnEchoAreSkipped()
    {
        clipboard("  \n\t ");
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.rejected.size(), 1);
        QCOMPARE(rig.lastVerdict(), clipitto::ClipFilter::Verdict::Empty);

        rig.pipe.filter().noteOwnWrite(QStringLiteral("pasted from history"));
        clipboard("pasted from history");
        write(QStringLiteral("event"));
        QTRY_COMPARE(rig.rejected.size(), 2);
        QCOMPARE(rig.lastVerdict(), clipitto::ClipFilter::Verdict::OwnWrite);
        QCOMPARE(rig.db.repo->count(), 0);
    }

    void aBurstOfCopiesEndsWithTheLastOne()
    {
        clipboard("copy 0");
        Rig rig(m_program);
        rig.backend.start();
        QTRY_COMPARE(rig.stored.size(), 1);
        for (int i = 1; i <= 8; ++i) {
            clipboard(QByteArray("copy ") + QByteArray::number(i));
            write(QStringLiteral("event"));
            QTest::qWait(30);
        }
        QTRY_COMPARE(rig.newest().content, QStringLiteral("copy 8"));
        QVERIFY(rig.db.repo->count() >= 2); // changes may coalesce, but the last copy always wins
    }

    void restartsWhenTheWatcherExits()
    {
        clipboard("before");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);

        clipboard("after restart");
        write(QStringLiteral("die"));
        // Restarted after a 500 ms backoff; the new watcher reports the current content.
        QTRY_COMPARE_WITH_TIMEOUT(lastText(captured), QStringLiteral("after restart"), 5000);
    }

    void stopEndsCapture()
    {
        clipboard("one");
        WlPasteBackend backend(m_program);
        QSignalSpy captured(&backend, &WlPasteBackend::captured);
        backend.start();
        QTRY_COMPARE(captured.size(), 1);
        backend.stop();
        clipboard("two");
        write(QStringLiteral("event"));
        QTest::qWait(200);
        QCOMPARE(captured.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestWlPaste)
#include "tst_wlpaste.moc"
