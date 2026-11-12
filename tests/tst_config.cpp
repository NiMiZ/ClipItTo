#include "Config.h"
#include "Hotkey.h"
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
    void startFocus()
    {
        QVERIFY(Config{}.startFocus == clipitto::StartFocus::Pinned); // as before the setting
        QVERIFY(Config::fromToml("start_focus = \"grouped\"").startFocus == clipitto::StartFocus::Grouped);
        const Config unsaved = Config::fromToml("start_focus = \"unsaved\"");
        QVERIFY(unsaved.startFocus == clipitto::StartFocus::Unsaved);
        QVERIFY(Config::fromToml(unsaved.toToml()).startFocus == clipitto::StartFocus::Unsaved);
        QStringList warnings;
        QVERIFY(Config::fromToml("start_focus = \"top\"", &warnings).startFocus == clipitto::StartFocus::Pinned);
        QCOMPARE(warnings.size(), 1);
    }

    void debugLog()
    {
        QVERIFY(!Config().debugLog); // off by default
        QVERIFY(Config::fromToml("debug_log = true\n").debugLog);
    }

    void autoGroup()
    {
        QVERIFY(!Config().autoGroup); // off by default
        QVERIFY(Config::fromToml("auto_group = true\n").autoGroup);
        Config c;
        c.autoGroup = true;
        QVERIFY(Config::fromToml(c.toToml()).autoGroup); // saved and read back
        QVERIFY(c.toToml().find("auto_group = true") != std::string::npos);
    }

    void keepFormatting()
    {
        QVERIFY(Config().keepFormatting);
        QVERIFY(!Config::fromToml("keep_formatting = false\n").keepFormatting);
    }

    void indexPictures()
    {
        QVERIFY(Config().indexPictures);
        QVERIFY(!Config::fromToml("index_pictures = false\n").indexPictures);
        QVERIFY(Config::fromToml(Config().toToml()) == Config());
    }

    void ocrLanguages()
    {
        QStringList warnings;
        const Config c = Config::fromToml("ocr_languages = \"en, he\"\n", &warnings);
        QCOMPARE(c.ocrLanguages, QStringLiteral("en, he"));
        QVERIFY(warnings.isEmpty());
        QVERIFY(Config::fromToml(c.toToml()) == c);
        const Config bad = Config::fromToml("ocr_languages = \"en; rm -rf\"\n", &warnings);
        QVERIFY(bad.ocrLanguages.isEmpty());
        QCOMPARE(warnings.size(), 1);
    }

    void keyboardLayouts()
    {
        QVERIFY(Config().keyboardLayouts.isEmpty());
        QStringList warnings;
        const Config c = Config::fromToml("keyboard_layouts = \"he, ru\"\n", &warnings);
        QCOMPARE(c.keyboardLayouts, QStringLiteral("he, ru"));
        QVERIFY(warnings.isEmpty());
        QVERIFY(Config::fromToml(c.toToml()) == c);
        Config::fromToml("keyboard_layouts = \"he; rm\"\n", &warnings);
        QCOMPARE(warnings.size(), 1);
    }

    void translateTimeout()
    {
        QCOMPARE(Config().translateTimeout, 5);
        QCOMPARE(Config::fromToml("translate_timeout = 8\n").translateTimeout, 8);
        QCOMPARE(Config::fromToml(Config::fromToml("translate_timeout = 3\n").toToml()).translateTimeout, 3);
    }

    void pasteAsItems()
    {
        QVERIFY(Config().pasteAsText.isEmpty() && Config().pasteAsPicture.isEmpty());
        QStringList warnings;
        const Config c = Config::fromToml("paste_as_text = \"lower,-upper,translate\"\npaste_as_picture = \"pic_paste\"\n", &warnings);
        QCOMPARE(c.pasteAsText, QStringLiteral("lower,-upper,translate"));
        QCOMPARE(c.pasteAsPicture, QStringLiteral("pic_paste"));
        QVERIFY(warnings.isEmpty());
        QVERIFY(Config().pasteAsOthers);
        QVERIFY(!Config::fromToml("paste_as_others = false\n").pasteAsOthers);
        QVERIFY(Config::fromToml(c.toToml()) == c);
        Config::fromToml("paste_as_text = \"a; b\"\n", &warnings);
        QCOMPARE(warnings.size(), 1);
    }

    void passwordSettings()
    {
        const Config defaults;
        QVERIFY(defaults.keepPasswords);
        QVERIFY(defaults.passwordsInGroup);
        QVERIFY(defaults.passwordAskNote);
        QVERIFY(defaults.passwordsPerNote);
        QVERIFY(defaults.detectSecrets);
        QVERIFY(!defaults.detectPasswords);
        QVERIFY(defaults.ocrLanguages.isEmpty());
        QVERIFY(!defaults.ignoreCase);
        QCOMPARE(defaults.passwordUnlockMinutes, 5);
        QCOMPARE(defaults.passwordClearSeconds, 2);
        QCOMPARE(defaults.pasteOnceClearSeconds, 2);
        QVERIFY(!defaults.windowsHello);
        QCOMPARE(defaults.passwordSearchKeyword, QStringLiteral("p[]"));

        const Config c = Config::fromToml("keep_passwords = false\npasswords_in_group = true\n"
                                          "password_ask_note = true\npasswords_per_note = true\nignore_case = true\ndetect_secrets = false\ndetect_passwords = true\n"
                                          "password_unlock_minutes = 15\nwindows_hello = true\n"
                                          "password_search_keyword = \"pw:\"\n");
        QVERIFY(!c.keepPasswords);
        QVERIFY(c.passwordsInGroup);
        QVERIFY(c.passwordAskNote);
        QVERIFY(c.passwordsPerNote);
        QVERIFY(!c.detectSecrets);
        QVERIFY(c.detectPasswords);
        QVERIFY(c.ignoreCase);
        QCOMPARE(c.passwordUnlockMinutes, 15);
        QVERIFY(c.windowsHello);
        QCOMPARE(c.passwordSearchKeyword, QStringLiteral("pw:"));
        QVERIFY(Config::fromToml(c.toToml()) == c);

        // Rules: written as a list of strings, survive a merge into an existing file, bad ones are skipped.
        QVERIFY(defaults.passwordRules.isEmpty());
        Config withRules;
        clipitto::PasswordRule rule;
        rule.minLength = 8;
        rule.maxLength = 20;
        rule.digits = 2;
        rule.digitsAtEnd = true;
        withRules.passwordRules = {rule, clipitto::PasswordRule()};
        QVERIFY(Config::fromToml(withRules.toToml()) == withRules);
        QVERIFY(Config::fromToml(withRules.mergeInto("password_rules = []   # mine\nrecent_count = 7\n")) == withRules);
        QVERIFY(Config::fromToml(withRules.mergeInto("password_rules = [ \"1-2 digits=1\",\n")).passwordRules.size() <= 2);
        QStringList ruleWarnings;
        const Config sixRules = Config::fromToml("password_rules = [\"8-9\", \"x\", \"9-8\", \"6-9\", \"6-9\", \"6-9\", \"6-9\", \"6-9\"]",
                                                 &ruleWarnings);
        QCOMPARE(sixRules.passwordRules.size(), 5);
        QCOMPARE(ruleWarnings.size(), 3); // two malformed, one over the limit of five
        QStringList notList;
        QVERIFY(Config::fromToml("password_rules = 5", &notList).passwordRules.isEmpty());
        QCOMPARE(notList.size(), 1);

        QStringList warnings;
        const Config bad = Config::fromToml("password_search_keyword = \"p w\"\npassword_unlock_minutes = 0\n",
                                            &warnings);
        QCOMPARE(bad.passwordSearchKeyword, QStringLiteral("p[]"));
        QCOMPARE(bad.passwordUnlockMinutes, 5);
        QCOMPARE(warnings.size(), 2);
        QStringList more;
        QCOMPARE(Config::fromToml("password_search_keyword = \"\"", &more).passwordSearchKeyword, QStringLiteral("p[]"));
        QCOMPARE(more.size(), 1);
    }

    void autoForgetRules()
    {
        QVERIFY(Config{}.autoForget.isEmpty()); // nothing happens by default

        Config c;
        clipitto::AutoForgetRule age;
        age.amount = 14;
        clipitto::AutoForgetRule bearer;
        bearer.type = clipitto::AutoForgetRule::Type::Contains;
        bearer.amount = 5;
        bearer.text = QStringLiteral("Bearer ");
        clipitto::AutoForgetRule card;
        card.type = clipitto::AutoForgetRule::Type::Secret;
        card.amount = 3;
        card.secret = clipitto::AutoForgetRule::SecretKind::Card;
        c.autoForget = {age, bearer, card};
        QVERIFY(Config::fromToml(c.toToml()) == c);
        QVERIFY(Config::fromToml(c.mergeInto("auto_forget = []  # mine\nrecent_count = 7\n")) == c);

        // The file is a list of strings, one per rule.
        const Config fromFile = Config::fromToml("auto_forget = [\"age 14d\", \"match 5m contains \\\"Bearer \\\"\", \"match 3m card\"]");
        QVERIFY(fromFile.autoForget == c.autoForget);

        // Bad rules are skipped with one warning each; the warning never quotes the rule (it may hold what the
        // user wants forgotten). More than eight: the rest is dropped with a warning.
        QStringList warnings;
        const Config bad = Config::fromToml("auto_forget = [\"age 0d\", \"nonsense\", \"match 5m contains \\\"secretword\\\"x\", 7, \"age 3d\"]",
                                            &warnings);
        QCOMPARE(bad.autoForget.size(), 1);
        QCOMPARE(bad.autoForget.front().amount, 3);
        QCOMPARE(warnings.size(), 4);
        for (const QString& w : std::as_const(warnings)) {
            QVERIFY(!w.contains(QLatin1String("secretword")));
        }
        QStringList many;
        QString nine = QStringLiteral("auto_forget = [");
        for (int i = 0; i < 9; ++i) {
            nine += QStringLiteral("\"age %1d\",").arg(i + 1);
        }
        nine += QLatin1Char(']');
        QCOMPARE(Config::fromToml(nine.toStdString(), &many).autoForget.size(), 8);
        QCOMPARE(many.size(), 1);
        QStringList notList;
        QVERIFY(Config::fromToml("auto_forget = 5", &notList).autoForget.isEmpty());
        QCOMPARE(notList.size(), 1);
    }

    void pasteOnceClearSecondsRange()
    {
        QCOMPARE(Config{}.pasteOnceClearSeconds, 2);
        QCOMPARE(Config::fromToml("paste_once_clear_seconds = 0").pasteOnceClearSeconds, 0);
        QCOMPARE(Config::fromToml("paste_once_clear_seconds = 60").pasteOnceClearSeconds, 60);
        QStringList warnings;
        QCOMPARE(Config::fromToml("paste_once_clear_seconds = 61", &warnings).pasteOnceClearSeconds, 2); // out of range: default
        QVERIFY(!warnings.isEmpty());
        QCOMPARE(Config::fromToml("paste_once_clear_seconds = -1").pasteOnceClearSeconds, 2);
        // Written and read back.
        Config c;
        c.pasteOnceClearSeconds = 17;
        QCOMPARE(Config::fromToml(c.toToml()).pasteOnceClearSeconds, 17);
    }

    void passwordClearSecondsRange()
    {
        QCOMPARE(Config{}.passwordClearSeconds, 2);
        QCOMPARE(Config::fromToml("password_clear_seconds = 0").passwordClearSeconds, 0);
        QCOMPARE(Config::fromToml("password_clear_seconds = 60").passwordClearSeconds, 60);
        QStringList warnings;
        QCOMPARE(Config::fromToml("password_clear_seconds = 61", &warnings).passwordClearSeconds, 2); // out of range: default
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(Config::fromToml("password_clear_seconds = -1").passwordClearSeconds, 2);
    }

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

    void popupPositionsAndLatestAfterPaste()
    {
        QVERIFY(Config{}.latestAfterPaste);
        QVERIFY(!Config::fromToml("latest_after_paste = false").latestAfterPaste);
        QVERIFY(Config{}.popupPosition == clipitto::PopupPosition::Cursor);
        const std::pair<const char*, clipitto::PopupPosition> cases[] = {
            {"top-left", clipitto::PopupPosition::TopLeft},       {"top-right", clipitto::PopupPosition::TopRight},
            {"bottom-left", clipitto::PopupPosition::BottomLeft}, {"bottom-right", clipitto::PopupPosition::BottomRight},
            {"center", clipitto::PopupPosition::Center},          {"last", clipitto::PopupPosition::Last},
        };
        for (const auto& [name, position] : cases) {
            QStringList warnings;
            const Config c = Config::fromToml("popup_position = \"" + std::string(name) + "\"", &warnings);
            QVERIFY2(warnings.isEmpty(), name);
            QVERIFY2(c.popupPosition == position, name);
            QVERIFY(Config::fromToml(c.toToml()).popupPosition == position); // written back the same
        }
    }

    void pictureLimits()
    {
        QCOMPARE(Config{}.maxImages, 100);
        QCOMPARE(Config{}.maxImageBytes, 50LL * 1024 * 1024);
        const Config c = Config::fromToml("max_images = 0\nmax_image_bytes = 1048576");
        QCOMPARE(c.maxImages, 0);
        QCOMPARE(c.maxImageBytes, 1048576);
        QCOMPARE(Config::fromToml(c.toToml()).maxImages, 0);
        QCOMPARE(Config::fromToml("max_images = -1").maxImages, 100); // out of range: default
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
        Config expected;
#ifdef Q_OS_MACOS
        expected.hotkey = c.hotkey; // the example shows Ctrl+Alt+V; a Mac's own default is Alt+Cmd+V
        QVERIFY(clipitto::parseHotkey(c.hotkey));
#endif
        QVERIFY(c == expected);
    }

    void backendAliasIsAccepted()
    {
        QStringList warnings;
        const Config c = Config::fromToml(R"(backend = "x11")", &warnings);
        QVERIFY(warnings.isEmpty());
        QVERIFY(c.backend == BackendChoice::QtClipboard);
        QVERIFY(c.toToml().find("backend = \"qt-clipboard\"") != std::string::npos);
    }

    void savingUnchangedSettingsLeavesTheFileAsItIs()
    {
        // OK in Settings with nothing touched must not append the keys the user never set, nor rewrite values.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        const QByteArray text = "# mine\r\ntheme = \"dark\"  # dark\r\nfuture_key = 1\r\n";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(text);
        f.close();
        QVERIFY(Config::load(path).save(path));
        QFile again(path);
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), text);
        again.close();

        // A changed value still is saved (and the missing keys are appended as before).
        Config changed = Config::load(path);
        changed.recentCount = 33;
        QVERIFY(changed.save(path));
        QVERIFY(Config::load(path).recentCount == 33);

        // A wrong value is repaired by a save, not kept.
        QFile bad(path);
        QVERIFY(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bad.write("recent_count = 99999\n");
        bad.close();
        QVERIFY(Config::load(path).save(path));
        QFile fixed(path);
        QVERIFY(fixed.open(QIODevice::ReadOnly));
        QVERIFY(fixed.readAll().contains("recent_count = 20"));
        fixed.close();
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
