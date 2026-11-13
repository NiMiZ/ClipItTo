// Sync through a shared folder (src/core/Sync.*, docs/sync-design.md): the file, its encryption, the atomic write,
// the merge rules, and that merging does not stall the GUI thread.
#include "StallWatchdog.h"
#include "Sync.h"
#include "TestDb.h"

#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent>

#include <map>

using namespace clipitto;
namespace s = clipitto::sync;

namespace {

const VaultKdfCost kCheap{64, 1}; // Argon2id for tests: tiny

s::Key makeKey(const QByteArray& pass = "correct horse", const QByteArray& salt = {})
{
    return s::deriveKey(pass, salt, kCheap);
}

s::Options options()
{
    s::Options o;
    o.deviceName = QStringLiteral("test");
    return o;
}

s::Content exportOf(TestDb& t, const s::Options& o = options())
{
    s::Collector c(*t.repo, o);
    while (!c.step(s::kBatch)) {
    }
    return c.take();
}

s::MergeStats mergeInto(TestDb& t, std::vector<s::Content> remotes, const s::Options& o = options())
{
    s::Merger m(*t.repo, std::move(remotes), o);
    while (!m.step(7)) { // an odd batch size, to cross batch borders
    }
    return m.stats();
}

// The state of a history that sync is meant to make equal: hash -> filing.
std::map<QString, QString> shape(TestDb& t)
{
    std::map<QString, QString> out;
    for (const Entry& e : t.repo->allEntries()) {
        if (e.secret) {
            continue;
        }
        out[e.contentHash] = e.note + QLatin1Char('|') + e.group + QLatin1Char('|') + (e.favorite ? "f" : "-") + (e.pinned() ? "p" : "-");
    }
    return out;
}

QString hashOf(TestDb& t, const QString& text)
{
    return t.repo->textHash(text);
}

ImageClip picture(int w, int h, const QString& hash, int bytes = 100)
{
    ImageClip i;
    i.png = QByteArray(bytes, 'P');
    i.thumbnail = QByteArray(10, 'T');
    i.width = w;
    i.height = h;
    i.pixelHash = hash;
    return i;
}

} // namespace

class TestSync : public QObject {
    Q_OBJECT

private slots:
    void fileRoundTrip()
    {
        TestDb a;
        a.repo->setSyncTracking(true);
        const auto x = a.repo->ingest(QStringLiteral("hello"));
        a.repo->setNote(x.id, QStringLiteral("greeting"));
        a.repo->setGroup(x.id, QStringLiteral("Work/Servers"));
        a.repo->setFavorite(x.id, true);
        a.repo->ingest(QStringLiteral("second"));
        a.repo->ingestImage(picture(4, 3, QStringLiteral("abcd")));
        a.repo->remove(a.repo->ingest(QStringLiteral("gone")).id);

        const s::Key key = makeKey();
        const s::Content c = exportOf(a);
        const QByteArray file = s::encode(c, key, kCheap);
        QVERIFY(file.startsWith("CLIPSYNC"));
        QVERIFY(!file.contains("hello")); // encrypted and compressed

        const s::Decoded d = s::decode(file, key);
        QCOMPARE(d.status, s::ReadStatus::Ok);
        QCOMPARE(d.content.deviceId, a.repo->syncDeviceId());
        QCOMPARE(d.content.items.size(), c.items.size());
        QCOMPARE(d.content.items.size(), size_t(3));
        QCOMPARE(d.content.tombstones.size(), size_t(1));
        const auto it = std::find_if(d.content.items.begin(), d.content.items.end(), [](const s::Item& i) { return i.content == QLatin1String("hello"); });
        QVERIFY(it != d.content.items.end());
        QCOMPARE(it->note, QStringLiteral("greeting"));
        QCOMPARE(it->group, QStringLiteral("Work/Servers"));
        QVERIFY(it->favorite);
        QVERIFY(!it->metaChanged.isEmpty());
        QVERIFY(d.content.groups.size() >= 2);
        const auto pic = std::find_if(d.content.items.begin(), d.content.items.end(), [](const s::Item& i) { return i.image; });
        QVERIFY(pic != d.content.items.end());
        QCOMPARE(pic->pixelHash, QStringLiteral("abcd"));
        QCOMPARE(pic->png, QByteArray(100, 'P'));
    }

