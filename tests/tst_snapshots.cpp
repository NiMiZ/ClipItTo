// Snapshots (roadmap 29, docs/snapshots-design.md): the repository side (create, restore, limits, survive prune/Clear,
// wiped by the wipe path, nothing readable in the raw file, also with an encrypted history), the Snapshots window and the
// popup's menu actions. Texts are synthetic. The panic action itself is in tst_panic.
#include "Config.h"
#include "PopupWindow.h"
#include "SnapshotsWindow.h"
#include "TestDb.h"
#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::Config;
using clipitto::db::Repository;
using clipitto::ui::PopupWindow;
using clipitto::ui::SnapshotsWindow;
using Status = Repository::SnapshotStatus;

namespace {

std::vector<qint64> ids(Repository& repo, const QStringList& texts)
{
    std::vector<qint64> out;
    for (const QString& text : texts) {
        out.push_back(repo.ingest(text).id);
    }
    return out;
}

QStringList newestFirst(Repository& repo)
{
    QStringList out;
    for (const auto& e : repo.recentUnpinned(1000)) {
        out << e.content;
    }
    return out;
}

bool fileHolds(const QString& path, const QByteArray& needle)
{
    for (const QString& name : {path, path + QStringLiteral("-wal")}) {
        QFile f(name);
        if (f.open(QIODevice::ReadOnly) && f.readAll().contains(needle)) {
            return true;
        }
    }
    return false;
}

} // namespace

class TestSnapshots : public QObject {
    Q_OBJECT

private slots:
    // ------------------------------------------------------------ repository

    void createsFromCopiesAndRestoresThem()
    {
        TestDb t;
        const auto made = ids(*t.repo, {QStringLiteral("snap-a"), QStringLiteral("snap-b"), QStringLiteral("snap-c")});
        t.repo->setNote(made[1], QStringLiteral("the b note"));
        const auto r = t.repo->createSnapshot(QStringLiteral("  Project   X "), made);
        QCOMPARE(r.status, Status::Done);
        QCOMPARE(r.saved, 3);
        QCOMPARE(r.skippedTotal(), 0);
        const auto list = t.repo->snapshots();
        QCOMPARE(list.size(), size_t(1));
        QCOMPARE(list[0].name, QStringLiteral("Project X")); // spaces tidied
        QCOMPARE(list[0].items, 3);

        t.repo->clearHistory(true); // Clear everything leaves the snapshot
        QCOMPARE(t.repo->count(), qint64(0));
        QCOMPARE(t.repo->countSnapshots(), qint64(1));

        const auto back = t.repo->restoreSnapshot(list[0].id, {});
        QVERIFY(back.ok);
        QCOMPARE(back.added, 3);
        QCOMPARE(back.alreadyThere, 0);
        QCOMPARE(newestFirst(*t.repo), (QStringList{QStringLiteral("snap-c"), QStringLiteral("snap-b"), QStringLiteral("snap-a")}));
        const auto b = t.repo->findByHash(t.repo->textHash(QStringLiteral("snap-b")));
        QVERIFY(b);
        QCOMPARE(t.repo->get(*b)->note, QStringLiteral("the b note"));
    }

