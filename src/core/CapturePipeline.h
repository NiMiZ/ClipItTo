#pragma once

#include "ClipFilter.h"

#include <QObject>

namespace clipitto {

class PasswordVault;

namespace db {
class Repository;
}

// Captured text or picture -> filter -> store (dedupe) -> prune. Shared by every capture backend.
class CapturePipeline : public QObject {
    Q_OBJECT

public:
    CapturePipeline(db::Repository& repo, int maxHistory, qint64 maxItemBytes, QObject* parent = nullptr);

    void setLimits(int maxHistory, qint64 maxItemBytes);
    // maxImages = 0 records no pictures at all.
    void setImageLimits(int maxImages, qint64 maxImageBytes);
    int maxImages() const { return m_maxImages; }
    ClipFilter& filter() { return m_filter; }

    // Copies marked by a password manager are kept as password copies, sealed with `vault`
    // (owned by the caller), when `keep` and the vault has a master password; otherwise they
    // are rejected as before. `group` is where new ones go ("" for none).
    void setPasswords(const PasswordVault* vault, bool keep, const QString& group);
    // Whether marked copies are kept now: capture backends read their text only then.
    bool keepsPasswords() const;
    // Copies that look like a secret (secretscan) are kept as password copies when passwords are kept.
    void setDetectSecrets(bool on) { m_detectSecrets = on; }

    // While paused (the database is busy elsewhere, e.g. being encrypted), accepted clips
    // wait in memory; resuming stores them in order.
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

public slots:
    void process(const QString& text, const QStringList& mimeTypes);
    void processImage(const clipitto::ImageClip& image, const QStringList& mimeTypes);

signals:
    void stored(qint64 id, bool inserted);
    void rejected(clipitto::ClipFilter::Verdict verdict);
    // A marked copy was dropped because no master password is set (with keeping turned on).
    void passwordSkipped();
    // A password copy that is new (not the same password copied again): its entry id and the
    // app it came from ("" when unknown), for "ask for a name after copying a password".
    void passwordStored(qint64 id, const QString& sourceApp);

private:
    void store(const QString& text);
    void storeImage(const ImageClip& image);
    struct SealedCopy {
        QByteArray sealed;
        QString match;
        QString note;
    };
    void storeSecret(const SealedCopy& copy);

    db::Repository& m_repo;
    ClipFilter m_filter;
    int m_maxHistory;
    int m_maxImages = 100;
    bool m_paused = false;
    QList<QString> m_waiting;
    QList<ImageClip> m_waitingImages; // a few at most: pictures are big
    QList<SealedCopy> m_waitingSecrets; // sealed already: never held in clear
    const PasswordVault* m_vault = nullptr;
    bool m_keepPasswords = false;
    bool m_detectSecrets = false;
    QString m_passwordGroup;
};

} // namespace clipitto
