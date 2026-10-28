#include "TextTransform.h"

#include <QTest>

using namespace clipitto::texttransform;

class TestTextTransform : public QObject {
    Q_OBJECT

private slots:
    void cases()
    {
        QCOMPARE(*apply(Kind::Upper, QStringLiteral("abc Déf")), QStringLiteral("ABC DÉF"));
        QCOMPARE(*apply(Kind::Lower, QStringLiteral("ABC Déf")), QStringLiteral("abc déf"));
        QCOMPARE(*apply(Kind::Title, QStringLiteral("hELLO wORLD, it's 3rd-time")), QStringLiteral("Hello World, It's 3rd-Time"));
    }

    void whiteSpace()
    {
        QCOMPARE(*apply(Kind::Trim, QStringLiteral("  a b \n")), QStringLiteral("a b"));
        QCOMPARE(*apply(Kind::CollapseSpaces, QStringLiteral("a   b\t\tc\n  d")), QStringLiteral("a b c\n d"));
        QCOMPARE(*apply(Kind::OneLine, QStringLiteral("  one\r\n  two\n\nthree  ")), QStringLiteral("one two three"));
    }

    void json()
    {
        QCOMPARE(*apply(Kind::JsonMinify, QStringLiteral("{ \"a\": 1,\n \"b\": [1, 2] }")), QStringLiteral("{\"a\":1,\"b\":[1,2]}"));
        const QString pretty = *apply(Kind::JsonPretty, QStringLiteral("{\"a\":1}"));
        QVERIFY(pretty.contains(QLatin1Char('\n')));
        QVERIFY(pretty.contains(QStringLiteral("    \"a\": 1")));
        QString why;
        QVERIFY(!apply(Kind::JsonPretty, QStringLiteral("{nope"), &why));
        QVERIFY(why.contains(QStringLiteral("JSON")));
    }

    void urlAndBase64RoundTrip()
    {
        const QString text = QStringLiteral("a b&c=d/é");
        const QString encoded = *apply(Kind::UrlEncode, text);
        QCOMPARE(encoded, QStringLiteral("a%20b%26c%3Dd%2F%C3%A9"));
        QCOMPARE(*apply(Kind::UrlDecode, encoded), text);
        const QString b64 = *apply(Kind::Base64Encode, text);
        QCOMPARE(*apply(Kind::Base64Decode, b64), text);
        QString why;
        QVERIFY(!apply(Kind::Base64Decode, QStringLiteral("not base64!!"), &why));
        QVERIFY(!apply(Kind::Base64Decode, QStringLiteral("/w=="), &why)); // 0xFF: not UTF-8
    }

    void hebrewKeyboard()
    {
        const auto toHebrew = [](const char* text) { return *convertLayout(QString::fromUtf8(text), QStringLiteral("en"), QStringLiteral("he")); };
        QCOMPARE(toHebrew("xccv"), QStringLiteral("סבבה"));
        QCOMPARE(toHebrew("akuo"), QStringLiteral("שלום"));
        QCOMPARE(toHebrew("XCCV 12"), QStringLiteral("סבבה 12")); // case is lost (Hebrew has none), digits stay
        QCOMPARE(toHebrew("iop;.l"), QStringLiteral("ןםפףץך")); // final letters have their own keys
        QCOMPARE(toHebrew("t/e,"), QStringLiteral("א.קת"));
        QCOMPARE(toHebrew("(a)"), QStringLiteral(")ש(")); // Hebrew keyboards mirror the brackets
        QCOMPARE(*convertLayout(QStringLiteral("סבבה"), QStringLiteral("he"), QStringLiteral("en")), QStringLiteral("xccv"));
        QString why;
        QVERIFY(!convertLayout(QStringLiteral("123 456"), QStringLiteral("en"), QStringLiteral("he"), &why));
        QVERIFY(!why.isEmpty());
    }

    void russianAndGreekKeepCapitals()
    {
        QCOMPARE(*convertLayout(QStringLiteral("ghbdtn"), QStringLiteral("en"), QStringLiteral("ru")), QStringLiteral("привет"));
        QCOMPARE(*convertLayout(QStringLiteral("Ghbdtn"), QStringLiteral("en"), QStringLiteral("ru")), QStringLiteral("Привет"));
        QCOMPARE(*convertLayout(QStringLiteral("Привет"), QStringLiteral("ru"), QStringLiteral("en")), QStringLiteral("Ghbdtn"));
        QCOMPARE(*convertLayout(QStringLiteral("gjrf"), QStringLiteral("en"), QStringLiteral("ru")), QStringLiteral("пока"));
        QCOMPARE(*convertLayout(QStringLiteral("nesting"), QStringLiteral("en"), QStringLiteral("el")), QStringLiteral("νεστινγ"));
        QCOMPARE(*convertLayout(QStringLiteral("Wsa"), QStringLiteral("en"), QStringLiteral("el")), QStringLiteral("Σσα"));
        // From one non-English layout to another, through the keys.
        QCOMPARE(*convertLayout(QStringLiteral("סבבה"), QStringLiteral("he"), QStringLiteral("ru")), QStringLiteral("чссм"));
    }

