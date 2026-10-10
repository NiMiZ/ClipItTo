#include "Daemon.h"

#include "CapturePipeline.h"
#include "GnomeShortcut.h"
#include "Hotkey.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "ClipboardWriter.h"
#include "PopupWindow.h"
#include "WlPasteBackend.h"
#include "SyntheticPaste.h"
#include "Paths.h"
#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QTimer>

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

Daemon::Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control,
               std::unique_ptr<DesktopIntegration> integration, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_control(control.release())
    , m_integration(integration.release())
{
    m_control->setParent(this);
    m_control->setStatusProvider([this] { return status(); });
    connect(m_control, &ControlChannel::reloadRequested, this, &Daemon::reloadConfig);
    if (m_integration) {
        m_integration->setParent(this);
        // The extension can appear or vanish at any time: re-pick the capture backend.
        connect(m_integration, &DesktopIntegration::availabilityChanged, this, [this] {
            if (m_pipeline) {
                startCapture();
            }
        });
    }
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

    m_writer = std::make_unique<ClipboardWriter>(QGuiApplication::clipboard(), m_pipeline->filter(), m_integration);
    ui::applyTheme(m_config.theme);
    m_popup = std::make_unique<ui::PopupWindow>(*m_repo, m_config);
    connect(m_popup.get(), &ui::PopupWindow::settingsRequested, this, &Daemon::openSettings);
    connect(m_control, &ControlChannel::toggleRequested, m_popup.get(), &ui::PopupWindow::togglePopup);
    connect(m_control, &ControlChannel::showRequested, m_popup.get(), &ui::PopupWindow::showPopup);
    connect(m_control, &ControlChannel::hideRequested, m_popup.get(), &ui::PopupWindow::hidePopup);
    connect(m_popup.get(), &ui::PopupWindow::visibilityChanged, this, &Daemon::onPopupVisibilityChanged);
    connect(m_popup.get(), &ui::PopupWindow::entryActivated, this, &Daemon::onEntryActivated);
    connect(m_pipeline.get(), &CapturePipeline::stored, m_popup.get(), &ui::PopupWindow::historyChanged);
    return StartResult::Ok;
}

void Daemon::startCapture()
{
    if (m_capture.backend) {
        m_capture.backend->stop();
    }
    m_capture = capture::selectBackend(m_config, m_options.session, capabilities(), QGuiApplication::clipboard(),
                                       m_options.wlPasteProgram);
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

capture::Capabilities Daemon::capabilities()
{
    capture::Capabilities caps;
    caps.gnomeExtension = integrationAvailable();
    // Probing spawns wl-paste and can take a few hundred ms, so only do it when it could matter.
    const bool wantsWlPaste = m_options.session.display == SessionEnv::Display::Wayland
        && m_config.backend == BackendChoice::Auto && !caps.gnomeExtension;
    if (wantsWlPaste && !m_wlDataControl) {
        m_wlDataControl = capture::WlPasteBackend::probe(m_options.wlPasteProgram);
    }
    caps.wlDataControl = m_wlDataControl.value_or(false);
    return caps;
}

void Daemon::onPopupVisibilityChanged(bool visible)
{
    if (visible && integrationAvailable()) {
        // Wayland compositors won't let us take focus ourselves; the shell can.
        m_integration->activatePopup();
    }
    emit m_control->visibilityChanged(visible);
}

void Daemon::onEntryActivated(const Entry& entry)
{
    // Runs before the popup hides: without the shell integration on Wayland, setting the
    // clipboard needs our focus.
    m_writer->setText(entry.content);
    try {
        m_repo->touch(entry.id);
    } catch (const db::DbError& e) {
        qCWarning(lcDaemon) << "failed to update last_used:" << e.what();
    }

    if (m_config.pasteOnEnter) {
        // Give the previously focused window time to get focus back after the popup hides.
        QTimer::singleShot(80, this, [this] {
            if (integrationAvailable()) {
                m_integration->paste(m_config.pasteKeys);
            } else if (!pasteWithSyntheticKeys(m_config.pasteKeys)) {
                qCInfo(lcDaemon) << "paste_on_enter is not supported in this session "
                                    "(needs the GNOME Shell extension on Wayland)";
            }
        });
    }
}

void Daemon::reloadConfig()
{
    reload(false);
}

void Daemon::openSettings()
{
    if (!m_popup) {
        return;
    }
    ui::ShortcutSupport support;
    if (m_options.session.display == SessionEnv::Display::Windows) {
        support.note = tr("ClipItTo registers this shortcut itself.");
    } else if (m_options.session.isGnome) {
        support.note = tr("Saved as the GNOME keyboard shortcut for ClipItTo.");
    } else {
        support.editable = false;
        support.note = tr("Bind \"clipitto --toggle\" to a shortcut in your desktop's keyboard settings.");
    }
    ui::SettingsDialog dialog(m_config, *m_repo, support, m_popup.get());
    if (m_popup->execDialog(dialog) == QDialog::Accepted) {
        QString error;
        if (!applySettings(dialog.config(), &error)) {
            qCWarning(lcDaemon).noquote() << "saving settings failed:" << error;
        }
    } else if (dialog.groupsChanged()) {
        m_popup->refresh();
    }
}

bool Daemon::applySettings(const Config& config, QString* error)
{
    if (!config.save(m_options.configPath, error)) {
        return false;
    }
    reload(/*forceShortcut=*/true);
    return true;
}

QString Daemon::cliCommand() const
{
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("clipitto"));
    return (QFileInfo(local).isExecutable() ? local : QStringLiteral("clipitto")) + QStringLiteral(" --toggle");
}

