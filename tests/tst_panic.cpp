// The panic key (roadmap 20): one action that locks the passwords, empties the system clipboard and wipes the history, with
// no question. A real Daemon (offscreen) with a fake keyring; the panic shortcut's registration against a fake hotkey and
// a fake gsettings. Texts are synthetic and never logged. Not shown here (docs/untested.md): the real global shortcuts
// (Windows, macOS, GNOME), the real system clipboards, `clipitto --panic` against a running desktop.
#include "ControlChannel.h"
#include "Daemon.h"
#include "FakeKeyStore.h"
#include "HistoryModel.h"
#include "PanicHotkey.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::Config;
using clipitto::Daemon;
using clipitto::DaemonOptions;
using clipitto::ClipboardWriter;

namespace {

class FakeControl : public clipitto::ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

// A platform hotkey stand-in (WindowsHotkey / MacHotkey have this interface).
struct FakeHotkey {
    QString registered;
    QString failWith; // non-empty: another program owns every shortcut
    int clears = 0;
    bool setShortcut(const QString& shortcut, QString* error)
    {
        if (!failWith.isEmpty()) {
            if (error) {
                *error = failWith;
            }
            return false;
        }
        registered = shortcut;
        return true;
    }
    void clear()
    {
        registered.clear();
        ++clears;
    }
    bool isRegistered() const { return !registered.isEmpty(); }
    QString shortcut() const { return registered; }
};

QByteArray pngBytes()
{
    QImage picture(24, 24, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    picture.save(&buffer, "PNG");
    return png;
}

} // namespace

class TestPanic : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_notifications = 0;

    DaemonOptions options(int quietMs = 0) const
    {
        DaemonOptions o;
        o.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = m_dir.filePath(QStringLiteral("data/history.db"));
        o.syntheticPaste = [](clipitto::PasteKeys) { return false; }; // never types real keys
        o.notify = [this](const QString&, const QString&) {
            const_cast<TestPanic*>(this)->m_notifications++;
            return true;
        };
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        o.panicQuietMs = quietMs;
        o.gsettingsProgram = QStringLiteral("/nonexistent/gsettings");
        return o;
    }

    void writeConfig(const QByteArray& text) const
    {
        QFile file(options().configPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text);
    }