    void everyKeyOfEveryLayoutRoundTrips()
    {
        const QString keys = QStringLiteral("qwertyuiopasdfghjklzxcvbnm");
        for (const char* id : {"he", "ru", "el"}) {
            const QString own = *convertLayout(keys, QStringLiteral("en"), QString::fromLatin1(id));
            QCOMPARE(own.size(), keys.size());
            QCOMPARE(*convertLayout(own, QString::fromLatin1(id), QStringLiteral("en")), keys);
        }
        const QString punctuation = QStringLiteral(",./;'`[]");
        const QString hebrew = *convertLayout(punctuation, QStringLiteral("en"), QStringLiteral("he"));
        QCOMPARE(*convertLayout(hebrew, QStringLiteral("he"), QStringLiteral("en")), punctuation);
    }

    void whichLayoutsAreOffered()
    {
        const QStringList he{QStringLiteral("he")};
        const QStringList heRu{QStringLiteral("he"), QStringLiteral("ru")};
        QCOMPARE(layoutOf(QStringLiteral("xccv"), he), QStringLiteral("en"));
        QCOMPARE(layoutOf(QStringLiteral("סבבה"), he), QStringLiteral("he"));
        QCOMPARE(layoutOf(QStringLiteral("hello שלום עולם"), he), QStringLiteral("he")); // the most letters
        QCOMPARE(layoutOf(QStringLiteral("123"), he), QStringLiteral("en"));
        QCOMPARE(layoutOf(QStringLiteral("Привет"), he), QStringLiteral("en")); // Russian is not switched on
        // One layout: the opposite of the text's language.
        QCOMPARE(convertibleTo(QStringLiteral("xccv"), he), QStringList{QStringLiteral("he")});
        QCOMPARE(convertibleTo(QStringLiteral("סבבה"), he), QStringList{QStringLiteral("en")});
        // Several: each other one (English first when the text is not English).
        QCOMPARE(convertibleTo(QStringLiteral("xccv"), heRu), heRu);
        QCOMPARE(convertibleTo(QStringLiteral("סבבה"), heRu), (QStringList{QStringLiteral("en"), QStringLiteral("ru")}));
        QVERIFY(convertibleTo(QStringLiteral("xccv"), {}).isEmpty()); // English alone: nothing to convert to
    }

    void theSettingChoosesTheLayouts()
    {
        QCOMPARE(enabledLayouts(QString(), QStringLiteral("he-IL")), QStringList{QStringLiteral("he")}); // automatic
        QCOMPARE(enabledLayouts(QString(), QStringLiteral("iw")), QStringList{QStringLiteral("he")});
        QVERIFY(enabledLayouts(QString(), QStringLiteral("en-US")).isEmpty());
        QVERIFY(enabledLayouts(QStringLiteral("none"), QStringLiteral("he-IL")).isEmpty());
        QCOMPARE(enabledLayouts(QStringLiteral(" ru, he, xx, ru "), QStringLiteral("en")),
                 (QStringList{QStringLiteral("ru"), QStringLiteral("he")}));
        QCOMPARE(layoutName(QStringLiteral("en")), QStringLiteral("English"));
        QCOMPARE(layoutName(QStringLiteral("he")), QStringLiteral("Hebrew"));
        QCOMPARE(layouts().size(), 3);
    }

    void everyTransformHasAMnemonicLabel()
    {
        const auto list = all();
        QCOMPARE(list.size(), 12);
        QSet<QChar> keys;
        for (const Info& i : list) {
            const int at = i.label.indexOf(QLatin1Char('&'));
            QVERIFY2(at >= 0, qPrintable(i.label));
            QVERIFY2(!keys.contains(i.label.at(at + 1).toLower()), qPrintable(i.label)); // no clash
            keys.insert(i.label.at(at + 1).toLower());
        }
    }
};

QTEST_GUILESS_MAIN(TestTextTransform)
#include "tst_texttransform.moc"