    void wrongPassphraseAndTamperedFile()
    {
        TestDb a;
        a.repo->ingest(QStringLiteral("secret-ish text"));
        const s::Key key = makeKey();
        const QByteArray file = s::encode(exportOf(a), key, kCheap);

        // The same salt with another passphrase: does not open.
        const s::Key wrong = makeKey("wrong passphrase", key.salt);
        QCOMPARE(s::decode(file, wrong).status, s::ReadStatus::Damaged);
        // Another salt: another group of devices.
        QCOMPARE(s::decode(file, makeKey()).status, s::ReadStatus::OtherKey);

        // Any changed byte, in the box or in the header it authenticates.
        for (int pos : {s::peekHeader(file).valid ? 40 : 0, int(file.size()) - 1, int(file.size()) / 2}) {
            QByteArray bad = file;
            bad[pos] = char(bad.at(pos) ^ 0x01);
            QCOMPARE(s::decode(bad, key).status, s::ReadStatus::Damaged);
        }
        QByteArray badCost = file;
        badCost[26] = char(badCost.at(26) ^ 0x01); // the Argon2 cost is in the authenticated header
        QCOMPARE(s::decode(badCost, key).status, s::ReadStatus::Damaged);
        // Half a file (a sync client still copying).
        QCOMPARE(s::decode(file.left(file.size() / 2), key).status, s::ReadStatus::Damaged);
        QCOMPARE(s::decode(file.left(10), key).status, s::ReadStatus::NotOurs);
        QCOMPARE(s::decode(QByteArray("just some text, long enough to have a header, really"), key).status, s::ReadStatus::NotOurs);
        QCOMPARE(s::decode({}, key).status, s::ReadStatus::NotOurs);
        // A newer format.
        QByteArray newer = file;
        newer[8] = char(s::kFormatVersion + 1);
        QCOMPARE(s::decode(newer, key).status, s::ReadStatus::NewerVersion);
        // No key at all.
        QCOMPARE(s::decode(file, s::Key{}).status, s::ReadStatus::OtherKey);
    }

    void joiningAGroupNeedsTheRightPassphrase()
    {
        QTemporaryDir dir;
        bool existing = true;
        const auto first = s::keyForFolder(dir.path(), "pass one", &existing, kCheap);
        QVERIFY(first && first->isValid());
        QVERIFY(!existing); // nothing there: a new group with a new salt

        TestDb a;
        a.repo->ingest(QStringLiteral("x"));
        QString error;
        QVERIFY(s::writeAtomic(dir.path(), s::fileNameFor(QStringLiteral("dev1")), s::encode(exportOf(a), *first, kCheap), &error));

        const auto second = s::keyForFolder(dir.path(), "pass one", &existing, kCheap);
        QVERIFY(second && existing);
        QCOMPARE(second->salt, first->salt); // the group's salt is adopted
        QCOMPARE(second->key, first->key);
        QVERIFY(!s::keyForFolder(dir.path(), "pass two", &existing, kCheap)); // wrong passphrase: refused
        QVERIFY(existing);
    }

