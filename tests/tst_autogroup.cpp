#include "AutoGroup.h"
#include "CapturePipeline.h"
#include "HistoryDelegate.h"
#include "HistoryModel.h"
#include "PasswordVault.h"
#include "TestDb.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTest>

using clipitto::CapturePipeline;
using clipitto::Entry;
using clipitto::db::Repository;

namespace {

const QStringList kPlain{QStringLiteral("text/plain")};

QString docsUrl(const QString& page)
{
    return QStringLiteral("https://docs.python.org/3/library/%1.html").arg(page);
}

QString sshCommand(const QString& host)
{
    return QStringLiteral("ssh admin@%1").arg(host);
}

// Puts a copy in a group the way the user does (Ctrl+G): it is stored, then moved.
qint64 fileByHand(Repository& repo, const QString& text, const QString& group)
{
    const auto result = repo.ingest(text);
    repo.setGroup(result.id, group);
    return result.id;
}

// Two groups with a couple of examples each (turn auto grouping on first, or nothing is learned).
void teachTwoGroups(Repository& repo)
{
    fileByHand(repo, docsUrl(QStringLiteral("os")), QStringLiteral("Docs"));
    fileByHand(repo, docsUrl(QStringLiteral("sys")), QStringLiteral("Docs"));
    fileByHand(repo, sshCommand(QStringLiteral("10.0.0.5")), QStringLiteral("Servers"));
    fileByHand(repo, sshCommand(QStringLiteral("10.0.0.9")), QStringLiteral("Servers"));
}

QString groupOf(Repository& repo, qint64 id)
{
    const auto e = repo.get(id);
    return e ? e->group : QStringLiteral("<missing>");
}

qint64 lastStored(const QSignalSpy& spy)
{
    return spy.last().at(0).toLongLong();
}

} // namespace

class TestAutoGroup : public QObject {
    Q_OBJECT

private slots:
    void tokensAreShapesAndWordsAndNeverText()
    {
        using clipitto::autogroup::tokens;
        QVERIFY(tokens(QString()).empty());
        QVERIFY(tokens(QStringLiteral("   \n ")).empty());
        // A URL gives its host, domain and first folder as shapes (weight above a plain word).
        const auto url = tokens(QStringLiteral("https://www.docs.python.org/3/library/os.html"));
        QVERIFY(url.size() >= 3);
        QCOMPARE(url.front().weight, 4);
        // The same site gives the same token whatever the page.
        const auto other = tokens(QStringLiteral("https://docs.python.org/3/library/sys.html"));
        QCOMPARE(other.front().hash, url.front().hash);
        // A command's first word, a path's folder.
        QCOMPARE(tokens(QStringLiteral("git status -sb")).front().weight, 4);
        QCOMPARE(tokens(QStringLiteral("/etc/nginx/nginx.conf")).front().weight, 3);
        // Bounded: a huge copy yields a bounded number of tokens.
        QString big;
        for (int i = 0; i < 5000; ++i) {
            big += QStringLiteral("word%1 ").arg(i);
        }
        QVERIFY(int(tokens(big).size()) <= clipitto::autogroup::kMaxTokens);
    }

