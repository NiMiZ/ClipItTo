#pragma once

#include "Config.h"
#include "KeyStore.h"
#include "PasteKeys.h"
#include "SelfTest.h"
#include "SessionEnv.h"

#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

// The checks of "Check this installation" (SelfTest.h). Every check is a small function from facts to a Result, so a test can
// give it a fake environment; the real probes (the database, the config file, the log file) are separate functions. The daemon
// fills an Env from its own state (Daemon::selfTestEnv()); nothing in here knows the Daemon.
namespace clipitto::selftest {

// --- Facts, as the probes find them ---------------------------------------------------------------

struct DbFacts {
    bool opened = false;
    QString error; // why it did not open
    QString quickCheck; // "ok", or the first lines PRAGMA quick_check gave (table and index names, never rows)
    int schema = -1;
    int expectedSchema = -1;
    bool writable = false;
    QString writeError;
    bool writeBusy = false; // another connection (the daemon itself) held the write lock
    qint64 freeBytes = -1; // free on the disk that holds the file; -1 unknown
    qint64 fileBytes = -1;
    bool encrypted = false;
};

struct ConfigFacts {
    bool exists = false;
    QStringList warnings; // what Config::fromToml says: key names and what was expected
};

struct LogFacts {
    bool writable = false;
    QString error;
};

struct TranslationFacts {
    bool builtIn = false;
    struct Pack {
        QString id; // "he-en"
        bool loadable = false; // the model files are there and readable
    };
    QList<Pack> packs;
};

struct OcrFacts {
    bool builtIn = false;
    bool available = false;
    QString why; // when not available: the reason, and what to install
    QStringList languages; // the ones the user asked for; empty = automatic
};

// Does the GNOME Shell extension answer FocusedApp?
enum class ExtensionAnswer { Answers, TooOld, NotAnswering };

// What the live capture test found.
enum class CaptureOutcome { Seen, SeenNotRemoved, NotSeen, CannotWrite, Paused, NoHistory, HistoryFull };

// What the live paste test found (the Settings window's test box).
enum class PasteOutcome { Received, NotReceived, CannotType, Disabled, NoFocus };

struct SyncFacts {
    bool folderSet = false;
    bool folderExists = false;
    bool folderWritable = false;
    bool passphrasePresent = false;
};

// --- The environment ------------------------------------------------------------------------------

struct Env {
    QString version; // the daemon's
    QString platform; // "Ubuntu 24.04", "Windows 11 Version 23H2"
    SessionEnv session;
    QVariantMap status; // Daemon::status(): the same data as `clipitto --status`
    Config config;
    QString configPath;
    QString databasePath;
    QString logPath;
    bool historyConverting = false; // the database belongs to the conversion worker

    // Keyrings: the stores the daemon uses (announcing ones, so the user is told why a keyring prompt appears). A real read
    // happens only where `*Needed` says the feature is on.
    std::shared_ptr<KeyStore> historyKeys;
    std::shared_ptr<KeyStore> matchKeys;
    std::shared_ptr<KeyStore> syncKeys;
    bool historyKeyNeeded = false; // the history is encrypted (or encryption is on)
    bool matchKeyNeeded = false; // a master password is set
    bool syncKeyNeeded = false; // sync is on

    // Passwords.
    bool passwordsAvailable = false; // the daemon has a PasswordKeeper
    bool passwordsSetUp = false;
    QString passwordState; // "locked", "unlocked (4 min left)", "open (no master password: not secure)", "none"
    bool canMatch = false;
    QString matchProblem;
    QString lastNameBox; // "" = none yet

    bool lockWatching = false; // the screen lock watcher is running
    QString helloName; // "Windows Hello", "Touch ID"

    // Probes. The ones marked "worker" block and are run off the GUI thread by the runner; the others run on the GUI thread.
    std::function<DbFacts()> database; // worker
    std::function<ConfigFacts()> configFile; // worker
    std::function<LogFacts()> logFile; // worker
    std::function<TranslationFacts()> translation; // worker
    std::function<OcrFacts()> ocr; // worker (starts the engine)
    std::function<bool(QString* why)> helloAvailable; // worker; empty on Linux
    std::function<SyncFacts()> syncFacts; // worker
    std::function<PasteTarget()> pasteTarget;
    std::function<void(std::function<void(ExtensionAnswer)>)> extensionProbe; // GUI; empty = no extension integration
    std::function<void(std::function<void(CaptureOutcome)>)> captureProbe; // GUI
    std::function<void(std::function<void(PasteOutcome)>)> pasteProbe; // GUI; empty outside the Settings window
};

// --- Probes (real) --------------------------------------------------------------------------------

// Opens the history file with a second connection and runs PRAGMA quick_check; `key` opens an encrypted one. Touches no row.
DbFacts probeDatabase(const QString& path, const QByteArray& key = {});
ConfigFacts probeConfig(const QString& path);
// Can the diagnostic log be written there? Creates and removes a small file next to it; never touches the log itself.
LogFacts probeLogFile(const QString& path);
SyncFacts probeSyncFolder(const QString& folder, bool passphrasePresent);

// --- Checks (facts to Result) ---------------------------------------------------------------------

Result daemonCheck(const Env& env);
Result captureBackendCheck(const Env& env);
Result captureCopyResult(CaptureOutcome outcome);
Result hotkeyCheck(const Env& env);
Result pasteResult(PasteOutcome outcome, const Env& env);
Result pasteWithoutBoxCheck(const Env& env); // the CLI: skipped (macOS: the permission is reported)
Result databaseResult(const DbFacts& facts, const Env& env);
Result configResult(const ConfigFacts& facts, const Env& env);
Result logResult(const LogFacts& facts, const Env& env);
// `store` may be null. `needed`: a real read is made. `missingIsFail`: no key in a reachable keyring is a failure.
Result keyringCheck(KeyStore* store, bool needed, bool missingIsFail, const QString& what, const QString& neededFor);
Result passwordChainCheck(const Env& env);
Result pasteTargetCheck(const Env& env);
Result gnomeExtensionResult(ExtensionAnswer answer);
Result screenLockCheck(const Env& env);
Result translationResult(const TranslationFacts& facts);
Result ocrResult(const OcrFacts& facts, const Env& env);
Result syncResult(const SyncFacts& facts, const Env& env);
Result forgetPanicCheck(const Env& env);
Result helloResult(bool available, const QString& why, const Env& env);

// All the checks, in the order of the report.
QList<Check> buildChecks(const Env& env);

} // namespace clipitto::selftest
