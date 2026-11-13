#include "SyncService.h"

#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSysInfo>
#include <QtConcurrent>

#include <algorithm>

namespace clipitto {

Q_LOGGING_CATEGORY(lcSync, "clipitto.sync")

namespace {

constexpr int kMaxBackoffMs = 15 * 60 * 1000;
constexpr int kFirstBackoffMs = 60 * 1000;

QString stampOf(const QDateTime& time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QString clock(const QDateTime& time)
{
    return time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

SyncService::SyncService(db::Repository& repo, std::shared_ptr<KeyStore> keys, QObject* parent)
    : QObject(parent), m_repo(repo), m_keys(std::move(keys))
{
    m_poll.setInterval(m_pollMs);
    connect(&m_poll, &QTimer::timeout, this, [this] { run(false); });
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(m_debounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { run(false); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        m_debounce.start(); // a burst of changes (a sync client writing) is one run
    });
    connect(&m_reading, &QFutureWatcher<ReadJob>::finished, this, &SyncService::onRead);
    connect(&m_writing, &QFutureWatcher<WriteJob>::finished, this, &SyncService::onWritten);
}

SyncService::~SyncService()
{
    // A worker may still be reading or writing: let it end (it touches no database) before the members go.
    m_reading.waitForFinished();
    m_writing.waitForFinished();
    m_key.wipe();
}

void SyncService::setIntervals(int pollMs, int debounceMs)
{
    m_pollMs = pollMs;
    m_debounceMs = debounceMs;
    m_debounce.setInterval(debounceMs);
    updatePoll();
}

sync::Options SyncService::options() const
{
    sync::Options o;
    o.maxCopies = m_config.syncMaxCopies;
    o.pictures = m_config.syncPictures;
    o.maxPictureBytes = qint64(m_config.syncPictureMaxKb) * 1024;
    o.tombstoneDays = m_config.syncTombstoneDays;
    o.maxItemBytes = m_config.maxItemBytes;
    o.maxHistory = m_config.maxHistory;
    o.maxImages = m_config.maxImages;
    o.deviceName = m_config.syncDeviceName.isEmpty() ? QSysInfo::machineHostName() : m_config.syncDeviceName;
    return o;
}

void SyncService::applyConfig(const Config& config, bool runSoon)
{
    const Config previous = m_config;
    const bool first = !m_configured;
    m_config = config;
    m_configured = true;
    try {
        if (first || previous.syncEnabled != config.syncEnabled) {
            m_repo.setSyncTracking(config.syncEnabled);
        }
        if (config.syncEnabled && m_deviceId.isEmpty()) {
            m_deviceId = m_repo.syncDeviceId();
        }
    } catch (const db::DbError& e) {
        qCWarning(lcSync) << "the database refused a sync setting:" << e.what();
    }
    if (!config.syncEnabled) {
        m_poll.stop();
        m_debounce.stop();
        if (!m_watched.isEmpty()) {
            m_watcher.removePath(m_watched);
            m_watched.clear();
        }
        m_backoffMs = 0;
        return;
    }
    if (first || !previous.syncEnabled || previous.syncFolder != config.syncFolder) {
        ++m_generation;
        m_known.clear();
        m_devices.clear();
        if (!m_watched.isEmpty()) {
            m_watcher.removePath(m_watched);
            m_watched.clear();
        }
        m_backoffMs = 0;
    }
    updatePoll();
    const bool syncChanged = previous.syncEnabled != config.syncEnabled || previous.syncFolder != config.syncFolder
        || previous.syncDeviceName != config.syncDeviceName || previous.syncMaxCopies != config.syncMaxCopies
        || previous.syncPictures != config.syncPictures || previous.syncPictureMaxKb != config.syncPictureMaxKb
        || previous.syncTombstoneDays != config.syncTombstoneDays || previous.maxHistory != config.maxHistory
        || previous.maxImages != config.maxImages;
    if (runSoon && (first || syncChanged)) {
        scheduleRun(1500);
    }
}

void SyncService::updatePoll()
{
    if (!m_config.syncEnabled) {
        return;
    }
    m_poll.start(std::max(m_pollMs, m_backoffMs));
}

void SyncService::scheduleRun(int delayMs)
{
    m_debounce.start(delayMs);
}

bool SyncService::available(QString* why)
{
    if (m_availableState < 0) {
        QString reason;
        m_availableState = m_keys && m_keys->available(&reason) ? 1 : 0;
        m_availableWhy = reason.isEmpty() && m_availableState == 0 ? tr("no system keyring is available") : reason;
    }
    if (why) {
        *why = m_availableWhy;
    }
    return m_availableState == 1;
}

bool SyncService::loadKey()
{
    if (!m_keyLoaded) {
        m_keyLoaded = true;
        m_keyProblem.clear();
        if (!m_keys) {
            m_keyProblem = tr("no system keyring is available");
        } else {
            QString error;
            const auto bytes = m_keys->read(&error);
            if (bytes) {
                m_key = sync::Key::parse(*bytes);
            } else if (!error.isEmpty()) {
                m_keyProblem = error;
                m_keyLoaded = false; // the keyring may answer later
            }
        }
    }
    return m_key.isValid();
}

bool SyncService::hasPassphrase()
{
    return loadKey();
}

void SyncService::setPassphrase(const QString& folder, QByteArray passphrase, bool startNew,
                                std::function<void(QString, bool)> done)
{
    struct Result {
        sync::Key key;
        QString error;
        bool mismatch = false;
    };
    const VaultKdfCost cost = m_cost;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, watcher, done = std::move(done), folder]() mutable {
        Result r = watcher->result();
        watcher->deleteLater();
        if (!r.key.isValid()) {
            done(r.error.isEmpty() ? tr("The sync key could not be made.") : r.error, r.mismatch);
            return;
        }
        QString error;
        if (!m_keys || !m_keys->write(r.key.serialize(), &error)) {
            r.key.wipe();
            done(tr("The system keyring did not take the sync key%1.")
                     .arg(error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(error)),
                 false);
            return;
        }
        ++m_generation;
        m_key.wipe();
        m_key = r.key;
        m_keyLoaded = true;
        m_keyProblem.clear();
        m_known.clear(); // the files are read again with the new key
        m_devices.clear();
        m_lastProblem.clear();
        try {
            m_repo.setSyncFingerprint(QString()); // our own file is rewritten with the new key
        } catch (const db::DbError&) {
        }
        qCInfo(lcSync).noquote() << "sync key set for" << folder;
        done(QString(), false);
        if (m_config.syncEnabled) {
            scheduleRun(500);
        }
    });
    watcher->setFuture(QtConcurrent::run([passphrase = std::move(passphrase), folder, startNew, cost]() mutable {
        Result r;
        const sync::FolderState state = sync::checkFolder(folder);
        if (state != sync::FolderState::Ok) {
            r.error = sync::describe(state);
        } else if (startNew) {
            r.key = sync::deriveKey(passphrase, {}, cost);
        } else {
            bool existing = false;
            if (auto key = sync::keyForFolder(folder, passphrase, &existing, cost)) {
                r.key = *key;
            } else {
                r.mismatch = true;
                r.error = QObject::tr("This passphrase does not open the sync files in the folder. Use the one you set "
                                      "on your other PCs.");
            }
        }
        PasswordVault::wipe(passphrase);
        return r;
    }));
}

void SyncService::removePassphrase()
{
    if (m_keys) {
        QString error;
        if (!m_keys->remove(&error)) {
            qCWarning(lcSync).noquote() << "could not remove the sync key from the keyring:" << error;
        }
    }
    ++m_generation;
    m_key.wipe();
    m_key = {};
    m_keyLoaded = true;
    m_known.clear();
    m_devices.clear();
    m_lastProblem.clear();
    try {
        m_repo.setSyncFingerprint(QString());
    } catch (const db::DbError&) {
    }
}

void SyncService::syncNow()
{
    run(/*manual=*/true);
}

void SyncService::run(bool manual)
{
    if (!m_config.syncEnabled) {
        if (manual) {
            m_lastProblem = tr("Sync is turned off.");
            emit finished();
        }
        return;
    }
    if (manual) {
        m_backoffMs = 0;
        updatePoll();
    }
    if (m_running) {
        m_again = true;
        return;
    }
    if (blocked()) {
        scheduleRun(5000); // the history is being converted
        return;
    }
    if (!loadKey()) {
        m_lastProblem = m_keyProblem.isEmpty() ? tr("No sync passphrase is set on this PC.")
                                               : tr("The sync key could not be read: %1").arg(m_keyProblem);
        emit finished();
        return;
    }
    const QString folder = m_config.syncFolder.trimmed();
    if (folder.isEmpty()) {
        m_lastProblem = tr("No sync folder is chosen.");
        emit finished();
        return;
    }
    try {
        m_deviceId = m_deviceId.isEmpty() ? m_repo.syncDeviceId() : m_deviceId;
    } catch (const db::DbError& e) {
        m_lastProblem = QString::fromUtf8(e.what());
        emit finished();
        return;
    }
    m_running = true;
    m_again = false;
    m_lastMerge = {};
    m_lastProblem.clear();
    const sync::Key key = m_key;
    const QString own = m_deviceId;
    const QHash<QString, sync::Known> known = m_known;
    m_readGeneration = m_generation;
    m_reading.setFuture(QtConcurrent::run([folder, key, own, known] {
        ReadJob job;
        try {
            job.state = sync::checkFolder(folder);
            if (job.state == sync::FolderState::Ok) {
                job.files = sync::readFolder(folder, key, own, known);
                job.ownFileExists = QFileInfo::exists(QDir(folder).filePath(sync::fileNameFor(own)));
            }
        } catch (const std::exception&) {
            job.error = QObject::tr("The sync folder could not be read.");
        }
        return job;
    }));
}

void SyncService::onRead()
{
    ReadJob job;
    try {
        job = m_reading.result();
    } catch (const std::exception&) {
        job.state = sync::FolderState::Missing;
        job.error = tr("The sync folder could not be read.");
    }
    if (m_readGeneration != m_generation) {
        finishRun(); // the key or the folder changed meanwhile: this answer is stale, the next run starts afresh
        scheduleRun(500);
        return;
    }
    if (!job.error.isEmpty()) {
        fail(job.error);
        return;
    }
    if (job.state != sync::FolderState::Ok) {
        fail(sync::describe(job.state));
        return;
    }
    m_ownFileExists = job.ownFileExists;
    watchFolder();

    // What is in the folder now.
    QHash<QString, Device> devices;
    QHash<QString, sync::Known> known;
    std::vector<sync::Content> remotes;
    m_ignoredOtherKey = m_ignoredDamaged = m_ignoredNewer = 0;
    for (sync::RemoteFile& f : job.files) {
        Device d;
        d.id = f.deviceId;
        d.name = f.deviceName;
        d.modified = f.modified;
        d.copies = f.copies;
        d.status = f.status;
        if (f.unchanged) {
            const Device old = m_devices.value(f.name);
            d.id = old.id.isEmpty() ? d.id : old.id;
            d.name = old.name;
            d.copies = old.copies;
            known.insert(f.name, m_known.value(f.name));
        } else if (f.status == sync::ReadStatus::Ok) {
            known.insert(f.name, {f.size, f.modified});
            remotes.push_back(std::move(f.content));
        }
        switch (f.status) {
        case sync::ReadStatus::OtherKey:
            ++m_ignoredOtherKey;
            break;
        case sync::ReadStatus::NewerVersion:
            ++m_ignoredNewer;
            break;
        case sync::ReadStatus::Ok:
            break;
        default:
            ++m_ignoredDamaged;
            break;
        }
        devices.insert(f.name, d);
    }
    m_devices = devices;
    m_known = known;
    qCInfo(lcSync).noquote() << "sync folder" << m_config.syncFolder << ":" << job.files.size() << "file(s) of other devices," << remotes.size()
                             << "to merge";
    m_merger = std::make_unique<sync::Merger>(m_repo, std::move(remotes), options());
    QTimer::singleShot(0, this, &SyncService::stepMerge);
}

void SyncService::stepMerge()
{
    if (!m_merger) {
        return;
    }
    if (blocked()) {
        m_merger.reset();
        fail(tr("The history was being converted; sync tries again."));
        return;
    }
    bool done = false;
    try {
        done = m_merger->step(sync::kBatch);
    } catch (const db::DbError& e) {
        m_merger.reset();
        fail(QString::fromUtf8(e.what()));
        return;
    }
    if (!done) {
        QTimer::singleShot(0, this, &SyncService::stepMerge); // the event loop runs between two batches
        return;
    }
    m_lastMerge = m_merger->stats();
    m_merger.reset();
    try {
        if (m_lastMerge.added > 0) {
            m_repo.prune(m_config.maxHistory); // what was added may exceed this PC's limits
            m_repo.pruneImages(m_config.maxImages);
        }
        // Hashes of deleted copies are kept no longer than they are announced.
        m_repo.syncPurgeTombstones(stampOf(QDateTime::currentDateTimeUtc().addDays(-m_config.syncTombstoneDays)));
    } catch (const db::DbError& e) {
        fail(QString::fromUtf8(e.what()));
        return;
    }
    qCInfo(lcSync) << "merged:" << m_lastMerge.added << "added," << m_lastMerge.updated << "changed," << m_lastMerge.deleted
                   << "deleted," << m_lastMerge.skipped << "refused";
    if (m_lastMerge.added || m_lastMerge.updated || m_lastMerge.deleted) {
        emit historyChanged();
    }
    startCollect();
}

void SyncService::startCollect()
{
    try {
        m_collector = std::make_unique<sync::Collector>(m_repo, options());
        if (m_collector->fingerprint() == m_repo.syncFingerprint() && m_ownFileExists) {
            m_collector.reset(); // nothing changed since the last write: the file stays as it is
            m_lastRun = QDateTime::currentDateTime();
            finishRun();
            return;
        }
    } catch (const db::DbError& e) {
        m_collector.reset();
        fail(QString::fromUtf8(e.what()));
        return;
    }
    QTimer::singleShot(0, this, &SyncService::stepCollect);
}

void SyncService::stepCollect()
{
    if (!m_collector) {
        return;
    }
    if (blocked()) {
        m_collector.reset();
        fail(tr("The history was being converted; sync tries again."));
        return;
    }
    bool done = false;
    try {
        done = m_collector->step(sync::kBatch);
    } catch (const db::DbError& e) {
        m_collector.reset();
        fail(QString::fromUtf8(e.what()));
        return;
    }
    if (!done) {
        QTimer::singleShot(0, this, &SyncService::stepCollect);
        return;
    }
    m_pendingFingerprint = m_collector->fingerprint();
    sync::Content content = m_collector->take();
    m_collector.reset();
    m_lastWrittenCopies = int(content.items.size());
    const sync::Key key = m_key;
    const QString folder = m_config.syncFolder.trimmed();
    const VaultKdfCost cost = m_cost;
    m_writing.setFuture(QtConcurrent::run([content = std::move(content), key, folder, cost] {
        WriteJob job;
        const QByteArray data = sync::encode(content, key, cost);
        job.bytes = data.size();
        try {
            job.ok = !data.isEmpty() && sync::writeAtomic(folder, sync::fileNameFor(content.deviceId), data, &job.error);
        } catch (const std::exception&) {
            job.ok = false;
        }
        if (data.isEmpty() && job.error.isEmpty()) {
            job.error = QObject::tr("The sync file could not be made.");
        }
        return job;
    }));
}

void SyncService::onWritten()
{
    WriteJob job;
    try {
        job = m_writing.result();
    } catch (const std::exception&) {
        job.error = tr("The sync file could not be written.");
    }
    if (m_readGeneration != m_generation) {
        finishRun();
        scheduleRun(500);
        return;
    }
    if (!job.ok) {
        fail(job.error);
        return;
    }
    try {
        m_repo.setSyncFingerprint(m_pendingFingerprint);
    } catch (const db::DbError&) {
    }
    m_ownFileExists = true;
    m_lastWrite = QDateTime::currentDateTime();
    m_lastRun = m_lastWrite;
    qCInfo(lcSync) << "sync file written:" << m_lastWrittenCopies << "copies," << job.bytes << "bytes";
    finishRun();
}

void SyncService::fail(const QString& message)
{
    m_lastProblem = message;
    qCWarning(lcSync).noquote() << "sync did not complete:" << message;
    // A quiet back-off: the folder may be an unplugged share. A change in the folder, "Sync now" or the settings end it.
    m_backoffMs = m_backoffMs == 0 ? kFirstBackoffMs : std::min(m_backoffMs * 2, kMaxBackoffMs);
    updatePoll();
    finishRun();
}

void SyncService::finishRun()
{
    if (m_lastProblem.isEmpty() && m_backoffMs != 0) {
        m_backoffMs = 0;
        updatePoll();
    }
    m_running = false;
    emit finished();
    if (m_again) {
        m_again = false;
        scheduleRun(500);
    }
}

void SyncService::watchFolder()
{
    const QString folder = m_config.syncFolder.trimmed();
    if (folder == m_watched && m_watcher.directories().contains(folder)) {
        return; // (a watch the system dropped, as when a share went away, is added again)
    }
    if (!m_watched.isEmpty()) {
        m_watcher.removePath(m_watched);
    }
    m_watched.clear();
    if (!folder.isEmpty() && m_watcher.addPath(folder)) { // may fail on a network share: the timer still runs
        m_watched = folder;
    }
}

QString SyncService::statusLine() const
{
    if (!m_config.syncEnabled) {
        return QStringLiteral("off");
    }
    QString line = QStringLiteral("on, %1 other device(s)").arg(m_devices.size());
    if (!m_lastProblem.isEmpty()) {
        line += QStringLiteral("; waiting: ") + m_lastProblem;
    } else if (m_lastRun.isValid()) {
        line += QStringLiteral("; last sync ") + clock(m_lastRun);
    }
    return line;
}

QString SyncService::statusText() const
{
    QStringList lines;
    if (!m_config.syncEnabled) {
        lines << tr("Sync is off.");
    }
    if (!m_lastProblem.isEmpty()) {
        lines << tr("Not synced: %1").arg(m_lastProblem);
        if (m_backoffMs > 0) {
            lines << tr("ClipItTo tries again by itself, or press Sync now.");
        }
    } else if (m_running) {
        lines << tr("Syncing…");
    } else if (m_lastRun.isValid()) {
        lines << tr("Last sync: %1. From other PCs: %2 added, %3 changed, %4 deleted.")
                     .arg(clock(m_lastRun)).arg(m_lastMerge.added).arg(m_lastMerge.updated).arg(m_lastMerge.deleted);
    }
    if (m_lastWrite.isValid()) {
        lines << tr("This PC's file: %1 copies, written %2.").arg(m_lastWrittenCopies).arg(clock(m_lastWrite));
    }
    if (!m_devices.isEmpty()) {
        QStringList names;
        std::vector<Device> sorted;
        for (const Device& d : m_devices) {
            sorted.push_back(d);
        }
        std::sort(sorted.begin(), sorted.end(), [](const Device& a, const Device& b) { return a.id < b.id; });
        for (const Device& d : sorted) {
            if (d.status == sync::ReadStatus::Ok) {
                names << tr("%1 (%2 copies, %3)")
                             .arg(d.name.isEmpty() ? d.id.left(8) : d.name).arg(d.copies).arg(clock(d.modified));
            }
        }
        if (!names.isEmpty()) {
            lines << tr("Other PCs: %1").arg(names.join(QStringLiteral("; ")));
        }
    }
    if (m_ignoredOtherKey > 0) {
        lines << tr("Ignored: %n file(s) made with a different sync passphrase.", nullptr, m_ignoredOtherKey);
    }
    if (m_ignoredDamaged > 0) {
        lines << tr("Ignored for now: %n file(s) damaged or still being copied.", nullptr, m_ignoredDamaged);
    }
    if (m_ignoredNewer > 0) {
        lines << tr("Ignored: %n file(s) made by a newer ClipItTo; update ClipItTo.", nullptr, m_ignoredNewer);
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace clipitto
