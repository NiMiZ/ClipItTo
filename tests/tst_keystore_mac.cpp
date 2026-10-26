#include "SystemKeyStore.h"

#include <QTest>

#include <unistd.h>

using clipitto::createSystemKeyStore;

// The macOS Keychain, for real (this suite is built on macOS only). A runner or a Mac whose
// login keychain is locked or missing can't store anything: the test then skips.
class TestKeyStoreMac : public QObject {
    Q_OBJECT

private slots:
    void storesReadsReplacesAndRemovesAKey()
    {
        const QString entry = QStringLiteral("clipitto-test-%1").arg(getpid());
        auto store = createSystemKeyStore(entry);
        QVERIFY(store->available());

        const QByteArray first = QByteArray::fromHex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
        const QByteArray second(32, '\x7f');
        QString error;
        std::optional<QByteArray> found = store->read(&error);
        QVERIFY2(!found && error.isEmpty(), "a key is already there"); // not found is not an error

        if (!store->write(first, &error)) {
            QSKIP(qPrintable(QStringLiteral("the Keychain can't be written here: %1").arg(error)));
        }
        found = store->read(&error);
        QVERIFY2(found.has_value(), qPrintable(error));
        QCOMPARE(*found, first);

        QVERIFY2(store->write(second, &error), qPrintable(error)); // replaces
        QCOMPARE(store->read(&error).value_or(QByteArray()), second);

        // Another entry is another item.
        auto other = createSystemKeyStore(entry + QStringLiteral("-other"));
        QVERIFY(!other->read(&error).has_value());
        QVERIFY(error.isEmpty());

        QVERIFY2(store->remove(&error), qPrintable(error));
        QVERIFY(!store->read(&error).has_value());
        QVERIFY(error.isEmpty());
        QVERIFY2(store->remove(&error), qPrintable(error)); // already gone: fine
    }
};

QTEST_GUILESS_MAIN(TestKeyStoreMac)
#include "tst_keystore_mac.moc"
