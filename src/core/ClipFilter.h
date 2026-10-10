#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

namespace clipitto {

// Decides whether a captured clipboard text should be stored.
class ClipFilter {
public:
    enum class Verdict { Accept, Empty, TooLarge, Sensitive, OwnWrite };

    using MsClock = std::function<qint64()>; // monotonic milliseconds

    explicit ClipFilter(qint64 maxBytes, MsClock clock = {});

    Verdict check(const QString& text, const QStringList& mimeTypes) const;

    // Call right before ClipItTo itself puts text on the clipboard: the echo that the
    // capture backend reports within ownWriteWindowMs is then ignored.
    void noteOwnWrite(const QString& text);

    void setMaxBytes(qint64 maxBytes) { m_maxBytes = maxBytes; }

    // Password managers mark secrets with these MIME types (KeePassXC, KDE Klipper convention, ...).
    static bool isSensitive(const QStringList& mimeTypes);

    static constexpr qint64 ownWriteWindowMs = 500;

private:
    qint64 m_maxBytes;
    MsClock m_clock;
    QString m_ownText;
    qint64 m_ownAtMs = 0;
    bool m_hasOwn = false;
};

} // namespace clipitto
