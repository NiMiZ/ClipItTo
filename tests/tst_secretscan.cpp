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

    void luhn()
    {
        QVERIFY(luhnValid(QStringLiteral("4111111111111111")));
        QVERIFY(!luhnValid(QStringLiteral("4111111111111112")));
        QVERIFY(!luhnValid(QStringLiteral("411111111111"))); // too short
    }
};

QTEST_GUILESS_MAIN(TestSecretScan)
#include "tst_secretscan.moc"
