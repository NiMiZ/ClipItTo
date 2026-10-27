#include "SearchFilter.h"

#include <QTest>
#include <QTimeZone>

using clipitto::SearchFilter;

class TestSearchFilter : public QObject {
    Q_OBJECT

    const QDateTime now = QDateTime(QDate(2026, 10, 27), QTime(15, 30), QTimeZone::utc());

private slots:
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
