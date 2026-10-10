#pragma once

#include "BackendSelector.h"
#include "Config.h"
#include "ControlChannel.h"
#include "SessionEnv.h"

#include <QObject>
#include <QVariantMap>

#include <memory>

namespace clipitto {

class CapturePipeline;

namespace db {
class Db;
class Repository;
} // namespace db

struct DaemonOptions {
    QString configPath;
    QString databasePath;
    SessionEnv session;
};

// Owns everything that lives for the whole session: config, database, capture and the
// platform control channel (D-Bus on Linux).
class Daemon : public QObject {
    Q_OBJECT

public:
    using StartResult = ControlChannel::StartResult;

    Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control, QObject* parent = nullptr);
    ~Daemon() override;

    StartResult start(QString* error = nullptr);
    void reloadConfig();
    QVariantMap status() const;

    const Config& config() const { return m_config; }
    db::Repository* repository() const { return m_repo.get(); }
    ControlChannel* control() const { return m_control; }

private:
    void startCapture();
    void loadConfig();

    DaemonOptions m_options;
    Config m_config;
    std::unique_ptr<db::Db> m_db;
    std::unique_ptr<db::Repository> m_repo;
    std::unique_ptr<CapturePipeline> m_pipeline;
    capture::Selection m_capture;
    ControlChannel* m_control; // child of this
};

} // namespace clipitto
