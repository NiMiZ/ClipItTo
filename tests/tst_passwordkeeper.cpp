#include "FakeKeyStore.h"
#include "KeyStore.h"
#include "PasswordKeeper.h"
#include "TestDb.h"

#include <QSignalSpy>
#include <QTest>

using clipitto::PasswordKeeper;
using clipitto::PasswordVault;

// Password copies' lifecycle: master password, unlock/lock, and what needs an unlock.
class TestPasswordKeeper : public QObject {
    Q_OBJECT

private:
    // Runs an async keeper call to completion.
    static bool wait(const std::function<void(PasswordKeeper::Done)>& call)
    {
        std::optional<bool> result;
        call([&result](bool ok) { result = ok; });
        if (!QTest::qWaitFor([&result] { return result.has_value(); }, 10000)) {
            return false;
        }
        return *result;
    }

    struct Fixture {
        TestDb t;
        std::shared_ptr<FakeKeyStore> keys = std::make_shared<FakeKeyStore>();
        PasswordKeeper keeper{*t.repo, keys};

        Fixture() { keeper.setKdfCost({64, 1}); }

        qint64 storePassword(const QByteArray& password, const QString& note = {})
        {
            const PasswordVault& v = keeper.vault();
            return t.repo->ingestSecret(v.seal(password), v.match(password), note, QString()).id;
        }
    };

private slots:
    void masterPasswordSetsUpTheVault()
    {
        Fixture f;
        f.keeper.reload();
        QVERIFY(!f.keeper.isSetUp());
        QSignalSpy changed(&f.keeper, &PasswordKeeper::vaultChanged);
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        QVERIFY(f.keeper.isSetUp());
        QVERIFY(!f.keeper.isUnlocked());
        QVERIFY(f.keeper.canMatch());
        QVERIFY(f.keys->stored && f.keys->stored->size() == 32); // the match key went to the keyring
        QCOMPARE(changed.size(), 1);
        QVERIFY(f.t.repo->loadVault());
        QVERIFY(!wait([&](auto done) { f.keeper.setMasterPassword("again", done); })); // change it instead

        // After a restart: the same vault and match key.
        PasswordKeeper again(*f.t.repo, f.keys);
        again.reload();
        QVERIFY(again.isSetUp());
        QVERIFY(again.canMatch());
    }

