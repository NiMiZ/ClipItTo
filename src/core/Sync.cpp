#include "Sync.h"

#include "GroupPath.h"
#include "db/Repository.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <filesystem>
#include <map>
#include <tuple>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace clipitto::sync {

namespace {

const QByteArray kMagic = QByteArrayLiteral("CLIPSYNC");
constexpr int kHeaderSize = 8 + 1 + 16 + 4 + 4;
constexpr quint32 kMaxPlain = 1024u * 1024u * 1024u; // the announced size of the decompressed text
constexpr qint64 kMaxPictureBytes = 64LL * 1024 * 1024;
const QString kFormat = QStringLiteral("clipitto-sync");

QString b64(const QByteArray& data)
{
    return QString::fromLatin1(data.toBase64());
}

QByteArray unb64(const QJsonValue& value)
{
    return QByteArray::fromBase64(value.toString().toLatin1());
}

// An ISO time as the history writes it (UTC, milliseconds, so strings order like times); empty when not a time.
QString normalizeStamp(const QString& text)
{
    if (text.isEmpty()) {
        return {};
    }
    QDateTime t = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!t.isValid()) {
        t = QDateTime::fromString(text, Qt::ISODate);
    }
    return t.isValid() ? t.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QString stamp(const QDateTime& time)
{
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QByteArray header(const Key& key, VaultKdfCost cost)
{
    QByteArray h = kMagic;
    h.append(char(kFormatVersion));
    h.append(key.salt);
    char word[4];
    qToLittleEndian<quint32>(cost.blocks, word);
    h.append(word, 4);
    qToLittleEndian<quint32>(cost.passes, word);
    h.append(word, 4);
    return h;
}

QJsonObject toJson(const Item& it)
{
    QJsonObject o;
    o[QStringLiteral("k")] = it.image ? QStringLiteral("i") : QStringLiteral("t");
    o[QStringLiteral("n")] = it.note;
    o[QStringLiteral("g")] = it.group;
    o[QStringLiteral("f")] = it.favorite;
    o[QStringLiteral("p")] = it.pinned;
    o[QStringLiteral("o")] = it.offRecent;
    o[QStringLiteral("cr")] = it.created;
    o[QStringLiteral("lu")] = it.lastUsed;
    o[QStringLiteral("mc")] = it.metaChanged;
    if (it.image) {
        o[QStringLiteral("png")] = b64(it.png);
        o[QStringLiteral("th")] = b64(it.thumbnail);
        o[QStringLiteral("w")] = it.width;
        o[QStringLiteral("h")] = it.height;
        o[QStringLiteral("ph")] = it.pixelHash;
    } else {
        o[QStringLiteral("c")] = it.content;
    }
    return o;
}

Item itemFromJson(const QJsonObject& o)
{
    Item it;
    it.image = o.value(QStringLiteral("k")).toString() == QLatin1String("i");
    it.note = o.value(QStringLiteral("n")).toString();
    it.group = o.value(QStringLiteral("g")).toString();
    it.favorite = o.value(QStringLiteral("f")).toBool();
    it.pinned = o.value(QStringLiteral("p")).toBool();
    it.offRecent = o.value(QStringLiteral("o")).toBool();
    it.created = normalizeStamp(o.value(QStringLiteral("cr")).toString());
    it.lastUsed = normalizeStamp(o.value(QStringLiteral("lu")).toString());
    it.metaChanged = normalizeStamp(o.value(QStringLiteral("mc")).toString());
    if (it.image) {
        it.png = unb64(o.value(QStringLiteral("png")));
        it.thumbnail = unb64(o.value(QStringLiteral("th")));
        it.width = o.value(QStringLiteral("w")).toInt();
        it.height = o.value(QStringLiteral("h")).toInt();
        it.pixelHash = o.value(QStringLiteral("ph")).toString();
    } else {
        it.content = o.value(QStringLiteral("c")).toString();
    }
    return it;
}

QString nameStem(const QString& fileName)
{
    return fileName.left(fileName.size() - int(qstrlen(kFileSuffix)));
}

} // namespace

// ---- Key

Key Key::parse(const QByteArray& bytes)
{
    Key k;
    if (bytes.size() == 48) {
        k.salt = bytes.left(16);
        k.key = bytes.mid(16);
    }
    return k;
}

void Key::wipe()
{
    PasswordVault::wipe(key);
    key.clear();
}

Key deriveKey(const QByteArray& passphrase, QByteArray salt, VaultKdfCost cost)
{
    if (salt.isEmpty()) {
        salt = PasswordVault::randomBytes(16);
    }
    Key k;
    k.key = PasswordVault::deriveBackupKey(passphrase, salt, cost);
    if (k.key.size() == 32) {
        k.salt = salt;
    } else {
        k.key.clear();
    }
    return k;
}

// ---- the file

QByteArray encode(const Content& content, const Key& key, VaultKdfCost cost)
{
    if (!key.isValid()) {
        return {};
    }
    QJsonObject root;
    root[QStringLiteral("format")] = kFormat;
    root[QStringLiteral("device")] = content.deviceId;
    root[QStringLiteral("name")] = content.deviceName;
    root[QStringLiteral("written")] = content.written;
    QJsonArray items;
    for (const Item& it : content.items) {
        items.append(toJson(it));
    }
    root[QStringLiteral("items")] = items;
    QJsonArray groups;
    for (const Group& g : content.groups) {
        QJsonObject o;
        o[QStringLiteral("path")] = g.path;
        o[QStringLiteral("hide")] = g.hideFromRecent;
        groups.append(o);
    }
    root[QStringLiteral("groups")] = groups;
    QJsonArray tombs;
    for (const Tombstone& t : content.tombstones) {
        QJsonObject o;
        o[QStringLiteral("h")] = t.hash;
        o[QStringLiteral("at")] = t.at;
        tombs.append(o);
    }
    root[QStringLiteral("tombstones")] = tombs;

    QByteArray plain = qCompress(QJsonDocument(root).toJson(QJsonDocument::Compact), 6);
    const QByteArray h = header(key, cost);
    const QByteArray out = h + PasswordVault::boxWithSyncKey(key.key, plain, h);
    PasswordVault::wipe(plain);
    return out;
}

Header peekHeader(const QByteArray& file)
{
    Header h;
    if (file.size() < kHeaderSize || !file.startsWith(kMagic)) {
        return h;
    }
    h.version = uchar(file.at(8));
    h.salt = file.mid(9, 16);
    h.cost.blocks = qFromLittleEndian<quint32>(file.constData() + 25);
    h.cost.passes = qFromLittleEndian<quint32>(file.constData() + 29);
    h.valid = true;
    return h;
}

Decoded decode(const QByteArray& file, const Key& key)
{
    Decoded out;
    if (file.size() > kMaxFileBytes) {
        out.status = ReadStatus::TooBig;
        return out;
    }
    const Header h = peekHeader(file);
    if (!h.valid || h.version < 1) {
        out.status = ReadStatus::NotOurs;
        return out;
    }
    if (h.version > kFormatVersion) {
        out.status = ReadStatus::NewerVersion;
        return out;
    }
    if (!key.isValid() || h.salt != key.salt) {
        out.status = ReadStatus::OtherKey;
        return out;
    }
    out.status = ReadStatus::Damaged;
    const auto compressed = PasswordVault::unboxWithSyncKey(key.key, file.mid(kHeaderSize), file.left(kHeaderSize));
    if (!compressed || compressed->size() < 4 || qFromBigEndian<quint32>(compressed->constData()) > kMaxPlain) {
        return out;
    }
    QByteArray json = qUncompress(*compressed);
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    PasswordVault::wipe(json);
    if (!doc.isObject() || doc.object().value(QStringLiteral("format")).toString() != kFormat) {
        return out;
    }
    const QJsonObject root = doc.object();
    Content& c = out.content;
    c.deviceId = root.value(QStringLiteral("device")).toString();
    c.deviceName = root.value(QStringLiteral("name")).toString();
    c.written = normalizeStamp(root.value(QStringLiteral("written")).toString());
    for (const QJsonValue& v : root.value(QStringLiteral("items")).toArray()) {
        c.items.push_back(itemFromJson(v.toObject()));
    }
    for (const QJsonValue& v : root.value(QStringLiteral("groups")).toArray()) {
        c.groups.push_back({v.toObject().value(QStringLiteral("path")).toString(), v.toObject().value(QStringLiteral("hide")).toBool()});
    }
    for (const QJsonValue& v : root.value(QStringLiteral("tombstones")).toArray()) {
        const QString at = normalizeStamp(v.toObject().value(QStringLiteral("at")).toString());
        const QString hash = v.toObject().value(QStringLiteral("h")).toString();
        if (!at.isEmpty() && !hash.isEmpty()) {
            c.tombstones.push_back({hash, at});
        }
    }
    out.status = ReadStatus::Ok;
    return out;
}

// ---- the folder

QString fileNameFor(const QString& deviceId)
{
    return deviceId + QLatin1String(kFileSuffix);
}

FolderState checkFolder(const QString& folder)
{
    if (folder.trimmed().isEmpty()) {
        return FolderState::Missing;
    }
    const QFileInfo info(folder);
    if (!info.exists()) {
        return FolderState::Missing;
    }
    if (!info.isDir()) {
        return FolderState::NotDirectory;
    }
    if (!info.isWritable() || !info.isExecutable()) {
        return FolderState::NotWritable;
    }
    return FolderState::Ok;
}

QString describe(FolderState state)
{
    switch (state) {
    case FolderState::Ok:
        return QObject::tr("The folder is available.");
    case FolderState::Missing:
        return QObject::tr("The folder is not available (a drive or share that is not connected?).");
    case FolderState::NotDirectory:
        return QObject::tr("The chosen path is not a folder.");
    case FolderState::NotWritable:
        return QObject::tr("The folder can't be written to.");
    }
    return {};
}

namespace {

void syncToDisk(QFile& file)
{
    file.flush();
#ifdef Q_OS_WIN
    _commit(file.handle());
#else
    ::fsync(file.handle());
#endif
}

void syncFolder(const QString& folder)
{
#ifndef Q_OS_WIN
    const int fd = ::open(QFile::encodeName(folder).constData(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    Q_UNUSED(folder)
#endif
}

std::filesystem::path pathOf(const QString& p)
{
    return std::filesystem::path(p.toStdU16String());
}

} // namespace

bool writeAtomic(const QString& folder, const QString& fileName, const QByteArray& data, QString* error,
                 const WriteHooks& hooks)
{
    const auto fail = [&](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    const FolderState state = checkFolder(folder);
    if (state != FolderState::Ok) {
        return fail(describe(state));
    }
    const QDir dir(folder);
    const QString finalPath = dir.filePath(fileName);
    const QString tempPath = finalPath + QStringLiteral(".tmp-") + QString::fromLatin1(PasswordVault::randomBytes(4).toHex());
    {
        QFile file(tempPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return fail(QObject::tr("Can't write in %1: %2").arg(folder, file.errorString()));
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const bool wrote = file.write(data) == data.size();
        syncToDisk(file);
        file.close(); // closed before the rename: Windows would refuse otherwise
        if (!wrote || file.error() != QFileDevice::NoError) {
            const QString why = file.errorString();
            QFile::remove(tempPath);
            return fail(QObject::tr("Can't write in %1: %2").arg(folder, why));
        }
    }
    if (hooks.beforeRename && !hooks.beforeRename()) {
        QFile::remove(tempPath);
        return fail(QObject::tr("Writing was interrupted."));
    }
    std::error_code ec;
    int delay = 100;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::filesystem::rename(pathOf(tempPath), pathOf(finalPath), ec);
        if (!ec) {
            break;
        }
        if (attempt < 4) {
            QThread::msleep(ulong(delay)); // the sync client may hold the file for a moment
            delay *= 2;
        }
    }
    if (ec) {
        QFile::remove(tempPath);
        return fail(QObject::tr("Can't replace the sync file in %1 (%2).").arg(folder, QString::fromStdString(ec.message())));
    }
    syncFolder(folder);
    return true;
}

namespace {

// The file's bytes, or nullopt when it can't be opened (locked by a sync client?) after a few tries.
std::optional<QByteArray> readWhole(const QString& path)
{
    int delay = 100;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            if (file.size() > kMaxFileBytes) {
                return QByteArray();
            }
            return file.readAll();
        }
        if (attempt < 2) {
            QThread::msleep(ulong(delay));
            delay *= 2;
        }
    }
    return std::nullopt;
}

QFileInfoList syncFiles(const QString& folder)
{
    return QDir(folder).entryInfoList({QStringLiteral("*") + QLatin1String(kFileSuffix)}, QDir::Files | QDir::NoSymLinks,
                                      QDir::Name);
}

} // namespace

std::optional<Key> keyForFolder(const QString& folder, const QByteArray& passphrase, bool* existing, VaultKdfCost newCost)
{
    if (existing) {
        *existing = false;
    }
    QSet<QByteArray> tried;
    // Every group in the folder this passphrase opens; the one with the smallest salt wins, so two PCs that made a group
    // each (they set the passphrase before either file was in the folder) agree on one when they enter it again.
    std::optional<Key> best;
    for (const QFileInfo& info : syncFiles(folder)) {
        const auto bytes = readWhole(info.absoluteFilePath());
        if (!bytes) {
            continue;
        }
        const Header h = peekHeader(*bytes);
        if (!h.valid || h.version != kFormatVersion) {
            continue;
        }
        if (existing) {
            *existing = true;
        }
        const QByteArray id = h.salt + QByteArray::number(h.cost.blocks) + QByteArray::number(h.cost.passes);
        // A file is not trusted to ask for any amount of memory: only costs near ours are tried.
        if (h.cost.blocks > 256 * 1024 || h.cost.passes > 6 || tried.contains(id) || tried.size() >= 4) {
            continue;
        }
        tried.insert(id);
        Key key = deriveKey(passphrase, h.salt, h.cost);
        if (key.isValid() && decode(*bytes, key).status == ReadStatus::Ok) {
            if (!best || key.salt < best->salt) {
                if (best) {
                    best->wipe();
                }
                best = key;
            }
            continue;
        }
        key.wipe();
    }
    if (best) {
        return best;
    }
    if (existing && *existing) {
        return std::nullopt;
    }
    return deriveKey(passphrase, {}, newCost);
}

std::vector<RemoteFile> readFolder(const QString& folder, const Key& key, const QString& ownDeviceId,
                                   const QHash<QString, Known>& known)
{
    std::vector<RemoteFile> out;
    for (const QFileInfo& info : syncFiles(folder)) {
        if (info.fileName() == fileNameFor(ownDeviceId)) {
            continue;
        }
        RemoteFile r;
        r.name = info.fileName();
        r.size = info.size();
        r.modified = info.lastModified();
        r.deviceId = nameStem(r.name);
        const auto old = known.constFind(r.name);
        if (old != known.constEnd() && old->size == r.size && old->modified == r.modified) {
            r.status = ReadStatus::Ok;
            r.unchanged = true;
            out.push_back(std::move(r));
            continue;
        }
        if (r.size > kMaxFileBytes) {
            r.status = ReadStatus::TooBig;
            out.push_back(std::move(r));
            continue;
        }
        const auto bytes = readWhole(info.absoluteFilePath());
        if (!bytes) {
            r.status = ReadStatus::Damaged; // locked or vanished: tried again at the next run
            out.push_back(std::move(r));
            continue;
        }
        Decoded d = decode(*bytes, key);
        r.status = d.status;
        if (d.status == ReadStatus::Ok) {
            if (d.content.deviceId == ownDeviceId) {
                continue; // a copy of our own file under another name
            }
            r.deviceId = d.content.deviceId.isEmpty() ? r.deviceId : d.content.deviceId;
            r.deviceName = d.content.deviceName;
            r.copies = int(d.content.items.size());
            r.content = std::move(d.content);
        }
        out.push_back(std::move(r));
    }
    return out;
}

// ---- Collector

Collector::Collector(db::Repository& repo, const Options& options) : m_repo(repo), m_options(options)
{
    const auto list = repo.syncExportList(options.maxCopies, options.pictures, options.maxPictureBytes);
    const QString since = stamp(QDateTime::currentDateTimeUtc().addDays(-options.tombstoneDays));
    const auto tombs = repo.syncSharedTombstones(since);

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto feed = [&hash](const QString& s) {
        hash.addData(s.toUtf8());
        hash.addData(QByteArrayView("\x1f", 1));
    };
    feed(QString::number(kFormatVersion));
    feed(m_options.deviceName);
    feed(QString::number(options.maxCopies) + QLatin1Char(options.pictures ? 'p' : '-') + QString::number(options.maxPictureBytes));
    for (const auto& c : list) {
        m_ids.push_back(c.id);
        feed(c.hash);
        feed(c.lastUsed);
        feed(c.metaChanged);
    }
    feed(QStringLiteral("tombstones"));
    for (const auto& t : tombs) {
        m_content.tombstones.push_back({t.hash, t.deletedAt});
        feed(t.hash);
        feed(t.deletedAt);
    }
    feed(QStringLiteral("groups"));
    for (const QString& g : repo.groups()) {
        const bool hide = repo.groupHiddenFromRecent(g);
        m_content.groups.push_back({g, hide});
        feed(g);
        feed(hide ? QStringLiteral("h") : QStringLiteral("-"));
    }
    m_fingerprint = QString::fromLatin1(hash.result().toHex());
}

Collector::~Collector() = default;

bool Collector::step(int budget)
{
    for (int i = 0; i < budget && m_next < m_ids.size(); ++i, ++m_next) {
        const auto row = m_repo.syncItem(m_ids[m_next]);
        if (!row) {
            continue; // gone meanwhile
        }
        const Entry& e = row->entry;
        Item it;
        it.image = e.image;
        it.note = e.note;
        it.group = e.group;
        it.favorite = e.favorite;
        it.pinned = e.pinned();
        it.offRecent = row->offRecent;
        it.created = stamp(e.createdAt);
        it.lastUsed = stamp(e.lastUsed);
        it.metaChanged = row->metaChanged;
        if (e.image) {
            it.png = row->png;
            it.thumbnail = e.thumbnail;
            it.width = e.imageWidth;
            it.height = e.imageHeight;
            it.pixelHash = e.contentHash.mid(int(QStringLiteral("image:").size()));
            if (it.png.isEmpty()) {
                continue;
            }
        } else {
            it.content = e.content;
        }
        m_content.items.push_back(std::move(it));
    }
    return m_next >= m_ids.size();
}

Content Collector::take()
{
    m_content.deviceId = m_repo.syncDeviceId();
    m_content.deviceName = m_options.deviceName;
    m_content.written = stamp(QDateTime::currentDateTimeUtc());
    return std::move(m_content);
}

// ---- Merger

namespace {

// When a copy last changed here: used or filed.
QString changeTime(const QString& lastUsed, const QString& metaChanged)
{
    return std::max(lastUsed, metaChanged);
}

QString canonical(const QString& note, const QString& group, bool favorite, bool pinned)
{
    return group + QLatin1Char('\x1f') + (favorite ? QLatin1Char('1') : QLatin1Char('0')) + (pinned ? QLatin1Char('1') : QLatin1Char('0'))
        + QLatin1Char('\x1f') + note;
}

} // namespace

Merger::Merger(db::Repository& repo, std::vector<Content> remotes, const Options& options)
    : m_repo(repo), m_options(options), m_remotes(std::move(remotes))
{
    // Always in the same order, whatever order the files were listed in.
    std::stable_sort(m_remotes.begin(), m_remotes.end(),
                     [](const Content& a, const Content& b) { return a.deviceId < b.deviceId; });
    for (const Content& c : m_remotes) {
        for (const Tombstone& t : c.tombstones) {
            auto& at = m_tombAt[t.hash];
            at = std::max(at, t.at);
        }
    }
    for (auto it = m_tombAt.constBegin(); it != m_tombAt.constEnd(); ++it) {
        m_tombs.emplace_back(it.key(), it.value());
    }
    std::sort(m_tombs.begin(), m_tombs.end());
    for (size_t f = 0; f < m_remotes.size(); ++f) {
        for (size_t i = 0; i < m_remotes[f].items.size(); ++i) {
            m_items.emplace_back(f, i);
        }
    }
}

Merger::~Merger() = default;

bool Merger::step(int budget)
{
    int done = 0;
    m_unsaved[0].loaded = m_unsaved[1].loaded = false;
    m_repo.syncBatch([&] {
        while (done < budget && m_nextTomb < m_tombs.size()) {
            tombstone(m_tombs[m_nextTomb].first, m_tombs[m_nextTomb].second);
            ++m_nextTomb;
            ++done;
        }
        while (done < budget && m_nextTomb >= m_tombs.size() && m_nextItem < m_items.size()) {
            const auto [f, i] = m_items[m_nextItem];
            item(m_remotes[f].items[i]);
            ++m_nextItem;
            ++done;
        }
    });
    const bool finished = m_nextTomb >= m_tombs.size() && m_nextItem >= m_items.size();
    if (finished && m_stats.deleted > 0 && !m_scrubbed) {
        m_scrubbed = true;
        m_repo.syncScrub(); // what was deleted is gone from the file, not only from the list
    }
    return finished;
}

void Merger::tombstone(const QString& hash, const QString& at)
{
    if (hash.isEmpty() || hash.startsWith(QLatin1String("secret:"))) {
        return; // a password copy never travels
    }
    const auto id = m_repo.findByHash(hash);
    if (!id) {
        return;
    }
    const auto local = m_repo.syncItem(*id);
    if (!local) {
        return; // a password or Paste once copy
    }
    const QString changed = changeTime(stamp(local->entry.lastUsed), local->metaChanged);
    if (changed <= at) {
        if (m_repo.syncDelete(*id)) {
            ++m_stats.deleted;
        }
    } else {
        ++m_stats.skipped; // used again after it was deleted elsewhere: kept
    }
}

void Merger::item(const Item& it)
{
    // Validate: the file comes from a folder other programs can write to.
    if (it.lastUsed.isEmpty()) {
        ++m_stats.skipped;
        return;
    }
    QString hash;
    if (it.image) {
        if (it.png.isEmpty() || it.png.size() > kMaxPictureBytes || it.thumbnail.isEmpty() || it.width <= 0 || it.height <= 0
            || it.width > 100000 || it.height > 100000 || it.pixelHash.isEmpty() || it.pixelHash.size() > 128) {
            ++m_stats.skipped;
            return;
        }
        hash = QStringLiteral("image:") + it.pixelHash;
    } else {
        if (it.content.isEmpty() || it.content.toUtf8().size() > m_options.maxItemBytes) {
            ++m_stats.skipped;
            return;
        }
        hash = m_repo.textHash(it.content);
    }
    const QString note = it.note.trimmed();
    const QString group = groups::normalize(it.group);
    const QString remoteChange = changeTime(it.lastUsed, it.metaChanged);

    if (const auto id = m_repo.findByHash(hash)) {
        const auto local = m_repo.syncItem(*id);
        if (!local) {
            return;
        }
        const Entry& e = local->entry;
        if (e.note == note && e.group == group && e.favorite == it.favorite && e.pinned() == it.pinned) {
            return; // the same filing
        }
        const bool newer = it.metaChanged > local->metaChanged
            || (it.metaChanged == local->metaChanged
                && canonical(note, group, it.favorite, it.pinned) > canonical(e.note, e.group, e.favorite, e.pinned()));
        if (newer) {
            m_repo.syncSetMeta(*id, note, group, it.favorite, it.pinned, it.metaChanged);
            ++m_stats.updated;
        }
        return;
    }

    const QString tomb = std::max(m_tombAt.value(hash), m_repo.syncTombstone(hash));
    if (!tomb.isEmpty() && tomb >= remoteChange) {
        ++m_stats.skipped; // deleted after it was last used there
        return;
    }
    const bool saved = !group.isEmpty() || it.favorite || it.pinned;
    if (!saved) {
        const int limit = it.image ? m_options.maxImages : m_options.maxHistory;
        Unsaved& u = m_unsaved[it.image ? 1 : 0];
        if (!u.loaded) {
            std::tie(u.oldest, u.count) = m_repo.syncOldestUnsaved(it.image);
            u.loaded = true;
        }
        if (limit <= 0 || (u.count >= limit && !u.oldest.isEmpty() && it.lastUsed < u.oldest)) {
            ++m_stats.skipped; // it would be pruned at once
            return;
        }
    }

    db::Repository::SyncItem add;
    add.entry.image = it.image;
    add.entry.content = it.image ? QString() : it.content;
    add.entry.note = note;
    add.entry.group = group;
    add.entry.favorite = it.favorite;
    add.entry.createdAt = QDateTime::fromString(it.created.isEmpty() ? it.lastUsed : it.created, Qt::ISODateWithMs);
    add.entry.lastUsed = QDateTime::fromString(it.lastUsed, Qt::ISODateWithMs);
    add.metaChanged = it.metaChanged;
    add.offRecent = it.offRecent;
    ImageClip image;
    if (it.image) {
        image.png = it.png;
        image.thumbnail = it.thumbnail;
        image.width = it.width;
        image.height = it.height;
        image.pixelHash = it.pixelHash;
    }
    const qint64 id = m_repo.syncAdd(add, image);
    if (!saved) {
        Unsaved& u = m_unsaved[it.image ? 1 : 0];
        if (u.count++ == 0 || it.lastUsed < u.oldest) {
            u.oldest = it.lastUsed;
        }
    }
    if (it.pinned) {
        m_repo.syncSetMeta(id, note, group, it.favorite, true, it.metaChanged);
    }
    ++m_stats.added;
}

} // namespace clipitto::sync
