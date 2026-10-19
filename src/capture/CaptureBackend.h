#pragma once

#include "ImageClip.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace clipitto::capture {

// A source of clipboard-change events. Implementations must be event-driven (no polling).
class CaptureBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;
    // Pictures whose PNG would be larger are not encoded to the end (max_image_bytes).
    void setMaxImageBytes(qint64 bytes) { m_maxImageBytes = bytes; }
    qint64 maxImageBytes() const { return m_maxImageBytes; }
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void captured(const QString& text, const QStringList& mimeTypes);
    // A copied picture (when the copy has no plain text), already encoded.
    void capturedImage(const clipitto::ImageClip& image, const QStringList& mimeTypes);

private:
    qint64 m_maxImageBytes = 50LL * 1024 * 1024;
};

} // namespace clipitto::capture
