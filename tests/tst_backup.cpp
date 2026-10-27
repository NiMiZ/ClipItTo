#include "Backup.h"
#include "FakeKeyStore.h"
#include "ImageClip.h"
#include "PasswordKeeper.h"
#include "TestDb.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using clipitto::PasswordKeeper;
using clipitto::PasswordVault;
namespace backup = clipitto::backup;

class TestBackup : public QObject {
    Q_OBJECT

private:
    static bool wait(const std::function<void(PasswordKeeper::Done)>& call)
    {
        std::optional<bool> result;
        call([&result](bool ok) { result = ok; });
        return QTest::qWaitFor([&result] { return result.has_value(); }, 10000) && *result;
    }

    // A history with a keeper (master password "master"), unlocked.
    struct Side {
        TestDb t;
        std::shared_ptr<FakeKeyStore> keys = std::make_shared<FakeKeyStore>();
        PasswordKeeper keeper{*t.repo, keys};

        Side() { keeper.setKdfCost({64, 1}); }

        bool setUp(const QByteArray& master)
        {
            return wait([&](auto done) { keeper.setMasterPassword(master, done); })
                && wait([&](auto done) { keeper.unlock(master, done); });
        }
    };

    QTemporaryDir dir;
    QString file() const { return dir.filePath(QStringLiteral("backup.json")); }

private slots:
    void copiesGroupsStarsPinsAndPicturesRoundTrip()
    {
        TestDb a;
        const auto plain = a.repo->ingest(QStringLiteral("plain")).id;
        const auto grouped = a.repo->ingest(QStringLiteral("kubectl get pods")).id;
        const auto starred = a.repo->ingest(QStringLiteral("starred")).id;
        const auto pinned = a.repo->ingest(QStringLiteral("pinned")).id;
        a.repo->setNote(grouped, QStringLiteral("pods"));
        a.repo->setGroup(grouped, QStringLiteral("Work/Servers"));
        a.repo->setFavorite(starred, true);
        a.repo->setPinned(pinned, true);
        a.repo->setGroupHiddenFromRecent(QStringLiteral("Work"), true);
        // A custom order (Zebra before Apple, which is not alphabetical) and a copy taken off Recent.
        const auto zebra = a.repo->ingest(QStringLiteral("z")).id;
        const auto apple = a.repo->ingest(QStringLiteral("a")).id;
        a.repo->setGroup(zebra, QStringLiteral("Zebra"));
        a.repo->setGroup(apple, QStringLiteral("Apple"));
        a.repo->moveGroup(QStringLiteral("Zebra"), -1);
        a.repo->takeOffRecent(zebra);
        clipitto::ImageClip image;
        image.png = QByteArray("not-really-a-png");
        image.thumbnail = QByteArray("thumb");
        image.width = 2;
        image.height = 3;
        image.pixelHash = QStringLiteral("abc123");
        a.repo->ingestImage(image);
        Q_UNUSED(plain)

        QString error;
        QVERIFY2(backup::exportTo(file(), *a.repo, clipitto::Config{}, nullptr, {true, false}, {}, &error), qPrintable(error));
        const backup::Info info = backup::inspect(file());
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY(info.parts.copies);
        QVERIFY(!info.parts.settings);
        QCOMPARE(info.copies, 7);
        QCOMPARE(info.passwords, 0);

        TestDb b;
        const auto report = backup::importFrom(file(), {true, true}, *b.repo, nullptr, {}, &error);
        QVERIFY2(report, qPrintable(error));
        QCOMPARE(report->added, 7);
        QVERIFY(!report->settings); // the file has none
        QCOMPARE(b.repo->count(), 7);
        const auto g = b.repo->search(QStringLiteral("kubectl")).front();
        QCOMPARE(g.group, QStringLiteral("Work/Servers"));
        QCOMPARE(g.note, QStringLiteral("pods"));
        QCOMPARE(g.createdAt, a.repo->get(grouped)->createdAt);
        QVERIFY(b.repo->search(QStringLiteral("starred")).front().favorite);
        QCOMPARE(b.repo->pinned(10).size(), 1u);
        QVERIFY(b.repo->groupHiddenFromRecent(QStringLiteral("Work")));
        QCOMPARE(b.repo->countImages(), 1);
        QCOMPARE(b.repo->offRecentIds().size(), 1); // still off Recent
        QCOMPARE(b.repo->groups(), a.repo->groups());
        const auto picture = b.repo->findByHash(QStringLiteral("image:abc123"));
        QVERIFY(picture);
        QCOMPARE(b.repo->imageData(*picture), image.png);

        // Importing again changes nothing.
        const auto again = backup::importFrom(file(), {true, false}, *b.repo, nullptr, {}, &error);
        QVERIFY(again);
        QCOMPARE(again->added, 0);
        QCOMPARE(again->alreadyThere, 7);
        QCOMPARE(b.repo->count(), 7);
    }

