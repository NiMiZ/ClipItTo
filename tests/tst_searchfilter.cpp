#include "RegexSearch.h"
#include "SearchFilter.h"
#include "SearchHelp.h"

#include <QTest>
#include <QTimeZone>

using clipitto::SearchFilter;

class TestSearchFilter : public QObject {
    Q_OBJECT

    const QDateTime now = QDateTime(QDate(2026, 10, 27), QTime(15, 30), QTimeZone::utc());

private slots:
    // The "/?" help is generated from one table; it must list exactly the prefixes the parsers really act on.
    void helpTableMatchesTheFilterParser()
    {
        using namespace clipitto::searchhelp;
        QStringList helped = prefixes(Kind::Filter);
        QVERIFY(!helped.isEmpty());
        for (const Row& row : rows(QStringLiteral("p[]"))) {
            QVERIFY2(!row.example.isEmpty() && !row.meaning.isEmpty() && !row.prefix.isEmpty(), qPrintable(row.prefix));
            if (row.kind == Kind::Filter) {
                QVERIFY2(row.example.startsWith(row.prefix), qPrintable(row.prefix));
                const SearchFilter f = SearchFilter::parse(row.example, now);
                QVERIFY2(f.hasFilters() && f.words.isEmpty(), qPrintable(row.prefix)); // a real filter, not a word
            }
        }
        // The other way round: every key (1 to 3 letters, plus the longer words below) that the parser acts on is in
        // the table, apart from the alias image: (the same as img:).
        QStringList acted;
        const auto probe = [&](const QString& key) {
            if (SearchFilter::parse(key + QLatin1Char(':'), now).hasFilters()) {
                acted << key + QLatin1Char(':');
            }
        };
        QString key;
        for (int len = 1; len <= 3; ++len) {
            const int total = len == 1 ? 26 : len == 2 ? 676 : 17576;
            for (int n = 0; n < total; ++n) {
                key.clear();
                int rest = n;
                for (int i = 0; i < len; ++i) {
                    key.prepend(QChar(u'a' + rest % 26));
                    rest /= 26;
                }
                probe(key);
            }
        }
        for (const QString& longer : {QStringLiteral("image"), QStringLiteral("today"), QStringLiteral("since"), QStringLiteral("group"),
                                      QStringLiteral("star"), QStringLiteral("note"), QStringLiteral("text"), QStringLiteral("pin"),
                                      QStringLiteral("recent"), QStringLiteral("saved"), QStringLiteral("type"), QStringLiteral("date"),
                                      QStringLiteral("before"), QStringLiteral("after"), QStringLiteral("app"), QStringLiteral("source"),
                                      QStringLiteral("size"), QStringLiteral("file"), QStringLiteral("url"), QStringLiteral("link"),
                                      QStringLiteral("color"), QStringLiteral("lang"), QStringLiteral("password"), QStringLiteral("secret"),
                                      QStringLiteral("re"), QStringLiteral("regex"), QStringLiteral("pw")}) {
            probe(longer);
        }
        acted.removeDuplicates();
        acted.removeAll(QStringLiteral("image:"));
        acted.sort();
        helped.sort();
        QVERIFY2(acted == helped, qPrintable(acted.join(QLatin1Char(' ')) + QStringLiteral(" | ") + helped.join(QLatin1Char(' '))));
        // re: and pw: are not SearchFilter keys (their own parsers take them).
        QVERIFY(!SearchFilter::parse(QStringLiteral("re:x pw:x"), now).hasFilters());
    }

    void helpTableMatchesTheRegexParser()
    {
        using namespace clipitto::searchhelp;
        const QStringList regex = prefixes(Kind::Regex);
        QCOMPARE(regex, QStringList{QStringLiteral("re:")});
        for (const Row& row : rows(QString())) {
            if (row.kind == Kind::Regex || row.example.startsWith(QLatin1String("re:"))) {
                const clipitto::RegexQuery q = clipitto::RegexQuery::parse(row.example);
                QVERIFY2(q.present && q.error.isEmpty(), qPrintable(row.example));
            }
        }
        // re:"a b" really takes the spaces, (?-i) really switches case matching on.
        QCOMPARE(clipitto::RegexQuery::parse(QStringLiteral("re:\"a b\"")).pattern, QStringLiteral("a b"));
        QVERIFY(clipitto::RegexQuery::parse(QStringLiteral("re:abc")).regex.match(QStringLiteral("xABCx")).hasMatch());
        QVERIFY(!clipitto::RegexQuery::parse(QStringLiteral("re:(?-i)abc")).regex.match(QStringLiteral("xABCx")).hasMatch());
    }

