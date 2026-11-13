// Copies as many apps make them: the data each app puts on the clipboard (MIME types, Windows formats), driven
// through the real capture path (QtClipboardBackend, the Windows reader's results), CapturePipeline and the
// stored entry. The clipboard is Qt's in-process one (offscreen platform), never the real one.
#include "CapturePipeline.h"
#include "ClipboardWriter.h"
#include "PasswordVault.h"
#include "Previews.h"
#include "QtClipboardBackend.h"
#include "TestDb.h"

#include <QBuffer>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <memory>

using clipitto::CapturePipeline;
using clipitto::ClipFilter;
using clipitto::capture::NativeClipboardRead;
using clipitto::capture::QtClipboardBackend;

namespace {

QClipboard* clip()
{
    return QGuiApplication::clipboard();
}

// A backend, a pipeline and a database wired as the daemon wires them.
struct Rig {
    TestDb db;
    CapturePipeline pipe;
    QtClipboardBackend backend;
    QSignalSpy stored;
    QSignalSpy rejected;

    explicit Rig(QtClipboardBackend::NativeReader reader = {}, qint64 maxBytes = 1024 * 1024)
        : pipe(*db.repo, 1000, maxBytes)
        , backend(clip(), false, nullptr, std::move(reader))
        , stored(&pipe, &CapturePipeline::stored)
        , rejected(&pipe, &CapturePipeline::rejected)
    {
        QObject::connect(&backend, &QtClipboardBackend::captured, &pipe, &CapturePipeline::process);
        QObject::connect(&backend, &QtClipboardBackend::capturedImage, &pipe, &CapturePipeline::processImage);
        backend.start();
    }

    // Nothing more is coming: gives late events time to show up.
    static void settle() { QTest::qWait(120); }

    clipitto::Entry newest() const { return db.repo->recent(1, false).front(); }
    ClipFilter::Verdict lastVerdict() const { return rejected.last().at(0).value<ClipFilter::Verdict>(); }
};

QMimeData* mime(const QString& text, const QVariantMap& formats = {})
{
    auto* data = new QMimeData;
    if (!text.isNull()) {
        data->setText(text);
    }
    for (auto it = formats.begin(); it != formats.end(); ++it) {
        data->setData(it.key(), it.value().toByteArray());
    }
    return data;
}

NativeClipboardRead windowsRead(const QString& text, const QStringList& formats)
{
    NativeClipboardRead read;
    read.status = NativeClipboardRead::Status::Ok;
    read.text = text;
    read.formats = formats;
    return read;
}

QByteArray pngOf(QColor color, QSize size = {64, 48})
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

QString tempFile(const QString& name)
{
    return QDir::tempPath() + QLatin1Char('/') + name;
}

} // namespace

class TestCopyApps : public QObject {
    Q_OBJECT

private slots:
    // ---- Office, browsers, editors, terminal ----

    void appCopies_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("html");
        QTest::addColumn<QVariantMap>("extra");
        QTest::addColumn<bool>("rich");

