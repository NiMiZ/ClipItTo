#pragma once

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
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void captured(const QString& text, const QStringList& mimeTypes);
};

} // namespace clipitto::capture
