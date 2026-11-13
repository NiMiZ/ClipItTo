#include "Notify.h"
#include "AppIcon.h"
#include "Theme.h"
#include "Daemon.h"
#include "MacApp.h"
#include "Paths.h"
#include "PopupWindow.h"
#include "KeyStore.h"
#include "SystemKeyStore.h"
#include "TranslateCheck.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLoggingCategory>

#include <chrono>
#include <cstdlib>
#include <thread>

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
#include <sys/stat.h>
#endif

// The global shortcut and a tray (menu bar) icon, and a dialog for errors: no console to
// print to there.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include "GlobalHotkey.h"
#include "PanicHotkey.h"

#include <QMenu>
#include <QMessageBox>
#include <QSystemTrayIcon>

#endif

namespace {

void reportStartupError(const QString& message)
{
    qCritical().noquote() << "clipitto-daemon failed to start:" << message;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // A GUI program has no console to print to.
    QMessageBox::critical(nullptr, QStringLiteral("ClipItTo"),
                          QStringLiteral("ClipItTo could not start:\n%1").arg(message));
#endif
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef Q_OS_UNIX
    // Everything the daemon creates (history.db, -wal, -shm, config) is private to the user.
    // Windows: the profile folders it writes to are already private by ACL.
    umask(077);
#endif
    QApplication app(argc, argv);
    clipitto::ui::makeBackgroundApp(); // macOS: a menu-bar app, no Dock icon (no-op elsewhere)
    QApplication::setApplicationName(QStringLiteral("clipitto"));
    QApplication::setApplicationDisplayName(QStringLiteral("ClipItTo"));
    QApplication::setApplicationVersion(QStringLiteral(CLIPITTO_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.nimiz.ClipItTo")); // Wayland app_id
    QApplication::setWindowIcon(clipitto::ui::appIcon());
    clipitto::ui::onThemeApplied(
        [] { QApplication::setWindowIcon(clipitto::ui::appIcon(clipitto::ui::currentThemeIsDark())); });
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ClipItTo clipboard history daemon"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption verbose(QStringLiteral("verbose"), QStringLiteral("Enable debug logging."));
    parser.addOption(verbose);
    // Hidden: the translation quality check (scripts/translate-check.sh, docs/translation.md). Needs no running daemon.
    QCommandLineOption checkList(QStringLiteral("translate-check"), QStringLiteral("Translate a word list with the installed packs."),
                                 QStringLiteral("file"));
    QCommandLineOption checkPack(QStringLiteral("translate-pack"), QStringLiteral("Use only this pack, e.g. he-en."), QStringLiteral("id"));
    QCommandLineOption checkOut(QStringLiteral("translate-out"), QStringLiteral("Write the report to this file."), QStringLiteral("file"));
    for (QCommandLineOption* o : {&checkList, &checkPack, &checkOut}) {
        o->setFlags(QCommandLineOption::HiddenFromHelp);
        parser.addOption(*o);
    }
    parser.process(app);
    if (parser.isSet(checkList)) {
        return clipitto::translatecheck::runCommand(parser.value(checkList), parser.value(checkPack), parser.value(checkOut));
    }

    clipitto::DaemonOptions options;
    options.verbose = parser.isSet(verbose); // the daemon switches debug lines and the log file on (see applyDebugLog)
    options.configPath = clipitto::paths::configFile();
    options.databasePath = clipitto::paths::databaseFile();
    options.session = clipitto::SessionEnv::detect();
    // The keyring may show its own "unlock" window: the daemon says why just before (see announceKeyring()).
    auto keyringNotice = std::make_shared<std::function<void(const QString&)>>();
    options.keyStore = std::make_shared<clipitto::AnnouncingKeyStore>(
        clipitto::createSystemKeyStore(), QStringLiteral("history"), keyringNotice);
    options.matchKeyStore = std::make_shared<clipitto::AnnouncingKeyStore>(
        clipitto::createSystemKeyStore(QStringLiteral("password-match-key")), QStringLiteral("passwords"), keyringNotice);

#ifdef CLIPITTO_USE_DBUS
    clipitto::Daemon daemon(options,
                            std::make_unique<clipitto::ipc::DbusControlChannel>(QDBusConnection::sessionBus()),
                            std::make_unique<clipitto::ipc::GnomeShellIntegration>(QDBusConnection::sessionBus()));
#else
    clipitto::Daemon daemon(options, std::make_unique<clipitto::localipc::LocalControlChannel>());
#endif

    *keyringNotice = [&daemon](const QString& purpose) { daemon.announceKeyring(purpose); };

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

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Global shortcut, owned by the daemon itself on Windows and macOS.
    clipitto::GlobalHotkey hotkey;
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
    QObject::connect(&hotkey, &clipitto::GlobalHotkey::activated, daemon.popup(),
                     [&daemon] { daemon.popup()->togglePopup(); });

    // The panic key: registered only while panic_key is on and a shortcut is set (a conflict is reported like the main one).
    clipitto::GlobalHotkey panicHotkey(nullptr, 2);
    const auto registerPanicHotkey = [&] {
        const QString shown = clipitto::applyPanicHotkey(panicHotkey, daemon.config());
        if (shown.isEmpty()) {
            daemon.removeStatusValue(QStringLiteral("panic_hotkey"));
        } else {
            if (shown.startsWith(QLatin1String("unavailable"))) {
                qWarning().noquote() << "panic hotkey:" << shown;
            }
            daemon.setStatusValue(QStringLiteral("panic_hotkey"), shown);
        }
    };
    registerPanicHotkey();
    QObject::connect(&daemon, &clipitto::Daemon::configReloaded, &panicHotkey, registerPanicHotkey);
    // Queued: the wipe runs from the event loop, not from inside the native hotkey dispatch.
    QObject::connect(&panicHotkey, &clipitto::GlobalHotkey::activated, &daemon, [&daemon] { daemon.panic(); },
                     Qt::QueuedConnection);
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Tray icon (menu bar icon on macOS): the visible sign that ClipItTo runs, and the way to
    // quit it. The icon follows the theme chosen in Settings (and the desktop, for "system").
    QSystemTrayIcon tray(clipitto::ui::appIcon(clipitto::ui::currentThemeIsDark()));
    clipitto::ui::onThemeApplied([&tray] { tray.setIcon(clipitto::ui::appIcon(clipitto::ui::currentThemeIsDark())); });
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
    clipitto::setNotifyTray(&tray); // edited-text notices use the tray where the desktop has one
#endif

    // However long shutting down takes (a translation stuck in the engine cannot be stopped, and Qt waits for its thread),
    // the process ends five seconds after the quit request: an installer or an upgrade must not be kept waiting.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::_Exit(0);
        }).detach();
    });
    return app.exec();
}
