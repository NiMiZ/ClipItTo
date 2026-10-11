#pragma once

#include "CaptureBackend.h"

#include <QTimer>

class QProcess;

namespace clipitto::capture {

// Capture via wl-clipboard on compositors that implement the wlr/ext data-control protocol
// (wlroots-based, KDE). `wl-paste --watch` blocks in the compositor's event stream and
// prints a line per change; each change then fetches the MIME types and, unless they mark a
// secret or carry no text, the text itself.
class WlPasteBackend : public CaptureBackend {
    Q_OBJECT

public:
    explicit WlPasteBackend(QString program = QStringLiteral("wl-paste"), QObject* parent = nullptr);
    ~WlPasteBackend() override;

    QString name() const override { return QStringLiteral("wl-paste"); }
    void start() override;
    void stop() override;

    // True if `program --watch` keeps running, i.e. the compositor supports data-control.
    // Without it wl-paste exits immediately with an error. Blocks for up to timeoutMs.
    static bool probe(const QString& program = QStringLiteral("wl-paste"), int timeoutMs = 300);
    // The --type to request: a UTF-8 text type when one is offered, else wl-paste's "text".
    static QString textType(const QStringList& mimeTypes);

private:
    void spawnWatcher();
    void onWatcherOutput();
    void onWatcherFinished();
    void fetch();
    void fetchText(const QStringList& mimeTypes);
    void fetchDone();

    QString m_program;
    QProcess* m_watcher = nullptr;
    QTimer m_restart;
    int m_backoffMs = 500;
    bool m_running = false;
    bool m_fetching = false;
    bool m_changedWhileFetching = false;
};

} // namespace clipitto::capture
