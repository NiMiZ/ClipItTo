#include "TestDb.h"

#include <QSet>
#include <QTest>

using clipitto::Entry;
using clipitto::db::Repository;

namespace {

QList<qint64> ids(const std::vector<Entry>& entries)
{
    QList<qint64> result;
    for (const Entry& e : entries) {
        result << e.id;
    }
    return result;
}

QSet<qint64> idSet(const std::vector<Entry>& entries)
{
    const auto list = ids(entries);
    return {list.begin(), list.end()};
}

} // namespace

class TestRepository : public QObject {
    Q_OBJECT

private slots:
    void ingestInsertsNewText()
    {
        TestDb t;
        const auto r = t.repo->ingest(QStringLiteral("hello"));
        QVERIFY(r.inserted);
        const auto e = t.repo->get(r.id);
        QVERIFY(e);
        QCOMPARE(e->content, QStringLiteral("hello"));
        QVERIFY(e->note.isEmpty());
        QVERIFY(e->group.isEmpty());
        QVERIFY(!e->favorite);
        QVERIFY(e->createdAt.isValid());
        QCOMPARE(e->createdAt, e->lastUsed);
    }

    void ingestDuplicateBumpsLastUsed()
    {
        TestDb t;
        const auto first = t.repo->ingest(QStringLiteral("same"));
        t.repo->ingest(QStringLiteral("other"));
        const auto second = t.repo->ingest(QStringLiteral("same"));

        QVERIFY(!second.inserted);
        QCOMPARE(second.id, first.id);
        QCOMPARE(t.repo->count(), 2);

        const auto e = t.repo->get(first.id);
        QVERIFY(e->lastUsed > e->createdAt);
        QCOMPARE(t.repo->recent(1, false).front().id, first.id);
    }

