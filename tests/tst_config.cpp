#include "Config.h"
#include "SessionEnv.h"

#include <QTemporaryDir>
#include <QTest>

using clipitto::BackendChoice;
using clipitto::ClickMode;
using clipitto::Config;
using clipitto::PasteKeys;
using clipitto::SessionEnv;

class TestConfig : public QObject {
    Q_OBJECT

private slots:
    void defaultsMatchSpec()
    {
        const Config c;
        QCOMPARE(c.maxHistory, 1000);
        QCOMPARE(c.recentCount, 20);
        QVERIFY(c.showSavedOnTop);
        QVERIFY(c.pasteOnEnter); // paste after choosing is the default
        QVERIFY(c.clickToCopy == ClickMode::Single);
        QVERIFY(c.backend == BackendChoice::Auto);
    }

    void missingFileGivesDefaultsSilently()
    {
        QStringList warnings;
        const Config c = Config::load(QStringLiteral("/nonexistent/clipitto/config.toml"), &warnings);
        QVERIFY(c == Config{});
        QVERIFY(warnings.isEmpty());
    }

    void parsesAllKeys()
    {
        QStringList warnings;
        const Config c = Config::fromToml(R"(
            max_history = 250
            recent_count = 30
            show_saved_on_top = false
            saved_cap = 5
            max_item_bytes = 4096
            capture_primary = true
            paste_on_enter = false
            paste_keys = "shift+insert"
            click_to_copy = "double"
            hide_on_focus_loss = false
            search_debounce_ms = 50
            backend = "wl-paste"
            preview_lines = 3
            theme = "dark"
            hotkey = "ctrl+`"
            favorites_limit = 20
        )", &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1String("; "))));
        QCOMPARE(c.maxHistory, 250);
        QCOMPARE(c.recentCount, 30);
        QVERIFY(!c.showSavedOnTop);
        QCOMPARE(c.savedCap, 5);
        QCOMPARE(c.maxItemBytes, 4096);
        QVERIFY(c.capturePrimary);
        QVERIFY(!c.pasteOnEnter);
        QVERIFY(c.pasteKeys == PasteKeys::ShiftInsert);
        QVERIFY(c.clickToCopy == ClickMode::Double);
        QVERIFY(!c.hideOnFocusLoss);
        QCOMPARE(c.searchDebounceMs, 50);
        QVERIFY(c.backend == BackendChoice::WlPaste);
        QCOMPARE(c.previewLines, 3);
        QVERIFY(c.theme == clipitto::Theme::Dark);
        QCOMPARE(c.hotkey, QStringLiteral("Ctrl+`")); // normalized
        QCOMPARE(c.favoritesLimit, 20);
    }

    void favoritesLimitIsOneToTwenty()
    {
        QStringList warnings;
        QCOMPARE(Config{}.favoritesLimit, 10);
        QCOMPARE(Config::fromToml("favorites_limit = 21", &warnings).favoritesLimit, 10);
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(Config::fromToml("favorites_limit = 0").favoritesLimit, 10);
        QCOMPARE(Config::fromToml("favorites_limit = 1").favoritesLimit, 1);
    }

    void invalidValuesKeepDefaultsAndWarn()
    {
        QStringList warnings;
        const Config c = Config::fromToml(R"(
            max_history = 3
            recent_count = "lots"
            paste_keys = "ctrl+shift+v"
            show_saved_on_top = 1
            colour = "blue"
        )", &warnings);
        QVERIFY(c == Config{});
        QCOMPARE(warnings.size(), 5);
    }

    void malformedTomlGivesDefaultsAndWarns()
    {
        QStringList warnings;
        const Config c = Config::fromToml("max_history = = 5", &warnings);
        QVERIFY(c == Config{});
        QCOMPARE(warnings.size(), 1);
        QVERIFY(warnings.front().contains(QLatin1String("line 1")));
    }

