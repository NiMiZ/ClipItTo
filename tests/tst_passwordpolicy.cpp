#include "PasswordPolicy.h"

#include <QTest>

using namespace clipitto::passwordpolicy;

// The rules a new master password has to meet.
class TestPasswordPolicy : public QObject {
    Q_OBJECT

private:
    static bool ok(const QString& password, const QString& user = QStringLiteral("nimiz"))
    {
        return acceptable(password, user);
    }
    // The text of the first rule that is not met, empty when all are.
    static QString broken(const QString& password, const QString& user = QStringLiteral("nimiz"))
    {
        for (const Rule& r : check(password, user)) {
            if (!r.met) {
                return r.text;
            }
        }
        return {};
    }

private slots:
    void acceptsAGoodOne()
    {
        QVERIFY(ok(QStringLiteral("Blue-horse-47-staple")));
        QVERIFY(ok(QStringLiteral("Kx9mQ2vL")));
        QVERIFY(ok(QStringLiteral("Zz12Zz12")));
    }

    void needsLengthCaseAndDigit()
    {
        QVERIFY(!ok(QStringLiteral("Kx9mQ2v"))); // 7
        QVERIFY(!ok(QStringLiteral("kx9mq2vl"))); // no upper case
        QVERIFY(!ok(QStringLiteral("KX9MQ2VL"))); // no lower case
        QVERIFY(!ok(QStringLiteral("KxbmQwvL"))); // no digit
        // The case that was reported: one upper case, five lower case.
        QVERIFY(!ok(QStringLiteral("Abcdef")));
    }

    void everyRuleIsListedAndTellsWhatIsWrong()
    {
        const QList<Rule> rules = check(QStringLiteral("abc"), QStringLiteral("nimiz"));
        QCOMPARE(rules.size(), 7);
        QVERIFY(!rules.at(0).met); // length
        QVERIFY(!rules.at(1).met); // upper
        QVERIFY(rules.at(2).met); // lower
        QVERIFY(!rules.at(3).met); // digit
    }

    void deniesPasswordInAnySpelling_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("plain") << "Mypassword9x";
        QTest::newRow("upper") << "MyPASSWORD9x";
        QTest::newRow("at") << "My p@ssword 9x";
        QTest::newRow("leet") << "xP@ssw0rdX1";
        QTest::newRow("dollar") << "Pa$$word9Zq";
        QTest::newRow("separated") << "Pass-word-9Zq";
        QTest::newRow("with numbers") << "Password847Ab";
    }
    void deniesPasswordInAnySpelling()
    {
        QFETCH(QString, text);
        QVERIFY2(!ok(text), qPrintable(text));
        QVERIFY(broken(text).contains(QLatin1String("password")));
    }

    void deniesAdminAndRoot()
    {
        QVERIFY(!ok(QStringLiteral("Xadmin47Qz")));
        QVERIFY(!ok(QStringLiteral("xADM1Nq47Z")));
        QVERIFY(!ok(QStringLiteral("Zr00t47Qxy")));
        QVERIFY(!ok(QStringLiteral("My-Root-47-zz")));
        QVERIFY(ok(QStringLiteral("Rooster47Qxy"))); // "roost" is not "root"
    }

    void deniesTheUserName()
    {
        QVERIFY(!ok(QStringLiteral("NiMiZ-4747-Qx"), QStringLiteral("nimiz")));
        QVERIFY(!ok(QStringLiteral("xN1m1Z-47Qz"), QStringLiteral("nimiz")));
        QVERIFY(ok(QStringLiteral("NiMiZ-4747-Qx"), QStringLiteral("someoneelse")));
        QVERIFY(ok(QStringLiteral("Qa47xZb8"), QStringLiteral("a"))); // too short to count
    }

    void deniesThreeDigitsInARow_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("up") << "Qxz456abc" << false;
        QTest::newRow("down") << "Qxz321abc" << false;
        QTest::newRow("long up") << "Qxz34567abc" << false;
        QTest::newRow("two up") << "Qxz12abcd" << true;
        QTest::newRow("two down") << "Qxz98abcd" << true;
        QTest::newRow("same") << "Qxz111abc" << true;
        QTest::newRow("jump") << "Qxz135abc" << true;
        QTest::newRow("turn") << "Qxz1213abc" << true; // up one, down one, then a jump
        QTest::newRow("turn into a run") << "Qxz1210abc" << false; // 2,1,0 counts down
        QTest::newRow("split") << "Qxz12a3bcd" << true;
        QTest::newRow("wrap") << "Qxz890abc" << true; // 8,9,0 is not a step of one
    }
    void deniesThreeDigitsInARow()
    {
        QFETCH(QString, text);
        QFETCH(bool, allowed);
        QCOMPARE(ok(text), allowed);
    }
};

QTEST_GUILESS_MAIN(TestPasswordPolicy)
#include "tst_passwordpolicy.moc"
