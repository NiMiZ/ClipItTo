#pragma once

#include "AutoForget.h"
#include "BackendSelector.h"
#include "Config.h"
#include "ControlChannel.h"
#include "DesktopIntegration.h"
#include "Entry.h"
#include "ClipboardWriter.h"
#include "KeyStore.h"
#include "PasswordKeeper.h"
#include "ScreenLockWatcher.h"
#include "SelfTestChecks.h"
#include "SessionEnv.h"
#include "Translate.h"
#include "Translation.h"

#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace clipitto {

class CapturePipeline;
class SyncService;
class LoginItem;
class ClipboardWriter;

namespace ui {
class PopupWindow;
}

namespace db {
class Db;
class Repository;
} // namespace db

// Tests: stand-ins for the language packs and the translation engine, so that the real worker plumbing (the pool, the
// guard, the answer back on the GUI thread) can run in a build without the engine.
struct FakeTranslation {
    QList<translate::Pack> packs;
    std::function<std::optional<QString>(const translate::Pack&, const QString& text, QString* error,
                                         const std::atomic<bool>& cancelled)>
        engine;
};

// Tests: a stand-in for the text recognition (Ocr.h), so that reading pictures can be tested in a build without an engine.
struct FakeOcr {
    std::function<std::optional<QString>(const QByteArray& png, const QStringList& languages, QString* error)> recognize;
};

struct DaemonOptions {
    std::shared_ptr<FakeOcr> fakeOcr; // null: the platform's text recognition
    std::shared_ptr<FakeTranslation> fakeTranslation; // null: the real packs and engine
    QString configPath;
    QString databasePath;
    SessionEnv session;
    QString wlPasteProgram = QStringLiteral("wl-paste");
    QString gsettingsProgram = QStringLiteral("gsettings"); // GNOME shortcut updates
    QString loginItemPath; // macOS launch agent; empty: ~/Library/LaunchAgents (tests set one)
    std::shared_ptr<KeyStore> keyStore; // holds the history key; null: encryption unavailable
    std::shared_ptr<KeyStore> matchKeyStore; // password copies' match key; null: no keyring
    std::shared_ptr<KeyStore> syncKeyStore; // the key made from the sync passphrase (entry "sync-key"); null: no keyring
    // Pastes with synthetic keys where there is no desktop integration (tests replace it, so
    // they never type real keys); returns false where that isn't possible.
    std::function<bool(PasteKeys)> syntheticPaste;
    // Replaces the platform's answer to "which program gets the paste" (tests; see PasteKeys.h).
    std::function<PasteTarget()> pasteTarget;
    // Side notification (title, body); false = none could be shown, and the popup says it instead.
    // Null: the desktop's (D-Bus notifications on Linux, the tray's message elsewhere). Tests replace it.
    std::function<bool(const QString&, const QString&)> notify;
    bool verbose = false; // --verbose: debug lines and the log file for the whole run, whatever the debug_log setting says
    QString debugLogPath; // the diagnostic log file; empty: paths::debugLogFile() (tests set one)
    // Screen lock / sleep events for lock_on_screen_lock (ScreenLockWatcher.h). Null: none here, reported as
    // unsupported (main.cpp passes the platform's; tests pass a FakeScreenLockWatcher).
    std::shared_ptr<ScreenLockWatcher> screenLockWatcher;
    int syncPollMs = 0; // how often the sync folder is checked; 0 = the default (a minute). Tests shorten it
    int syncDebounceMs = 3000; // after a change in the folder
    int panicQuietMs = 500; // after a panic, copies arriving this long are ignored (one on its way must not put back what was wiped)
    int autoForgetIntervalMs = 60000; // how often the auto_forget rules run (tests shorten it)
    std::function<QDateTime()> autoForgetClock; // what "now" is for the rules; null: the system clock (tests)
};

// Owns everything that lives for the whole session: config, database, capture, the popup,
// the platform control channel (D-Bus on Linux) and the optional desktop integration
// (the GNOME Shell extension on Linux).
class Daemon : public QObject {
    Q_OBJECT

public:
    using StartResult = ControlChannel::StartResult;