void Daemon::applyDesktopShortcut()
{
    // Windows registers the hotkey in-process (see main.cpp, configReloaded); GNOME keeps
    // it as a custom keyboard shortcut; elsewhere the user binds `clipitto --toggle`.
    if (!m_options.session.isGnome || m_options.session.display == SessionEnv::Display::Windows) {
        return;
    }
    const auto hotkey = parseHotkey(m_config.hotkey);
    QString error;
    if (!hotkey || !applyGnomeShortcut(hotkey->gtkAccelerator, cliCommand(), &error, m_options.gsettingsProgram)) {
        qCWarning(lcDaemon).noquote() << "could not set the GNOME shortcut:" << error;
        setStatusValue(QStringLiteral("shortcut"), QStringLiteral("not set: ") + error);
    } else {
        setStatusValue(QStringLiteral("shortcut"), m_config.hotkey + QStringLiteral(" (GNOME)"));
    }
}

void Daemon::reload(bool forceShortcut)
{
    const Config previous = m_config;
    loadConfig();
    qCInfo(lcDaemon) << "configuration reloaded";
    if (!m_pipeline) {
        return;
    }
    m_pipeline->setLimits(m_config.maxHistory, m_config.maxItemBytes);
    if (m_config.theme != previous.theme) {
        ui::applyTheme(m_config.theme);
    }
    m_popup->applyConfig(m_config);
    if (forceShortcut || m_config.hotkey != previous.hotkey) {
        applyDesktopShortcut();
    }
    if (m_config.maxHistory < previous.maxHistory) {
        m_repo->prune(m_config.maxHistory);
    }
    if (m_config.backend != previous.backend || m_config.capturePrimary != previous.capturePrimary) {
        startCapture();
    }
    emit configReloaded();
}

QVariantMap Daemon::status() const
{
    QVariantMap s = m_extraStatus;
    s[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    s[QStringLiteral("session")] = displayName(m_options.session.display);
    s[QStringLiteral("gnome")] = m_options.session.isGnome;
    s[QStringLiteral("config")] = m_options.configPath;
    s[QStringLiteral("database")] = m_options.databasePath;
    s[QStringLiteral("max_history")] = m_config.maxHistory;
    s[QStringLiteral("visible")] = m_popup && m_popup->isVisible();
    s[QStringLiteral("desktop_integration")] = m_integration
        ? QStringLiteral("%1 (%2)").arg(m_integration->name(),
                                        m_integration->isAvailable() ? QStringLiteral("connected")
                                                                     : QStringLiteral("not running"))
        : QStringLiteral("none");
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
