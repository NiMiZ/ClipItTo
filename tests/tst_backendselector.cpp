#include "BackendSelector.h"
#include "Config.h"
#include "SessionEnv.h"

#include <QGuiApplication>
#include <QTest>

using clipitto::BackendChoice;
using clipitto::Config;
using clipitto::SessionEnv;
using clipitto::capture::Capabilities;
using clipitto::capture::selectBackend;

class TestBackendSelector : public QObject {
    Q_OBJECT

private slots:
    void picksPerSession_data()
    {
        QTest::addColumn<int>("display");
        QTest::addColumn<int>("choice");
        QTest::addColumn<bool>("gnomeExt");
        QTest::addColumn<bool>("dataControl");
        QTest::addColumn<QString>("backend");
        QTest::addColumn<bool>("degraded");

        const int W = int(SessionEnv::Display::Wayland);
        const int X = int(SessionEnv::Display::X11);
        const int Win = int(SessionEnv::Display::Windows);
        const int U = int(SessionEnv::Display::Unknown);
        const int Auto = int(BackendChoice::Auto);
        const QString qt = QStringLiteral("qt-clipboard");
        const QString gnome = QStringLiteral("gnome-ext");
        const QString wl = QStringLiteral("wl-paste");

        QTest::newRow("x11") << X << Auto << false << false << qt << false;
        QTest::newRow("windows") << Win << Auto << false << false << qt << false;
        QTest::newRow("wayland, nothing") << W << Auto << false << false << qt << true;
        QTest::newRow("wayland, extension") << W << Auto << true << false << gnome << false;
        QTest::newRow("wayland, data-control") << W << Auto << false << true << wl << false;
        QTest::newRow("wayland, both") << W << Auto << true << true << gnome << false;
        QTest::newRow("x11 with extension") << X << Auto << true << false << qt << false;
        QTest::newRow("unknown session") << U << Auto << false << false << qt << true;
        QTest::newRow("forced gnome-ext, missing") << W << int(BackendChoice::GnomeExt) << false << false << qt << true;
        QTest::newRow("forced gnome-ext") << W << int(BackendChoice::GnomeExt) << true << false << gnome << false;
        QTest::newRow("forced wl-paste") << W << int(BackendChoice::WlPaste) << false << false << wl << false;
        QTest::newRow("forced qt on wayland") << W << int(BackendChoice::QtClipboard) << true << true << qt << true;
    }

    void picksPerSession()
    {
        QFETCH(int, display);
        QFETCH(int, choice);
        QFETCH(bool, gnomeExt);
        QFETCH(bool, dataControl);
        QFETCH(QString, backend);
        QFETCH(bool, degraded);

        Config config;
        config.backend = BackendChoice(choice);
        SessionEnv session;
        session.display = SessionEnv::Display(display);
        Capabilities caps;
        caps.gnomeExtension = gnomeExt;
        caps.wlDataControl = dataControl;

        const auto s = selectBackend(config, session, caps, QGuiApplication::clipboard());
        QCOMPARE(s.backend->name(), backend);
        QCOMPARE(s.degraded, degraded);
        QVERIFY(!s.note.isEmpty());
    }

    void degradedWaylandNoteMentionsTheExtension()
    {
        SessionEnv session;
        session.display = SessionEnv::Display::Wayland;
        const auto s = selectBackend(Config{}, session, Capabilities{}, QGuiApplication::clipboard());
        QVERIFY(s.note.contains(QLatin1String("GNOME Shell extension")));
    }
};

QTEST_MAIN(TestBackendSelector)
#include "tst_backendselector.moc"
