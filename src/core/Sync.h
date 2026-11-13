#pragma once

#include "ImageClip.h"
#include "PasswordVault.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace clipitto {

namespace db {
class Repository;
}

// Sync through a folder the user already syncs (roadmap 13, docs/sync-design.md). ClipItTo makes no network
// connection: it writes ONE encrypted file of its own into the folder and reads the files of the other devices.
// The live database is never put there.
//
// Nothing in here logs or reports a copy's text; messages carry the folder path and counts only.
namespace sync {

inline constexpr const char* kFileSuffix = ".clipitto-sync";
inline constexpr int kFormatVersion = 1;
inline constexpr qint64 kMaxFileBytes = 256LL * 1024 * 1024; // larger files are not read
inline constexpr int kBatch = 25; // copies merged or collected per turn of the event loop

// Argon2id cost of the key from the sync passphrase (the backup's).
inline VaultKdfCost defaultCost() { return VaultKdfCost{64 * 1024, 3}; }

// What the keyring keeps (entry "sync-key"): the group's salt and the key derived from the passphrase.
struct Key {
    QByteArray salt; // 16 bytes, shared by all devices of the group
    QByteArray key; // 32 bytes

    bool isValid() const { return salt.size() == 16 && key.size() == 32; }
    QByteArray serialize() const { return salt + key; }
    static Key parse(const QByteArray& bytes);
    void wipe();
};

// Slow (Argon2id): never on the GUI thread. An empty `salt` makes a new random one.
Key deriveKey(const QByteArray& passphrase, QByteArray salt = {}, VaultKdfCost cost = defaultCost());

// ---- the content of one device's file

struct Item {
    bool image = false;
    QString content; // text
    QString note;
    QString group;
    bool favorite = false;
    bool pinned = false;
    bool offRecent = false;
    QString created; // ISO times, UTC, milliseconds
    QString lastUsed;
    QString metaChanged; // empty: never filed
    QByteArray png; // pictures
    QByteArray thumbnail;
    int width = 0;
    int height = 0;
    QString pixelHash;
};

struct Group {
    QString path;
    bool hideFromRecent = false;
};

struct Tombstone {
    QString hash;
    QString at;
};

struct Content {
    QString deviceId;
    QString deviceName;
    QString written; // ISO time
    std::vector<Item> items;
    std::vector<Group> groups;
    std::vector<Tombstone> tombstones;
};

// The file: header (magic, version, salt, Argon2 cost) + XChaCha20-Poly1305 box of the compressed JSON, the header being
// authenticated too. Empty when the key is not valid.
QByteArray encode(const Content& content, const Key& key, VaultKdfCost cost = defaultCost());

enum class ReadStatus {
    Ok,
    NotOurs, // not a sync file
    NewerVersion, // made by a newer ClipItTo
    OtherKey, // another salt: made with another sync passphrase
    Damaged, // does not open with our key: damaged, tampered with, or still being copied
    TooBig,
};

struct Header {
    bool valid = false;
    int version = 0;
    QByteArray salt;
    VaultKdfCost cost;
};
Header peekHeader(const QByteArray& file);

struct Decoded {
    ReadStatus status = ReadStatus::NotOurs;
    Content content;
};
Decoded decode(const QByteArray& file, const Key& key);

// Join a group: the key for `passphrase` that opens the files already in the folder (their salt and cost are used).
// nullopt when files exist and none opens (wrong passphrase); a Key with a NEW salt when the folder has no sync file.
// Slow (Argon2id): never on the GUI thread. `*existing` says whether a file was found.
std::optional<Key> keyForFolder(const QString& folder, const QByteArray& passphrase, bool* existing = nullptr,
                                VaultKdfCost newCost = defaultCost());

// ---- the folder

enum class FolderState { Ok, Missing, NotDirectory, NotWritable };
// Never creates the folder (an unplugged share would leave an empty local directory at its mount point).
FolderState checkFolder(const QString& folder);
QString describe(FolderState state);

struct WriteHooks {
    std::function<bool()> beforeRename; // tests: false = fail between the write and the rename
};
// Temp file in the same folder, flush, fsync, rename over `fileName`, fsync the folder. A failed attempt leaves nothing
// behind and the old file as it was. Retries a rename that a sync client blocks (Windows) with a growing delay.
bool writeAtomic(const QString& folder, const QString& fileName, const QByteArray& data, QString* error,
                 const WriteHooks& hooks = {});

QString fileNameFor(const QString& deviceId);

struct RemoteFile {
    QString name;
    qint64 size = 0;
    QDateTime modified;
    ReadStatus status = ReadStatus::NotOurs;
    bool unchanged = false; // same size and time as `known`: not read again
    Content content; // when Ok and not unchanged
    QString deviceId; // from the content (or the file name when not opened)
    QString deviceName;
    int copies = 0;
};

struct Known {
    qint64 size = 0;
    QDateTime modified;
};

// Reads every *.clipitto-sync file but `ownDeviceId`'s, with `key`. A file whose size and time are in `known` and that
// opened before is not read again. Off the GUI thread (touches no database).
std::vector<RemoteFile> readFolder(const QString& folder, const Key& key, const QString& ownDeviceId,
                                   const QHash<QString, Known>& known = {});

// ---- what to export and import

struct Options {
    int maxCopies = 2000; // unsaved copies in the file; every saved one is in it besides
    bool pictures = true;
    qint64 maxPictureBytes = 2LL * 1024 * 1024;
    int tombstoneDays = 30;
    qint64 maxItemBytes = 1024 * 1024; // a copy above this in a file is ignored
    int maxHistory = 1000; // this PC's limits: a copy that would be pruned at once is not added
    int maxImages = 100;
    QString deviceName;
};

// Gathers this device's file content in small batches (the database belongs to the GUI thread). `fingerprint()` says
// whether anything that goes into the file changed since the last write, without reading any contents.
class Collector {
public:
    Collector(db::Repository& repo, const Options& options);
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    const QString& fingerprint() const { return m_fingerprint; }
    int copies() const { return int(m_ids.size()); }
    // Reads up to `budget` copies; true when all are read.
    bool step(int budget);
    Content take(); // after step() said true

private:
    db::Repository& m_repo;
    Options m_options;
    QString m_fingerprint;
    std::vector<qint64> m_ids;
    size_t m_next = 0;
    Content m_content;
};

struct MergeStats {
    int added = 0;
    int updated = 0; // filing taken from another device
    int deleted = 0; // by a tombstone
    int skipped = 0; // refused: tombstoned, over the limits, invalid
};

// Merges the other devices' files into the history, in batches (one transaction each). Idempotent. Rules in
// docs/sync-design.md.
class Merger {
public:
    Merger(db::Repository& repo, std::vector<Content> remotes, const Options& options);
    ~Merger();
    Merger(const Merger&) = delete;
    Merger& operator=(const Merger&) = delete;

    // Handles up to `budget` tombstones/copies; true when everything is done (the search index merged and the log
    // folded in when copies were deleted).
    bool step(int budget = kBatch);
    const MergeStats& stats() const { return m_stats; }

private:
    void tombstone(const QString& hash, const QString& at);
    void item(const Item& item);

    db::Repository& m_repo;
    Options m_options;
    std::vector<Content> m_remotes;
    std::vector<std::pair<QString, QString>> m_tombs; // newest deletion per hash, over all files
    QHash<QString, QString> m_tombAt;
    std::vector<std::pair<size_t, size_t>> m_items; // (file, index)
    size_t m_nextTomb = 0;
    size_t m_nextItem = 0;
    bool m_scrubbed = false;
    struct Unsaved {
        QString oldest;
        qint64 count = 0;
        bool loaded = false;
    };
    Unsaved m_unsaved[2]; // text, pictures: read once per batch, then counted as copies are added
    MergeStats m_stats;
};

} // namespace sync
} // namespace clipitto