    void restoreDedupesAndOrdersAsAsked()
    {
        TestDb t;
        const auto made = ids(*t.repo, {QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
        // Saved order: the list as given (one, two, three).
        const qint64 id = t.repo->createSnapshot(QStringLiteral("Order"), made).id;
        t.repo->clearHistory(true);
        t.repo->ingest(QStringLiteral("two")); // already there
        t.repo->ingest(QStringLiteral("unrelated"));

        Repository::SnapshotRestoreOptions saved;
        saved.savedOrder = true;
        const auto r = t.repo->restoreSnapshot(id, saved);
        QCOMPARE(r.added, 2);
        QCOMPARE(r.alreadyThere, 1);
        QCOMPARE(t.repo->count(), qint64(4)); // "two" not doubled
        QCOMPARE(newestFirst(*t.repo).mid(0, 3), (QStringList{QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}));

        // As copied: the copy made last ends on top ("three" was copied last).
        const auto again = t.repo->restoreSnapshot(id, {});
        QCOMPARE(again.added, 0);
        QCOMPARE(again.alreadyThere, 3);
        QCOMPARE(newestFirst(*t.repo).mid(0, 3), (QStringList{QStringLiteral("three"), QStringLiteral("two"), QStringLiteral("one")}));
        QCOMPARE(t.repo->count(), qint64(4));
    }

    void restoreIntoAGroupNamedAfterTheSnapshot()
    {
        TestDb t;
        const auto made = ids(*t.repo, {QStringLiteral("g-one"), QStringLiteral("g-two"), QStringLiteral("g-three")});
        const qint64 id = t.repo->createSnapshot(QStringLiteral("Q3/plan"), made).id; // "/" does not make sub-groups
        t.repo->clearHistory(true);
        const qint64 existing = t.repo->ingest(QStringLiteral("g-two")).id;
        t.repo->setGroup(existing, QStringLiteral("Elsewhere")); // stays where it is
        Repository::SnapshotRestoreOptions o;
        o.intoGroup = true;
        const auto r = t.repo->restoreSnapshot(id, o);
        QCOMPARE(r.group, QStringLiteral("Q3-plan"));
        QCOMPARE(r.filed, 2);
        QVERIFY(t.repo->groups().contains(QStringLiteral("Q3-plan")));
        QCOMPARE(t.repo->get(existing)->group, QStringLiteral("Elsewhere"));
        const auto one = t.repo->findByHash(t.repo->textHash(QStringLiteral("g-one")));
        QCOMPARE(t.repo->get(*one)->group, QStringLiteral("Q3-plan"));
        // Grouped copies are saved: pruning never takes them.
        t.repo->prune(0);
        QVERIFY(t.repo->get(*one));
    }

    void fromAWholeGroupIncludingSubGroups()
    {
        TestDb t;
        const auto made = ids(*t.repo, {QStringLiteral("in-work"), QStringLiteral("in-sub"), QStringLiteral("elsewhere")});
        t.repo->setGroup(made[0], QStringLiteral("Work"));
        t.repo->setGroup(made[1], QStringLiteral("Work/Servers"));
        t.repo->setGroup(made[2], QStringLiteral("Workshop")); // a different group with the same prefix
        const auto r = t.repo->createSnapshotFromGroup(QStringLiteral("W"), QStringLiteral("Work"));
        QCOMPARE(r.status, Status::Done);
        QCOMPARE(r.saved, 2);
        QCOMPARE(t.repo->createSnapshotFromGroup(QStringLiteral("Empty"), QStringLiteral("Nope")).status, Status::Nothing);
        QCOMPARE(t.repo->countSnapshots(), qint64(1)); // no empty snapshot is left behind
    }

    void survivesPruneAndClearRecent()
    {
        TestDb t;
        const auto made = ids(*t.repo, {QStringLiteral("p-1"), QStringLiteral("p-2")});
        const qint64 id = t.repo->createSnapshot(QStringLiteral("Keep"), made).id;
        t.repo->prune(0);
        t.repo->pruneImages(0);
        t.repo->clearHistory(false);
        t.repo->clearRecent();
        t.repo->clearHistory(true);
        QCOMPARE(t.repo->count(), qint64(0));
        QCOMPARE(t.repo->snapshots().at(0).items, 2);
        QCOMPARE(t.repo->restoreSnapshot(id, {}).added, 2);
    }

    void wipeRemovesSnapshotsToo()
    {
        TestDb t;
        t.repo->createSnapshot(QStringLiteral("A"), ids(*t.repo, {QStringLiteral("w-1")}));
        t.repo->createSnapshot(QStringLiteral("B"), ids(*t.repo, {QStringLiteral("w-2")}));
        QCOMPARE(t.repo->countSnapshots(), qint64(2));
        t.repo->clearHistory(true, /*alsoSnapshots=*/true);
        QCOMPARE(t.repo->countSnapshots(), qint64(0));
        QVERIFY(t.repo->snapshots().empty());
    }

    void passwordsPicturesAndOnceCopiesAreSkippedAndCounted()
    {
        TestDb t;
        const qint64 plain = t.repo->ingest(QStringLiteral("s-plain")).id;
        const qint64 secret = t.repo->ingestSecret(QByteArray("sealed-blob"), QStringLiteral("secret:aa"), QStringLiteral("KeePassXC"), QString()).id;
        const qint64 once = t.repo->ingest(QStringLiteral("s-once")).id;
        t.repo->setPasteOnce(once, true);
        clipitto::ImageClip picture;
        picture.png = QByteArray("not really a png");
        picture.pixelHash = QStringLiteral("abc");
        picture.width = 4;
        picture.height = 4;
        picture.thumbnail = picture.png;
        const qint64 image = t.repo->ingestImage(picture).id;
        const auto r = t.repo->createSnapshot(QStringLiteral("Mixed"), {plain, secret, once, image});
        QCOMPARE(r.status, Status::Done);
        QCOMPARE(r.saved, 1);
        QCOMPARE(r.skippedPasswords, 1);
        QCOMPARE(r.skippedPictures, 1);
        QCOMPARE(r.skippedOnce, 1);
        const QString text = clipitto::ui::snapshotSummary(r, QStringLiteral("Mixed"));
        QVERIFY(text.contains(QStringLiteral("1 password")));
        QVERIFY(text.contains(QStringLiteral("1 picture")));
        // Only passwords: nothing is made, and the message says why.
        const auto none = t.repo->createSnapshot(QStringLiteral("OnlyPw"), {secret});
        QCOMPARE(none.status, Status::Nothing);
        QCOMPARE(none.skippedPasswords, 1);
        QCOMPARE(t.repo->countSnapshots(), qint64(1));
        QVERIFY(clipitto::ui::snapshotSummary(none, QStringLiteral("OnlyPw")).contains(QStringLiteral("1 password")));
        // The snapshot holds the one plain copy and nothing of the password's label.
        t.repo->clearHistory(true);
        QCOMPARE(t.repo->restoreSnapshot(r.id, {}).added, 1);
        QCOMPARE(t.repo->count(), qint64(1));
        QCOMPARE(t.repo->countSecrets(), qint64(0));
    }

    void limitsAreEnforcedWithMessages()
    {
        TestDb t;
        // Name rules.
        const auto one = ids(*t.repo, {QStringLiteral("l-1")});
        QCOMPARE(t.repo->createSnapshot(QStringLiteral("   "), one).status, Status::NameEmpty);
        QCOMPARE(t.repo->createSnapshot(QString(81, QLatin1Char('n')), one).status, Status::NameTooLong);
        QCOMPARE(t.repo->createSnapshot(QStringLiteral("Same"), one).status, Status::Done);
        QCOMPARE(t.repo->createSnapshot(QStringLiteral("SAME"), one).status, Status::NameTaken); // not case-sensitive
        // 50 snapshots.
        for (int i = 1; i < Repository::kMaxSnapshots; ++i) {
            QCOMPARE(t.repo->createSnapshot(QStringLiteral("S%1").arg(i), one).status, Status::Done);
        }
        QCOMPARE(t.repo->countSnapshots(), qint64(Repository::kMaxSnapshots));
        const auto full = t.repo->createSnapshot(QStringLiteral("One too many"), one);
        QCOMPARE(full.status, Status::TooManySnapshots);
        QVERIFY(clipitto::ui::snapshotSummary(full, QStringLiteral("x")).contains(QStringLiteral("50")));
        QCOMPARE(t.repo->countSnapshots(), qint64(Repository::kMaxSnapshots));
    }

    void copyAndSizeLimits()
    {
        TestDb t;
        std::vector<qint64> many;
        for (int i = 0; i < Repository::kMaxSnapshotItems + 7; ++i) {
            many.push_back(t.repo->ingest(QStringLiteral("item %1").arg(i)).id);
        }
        const qint64 big = t.repo->ingest(QString(Repository::kMaxSnapshotItemBytes + 1, QLatin1Char('x'))).id;
        const qint64 exactly = t.repo->ingest(QString(Repository::kMaxSnapshotItemBytes, QLatin1Char('y'))).id;
        many.insert(many.begin(), {big, exactly});
        const auto r = t.repo->createSnapshot(QStringLiteral("Big"), many);
        QCOMPARE(r.status, Status::Done);
        QCOMPARE(r.saved, Repository::kMaxSnapshotItems);
        QCOMPARE(r.skippedTooLarge, 1);
        QCOMPARE(r.skippedOverLimit, 8); // 507 + the exact one, minus 500
        QVERIFY(clipitto::ui::snapshotSummary(r, QStringLiteral("Big")).contains(QStringLiteral("over 1 MB")));
        // Adding to a full snapshot says so; the same text twice is kept once.
        const auto more = t.repo->addToSnapshot(r.id, {t.repo->ingest(QStringLiteral("late")).id});
        QCOMPARE(more.status, Status::Nothing);
        QCOMPARE(more.skippedOverLimit, 1);
        const auto small = t.repo->createSnapshot(QStringLiteral("Twice"), {many[2], many[2]});
        QCOMPARE(small.saved, 1);
        QCOMPARE(small.alreadyIn, 1);
    }

    void addToRenameDelete()
    {
        TestDb t;
        const qint64 id = t.repo->createSnapshot(QStringLiteral("First"), ids(*t.repo, {QStringLiteral("r-1")})).id;
        t.repo->createSnapshot(QStringLiteral("Second"), ids(*t.repo, {QStringLiteral("r-2")}));
        QCOMPARE(t.repo->addToSnapshot(id, ids(*t.repo, {QStringLiteral("r-3")})).saved, 1);
        QCOMPARE(t.repo->addToSnapshot(9999, {}).status, Status::Missing);
        QCOMPARE(t.repo->snapshots().size(), size_t(2));
        QCOMPARE(t.repo->renameSnapshot(id, QStringLiteral("Second")), Status::NameTaken);
        QCOMPARE(t.repo->renameSnapshot(id, QString()), Status::NameEmpty);
        QCOMPARE(t.repo->renameSnapshot(id, QStringLiteral("Renamed")), Status::Done);
        QCOMPARE(t.repo->renameSnapshot(id, QStringLiteral("renamed")), Status::Done); // itself, other case
        QCOMPARE(t.repo->renameSnapshot(9999, QStringLiteral("x")), Status::Missing);
        bool found = false;
        for (const auto& s : t.repo->snapshots()) {
            if (s.id == id) {
                found = true;
                QCOMPARE(s.name, QStringLiteral("renamed"));
                QCOMPARE(s.items, 2);
            }
        }
        QVERIFY(found);
        QVERIFY(t.repo->deleteSnapshot(id));
        QVERIFY(!t.repo->deleteSnapshot(id));
        QCOMPARE(t.repo->countSnapshots(), qint64(1));
        clipitto::db::Stmt left(t.db, "SELECT count(*) FROM snapshot_items");
        QVERIFY(left.step());
        QCOMPARE(left.colInt(0), 1); // the deleted snapshot's copies went with it
    }

    void snapshotTextIsNotFoundBySearchAndNotInTheHistory()
    {
        TestDb t;
        t.repo->createSnapshot(QStringLiteral("Hidden"), ids(*t.repo, {QStringLiteral("zebra-snapshot-text")}));
        t.repo->clearHistory(true);
        QVERIFY(t.repo->search(QStringLiteral("zebra-snapshot")).empty());
        QVERIFY(t.repo->search(QStringLiteral("Hidden")).empty());
        QCOMPARE(t.repo->count(), qint64(0));
    }

    // ------------------------------------------------------------ the file

    void rawFilePlainWipeAndDelete()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("history.db"));
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            const auto made = ids(repo, {QStringLiteral("zqsnap-keep-0001"), QStringLiteral("zqsnap-gone-0002")});
            const qint64 keep = repo.createSnapshot(QStringLiteral("zqsnap-name-keep"), {made[0]}).id;
            const qint64 gone = repo.createSnapshot(QStringLiteral("zqsnap-name-gone"), {made[1]}).id;
            repo.clearHistory(true); // the copies themselves are gone; the snapshots hold their own text
            QVERIFY(fileHolds(path, "zqsnap-keep-0001")); // plain history: readable, as the copies are
            QVERIFY(fileHolds(path, "zqsnap-gone-0002"));
            QVERIFY(repo.deleteSnapshot(gone));
            QVERIFY(!fileHolds(path, "zqsnap-gone-0002")); // deleting scrubs the file and the log
            QVERIFY(!fileHolds(path, "zqsnap-name-gone"));
            repo.clearHistory(true); // Clear everything: the snapshot stays
            QVERIFY(fileHolds(path, "zqsnap-keep-0001"));
            repo.clearHistory(true, true);
            QVERIFY(!fileHolds(path, "zqsnap-keep-0001")); // the wipe: nothing left behind
            QVERIFY(!fileHolds(path, "zqsnap-name-keep"));
            QCOMPARE(repo.countSnapshots(), qint64(0));
            Q_UNUSED(keep);
        }
    }

    void encryptedHistoryKeepsSnapshotsEncrypted()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        const QByteArray key = QByteArray::fromHex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("enc.db"));
        qint64 id = 0;
        {
            clipitto::db::Db db(path, key);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            id = repo.createSnapshot(QStringLiteral("zqenc-name-0001"), ids(repo, {QStringLiteral("zqenc-text-0002")})).id;
            repo.setNote(repo.findByHash(repo.textHash(QStringLiteral("zqenc-text-0002"))).value(), QStringLiteral("zqenc-note-0003"));
            repo.addToSnapshot(id, {repo.findByHash(repo.textHash(QStringLiteral("zqenc-text-0002"))).value()});
            repo.clearHistory(true);
        }
        QVERIFY(!fileHolds(path, "zqenc-name-0001"));
        QVERIFY(!fileHolds(path, "zqenc-text-0002")); // the snapshot is in the file, and not readable in it
        QVERIFY(!fileHolds(path, "zqenc-note-0003"));
        {
            clipitto::db::Db db(path, key);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            Repository repo(db);
            QCOMPARE(repo.restoreSnapshot(id, {}).added, 1); // and comes back with the key
        }
    }

    void switchingEncryptionOnCoversSnapshots()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("live.db"));
        clipitto::db::Db db(path);
        clipitto::db::configureConnection(db);
        clipitto::db::migrate(db);
        Repository repo(db);
        repo.createSnapshot(QStringLiteral("zqrk-name"), ids(repo, {QStringLiteral("zqrk-text-0001")}));
        repo.clearHistory(true);
        QVERIFY(fileHolds(path, "zqrk-text-0001") || QFile::exists(path + QStringLiteral("-wal")));
        db.rekey(QByteArray::fromHex("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"));
        QVERIFY(!fileHolds(path, "zqrk-text-0001"));
        QVERIFY(!fileHolds(path, "zqrk-name"));
        QCOMPARE(repo.snapshots().at(0).items, 1);
    }

    // ------------------------------------------------------------ the window

    void windowListsRestoresRenamesAndDeletes()
    {
        TestDb t;
        t.repo->createSnapshot(QStringLiteral("Alpha"), ids(*t.repo, {QStringLiteral("w-a1"), QStringLiteral("w-a2")}));
        t.repo->clearHistory(true);
        std::optional<QString> newName = QStringLiteral("Beta");
        bool sure = false;
        QStringList questions;
        SnapshotsWindow window(
            *t.repo, [&newName](QWidget*, const QString&, const QString&, const QString&) { return newName; },
            [&](QWidget*, const QString& question) {
                questions << question;
                return sure;
            });
        auto* list = window.findChild<QListWidget*>(QStringLiteral("snapshotList"));
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Alpha")));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("2 copies"))); // names and counts, never the text
        QVERIFY(!list->item(0)->text().contains(QStringLiteral("w-a1")));

        QSignalSpy changed(&window, &SnapshotsWindow::historyChanged);
        auto* restore = window.findChild<QPushButton*>(QStringLiteral("snapshotRestore"));
        QVERIFY(restore->isEnabled());
        window.findChild<QCheckBox*>(QStringLiteral("snapshotIntoGroup"))->setChecked(true);
        window.findChild<QComboBox*>(QStringLiteral("snapshotOrder"))->setCurrentIndex(1);
        restore->click();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(t.repo->count(), qint64(2));
        QVERIFY(window.status().contains(QStringLiteral("2 copies")));
        QVERIFY(t.repo->groups().contains(QStringLiteral("Alpha")));
        restore->click(); // again: nothing doubled
        QCOMPARE(t.repo->count(), qint64(2));
        QVERIFY(window.status().contains(QStringLiteral("already")));

        window.findChild<QPushButton*>(QStringLiteral("snapshotRename"))->click();
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Beta")));
        newName = std::nullopt; // cancelled
        window.findChild<QPushButton*>(QStringLiteral("snapshotRename"))->click();
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Beta")));

        auto* del = window.findChild<QPushButton*>(QStringLiteral("snapshotDelete"));
        del->click(); // asks; "No"
        QCOMPARE(questions.size(), 1);
        QVERIFY(questions[0].contains(QStringLiteral("Beta")));
        QCOMPARE(t.repo->countSnapshots(), qint64(1));
        sure = true;
        del->click();
        QCOMPARE(t.repo->countSnapshots(), qint64(0));
        QCOMPARE(list->count(), 0);
        QVERIFY(!restore->isEnabled());
        QVERIFY(!del->isEnabled());
    }

    // ------------------------------------------------------------ the popup

    void popupMenusSaveAndSayWhatWasSkipped()
    {
        TestDb t;
        const qint64 plain = t.repo->ingest(QStringLiteral("menu-plain")).id;
        const qint64 other = t.repo->ingest(QStringLiteral("menu-other")).id;
        t.repo->setGroup(plain, QStringLiteral("Work"));
        t.repo->setGroup(other, QStringLiteral("Work/Sub"));
        const qint64 secret = t.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:bb"), QStringLiteral("App"), QStringLiteral("Work")).id;
        Config cfg;
        cfg.hideOnFocusLoss = false;
        PopupWindow popup(*t.repo, cfg);
        QStringList notes;
        std::optional<QString> name = QStringLiteral("Proj");
        popup.setTextPrompt([&name](QWidget*, const QString&, const QString&, const QString&) { return name; });
        popup.setNotify([&notes](QWidget*, const QString& message) { notes << message; });

        // The group menu: saves the group and its sub-groups, and says a password copy was skipped.
        std::unique_ptr<QMenu> groupMenu(popup.createGroupMenu(QStringLiteral("Work"), nullptr));
        QAction* save = nullptr;
        for (QAction* a : groupMenu->actions()) {
            if (a->text().contains(QStringLiteral("Save group as snapshot"))) {
                save = a;
            }
        }
        QVERIFY(save);
        save->trigger();
        QCOMPARE(t.repo->snapshots().size(), size_t(1));
        QCOMPARE(t.repo->snapshots()[0].items, 2);
        QCOMPARE(notes.size(), 1);
        QVERIFY(notes[0].contains(QStringLiteral("1 password")));
        QVERIFY(!notes[0].contains(QStringLiteral("menu-plain")));

        // A copy's menu: a new snapshot or add to an existing one; a password copy is not offered the entry at all.
        const auto entryMenuHas = [&](const clipitto::Entry& entry, const QString& text) {
            std::unique_ptr<QMenu> menu(popup.createContextMenu(entry, &popup));
            for (QAction* a : menu->actions()) {
                if (a->menu() && a->text().contains(text)) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(entryMenuHas(*t.repo->get(plain), QStringLiteral("snapshot")));
        QVERIFY(!entryMenuHas(*t.repo->get(secret), QStringLiteral("snapshot")));

        name = QStringLiteral("Single");
        popup.saveCopyAsSnapshot(plain);
        QCOMPARE(t.repo->snapshots().size(), size_t(2));
        name = QStringLiteral("single"); // taken: says so, nothing changes
        popup.saveCopyAsSnapshot(plain);
        QCOMPARE(t.repo->snapshots().size(), size_t(2));
        QVERIFY(notes.last().contains(QStringLiteral("exists")));
        name = std::nullopt; // cancelled
        popup.saveCopyAsSnapshot(plain);
        QCOMPARE(t.repo->snapshots().size(), size_t(2));
        const qint64 proj = t.repo->snapshots()[1].id;
        popup.addCopyToSnapshot(other, proj);
        popup.addCopyToSnapshot(secret, proj); // refused by the repository too
        QVERIFY(notes.last().contains(QStringLiteral("password")));
    }

    void clearEverythingSaysSnapshotsStay()
    {
        TestDb t;
        t.repo->createSnapshot(QStringLiteral("Stay"), ids(*t.repo, {QStringLiteral("stay-1")}));
        Config cfg;
        cfg.hideOnFocusLoss = false;
        PopupWindow popup(*t.repo, cfg);
        QString question;
        popup.setConfirmPrompt([&question](QWidget*, const QString& q) {
            question = q;
            return true;
        });
        popup.clearHistory(true);
        QVERIFY(question.contains(QStringLiteral("snapshots stay")));
        QCOMPARE(t.repo->count(), qint64(0));
        QCOMPARE(t.repo->countSnapshots(), qint64(1));
    }
};

QTEST_MAIN(TestSnapshots)
#include "tst_snapshots.moc"
