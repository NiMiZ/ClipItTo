#pragma once

#include "BackendSelector.h"
#include "Config.h"
#include "SessionEnv.h"

#include <QDBusConnection>
#include <QObject>
#include <QVariantMap>

#include <memory>

namespace clipitto {

class CapturePipeline;

namespace db {
class Db;
class Repository;
} // namespace db

namespace ipc {
class DaemonService;
}

struct DaemonOptions {
    QString configPath;
    QString databasePath;
    SessionEnv session;
    QDBusConnection bus = QDBusConnection::sessionBus();
};

// Owns everything that lives for the whole session: config, database, capture, D-Bus service.
class Daemon : public QObject {
    Q_OBJECT

public:
    enum class StartResult { Ok, AlreadyRunning, Failed };

    explicit Daemon(DaemonOptions options, QObject* parent = nullptr);
    ~Daemon() override;

    StartResult start(QString* error = nullptr);
    void reloadConfig();
    QVariantMap status() const;

    const Config& config() const { return m_config; }
    db::Repository* repository() const { return m_repo.get(); }

private:
    void startCapture();
    void loadConfig();

    DaemonOptions m_options;
    Config m_config;
    std::unique_ptr<db::Db> m_db;
    std::unique_ptr<db::Repository> m_repo;
    std::unique_ptr<CapturePipeline> m_pipeline;
    capture::Selection m_capture;
    ipc::DaemonService* m_service = nullptr;
};

} // namespace clipitto