    void learnsFromMovesAndFilesSimilarNewCopies()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);

        p.process(docsUrl(QStringLiteral("re")), kPlain);
        p.process(sshCommand(QStringLiteral("10.0.0.7")), kPlain);
        QTRY_COMPARE(filed.size(), 2);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(stored.size(), 2);
        QCOMPARE(groupOf(*t.repo, stored.at(0).at(0).toLongLong()), QStringLiteral("Docs"));
        QCOMPARE(groupOf(*t.repo, stored.at(1).at(0).toLongLong()), QStringLiteral("Servers"));
        const auto e = t.repo->get(stored.at(0).at(0).toLongLong());
        QVERIFY(e && e->autoFiled);
        // The groups the user made are the only ones: none was created.
        QCOMPARE(t.repo->groups(), (QStringList{QStringLiteral("Docs"), QStringLiteral("Servers")}));
    }

    void copiesAlreadyInGroupsAreExamplesFromTheStart()
    {
        TestDb t;
        // Filed while the setting was off: nothing is learned then, but the copies are in their groups.
        teachTwoGroups(*t.repo);
        QVERIFY(t.repo->autoExamples().empty());
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        QSignalSpy filedSpy(&p, &CapturePipeline::autoFiled);
        p.process(docsUrl(QStringLiteral("json")), kPlain);
        QTRY_COMPARE(filedSpy.size(), 1);
        const auto examples = t.repo->autoExamples();
        QCOMPARE(examples.size(), size_t(2));
        QCOMPARE(examples.at(0).examples, 2);
    }

    void aWeakGuessIsNotFiled()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        // One shared word ("docs"), nothing else: below the threshold.
        p.process(QStringLiteral("remember to read the docs tonight"), kPlain);
        // Nothing in common at all.
        p.process(QStringLiteral("grocery list: milk, eggs, bread"), kPlain);
        QCOMPARE(stored.size(), 2);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, stored.at(0).at(0).toLongLong()), QString());
        QCOMPARE(groupOf(*t.repo, stored.at(1).at(0).toLongLong()), QString());
        QCOMPARE(t.repo->recent(10, true).size(), size_t(2)); // both are still in Recent, unsaved
    }

    void aTieBetweenGroupsIsNotFiled()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        fileByHand(*t.repo, docsUrl(QStringLiteral("os")), QStringLiteral("A"));
        fileByHand(*t.repo, docsUrl(QStringLiteral("sys")), QStringLiteral("B"));
        QSignalSpy stored(&p, &CapturePipeline::stored);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QString());
    }

    void aCorrectionCountsMoreThanAFirstExample()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        const auto wiki = [](const QString& page) { return QStringLiteral("https://wiki.example.com/%1").arg(page); };
        fileByHand(*t.repo, wiki(QStringLiteral("a")), QStringLiteral("Wiki"));
        fileByHand(*t.repo, wiki(QStringLiteral("b")), QStringLiteral("Wiki"));
        fileByHand(*t.repo, QStringLiteral("https://other.example.org/x"), QStringLiteral("Other"));
        QSignalSpy stored(&p, &CapturePipeline::stored);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);

        p.process(wiki(QStringLiteral("c")), kPlain);
        QTRY_COMPARE(filed.size(), 1);
        const qint64 guess = lastStored(stored);
        QCOMPARE(groupOf(*t.repo, guess), QStringLiteral("Wiki"));

        // The user moves it: one move is worth more than a first example, so the next one follows.
        t.repo->setGroup(guess, QStringLiteral("Other"));
        const auto moved = t.repo->get(guess);
        QVERIFY(moved && !moved->autoFiled); // now filed by the user
        p.process(wiki(QStringLiteral("d")), kPlain);
        QTRY_COMPARE(filed.size(), 2);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QStringLiteral("Other"));
    }

    void movingAHandFiledCopyDoesNotUnlearnTheOldGroup()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        const qint64 mine = fileByHand(*t.repo, docsUrl(QStringLiteral("io")), QStringLiteral("Docs"));
        t.repo->setGroup(mine, QStringLiteral("Servers")); // not an automatic filing: no correction, only a new example
        QSignalSpy stored(&p, &CapturePipeline::stored);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QStringLiteral("Docs"));
    }

    void removingAnAutoFiledCopyFromItsGroupTeachesTheOtherWay()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QTRY_COMPARE(filed.size(), 1);
        t.repo->setGroup(lastStored(stored), QString()); // taken out of the group
        const auto before = t.repo->autoExamples();
        QCOMPARE(before.size(), size_t(2));
        QCOMPARE(before.at(0).group, QStringLiteral("Docs"));
        QCOMPARE(before.at(0).examples, 1); // 2 examples, minus the correction
        // A correction weighs three examples: the group no longer matches this kind of copy.
        p.process(docsUrl(QStringLiteral("zlib")), kPlain);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(filed.size(), 1);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QString());
    }

    void neverForPasswordCopiesOrPictures()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        clipitto::PasswordVault vault;
        vault.load(clipitto::PasswordVault::create("master", {64, 1}));
        vault.setMatchKey(QByteArray(32, 'm'));
        p.setPasswords(&vault, true, QString());
        p.setDetectSecrets(true);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);

        // A marked copy, and a token: both kept as password copies, never filed, and never taught.
        const QStringList marked = {QStringLiteral("text/plain"), QStringLiteral("x-kde-passwordManagerHint")};
        p.process(QStringLiteral("ssh admin@10.0.0.77"), marked);
        p.process(QStringLiteral("ghp_0123456789abcdefghijklmnopqrstuvwxyzAB"), kPlain);
        QCOMPARE(t.repo->countSecrets(), 2);
        const qint64 secret = lastStored(stored);
        const auto before = t.repo->autoExamples();
        t.repo->setGroup(secret, QStringLiteral("Servers")); // the user files a password: it teaches nothing
        const auto after = t.repo->autoExamples();
        QCOMPARE(after.size(), before.size());
        for (size_t i = 0; i < after.size(); ++i) {
            QCOMPARE(after.at(i).examples, before.at(i).examples);
        }

        // A picture: neither filed nor taught, even when its label would match a group.
        clipitto::ImageClip image;
        image.png = QByteArray(64, 'x');
        image.thumbnail = QByteArray(8, 't');
        image.width = 800;
        image.height = 600;
        image.pixelHash = QStringLiteral("h1");
        const auto first = t.repo->ingestImage(image);
        t.repo->setGroup(first.id, QStringLiteral("Servers"));
        image.pixelHash = QStringLiteral("h2");
        p.processImage(image, {QStringLiteral("image/png")});
        QCOMPARE(stored.size(), 3); // the picture was stored
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QString());
        QCOMPARE(filed.size(), 0);
        for (const auto& e : t.repo->autoExamples()) {
            QVERIFY(e.group != QLatin1String("Servers") || e.examples == 2);
        }
    }

    void forgettingClearsWhatWasLearnedButKeepsFiledCopies()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QSignalSpy stored(&p, &CapturePipeline::stored);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QTRY_COMPARE(filed.size(), 1);
        const qint64 kept = lastStored(stored);

        t.repo->forgetAutoLearned();
        QVERIFY(t.repo->autoExamples().empty());
        QVERIFY(t.repo->autoModel()->empty());
        QCOMPARE(groupOf(*t.repo, kept), QStringLiteral("Docs")); // already filed copies stay
        QVERIFY(t.repo->get(kept)->autoFiled);

        // Nothing matches any more, and the copies in groups are not read as examples again.
        p.process(docsUrl(QStringLiteral("abc")), kPlain);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(filed.size(), 1);
        QVERIFY(t.repo->autoExamples().empty());
        // It learns again from the next move.
        fileByHand(*t.repo, docsUrl(QStringLiteral("x1")), QStringLiteral("Docs"));
        fileByHand(*t.repo, docsUrl(QStringLiteral("x2")), QStringLiteral("Docs"));
        p.process(docsUrl(QStringLiteral("x3")), kPlain);
        QTRY_COMPARE(filed.size(), 2);
    }

    void autoFiledCopiesAreNotExamplesThemselves()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QTRY_COMPARE(filed.size(), 1);
        const auto examples = t.repo->autoExamples();
        QCOMPARE(examples.at(0).examples, 2); // a guess does not reinforce itself
    }

    void settingOffDoesNothing()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096); // off by default
        QVERIFY(!p.autoGroupingPending());
        teachTwoGroups(*t.repo);
        QVERIFY(t.repo->autoExamples().empty()); // nothing learned while off
        QSignalSpy stored(&p, &CapturePipeline::stored);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QCOMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QString());

        // On: files a copy. Off again: the filed copy stays, and the next copy is left alone.
        p.setAutoGrouping(true);
        QSignalSpy filed(&p, &CapturePipeline::autoFiled);
        p.process(docsUrl(QStringLiteral("json")), kPlain);
        QTRY_COMPARE(filed.size(), 1);
        const qint64 filedId = lastStored(stored);
        p.setAutoGrouping(false);
        p.process(docsUrl(QStringLiteral("csv")), kPlain);
        QCOMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QString());
        QCOMPARE(groupOf(*t.repo, filedId), QStringLiteral("Docs"));
        QVERIFY(t.repo->get(filedId)->autoFiled);
    }

    void aDeletedGroupIsNeverRecreated()
    {
        TestDb t;
        CapturePipeline p(*t.repo, 100, 4096);
        p.setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        t.repo->autoModel(); // cached with Docs in it
        t.repo->deleteGroup(QStringLiteral("Docs"));
        QSignalSpy stored(&p, &CapturePipeline::stored);
        p.process(docsUrl(QStringLiteral("re")), kPlain);
        QTRY_COMPARE(p.autoGroupingPending(), 0);
        QCOMPARE(groupOf(*t.repo, lastStored(stored)), QString());
        QVERIFY(!t.repo->groups().contains(QStringLiteral("Docs")));
        // A stale answer for a group that no longer exists is refused too.
        QVERIFY(!t.repo->setAutoGroup(lastStored(stored), QStringLiteral("Docs")));
        QVERIFY(!t.repo->groups().contains(QStringLiteral("Docs")));
    }

    void aCopyTheUserSavedMeanwhileIsLeftAlone()
    {
        TestDb t;
        teachTwoGroups(*t.repo);
        const auto starred = t.repo->ingest(QStringLiteral("some text"));
        t.repo->setFavorite(starred.id, true);
        QVERIFY(!t.repo->setAutoGroup(starred.id, QStringLiteral("Docs")));
        const auto mine = t.repo->ingest(QStringLiteral("other text"));
        t.repo->setGroup(mine.id, QStringLiteral("Servers"));
        QVERIFY(!t.repo->setAutoGroup(mine.id, QStringLiteral("Docs")));
        QCOMPARE(groupOf(*t.repo, mine.id), QStringLiteral("Servers"));
    }

    void aLateGuessNeverOverwritesTheUsersOwnFiling()
    {
        TestDb t;
        teachTwoGroups(*t.repo);
        const auto copy = t.repo->ingest(QStringLiteral("some text"));
        t.repo->setGroup(copy.id, QStringLiteral("Servers")); // the user got there first
        QVERIFY(!t.repo->setAutoGroup(copy.id, QStringLiteral("Docs")));
        QCOMPARE(groupOf(*t.repo, copy.id), QStringLiteral("Servers"));
    }

    void clearingEverythingForgetsWhatWasLearned()
    {
        TestDb t;
        t.repo->setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QVERIFY(!t.repo->autoExamples().empty());
        t.repo->clearHistory(true);
        QVERIFY(t.repo->autoExamples().empty());
    }

    void keysAndPasswordsFiledByHandTeachNothing()
    {
        TestDb t;
        t.repo->setAutoGrouping(true);
        fileByHand(*t.repo, QStringLiteral("ghp_0123456789abcdefghijklmnopqrstuvwxyzAB"), QStringLiteral("Tokens"));
        QVERIFY(t.repo->autoExamples().empty());
    }

    void renamingAndDeletingGroupsKeepsTheExamplesInStep()
    {
        TestDb t;
        t.repo->setAutoGrouping(true);
        teachTwoGroups(*t.repo);
        QVERIFY(t.repo->renameGroup(QStringLiteral("Docs"), QStringLiteral("Reading")));
        const auto examples = t.repo->autoExamples();
        QStringList names;
        for (const auto& e : examples) {
            names << e.group;
        }
        QCOMPARE(names, (QStringList{QStringLiteral("Reading"), QStringLiteral("Servers")}));
        QCOMPARE(t.repo->autoModel()->groupCount(), 2);
        t.repo->deleteGroup(QStringLiteral("Servers"));
        QCOMPARE(t.repo->autoExamples().size(), size_t(1));
        QCOMPARE(t.repo->autoModel()->groupCount(), 1);
    }

    void theAutoTagIsShownOnTheGroupChip()
    {
        using clipitto::ui::HistoryModel;
        Entry e;
        e.id = 1;
        e.content = QStringLiteral("https://docs.python.org/3/library/re.html");
        e.group = QStringLiteral("Docs");
        Entry mine = e;
        mine.id = 2;
        e.autoFiled = true;
        HistoryModel m;
        m.setContent({{QString(), {e, mine}}});
        QVERIFY(m.index(0, 0).data(HistoryModel::AutoFiledRole).toBool());
        QVERIFY(!m.index(1, 0).data(HistoryModel::AutoFiledRole).toBool());

        // The chip is drawn differently with the tag: the row's pixels differ between the two copies.
        clipitto::ui::HistoryDelegate delegate;
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 420, 28);
        option.font = QFont();
        QImage a(option.rect.size(), QImage::Format_ARGB32);
        QImage b(option.rect.size(), QImage::Format_ARGB32);
        a.fill(Qt::white);
        b.fill(Qt::white);
        {
            QPainter pa(&a);
            delegate.paint(&pa, option, m.index(0, 0));
            QPainter pb(&b);
            delegate.paint(&pb, option, m.index(1, 0));
        }
        QVERIFY(a != b);
    }

    void whatWasLearnedSurvivesARestart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("history.db"));
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            repo.setAutoGrouping(true);
            teachTwoGroups(repo);
            QCOMPARE(repo.autoExamples().size(), size_t(2));
        } // closed before the directory goes (Windows)
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            QCOMPARE(repo.autoExamples().size(), size_t(2));
            CapturePipeline p(repo, 100, 4096);
            p.setAutoGrouping(true);
            QSignalSpy stored(&p, &CapturePipeline::stored);
            QSignalSpy filed(&p, &CapturePipeline::autoFiled);
            p.process(sshCommand(QStringLiteral("10.0.0.7")), kPlain);
            QTRY_COMPARE(filed.size(), 1);
            QCOMPARE(groupOf(repo, lastStored(stored)), QStringLiteral("Servers"));
            QTRY_COMPARE(p.autoGroupingPending(), 0);
        }
    }

    void learnedDataHoldsNoTextAndOutlivesTheCopy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("history.db"));
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            repo.setAutoGrouping(true);
            const auto copy = repo.ingest(QStringLiteral("zebrafish quartz mellifluous"));
            repo.setGroup(copy.id, QStringLiteral("Words"));
            repo.remove(copy.id); // the copy is gone; what was learned has no text in it
            clipitto::db::Stmt all(db, "SELECT count(*) FROM auto_tokens");
            QVERIFY(all.step() && all.colInt(0) > 0);
            // Tokens are 64-bit hashes: no column holds a word.
            clipitto::db::Stmt text(db, "SELECT count(*) FROM auto_tokens WHERE typeof(token) <> 'integer'");
            QVERIFY(text.step());
            QCOMPARE(text.colInt(0), 0);
        }
    }

    void costPerCopyStaysSmall()
    {
        // 30 groups of 60 examples each, then a stream of new copies. The budget is loose (CI machines are
        // slow and shared): it only catches an accidental quadratic or a model rebuilt for every copy.
        TestDb t;
        CapturePipeline p(*t.repo, 100000, 1 << 20);
        p.setAutoGrouping(true);
        for (int g = 0; g < 30; ++g) {
            for (int i = 0; i < 60; ++i) {
                fileByHand(*t.repo,
                           QStringLiteral("https://site%1.example.com/section%2/page%3 notes about topic%1 item%3")
                               .arg(g)
                               .arg(i % 5)
                               .arg(i),
                           QStringLiteral("Group %1").arg(g));
            }
        }
        const auto model = t.repo->autoModel();
        QCOMPARE(model->groupCount(), 30);

        // The matching itself, as the worker does it.
        QElapsedTimer timer;
        timer.start();
        int hits = 0;
        for (int i = 0; i < 1000; ++i) {
            hits += !model->classify(QStringLiteral("https://site%1.example.com/section1/new%2 topic%1").arg(i % 30).arg(i)).isEmpty();
        }
        const qint64 matching = timer.elapsed();
        QVERIFY2(matching < 3000, qPrintable(QStringLiteral("1000 matches took %1 ms").arg(matching)));
        QVERIFY(hits > 900);

        // What capture pays on the GUI thread: storing the copy and starting the match.
        QSignalSpy stored(&p, &CapturePipeline::stored);
        timer.restart();
        for (int i = 0; i < 300; ++i) {
            p.process(QStringLiteral("https://site%1.example.com/section2/fresh%2 topic%1 item%2").arg(i % 30).arg(i), kPlain);
        }
        const qint64 capture = timer.elapsed();
        QVERIFY2(capture < 5000, qPrintable(QStringLiteral("300 copies took %1 ms").arg(capture)));
        QTRY_COMPARE_WITH_TIMEOUT(p.autoGroupingPending(), 0, 20000);
        QCOMPARE(stored.size(), 300);
    }
};

QTEST_MAIN(TestAutoGroup)
#include "tst_autogroup.moc"
