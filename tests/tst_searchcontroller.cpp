#include "Config.h"
#include "HistoryModel.h"
#include "SearchController.h"
#include "TestDb.h"

#include <QSignalSpy>
#include <QTest>

using clipitto::Config;
using clipitto::ui::HistoryModel;
using clipitto::ui::SearchController;

namespace {

QStringList titles(const HistoryModel& m)
{
    QStringList result;
    for (int i = 0; i < m.rowCount(); ++i) {
        const QString t = m.index(i).data(HistoryModel::TitleRole).toString();
        result << (m.index(i).data(HistoryModel::IsHeaderRole).toBool() ? QLatin1Char('#') + t : t);
    }
    return result;
}

} // namespace

class TestSearchController : public QObject {
    Q_OBJECT

private slots:
    void defaultViewShowsSavedThenRecent()
    {
        TestDb t;
        const auto fav = t.repo->ingest(QStringLiteral("fav")).id;
        t.repo->ingest(QStringLiteral("one"));
        t.repo->ingest(QStringLiteral("two"));
        t.repo->setFavorite(fav, true);

        HistoryModel m;
        SearchController c(*t.repo, m);
        c.refresh();
        QCOMPARE(titles(m), (QStringList{QStringLiteral("#Saved"), QStringLiteral("fav"), QStringLiteral("#Recent"),
                                         QStringLiteral("two"), QStringLiteral("one")}));
    }

    void defaultViewWithoutSavedHasNoHeaders()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("one"));
        t.repo->ingest(QStringLiteral("two"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.refresh();
        QCOMPARE(titles(m), (QStringList{QStringLiteral("two"), QStringLiteral("one")}));
    }

    void savedOnTopCanBeDisabled()
    {
        TestDb t;
        const auto fav = t.repo->ingest(QStringLiteral("fav")).id;
        t.repo->ingest(QStringLiteral("one"));
        t.repo->setFavorite(fav, true);

        Config cfg;
        cfg.showSavedOnTop = false;
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.applyConfig(cfg);
        c.refresh();
        QCOMPARE(titles(m), (QStringList{QStringLiteral("one"), QStringLiteral("fav")}));
    }

    void recentCountLimitsDefaultView()
    {
        TestDb t;
        for (int i = 0; i < 30; ++i) {
            t.repo->ingest(QStringLiteral("clip %1").arg(i));
        }
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.refresh();
        QCOMPARE(m.rowCount(), 20);
        QCOMPARE(m.entryAt(0)->content, QStringLiteral("clip 29"));
    }

    void keystrokesAreDebouncedIntoOneQuery()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("deploy staging"));
        t.repo->ingest(QStringLiteral("other"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QSignalSpy updated(&c, &SearchController::updated);

        for (const char* q : {"d", "de", "dep", "depl", "deploy"}) {
            c.setQuery(QString::fromLatin1(q));
        }
        QCOMPARE(c.queriesRun(), 0);
        QVERIFY(c.isPending());
        QTRY_COMPARE(c.queriesRun(), 1);
        QCOMPARE(updated.size(), 1);
        QCOMPARE(titles(m), (QStringList{QStringLiteral("deploy staging")}));
    }

    void clearingTheQueryIsImmediate()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("alpha"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.setQuery(QStringLiteral("zzz"));
        c.setQuery(QString());
        QCOMPARE(c.queriesRun(), 1); // ran synchronously, pending search cancelled
        QVERIFY(!c.isPending());
        QCOMPARE(titles(m), (QStringList{QStringLiteral("alpha")}));
    }

    void searchListsSavedMatchesFirstWithoutHeaders()
    {
        TestDb t;
        const auto grouped = t.repo->ingest(QStringLiteral("deploy prod")).id;
        t.repo->ingest(QStringLiteral("deploy dev"));
        t.repo->setGroup(grouped, QStringLiteral("Ops"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.setQuery(QStringLiteral("deploy"));
        c.refresh();
        QCOMPARE(titles(m), (QStringList{QStringLiteral("deploy prod"), QStringLiteral("deploy dev")}));
    }
};

QTEST_GUILESS_MAIN(TestSearchController)
#include "tst_searchcontroller.moc"