    void ingestKeepsWhitespaceDistinct()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("abc"));
        QVERIFY(t.repo->ingest(QStringLiteral("abc\n")).inserted);
        QCOMPARE(t.repo->count(), 2);
    }

    void recentIsNewestFirstAndCanExcludeSaved()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto c = t.repo->ingest(QStringLiteral("c")).id;
        t.repo->setFavorite(b, true);

        QCOMPARE(ids(t.repo->recent(10, false)), (QList<qint64>{c, b, a}));
        QCOMPARE(ids(t.repo->recent(10, true)), (QList<qint64>{c, a}));
        QCOMPARE(ids(t.repo->recent(1, true)), (QList<qint64>{c}));
    }

    void savedListsFavoritesBeforeGrouped()
    {
        TestDb t;
        const auto fav = t.repo->ingest(QStringLiteral("fav")).id;
        const auto grouped = t.repo->ingest(QStringLiteral("grouped")).id;
        t.repo->ingest(QStringLiteral("plain"));
        t.repo->setFavorite(fav, true);
        t.repo->setGroup(grouped, QStringLiteral("Work"));

        QCOMPARE(ids(t.repo->saved(10)), (QList<qint64>{fav, grouped}));
    }

    void touchMovesEntryToTop()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        t.repo->ingest(QStringLiteral("b"));
        t.repo->touch(a);
        QCOMPARE(t.repo->recent(1, false).front().id, a);
    }

    void groupAndNoteEditing()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("kubectl get pods")).id;

        t.repo->setGroup(id, QStringLiteral("  Commands "));
        t.repo->setNote(id, QStringLiteral("list pods"));
        auto e = t.repo->get(id);
        QCOMPARE(e->group, QStringLiteral("Commands"));
        QCOMPARE(e->note, QStringLiteral("list pods"));
        QCOMPARE(e->displayTitle(), QStringLiteral("list pods"));
        QVERIFY(e->isSaved());

        t.repo->setGroup(id, QStringLiteral("   "));
        t.repo->setNote(id, QString());
        e = t.repo->get(id);
        QVERIFY(e->group.isEmpty());
        QVERIFY(e->note.isEmpty());
        QCOMPARE(e->displayTitle(), QStringLiteral("kubectl get pods"));
        QVERIFY(!e->isSaved());
        QCOMPARE(t.repo->countUnsaved(), 1);
    }

    void groupsAreDistinctAndSorted()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto c = t.repo->ingest(QStringLiteral("c")).id;
        t.repo->setGroup(a, QStringLiteral("work"));
        t.repo->setGroup(b, QStringLiteral("Code"));
        t.repo->setGroup(c, QStringLiteral("work"));
        QCOMPARE(t.repo->groups(), (QStringList{QStringLiteral("Code"), QStringLiteral("work")}));
    }

    void removeDeletesRowAndIndex()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("secret token value")).id;
        QCOMPARE(t.repo->search(QStringLiteral("token")).size(), 1u);
        t.repo->remove(id);
        QVERIFY(!t.repo->get(id));
        QVERIFY(t.repo->search(QStringLiteral("token")).empty());
    }

    void pruneKeepsSavedItems()
    {
        TestDb t;
        for (int i = 0; i < 50; ++i) {
            const auto id = t.repo->ingest(QStringLiteral("saved %1").arg(i)).id;
            if (i % 2) {
                t.repo->setFavorite(id, true);
            } else {
                t.repo->setGroup(id, QStringLiteral("Snippets"));
            }
        }
        for (int i = 0; i < 1100; ++i) {
            t.repo->ingest(QStringLiteral("item %1").arg(i));
        }

        QCOMPARE(t.repo->prune(1000), 100);
        QCOMPARE(t.repo->countUnsaved(), 1000);
        QCOMPARE(t.repo->count(), 1050);
        QCOMPARE(t.repo->saved(100).size(), 50u);

        // The oldest unsaved items went first.
        const auto unsaved = t.repo->recent(1000, true);
        QCOMPARE(unsaved.front().content, QStringLiteral("item 1099"));
        QCOMPARE(unsaved.back().content, QStringLiteral("item 100"));
    }

    void pruneUnderLimitIsNoop()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("a"));
        t.repo->ingest(QStringLiteral("b"));
        QCOMPARE(t.repo->prune(10), 0);
        QCOMPARE(t.repo->count(), 2);
    }

    void searchBlankQueryReturnsNothing()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("anything"));
        QVERIFY(t.repo->search(QString()).empty());
        QVERIFY(t.repo->search(QStringLiteral("   ")).empty());
    }

    void searchRanksSavedFirst()
    {
        TestDb t;
        const auto saved = t.repo->ingest(QStringLiteral("deploy staging")).id;
        const auto plain = t.repo->ingest(QStringLiteral("deploy production now")).id;
        t.repo->setGroup(saved, QStringLiteral("Commands"));

        QCOMPARE(ids(t.repo->search(QStringLiteral("deploy"))), (QList<qint64>{saved, plain}));
        QCOMPARE(ids(t.repo->search(QStringLiteral("de"))), (QList<qint64>{saved, plain}));
    }

    void searchRanksNoteAboveContent()
    {
        TestDb t;
        const auto byContent = t.repo->ingest(QStringLiteral("rsync backup script")).id;
        const auto byNote = t.repo->ingest(QStringLiteral("tar czf /tmp/x.tgz ~/docs")).id;
        t.repo->setNote(byNote, QStringLiteral("backup"));
        t.repo->touch(byContent); // most recent, so recency alone would put it first

        QCOMPARE(ids(t.repo->search(QStringLiteral("backup"))), (QList<qint64>{byNote, byContent}));
        QCOMPARE(ids(t.repo->search(QStringLiteral("backup"), 100, Repository::SearchMode::ForceLike)),
                 (QList<qint64>{byNote, byContent}));
    }

    void searchMatchesGroupName()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("ssh -J bastion app")).id;
        t.repo->ingest(QStringLiteral("unrelated"));
        t.repo->setGroup(id, QStringLiteral("Servers"));
        QCOMPARE(ids(t.repo->search(QStringLiteral("servers"))), (QList<qint64>{id}));
    }

    void searchIsCaseInsensitiveSubstring()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("https://github.com/NiMiZ/ClipItTo")).id;
        QCOMPARE(ids(t.repo->search(QStringLiteral("clipitto"))), (QList<qint64>{id}));
        QCOMPARE(ids(t.repo->search(QStringLiteral("HUB.COM"))), (QList<qint64>{id}));
    }

    void searchMultipleTokensAreAnded()
    {
        TestDb t;
        const auto both = t.repo->ingest(QStringLiteral("git push origin main")).id;
        t.repo->ingest(QStringLiteral("git pull"));
        QCOMPARE(ids(t.repo->search(QStringLiteral("push git"))), (QList<qint64>{both}));
        // Mixed short/long tokens use the LIKE path.
        QCOMPARE(ids(t.repo->search(QStringLiteral("git pu"))).size(), 2);
    }

    void searchLimitIsHonored()
    {
        TestDb t;
        for (int i = 0; i < 30; ++i) {
            t.repo->ingest(QStringLiteral("line %1").arg(i));
        }
        QCOMPARE(t.repo->search(QStringLiteral("line"), 5).size(), 5u);
        QCOMPARE(t.repo->search(QStringLiteral("li"), 5).size(), 5u);
    }

    void searchTreatsSyntaxLiterally_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("content");
        QTest::newRow("quote") << QStringLiteral("say \"hi\"") << QStringLiteral("echo say \"hi\" there");
        QTest::newRow("star") << QStringLiteral("a*b") << QStringLiteral("x a*b y");
        QTest::newRow("near") << QStringLiteral("NEAR(") << QStringLiteral("call NEAR( x");
        QTest::newRow("minus") << QStringLiteral("-rf") << QStringLiteral("rm -rf build");
        QTest::newRow("column filter") << QStringLiteral("content:abc") << QStringLiteral("content:abc");
        QTest::newRow("percent") << QStringLiteral("50%") << QStringLiteral("down 50% today");
        QTest::newRow("underscore") << QStringLiteral("a_b") << QStringLiteral("a_b");
        QTest::newRow("backslash") << QStringLiteral("C:\\") << QStringLiteral("C:\\Users");
    }

    void searchTreatsSyntaxLiterally()
    {
        QFETCH(QString, query);
        QFETCH(QString, content);
        TestDb t;
        const auto id = t.repo->ingest(content).id;
        t.repo->ingest(QStringLiteral("decoy axb 50x a-b C:/ NEAR"));
        QCOMPARE(ids(t.repo->search(query)), (QList<qint64>{id}));
        QCOMPARE(ids(t.repo->search(query, 100, Repository::SearchMode::ForceLike)), (QList<qint64>{id}));
    }

    void ftsAndLikeAgree()
    {
        TestDb t;
        const QStringList corpus = {
            QStringLiteral("kubectl rollout restart deploy/api"), QStringLiteral("SELECT * FROM users"),
            QStringLiteral("ssh deploy@10.0.0.1"),                QStringLiteral("Über-schnell café"),
            QStringLiteral("docker compose up -d"),               QStringLiteral("git commit -m 'deploy fix'"),
        };
        for (const QString& text : corpus) {
            t.repo->ingest(text);
        }
        const auto id = t.repo->recent(1, false).front().id;
        t.repo->setNote(id, QStringLiteral("commit with message"));

        for (const QString& q : {QStringLiteral("dep"), QStringLiteral("SEL"), QStringLiteral("café"),
                                 QStringLiteral("com"), QStringLiteral("10.0"), QStringLiteral("ploy res")}) {
            QCOMPARE(idSet(t.repo->search(q)), idSet(t.repo->search(q, 100, Repository::SearchMode::ForceLike)));
        }
    }

    void editedNoteIsSearchable()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("0x7fff5fbff8a8")).id;
        t.repo->setNote(id, QStringLiteral("stack pointer"));
        QCOMPARE(ids(t.repo->search(QStringLiteral("pointer"))), (QList<qint64>{id}));
        t.repo->setNote(id, QStringLiteral("frame base"));
        QVERIFY(t.repo->search(QStringLiteral("pointer")).empty());
        QCOMPARE(ids(t.repo->search(QStringLiteral("frame"))), (QList<qint64>{id}));
    }
};

QTEST_GUILESS_MAIN(TestRepository)
#include "tst_repository.moc"
