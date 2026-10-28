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

    void keyboardLayouts()
    {
        // The standard example: typed on English keys while the Hebrew layout was meant, and back.
        QCOMPARE(*apply(Kind::HebrewKeyboard, QStringLiteral("xccv")), QStringLiteral("סבבה"));
        QCOMPARE(*apply(Kind::EnglishKeyboard, QStringLiteral("סבבה")), QStringLiteral("xccv"));
        QCOMPARE(*apply(Kind::HebrewKeyboard, QStringLiteral("akuo")), QStringLiteral("שלום"));
        QCOMPARE(*apply(Kind::HebrewKeyboard, QStringLiteral("XCCV 12")), QStringLiteral("סבבה 12")); // case, digits
        // Final letters have their own keys.
        QCOMPARE(*apply(Kind::HebrewKeyboard, QStringLiteral("iop;.l")), QStringLiteral("ןםפףץך"));
        // Punctuation on the same keys, brackets mirrored.
        QCOMPARE(*apply(Kind::HebrewKeyboard, QStringLiteral("t/e,")), QStringLiteral("א.קת"));
        QCOMPARE(*apply(Kind::HebrewKeyboard, QStringLiteral("(a)")), QStringLiteral(")ש("));
        // Every key of the table round-trips (lower case, since Hebrew has none).
        const QString keys = QStringLiteral("qwertyuiopasdfghjklzxcvbnm;,./`[]'(){}<>");
        const QString hebrew = *apply(Kind::HebrewKeyboard, keys);
        QCOMPARE(hebrew.size(), keys.size());
        QCOMPARE(*apply(Kind::EnglishKeyboard, hebrew), keys);
        // Nothing to convert is said, not silently pasted.
        QString why;
        QVERIFY(!apply(Kind::HebrewKeyboard, QStringLiteral("123 456"), &why));
        QVERIFY(!why.isEmpty());
    }

    void keyboardConversionsAreOfferedWhereTheyFit()
    {
        QVERIFY(fits(Kind::HebrewKeyboard, QStringLiteral("xccv")));
        QVERIFY(!fits(Kind::EnglishKeyboard, QStringLiteral("xccv")));
        QVERIFY(fits(Kind::EnglishKeyboard, QStringLiteral("סבבה")));
        QVERIFY(!fits(Kind::HebrewKeyboard, QStringLiteral("סבבה")));
        QVERIFY(fits(Kind::HebrewKeyboard, QStringLiteral("hello שלום")) && fits(Kind::EnglishKeyboard, QStringLiteral("hello שלום")));
        QVERIFY(fits(Kind::HebrewKeyboard, QStringLiteral("123"))); // no letters at all: either may be tried
        QVERIFY(fits(Kind::Upper, QStringLiteral("סבבה")));
    }

    void everyTransformHasAMnemonicLabel()
    {
        const auto list = all();
        QCOMPARE(list.size(), 14);
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