    void exampleConfigIsValidAndMatchesDefaults()
    {
        QStringList warnings;
        const Config c = Config::load(QStringLiteral(CLIPITTO_EXAMPLE_CONFIG), &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1String("; "))));
        QVERIFY(c == Config{});
    }

    void backendAliasIsAccepted()
    {
        QStringList warnings;
        const Config c = Config::fromToml(R"(backend = "x11")", &warnings);
        QVERIFY(warnings.isEmpty());
        QVERIFY(c.backend == BackendChoice::QtClipboard);
        QVERIFY(c.toToml().find("backend = \"qt-clipboard\"") != std::string::npos);
    }

    void saveKeepsTheUsersFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# my notes about clipitto\r\n"
                "recent_count = 20   # I like 20\r\n"
                "\r\n"
                "theme = \"light\" # was dark\r\n"
                "future_key = 1\r\n");
        f.close();

        Config c;
        c.recentCount = 42;
        c.theme = clipitto::Theme::Dark;
        QVERIFY(c.save(path));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray saved = f.readAll();
        QVERIFY(saved.startsWith("# my notes about clipitto\r\n"
                                 "recent_count = 42   # I like 20\r\n"
                                 "\r\n"
                                 "theme = \"dark\" # was dark\r\n"
                                 "future_key = 1\r\n"));
        QVERIFY(saved.contains("encrypt_history = false"));     // missing keys appended...
        QVERIFY(saved.contains("\r\nmax_history = 1000"));      // ...with the file's line endings
        QStringList warnings;
        const Config loaded = Config::load(path, &warnings);
        QCOMPARE(loaded.recentCount, 42);
        QVERIFY(loaded.theme == clipitto::Theme::Dark);
        QCOMPARE(warnings, QStringList{QStringLiteral("unknown key 'future_key'")});
    }

    void mergeKeepsABomAndStillFindsTheFirstKey()
    {
        Config c;
        c.recentCount = 7;
        const std::string merged = c.mergeInto("\xEF\xBB\xBFrecent_count = 20\n");
        QVERIFY(merged.rfind("\xEF\xBB\xBFrecent_count = 7\n", 0) == 0);
        QCOMPARE(QString::fromStdString(merged).count(QLatin1String("recent_count =")), 1); // not appended again
        QStringList warnings;
        QCOMPARE(Config::fromToml(merged, &warnings).recentCount, 7); // no duplicate key
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1String("; "))));
    }

    void mergeLeavesTablesAndQuotedHashesAlone()
    {
        Config c;
        c.hotkey = QStringLiteral("Ctrl+\\"); // backslash must be escaped in TOML
        const std::string merged = c.mergeInto("hotkey = \"Ctrl+#\" # quoted hash is part of the value\n"
                                               "[other]\ntheme = \"keep\"\n");
        QCOMPARE(QString::fromStdString(merged).section(QLatin1Char('\n'), 0, 0),
                 QStringLiteral("hotkey = \"Ctrl+\\\\\" # quoted hash is part of the value"));
        QVERIFY(merged.find("[other]\ntheme = \"keep\"") != std::string::npos);
        QStringList warnings;
        QCOMPARE(Config::fromToml(c.toToml(), &warnings).hotkey, QStringLiteral("Ctrl+\\"));
        QVERIFY(warnings.isEmpty());
    }

    void newFileHasAlignedComments()
    {
        const std::string text = Config{}.toToml();
        QVERIFY(text.find("recent_count = 20               # copies listed") != std::string::npos);
        QVERIFY(text.find("theme = \"system\"                # system | light | dark") != std::string::npos);
    }

    void saveLoadRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("nested/config.toml"));

        Config c;
        c.maxHistory = 42;
        c.pasteOnEnter = false;
        c.pasteKeys = PasteKeys::ShiftInsert;
        c.backend = BackendChoice::QtClipboard;
        c.theme = clipitto::Theme::Light;
        c.previewLines = 4;
        c.hotkey = QStringLiteral("Ctrl+`");
        c.popupPosition = clipitto::PopupPosition::Center;
        c.encryptHistory = true;
        QString error;
        QVERIFY2(c.save(path, &error), qPrintable(error));

        QStringList warnings;
        QVERIFY(Config::load(path, &warnings) == c);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1String("; "))));
    }

    void sessionDetection_data()
    {
        QTest::addColumn<QString>("sessionType");
        QTest::addColumn<QString>("desktop");
        QTest::addColumn<bool>("wayland");
        QTest::addColumn<bool>("x11");
        QTest::addColumn<int>("display");
        QTest::addColumn<bool>("gnome");

        const int W = int(SessionEnv::Display::Wayland);
        const int X = int(SessionEnv::Display::X11);
        const int U = int(SessionEnv::Display::Unknown);
        QTest::newRow("ubuntu wayland") << QStringLiteral("wayland") << QStringLiteral("ubuntu:GNOME") << true << true << W << true;
        QTest::newRow("ubuntu xorg") << QStringLiteral("x11") << QStringLiteral("ubuntu:GNOME") << false << true << X << true;
        QTest::newRow("kde wayland") << QStringLiteral("wayland") << QStringLiteral("KDE") << true << true << W << false;
        QTest::newRow("type wins over vars") << QStringLiteral("x11") << QStringLiteral("") << true << true << X << false;
        QTest::newRow("fallback wayland var") << QStringLiteral("") << QStringLiteral("") << true << true << W << false;
        QTest::newRow("fallback x11 var") << QStringLiteral("tty") << QStringLiteral("") << false << true << X << false;
        QTest::newRow("nothing") << QStringLiteral("") << QStringLiteral("") << false << false << U << false;
    }

    void sessionDetection()
    {
        QFETCH(QString, sessionType);
        QFETCH(QString, desktop);
        QFETCH(bool, wayland);
        QFETCH(bool, x11);
        QFETCH(int, display);
        QFETCH(bool, gnome);
        const SessionEnv env = SessionEnv::from(sessionType, desktop, wayland, x11);
        QCOMPARE(int(env.display), display);
        QCOMPARE(env.isGnome, gnome);
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "tst_config.moc"
