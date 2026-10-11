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

    void pinnedListsPinsInPinOrder()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto c = t.repo->ingest(QStringLiteral("c")).id;
        QCOMPARE(t.repo->setPinned(b, true), Repository::Change::Done);
        QCOMPARE(t.repo->setPinned(a, true), Repository::Change::Done);
        QCOMPARE(ids(t.repo->pinned(10)), (QList<qint64>{b, a})); // pin order, not age
        QVERIFY(t.repo->get(a)->pinned());
        QVERIFY(!t.repo->get(c)->pinned());

        // Pinned copies are saved: out of Recent's unsaved list.
        QCOMPARE(ids(t.repo->recent(10, true)), (QList<qint64>{c}));

        QCOMPARE(t.repo->setPinned(b, false), Repository::Change::Done);
        QCOMPARE(ids(t.repo->pinned(10)), (QList<qint64>{a}));
        QCOMPARE(t.repo->setPinned(b, true), Repository::Change::Done); // back in, at the end
        QCOMPARE(ids(t.repo->pinned(10)), (QList<qint64>{a, b}));
    }

    void pinsAreCappedAtTen()
    {
        TestDb t;
        QList<qint64> pinned;
        for (int i = 0; i < 10; ++i) {
            const auto id = t.repo->ingest(QStringLiteral("pin %1").arg(i)).id;
            QCOMPARE(t.repo->setPinned(id, true), Repository::Change::Done);
            pinned << id;
        }
        const auto extra = t.repo->ingest(QStringLiteral("one too many")).id;
        QCOMPARE(t.repo->setPinned(extra, true), Repository::Change::PinsFull);
        QVERIFY(!t.repo->get(extra)->pinned());
        QCOMPARE(ids(t.repo->pinned(20)), pinned);

        // Unpinning one makes room again.
        t.repo->setPinned(pinned.first(), false);
        QCOMPARE(t.repo->setPinned(extra, true), Repository::Change::Done);
    }

    void movePinnedReorders()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto c = t.repo->ingest(QStringLiteral("c")).id;
        for (const auto id : {a, b, c}) {
            t.repo->setPinned(id, true);
        }
        QVERIFY(t.repo->movePinned(c, -1));
        QCOMPARE(ids(t.repo->pinned(10)), (QList<qint64>{a, c, b}));
        QVERIFY(t.repo->movePinned(a, 1));
        QCOMPARE(ids(t.repo->pinned(10)), (QList<qint64>{c, a, b}));
        QVERIFY(!t.repo->movePinned(c, -1)); // already first
        QVERIFY(!t.repo->movePinned(b, 1)); // already last
        const auto loose = t.repo->ingest(QStringLiteral("not pinned")).id;
        QVERIFY(!t.repo->movePinned(loose, 1));
        QCOMPARE(ids(t.repo->pinned(10)), (QList<qint64>{c, a, b}));
    }

    void favoritesAreStarredCopiesWithoutAGroup()
    {
        TestDb t;
        const auto loose = t.repo->ingest(QStringLiteral("loose star")).id;
        const auto inGroup = t.repo->ingest(QStringLiteral("star in Work")).id;
        t.repo->ingest(QStringLiteral("plain"));
        t.repo->setFavorite(loose, true);
        t.repo->setGroup(inGroup, QStringLiteral("Work"));
        t.repo->setFavorite(inGroup, true);

        QCOMPARE(ids(t.repo->favorites()), (QList<qint64>{loose}));
        // Starred copies are listed first in their group.
        const auto other = t.repo->ingest(QStringLiteral("newer in Work")).id;
        t.repo->setGroup(other, QStringLiteral("Work"));
        QCOMPARE(ids(t.repo->groupedEntries()), (QList<qint64>{inGroup, other}));

        // Removing the group makes its starred copy a favorite.
        QCOMPARE(t.repo->setGroup(inGroup, QString()), Repository::Change::Done);
        QCOMPARE(idSet(t.repo->favorites()), (QSet<qint64>{loose, inGroup}));
    }

    void favoritesLimitIsEnforced()
    {
        TestDb t;
        t.repo->setLimits({/*favorites=*/2, /*starsPerGroup=*/10, /*pins=*/10});
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto c = t.repo->ingest(QStringLiteral("c")).id;
        QCOMPARE(t.repo->setFavorite(a, true), Repository::Change::Done);
        QCOMPARE(t.repo->setFavorite(b, true), Repository::Change::Done);
        QCOMPARE(t.repo->setFavorite(c, true), Repository::Change::FavoritesFull);
        QVERIFY(!t.repo->get(c)->favorite);

        // A starred copy in a group can't drop into a full Favorites...
        t.repo->setGroup(c, QStringLiteral("Work"));
        QCOMPARE(t.repo->setFavorite(c, true), Repository::Change::Done); // Work has room
        QCOMPARE(t.repo->setGroup(c, QString()), Repository::Change::FavoritesFull);
        QCOMPARE(t.repo->get(c)->group, QStringLiteral("Work"));

        // ...and a lowered limit keeps what is already there.
        t.repo->setLimits({1, 10, 10});
        QCOMPARE(t.repo->favorites().size(), 2u);
        QCOMPARE(t.repo->setFavorite(a, false), Repository::Change::Done); // unstarring always works
    }

    void starsPerGroupAreCapped()
    {
        TestDb t;
        for (int i = 0; i < 10; ++i) {
            const auto id = t.repo->ingest(QStringLiteral("work %1").arg(i)).id;
            t.repo->setGroup(id, QStringLiteral("Work"));
            QCOMPARE(t.repo->setFavorite(id, true), Repository::Change::Done);
        }
        const auto extra = t.repo->ingest(QStringLiteral("eleventh")).id;
        t.repo->setGroup(extra, QStringLiteral("Work"));
        QCOMPARE(t.repo->setFavorite(extra, true), Repository::Change::GroupStarsFull);

        // Sub-groups count separately.
        t.repo->setGroup(extra, QStringLiteral("Work/Servers"));
        QCOMPARE(t.repo->setFavorite(extra, true), Repository::Change::Done);

        // A starred copy can't move into a group that is full of stars; it stays put.
        QCOMPARE(t.repo->setGroup(extra, QStringLiteral("Work")), Repository::Change::GroupStarsFull);
        QCOMPARE(t.repo->get(extra)->group, QStringLiteral("Work/Servers"));
        // An unstarred copy can.
        const auto plain = t.repo->ingest(QStringLiteral("plain")).id;
        QCOMPARE(t.repo->setGroup(plain, QStringLiteral("Work")), Repository::Change::Done);
    }

    void groupsKeepTheUsersOrder()
    {
        TestDb t;
        for (const char* path : {"Beta", "alpha", "Gamma", "Beta/Two", "Beta/One"}) {
            t.repo->createGroup(QString::fromUtf8(path));
        }
        // By name until the user orders them; parents before children.
        QCOMPARE(t.repo->groups(),
                 (QStringList{"alpha", "Beta", "Beta/One", "Beta/Two", "Gamma"}));

        QVERIFY(t.repo->moveGroup(QStringLiteral("Gamma"), -1));
        QCOMPARE(t.repo->groups(), (QStringList{"alpha", "Gamma", "Beta", "Beta/One", "Beta/Two"}));
        QVERIFY(t.repo->moveGroup(QStringLiteral("Gamma"), -1));
        QVERIFY(!t.repo->moveGroup(QStringLiteral("Gamma"), -1)); // first already
        QVERIFY(t.repo->moveGroup(QStringLiteral("Beta/One"), 1)); // within its parent only
        QVERIFY(!t.repo->moveGroup(QStringLiteral("Beta/One"), 1));
        QCOMPARE(t.repo->groups(), (QStringList{"Gamma", "alpha", "Beta", "Beta/Two", "Beta/One"}));

        // A new group goes after the ordered ones; groupTree has the same order.
        t.repo->createGroup(QStringLiteral("Aardvark"));
        QCOMPARE(t.repo->groups(), (QStringList{"Gamma", "alpha", "Beta", "Beta/Two", "Beta/One", "Aardvark"}));
        QCOMPARE(t.repo->groupTree().front().path, QStringLiteral("Gamma"));

        // Renaming in place keeps the place; moving to another parent goes last there.
        QVERIFY(t.repo->renameGroup(QStringLiteral("alpha"), QStringLiteral("Alpha")));
        QCOMPARE(t.repo->groups().at(1), QStringLiteral("Alpha"));
        QVERIFY(t.repo->renameGroup(QStringLiteral("Gamma"), QStringLiteral("Beta/Gamma")));
        QCOMPARE(t.repo->groups(), (QStringList{"Alpha", "Beta", "Beta/Two", "Beta/One", "Beta/Gamma", "Aardvark"}));
    }

    void sameMillisecondWritesStillOrder()
    {
        TestDb t;
        const QDateTime frozen = t.clock;
        Repository repo(t.db, [frozen] { return frozen; }); // every reading is the same instant
        const auto a = repo.ingest(QStringLiteral("a")).id;
        const auto b = repo.ingest(QStringLiteral("b")).id;
        QCOMPARE(repo.recent(1, false).front().id, b);
        repo.touch(a);
        QCOMPARE(repo.recent(1, false).front().id, a);
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

    void groupPathsAreNormalized()
    {
        TestDb t;
        const auto id = t.repo->ingest(QStringLiteral("x")).id;
        t.repo->setGroup(id, QStringLiteral(" Work /  Servers / "));
        QCOMPARE(t.repo->get(id)->group, QStringLiteral("Work/Servers"));
        t.repo->setGroup(id, QStringLiteral("Work \u203A Prod")); // the display separator also works
        QCOMPARE(t.repo->get(id)->group, QStringLiteral("Work/Prod"));
    }

    void creatingAGroupCreatesItsParents()
    {
        TestDb t;
        QCOMPARE(t.repo->createGroup(QStringLiteral("Work/Servers/Prod")), QStringLiteral("Work/Servers/Prod"));
        QCOMPARE(t.repo->groups(), (QStringList{QStringLiteral("Work"), QStringLiteral("Work/Servers"),
                                                QStringLiteral("Work/Servers/Prod")}));
        QVERIFY(t.repo->createGroup(QStringLiteral("  / ")).isEmpty());
        QCOMPARE(t.repo->groups().size(), 3);
    }

    void groupTreeCountsDirectClips()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto c = t.repo->ingest(QStringLiteral("c")).id;
        t.repo->setGroup(a, QStringLiteral("Work"));
        t.repo->setGroup(b, QStringLiteral("Work/Servers"));
        t.repo->setGroup(c, QStringLiteral("Work/Servers"));
        t.repo->createGroup(QStringLiteral("Empty"));

        const auto tree = t.repo->groupTree();
        QCOMPARE(tree.size(), 3u);
        QCOMPARE(tree[0].path, QStringLiteral("Empty"));
        QCOMPARE(tree[0].clipCount, 0);
        QCOMPARE(tree[1].clipCount, 1); // Work: a only, not its sub-group's clips
        QCOMPARE(tree[2].clipCount, 2);
        QCOMPARE(t.repo->groupedEntries().size(), 3u);
    }

    void renamingMovesTheWholeSubtree()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto other = t.repo->ingest(QStringLiteral("other")).id;
        t.repo->setGroup(a, QStringLiteral("Work"));
        t.repo->setGroup(b, QStringLiteral("Work/Servers"));
        t.repo->setGroup(other, QStringLiteral("Workshop")); // shares a prefix, must not move

        QVERIFY(t.repo->renameGroup(QStringLiteral("Work"), QStringLiteral("Jobs/Acme")));
        QCOMPARE(t.repo->get(a)->group, QStringLiteral("Jobs/Acme"));
        QCOMPARE(t.repo->get(b)->group, QStringLiteral("Jobs/Acme/Servers"));
        QCOMPARE(t.repo->get(other)->group, QStringLiteral("Workshop"));
        QCOMPARE(t.repo->groups(), (QStringList{QStringLiteral("Jobs"), QStringLiteral("Jobs/Acme"),
                                                QStringLiteral("Jobs/Acme/Servers"), QStringLiteral("Workshop")}));
        // Searching the new name finds clips in the moved sub-group too.
        QCOMPARE(t.repo->search(QStringLiteral("acme")).size(), 2u);
    }

    void renameRefusesConflictsAndCycles()
    {
        TestDb t;
        t.repo->createGroup(QStringLiteral("Work/Servers"));
        t.repo->createGroup(QStringLiteral("Home"));
        QString error;
        QVERIFY(!t.repo->renameGroup(QStringLiteral("Work"), QStringLiteral("Home"), &error));
        QVERIFY(error.contains(QLatin1String("already exists")));
        QVERIFY(!t.repo->renameGroup(QStringLiteral("Work"), QStringLiteral("Work/Servers/Deeper"), &error));
        QVERIFY(error.contains(QLatin1String("into itself")));
        QVERIFY(t.repo->renameGroup(QStringLiteral("Work"), QStringLiteral(" Work ")));
        QCOMPARE(t.repo->groups().size(), 3);
    }

    void deletingAGroupUngroupsItsClips()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("a")).id;
        const auto b = t.repo->ingest(QStringLiteral("b")).id;
        const auto keep = t.repo->ingest(QStringLiteral("keep")).id;
        t.repo->setGroup(a, QStringLiteral("Work"));
        t.repo->setGroup(b, QStringLiteral("Work/Servers"));
        t.repo->setGroup(keep, QStringLiteral("Workshop"));
        t.repo->setFavorite(b, true);

        t.repo->deleteGroup(QStringLiteral("Work"));
        QCOMPARE(t.repo->groups(), QStringList{QStringLiteral("Workshop")});
        QVERIFY(t.repo->get(a)->group.isEmpty());
        QVERIFY(t.repo->get(b)->group.isEmpty());
        QVERIFY(t.repo->get(b)->favorite); // still saved through the favorite flag
        QCOMPARE(t.repo->get(keep)->group, QStringLiteral("Workshop"));
        QCOMPARE(t.repo->count(), 3); // clips are kept
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
        t.repo->setLimits({/*favorites=*/50, /*starsPerGroup=*/10, /*pins=*/10});
        for (int i = 0; i < 50; ++i) {
            const auto id = t.repo->ingest(QStringLiteral("saved %1").arg(i)).id;
            if (i < 5) {
                t.repo->setPinned(id, true);
            } else if (i % 2) {
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
        QCOMPARE(t.repo->count() - t.repo->countUnsaved(), 50); // pinned, starred and grouped
        QCOMPARE(t.repo->pinned(100).size(), 5u);

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
