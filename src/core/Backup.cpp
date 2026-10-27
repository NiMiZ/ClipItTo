#include "Backup.h"

#include "ImageClip.h"
#include "PasswordKeeper.h"
#include "PasswordVault.h"
#include "db/Repository.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace clipitto::backup {

namespace {

constexpr int kVersion = 1;
const QString kFormat = QStringLiteral("clipitto-backup");
// Argon2id for the passwords' key: a bit lighter than the vault's, as it runs once per file.
const VaultKdfCost kCost{64 * 1024, 3};

QString b64(const QByteArray& data)
{
    return QString::fromLatin1(data.toBase64());
}

QByteArray unb64(const QJsonValue& value)
{
    return QByteArray::fromBase64(value.toString().toLatin1());
}

QString stamp(const QDateTime& time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime parseStamp(const QJsonValue& value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QJsonObject filing(const Entry& e)
{
    QJsonObject o;
    o[QStringLiteral("note")] = e.note;
    o[QStringLiteral("group")] = e.group;
    o[QStringLiteral("favorite")] = e.favorite;
    o[QStringLiteral("pin")] = e.pinPosition;
    o[QStringLiteral("created")] = stamp(e.createdAt);
    o[QStringLiteral("last_used")] = stamp(e.lastUsed);
    return o;
}

bool fail(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
    return false;
}

std::optional<QJsonObject> readFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QObject::tr("Can't read %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!doc.isObject() || doc.object().value(QStringLiteral("format")).toString() != kFormat) {
        fail(error, QObject::tr("This is not a ClipItTo backup file."));
        return std::nullopt;
    }
    if (doc.object().value(QStringLiteral("version")).toInt() > kVersion) {
        fail(error, QObject::tr("This backup was made by a newer ClipItTo. Update ClipItTo to open it."));
        return std::nullopt;
    }
    return doc.object();
}

} // namespace

bool exportTo(const QString& path, db::Repository& repo, const Config& config, PasswordKeeper* keeper, Parts parts,
              const QByteArray& password, QString* error)
{
    if (!parts.any()) {
        return fail(error, QObject::tr("Nothing is chosen to export."));
    }
    QJsonObject root;
    root[QStringLiteral("format")] = kFormat;
    root[QStringLiteral("version")] = kVersion;
    root[QStringLiteral("created")] = stamp(QDateTime::currentDateTimeUtc());

    if (parts.settings) {
        root[QStringLiteral("settings")] = QString::fromStdString(config.toToml());
    }
    if (parts.copies) {
        QJsonObject copies;
        QJsonArray groups;
        for (const QString& g : repo.groups()) {
            QJsonObject o;
            o[QStringLiteral("path")] = g;
            o[QStringLiteral("hide_from_recent")] = repo.groupHiddenFromRecent(g);
            groups.append(o);
        }
        copies[QStringLiteral("groups")] = groups;
        copies[QStringLiteral("favorites_hide_from_recent")] = repo.favoritesHiddenFromRecent();

        const std::vector<Entry> entries = repo.allEntries();
        const QSet<qint64> offRecent = repo.offRecentIds();
        const bool hasPasswords = std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return e.secret; });
        QByteArray key;
        QJsonObject passwords;
        QJsonArray passwordItems;
        if (hasPasswords) {
            if (!keeper || !keeper->isUnlocked() || password.isEmpty()) {
                return fail(error, QObject::tr("Password copies can only be exported while unlocked."));
            }
            const QByteArray salt = PasswordVault::randomBytes(16);
            key = PasswordVault::deriveBackupKey(password, salt, kCost);
            if (key.isEmpty()) {
                return fail(error, QObject::tr("Could not derive the encryption key."));
            }
            QJsonObject kdf;
            kdf[QStringLiteral("salt")] = b64(salt);
            kdf[QStringLiteral("blocks")] = qint64(kCost.blocks);
            kdf[QStringLiteral("passes")] = qint64(kCost.passes);
            passwords[QStringLiteral("kdf")] = kdf;
            // A known text under the key: tells a wrong password apart from a damaged file.
            passwords[QStringLiteral("check")] = b64(PasswordVault::boxWithBackupKey(key, QByteArrayLiteral("clipitto")));
        }

