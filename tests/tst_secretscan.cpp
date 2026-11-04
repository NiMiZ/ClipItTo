#include "SecretScan.h"

#include <QTest>

using namespace clipitto::secretscan;

class TestSecretScan : public QObject {
    Q_OBJECT

private slots:
    void findsKnownKinds_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("kind");
        QTest::newRow("pem") << "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----"
                             << "private key";
        QTest::newRow("pem rsa") << "-----BEGIN RSA PRIVATE KEY-----" << "private key";
        QTest::newRow("aws") << "key=AKIAIOSFODNN7EXAMPLE here" << "AWS access key";
        QTest::newRow("github") << "token ghp_0123456789abcdefghijklmnopqrstuvwxyzAB" << "GitHub token";
        QTest::newRow("github pat") << "github_pat_11ABCDEFG0123456789_abcdefghijk" << "GitHub token";
        QTest::newRow("slack") << "xoxb-1234567890-abcdefghij" << "Slack token";
        QTest::newRow("google") << "AIzaSyA1234567890abcdefghijklmnopqrstuv" << "Google API key";
        QTest::newRow("stripe") << "sk_live_0123456789abcdefABCDEF" << "Stripe key";
        QTest::newRow("anthropic") << "sk-ant-api03-abcdefghijklmnopqrstuvwxyz" << "Anthropic key";
        QTest::newRow("jwt") << "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abcdefghijkl" << "JSON web token";
        QTest::newRow("card plain") << "4111111111111111" << "card number";
        QTest::newRow("card spaced") << " 4111 1111 1111 1111 " << "card number";
        QTest::newRow("card dashed") << "5500-0000-0000-0004" << "card number";
    }
    void findsKnownKinds()
    {
        QFETCH(QString, text);
        QFETCH(QString, kind);
        QCOMPARE(kindOf(text), kind);
        QVERIFY(looksSecret(text));
    }

    void leavesOrdinaryTextAlone_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("sentence") << "Meeting at 4111 on floor 11, ask for AKIA";
        QTest::newRow("phone") << "+972 54 123 4567";
        QTest::newRow("bad luhn") << "4111111111111112";
        QTest::newRow("long number in text") << "order 4111111111111111 shipped";
        QTest::newRow("short sk") << "sk-short";
        QTest::newRow("public key") << "-----BEGIN PUBLIC KEY-----\nabc";
        QTest::newRow("uuid") << "123e4567-e89b-12d3-a456-426614174000";
        QTest::newRow("code") << "const token = getToken(); // ghp_ is the prefix";
        QTest::newRow("empty") << "";
    }
    void leavesOrdinaryTextAlone()
    {
        QFETCH(QString, text);
        QVERIFY2(!looksSecret(text), qPrintable(kindOf(text)));
    }

    void generatedPasswordShape_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("match");
        QTest::newRow("troubador") << "Tr0ub4dor&3x" << true;
        QTest::newRow("random symbols") << "k9#Vf2!qLm8@Zp" << true;
        QTest::newRow("no symbol") << "aB3dE5gH7jK9" << true;
        QTest::newRow("passphrase") << "correct-horse-battery" << false;
        QTest::newRow("word") << "hello" << false;
        QTest::newRow("long word") << "internationalization" << false;
        QTest::newRow("camelCase") << "getUserName" << false;
        QTest::newRow("camelCase digit") << "getUserName2" << false;
        QTest::newRow("snake") << "my_Variable_name2" << false;
        QTest::newRow("capitalised word number") << "Password123" << false;
        QTest::newRow("with space") << "Tr0ub4dor &3x" << false;
        QTest::newRow("two lines") << "Tr0ub4dor&3x\nk9#Vf2!qLm" << false;
        QTest::newRow("too short") << "aB3#e" << false;
        QTest::newRow("too long") << QString(65, QLatin1Char('a')) + "B3#" << false;
        QTest::newRow("url") << "https://Example.com/a?b=1&c=2" << false;
        QTest::newRow("www") << "www.Example.com/Path1" << false;
        QTest::newRow("unix path") << "/home/User1/Docs/a.txt" << false;
        QTest::newRow("home path") << "~/Projects/Foo-1/src" << false;
        QTest::newRow("windows path") << "C:\\Users\\Bob\\file1.txt" << false;
        QTest::newRow("relative path") << "src/core/SecretScan.cpp" << false;
        QTest::newRow("email") << "Jane.Doe1@example.com" << false;
        QTest::newRow("colour") << "#A1b2C3" << false;
        QTest::newRow("css colour") << "rgb(10,20,30)" << false;
        QTest::newRow("uuid") << "123e4567-e89b-12d3-a456-426614174000" << false;
        QTest::newRow("uuid upper") << "123E4567-E89B-12D3-A456-426614174000" << false;
        QTest::newRow("sha256") << "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08" << false;
        QTest::newRow("mixed hex hash") << "9F86D081884C7d659a2feaa0c55ad015" << false;
        QTest::newRow("number") << "1234567890.123" << false;
        QTest::newRow("date") << "2024-01-15T10:30:00Z" << false;
        QTest::newRow("version") << "v1.2.3-beta.4" << false;
        QTest::newRow("call") << "getToken();" << false;
        QTest::newRow("non ascii") << QString::fromUtf8("Tr0ub4dor&3\xc3\xa9x") << false;
        QTest::newRow("empty") << "" << false;
    }
    void generatedPasswordShape()
    {
        QFETCH(QString, text);
        QFETCH(bool, match);
        QCOMPARE(looksLikeGeneratedPassword(text), match);
    }

    void luhn()
    {
        QVERIFY(luhnValid(QStringLiteral("4111111111111111")));
        QVERIFY(!luhnValid(QStringLiteral("4111111111111112")));
        QVERIFY(!luhnValid(QStringLiteral("411111111111"))); // too short
    }
};

QTEST_GUILESS_MAIN(TestSecretScan)
#include "tst_secretscan.moc"
