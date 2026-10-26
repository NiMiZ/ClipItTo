#pragma once

#include "ImageClip.h"

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

namespace clipitto {

// Decides whether a captured clipboard text or picture should be stored.
class ClipFilter {
public:
    enum class Verdict { Accept, Empty, TooLarge, Sensitive, OwnWrite };

    using MsClock = std::function<qint64()>; // monotonic milliseconds

    explicit ClipFilter(qint64 maxBytes, MsClock clock = {});

    // Not const: a copy of something else ends the wait for a pasted password's echo.
    Verdict check(const QString& text, const QStringList& mimeTypes);
    // A picture: password-manager markers, its encoded size (maxImageBytes), and our own
    // pasted picture coming back (matched by its pixels).
    Verdict checkImage(const ImageClip& image, const QStringList& mimeTypes) const;

    // Call right before ClipItTo puts a password on the clipboard: its echo is ignored until the
    // clipboard holds something else, however late it comes (the GNOME extension can't mark it,
    // so a late echo would otherwise pass as plain text). Also counts as noteOwnWrite().
    void noteOwnPassword(const QString& text);
    // Our pasted password is still the last thing on the clipboard (nothing else was copied).
    bool ownPasswordPending() const { return !m_ownPasswordHash.isEmpty(); }
    void forgetOwnPassword() { m_ownPasswordHash.clear(); }

    // Call right before ClipItTo itself puts text on the clipboard: every echo of it that the
    // capture backend reports within ownWriteWindowMs is then ignored (Windows may report one
    // write more than once, the last after the native reader's retries), so pasting never
    // reorders the history by itself; latest_after_paste does that explicitly.
    void noteOwnWrite(const QString& text);
    void noteOwnImage(const QString& pixelHash); // the same, for a picture

    void setMaxBytes(qint64 maxBytes) { m_maxBytes = maxBytes; }
    qint64 maxBytes() const { return m_maxBytes; }
    void setMaxImageBytes(qint64 maxBytes) { m_maxImageBytes = maxBytes; }
    qint64 maxImageBytes() const { return m_maxImageBytes; }

    // Password managers mark secrets with these MIME types (KeePassXC, KDE Klipper convention, ...).
    static bool isSensitive(const QStringList& mimeTypes);

    // Capture backends name the app a marked copy came from with this pseudo MIME type
    // ("x-clipitto-source:KeePassXC"); it becomes the password's default note.
    static QString sourceMimeType(const QString& app); // empty when `app` is
    static QString sourceApp(const QStringList& mimeTypes); // the last one; empty when not given
    static QStringList withoutSource(QStringList mimeTypes); // drops any the copying app offered

    static constexpr qint64 ownWriteWindowMs = 1000;

private:
    bool isOwnEcho(bool has, qint64 atMs) const;

    qint64 m_maxBytes;
    qint64 m_maxImageBytes = 50LL * 1024 * 1024;
    MsClock m_clock;
    QByteArray ownHash(const QString& text) const; // keyed per process: it may be a password
    QByteArray m_hashKey; // random, this process only
    QByteArray m_ownTextHash;
    QByteArray m_ownPasswordHash; // until the clipboard holds something else
    qint64 m_ownAtMs = 0;
    bool m_hasOwn = false;
    QString m_ownImageHash;
    qint64 m_ownImageAtMs = 0;
    bool m_hasOwnImage = false;
};

} // namespace clipitto