        QJsonArray items;
        for (const Entry& e : entries) {
            QJsonObject o = filing(e);
            o[QStringLiteral("off_recent")] = offRecent.contains(e.id);
            if (e.secret) {
                auto plain = keeper->reveal(e.id);
                if (!plain) {
                    PasswordVault::wipe(key);
                    return fail(error, QObject::tr("A password copy could not be opened."));
                }
                o[QStringLiteral("data")] = b64(PasswordVault::boxWithBackupKey(key, *plain));
                PasswordVault::wipe(*plain);
                passwordItems.append(o);
                continue;
            }
            if (e.image) {
                o[QStringLiteral("kind")] = QStringLiteral("image");
                o[QStringLiteral("pixel_hash")] = e.contentHash.mid(int(QStringLiteral("image:").size()));
                o[QStringLiteral("png")] = b64(repo.imageData(e.id));
                o[QStringLiteral("thumbnail")] = b64(e.thumbnail);
                o[QStringLiteral("width")] = e.imageWidth;
                o[QStringLiteral("height")] = e.imageHeight;
            } else {
                o[QStringLiteral("kind")] = QStringLiteral("text");
                o[QStringLiteral("content")] = e.content;
            }
            items.append(o);
        }
        PasswordVault::wipe(key);
        copies[QStringLiteral("items")] = items;
        if (hasPasswords) {
            passwords[QStringLiteral("items")] = passwordItems;
            copies[QStringLiteral("passwords")] = passwords;
        }
        root[QStringLiteral("copies")] = copies;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(error, QObject::tr("Can't write %1: %2").arg(path, file.errorString()));
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); // copies may be private
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        return fail(error, QObject::tr("Can't write %1: %2").arg(path, file.errorString()));
    }
    return true;
}

Info inspect(const QString& path)
{
    Info info;
    const auto root = readFile(path, &info.error);
    if (!root) {
        return info;
    }
    info.parts.settings = root->contains(QStringLiteral("settings"));
    if (root->contains(QStringLiteral("copies"))) {
        const QJsonObject copies = root->value(QStringLiteral("copies")).toObject();
        info.parts.copies = true;
        info.copies = int(copies.value(QStringLiteral("items")).toArray().size());
        info.groups = int(copies.value(QStringLiteral("groups")).toArray().size());
        info.passwords = int(copies.value(QStringLiteral("passwords")).toObject().value(QStringLiteral("items")).toArray().size());
    }
    info.valid = info.parts.any();
    if (!info.valid) {
        info.error = QObject::tr("The backup is empty.");
    }
    return info;
}

namespace {

// The key for the file's password copies, or empty when the password is wrong.
QByteArray passwordsKey(const QJsonObject& passwords, const QByteArray& password)
{
    const QJsonObject kdf = passwords.value(QStringLiteral("kdf")).toObject();
    const VaultKdfCost cost{quint32(kdf.value(QStringLiteral("blocks")).toInteger()),
                            quint32(kdf.value(QStringLiteral("passes")).toInteger())};
    QByteArray key = PasswordVault::deriveBackupKey(password, unb64(kdf.value(QStringLiteral("salt"))), cost);
    const auto check = PasswordVault::unboxWithBackupKey(key, unb64(passwords.value(QStringLiteral("check"))));
    if (!check || *check != QByteArrayLiteral("clipitto")) {
        PasswordVault::wipe(key);
        return {};
    }
    return key;
}

} // namespace

bool passwordOpens(const QString& path, const QByteArray& password)
{
    const auto root = readFile(path, nullptr);
    if (!root) {
        return false;
    }
    const QJsonObject passwords =
        root->value(QStringLiteral("copies")).toObject().value(QStringLiteral("passwords")).toObject();
    if (passwords.isEmpty()) {
        return true;
    }
    QByteArray key = passwordsKey(passwords, password);
    const bool ok = !key.isEmpty();
    PasswordVault::wipe(key);
    return ok;
}