        QTest::newRow("Word (html with meta charset, rtf)")
            << "Quarterly report draft"
            << "<html xmlns:o=\"urn:schemas-microsoft-com:office:office\"><head><meta http-equiv=Content-Type "
               "content=\"text/html; charset=utf-8\"><meta name=ProgId content=Word.Document></head><body lang=EN-US>"
               "<!--StartFragment--><p class=MsoNormal><b>Quarterly</b> report draft</p><!--EndFragment--></body></html>"
            << QVariantMap{{"text/rtf", QByteArray("{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Calibri;}}\\b Quarterly\\b0  report draft}")}}
            << true;
        QTest::newRow("LibreOffice Writer (html, rtf)")
            << "Meeting notes: ship on Friday"
            << "<!DOCTYPE HTML><html><head><meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\"/>"
               "<meta name=\"generator\" content=\"LibreOffice 24.2\"/></head><body><p style=\"font-style: italic\">"
               "Meeting notes: ship on Friday</p></body></html>"
            << QVariantMap{{"application/rtf", QByteArray("{\\rtf1\\ansi\\i Meeting notes: ship on Friday}")},
                           {"application/x-libreoffice-internal-id-12345", QByteArray("1")}}
            << true;
        QTest::newRow("Chrome, plain text (only a wrapper)")
            << "just some words"
            << "<meta charset='utf-8'><span style=\"color: rgb(32, 33, 36); font-family: Roboto, arial; font-size: 14px;\">"
               "just some words</span>"
            << QVariantMap{} << false;
        QTest::newRow("Chrome, text with a link")
            << "read the docs here"
            << "<meta charset='utf-8'><span>read the docs </span><a href=\"https://example.org/docs\">here</a>"
            << QVariantMap{{"chromium/x-source-url", QByteArray("https://example.org/page")}} << true;
        QTest::newRow("Firefox (html with moz context)")
            << "Bold and linked"
            << "<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\"><strong>Bold</strong> and "
               "<a href=\"https://example.org\">linked</a>"
            << QVariantMap{{"text/_moz_htmlcontext", QByteArray("<html><body>")}, {"text/_moz_htmlinfo", QByteArray("0,0")}}
            << true;
        QTest::newRow("Excel (table html, csv, tab separated text)")
            << "Name\tQty\nApple\t3\nPear\t5"
            << "<html><head><meta http-equiv=Content-Type content=\"text/html; charset=utf-8\"></head><body><table><tr><td>Name</td>"
               "<td>Qty</td></tr><tr><td>Apple</td><td>3</td></tr><tr><td>Pear</td><td>5</td></tr></table></body></html>"
            << QVariantMap{{"text/csv", QByteArray("Name,Qty\r\nApple,3\r\nPear,5\r\n")}} << true;
        QTest::newRow("VS Code (text and editor data)")
            << "int main() { return 0; }"
            << QString()
            << QVariantMap{{"vscode-editor-data", QByteArray("{\"version\":1,\"isFromEmptySelection\":false,\"mode\":\"cpp\"}")}}
            << false;
        QTest::newRow("Terminal (plain text only)")
            << "$ ls -la /var/log"
            << QString() << QVariantMap{{"text/plain;charset=utf-8", QByteArray("$ ls -la /var/log")}} << false;
        QTest::newRow("Windows \"HTML Format\" with header offsets")
            << "offset check text"
            << "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000262\r\nStartFragment:0000000141\r\nEndFragment:0000000226\r\n"
               "<html><body><!--StartFragment--><b>offset</b> check text<!--EndFragment--></body></html>"
            << QVariantMap{} << true;
    }
    void appCopies()
    {
        QFETCH(QString, text);
        QFETCH(QString, html);
        QFETCH(QVariantMap, extra);
        QFETCH(bool, rich);
        Rig rig;
        auto* data = mime(text, extra);
        if (!html.isEmpty()) {
            data->setHtml(html);
        }
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        const auto entry = rig.newest();
        QCOMPARE(entry.content, text); // the plain text is the entry; nothing from the other formats leaks in
        QCOMPARE(entry.rich, rich);
        if (rich) {
            QCOMPARE(rig.db.repo->richText(entry.id), html);
        }
    }

    // ---- Password managers ----

    void markedCopiesAreNeverStoredInClear_data()
    {
        QTest::addColumn<QStringList>("native"); // formats reported by the Windows / macOS reader; empty: through Qt
        QTest::addColumn<QString>("marker");
        QTest::newRow("KeePassXC (x-kde-passwordManagerHint)") << QStringList{} << "x-kde-passwordManagerHint";
        QTest::newRow("Bitwarden (Windows, ExcludeClipboardContentFromMonitorProcessing)")
            << QStringList{"text/plain", "ExcludeClipboardContentFromMonitorProcessing"} << QString();
        QTest::newRow("Bitwarden (Windows, CanIncludeInClipboardHistory=0)")
            << QStringList{"text/plain", "CanIncludeInClipboardHistory=0", "CanUploadToCloudClipboard=0"} << QString();
        QTest::newRow("KeePass (Windows, Clipboard Viewer Ignore)") << QStringList{"text/plain", "Clipboard Viewer Ignore"} << QString();
        QTest::newRow("KeeWeb (macOS, net.antelle.keeweb)")
            << QStringList{"public.utf8-plain-text", "net.antelle.keeweb"} << QString();
        QTest::newRow("1Password (macOS, org.nspasteboard.ConcealedType)")
            << QStringList{"public.utf8-plain-text", "org.nspasteboard.ConcealedType"} << QString();
        QTest::newRow("1Password (com.agilebits.onepassword)") << QStringList{"public.utf8-plain-text", "com.agilebits.onepassword"} << QString();
    }
    void markedCopiesAreNeverStoredInClear()
    {
        QFETCH(QStringList, native);
        QFETCH(QString, marker);
        const QString secret = QStringLiteral("c0rrect-h0rse-battery");
        QtClipboardBackend::NativeReader reader;
        if (!native.isEmpty()) {
            reader = [secret, native] { return windowsRead(secret, native); };
        }
        Rig rig(reader);
        QSignalSpy captured(&rig.backend, &QtClipboardBackend::captured);
        auto* data = mime(secret);
        if (!marker.isEmpty()) {
            data->setData(marker, "secret");
            data->setHtml(QStringLiteral("<b>") + secret + QStringLiteral("</b>"));
        }
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.rejected.size(), 1);
        QCOMPARE(rig.lastVerdict(), ClipFilter::Verdict::Sensitive);
        QCOMPARE(captured.size(), 1);
        QVERIFY(captured.at(0).at(0).toString().isEmpty()); // the backend never even passed the text on
        QVERIFY(captured.at(0).at(2).toString().isEmpty()); // nor its formatted version
        Rig::settle();
        QCOMPARE(rig.db.repo->count(), 0);
        QVERIFY(rig.db.repo->search(QStringLiteral("horse")).empty());
    }

    void aMarkedCopyIsKeptOnlySealedWhenPasswordsAreKept()
    {
        clipitto::PasswordVault vault;
        vault.load(clipitto::PasswordVault::create("master", {64, 1}));
        vault.setMatchKey(QByteArray(32, 'm'));
        Rig rig;
        rig.pipe.setPasswords(&vault, true, QString());
        rig.backend.setReadMarkedText(rig.pipe.keepsPasswords());
        auto* data = mime(QStringLiteral("s3cret-from-the-vault"), {{"x-kde-passwordManagerHint", QByteArray("secret")}});
        data->setHtml(QStringLiteral("<b>s3cret-from-the-vault</b>"));
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        const auto entry = rig.newest();
        QVERIFY(entry.secret);
        QCOMPARE(entry.content, clipitto::db::Repository::secretLabel());
        QVERIFY(!entry.rich);
        QVERIFY(!rig.db.repo->sealedSecret(entry.id).contains("s3cret"));
        QVERIFY(rig.db.repo->search(QStringLiteral("s3cret")).empty());
    }

    void aMarkedPictureIsNeverStored()
    {
        Rig rig;
        QSignalSpy images(&rig.backend, &QtClipboardBackend::capturedImage);
        auto* data = new QMimeData;
        data->setImageData(QImage::fromData(pngOf(Qt::red)));
        data->setData(QStringLiteral("image/png"), pngOf(Qt::red));
        data->setData(QStringLiteral("x-kde-passwordManagerHint"), "secret");
        clip()->setMimeData(data);
        QTest::qWait(250);
        QCOMPARE(images.size(), 0);
        QCOMPARE(rig.db.repo->countImages(), 0);
        QCOMPARE(rig.db.repo->count(), 0);
    }

    // ---- Files ----

    void nautilusFileCopyIsStoredAsItsPath()
    {
        Rig rig;
        const QString path = tempFile(QStringLiteral("ClipItTo copied report.txt"));
        const QUrl url = QUrl::fromLocalFile(path);
        auto* data = mime(QString(), {{"x-special/gnome-copied-files", QByteArray("copy\n") + url.toEncoded()}});
        data->setUrls({url});
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        const auto entry = rig.newest();
        QCOMPARE(entry.content, QDir::toNativeSeparators(path));
        // A single path is shown in full, and is a path to the previews.
        QCOMPARE(clipitto::preview::filePaths(entry.content).size(), 1);
        QCOMPARE(clipitto::preview::fileSummary(clipitto::preview::filePaths(entry.content)), QDir::toNativeSeparators(path));
    }

    void explorerFileCopyHasNoTextAndIsStoredAsPaths()
    {
        // Windows Explorer: CF_HDROP only; the native reader reports "no text".
        auto reader = [] {
            NativeClipboardRead read;
            read.status = NativeClipboardRead::Status::NoText;
            read.formats = {QStringLiteral("application/x-qt-windows-mime;value=\"Shell IDList Array\""),
                            QStringLiteral("text/uri-list")};
            return read;
        };
        Rig rig(reader);
        const QString a = tempFile(QStringLiteral("a.docx"));
        const QString b = tempFile(QStringLiteral("b c.xlsx"));
        auto* data = new QMimeData;
        data->setUrls({QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)});
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, QDir::toNativeSeparators(a) + QLatin1Char('\n') + QDir::toNativeSeparators(b));
        QCOMPARE(clipitto::preview::filePaths(rig.newest().content).size(), 2);
        QCOMPARE(clipitto::preview::fileSummary(clipitto::preview::filePaths(rig.newest().content)), QStringLiteral("a.docx, b c.xlsx"));
    }

    void finderFileCopyWithNonAsciiNames()
    {
        // Finder: file URLs (public.file-url), percent-encoded, with accents and Hebrew in the names.
        Rig rig;
        const QString path = tempFile(QStringLiteral("Café שלום.pdf"));
        auto* data = mime(QString(), {{"public.file-url", QUrl::fromLocalFile(path).toEncoded()}});
        data->setUrls({QUrl::fromLocalFile(path)});
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, QDir::toNativeSeparators(path));
        QCOMPARE(clipitto::preview::filePaths(rig.newest().content).size(), 1);
    }

    void aUriListWithItsOwnPlainTextKeepsTheText()
    {
        // A file manager that also offers text/plain (the file's URI): that text is the copy, and the previews
        // still read it as a file.
        Rig rig;
        const QString path = tempFile(QStringLiteral("shared file.txt"));
        const QUrl url = QUrl::fromLocalFile(path);
        auto* data = mime(QString::fromLatin1(url.toEncoded()));
        data->setUrls({url});
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, QString::fromLatin1(url.toEncoded()));
        QCOMPARE(clipitto::preview::filePaths(rig.newest().content), QStringList{path});
    }

    void aWebLinkInAUriListIsStoredAsItsAddressNotAsAFile()
    {
        Rig rig;
        auto* data = new QMimeData;
        data->setUrls({QUrl(QStringLiteral("https://example.org/page"))});
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, QStringLiteral("https://example.org/page"));
        QVERIFY(clipitto::preview::filePaths(rig.newest().content).isEmpty());
    }

    // ---- Pictures ----

    void aScreenshotIsStoredAsAPicture()
    {
        Rig rig;
        auto* data = new QMimeData;
        data->setImageData(QImage::fromData(pngOf(QColor(10, 120, 200), {320, 200}))); // as Qt's own clipboard reports one
        data->setData(QStringLiteral("image/png"), pngOf(QColor(10, 120, 200), {320, 200}));
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        const auto entry = rig.newest();
        QVERIFY(entry.image);
        QCOMPARE(entry.imageWidth, 320);
        QCOMPARE(entry.imageHeight, 200);
        QCOMPARE(rig.db.repo->count() - rig.db.repo->countImages(), 0); // not also a text entry
    }

    void aBrowserPictureWithHtmlIsStoredAsAPictureNotAsText()
    {
        // Chrome's "Copy image": the picture plus an <img> in text/html, and no plain text.
        Rig rig;
        auto* data = new QMimeData;
        data->setImageData(QImage::fromData(pngOf(Qt::green)));
        data->setData(QStringLiteral("image/png"), pngOf(Qt::green));
        data->setHtml(QStringLiteral("<meta charset='utf-8'><img src=\"https://example.org/cat.png\">"));
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QVERIFY(rig.newest().image);
        Rig::settle();
        QCOMPARE(rig.db.repo->count(), 1);
    }

    // ---- Text styles ----

    void textStyles_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("URL") << "https://example.org/search?q=clip%20board&lang=he#top";
        QTest::newRow("colour #rrggbb") << "#ff8800";
        QTest::newRow("colour rgb()") << "rgb(255, 136, 0)";
        QTest::newRow("Hebrew with right-to-left marks") << QString::fromUtf8("‏שלום עולם ‏(ClipItTo) 123");
        QTest::newRow("emoji and surrogate pairs") << QString::fromUtf8("ok \xF0\x9F\x98\x80 \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 \xF0\x9F\x87\xAE\xF0\x9F\x87\xB1");
    }
    void textStyles()
    {
        QFETCH(QString, text);
        Rig rig;
        clip()->setText(text);
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, text); // stored exactly as copied
    }

    void coloursAndLinksAreRecognisedByThePreviews()
    {
        Rig rig;
        clip()->setText(QStringLiteral("#ff8800"));
        QTRY_COMPARE(rig.stored.size(), 1);
        const auto c = clipitto::preview::colorOf(rig.newest().content);
        QVERIFY(c.has_value());
        QCOMPARE(*c, (clipitto::preview::Rgba{255, 136, 0, 255}));
        clip()->setText(QStringLiteral("https://example.org/a"));
        QTRY_COMPARE(rig.stored.size(), 2);
        QVERIFY(!clipitto::preview::colorOf(rig.newest().content).has_value());
        QVERIFY(clipitto::preview::filePaths(rig.newest().content).isEmpty()); // a web address is not a file
    }

    void emojiCountAsFourBytesEachAgainstTheCap()
    {
        Rig rig({}, 8);
        clip()->setText(QString::fromUtf8("\xF0\x9F\x98\x80\xF0\x9F\x98\x80")); // 2 characters, 8 bytes: fits
        QTRY_COMPARE(rig.stored.size(), 1);
        clip()->setText(QString::fromUtf8("\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80")); // 12 bytes
        QTRY_COMPARE(rig.rejected.size(), 1);
        QCOMPARE(rig.lastVerdict(), ClipFilter::Verdict::TooLarge);
    }

    void blankCopiesAreSkipped_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("spaces") << "     ";
        QTest::newRow("tabs and newlines") << "\n\t\r\n\t  \n";
        QTest::newRow("no-break space") << QString::fromUtf8("  ");
        QTest::newRow("empty text") << "";
    }
    void blankCopiesAreSkipped()
    {
        QFETCH(QString, text);
        Rig rig;
        auto* data = new QMimeData;
        data->setData(QStringLiteral("text/plain"), text.toUtf8()); // an empty text still announces text/plain
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.rejected.size(), 1);
        QCOMPARE(rig.lastVerdict(), ClipFilter::Verdict::Empty);
        QCOMPARE(rig.db.repo->count(), 0);
    }

    void theSizeCapIsInBytesAtOneMegabyte_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("kept");
        QTest::newRow("exactly 1 MiB of ASCII") << QString(1024 * 1024, QLatin1Char('a')) << true;
        QTest::newRow("1 MiB and one byte") << QString(1024 * 1024 + 1, QLatin1Char('a')) << false;
        QTest::newRow("under 1 M characters, over 1 MiB in UTF-8") << QString(524289, QChar(0x00e9)) << false;
    }
    void theSizeCapIsInBytesAtOneMegabyte()
    {
        QFETCH(QString, text);
        QFETCH(bool, kept);
        Rig rig;
        clip()->setText(text);
        if (kept) {
            QTRY_COMPARE_WITH_TIMEOUT(rig.stored.size(), 1, 5000);
            QCOMPARE(rig.newest().content.size(), text.size());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(rig.rejected.size(), 1, 5000);
            QCOMPARE(rig.lastVerdict(), ClipFilter::Verdict::TooLarge);
            QCOMPARE(rig.db.repo->count(), 0);
        }
    }

    void copyingTheSameTextAgainMovesItToTheTop()
    {
        Rig rig;
        for (const QString& text : {QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("alpha")}) {
            const int before = rig.stored.size();
            clip()->setMimeData(mime(text));
            QTRY_COMPARE(rig.stored.size(), before + 1);
        }
        QCOMPARE(rig.stored.at(2).at(1).toBool(), false); // a bump, not an insert
        const auto entries = rig.db.repo->recent(10, false);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).content, QStringLiteral("alpha"));
        QCOMPARE(entries.at(1).content, QStringLiteral("beta"));
    }

    void aBurstOfCopiesIsStoredInOrder()
    {
        // Each change is read in turn (the reader returns the copy of that moment).
        auto n = std::make_shared<int>(0);
        Rig rig([n] { return windowsRead(QStringLiteral("burst %1").arg(++*n), {QStringLiteral("text/plain")}); });
        for (int i = 0; i < 12; ++i) {
            clip()->setText(QStringLiteral("trigger %1").arg(i));
        }
        QTRY_COMPARE(rig.stored.size(), 12);
        const auto entries = rig.db.repo->recent(20, false); // newest first
        QCOMPARE(entries.size(), 12);
        for (int i = 0; i < 12; ++i) {
            QCOMPARE(entries.at(i).content, QStringLiteral("burst %1").arg(12 - i));
        }
    }

    void crlfAndLfCopiesAreOneEntry()
    {
        // The same text with Windows and Unix line ends is one entry, stored with LF (as on Windows and macOS).
        Rig rig;
        clip()->setText(QStringLiteral("line one\r\nline two"));
        QTRY_COMPARE(rig.stored.size(), 1);
        clip()->setText(QStringLiteral("line one\nline two"));
        QTRY_COMPARE(rig.stored.size(), 2); // both were captured ...
        QCOMPARE(rig.db.repo->count(), 1); // ... but they are one entry
        QCOMPARE(rig.newest().content, QStringLiteral("line one\nline two"));
    }

    // ---- Our own paste-back ----

    void pastingAnEntryBackIsNotRecordedAgain_data()
    {
        QTest::addColumn<bool>("withFormatting");
        QTest::newRow("paste plain") << false;
        QTest::newRow("paste with formatting") << true;
    }
    void pastingAnEntryBackIsNotRecordedAgain()
    {
        QFETCH(bool, withFormatting);
        Rig rig;
        const QString text = QStringLiteral("Quarterly report draft");
        const QString html = QStringLiteral("<b>Quarterly</b> report draft");
        auto* data = mime(text);
        data->setHtml(html);
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        const qint64 id = rig.newest().id;
        QVERIFY(rig.newest().rich);

        clipitto::ClipboardWriter writer(clip(), rig.pipe.filter());
        if (withFormatting) {
            writer.setRich(text, html);
        } else {
            writer.setText(text);
        }
        QTRY_COMPARE(rig.rejected.size(), 1); // the echo
        QCOMPARE(rig.lastVerdict(), ClipFilter::Verdict::OwnWrite);
        Rig::settle();
        QCOMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.db.repo->count(), 1);
        QCOMPARE(rig.newest().id, id);
        QVERIFY(rig.newest().rich); // an echo must not strip or change the stored formatting
    }

    // ---- Formatting ----

    void pasteWithFormattingPutsHtmlAndTextOnTheClipboardPlainPutsTextOnly()
    {
        TestDb t;
        ClipFilter filter(1024 * 1024);
        clipitto::ClipboardWriter writer(clip(), filter);
        writer.setRich(QStringLiteral("bold word"), QStringLiteral("<b>bold</b> word"));
        QVERIFY(clip()->mimeData()->hasHtml());
        QVERIFY(clip()->mimeData()->html().contains(QStringLiteral("<b>bold</b>")));
        QCOMPARE(clip()->mimeData()->text(), QStringLiteral("bold word")); // for apps that take plain text only
        writer.setText(QStringLiteral("bold word"));
        QVERIFY(!clip()->mimeData()->hasHtml());
        QCOMPARE(clip()->mimeData()->text(), QStringLiteral("bold word"));
    }

    void keepFormattingOnAndOff()
    {
        const QString html = QStringLiteral("<ul><li>first item</li><li>second item</li></ul>");
        const QString text = QStringLiteral("first item\nsecond item");
        {
            Rig rig; // keep_formatting on (the default)
            auto* data = mime(text);
            data->setHtml(html);
            clip()->setMimeData(data);
            QTRY_COMPARE(rig.stored.size(), 1);
            QVERIFY(rig.newest().rich);
        }
        {
            Rig rig; // off: the daemon tells the backend not to read it and the pipeline not to keep it
            rig.backend.setReadRichText(false);
            rig.pipe.setKeepFormatting(false);
            QSignalSpy captured(&rig.backend, &QtClipboardBackend::captured);
            auto* data = mime(QStringLiteral("other item\nanother item"));
            data->setHtml(QStringLiteral("<ul><li>other item</li><li>another item</li></ul>"));
            clip()->setMimeData(data);
            QTRY_COMPARE(rig.stored.size(), 1);
            QVERIFY(captured.at(0).at(2).toString().isEmpty()); // never even read
            QVERIFY(!rig.newest().rich);
        }
    }

    void htmlLargerThanTheCapIsDroppedButTheTextIsKept()
    {
        Rig rig({}, 4096);
        auto* data = mime(QStringLiteral("small text"));
        data->setHtml(QStringLiteral("<b>small</b> text<!-- ") + QString(8000, QLatin1Char('x')) + QStringLiteral(" -->"));
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QCOMPARE(rig.newest().content, QStringLiteral("small text"));
        QVERIFY(!rig.newest().rich);
    }

    void htmlThatDoesNotMatchTheTextIsNotKept()
    {
        // An app whose html is stale or for another selection: never stored under this text.
        Rig rig;
        auto* data = mime(QStringLiteral("what is on the screen now"));
        data->setHtml(QStringLiteral("<b>something copied minutes ago</b>"));
        clip()->setMimeData(data);
        QTRY_COMPARE(rig.stored.size(), 1);
        QVERIFY(!rig.newest().rich);
    }
};

QTEST_MAIN(TestCopyApps)
#include "tst_copyapps.moc"
