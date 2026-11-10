#include "PasswordRules.h"

#include <QTest>

using namespace clipitto;
using namespace clipitto::passwordrules;

class TestPasswordRules : public QObject {
    Q_OBJECT

private slots:
    void countsAreMinimums()
    {
        PasswordRule r;
        r.minLength = 2;
        r.maxLength = 12;
        r.digits = 2;
        QVERIFY(matches(r, QStringLiteral("12")));
        QVERIFY(matches(r, QStringLiteral("ab123")));
        QVERIFY(!matches(r, QStringLiteral("ab1")));
        r.minLength = 3; // "at least 2 digits, not only 2 digits"
        QVERIFY(!matches(r, QStringLiteral("12")));
        QVERIFY(matches(r, QStringLiteral("a12")));
    }

    void classesAndEnd()
    {
        PasswordRule r;
        r.minLength = 8;
        r.maxLength = 16;
        r.lower = 1;
        r.upper = 1;
        r.symbols = 1;
        r.digitsAtEnd = true;
        QVERIFY(matches(r, QStringLiteral("aB$cdefg7")));
        QCOMPARE(whyNot(r, QStringLiteral("aB$cdefgh")), QStringLiteral("does not end with a digit"));
        QCOMPARE(whyNot(r, QStringLiteral("ab$cdefg7")), QStringLiteral("too few upper case letters"));
        QCOMPARE(whyNot(r, QStringLiteral("aBcdefgh7")), QStringLiteral("too few symbols"));
        QCOMPARE(whyNot(r, QStringLiteral("aB$7")), QStringLiteral("too short"));
        QCOMPARE(whyNot(r, QStringLiteral("aB$ cdefg7")), QStringLiteral("contains white space"));
        QCOMPARE(whyNot(r, QStringLiteral("aB$cdefg7\nx")), QStringLiteral("not a single line"));
        QVERIFY(!whyNot(r, QStringLiteral("aB$cdefgh7")).contains(QLatin1String("aB$"))); // never echoes the text
    }

    void anyRule()
    {
        PasswordRule a;
        a.digits = 4;
        a.minLength = 4;
        a.maxLength = 4;
        PasswordRule b;
        b.upper = 3;
        QVERIFY(matchesAny({a, b}, QStringLiteral("1234")));
        QVERIFY(matchesAny({a, b}, QStringLiteral("ABCdefgh")));
        QVERIFY(!matchesAny({a, b}, QStringLiteral("abcdefgh")));
        QVERIFY(!matchesAny({}, QStringLiteral("1234")));
    }

    void validates()
    {
        PasswordRule r;
        QVERIFY(validate(r).isEmpty());
        r.minLength = 0;
        QVERIFY(!validate(r).isEmpty());
        r = PasswordRule();
        r.minLength = 20;
        r.maxLength = 10;
        QVERIFY(!validate(r).isEmpty());
        r = PasswordRule();
        r.maxLength = 10;
        r.lower = 6;
        r.digits = 6;
        QVERIFY(!validate(r).isEmpty());
        r = PasswordRule();
        r.maxLength = kLengthLimit + 1;
        QVERIFY(!validate(r).isEmpty());
        r = PasswordRule();
        r.symbols = -1;
        QVERIFY(!validate(r).isEmpty());
    }

    void textFormRoundTrips()
    {
        PasswordRule r;
        r.minLength = 8;
        r.maxLength = 20;
        r.lower = 1;
        r.upper = 1;
        r.digits = 2;
        r.symbols = 0;
        r.digitsAtEnd = true;
        PasswordRule back;
        QVERIFY(parse(format(r), back));
        QCOMPARE(back, r);
        QVERIFY(parse(QStringLiteral("  6-9   digits=2 "), back));
        QCOMPARE(back.digits, 2);
        for (const char* bad : {"", "abc", "8", "8-", "9-8", "8-20 foo=1", "8-20 lower=x", "8-20 lower", "0-5", "8-20 lower=-1"}) {
            PasswordRule keep = r;
            QVERIFY2(!parse(QString::fromLatin1(bad), keep), bad);
            QCOMPARE(keep, r);
        }
    }
};

QTEST_APPLESS_MAIN(TestPasswordRules)
#include "tst_passwordrules.moc"