std::optional<Report> importFrom(const QString& path, Parts parts, db::Repository& repo, PasswordKeeper* keeper,
                                 const QByteArray& password, QString* error)
{
    const auto root = readFile(path, error);
    if (!root) {
        return std::nullopt;
    }
    Report report;
    if (parts.settings && root->contains(QStringLiteral("settings"))) {
        report.settings = Config::fromToml(root->value(QStringLiteral("settings")).toString().toStdString());
    }
    if (!parts.copies || !root->contains(QStringLiteral("copies"))) {
        return report;
    }

    const QJsonObject copies = root->value(QStringLiteral("copies")).toObject();
    const QStringList existing = repo.groups();
    for (const QJsonValue& g : copies.value(QStringLiteral("groups")).toArray()) {
        const QString groupPath = g.toObject().value(QStringLiteral("path")).toString();
        const QString created = repo.createGroup(groupPath);
        if (created.isEmpty()) {
            continue;
        }
        if (!existing.contains(created)) {
            repo.placeGroupLast(created); // in the backup's order (the file lists them that way)
        }
        if (g.toObject().value(QStringLiteral("hide_from_recent")).toBool()) {
            repo.setGroupHiddenFromRecent(groupPath, true);
        }
    }
    if (copies.value(QStringLiteral("favorites_hide_from_recent")).toBool()) {
        repo.setFavoritesHiddenFromRecent(true);
    }

    std::vector<std::pair<int, qint64>> pins; // (position in the backup, new id)
    const auto restore = [&](qint64 id, const QJsonObject& o) {
        repo.restoreFiling(id, parseStamp(o.value(QStringLiteral("created"))), parseStamp(o.value(QStringLiteral("last_used"))),
                           o.value(QStringLiteral("note")).toString(), o.value(QStringLiteral("group")).toString(),
                           o.value(QStringLiteral("favorite")).toBool(), o.value(QStringLiteral("off_recent")).toBool());
        const int pin = o.value(QStringLiteral("pin")).toInt(-1);
        if (pin >= 0) {
            pins.emplace_back(pin, id);
        }
    };

    for (const QJsonValue& v : copies.value(QStringLiteral("items")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("kind")).toString() == QLatin1String("image")) {
            ImageClip image;
            image.png = unb64(o.value(QStringLiteral("png")));
            image.thumbnail = unb64(o.value(QStringLiteral("thumbnail")));
            image.width = o.value(QStringLiteral("width")).toInt();
            image.height = o.value(QStringLiteral("height")).toInt();
            image.pixelHash = o.value(QStringLiteral("pixel_hash")).toString();
            if (!image.isValid()) {
                continue;
            }
            if (repo.findByHash(QStringLiteral("image:") + image.pixelHash)) {
                ++report.alreadyThere;
                continue;
            }
            restore(repo.ingestImage(image).id, o);
            ++report.added;
        } else {
            const QString content = o.value(QStringLiteral("content")).toString();
            if (content.isEmpty()) {
                continue;
            }
            if (repo.findByHash(repo.textHash(content))) {
                ++report.alreadyThere;
                continue;
            }
            restore(repo.ingest(content).id, o);
            ++report.added;
        }
    }

    const QJsonObject passwords = copies.value(QStringLiteral("passwords")).toObject();
    const QJsonArray sealedItems = passwords.value(QStringLiteral("items")).toArray();
    if (!sealedItems.isEmpty()) {
        QByteArray key;
        if (keeper && keeper->isSetUp() && !password.isEmpty()) {
            key = passwordsKey(passwords, password);
        }
        if (key.isEmpty()) {
            report.passwordsSkipped = int(sealedItems.size());
        } else {
            const PasswordVault& vault = keeper->vault();
            for (const QJsonValue& v : sealedItems) {
                const QJsonObject o = v.toObject();
                auto plain = PasswordVault::unboxWithBackupKey(key, unb64(o.value(QStringLiteral("data"))));
                if (!plain) {
                    ++report.passwordsSkipped;
                    continue;
                }
                const QString match = vault.match(*plain);
                if (!match.isEmpty() && repo.hasSecret(match, o.value(QStringLiteral("note")).toString())) {
                    ++report.alreadyThere;
                } else {
                    restore(repo.ingestSecret(vault.seal(*plain), match, o.value(QStringLiteral("note")).toString(), QString()).id, o);
                    ++report.passwordsAdded;
                }
                PasswordVault::wipe(*plain);
            }
            PasswordVault::wipe(key);
        }
    }

    std::sort(pins.begin(), pins.end());
    for (const auto& [position, id] : pins) {
        Q_UNUSED(position)
        repo.setPinned(id, true); // as many as the pin limit allows, in the backup's order
    }
    return report;
}

} // namespace clipitto::backup
