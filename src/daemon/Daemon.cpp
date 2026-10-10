#include "Daemon.h"

#include "CapturePipeline.h"
#include "Paths.h"
#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QGuiApplication>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcDaemon, "clipitto.daemon")

namespace clipitto {

namespace {

QString displayName(SessionEnv::Display display)
{
    switch (display) {
    case SessionEnv::Display::Wayland:
        return QStringLiteral("wayland");
    case SessionEnv::Display::X11:
        return QStringLiteral("x11");
    case SessionEnv::Display::Windows:
        return QStringLiteral("windows");
    case SessionEnv::Display::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

} // namespace

Daemon::Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_control(control.release())
{
    m_control->setParent(this);
    m_control->setStatusProvider([this] { return status(); });

    // The popup arrives with the UI layer; until then these requests are only logged.
    connect(m_control, &ControlChannel::toggleRequested, this,
            [](const QString& token) { qCInfo(lcDaemon) << "toggle requested, token:" << !token.isEmpty(); });
    connect(m_control, &ControlChannel::showRequested, this,
            [](const QString& token) { qCInfo(lcDaemon) << "show requested, token:" << !token.isEmpty(); });
    connect(m_control, &ControlChannel::hideRequested, this, [] { qCInfo(lcDaemon) << "hide requested"; });
    connect(m_control, &ControlChannel::reloadRequested, this, &Daemon::reloadConfig);
}

Daemon::~Daemon()
{
    if (m_repo) {
        try {
            m_repo->optimize();
        } catch (const db::DbError& e) {
            qCWarning(lcDaemon) << "optimize on shutdown failed:" << e.what();
        }
    }
}

void Daemon::loadConfig()
{
    QStringList warnings;
    m_config = Config::load(m_options.configPath, &warnings);
    for (const QString& w : warnings) {
        qCWarning(lcDaemon).noquote() << m_options.configPath + QStringLiteral(": ") + w;
    }
}

Daemon::StartResult Daemon::start(QString* error)
{
    // Claim single-instance ownership first: a second instance must not touch the database.
    const StartResult controlResult = m_control->start(error);
    if (controlResult != StartResult::Ok) {
        return controlResult;
    }

    loadConfig();

    try {
        if (!paths::ensureParentDir(m_options.databasePath)) {
            throw db::DbError("cannot create directory for " + m_options.databasePath.toStdString(), 0);
        }
        m_db = std::make_unique<db::Db>(m_options.databasePath);
        db::configureConnection(*m_db);
        db::migrate(*m_db);
        m_repo = std::make_unique<db::Repository>(*m_db);
        m_repo->prune(m_config.maxHistory); // the limit may have been lowered since last run
    } catch (const db::DbError& e) {
        m_repo.reset();
        m_db.reset();
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return StartResult::Failed;
    }

    m_pipeline = std::make_unique<CapturePipeline>(*m_repo, m_config.maxHistory, m_config.maxItemBytes);
    connect(m_control, &ControlChannel::ingestRequested, m_pipeline.get(), &CapturePipeline::process);
    startCapture();
    return StartResult::Ok;
}

void Daemon::startCapture()
{
    if (m_capture.backend) {
        m_capture.backend->stop();
    }
    m_capture = capture::selectBackend(m_config, m_options.session, QGuiApplication::clipboard());
    connect(m_capture.backend.get(), &capture::CaptureBackend::captured, m_pipeline.get(),
            &CapturePipeline::process);
    m_capture.backend->start();

    if (m_capture.degraded) {
        qCWarning(lcDaemon).noquote() << "capture backend" << m_capture.backend->name() << "(degraded):"
                                      << m_capture.note;
    } else {
        qCInfo(lcDaemon).noquote() << "capture backend" << m_capture.backend->name() << "-" << m_capture.note;
    }
}

void Daemon::reloadConfig()
{
    const Config previous = m_config;
    loadConfig();
    qCInfo(lcDaemon) << "configuration reloaded";
    if (!m_pipeline) {
        return;
    }
    m_pipeline->setLimits(m_config.maxHistory, m_config.maxItemBytes);
    if (m_config.maxHistory < previous.maxHistory) {
        m_repo->prune(m_config.maxHistory);
    }
    if (m_config.backend != previous.backend || m_config.capturePrimary != previous.capturePrimary) {
        startCapture();
    }
}

QVariantMap Daemon::status() const
{
    QVariantMap s;
    s[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    s[QStringLiteral("session")] = displayName(m_options.session.display);
    s[QStringLiteral("gnome")] = m_options.session.isGnome;
    s[QStringLiteral("config")] = m_options.configPath;
    s[QStringLiteral("database")] = m_options.databasePath;
    s[QStringLiteral("max_history")] = m_config.maxHistory;
    if (m_capture.backend) {
        s[QStringLiteral("backend")] = m_capture.backend->name();
        s[QStringLiteral("backend_degraded")] = m_capture.degraded;
        s[QStringLiteral("backend_note")] = m_capture.note;
    }
    if (m_repo) {
        try {
            s[QStringLiteral("items")] = m_repo->count();
            s[QStringLiteral("unsaved_items")] = m_repo->countUnsaved();
        } catch (const db::DbError& e) {
            s[QStringLiteral("error")] = QString::fromUtf8(e.what());
        }
    }
    return s;
}

} // namespace clipitto
