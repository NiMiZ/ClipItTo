#include "AppIcon.h"
#include "Daemon.h"
#include "Paths.h"
#include "PopupWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLoggingCategory>

#ifdef CLIPITTO_USE_DBUS
#include "DbusControlChannel.h"
#include "GnomeShellIntegration.h"
#else
#include "LocalClient.h"
#include "LocalControlChannel.h"
#endif

#ifdef Q_OS_UNIX
#include "UnixSignals.h"

#include <csignal>
#endif

#ifdef Q_OS_WIN
#include "WindowsHotkey.h"

#include <QMenu>
#include <QMessageBox>
#include <QSystemTrayIcon>
#endif

namespace {

void reportStartupError(const QString& message)
{
    qCritical().noquote() << "clipitto-daemon failed to start:" << message;
#ifdef Q_OS_WIN
    // A GUI-subsystem program has no console to print to.
    QMessageBox::critical(nullptr, QStringLiteral("ClipItTo"),
                          QStringLiteral("ClipItTo could not start:\n%1").arg(message));
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("clipitto"));
    QApplication::setApplicationDisplayName(QStringLiteral("ClipItTo"));
    QApplication::setApplicationVersion(QStringLiteral(CLIPITTO_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.nimiz.ClipItTo")); // Wayland app_id
    QApplication::setWindowIcon(clipitto::ui::appIcon());
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ClipItTo clipboard history daemon"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption verbose(QStringLiteral("verbose"), QStringLiteral("Enable debug logging."));
    parser.addOption(verbose);
    parser.process(app);
    if (parser.isSet(verbose)) {
        QLoggingCategory::setFilterRules(QStringLiteral("clipitto.*.debug=true"));
    }

    clipitto::DaemonOptions options;
    options.configPath = clipitto::paths::configFile();
    options.databasePath = clipitto::paths::databaseFile();
    options.session = clipitto::SessionEnv::detect();

#ifdef CLIPITTO_USE_DBUS
    clipitto::Daemon daemon(options,
                            std::make_unique<clipitto::ipc::DbusControlChannel>(QDBusConnection::sessionBus()),
                            std::make_unique<clipitto::ipc::GnomeShellIntegration>(QDBusConnection::sessionBus()));
#else
    clipitto::Daemon daemon(options, std::make_unique<clipitto::localipc::LocalControlChannel>());
#endif

    QString error;
    switch (daemon.start(&error)) {
    case clipitto::Daemon::StartResult::Ok:
        break;
    case clipitto::Daemon::StartResult::AlreadyRunning:
#ifdef CLIPITTO_USE_DBUS
        qInfo().noquote() << "clipitto-daemon is already running:" << error;
#else
        // Launching the app again (Start menu, desktop shortcut) opens the running one's popup.
        clipitto::localipc::call(clipitto::localipc::defaultServerName(), {QStringLiteral("Show"), {}});
#endif
        return 0;
    case clipitto::Daemon::StartResult::Failed:
        reportStartupError(error);
        return 1;
    }
    QObject::connect(daemon.control(), &clipitto::ControlChannel::quitRequested, &app, &QCoreApplication::quit);

    // Clean shutdown (history optimize, socket cleanup): SIGTERM/SIGINT on Linux (systemd,
    // Ctrl+C); on Windows, Qt quits on session end, and the tray menu / `clipitto --quit`.
#ifdef Q_OS_UNIX
    clipitto::UnixSignals unixSignals({SIGTERM, SIGINT, SIGHUP});
    QObject::connect(&unixSignals, &clipitto::UnixSignals::received, &daemon, [&daemon](int signo) {
        if (signo == SIGHUP) {
            daemon.reloadConfig();
        } else {
            QCoreApplication::quit();
        }
    });
#endif

#ifdef Q_OS_WIN
    // Global shortcut, owned by the daemon itself on Windows.
    clipitto::WindowsHotkey hotkey;
    const auto registerHotkey = [&] {
        QString hotkeyError;
        if (hotkey.setShortcut(daemon.config().hotkey, &hotkeyError)) {
            daemon.setStatusValue(QStringLiteral("hotkey"), hotkey.shortcut());
        } else {
            qWarning().noquote() << "hotkey:" << hotkeyError;
            daemon.setStatusValue(QStringLiteral("hotkey"), QStringLiteral("unavailable: ") + hotkeyError);
        }
    };
    registerHotkey();
    QObject::connect(&daemon, &clipitto::Daemon::configReloaded, &hotkey, registerHotkey);
    QObject::connect(&hotkey, &clipitto::WindowsHotkey::activated, daemon.popup(),
                     [&daemon] { daemon.popup()->togglePopup(); });

    // Tray icon: the visible sign that ClipItTo runs, and the way to quit it.
    QSystemTrayIcon tray(clipitto::ui::appIcon());
    QMenu trayMenu;
    trayMenu.addAction(QObject::tr("Open ClipItTo"), daemon.popup(), [&daemon] { daemon.popup()->showPopup(); });
    trayMenu.addAction(QObject::tr("Reload settings"), &daemon, &clipitto::Daemon::reloadConfig);
    trayMenu.addSeparator();
    trayMenu.addAction(QObject::tr("Quit ClipItTo"), &app, &QCoreApplication::quit);
    tray.setContextMenu(&trayMenu);
    const auto updateTooltip = [&] {
        tray.setToolTip(hotkey.isRegistered() ? QStringLiteral("ClipItTo (%1)").arg(hotkey.shortcut())
                                              : QStringLiteral("ClipItTo"));
    };
    updateTooltip();
    QObject::connect(&daemon, &clipitto::Daemon::configReloaded, &tray, updateTooltip);
    QObject::connect(&tray, &QSystemTrayIcon::activated, daemon.popup(),
                     [&daemon](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger) {
                             daemon.popup()->togglePopup();
                         }
                     });
    tray.show();
#endif

    return app.exec();
}