    void settingsRoundTripAndPartsAreChosen()
    {
        TestDb a;
        a.repo->ingest(QStringLiteral("x"));
        clipitto::Config config;
        config.recentCount = 33;
        config.theme = clipitto::Theme::Dark;
        QString error;
        QVERIFY(backup::exportTo(file(), *a.repo, config, nullptr, {true, true}, {}, &error));
        const backup::Info info = backup::inspect(file());
        QVERIFY(info.parts.copies && info.parts.settings);

        TestDb b;
        const auto onlySettings = backup::importFrom(file(), {false, true}, *b.repo, nullptr, {}, &error);
        QVERIFY(onlySettings);
        QVERIFY(onlySettings->settings);
        QCOMPARE(onlySettings->settings->recentCount, 33);
        QCOMPARE(int(onlySettings->settings->theme), int(clipitto::Theme::Dark));
        QCOMPARE(b.repo->count(), 0); // no copies asked for

        const auto onlyCopies = backup::importFrom(file(), {true, false}, *b.repo, nullptr, {}, &error);
        QVERIFY(onlyCopies);
        QVERIFY(!onlyCopies->settings);
        QCOMPARE(b.repo->count(), 1);

        QVERIFY(!backup::exportTo(file(), *a.repo, config, nullptr, {false, false}, {}, &error)); // nothing chosen
    }

    void passwordsAreEncryptedWithTheMasterPasswordAndMoveToAnotherComputer()
    {
        Side a;
        QVERIFY(a.setUp("master"));
        const PasswordVault& va = a.keeper.vault();
        const auto id = a.t.repo->ingestSecret(va.seal("hunter2"), va.match("hunter2"), QStringLiteral("KeePassXC"),
                                               QStringLiteral("Passwords")).id;
        a.t.repo->setFavorite(id, true);
        a.t.repo->ingest(QStringLiteral("plain text"));

        QString error;
        // Locked or without the password: refused.
        a.keeper.lock();
        QVERIFY(!backup::exportTo(file(), *a.t.repo, clipitto::Config{}, &a.keeper, {true, false}, "master", &error));
        QVERIFY(wait([&](auto done) { a.keeper.unlock("master", done); }));
        QVERIFY(!backup::exportTo(file(), *a.t.repo, clipitto::Config{}, &a.keeper, {true, false}, {}, &error));
        QVERIFY2(backup::exportTo(file(), *a.t.repo, clipitto::Config{}, &a.keeper, {true, false}, "master", &error),
                 qPrintable(error));

        QFile f(file());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray contents = f.readAll();
        f.close();
        QVERIFY(!contents.contains("hunter2")); // never in clear
        QVERIFY(contents.contains("plain text")); // the rest is plain, as documented
        const backup::Info info = backup::inspect(file());
        QCOMPARE(info.passwords, 1);
        QCOMPARE(info.copies, 1);

        QVERIFY(backup::passwordOpens(file(), "master"));
        QVERIFY(!backup::passwordOpens(file(), "other"));

        // Another computer: its own master password, vault and keyring.
        Side b;
        QVERIFY(b.setUp("a different master"));
        const auto wrong = backup::importFrom(file(), {true, false}, *b.t.repo, &b.keeper, "a different master", &error);
        QVERIFY(wrong);
        QCOMPARE(wrong->passwordsAdded, 0);
        QCOMPARE(wrong->passwordsSkipped, 1);
        QCOMPARE(wrong->added, 1); // the plain copy still came

        const auto ok = backup::importFrom(file(), {true, false}, *b.t.repo, &b.keeper, "master", &error);
        QVERIFY(ok);
        QCOMPARE(ok->passwordsAdded, 1);
        QCOMPARE(b.t.repo->countSecrets(), 1);
        const auto restored = b.t.repo->findByHash(b.keeper.vault().match("hunter2"));
        QVERIFY(restored);
        QCOMPARE(b.keeper.reveal(*restored).value_or(QByteArray()), QByteArray("hunter2")); // under b's own vault
        const auto entry = b.t.repo->get(*restored);
        QCOMPARE(entry->note, QStringLiteral("KeePassXC"));
        QCOMPARE(entry->group, QStringLiteral("Passwords"));
        QVERIFY(entry->favorite);

        // Again: it is already there.
        const auto twice = backup::importFrom(file(), {true, false}, *b.t.repo, &b.keeper, "master", &error);
        QCOMPARE(twice->passwordsAdded, 0);
        QCOMPARE(b.t.repo->countSecrets(), 1);

        // A computer without a master password skips them.
        TestDb c;
        const auto none = backup::importFrom(file(), {true, false}, *c.repo, nullptr, "master", &error);
        QVERIFY(none);
        QCOMPARE(none->passwordsSkipped, 1);
        QCOMPARE(c.repo->countSecrets(), 0);
        QCOMPARE(c.repo->count(), 1);
    }

    void badFilesAreRefusedWithAMessage()
    {
        TestDb a;
        QString error;
        QVERIFY(!backup::importFrom(dir.filePath(QStringLiteral("missing.json")), {true, true}, *a.repo, nullptr, {}, &error));
        QVERIFY(!error.isEmpty());

        QFile f(file());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"hello\": 1}");
        f.close();
        const backup::Info info = backup::inspect(file());
        QVERIFY(!info.valid);
        QVERIFY(info.error.contains(QLatin1String("not a ClipItTo backup")));

        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"format\": \"clipitto-backup\", \"version\": 99}");
        f.close();
        QVERIFY(backup::inspect(file()).error.contains(QLatin1String("newer")));
    }
};

QTEST_GUILESS_MAIN(TestBackup)
#include "tst_backup.moc"