    // The system keyring is about to be asked for the key `purpose` ("history" or "passwords"): says so in a
    // side notification (at most every ten minutes per purpose), because the keyring may show its own
    // "unlock" window and nobody would know what wanted it. Also what `--status` reports as "keyring".
    void announceKeyring(const QString& purpose);

    Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control,
           std::unique_ptr<DesktopIntegration> integration = nullptr, QObject* parent = nullptr);
    ~Daemon() override;

    StartResult start(QString* error = nullptr);
    void reloadConfig();
    // Opens the settings dialog over the popup (the popup's ⚙ button).
    void openSettings();
    // Saves the settings to the config file and applies them. On GNOME the shortcut is
    // (re)applied even when unchanged, so the first save creates it.
    bool applySettings(const Config& config, QString* error = nullptr);
    QVariantMap status() const;
    // Adds platform details (e.g. the Windows hotkey) to status().
    void setStatusValue(const QString& key, const QVariant& value) { m_extraStatus.insert(key, value); }
    void removeStatusValue(const QString& key) { m_extraStatus.remove(key); }

    // The panic key (roadmap 20): one action, no question. Locks the password copies, empties the system clipboard
    // (unconditionally), and wipes the history exactly as "Clear everything, also in groups" does (pinned, starred and
    // grouped copies and the password copies go too; the groups, the settings and the vault stay). Nothing here logs
    // what was in the history. Triggered by `clipitto --panic` and the panic shortcut.
    struct PanicResult {
        bool locked = false; // the password copies are locked (false: no vault, or the open mode that cannot lock)
        ClipboardWriter::ClearResult clipboard = ClipboardWriter::ClearResult::Unsupported;
        bool wiped = false; // the history was deleted
        bool deferred = false; // the history file is being converted: it is wiped as soon as that is over
        qint64 deleted = 0;
    };
    PanicResult panic();

    bool isConverting() const { return m_converting; } // history being encrypted/decrypted
    bool isHistoryEncrypted() const { return m_encrypted; }
    // Whether encryption can be turned on here (build support + reachable keyring).
    bool encryptionAvailable(QString* why = nullptr) const;

    const Config& config() const { return m_config; }
    db::Repository* repository() const { return m_repo.get(); }
    ControlChannel* control() const { return m_control; }
    ui::PopupWindow* popup() const { return m_popup.get(); } // null until start() succeeded
    PasswordKeeper* passwords() const { return m_passwords.get(); } // null until start() succeeded
    SyncService* sync() const { return m_sync.get(); } // sync through a shared folder; null until start() succeeded
    // Runs the auto_forget rules now (as the timer, the start and the popup opening do); returns how many copies went.
    // Does nothing without rules, while the history is converted or on a database error.
    qint64 runAutoForget();

    // "Check this installation" (roadmap 31): a runner for all the checks, not started yet (connect to its signals, then
    // start()). Returns the one already running, if there is one, so two callers share a run. `box` is the Settings window's
    // paste test box; without it the paste check is skipped. The runner is the daemon's child and deletes itself when done.
    selftest::Runner* createSelfTest(const selftest::PasteBox* box = nullptr);
    selftest::Env selfTestEnv(const selftest::PasteBox* box);

signals:
    void configReloaded();
    void encryptionFinished(bool ok, const QString& error); // after a runtime switch
    void shortcutApplied(); // after each GNOME shortcut update, whether or not it worked
    void panicShortcutApplied(); // after each GNOME update of the panic shortcut