    void atomicWrite()
    {
        QTemporaryDir dir;
        const QString name = s::fileNameFor(QStringLiteral("dev"));
        QString error;
        QVERIFY2(s::writeAtomic(dir.path(), name, QByteArray("first"), &error), qPrintable(error));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1); // no temp file left
        QVERIFY2(s::writeAtomic(dir.path(), name, QByteArray("second, longer"), &error), qPrintable(error));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{name});
        QFile f(QDir(dir.path()).filePath(name));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("second, longer"));
        f.close();

        // Interrupted between the write and the rename: the old file is whole, no half file, no temp file.
        s::WriteHooks hooks;
        hooks.beforeRename = [&] {
            QStringList files = QDir(dir.path()).entryList(QDir::Files);
            return files.size() != 2 ? true : false; // the temp file exists next to the old one: 2 files
        };
        QVERIFY(!s::writeAtomic(dir.path(), name, QByteArray("third"), &error, hooks));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{name});
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("second, longer"));
        f.close();

        // The target cannot be replaced (a directory is in the way): the attempt cleans up.
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("blocked") + QLatin1String(s::kFileSuffix)));
        QVERIFY(!s::writeAtomic(dir.path(), QStringLiteral("blocked") + QLatin1String(s::kFileSuffix), QByteArray("x"), &error));
        QVERIFY(!error.isEmpty());
        QStringList left = QDir(dir.path()).entryList(QDir::Files);
        QCOMPARE(left, QStringList{name}); // no stray *.tmp-*
    }

    void folderUnavailableIsReportedAndNeverCreated()
    {
        QTemporaryDir dir;
        const QString gone = QDir(dir.path()).filePath(QStringLiteral("unplugged"));
        QCOMPARE(s::checkFolder(gone), s::FolderState::Missing);
        QCOMPARE(s::checkFolder(QString()), s::FolderState::Missing);
        QString error;
        QVERIFY(!s::writeAtomic(gone, QStringLiteral("a") + QLatin1String(s::kFileSuffix), QByteArray("x"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(gone)); // an unplugged share must not leave a local directory behind
        QVERIFY(s::readFolder(gone, makeKey(), QStringLiteral("me")).empty());

        QFile file(QDir(dir.path()).filePath(QStringLiteral("plain")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QCOMPARE(s::checkFolder(file.fileName()), s::FolderState::NotDirectory);
        QCOMPARE(s::checkFolder(dir.path()), s::FolderState::Ok);
    }

    void readFolderSkipsOwnFileAndReportsTheOthers()
    {
        QTemporaryDir dir;
        const s::Key key = makeKey();
        TestDb a;
        a.repo->ingest(QStringLiteral("from a"));
        TestDb b;
        b.repo->ingest(QStringLiteral("from b"));
        const QString idA = a.repo->syncDeviceId();
        const QString idB = b.repo->syncDeviceId();
        QVERIFY(idA != idB);
        QCOMPARE(idA.size(), 32);
        QVERIFY(s::writeAtomic(dir.path(), s::fileNameFor(idA), s::encode(exportOf(a), key, kCheap), nullptr));
        QVERIFY(s::writeAtomic(dir.path(), s::fileNameFor(idB), s::encode(exportOf(b), key, kCheap), nullptr));
        // another group's file, a damaged one, and a file that is not ours
        QVERIFY(s::writeAtomic(dir.path(), s::fileNameFor(QStringLiteral("other")), s::encode(exportOf(a), makeKey("x"), kCheap), nullptr));
        QVERIFY(s::writeAtomic(dir.path(), s::fileNameFor(QStringLiteral("broken")), QByteArray("CLIPSYNC\x01 not really"), nullptr));
        QVERIFY(s::writeAtomic(dir.path(), QStringLiteral("notes.txt"), QByteArray("hi"), nullptr));

        auto files = s::readFolder(dir.path(), key, idA);
        QCOMPARE(files.size(), size_t(3)); // not our own file, not notes.txt
        std::map<QString, s::ReadStatus> byName;
        for (const auto& f : files) {
            byName[f.name] = f.status;
        }
        QCOMPARE(byName[s::fileNameFor(idB)], s::ReadStatus::Ok);
        QCOMPARE(byName[s::fileNameFor(QStringLiteral("other"))], s::ReadStatus::OtherKey);
        QVERIFY(byName[s::fileNameFor(QStringLiteral("broken"))] != s::ReadStatus::Ok);

        // Unchanged files (same size and time) are not read again.
        QHash<QString, s::Known> known;
        for (const auto& f : files) {
            if (f.status == s::ReadStatus::Ok) {
                known.insert(f.name, {f.size, f.modified});
            }
        }
        files = s::readFolder(dir.path(), key, idA, known);
        const auto okFile = std::find_if(files.begin(), files.end(), [&](const auto& f) { return f.name == s::fileNameFor(idB); });
        QVERIFY(okFile != files.end() && okFile->unchanged && okFile->content.items.empty());
    }

    // ---- what goes into the file

    void passwordsPasteOnceAndPicturesOverTheCapAreNotExported()
    {
        TestDb a;
        a.repo->ingest(QStringLiteral("plain text"));
        a.repo->ingestSecret(QByteArray("sealed-blob"), QStringLiteral("secret:abc"), QStringLiteral("KeePass"), QString());
        const auto once = a.repo->ingest(QStringLiteral("one shot"));
        a.repo->setPasteOnce(once.id, true);
        a.repo->ingestImage(picture(2, 2, QStringLiteral("small"), 100));
        a.repo->ingestImage(picture(9, 9, QStringLiteral("huge"), 5000));

        s::Options o = options();
        o.maxPictureBytes = 1000;
        s::Content c = exportOf(a, o);
        QStringList texts;
        QStringList pics;
        for (const auto& i : c.items) {
            (i.image ? pics : texts) << (i.image ? i.pixelHash : i.content);
        }
        QCOMPARE(texts, QStringList{QStringLiteral("plain text")});
        QCOMPARE(pics, QStringList{QStringLiteral("small")}); // the cap
        const QByteArray file = s::encode(c, makeKey(), kCheap);
        QVERIFY(!file.contains("sealed-blob"));

        o.pictures = false; // the setting
        c = exportOf(a, o);
        QCOMPARE(c.items.size(), size_t(1));
    }

    void copyCapKeepsSavedAndTheNewest()
    {
        TestDb a;
        for (int i = 0; i < 20; ++i) {
            a.repo->ingest(QStringLiteral("old %1").arg(i));
        }
        const auto saved = a.repo->ingest(QStringLiteral("old saved"));
        a.repo->setPinned(saved.id, true);
        for (int i = 0; i < 20; ++i) {
            a.repo->ingest(QStringLiteral("new %1").arg(i));
        }
        s::Options o = options();
        o.maxCopies = 5;
        const s::Content c = exportOf(a, o);
        QCOMPARE(c.items.size(), size_t(6)); // 5 newest + the pinned one
        int fresh = 0;
        bool pinned = false;
        for (const auto& i : c.items) {
            fresh += i.content.startsWith(QLatin1String("new"));
            pinned |= i.pinned;
        }
        QCOMPARE(fresh, 5);
        QVERIFY(pinned);
    }

    void fingerprintChangesOnlyWhenTheFileWould()
    {
        TestDb a;
        a.repo->ingest(QStringLiteral("one"));
        const QString f1 = s::Collector(*a.repo, options()).fingerprint();
        QCOMPARE(s::Collector(*a.repo, options()).fingerprint(), f1); // nothing changed: no new file
        a.repo->ingestSecret(QByteArray("blob"), QStringLiteral("secret:1"), QString(), QString());
        QCOMPARE(s::Collector(*a.repo, options()).fingerprint(), f1); // a password copy is not in the file
        const auto two = a.repo->ingest(QStringLiteral("two"));
        const QString f2 = s::Collector(*a.repo, options()).fingerprint();
        QVERIFY(f2 != f1);
        a.repo->setNote(two.id, QStringLiteral("n"));
        const QString f3 = s::Collector(*a.repo, options()).fingerprint();
        QVERIFY(f3 != f2); // filing counts
        s::Options o = options();
        o.maxCopies = 1;
        QVERIFY(s::Collector(*a.repo, o).fingerprint() != f3); // so does a setting
    }

    // ---- merging

    void aNewCopyIsAddedWithItsOriginalTimes()
    {
        TestDb a;
        TestDb b;
        a.repo->ingest(QStringLiteral("old one"));
        const auto x = a.repo->ingest(QStringLiteral("newer"));
        a.repo->setGroup(x.id, QStringLiteral("Work"));
        a.repo->setNote(x.id, QStringLiteral("n"));
        b.clock = b.clock.addYears(1); // b's own clock is later: the copy must keep a's times
        const auto remote = exportOf(a);
        const auto stats = mergeInto(b, {remote});
        QCOMPARE(stats.added, 2);
        const auto e = b.repo->get(*b.repo->findByHash(hashOf(b, QStringLiteral("newer"))));
        QVERIFY(e);
        QCOMPARE(e->group, QStringLiteral("Work"));
        QCOMPARE(e->note, QStringLiteral("n"));
        QVERIFY(!e->autoFiled);
        const auto original = a.repo->get(x.id);
        QCOMPARE(e->lastUsed, original->lastUsed);
        QCOMPARE(e->createdAt, original->createdAt);
        QVERIFY(b.repo->groups().contains(QStringLiteral("Work")));
        // Newest first as on a: the synced copies sort among themselves as they did, not as "just copied".
        QCOMPARE(b.repo->recent(10, false).front().content, QStringLiteral("newer"));
    }

    void theSameCopyOnTwoDevicesStaysOneAndKeepsItsLastUsed()
    {
        TestDb a;
        TestDb b;
        a.repo->ingest(QStringLiteral("shared"));
        b.repo->ingest(QStringLiteral("other"));
        const auto mine = b.repo->ingest(QStringLiteral("shared"));
        const auto before = b.repo->get(mine.id);
        const auto stats = mergeInto(b, {exportOf(a)});
        QCOMPARE(stats.added, 0);
        QCOMPARE(b.repo->count(), qint64(2));
        QCOMPARE(b.repo->get(mine.id)->lastUsed, before->lastUsed);
    }

    void theNewestFilingWinsAsOneSet()
    {
        TestDb a;
        TestDb b;
        const auto xa = a.repo->ingest(QStringLiteral("x"));
        const auto xb = b.repo->ingest(QStringLiteral("x"));
        a.repo->syncBatch([&] { a.repo->syncSetMeta(xa.id, QStringLiteral("note A"), QStringLiteral("GroupA"), false, true, QStringLiteral("2026-03-01T10:00:00.000Z")); });
        b.repo->syncBatch([&] { b.repo->syncSetMeta(xb.id, QStringLiteral("note B"), QString(), true, false, QStringLiteral("2026-03-02T10:00:00.000Z")); });

        mergeInto(a, {exportOf(b)}); // b is newer: a takes b's whole filing (not group A with b's note)
        auto e = *a.repo->get(xa.id);
        QCOMPARE(e.note, QStringLiteral("note B"));
        QCOMPARE(e.group, QString());
        QVERIFY(e.favorite);
        QVERIFY(!e.pinned());

        mergeInto(b, {exportOf(a)}); // a now has b's filing and b's time: nothing to take
        e = *b.repo->get(xb.id);
        QCOMPARE(e.note, QStringLiteral("note B"));
        QVERIFY(shape(a) == shape(b));
    }

    void anOlderFilingDoesNotWin()
    {
        TestDb a;
        TestDb b;
        const auto xa = a.repo->ingest(QStringLiteral("x"));
        b.repo->ingest(QStringLiteral("x"));
        a.repo->syncBatch([&] { a.repo->syncSetMeta(xa.id, QStringLiteral("mine"), QString(), false, false, QStringLiteral("2026-05-01T00:00:00.000Z")); });
        const auto xb = *b.repo->findByHash(hashOf(b, QStringLiteral("x")));
        b.repo->syncBatch([&] { b.repo->syncSetMeta(xb, QStringLiteral("theirs"), QString(), false, false, QStringLiteral("2026-04-01T00:00:00.000Z")); });
        const auto stats = mergeInto(a, {exportOf(b)});
        QCOMPARE(stats.updated, 0);
        QCOMPARE(a.repo->get(xa.id)->note, QStringLiteral("mine"));
    }

    void aTombstoneDeletesUnlessTheLocalCopyIsNewer()
    {
        TestDb a;
        a.repo->ingest(QStringLiteral("doomed")); // used 2026-01-01
        a.repo->ingest(QStringLiteral("kept"));
        a.repo->ingest(QStringLiteral("untouched"));
        s::Content remote;
        remote.deviceId = QStringLiteral("dev");
        remote.tombstones.push_back({hashOf(a, QStringLiteral("doomed")), QStringLiteral("2026-06-01T00:00:00.000Z")}); // after its last use
        remote.tombstones.push_back({hashOf(a, QStringLiteral("kept")), QStringLiteral("2025-06-01T00:00:00.000Z")}); // before it
        remote.tombstones.push_back({hashOf(a, QStringLiteral("not here")), QStringLiteral("2026-06-01T00:00:00.000Z")});
        const auto stats = mergeInto(a, {remote});
        QCOMPARE(stats.deleted, 1);
        QVERIFY(!a.repo->findByHash(hashOf(a, QStringLiteral("doomed"))));
        QVERIFY(a.repo->findByHash(hashOf(a, QStringLiteral("kept"))));
        QVERIFY(a.repo->findByHash(hashOf(a, QStringLiteral("untouched"))));

        // And the copy does not come back from a file that still has it, as long as it is older than the tombstone.
        s::Content stale;
        stale.deviceId = QStringLiteral("zz");
        s::Item item;
        item.content = QStringLiteral("doomed");
        item.created = item.lastUsed = QStringLiteral("2026-01-01T12:00:00.001Z");
        stale.items.push_back(item);
        QCOMPARE(mergeInto(a, {remote, stale}).added, 0);
        QVERIFY(!a.repo->findByHash(hashOf(a, QStringLiteral("doomed"))));
        // Used again after the deletion: it is added.
        stale.items[0].lastUsed = QStringLiteral("2026-07-01T00:00:00.000Z");
        QCOMPARE(mergeInto(a, {remote, stale}).added, 1);
    }

    void aFiledCopyIsDeletedByATombstoneToo()
    {
        TestDb a;
        const auto x = a.repo->ingest(QStringLiteral("pinned thing"));
        a.repo->setPinned(x.id, true);
        s::Content remote;
        remote.deviceId = QStringLiteral("dev");
        remote.tombstones.push_back({hashOf(a, QStringLiteral("pinned thing")), QStringLiteral("2999-01-01T00:00:00.000Z")});
        QCOMPARE(mergeInto(a, {remote}).deleted, 1);
        QCOMPARE(a.repo->count(), qint64(0));
    }

    void aTombstoneNeverTouchesAPasswordCopy()
    {
        TestDb a;
        const auto pw = a.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:feed"), QStringLiteral("note"), QString());
        const auto text = a.repo->ingest(QStringLiteral("t"));
        a.repo->setPasteOnce(text.id, true);
        s::Content remote;
        remote.deviceId = QStringLiteral("dev");
        remote.tombstones.push_back({QStringLiteral("secret:feed"), QStringLiteral("2999-01-01T00:00:00.000Z")});
        remote.tombstones.push_back({hashOf(a, QStringLiteral("t")), QStringLiteral("2999-01-01T00:00:00.000Z")});
        QCOMPARE(mergeInto(a, {remote}).deleted, 0);
        QVERIFY(a.repo->get(pw.id));
        QVERIFY(a.repo->get(text.id));
    }

    void invalidItemsInAFileAreIgnored()
    {
        TestDb a;
        s::Content remote;
        remote.deviceId = QStringLiteral("dev");
        s::Item empty;
        empty.lastUsed = QStringLiteral("2026-01-01T00:00:00.000Z");
        s::Item noTime;
        noTime.content = QStringLiteral("no time");
        s::Item brokenPicture;
        brokenPicture.image = true;
        brokenPicture.lastUsed = QStringLiteral("2026-01-01T00:00:00.000Z");
        s::Item huge;
        huge.content = QString(2000, QLatin1Char('x'));
        huge.lastUsed = QStringLiteral("2026-01-01T00:00:00.000Z");
        remote.items = {empty, noTime, brokenPicture, huge};
        s::Options o = options();
        o.maxItemBytes = 1000;
        const auto stats = mergeInto(a, {remote}, o);
        QCOMPARE(stats.added, 0);
        QCOMPARE(stats.skipped, 4);
        QCOMPARE(a.repo->count(), qint64(0));
    }

    void picturesAreMerged()
    {
        TestDb a;
        TestDb b;
        a.repo->ingestImage(picture(8, 6, QStringLiteral("pix1")));
        const auto stats = mergeInto(b, {exportOf(a)});
        QCOMPARE(stats.added, 1);
        const auto id = b.repo->findByHash(QStringLiteral("image:pix1"));
        QVERIFY(id);
        QCOMPARE(b.repo->imageData(*id), QByteArray(100, 'P'));
        QCOMPARE(b.repo->get(*id)->imageWidth, 8);
    }

    void aCopyThatWouldBePrunedAtOnceIsNotAdded()
    {
        TestDb a;
        TestDb b;
        a.repo->ingest(QStringLiteral("ancient")); // 2026-01-01 12:00:00.001
        b.clock = b.clock.addMonths(2);
        for (int i = 0; i < 3; ++i) {
            b.repo->ingest(QStringLiteral("recent %1").arg(i));
        }
        s::Options o = options();
        o.maxHistory = 3;
        const auto stats = mergeInto(b, {exportOf(a)}, o);
        QCOMPARE(stats.added, 0);
        QCOMPARE(stats.skipped, 1);
        // ...but a pinned one is, however old.
        const auto p = a.repo->ingest(QStringLiteral("ancient pinned"));
        a.repo->setPinned(p.id, true);
        QCOMPARE(mergeInto(b, {exportOf(a)}, o).added, 1);
    }

    void mergingTwiceChangesNothing()
    {
        TestDb a;
        TestDb b;
        const auto x = a.repo->ingest(QStringLiteral("x"));
        a.repo->setGroup(x.id, QStringLiteral("G"));
        a.repo->setFavorite(x.id, true);
        a.repo->ingest(QStringLiteral("y"));
        a.repo->ingestImage(picture(3, 3, QStringLiteral("pp")));
        const auto doomed = b.repo->ingest(QStringLiteral("z"));
        Q_UNUSED(doomed)
        s::Content remote = exportOf(a);
        remote.tombstones.push_back({hashOf(b, QStringLiteral("z")), QStringLiteral("2999-01-01T00:00:00.000Z")});
        const auto first = mergeInto(b, {remote});
        QCOMPARE(first.added, 3);
        QCOMPARE(first.deleted, 1);
        const auto before = shape(b);
        const auto times = [&] {
            QStringList t;
            for (const Entry& e : b.repo->allEntries()) {
                t << e.lastUsed.toString(Qt::ISODateWithMs);
            }
            return t;
        };
        const QStringList t1 = times();
        const auto second = mergeInto(b, {remote});
        QCOMPARE(second.added, 0);
        QCOMPARE(second.updated, 0);
        QCOMPARE(second.deleted, 0);
        QVERIFY(shape(b) == before);
        QCOMPARE(times(), t1);
    }

    void userDeletionsAreSharedButPruningIsNot()
    {
        TestDb a;
        a.repo->setSyncTracking(true);
        const auto del = a.repo->ingest(QStringLiteral("deleted by hand"));
        a.repo->remove(del.id);
        for (int i = 0; i < 5; ++i) {
            a.repo->ingest(QStringLiteral("p %1").arg(i));
        }
        QCOMPARE(a.repo->prune(2), qint64(3)); // pruned: local only
        a.repo->ingestSecret(QByteArray("blob"), QStringLiteral("secret:1"), QString(), QString());
        a.repo->clearHistory(true); // clear: shared (including the password copy? never)
        const s::Content c = exportOf(a);
        QStringList hashes;
        for (const auto& t : c.tombstones) {
            hashes << t.hash;
        }
        QVERIFY(hashes.contains(hashOf(a, QStringLiteral("deleted by hand"))));
        QVERIFY(hashes.contains(hashOf(a, QStringLiteral("p 3")))); // cleared with Clear history
        QVERIFY(!hashes.contains(hashOf(a, QStringLiteral("p 0")))); // pruned earlier: not shared...
        QVERIFY(!hashes.contains(QStringLiteral("secret:1")));
        for (const auto& t : c.tombstones) {
            QVERIFY(!t.hash.startsWith(QLatin1String("secret:")));
        }
        // ...but remembered here: this PC does not take it back from another PC's file.
        QVERIFY(!a.repo->syncTombstone(hashOf(a, QStringLiteral("p 0"))).isEmpty());
        s::Content other;
        other.deviceId = QStringLiteral("dev");
        s::Item back;
        back.content = QStringLiteral("p 0");
        back.created = back.lastUsed = QStringLiteral("2026-01-01T12:00:00.001Z");
        other.items.push_back(back);
        QCOMPARE(mergeInto(a, {other}).added, 0);
    }

    void nothingIsTrackedWhileSyncIsOff()
    {
        TestDb a;
        a.repo->remove(a.repo->ingest(QStringLiteral("x")).id);
        QVERIFY(exportOf(a).tombstones.empty());
        QVERIFY(a.repo->syncTombstone(hashOf(a, QStringLiteral("x"))).isEmpty());
    }

    void twoDevicesConverge()
    {
        TestDb a;
        TestDb b;
        a.repo->setSyncTracking(true);
        b.repo->setSyncTracking(true);
        // each has its own copies, a shared one, and its own filing and deletion
        a.repo->ingest(QStringLiteral("only on a"));
        b.repo->ingest(QStringLiteral("only on b"));
        const auto sa = a.repo->ingest(QStringLiteral("both"));
        b.repo->ingest(QStringLiteral("both"));
        const auto gone = a.repo->ingest(QStringLiteral("deleted on a"));
        b.repo->ingest(QStringLiteral("deleted on a"));
        a.repo->setNote(sa.id, QStringLiteral("a's note"));
        a.repo->setGroup(sa.id, QStringLiteral("Shared/Deep"));
        const auto pinB = b.repo->ingest(QStringLiteral("b pins this"));
        b.repo->setPinned(pinB.id, true);
        b.repo->ingestImage(picture(5, 5, QStringLiteral("bpic")));
        a.repo->remove(gone.id);

        for (int round = 0; round < 3; ++round) {
            const s::Content ca = exportOf(a);
            const s::Content cb = exportOf(b);
            mergeInto(a, {cb});
            mergeInto(b, {ca});
        }
        QVERIFY(shape(a) == shape(b));
        QCOMPARE(shape(a).size(), size_t(5));
        QVERIFY(!a.repo->findByHash(hashOf(a, QStringLiteral("deleted on a"))));
        QVERIFY(!b.repo->findByHash(hashOf(b, QStringLiteral("deleted on a"))));
        QCOMPARE(b.repo->get(*b.repo->findByHash(hashOf(b, QStringLiteral("both"))))->group, QStringLiteral("Shared/Deep"));
        QVERIFY(a.repo->get(*a.repo->findByHash(hashOf(a, QStringLiteral("b pins this"))))->pinned());
        // Settled: the files do not change any more, so nothing is rewritten.
        const QString fa = s::Collector(*a.repo, options()).fingerprint();
        mergeInto(a, {exportOf(b)});
        QCOMPARE(s::Collector(*a.repo, options()).fingerprint(), fa);
    }

    void threeDevicesThroughAFolder()
    {
        QTemporaryDir dir;
        const s::Key key = makeKey();
        TestDb dev[3];
        for (int i = 0; i < 3; ++i) {
            dev[i].repo->ingest(QStringLiteral("copy from %1").arg(i));
        }
        for (int round = 0; round < 2; ++round) {
            for (int i = 0; i < 3; ++i) {
                QString error;
                QVERIFY2(s::writeAtomic(dir.path(), s::fileNameFor(dev[i].repo->syncDeviceId()), s::encode(exportOf(dev[i]), key, kCheap), &error), qPrintable(error));
            }
            for (int i = 0; i < 3; ++i) {
                std::vector<s::Content> remotes;
                for (auto& f : s::readFolder(dir.path(), key, dev[i].repo->syncDeviceId())) {
                    QCOMPARE(f.status, s::ReadStatus::Ok);
                    remotes.push_back(std::move(f.content));
                }
                QCOMPARE(remotes.size(), size_t(2));
                mergeInto(dev[i], std::move(remotes));
            }
        }
        QVERIFY(shape(dev[0]) == shape(dev[1]));
        QVERIFY(shape(dev[1]) == shape(dev[2]));
        QCOMPARE(shape(dev[0]).size(), size_t(3));
    }

    // ---- not stalling the GUI thread

    void readingOffThreadAndMergingInBatchesDoesNotStallTheGui()
    {
        TestDb a;
        const int n = 3000;
        a.repo->syncBatch([&] {
            for (int i = 0; i < n; ++i) {
                a.repo->ingest(QStringLiteral("a fairly ordinary copied line number %1 with some more words to it").arg(i));
            }
        });
        QTemporaryDir dir;
        const s::Key key = makeKey();
        s::Options o = options();
        o.maxCopies = 5000;
        o.maxHistory = 10000;
        QVERIFY(s::writeAtomic(dir.path(), s::fileNameFor(a.repo->syncDeviceId()), s::encode(exportOf(a, o), key, kCheap), nullptr));
        TestDb b;

        StallWatchdog wd;
        // 1. the folder is read, decrypted and parsed on a worker
        QFutureWatcher<std::vector<s::RemoteFile>> watcher;
        QSignalSpy done(&watcher, &QFutureWatcher<std::vector<s::RemoteFile>>::finished);
        watcher.setFuture(QtConcurrent::run([&] { return s::readFolder(dir.path(), key, QStringLiteral("b")); }));
        QVERIFY(done.wait(30000));
        auto files = watcher.result();
        QCOMPARE(files.size(), size_t(1));
        QCOMPARE(files[0].status, s::ReadStatus::Ok);
        std::vector<s::Content> remotes;
        remotes.push_back(std::move(files[0].content));

        // 2. the merge into the database: a batch per turn of the event loop
        s::Merger merger(*b.repo, std::move(remotes), o);
        bool finished = false;
        QTimer turn;
        connect(&turn, &QTimer::timeout, this, [&] {
            if (merger.step(s::kBatch)) {
                finished = true;
                turn.stop();
            }
        });
        turn.start(0);
        QTRY_VERIFY_WITH_TIMEOUT(finished, 60000);
        QCOMPARE(merger.stats().added, n);

        // 3. collecting this device's file likewise
        s::Collector collector(*b.repo, o);
        bool collected = false;
        QTimer collect;
        connect(&collect, &QTimer::timeout, this, [&] {
            if (collector.step(s::kBatch)) {
                collected = true;
                collect.stop();
            }
        });
        collect.start(0);
        QTRY_VERIFY_WITH_TIMEOUT(collected, 60000);
        QCOMPARE(collector.copies(), n);
        QVERIFY_NO_STALL(wd, "sync read + merge + collect of 3000 copies");
    }
};

QTEST_GUILESS_MAIN(TestSync)
#include "tst_sync.moc"
