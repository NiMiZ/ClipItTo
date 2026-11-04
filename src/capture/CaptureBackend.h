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
    // Copies marked by a password manager: their text is read (and reported with the markers
    // and the source app) only while password copies are kept; otherwise only the markers.
    void setReadMarkedText(bool read) { m_readMarkedText = read; }
    bool readMarkedText() const { return m_readMarkedText; }
    // Also read the HTML flavour of a text copy and report it with the text (keep_formatting).
    void setReadRichText(bool read) { m_readRichText = read; }
    bool readRichText() const { return m_readRichText; }
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    // `html`: the copy's HTML flavour, when asked for (setReadRichText) and the source offered one; the
    // pipeline decides whether it is worth keeping.
    void captured(const QString& text, const QStringList& mimeTypes, const QString& html = QString());
    // A copied picture (when the copy has no plain text), already encoded.
    void capturedImage(const clipitto::ImageClip& image, const QStringList& mimeTypes);

private:
    qint64 m_maxImageBytes = 50LL * 1024 * 1024;
    bool m_readMarkedText = false;
    bool m_readRichText = true;
};

} // namespace clipitto::capture
