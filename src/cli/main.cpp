// clipitto: tiny D-Bus client for the daemon, meant to be bound to a global shortcut.
// Links only QtCore + QtDBus so it starts fast.

#include "DbusNames.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QTextStream>

#include <algorithm>

namespace {

constexpr int kCallTimeoutMs = 5000; // generous: the first call may D-Bus-activate the daemon

QString activationToken()
{
    const QString wayland = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    return wayland.isEmpty() ? qEnvironmentVariable("DESKTOP_STARTUP_ID") : wayland;
}

QString printable(const QVariant& value)
{
    if (value.canConvert<QDBusVariant>()) {
        return printable(value.value<QDBusVariant>().variant());
    }
    return value.toString();
}

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
    const QCommandLineOption ingest(QStringLiteral("ingest"), QStringLiteral("Add <text> to the history."),
                                    QStringLiteral("text"));
    parser.addOptions({toggle, show, hide, status, reload, ingest});
    parser.process(app);

    const QList<QCommandLineOption> actions = {toggle, show, hide, status, reload, ingest};
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
    } else {
        method = QStringLiteral("Ingest");
        args << parser.value(ingest) << QStringList{QStringLiteral("text/plain")};
    }

    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(clipitto::ipc::kServiceName),
                                                       QString::fromLatin1(clipitto::ipc::kObjectPath),
                                                       QString::fromLatin1(clipitto::ipc::kInterface), method);
    call.setArguments(args);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, kCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        err << "clipitto: cannot reach the daemon: " << reply.errorMessage() << '\n';
        return 1;
    }

    if (method == QLatin1String("Status") && !reply.arguments().isEmpty()) {
        const QVariantMap map = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
        QTextStream out(stdout);
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            out << it.key() << ": " << printable(it.value()) << '\n';
        }
    }
    return 0;
}
