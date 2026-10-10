// clipitto: tiny client for the daemon, meant to be bound to a global shortcut.
// Talks D-Bus on Linux and a local socket (named pipe) on Windows; links no GUI modules.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include <algorithm>

#ifdef CLIPITTO_USE_DBUS
#include "DbusNames.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#else
#include "LocalClient.h"

#include <QJsonArray>
#include <QJsonObject>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr int kCallTimeoutMs = 5000; // generous: the first call may start the daemon

struct Result {
    bool ok = false;
    QVariantMap status; // filled for Status
    QString error;
};

QString activationToken()
{
    const QString wayland = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    return wayland.isEmpty() ? qEnvironmentVariable("DESKTOP_STARTUP_ID") : wayland;
}

#ifdef CLIPITTO_USE_DBUS
QVariant unwrap(const QVariant& value)
{
    return value.canConvert<QDBusVariant>() ? unwrap(value.value<QDBusVariant>().variant()) : value;
}

Result send(const QString& method, const QVariantList& args)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(clipitto::ipc::kServiceName),
                                                       QString::fromLatin1(clipitto::ipc::kObjectPath),
                                                       QString::fromLatin1(clipitto::ipc::kInterface), method);
    call.setArguments(args);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, kCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return {false, {}, reply.errorMessage()};
    }
    Result result{true, {}, {}};
    if (method == QLatin1String("Status") && !reply.arguments().isEmpty()) {
        const QVariantMap map = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            result.status.insert(it.key(), unwrap(it.value()));
        }
    }
    return result;
}
#else
Result send(const QString& method, const QVariantList& args)
{
#ifdef Q_OS_WIN
    // We were just started by the user (shortcut, Start menu): pass on our right to bring
    // a window to the foreground, so the daemon's popup can take focus.
    if (method == QLatin1String("Toggle") || method == QLatin1String("Show")) {
        AllowSetForegroundWindow(ASFW_ANY);
    }
#endif
    const auto reply = clipitto::localipc::call(clipitto::localipc::defaultServerName(),
                                                {method, QJsonArray::fromVariantList(args)}, kCallTimeoutMs);
    if (!reply.ok) {
        return {false, {}, reply.error};
    }
    return {true, reply.result.toObject().toVariantMap(), {}};
}
#endif

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("clipitto"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CLIPITTO_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Control the ClipItTo clipboard manager."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption toggle(QStringLiteral("toggle"), QStringLiteral("Show or hide the popup."));
    const QCommandLineOption show(QStringLiteral("show"), QStringLiteral("Show the popup."));
    const QCommandLineOption hide(QStringLiteral("hide"), QStringLiteral("Hide the popup."));
    const QCommandLineOption status(QStringLiteral("status"), QStringLiteral("Print daemon status."));
    const QCommandLineOption reload(QStringLiteral("reload"), QStringLiteral("Reload the configuration file."));
    const QCommandLineOption quit(QStringLiteral("quit"), QStringLiteral("Stop the daemon."));
    const QCommandLineOption ingest(QStringLiteral("ingest"), QStringLiteral("Add <text> to the history."),
                                    QStringLiteral("text"));
    const QList<QCommandLineOption> actions = {toggle, show, hide, status, reload, quit, ingest};
    parser.addOptions(actions);
    parser.process(app);

    const auto chosen = std::count_if(actions.begin(), actions.end(),
                                      [&](const QCommandLineOption& o) { return parser.isSet(o); });
    QTextStream err(stderr);
    if (chosen != 1) {
        err << "clipitto: specify exactly one action (see --help)\n";
        return 2;
    }

    QString method;
    QVariantList args;
    if (parser.isSet(toggle)) {
        method = QStringLiteral("Toggle");
        args << activationToken();
    } else if (parser.isSet(show)) {
        method = QStringLiteral("Show");
        args << activationToken();
    } else if (parser.isSet(hide)) {
        method = QStringLiteral("Hide");
    } else if (parser.isSet(status)) {
        method = QStringLiteral("Status");
    } else if (parser.isSet(reload)) {
        method = QStringLiteral("ReloadConfig");
    } else if (parser.isSet(quit)) {
        method = QStringLiteral("Quit");
    } else {
        method = QStringLiteral("Ingest");
        args << parser.value(ingest) << QStringList{QStringLiteral("text/plain")};
    }

    const Result result = send(method, args);
    if (!result.ok) {
        err << "clipitto: cannot reach the daemon: " << result.error << '\n';
        return 1;
    }
    QTextStream out(stdout);
    for (auto it = result.status.constBegin(); it != result.status.constEnd(); ++it) {
        out << it.key() << ": " << it.value().toString() << '\n';
    }
    return 0;
}
