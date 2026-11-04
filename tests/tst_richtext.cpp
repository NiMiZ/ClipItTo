#include "RichText.h"

#include <QTest>

using clipitto::richtext::hasFormatting;

class TestRichText : public QObject {
    Q_OBJECT

private slots:
    void tellsFormattingFromWrappedPlainText_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<bool>("formatted");
        QTest::newRow("bold") << "<b>hi</b> there" << true;
        QTest::newRow("strong, upper case") << "<STRONG>hi</STRONG>" << true;
        QTest::newRow("link") << "<a href=\"https://example.org\">site</a>" << true;
        QTest::newRow("list") << "<ul><li>one</li><li>two</li></ul>" << true;
        QTest::newRow("table") << "<table><tr><td>1</td></tr></table>" << true;
        QTest::newRow("heading") << "<h2>Title</h2>" << true;
        QTest::newRow("picture") << "<img src=\"a.png\">" << true;
        QTest::newRow("italic by style") << "<span style=\"font-style: italic\">x</span>" << true;
        QTest::newRow("bold by style") << "<span style='font-weight:700'>x</span>" << true;
        QTest::newRow("underline by style") << "<p style=\"text-decoration: underline;\">x</p>" << true;
        QTest::newRow("background by style") << "<span style=\"background-color: yellow\">x</span>" << true;
        // What a browser puts around any plain text it copies:
        QTest::newRow("browser wrapper") << "<meta charset='utf-8'><span style=\"color: rgb(0, 0, 0); font-family: Arial;\">plain</span>" << false;
        QTest::newRow("paragraphs") << "<html><body><p>one</p><p>two<br>three</p></body></html>" << false;
        QTest::newRow("div") << "<div>text</div>" << false;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("no markup") << "just text" << false;
        QTest::newRow("a letter in a word is not a tag") << "1 < 2 and a > b" << false;
    }
    void tellsFormattingFromWrappedPlainText()
    {
        QFETCH(QString, html);
        QFETCH(bool, formatted);
        QCOMPARE(hasFormatting(html), formatted);
    }

    void matchesTheTextOfTheSameCopyOnly()
    {
        using clipitto::richtext::matchesText;
        QVERIFY(matchesText(QStringLiteral("<p><b>Bold</b> word &amp; more</p>"), QStringLiteral("Bold word & more")));
        QVERIFY(matchesText(QStringLiteral("<ul><li>one thing</li></ul>"), QStringLiteral("\u2022 one thing")));
        QVERIFY(!matchesText(QStringLiteral("<b>another copy entirely</b>"), QStringLiteral("Bold word & more")));
    }
};

QTEST_APPLESS_MAIN(TestRichText)
#include "tst_richtext.moc"