    bool historyFileHolds(const QByteArray& needle) const
    {
        for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            QFile file(options().databasePath + suffix);
            if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray bytes = file.readAll();
            file.close(); // closed at once (Windows: a file open here cannot be deleted)
            if (bytes.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    static bool setUpVault(Daemon& d, bool open)
    {
        d.passwords()->setKdfCost({64, 1});
        std::optional<bool> ok;
        const auto done = [&ok](bool result) { ok = result; };
        if (open) {
            d.passwords()->useWithoutMasterPassword(done);
        } else {
            d.passwords()->setMasterPassword("master", done);
        }
        return QTest::qWaitFor([&ok] { return ok.has_value(); }, 10000) && *ok;
    }

    static bool unlock(Daemon& d)
    {
        std::optional<bool> ok;
        d.passwords()->unlock("master", [&ok](bool result) { ok = result; });
        return QTest::qWaitFor([&ok] { return ok.has_value(); }, 10000) && *ok;
    }

    static qint64 ingest(Daemon& d, const QString& text, const QString& html = QString())
    {
        emit d.control()->ingestRequested(text, {QStringLiteral("text/plain")}, html);
        return d.repository()->findByHash(d.repository()->textHash(text)).value_or(0);
    }

    static QStringList marked()
    {
        return {QStringLiteral("text/plain"), QStringLiteral("x-kde-passwordManagerHint"),
                QStringLiteral("x-clipitto-source:KeePassXC")};
    }

    // A history with everything panic has to delete: plain, pinned, starred, grouped, a password copy, formatted text,
    // a picture, learned auto-grouping data.
    struct Filled {
        qint64 plain = 0, pinned = 0, starred = 0, grouped = 0, rich = 0, picture = 0;
    };
    Filled fill(Daemon& d)
    {
        Filled f;
        auto* repo = d.repository();
        repo->setAutoGrouping(true);
        f.plain = ingest(d, QStringLiteral("panic-plain-text-0001"));
        f.pinned = ingest(d, QStringLiteral("panic-pinned-text-0002"));
        repo->setPinned(f.pinned, true);
        f.starred = ingest(d, QStringLiteral("panic-starred-text-0003"));
        repo->setFavorite(f.starred, true);
        f.grouped = ingest(d, QStringLiteral("panic-grouped-text-0004"));
        repo->createGroup(QStringLiteral("Work"));
        repo->setGroup(f.grouped, QStringLiteral("Work")); // learns, as the user's moves do
        f.rich = ingest(d, QStringLiteral("panic-rich-text-0005"), QStringLiteral("<p><b>panic-rich-text-0005</b></p>"));
        emit d.control()->ingestImageRequested(pngBytes(), QStringLiteral("image/png"), {QStringLiteral("image/png")});
        for (int i = 0; i < 500 && repo->countImages() == 0; ++i) {
            QTest::qWait(10);
        }
        return f;
    }

private slots:
    void init() { QGuiApplication::clipboard()->clear(); }

    void cleanup()
    {
        // A fresh data folder for every test.
        QDir(m_dir.filePath(QStringLiteral("data"))).removeRecursively();
        QFile::remove(m_dir.filePath(QStringLiteral("config.toml")));
    }

    // ================= one call: wipe + lock + clipboard =================

    void oneCallWipesLocksAndClearsTheClipboard()
    {
        writeConfig("password_ask_note = false\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, /*open=*/false));
        QVERIFY(unlock(d));
        const Filled f = fill(d);
        emit d.control()->ingestRequested(QStringLiteral("panic-password-0006"), marked());
        auto* repo = d.repository();
        QCOMPARE(repo->countSecrets(), qint64(1));
        QVERIFY(repo->countImages() == 1);
        QVERIFY(!repo->richText(f.rich).isEmpty());
        QVERIFY(!repo->autoExamples().empty());
        QGuiApplication::clipboard()->setText(QStringLiteral("panic-on-the-clipboard-0007"));
        QVERIFY(d.passwords()->isUnlocked());

        const Daemon::PanicResult r = d.panic();

        QVERIFY(r.wiped);
        QVERIFY(r.locked);
        QVERIFY(r.clipboard == ClipboardWriter::ClearResult::Cleared);
        QVERIFY(r.deleted >= 7);
        // The history: everything, pinned, starred, grouped, passwords, pictures, formatted text, learned data.
        QCOMPARE(repo->count(), qint64(0));
        QCOMPARE(repo->countSecrets(), qint64(0));
        QCOMPARE(repo->countImages(), qint64(0));
        QVERIFY(!repo->get(f.pinned));
        QVERIFY(!repo->get(f.starred));
        QVERIFY(!repo->get(f.grouped));
        QVERIFY(repo->richText(f.rich).isEmpty());
        QVERIFY(repo->autoExamples().empty());
        QVERIFY(repo->search(QStringLiteral("panic")).empty());
        // The passwords: locked, and the vault itself is still there.
        QVERIFY(!d.passwords()->isUnlocked());
        QVERIFY(d.passwords()->isSetUp());
        QCOMPARE(d.status().value(QStringLiteral("passwords")).toString(), QStringLiteral("locked"));
        // The system clipboard.
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
        // What survives: the groups (empty) and the settings.
        QVERIFY(repo->groups().contains(QStringLiteral("Work"))); // and the Passwords group the password copy made
        QVERIFY(repo->groups().contains(QStringLiteral("Passwords")));
        QVERIFY(QFile::exists(options().configPath));
        // Nothing of it is left in the file or its write-ahead log.
        for (const char* needle : {"panic-plain-text-0001", "panic-pinned-text-0002", "panic-starred-text-0003",
                                   "panic-grouped-text-0004", "panic-rich-text-0005",
                                   "panic-on-the-clipboard-0007"}) {
            QVERIFY2(!historyFileHolds(needle), needle);
        }
        // The master password still opens the vault after it.
        QVERIFY(unlock(d));
        QVERIFY(d.passwords()->isUnlocked());
        // The history keeps working.
        QVERIFY(ingest(d, QStringLiteral("after the panic")) != 0);
    }

    void theOpenModeCannotLockButItsPasswordCopiesAreWiped()
    {
        writeConfig("password_ask_note = false\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, /*open=*/true));
        emit d.control()->ingestRequested(QStringLiteral("panic-open-mode-pw"), marked());
        QCOMPARE(d.repository()->countSecrets(), qint64(1));
        const Daemon::PanicResult r = d.panic();
        QVERIFY(r.wiped);
        QVERIFY(!r.locked); // nothing to lock in this mode (it is not secure; the notice in Settings says so)
        QCOMPARE(d.repository()->countSecrets(), qint64(0));
        QVERIFY(!historyFileHolds("panic-open-mode-pw"));
    }

    void withoutAVaultItStillWipesAndClears()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        ingest(d, QStringLiteral("panic-no-vault"));
        QGuiApplication::clipboard()->setText(QStringLiteral("panic-clip-no-vault"));
        const Daemon::PanicResult r = d.panic();
        QVERIFY(r.wiped);
        QVERIFY(!r.locked);
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
        QCOMPARE(d.repository()->count(), qint64(0));
    }

    void thePictureOnTheClipboardIsClearedToo()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        auto* data = new QMimeData;
        data->setImageData(QImage::fromData(pngBytes(), "PNG"));
        data->setData(QStringLiteral("image/png"), pngBytes());
        QGuiApplication::clipboard()->setMimeData(data);
        QVERIFY(d.panic().clipboard == ClipboardWriter::ClearResult::Cleared);
        const QMimeData* now = QGuiApplication::clipboard()->mimeData();
        QVERIFY(!now || now->formats().isEmpty());
    }

    void theControlChannelRunsItWhateverTheSettingSays()
    {
        // `clipitto --panic`: deliberate typing, so the setting (which only gates the shortcut) is not asked.
        writeConfig("panic_key = false\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(!d.config().panicKey);
        ingest(d, QStringLiteral("panic-by-cli"));
        QGuiApplication::clipboard()->setText(QStringLiteral("panic-clip-by-cli"));
        emit d.control()->panicRequested();
        QCOMPARE(d.repository()->count(), qint64(0));
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
    }

    void aPanicOnAnEmptyHistoryIsFine()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const Daemon::PanicResult r = d.panic();
        QVERIFY(r.wiped);
        QCOMPARE(r.deleted, qint64(0));
        QVERIFY(d.panic().wiped); // twice in a row
    }

    // ================= what the user sees afterwards =================

    void theOpenPopupHidesAndShowsAnEmptyHistory()
    {
        writeConfig("hide_on_focus_loss = false\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const qint64 id = ingest(d, QStringLiteral("panic-in-the-popup"));
        d.popup()->showPopup();
        QVERIFY(d.popup()->isVisible());
        QVERIFY(d.popup()->model()->findEntry(id).isValid());
        d.popup()->searchBar()->setText(QStringLiteral("panic-in"));
        QStringList boxes;
        d.popup()->setNotify([&boxes](QWidget*, const QString& m) { boxes << m; });
        d.panic();
        QVERIFY(!d.popup()->isVisible());
        QVERIFY(d.popup()->searchBar()->text().isEmpty());
        QVERIFY(!d.popup()->model()->findEntry(id).isValid());
        QVERIFY(boxes.isEmpty()); // no message, no content anywhere
        QCOMPARE(m_notifications, 0);
        d.popup()->showPopup();
        QVERIFY(!d.popup()->model()->findEntry(id).isValid());
    }

    void statusShowsNothingSpecialAfterwards()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        ingest(d, QStringLiteral("panic-status"));
        QVariantMap before = d.status();
        d.panic();
        QVariantMap after = d.status();
        for (const char* volatileKey : {"visible"}) {
            before.remove(QString::fromLatin1(volatileKey));
            after.remove(QString::fromLatin1(volatileKey));
        }
        QCOMPARE(after.keys(), before.keys());
        QVERIFY(!after.contains(QStringLiteral("panic_hotkey"))); // the panic key is off: not even mentioned
    }

    void aPendingPasteOnceUndoBufferIsGone()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.popup()->setOnceAskDelayMs(0);
        const qint64 id = ingest(d, QStringLiteral("panic-paste-once-copy"));
        d.repository()->setPasteOnce(id, true);
        d.popup()->consumePasteOnce(id);
        QVERIFY(d.popup()->undoPending());
        d.panic();
        QVERIFY(!d.popup()->undoPending());
        QVERIFY(!d.popup()->undoPasteOnce());
        QCOMPARE(d.repository()->count(), qint64(0));
    }

    void aWaitingClipboardClearDoesNothingToLaterCopies()
    {
        writeConfig("paste_once_clear_seconds = 1\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.popup()->setOnceAskDelayMs(0);
        const qint64 id = ingest(d, QStringLiteral("panic-once-with-timer"));
        d.repository()->setPasteOnce(id, true);
        emit d.popup()->entryActivated(*d.repository()->get(id));
        d.panic();
        QGuiApplication::clipboard()->setText(QStringLiteral("copied after the panic"));
        QTest::qWait(2500); // the one-second clear (cancelled) would have fired by now
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copied after the panic"));
    }

    // ================= nothing comes back =================

    void aCopyOnItsWayIsIgnoredDuringTheQuietPeriod()
    {
        Daemon d(options(/*quietMs=*/60000), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        ingest(d, QStringLiteral("panic-before"));
        d.panic();
        emit d.control()->ingestRequested(QStringLiteral("panic-queued-call"), {QStringLiteral("text/plain")});
        QCOMPARE(d.repository()->count(), qint64(0));
        emit d.control()->ingestImageRequested(pngBytes(), QStringLiteral("image/png"), {QStringLiteral("image/png")});
        QTest::qWait(300);
        QCOMPARE(d.repository()->count(), qint64(0));
    }

    void withNoQuietPeriodTheNextCopyIsKept()
    {
        Daemon d(options(/*quietMs=*/0), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.panic();
        QVERIFY(ingest(d, QStringLiteral("a copy after the panic")) != 0);
    }

    void aPictureBeingDecodedWhenThePanicComesIsDropped()
    {
        Daemon d(options(/*quietMs=*/0), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestImageRequested(pngBytes(), QStringLiteral("image/png"), {QStringLiteral("image/png")});
        d.panic(); // the decode is still in flight: its answer arrives after this
        QTest::qWait(500);
        QCOMPARE(d.repository()->count(), qint64(0));
    }

    void aPanicDuringTheConversionWipesWhenItIsOver()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        DaemonOptions o = options();
        o.keyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        ingest(d, QStringLiteral("panic-converting-0009"));
        QSignalSpy finished(&d, &Daemon::encryptionFinished);
        Config c = d.config();
        c.encryptHistory = true;
        QVERIFY(d.applySettings(c));
        QVERIFY(d.isConverting()); // the conversion's answer needs this thread's event loop: still going
        const Daemon::PanicResult r = d.panic();
        QVERIFY(r.deferred);
        QVERIFY(!r.wiped);
        QVERIFY(r.clipboard == ClipboardWriter::ClearResult::Cleared); // the clipboard does not wait
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(d.repository()->count(), qint64(0));
        QVERIFY(!d.popup()->isBusy());
        // And what is copied now is kept.
        QVERIFY(ingest(d, QStringLiteral("after the converted panic")) != 0);
    }

    // ================= the shortcut =================

    void theShortcutIsNotRegisteredWhileTheSettingIsOff()
    {
        FakeHotkey hotkey;
        Config c;
        c.panicHotkey = QStringLiteral("Ctrl+Alt+Shift+Delete"); // set, but the key is off
        QVERIFY(clipitto::applyPanicHotkey(hotkey, c).isEmpty());
        QVERIFY(!hotkey.isRegistered());
        QVERIFY(hotkey.clears >= 1);
    }

    void theShortcutIsRegisteredWhileTheSettingIsOn()
    {
        FakeHotkey hotkey;
        Config c;
        c.panicKey = true;
        c.panicHotkey = QStringLiteral("ctrl+alt+shift+delete");
        QCOMPARE(clipitto::applyPanicHotkey(hotkey, c), QStringLiteral("Ctrl+Alt+Shift+Delete"));
        QVERIFY(hotkey.isRegistered());
        QCOMPARE(hotkey.registered, QStringLiteral("Ctrl+Alt+Shift+Delete"));
        // Turning it off takes it away again.
        c.panicKey = false;
        QVERIFY(clipitto::applyPanicHotkey(hotkey, c).isEmpty());
        QVERIFY(!hotkey.isRegistered());
    }

    void onWithoutAShortcutRegistersNothingAndSaysSo()
    {
        FakeHotkey hotkey;
        Config c;
        c.panicKey = true;
        const QString status = clipitto::applyPanicHotkey(hotkey, c);
        QVERIFY(status.startsWith(QLatin1String("unavailable")));
        QVERIFY(!hotkey.isRegistered());
    }

    void theSameShortcutAsThePopupsIsAConflict()
    {
        FakeHotkey hotkey;
        Config c;
        c.panicKey = true;
        c.hotkey = QStringLiteral("Ctrl+Alt+V");
        c.panicHotkey = QStringLiteral("ctrl+alt+v");
        const QString status = clipitto::applyPanicHotkey(hotkey, c);
        QVERIFY(status.startsWith(QLatin1String("unavailable")));
        QVERIFY(status.contains(QLatin1String("popup")));
        QVERIFY(!hotkey.isRegistered());
    }

    void aShortcutAnotherProgramOwnsIsReportedLikeTheMainOne()
    {
        FakeHotkey hotkey;
        hotkey.failWith = QStringLiteral("Ctrl+Alt+Shift+Delete is already used by another application");
        Config c;
        c.panicKey = true;
        c.panicHotkey = QStringLiteral("Ctrl+Alt+Shift+Delete");
        QCOMPARE(clipitto::applyPanicHotkey(hotkey, c), QStringLiteral("unavailable: ") + hotkey.failWith);
        QVERIFY(!hotkey.isRegistered());
    }

    void anInvalidShortcutIsReported()
    {
        FakeHotkey hotkey;
        Config c;
        c.panicKey = true;
        c.panicHotkey = QStringLiteral("Delete"); // no modifier: not a shortcut
        QVERIFY(clipitto::applyPanicHotkey(hotkey, c).startsWith(QLatin1String("unavailable")));
        QVERIFY(!hotkey.isRegistered());
    }

    // GNOME: a second custom shortcut running `clipitto --panic`, removed again when the key is turned off.
    QString gnomeValue(const QString& storeDir, const QString& key) const
    {
        QFile f(QDir(storeDir).filePath(QString(key).replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char(':'), QLatin1Char('_'))));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
    }

    void gnomeGetsASecondShortcutOnlyWhileTheKeyIsOn()
    {
#ifndef CLIPITTO_FAKE_GSETTINGS
        QSKIP("needs the POSIX fake gsettings");
#else
        QTemporaryDir store;
        qputenv("FAKE_GSETTINGS_DIR", QFile::encodeName(store.path()));
        DaemonOptions o = options();
        o.session.isGnome = true;
        o.gsettingsProgram = QStringLiteral(CLIPITTO_FAKE_GSETTINGS);
        writeConfig("hotkey = \"Ctrl+`\"\n");
        const QString base = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:"
                                            "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/");
        const QString list = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybindings");

        Daemon d(o, std::make_unique<FakeControl>());
        QSignalSpy applied(&d, &Daemon::panicShortcutApplied);
        QSignalSpy mainApplied(&d, &Daemon::shortcutApplied);
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QTRY_COMPARE(applied.count(), 1);
        QTRY_COMPARE(mainApplied.count(), 1);
        // Off: nothing is bound and nothing is mentioned.
        QVERIFY(!gnomeValue(store.path(), list).contains(QLatin1String("clipitto-panic")));
        QVERIFY(!d.status().contains(QStringLiteral("panic_hotkey")));
        QCOMPARE(gnomeValue(store.path(), base + QStringLiteral("clipitto/.binding")), QStringLiteral("<Control>grave"));

        // On: the second shortcut, running --panic.
        Config c = d.config();
        c.panicKey = true;
        c.panicHotkey = QStringLiteral("Ctrl+Alt+Shift+F12");
        QVERIFY(d.applySettings(c));
        QTRY_VERIFY(applied.count() >= 2);
        QTRY_VERIFY(d.status().value(QStringLiteral("panic_hotkey")).toString().contains(QLatin1String("(GNOME)")));
        QVERIFY(gnomeValue(store.path(), list).contains(QLatin1String("clipitto-panic")));
        QVERIFY(gnomeValue(store.path(), list).contains(QLatin1String("/clipitto/"))); // the main one is still listed
        const QString panicEntry = base + QStringLiteral("clipitto-panic/");
        QCOMPARE(gnomeValue(store.path(), panicEntry + QStringLiteral(".binding")), QStringLiteral("<Control><Alt><Shift>F12"));
        QVERIFY(gnomeValue(store.path(), panicEntry + QStringLiteral(".command")).endsWith(QLatin1String("--panic")));
        QCOMPARE(gnomeValue(store.path(), base + QStringLiteral("clipitto/.binding")), QStringLiteral("<Control>grave"));

        // The same shortcut as the popup's: reported, and nothing stays bound.
        c.panicHotkey = QStringLiteral("Ctrl+`");
        const int seen = applied.count();
        QVERIFY(d.applySettings(c));
        QTRY_VERIFY(applied.count() > seen);
        QTRY_VERIFY(d.status().value(QStringLiteral("panic_hotkey")).toString().startsWith(QLatin1String("unavailable")));
        QVERIFY(!gnomeValue(store.path(), list).contains(QLatin1String("clipitto-panic")));

        // Back on, then off again: the binding goes.
        c.panicHotkey = QStringLiteral("Ctrl+Alt+Shift+F12");
        QVERIFY(d.applySettings(c));
        QTRY_VERIFY(gnomeValue(store.path(), list).contains(QLatin1String("clipitto-panic")));
        c.panicKey = false;
        QVERIFY(d.applySettings(c));
        QTRY_VERIFY(!gnomeValue(store.path(), list).contains(QLatin1String("clipitto-panic")));
        QTRY_VERIFY(!d.status().contains(QStringLiteral("panic_hotkey")));
        QVERIFY(gnomeValue(store.path(), list).contains(QLatin1String("/clipitto/")));
#endif
    }

    void otherDesktopsAreToldToBindTheCommand()
    {
        writeConfig("panic_key = true\npanic_hotkey = \"Ctrl+Alt+Shift+F12\"\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(d.status().value(QStringLiteral("panic_hotkey")).toString().contains(QLatin1String("clipitto --panic")));
    }

    void bothConfigKeysSurviveASaveAndLoad()
    {
        Config c;
        c.panicKey = true;
        c.panicHotkey = QStringLiteral("Ctrl+Alt+Shift+F12");
        const Config back = Config::fromToml(c.toToml());
        QVERIFY(back.panicKey);
        QCOMPARE(back.panicHotkey, QStringLiteral("Ctrl+Alt+Shift+F12"));
    }
};

QTEST_MAIN(TestPanic)
#include "tst_panic.moc"
