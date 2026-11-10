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