    void helpQueryIsExactlySlashQuestionMark()
    {
        using clipitto::searchhelp::isHelpQuery;
        QVERIFY(isHelpQuery(QStringLiteral("/?")));
        QVERIFY(isHelpQuery(QStringLiteral("  /?  ")));
        QVERIFY(!isHelpQuery(QStringLiteral("/? x")));
        QVERIFY(!isHelpQuery(QStringLiteral("/")));
        QVERIFY(!isHelpQuery(QStringLiteral("a/?")));
        QVERIFY(!isHelpQuery(QString()));
    }

    void helpTextHasEveryRowAndTheKeyword()
    {
        using namespace clipitto::searchhelp;
        const QString t = text(QStringLiteral("p[]"));
        for (const Row& row : rows(QStringLiteral("p[]"))) {
            QVERIFY2(t.contains(row.example) && t.contains(row.meaning), qPrintable(row.prefix));
        }
        QVERIFY(prefixes(Kind::Password, QStringLiteral("p[]")) == (QStringList{QStringLiteral("pw:"), QStringLiteral("p[]")}));
        QVERIFY(prefixes(Kind::Password, QStringLiteral("pw:")) == QStringList{QStringLiteral("pw:")}); // no duplicate row
        QVERIFY(prefixes(Kind::Password, QString()) == QStringList{QStringLiteral("pw:")});
    }

    void plainWordsAreNotFilters()
    {
        const SearchFilter f = SearchFilter::parse(QStringLiteral("hello world"), now);
        QVERIFY(!f.hasFilters());
        QCOMPARE(f.words, QStringList({QStringLiteral("hello"), QStringLiteral("world")}));
    }

    void unknownKeysAndUrlsStayWords()
    {
        const SearchFilter f = SearchFilter::parse(QStringLiteral("http://x.test foo:bar since:soon img:cat"), now);
        QVERIFY(!f.hasFilters());
        QCOMPARE(f.words.size(), 4);
    }

    void flagsAndValues()
    {
        const SearchFilter f = SearchFilter::parse(QStringLiteral("IMG: pin: star: group:Work note:todo budget"), now);
        QVERIFY(f.images);
        QVERIFY(f.pinned);
        QVERIFY(f.starred);
        QCOMPARE(f.group.value_or(QString()), QStringLiteral("Work"));
        QCOMPARE(f.note.value_or(QString()), QStringLiteral("todo"));
        QCOMPARE(f.words, QStringList({QStringLiteral("budget")}));

        const SearchFilter any = SearchFilter::parse(QStringLiteral("group: note: text:"), now);
        QVERIFY(any.group && any.group->isEmpty());
        QVERIFY(any.note && any.note->isEmpty());
        QVERIFY(any.textOnly);
    }

    void sinceValues()
    {
        const auto since = [this](const char* q) { return SearchFilter::parse(QString::fromLatin1(q), now).since; };
        QCOMPARE(*since("today:"), QDateTime(QDate(2026, 10, 27), QTime(0, 0), QTimeZone::utc()));
        QCOMPARE(*since("since:yesterday"), QDateTime(QDate(2026, 10, 26), QTime(0, 0), QTimeZone::utc()));
        QCOMPARE(*since("since:2h"), now.addSecs(-7200));
        QCOMPARE(*since("since:30m"), now.addSecs(-1800));
        QCOMPARE(*since("since:3d"), now.addDays(-3));
        QCOMPARE(*since("since:2w"), now.addDays(-14));
        QCOMPARE(*since("since:2026-10-01"), QDateTime(QDate(2026, 10, 1), QTime(0, 0), QTimeZone::utc()));
        QVERIFY(!since("since:soon"));
    }
};

QTEST_GUILESS_MAIN(TestSearchFilter)
#include "tst_searchfilter.moc"
