#pragma once

#include "BackendSelector.h"
#include "Config.h"
#include "ControlChannel.h"
#include "DesktopIntegration.h"
#include "Entry.h"
#include "SessionEnv.h"

#include <QObject>
#include <QVariantMap>

#include <memory>
#include <optional>

namespace clipitto {

class CapturePipeline;
class ClipboardWriter;

namespace ui {
class PopupWindow;
}

namespace db {
class Db;
class Repository;
} // namespace db

struct DaemonOptions {
    QString configPath;
    QString databasePath;
    SessionEnv session;
    QString wlPasteProgram = QStringLiteral("wl-paste");
};

// Owns everything that lives for the whole session: config, database, capture, the popup,
// the platform control channel (D-Bus on Linux) and the optional desktop integration
// (the GNOME Shell extension on Linux).
class Daemon : public QObject {
    Q_OBJECT

public:
    using StartResult = ControlChannel::StartResult;

    Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control,
           std::unique_ptr<DesktopIntegration> integration = nullptr, QObject* parent = nullptr);
    ~Daemon() override;

    StartResult start(QString* error = nullptr);
    void reloadConfig();
    QVariantMap status() const;
    // Adds platform details (e.g. the Windows hotkey) to status().
    void setStatusValue(const QString& key, const QVariant& value) { m_extraStatus.insert(key, value); }

    const Config& config() const { return m_config; }
    db::Repository* repository() const { return m_repo.get(); }
    ControlChannel* control() const { return m_control; }
    ui::PopupWindow* popup() const { return m_popup.get(); } // null until start() succeeded

signals:
    void configReloaded();

private:
    void startCapture();
    void loadConfig();
    void onEntryActivated(const Entry& entry);
    void onPopupVisibilityChanged(bool visible);
    bool integrationAvailable() const { return m_integration && m_integration->isAvailable(); }
    capture::Capabilities capabilities();

    DaemonOptions m_options;
    Config m_config;
    std::unique_ptr<db::Db> m_db;
    std::unique_ptr<db::Repository> m_repo;
    std::unique_ptr<CapturePipeline> m_pipeline;
    capture::Selection m_capture;
    std::unique_ptr<ClipboardWriter> m_writer;
    std::unique_ptr<ui::PopupWindow> m_popup; // declared last: destroyed first, it uses m_repo
    ControlChannel* m_control; // child of this
    DesktopIntegration* m_integration = nullptr; // child of this; may be null
    std::optional<bool> m_wlDataControl; // probed lazily, once
    QVariantMap m_extraStatus;
};

} // namespace clipitto
