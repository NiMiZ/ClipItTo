#include "PasswordVault.h"

#include <QTest>

using clipitto::PasswordVault;

// Password copies' crypto. A tiny Argon2id cost keeps the tests fast; production uses 64 MiB.
class TestVault : public QObject {
    Q_OBJECT

private:
    static constexpr PasswordVault::KdfCost kFast{64, 1};
    const QByteArray m_master = QByteArrayLiteral("correct horse battery staple");

    PasswordVault::Stored created() const { return PasswordVault::create(m_master, kFast); }

    void unlock(PasswordVault& vault, const QByteArray& password) const
    {
        QVERIFY(vault.unlockWithKey(PasswordVault::deriveKey(password, *vault.stored())));
    }

private slots:
    void createdVaultHoldsNoPlainSecret()
    {
        const PasswordVault::Stored stored = created();
        QVERIFY(stored.isValid());
        QCOMPARE(stored.kdfBlocks, 64u);
        QVERIFY(!stored.wrappedSecret.contains(m_master));
        // Two vaults for the same password share nothing.
        const PasswordVault::Stored other = created();
        QVERIFY(stored.salt != other.salt);
        QVERIFY(stored.publicKey != other.publicKey);
    }

    void sealsWhileLockedAndOpensOnlyAfterUnlock()
    {
        PasswordVault vault;
        QVERIFY(vault.seal("x").isEmpty()); // not set up: nothing to seal to
        vault.load(created());
        QVERIFY(vault.isSetUp());
        QVERIFY(!vault.isUnlocked());

        const QByteArray sealed = vault.seal("hunter2!");
        QVERIFY(!sealed.isEmpty());
        QVERIFY(!sealed.contains("hunter2"));
        QVERIFY(sealed != vault.seal("hunter2!")); // fresh ephemeral key and nonce each time
        QVERIFY(!vault.open(sealed)); // locked

        unlock(vault, m_master);
        QCOMPARE(vault.open(sealed).value_or(QByteArray()), QByteArrayLiteral("hunter2!"));
        QCOMPARE(vault.open(vault.seal(QByteArray())).value_or("x"), QByteArray()); // empty is fine

        vault.lock();
        QVERIFY(!vault.isUnlocked());
        QVERIFY(!vault.open(sealed));
    }

    void wrongPasswordDoesNotUnlock()
    {
        PasswordVault vault;
        vault.load(created());
        QVERIFY(!vault.unlockWithKey(PasswordVault::deriveKey("Correct horse battery staple", *vault.stored())));
        QVERIFY(!vault.unlockWithKey(QByteArray(32, 'k')));
        QVERIFY(!vault.unlockWithKey(QByteArray()));
        QVERIFY(!vault.isUnlocked());
    }

    void damagedSealIsRejected()
    {
        PasswordVault vault;
        vault.load(created());
        unlock(vault, m_master);
        QByteArray sealed = vault.seal("secret");
        sealed[sealed.size() - 1] = char(sealed.at(sealed.size() - 1) ^ 1);
        QVERIFY(!vault.open(sealed));
        QVERIFY(!vault.open(QByteArray()));
        QVERIFY(!vault.open(QByteArray(10, '\1')));

        // A seal for another vault doesn't open here.
        PasswordVault other;
        other.load(created());
        QVERIFY(!vault.open(other.seal("secret")));
    }

    void changingThePasswordKeepsThePasswords()
    {
        PasswordVault vault;
        vault.load(created());
        const QByteArray sealed = vault.seal("kept");

        QVERIFY(!PasswordVault::changePassword(*vault.stored(), "wrong", "new", kFast));
        const auto changed = PasswordVault::changePassword(*vault.stored(), m_master, "new master", kFast);
        QVERIFY(changed);
        QCOMPARE(changed->publicKey, vault.stored()->publicKey); // same key pair, new wrapping
        QVERIFY(changed->salt != vault.stored()->salt);

        vault.load(*changed);
        QVERIFY(!vault.unlockWithKey(PasswordVault::deriveKey(m_master, *vault.stored())));
        unlock(vault, "new master");
        QCOMPARE(vault.open(sealed).value_or(QByteArray()), QByteArrayLiteral("kept"));
    }

    void aSecondWrappingUnlocksToo()
    {
        // As Windows Hello uses it: the secret key wrapped under another key.
        PasswordVault vault;
        vault.load(created());
        QVERIFY(vault.wrapSecret(QByteArray(32, 'h')).isEmpty()); // locked
        unlock(vault, m_master);
        PasswordVault::Stored stored = *vault.stored();
        stored.helloWrapped = vault.wrapSecret(QByteArray(32, 'h'));
        const QByteArray sealed = vault.seal("via hello");

        PasswordVault again;
        again.load(stored);
        QVERIFY(!again.unlockWithKey(QByteArray(32, 'x'), /*hello=*/true));
        QVERIFY(again.unlockWithKey(QByteArray(32, 'h'), /*hello=*/true));
        QCOMPARE(again.open(sealed).value_or(QByteArray()), QByteArrayLiteral("via hello"));
    }

    void matchNeedsTheKeyAndIsStable()
    {
        PasswordVault vault;
        QVERIFY(!vault.canMatch());
        QVERIFY(vault.match("pw").isEmpty());
        vault.setMatchKey(QByteArray(32, 'm'));
        QVERIFY(vault.canMatch());
        const QString m = vault.match("pw");
        QVERIFY(m.startsWith(QLatin1String("secret:")));
        QCOMPARE(m.size(), 7 + 64);
        QCOMPARE(vault.match("pw"), m);
        QVERIFY(vault.match("pw ") != m);

        PasswordVault other;
        other.setMatchKey(QByteArray(32, 'n'));
        QVERIFY(other.match("pw") != m); // keyed: useless without the keyring's key
        other.setMatchKey(QByteArray(5, 'n')); // wrong size: no matching
        QVERIFY(!other.canMatch());
    }

    void wipeClears()
    {
        QByteArray a("secret");
        PasswordVault::wipe(a);
        QVERIFY(a.isEmpty());
        QString s = QStringLiteral("secret");
        PasswordVault::wipe(s);
        QVERIFY(s.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestVault)
#include "tst_vault.moc"
