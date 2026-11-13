#pragma once

#include "Config.h"
#include "KeyStore.h"
#include "Sync.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace clipitto {

namespace db {
class Repository;
}

// Runs the sync through a folder (Sync.h, docs/sync-design.md): on a timer, when the folder changes (debounced), on
// demand. Reading and decrypting the other devices' files and writing the own file happen on worker threads; merging
// into the database and collecting the own copies happen on the GUI thread, a small batch per turn of the event loop.
// ClipItTo makes no network connection: the folder is an ordinary directory.
//
// The key made from the sync passphrase is kept in the system keyring (entry "sync-key"); the passphrase itself is
// wiped as soon as the key is derived. Nothing here logs or shows a copy's text: the folder path and counts only.
class SyncService : public QObject {
    Q_OBJECT

public:
    SyncService(db::Repository& repo, std::shared_ptr<KeyStore> keys, QObject* parent = nullptr);
    ~SyncService() override;

    // True while the database belongs to someone else (the history is being converted): a run waits.
    void setBlockedCheck(std::function<bool()> blocked) { m_blocked = std::move(blocked); }
    // Tests: a tiny Argon2id cost and short timers.
    void setKdfCost(VaultKdfCost cost) { m_cost = cost; }
    void setIntervals(int pollMs, int debounceMs);

    // Follows the settings: starts or stops the timers and the folder watch, and tells the database whether to track
    // deletions. A change runs a sync soon.
    void applyConfig(const Config& config, bool runSoon = true);

    // Whether a keyring can keep the key (asked once).
    bool available(QString* why = nullptr);
    bool hasPassphrase();
    // Joins the group of the files in `folder` (they must open with `passphrase`) or, with `startNew`, makes a new group.
    // The Argon2id work is on a worker. `done(error, mismatch)`: empty error = worked; `mismatch` = files exist that this
    // passphrase does not open. `passphrase` is wiped.
    void setPassphrase(const QString& folder, QByteArray passphrase, bool startNew,
                       std::function<void(QString error, bool mismatch)> done);
    void removePassphrase();

    // One run now (the Sync now button, `clipitto --sync`); forgets any back-off.
    void syncNow();
    bool isRunning() const { return m_running; }

    QString statusText() const; // for Settings: several lines
    QString statusLine() const; // for `clipitto --status`

signals:
    void historyChanged(); // a merge added, changed or deleted copies
    void finished(); // a run ended, whatever the outcome

private:
    struct ReadJob {
        sync::FolderState state = sync::FolderState::Ok;
        QString error; // the worker failed
        bool ownFileExists = false;
        std::vector<sync::RemoteFile> files;
    };
    struct WriteJob {
        bool ok = false;
        QString error;
        qint64 bytes = 0;
    };
    struct Device {
        QString id;
        QString name;
        QDateTime modified;
        int copies = 0;
        sync::ReadStatus status = sync::ReadStatus::Ok;
    };

    void run(bool manual);
    void scheduleRun(int delayMs);
    void onRead();
    void stepMerge();
    void startCollect();
    void stepCollect();
    void onWritten();
    void fail(const QString& message);
    void finishRun();
    bool loadKey();
    bool blocked() const { return m_blocked && m_blocked(); }
    sync::Options options() const;
    void watchFolder();
    void updatePoll();

    db::Repository& m_repo;
    std::shared_ptr<KeyStore> m_keys;
    std::function<bool()> m_blocked;
    VaultKdfCost m_cost = sync::defaultCost();
    int m_pollMs = 60000;
    int m_debounceMs = 3000;

    Config m_config;
    bool m_configured = false;
    sync::Key m_key;
    bool m_keyLoaded = false;
    QString m_keyProblem;
    int m_availableState = -1; // -1: not asked
    QString m_availableWhy;
    QString m_deviceId;

    QTimer m_poll;
    QTimer m_debounce;
    QFileSystemWatcher m_watcher;
    QString m_watched;

    quint64 m_generation = 0; // bumped when the key or the folder changes: an answer from before is dropped
    quint64 m_readGeneration = 0;
    bool m_running = false;
    bool m_again = false;
    int m_backoffMs = 0; // 0: not failing
    QFutureWatcher<ReadJob> m_reading;
    QFutureWatcher<WriteJob> m_writing;
    std::unique_ptr<sync::Merger> m_merger;
    std::unique_ptr<sync::Collector> m_collector;
    bool m_ownFileExists = false;
    QHash<QString, sync::Known> m_known; // files that opened, by name: not read again while unchanged
    QHash<QString, Device> m_devices;
    QString m_pendingFingerprint;

    // What the status shows.
    QDateTime m_lastRun; // the last run that reached the folder
    QString m_lastProblem; // why the last run failed; empty when it did not
    sync::MergeStats m_lastMerge;
    int m_lastWrittenCopies = 0;
    QDateTime m_lastWrite;
    int m_ignoredOtherKey = 0;
    int m_ignoredDamaged = 0;
    int m_ignoredNewer = 0;
};

} // namespace clipitto
