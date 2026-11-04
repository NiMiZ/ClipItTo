#include "NumberText.h"

#include <QTest>

using namespace clipitto::numbertext;

class TestNumberText : public QObject {
    Q_OBJECT

private slots:
    void hexOnly()
    {
        const auto plain = parse(QStringLiteral("2c"));
        QVERIFY(plain && plain->canBeHex && !plain->canBeDec);
        QCOMPARE(bitsText(plain->hexValue), QStringLiteral("5 3 2"));
        const auto prefixed = parse(QStringLiteral(" 0x2C\n"));
        QVERIFY(prefixed && prefixed->canBeHex && !prefixed->canBeDec && prefixed->hasPrefix);
        QCOMPARE(bitsText(prefixed->hexValue), QStringLiteral("5 3 2"));
    }

    void bothBases()
    {
        const auto n = parse(QStringLiteral("123"));
        QVERIFY(n && n->canBeHex && n->canBeDec);
        QCOMPARE(bitsText(n->hexValue), QStringLiteral("8 5 1 0")); // 0x123
        QCOMPARE(bitsText(n->decValue), QStringLiteral("6 5 4 3 1 0")); // 123
    }

    void notANumber()
    {
        QVERIFY(!parse(QString()));
        QVERIFY(!parse(QStringLiteral("  ")));
        QVERIFY(!parse(QStringLiteral("hello")));
        QVERIFY(!parse(QStringLiteral("12 34")));
        QVERIFY(!parse(QStringLiteral("-5")));
        QVERIFY(!parse(QStringLiteral("0x")));
        QVERIFY(!parse(QStringLiteral("0xg1")));
        QVERIFY(!parse(QStringLiteral("1.5")));
    }

    void sixtyFourBitLimit()
    {
        const auto max = parse(QStringLiteral("ffffffffffffffff"));
        QVERIFY(max && max->canBeHex && !max->canBeDec);
        QCOMPARE(setBits(max->hexValue).size(), 64);
        QVERIFY(!parse(QStringLiteral("1ffffffffffffffff")));
        QVERIFY(!parse(QStringLiteral("0x10000000000000000")));
        // Digits that fit as decimal but not as hex stay a decimal number.
        const auto dec = parse(QStringLiteral("18446744073709551615"));
        QVERIFY(dec && dec->canBeDec && !dec->canBeHex);
        QCOMPARE(dec->decValue, UINT64_MAX);
        QVERIFY(!parse(QStringLiteral("18446744073709551616")));
    }

    void zeroHasNoBits()
    {
        const auto zero = parse(QStringLiteral("0"));
        QVERIFY(zero);
        QVERIFY(setBits(zero->hexValue).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestNumberText)
#include "tst_numbertext.moc"
