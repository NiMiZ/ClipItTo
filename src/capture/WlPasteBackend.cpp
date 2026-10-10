#include "WlPasteBackend.h"

#include "ClipFilter.h"

#include <QLoggingCategory>
#include <QProcess>

#include <algorithm>

Q_LOGGING_CATEGORY(lcWlPaste, "clipitto.capture.wlpaste")

namespace clipitto::capture {

namespace {

constexpr int kMaxBackoffMs = 30000;

bool hasText(const QStringList& mimeTypes)
{
    static const QStringList x11TextTargets = {QStringLiteral("UTF8_STRING"), QStringLiteral("STRING"),
                                               QStringLiteral("TEXT")};
    return std::any_of(mimeTypes.begin(), mimeTypes.end(), [](const QString& type) {
        return type.startsWith(QLatin1String("text/")) || x11TextTargets.contains(type);
    });
}

} // namespace

WlPasteBackend::WlPasteBackend(QString program, QObject* parent)
    : CaptureBackend(parent)
    , m_program(std::move(program))
{
    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, &WlPasteBackend::spawnWatcher);
}

WlPasteBackend::~WlPasteBackend()
{
    stop();
}

bool WlPasteBackend::probe(const QString& program, int timeoutMs)
{
    QProcess process;
    process.start(program, {QStringLiteral("--watch"), QStringLiteral("true")});
    if (!process.waitForStarted(1000)) {
        return false; // not installed
    }
    if (process.waitForFinished(timeoutMs)) {
        return false; // exited: "Watch mode requires a compositor that supports ... data-control"
    }
    process.kill();
    process.waitForFinished(1000);
    return true;
}

void WlPasteBackend::start()
{
    m_running = true;
    m_backoffMs = 500;
    spawnWatcher();
}

void WlPasteBackend::stop()
{
    m_running = false;
    m_restart.stop();
    if (m_watcher) {
        m_watcher->disconnect(this);
        m_watcher->kill();
        m_watcher->waitForFinished(1000);
        delete m_watcher;
        m_watcher = nullptr;
    }
}

void WlPasteBackend::spawnWatcher()
{
    if (!m_running) {
        return;
    }
    delete m_watcher;
    m_watcher = new QProcess(this);
    m_watcher->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(m_watcher, &QProcess::readyReadStandardOutput, this, &WlPasteBackend::onWatcherOutput);
    connect(m_watcher, &QProcess::finished, this, &WlPasteBackend::onWatcherFinished);
    connect(m_watcher, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            onWatcherFinished();
        }
    });
    // `echo` prints one line per clipboard change (and once at startup for the current content).
    m_watcher->start(m_program, {QStringLiteral("--watch"), QStringLiteral("echo")});
}

void WlPasteBackend::onWatcherOutput()
{
    m_watcher->readAllStandardOutput(); // content is irrelevant: one line = one change
    m_backoffMs = 500;
    fetch();
}

void WlPasteBackend::onWatcherFinished()
{
    if (!m_running) {
        return;
    }
    qCWarning(lcWlPaste) << m_program << "--watch stopped; restarting in" << m_backoffMs << "ms";
    m_restart.start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, kMaxBackoffMs);
}

void WlPasteBackend::fetch()
{
    if (m_fetching) {
        m_changedWhileFetching = true; // coalesce bursts into one more fetch
        return;
    }
    m_fetching = true;
    auto* types = new QProcess(this);
    connect(types, &QProcess::finished, this, [this, types](int exitCode, QProcess::ExitStatus status) {
        types->deleteLater();
        const QStringList mimeTypes = status == QProcess::NormalExit && exitCode == 0
            ? QString::fromUtf8(types->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)
            : QStringList();
        // Never read the content of secrets, and skip images and other non-text selections.
        if (ClipFilter::isSensitive(mimeTypes) || !hasText(mimeTypes)) {
            fetchDone();
            return;
        }
        fetchText(mimeTypes);
    });
    connect(types, &QProcess::errorOccurred, this, [this, types](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            types->deleteLater();
            fetchDone();
        }
    });
    types->start(m_program, {QStringLiteral("--list-types")});
}

void WlPasteBackend::fetchText(const QStringList& mimeTypes)
{
    auto* text = new QProcess(this);
    connect(text, &QProcess::finished, this, [this, text, mimeTypes](int exitCode, QProcess::ExitStatus status) {
        text->deleteLater();
        if (m_running && status == QProcess::NormalExit && exitCode == 0) {
            emit captured(QString::fromUtf8(text->readAllStandardOutput()), mimeTypes);
        }
        fetchDone();
    });
    connect(text, &QProcess::errorOccurred, this, [this, text](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            text->deleteLater();
            fetchDone();
        }
    });
    text->start(m_program, {QStringLiteral("--no-newline"), QStringLiteral("--type"), QStringLiteral("text")});
}

void WlPasteBackend::fetchDone()
{
    m_fetching = false;
    if (m_changedWhileFetching && m_running) {
        m_changedWhileFetching = false;
        fetch();
    }
}

} // namespace clipitto::capture