private:
    void startCapture();
    void probeCapture(std::function<void(selftest::CaptureOutcome)> done);
    void probePaste(const selftest::PasteBox& box, std::function<void(selftest::PasteOutcome)> done);
    PasteTarget currentPasteTarget() const; // what the next paste goes to, as far as the platform can tell
    QString passwordStateText() const; // the "passwords" line of --status, without the match-key note
    void loadConfig();
    void applyDebugLog(); // debug lines and the log file follow --verbose and the debug_log setting
    void reload(bool forceShortcut);
    void openHistory();
    QByteArray historyKey(bool create, QString* error) const;
    void setEncryption(bool on);
    void finishConversion(bool encrypted, const QString& error);
    void revertEncryptionSetting(); // after a failed runtime switch: config follows the file
    void applyDesktopShortcut();
    void applyPanicShortcut(); // GNOME: the second custom shortcut, or its removal; elsewhere only the status
    bool wipeHistory(qint64* deleted); // Clear everything's database path, plus what lives beside it
    // Password copies follow the settings and the vault: kept or not, which group, unlock time.
    void applyPasswordSettings();
    // lock_on_screen_lock: starts or stops the watcher with the setting; a lock event locks the vault.
    void applyScreenLockWatch();
    void onScreenLocked(const QString& reason);
    void setUpWindowsHello(); // availability, and the popup's Hello unlock
    void setUpTranslation(); // the popup's Translate menus
    ui::TranslationSupport translationSupport();
    void indexNextPicture(); // search inside pictures: reads the next picture that was not read yet, one at a time
    void setUpOcr(); // whether text can be read from pictures here, and the popup's menu entry
    void onTextActivated(const QString& text);
    void onRichActivated(const Entry& entry);
    void onTextEdited(qint64 id, const QString& text);
    // Types the paste keys after the popup has hidden. `done(sent)` says whether they went out (false: paste_on_enter
    // is off, or this session cannot type them).
    void pasteIfWanted(std::function<void(bool sent)> done = {});
    void pasteEntry(const Entry& entry, bool once);
    void onOnceActivated(const Entry& entry);
    void helloUnlock(std::function<void(bool)> done);
    void setWindowsHello(bool on, std::function<void(QString error)> done);
    QString cliCommand(const QString& option = QStringLiteral("--toggle")) const;
    LoginItem loginItem() const; // macOS: start at login
    void onEntryActivated(const Entry& entry);
    void onPopupVisibilityChanged(bool visible);
    bool integrationAvailable() const { return m_integration && m_integration->isAvailable(); }
    capture::Capabilities capabilities();

    DaemonOptions m_options;
    Config m_config;
    std::unique_ptr<db::Db> m_db;
    std::unique_ptr<db::Repository> m_repo;
    std::unique_ptr<PasswordKeeper> m_passwords;
    std::unique_ptr<CapturePipeline> m_pipeline;
    capture::Selection m_capture;
    std::unique_ptr<ClipboardWriter> m_writer;
    std::unique_ptr<ui::PopupWindow> m_popup; // declared last: destroyed first, it uses m_repo
    ControlChannel* m_control; // child of this
    DesktopIntegration* m_integration = nullptr; // child of this; may be null
    std::optional<bool> m_wlDataControl; // probed lazily, once
    QVariantMap m_extraStatus;
    bool m_encrypted = false;
    int m_debugLogOn = -1; // -1: not applied yet
    bool m_decodingImage = false; // an IngestImage picture is being decoded
    bool m_converting = false;
    QHash<QString, QDateTime> m_keyringAsked; // purpose -> when the keyring was last asked for it
    bool m_indexing = false; // a picture is being read for the search
    bool m_indexFailed = false; // reading failed (no language data, ...): not tried again until the settings change
    QString m_encryptionError;
    QString m_startupWarning; // shown once, the first time the popup opens
    bool m_removeKeyAfterStart = false;
    QPointer<selftest::Runner> m_selfTest; // the run in progress
    QString m_lastNameBox; // outcome and time of the last password name box, for --status
    bool m_passwordNoticeShown = false; // "set a master password", once per session
    QTimer* m_clearPassword = nullptr; // takes a pasted password off the clipboard again
    void clearPastedPassword(); // now, if it is still pending and still on the clipboard
    QTimer* m_forgetMore = nullptr; // the next step of a backlog, one at a time
    QTimer* m_forgetTimer = nullptr; // runs the auto_forget rules once a minute
    std::unique_ptr<autoforget::Pruner> m_forgetter;
    std::unique_ptr<SyncService> m_sync; // after m_repo: it is destroyed first
    std::thread m_worker; // runs a conversion; the database belongs to it meanwhile
    QThreadPool m_shortcutPool; // one thread: GNOME shortcut updates run in order, off the GUI thread
    QThreadPool m_translatePool; // one thread: translating takes a second or two
    QThreadPool m_helloPool; // one thread: Windows Hello and text-recognition calls (blocking, WinRT; never on the GUI thread)
    bool m_helloAvailable = false;
    QString m_helloReason;
    quint64 m_panicEpoch = 0; // counts panics: an answer that was started before one is dropped
    bool m_panicAfterConversion = false; // a panic came while the file was being converted
    quint64 m_panicShortcutRequest = 0;
    quint64 m_shortcutRequest = 0; // results of older requests don't overwrite the status
};

} // namespace clipitto
