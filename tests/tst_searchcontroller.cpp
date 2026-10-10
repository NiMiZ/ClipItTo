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
        const QString t = m.index(i, 0).data(HistoryModel::TitleRole).toString();
        result << (m.index(i, 0).data(HistoryModel::IsHeaderRole).toBool() ? QLatin1Char('#') + t : t);
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
        QCOMPARE(m.entryAt(m.index(0, 0))->content, QStringLiteral("clip 29"));
    }

    void groupTreeComesFirstWithSubGroupsThenCopies()
    {
        TestDb t;
        const auto deploy = t.repo->ingest(QStringLiteral("deploy script")).id;
        const auto ssh = t.repo->ingest(QStringLiteral("ssh prod")).id;
        t.repo->ingest(QStringLiteral("loose"));
        t.repo->setGroup(deploy, QStringLiteral("Work"));
        t.repo->setGroup(ssh, QStringLiteral("Work/Servers"));
        t.repo->createGroup(QStringLiteral("Empty"));

        HistoryModel m;
        SearchController c(*t.repo, m);
        c.refresh();
        QCOMPARE(titles(m), (QStringList{QStringLiteral("Groups"), QStringLiteral("#Saved"), QStringLiteral("ssh prod"),
                                         QStringLiteral("deploy script"), QStringLiteral("#Recent"),
                                         QStringLiteral("loose")}));

        const QModelIndex root = m.groupsRoot();
        QCOMPARE(root.row(), 0);
        QCOMPARE(root.data(HistoryModel::CountRole).toInt(), 2);
        QCOMPARE(m.rowCount(root), 2);
        QCOMPARE(m.index(0, 0, root).data(HistoryModel::TitleRole).toString(), QStringLiteral("Empty"));
        QVERIFY(!m.isExpandable(m.index(0, 0, root))); // nothing inside

        const QModelIndex work = m.findGroup(QStringLiteral("Work"));
        QCOMPARE(work.parent(), root);
        QCOMPARE(work.data(HistoryModel::CountRole).toInt(), 2); // its own copy + Servers' copy
        QCOMPARE(m.rowCount(work), 2);
        QCOMPARE(m.kindAt(m.index(0, 0, work)), HistoryModel::Kind::Group); // sub-groups first
        QCOMPARE(m.index(0, 0, work).data(HistoryModel::TitleRole).toString(), QStringLiteral("Servers"));
        QCOMPARE(m.entryAt(m.index(1, 0, work))->id, deploy);

        const QModelIndex servers = m.findGroup(QStringLiteral("Work/Servers"));
        QCOMPARE(m.entryAt(m.index(0, 0, servers))->id, ssh);
        QCOMPARE(m.findEntry(ssh, QStringLiteral("Work/Servers")).parent(), servers);
        QCOMPARE(m.findEntry(ssh).parent(), QModelIndex()); // sections are preferred
    }

    void noGroupsMeansNoGroupsRow()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("a"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.refresh();
        QVERIFY(!m.groupsRoot().isValid());
    }

    void searchResultsHaveNoGroupTree()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("ssh prod")).id;
        t.repo->setGroup(id, QStringLiteral("Servers"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.setQuery(QStringLiteral("ssh"));
        c.refresh();
        QVERIFY(!m.groupsRoot().isValid());
        QCOMPARE(titles(m), QStringList{QStringLiteral("ssh prod")});
    }

    void previewLinesSetting()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("line one\nline two\n\n  line three\nline four"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.refresh();
        QCOMPARE(m.index(0, 0).data(HistoryModel::TitleRole).toString(),
                 QStringLiteral("line one line two line three line four"));
        QVERIFY(m.index(0, 0).data(HistoryModel::PreviewRole).toString().isEmpty());

        Config cfg;
        cfg.previewLines = 3;
        c.applyConfig(cfg);
        c.refresh();
        QCOMPARE(m.index(0, 0).data(HistoryModel::TitleRole).toString(), QStringLiteral("line one"));
        QCOMPARE(m.index(0, 0).data(HistoryModel::PreviewRole).toString(),
                 QStringLiteral("line two\nline three\u2026"));
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
