#include "Daemon.h"
#include "Paths.h"
#include "UnixSignals.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLoggingCategory>

#include <csignal>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("clipitto"));
    QApplication::setApplicationVersion(QStringLiteral(CLIPITTO_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.nimiz.ClipItTo")); // Wayland app_id
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ClipItTo clipboard history daemon"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption verbose(QStringLiteral("verbose"),
                                     QStringLiteral("Enable debug logging."));
    parser.addOption(verbose);
    parser.process(app);
    if (parser.isSet(verbose)) {
        QLoggingCategory::setFilterRules(QStringLiteral("clipitto.*.debug=true"));
    }

    clipitto::DaemonOptions options;
    options.configPath = clipitto::paths::configFile();
    options.databasePath = clipitto::paths::databaseFile();
    options.session = clipitto::SessionEnv::detect();

    clipitto::Daemon daemon(options);
    QString error;
    switch (daemon.start(&error)) {
    case clipitto::Daemon::StartResult::Ok:
        break;
    case clipitto::Daemon::StartResult::AlreadyRunning:
        qInfo().noquote() << "clipitto-daemon is already running:" << error;
        return 0;
    case clipitto::Daemon::StartResult::Failed:
        qCritical().noquote() << "clipitto-daemon failed to start:" << error;
        return 1;
    }

    clipitto::UnixSignals unixSignals({SIGTERM, SIGINT, SIGHUP});
    QObject::connect(&unixSignals, &clipitto::UnixSignals::received, &daemon, [&daemon](int signo) {
        if (signo == SIGHUP) {
            daemon.reloadConfig();
        } else {
            QCoreApplication::quit();
        }
    });

    return app.exec();
}
