#include "Previews.h"

#include <QTest>

using namespace clipitto::preview;

class TestPreviews : public QObject {
    Q_OBJECT

private slots:
    void colors_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("isColor");
        QTest::addColumn<int>("r");
        QTest::addColumn<int>("g");
        QTest::addColumn<int>("b");
        QTest::addColumn<int>("a");
        QTest::newRow("rrggbb") << "#ff8800" << true << 255 << 136 << 0 << 255;
        QTest::newRow("upper, spaces around") << "  #FF8800\n" << true << 255 << 136 << 0 << 255;
        QTest::newRow("rgb short") << "#f80" << true << 255 << 136 << 0 << 255;
        QTest::newRow("rrggbbaa") << "#ff880080" << true << 255 << 136 << 0 << 128;
        QTest::newRow("rgba short") << "#f808" << true << 255 << 136 << 0 << 136;
        QTest::newRow("rgb()") << "rgb(255, 136, 0)" << true << 255 << 136 << 0 << 255;
        QTest::newRow("rgb() percent") << "rgb(100%, 0%, 50%)" << true << 255 << 0 << 128 << 255;
        QTest::newRow("rgba()") << "rgba(0, 0, 0, 0.5)" << true << 0 << 0 << 0 << 128;
        QTest::newRow("rgb() modern") << "rgb(10 20 30 / 50%)" << true << 10 << 20 << 30 << 128;
        QTest::newRow("five digits") << "#12345" << false << 0 << 0 << 0 << 0;
        QTest::newRow("not hex") << "#ggg" << false << 0 << 0 << 0 << 0;
        QTest::newRow("a hashtag") << "#todo" << false << 0 << 0 << 0 << 0;
        QTest::newRow("too big") << "rgb(300, 0, 0)" << false << 0 << 0 << 0 << 0;
        QTest::newRow("a sentence") << "use #ff8800 here" << false << 0 << 0 << 0 << 0;
        QTest::newRow("two lines") << "#ff8800\n#00ff00" << false << 0 << 0 << 0 << 0;
        QTest::newRow("empty") << "" << false << 0 << 0 << 0 << 0;
    }
    void colors()
    {
        QFETCH(QString, text);
        QFETCH(bool, isColor);
        QFETCH(int, r);
        QFETCH(int, g);
        QFETCH(int, b);
        QFETCH(int, a);
        const auto c = colorOf(text);
        QCOMPARE(c.has_value(), isColor);
        if (isColor) {
            QCOMPARE(*c, (Rgba{r, g, b, a}));
        }
    }

    void files_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("paths");
        QTest::newRow("one") << "/home/me/notes.txt" << QStringList{"/home/me/notes.txt"};
        QTest::newRow("several, blank lines") << "/a/b.txt\n\n/a/c d.txt\n"
                                              << QStringList{"/a/b.txt", "/a/c d.txt"};
        QTest::newRow("home") << "~/Documents/a.pdf" << QStringList{"~/Documents/a.pdf"};
        QTest::newRow("windows") << "C:\\Users\\me\\a.txt" << QStringList{"C:\\Users\\me\\a.txt"};
        QTest::newRow("windows forward") << "D:/x/y.txt" << QStringList{"D:/x/y.txt"};
        QTest::newRow("unc") << "\\\\server\\share\\a.txt" << QStringList{"\\\\server\\share\\a.txt"};
        QTest::newRow("uri decoded") << "file:///home/me/a%20b.txt" << QStringList{"/home/me/a b.txt"};
        QTest::newRow("mixed lines") << "/a/b.txt\nfile:///c/d.txt" << QStringList{"/a/b.txt", "/c/d.txt"};
        QTest::newRow("a command") << "/help" << QStringList{};
        QTest::newRow("text between") << "/a/b.txt\nsee above" << QStringList{};
        QTest::newRow("a sentence") << "go to /a/b.txt now" << QStringList{};
        QTest::newRow("a web address") << "https://example.org/a/b" << QStringList{};
        QTest::newRow("remote uri") << "file://server/share/a.txt" << QStringList{};
        QTest::newRow("empty") << "" << QStringList{};
    }
    void files()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, paths);
        QCOMPARE(filePaths(text), paths);
    }

    void copiedFiles_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("text");
        QTest::newRow("nautilus copy") << "copy\nfile:///home/me/a.png\nfile:///home/me/b.txt"
                                       << "/home/me/a.png\n/home/me/b.txt";
        QTest::newRow("nautilus cut") << "cut\nfile:///home/me/a.png" << "/home/me/a.png";
        QTest::newRow("percent escapes") << "copy\nfile:///home/me/a%20b%C3%A9.png" << QString::fromUtf8("/home/me/a bé.png");
        QTest::newRow("percent hash and percent") << "copy\nfile:///home/me/100%25%20%23x.png" << "/home/me/100% #x.png";
        QTest::newRow("uri-list, CRLF and comments") << "# copied\r\nfile:///tmp/x.txt\r\nfile://localhost/tmp/y.txt\r\n"
                                                     << "/tmp/x.txt\n/tmp/y.txt";
        QTest::newRow("no trailing newline, blank lines") << "copy\n\nfile:///a/b.png\n\n" << "/a/b.png";
        QTest::newRow("upper case scheme") << "FILE:///a/b.png" << "/a/b.png";
        QTest::newRow("a file called copy is not the action") << "file:///home/me/copy" << "/home/me/copy";
        QTest::newRow("action only") << "copy" << "";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("remote host") << "copy\nfile://server/share/a.png" << "";
        QTest::newRow("smb") << "copy\nsmb://server/share/a.png" << "";
        QTest::newRow("one remote entry refuses all") << "copy\nfile:///a/b.png\nsmb://server/c.png" << "";
        QTest::newRow("https") << "https://example.org/a.png" << "";
        QTest::newRow("a plain path is not a uri") << "copy\n/home/me/a.png" << "";
        QTest::newRow("dot dot") << "copy\nfile:///home/me/../../etc/passwd" << "";
        QTest::newRow("encoded dot dot") << "copy\nfile:///home/me/%2e%2e/x" << "";
        QTest::newRow("dot part") << "copy\nfile:///home/./me/x" << "";
        QTest::newRow("encoded newline forges a line") << "copy\nfile:///home/me/a%0A/etc/passwd" << "";
        QTest::newRow("encoded carriage return") << "copy\nfile:///home/me/a%0Db" << "";
        QTest::newRow("encoded nul") << "copy\nfile:///home/me/a%00b" << "";
        QTest::newRow("invalid utf-8 escape") << "copy\nfile:///home/me/%FF%FEx.png" << "";
        QTest::newRow("query") << "copy\nfile:///home/me/a.png?x=1" << "";
        QTest::newRow("fragment") << "copy\nfile:///home/me/a.png#x" << "";
        QTest::newRow("no scheme slashes") << "copy\nfile:home/me/a.png" << "";
        QTest::newRow("too long a path") << "copy\nfile:///" + QString(5000, QLatin1Char('a')) << "";
        QTest::newRow("too much input") << "copy\nfile:///a/b\n" + QString(70000, QLatin1Char('\n')) << "";
    }
    void copiedFiles()
    {
        QFETCH(QString, raw);
        QFETCH(QString, text);
        QCOMPARE(copiedFilesText(raw), text);
        // What comes out is a list the row understands (every line a path), unless it is one single-component path.
        if (!text.isEmpty()) {
            QCOMPARE(filePaths(text).join(QLatin1Char('\n')), text);
        }
    }
    void copiedFilesCapsTheCount()
    {
        QStringList lines{QStringLiteral("copy")};
        for (int i = 0; i < 200; ++i) {
            lines << QStringLiteral("file:///a/f%1").arg(i);
        }
        QCOMPARE(copiedFilesText(lines.join(QLatin1Char('\n'))).split(QLatin1Char('\n')).size(), 200);
        lines << QStringLiteral("file:///a/one-more");
        QVERIFY(copiedFilesText(lines.join(QLatin1Char('\n'))).isEmpty());
    }
    void filesMimeTypeIsLikeTheSourceLabel()
    {
        const QStringList types{QStringLiteral("text/uri-list"), filesMimeType(QStringLiteral("gnome"))};
        QVERIFY(hasFilesMimeType(types));
        QVERIFY(!hasFilesMimeType({QStringLiteral("text/plain")}));
        QCOMPARE(withoutFilesMimeType(types), QStringList{QStringLiteral("text/uri-list")});
        QCOMPARE(filesMimeType(QStringLiteral("uri-list")), QStringLiteral("x-clipitto-files:uri-list"));
    }

    void summaryNamesTheFirstFiles()
    {
        QCOMPARE(fileSummary({"/a/b.txt"}), QStringLiteral("/a/b.txt")); // one path: all of it
        QCOMPARE(fileSummary({"/mnt/i/projects/ClipItTo"}), QStringLiteral("/mnt/i/projects/ClipItTo"));
        QCOMPARE(fileSummary({"/a/b.txt", "C:\\x\\c.txt"}), QStringLiteral("b.txt, c.txt"));
        QCOMPARE(fileSummary({"/a/b.txt", "/a/c.txt", "/a/d.txt", "/a/e.txt"}), QStringLiteral("b.txt, c.txt, +2 more"));
        QVERIFY(fileSummary({}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPreviews)
#include "tst_previews.moc"
