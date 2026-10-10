#include "db/FtsQuery.h"

#include <QTest>

namespace fts = clipitto::db::fts;

class TestFtsQuery : public QObject {
    Q_OBJECT

private slots:
    void tokenizeSplitsOnAnyWhitespace()
    {
        QCOMPARE(fts::tokenize(QStringLiteral("  git\tpush \n origin ")),
                 (QStringList{QStringLiteral("git"), QStringLiteral("push"), QStringLiteral("origin")}));
        QVERIFY(fts::tokenize(QStringLiteral("   ")).isEmpty());
        QVERIFY(fts::tokenize(QString()).isEmpty());
    }

    void trigramNeedsThreeCodePointsPerToken()
    {
        QVERIFY(fts::canUseTrigram({QStringLiteral("abc")}));
        QVERIFY(fts::canUseTrigram({QStringLiteral("abc"), QStringLiteral("défg")}));
        QVERIFY(!fts::canUseTrigram({QStringLiteral("ab")}));
        QVERIFY(!fts::canUseTrigram({QStringLiteral("abc"), QStringLiteral("x")}));
        QVERIFY(!fts::canUseTrigram({}));
        // Two emoji are 4 UTF-16 units but only 2 code points.
        QVERIFY(!fts::canUseTrigram({QStringLiteral("😀😀")}));
        QVERIFY(fts::canUseTrigram({QStringLiteral("😀😀😀")}));
    }

    void matchExpressionQuotesEveryToken()
    {
        QCOMPARE(fts::matchExpression({QStringLiteral("foo"), QStringLiteral("bar")}),
                 QStringLiteral("\"foo\" \"bar\""));
        QCOMPARE(fts::matchExpression({QStringLiteral("NEAR(a")}), QStringLiteral("\"NEAR(a\""));
        QCOMPARE(fts::matchExpression({QStringLiteral("col:x*")}), QStringLiteral("\"col:x*\""));
    }

    void matchExpressionDoublesQuotes()
    {
        QCOMPARE(fts::matchExpression({QStringLiteral("say\"hi")}), QStringLiteral("\"say\"\"hi\""));
        QCOMPARE(fts::matchExpression({QStringLiteral("\"")}), QStringLiteral("\"\"\"\""));
    }

    void likePatternEscapesWildcards()
    {
        QCOMPARE(fts::likePattern(QStringLiteral("abc")), QStringLiteral("%abc%"));
        QCOMPARE(fts::likePattern(QStringLiteral("50%")), QStringLiteral("%50\\%%"));
        QCOMPARE(fts::likePattern(QStringLiteral("a_b")), QStringLiteral("%a\\_b%"));
        QCOMPARE(fts::likePattern(QStringLiteral("C:\\")), QStringLiteral("%C:\\\\%"));
    }
};

QTEST_GUILESS_MAIN(TestFtsQuery)
#include "tst_ftsquery.moc"