    // A locked keyring must not be asked about at every start: the match key is read when a password is first
    // copied or searched.
    void theKeyringIsFirstAskedWhenTheMatchKeyIsNeeded()
    {
        Fixture f;
        f.keeper.reload();
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));

        int asked = 0;
        QStringList purposes;
        auto notice = std::make_shared<std::function<void(const QString&)>>([&](const QString& purpose) {
            ++asked;
            purposes << purpose;
        });
        auto counted = std::make_shared<clipitto::AnnouncingKeyStore>(f.keys, QStringLiteral("passwords"), notice);
        PasswordKeeper restarted(*f.t.repo, counted);
        restarted.setKdfCost({64, 1});
        restarted.reload();
        QVERIFY(restarted.isSetUp());
        QCOMPARE(asked, 0); // starting asks nothing
        QVERIFY(restarted.canMatch()); // "not asked yet" is not "missing"

        QByteArray plain = "hunter2";
        QVERIFY(!restarted.vault().match(plain).isEmpty()); // the first need asks, once
        QCOMPARE(asked, 1);
        QVERIFY(!restarted.vault().match(plain).isEmpty());
        QCOMPARE(asked, 1);
        QCOMPARE(purposes, QStringList{QStringLiteral("passwords")});

        // Without a keyring the answer comes at the first need, and canMatch() turns false then.
        f.keys->isAvailable = false;
        PasswordKeeper nokeys(*f.t.repo, counted);
        nokeys.setKdfCost({64, 1});
        nokeys.reload();
        QVERIFY(nokeys.canMatch());
        QVERIFY(nokeys.vault().match(plain).isEmpty());
        QVERIFY(!nokeys.canMatch());
        QVERIFY(!nokeys.matchProblem().isEmpty());
    }

    void withoutAMasterPasswordItIsOpenAtEveryStartAndNeverLocks()
    {
        Fixture f;
        f.keeper.reload();
        QVERIFY(wait([&](auto done) { f.keeper.useWithoutMasterPassword(done); }));
        QVERIFY(f.keeper.isSetUp());
        QVERIFY(f.keeper.isOpen());
        QVERIFY(f.keeper.isUnlocked());
        const qint64 id = f.storePassword("hunter2", QStringLiteral("KeePassXC"));
        f.keeper.lock(); // does nothing
        QVERIFY(f.keeper.isUnlocked());
        QCOMPARE(f.keeper.reveal(id).value_or(QByteArray()), QByteArray("hunter2"));

        // After a restart it opens by itself.
        PasswordKeeper again(*f.t.repo, f.keys);
        again.setKdfCost({64, 1});
        again.reload();
        QVERIFY(again.isOpen());
        QTRY_VERIFY_WITH_TIMEOUT(again.isUnlocked(), 10000);
        QCOMPARE(again.reveal(id).value_or(QByteArray()), QByteArray("hunter2"));
    }

    void anUnlockedVaultCanGetANewMasterPasswordWithoutTheOldOne()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("old", done); }));
        QVERIFY(wait([&](auto done) { f.keeper.unlock("old", done); }));
        const qint64 id = f.storePassword("hunter2");
        // Locked: refused.
        f.keeper.lock();
        QVERIFY(!wait([&](auto done) { f.keeper.resetMasterPassword("Brand-new-47x", done); }));
        QVERIFY(wait([&](auto done) { f.keeper.unlock("old", done); })); // stands in for a Windows Hello unlock
        QVERIFY(wait([&](auto done) { f.keeper.resetMasterPassword("Brand-new-47x", done); }));
        QVERIFY(!f.keeper.isUnlocked());
        QVERIFY(!wait([&](auto done) { f.keeper.unlock("old", done); }));
        QVERIFY(wait([&](auto done) { f.keeper.unlock("Brand-new-47x", done); }));
        QCOMPARE(f.keeper.reveal(id).value_or(QByteArray()), QByteArray("hunter2")); // kept
    }

    void aMasterPasswordCanReplaceTheOpenMode()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.useWithoutMasterPassword(done); }));
        const qint64 id = f.storePassword("hunter2");
        QVERIFY(wait([&](auto done) { f.keeper.changeMasterPassword(PasswordKeeper::openPassword(), "Real-one-47x", done); }));
        QVERIFY(!f.keeper.isOpen());
        QVERIFY(!f.keeper.isUnlocked());
        QVERIFY(!f.t.repo->vaultOpen());
        QVERIFY(wait([&](auto done) { f.keeper.unlock("Real-one-47x", done); }));
        QCOMPARE(f.keeper.reveal(id).value_or(QByteArray()), QByteArray("hunter2"));
        f.keeper.lock();
        QVERIFY(!f.keeper.isUnlocked()); // locks now

        // Turning it off deletes the copies.
        Fixture g;
        QVERIFY(wait([&](auto done) { g.keeper.useWithoutMasterPassword(done); }));
        g.storePassword("x");
        g.keeper.reset();
        QVERIFY(!g.keeper.isOpen());
        QCOMPARE(g.t.repo->countSecrets(), 0);
        QVERIFY(!g.t.repo->vaultOpen());
    }

    void unlockRevealsAndLockHides()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        const qint64 id = f.storePassword("hunter2");
        QVERIFY(!f.keeper.reveal(id));
        QCOMPARE(f.keeper.secondsLeft(), 0);

        QSignalSpy lockChanged(&f.keeper, &PasswordKeeper::lockChanged);
        QVERIFY(!wait([&](auto done) { f.keeper.unlock("wrong", done); }));
        QVERIFY(!f.keeper.isUnlocked());
        QVERIFY(wait([&](auto done) { f.keeper.unlock("master", done); }));
        QVERIFY(f.keeper.isUnlocked());
        QCOMPARE(lockChanged.size(), 1);
        QVERIFY(f.keeper.secondsLeft() > 0);
        QCOMPARE(f.keeper.reveal(id).value_or(QByteArray()), QByteArrayLiteral("hunter2"));
        QVERIFY(!f.keeper.reveal(f.t.repo->ingest(QStringLiteral("text")).id)); // not a password

        f.keeper.lock();
        QVERIFY(!f.keeper.isUnlocked());
        QCOMPARE(lockChanged.size(), 2);
        QVERIFY(!f.keeper.reveal(id));
    }

    void anUnlockFinishingAfterALockIsDropped()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        std::optional<bool> result;
        f.keeper.unlock("master", [&result](bool ok) { result = ok; });
        f.keeper.lock(); // e.g. "Lock now" while the key is still being derived
        QTRY_VERIFY(result.has_value());
        QVERIFY(!*result);
        QVERIFY(!f.keeper.isUnlocked());
    }

    void searchMatchesPartOnlyWhenUnlocked()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        const qint64 a = f.storePassword("Correct-Horse-1");
        const qint64 b = f.storePassword("battery staple");

        QCOMPARE(f.keeper.search(QStringLiteral("Correct-Horse-1")), (std::vector<qint64>{a}));
        QVERIFY(f.keeper.search(QStringLiteral("horse")).empty()); // locked: whole password only
        QVERIFY(f.keeper.search(QString()).empty());

        QVERIFY(wait([&](auto done) { f.keeper.unlock("master", done); }));
        QCOMPARE(f.keeper.search(QStringLiteral("horse")), (std::vector<qint64>{a}));
        QCOMPARE(f.keeper.search(QStringLiteral("t")), (std::vector<qint64>{b, a})); // newest first
        QVERIFY(f.keeper.search(QStringLiteral("zzz")).empty());
    }

    void withoutAKeyringThereIsNoLockedMatch()
    {
        Fixture f;
        f.keys->isAvailable = false;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        QVERIFY(!f.keeper.canMatch());
        QVERIFY(!f.keeper.matchProblem().isEmpty());
        f.storePassword("pw1");
        QVERIFY(f.keeper.search(QStringLiteral("pw1")).empty());
        QVERIFY(wait([&](auto done) { f.keeper.unlock("master", done); }));
        QCOMPARE(f.keeper.search(QStringLiteral("pw1")).size(), 1u); // unlocked still works
    }

    void markingAndUnmarking()
    {
        Fixture f;
        const qint64 text = f.t.repo->ingest(QStringLiteral("s3cret")).id;
        QVERIFY(!f.keeper.markAsPassword(text)); // no master password yet
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));

        QCOMPARE(f.keeper.markAsPassword(text).value_or(0), text); // works while locked
        QVERIFY(f.t.repo->get(text)->secret);
        QVERIFY(!f.keeper.markAsPassword(text)); // already one
        QCOMPARE(f.keeper.search(QStringLiteral("s3cret")), (std::vector<qint64>{text}));

        QVERIFY(!f.keeper.unmark(text)); // needs an unlock
        QVERIFY(wait([&](auto done) { f.keeper.unlock("master", done); }));
        QCOMPARE(f.keeper.unmark(text).value_or(0), text);
        QCOMPARE(f.t.repo->get(text)->content, QStringLiteral("s3cret"));
        QVERIFY(!f.t.repo->get(text)->secret);
    }

    void changingTheMasterPasswordKeepsPasswords()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("old", done); }));
        const qint64 id = f.storePassword("kept");
        QVERIFY(!wait([&](auto done) { f.keeper.changeMasterPassword("wrong", "new", done); }));
        QVERIFY(wait([&](auto done) { f.keeper.changeMasterPassword("old", "new", done); }));
        QVERIFY(!wait([&](auto done) { f.keeper.unlock("old", done); }));
        QVERIFY(wait([&](auto done) { f.keeper.unlock("new", done); }));
        QCOMPARE(f.keeper.reveal(id).value_or(QByteArray()), QByteArrayLiteral("kept"));
        QVERIFY(f.keeper.canMatch()); // the match key is unchanged
        QCOMPARE(f.keeper.search(QStringLiteral("kept")).size(), 1u);
    }

    void resetRemovesEverything()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        const qint64 id = f.storePassword("gone");
        const qint64 text = f.t.repo->ingest(QStringLiteral("stays")).id;
        f.keeper.reset();
        QVERIFY(!f.keeper.isSetUp());
        QVERIFY(!f.t.repo->get(id));
        QVERIFY(f.t.repo->get(text));
        QVERIFY(!f.t.repo->loadVault());
        QVERIFY(!f.keys->stored); // the match key is removed too
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("fresh", done); })); // can start over
    }

    void helloWrappingUnlocksAndSurvivesAPasswordChange()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        const qint64 id = f.storePassword("via hello");
        QVERIFY(f.keeper.wrapForHello(QByteArray(32, 'h')).isEmpty()); // needs an unlock
        QVERIFY(wait([&](auto done) { f.keeper.unlock("master", done); }));
        f.keeper.setHelloWrapped(f.keeper.wrapForHello(QByteArray(32, 'h')));
        QVERIFY(f.keeper.helloEnabled());
        QVERIFY(f.keeper.isUnlocked()); // setting it up doesn't lock

        f.keeper.lock();
        QVERIFY(!f.keeper.unlockWithHelloKey(QByteArray(32, 'x')));
        QVERIFY(f.keeper.unlockWithHelloKey(QByteArray(32, 'h')));
        QCOMPARE(f.keeper.reveal(id).value_or(QByteArray()), QByteArrayLiteral("via hello"));

        f.keeper.setHelloWrapped({});
        QVERIFY(!f.keeper.helloEnabled());
        QVERIFY(!f.keeper.unlockWithHelloKey(QByteArray(32, 'h')));
    }

    void changingTheMasterPasswordDropsHello()
    {
        Fixture f;
        QVERIFY(wait([&](auto done) { f.keeper.setMasterPassword("master", done); }));
        QVERIFY(wait([&](auto done) { f.keeper.unlock("master", done); }));
        f.keeper.setHelloWrapped(f.keeper.wrapForHello(QByteArray(32, 'h')));
        QVERIFY(wait([&](auto done) { f.keeper.changeMasterPassword("master", "new", done); }));
        QVERIFY(!f.keeper.helloEnabled()); // a new key pair: Windows Hello is set up again
        QVERIFY(!f.keeper.unlockWithHelloKey(QByteArray(32, 'h')));
    }
};

QTEST_GUILESS_MAIN(TestPasswordKeeper)
#include "tst_passwordkeeper.moc"
