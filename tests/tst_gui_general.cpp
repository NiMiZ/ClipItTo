#include "Config.h"
#include "Daemon.h"
#include "FakeKeyStore.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PasswordKeeper.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "TestDb.h"
#include "ControlChannel.h"
#include "ImageClip.h"
#include "db/Repository.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QBuffer>
#include <QImage>
#include <QMenu>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <functional>
#include <QSet>
#include <map>
#include <set>

using clipitto::Config;
using clipitto::Entry;
using clipitto::Theme;
using clipitto::ui::HistoryModel;
using clipitto::ui::PopupWindow;
using clipitto::ui::SettingsDialog;

#ifdef Q_OS_MACOS
#define CLIPITTO_DEFAULT_HOTKEY "Alt+Cmd+V"
#else
#define CLIPITTO_DEFAULT_HOTKEY "Ctrl+Alt+V"
#endif

namespace {

// Stands in for the platform channel (the daemon wants one to start).
class QuietControl : public clipitto::ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

// The lines of a config file as key -> "value" (comments dropped).
std::map<QString, QString> keyValues(const std::string& toml)
{
    static const QRegularExpression line(QStringLiteral("^([a-z_]+) = (.*?)(\\s+#.*)?$"));
    std::map<QString, QString> out;
    for (const QString& l : QString::fromStdString(toml).split(QLatin1Char('\n'))) {
        const auto m = line.match(l);
        if (m.hasMatch()) {
            out[m.captured(1)] = m.captured(2);
        }
    }
    return out;
}

// The keys whose values differ between two settings.
QStringList changedKeys(const Config& a, const Config& b)
{
    const auto x = keyValues(a.toToml());
    const auto y = keyValues(b.toToml());
    QStringList out;
    for (const auto& [key, value] : x) {
        if (!y.count(key) || y.at(key) != value) {
            out << key;
        }
    }
    return out;
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = f.readAll();
    f.close(); // Windows: no open handle left behind
    return bytes;
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(bytes);
        f.close();
    }
}

// Every top-level row of the list as text: "#NAME" for a header, "@Groups" for the Groups row, else the copy.
QStringList rowsOf(const HistoryModel* model)
{
    QStringList rows;
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex i = model->index(r, 0);
        switch (model->kindAt(i)) {
        case HistoryModel::Kind::Header:
            rows << QLatin1Char('#') + i.data(HistoryModel::TitleRole).toString();
            break;
        case HistoryModel::Kind::GroupsRoot:
            rows << QStringLiteral("@Groups");
            break;
        case HistoryModel::Kind::Entry:
            rows << model->entryAt(i)->content;
            break;
        default:
            rows << QStringLiteral("?");
        }
    }
    return rows;
}

// Settings dialog with everything switched on: a master password is set (so the password controls are
// enabled) and the history can be encrypted.
struct DialogEnv {
    TestDb t;
    std::shared_ptr<FakeKeyStore> keys = std::make_shared<FakeKeyStore>();
    clipitto::PasswordKeeper keeper{*t.repo, keys};

    explicit DialogEnv(bool masterPassword = true)
    {
        keeper.setKdfCost({64, 1});
        if (masterPassword) {
            bool done = false;
            keeper.setMasterPassword("master", [&done](bool) { done = true; });
            if (!QTest::qWaitFor([&done] { return done; })) {
                qFatal("the master password was not set");
            }
        }
    }

    std::unique_ptr<SettingsDialog> open(const Config& config, bool encryptionAvailable = true)
    {
        clipitto::ui::PasswordSupport passwords;
        passwords.keeper = &keeper;
        clipitto::ui::EncryptionSupport encryption;
        encryption.available = encryptionAvailable;
        encryption.reason = QStringLiteral("no keyring here");
        encryption.active = config.encryptHistory && encryptionAvailable;
        auto d = std::make_unique<SettingsDialog>(config, *t.repo, clipitto::ui::ShortcutSupport{}, encryption, nullptr,
                                                  passwords);
        d->setNotify([](QWidget*, const QString&) {});
        d->setInform([](QWidget*, const QString&) {});
        return d;
    }
};

QCheckBox* checkBoxWithText(QWidget* parent, const QString& start)
{
    for (QCheckBox* box : parent->findChildren<QCheckBox*>()) {
        if (box->text().startsWith(start)) {
            return box;
        }
    }
    return nullptr;
}

void toggle(QCheckBox* box)
{
    box->setChecked(!box->isChecked());
}

// What each Settings control does to the file. Changing the control must change exactly this key.
const std::map<QString, std::function<void(SettingsDialog&)>>& controlChanges()
{
    static const std::map<QString, std::function<void(SettingsDialog&)>> changes = {
        {"theme", [](SettingsDialog& d) { d.themeBox()->setCurrentIndex(d.themeBox()->findData(int(Theme::Dark))); }},
        {"recent_count", [](SettingsDialog& d) { d.copiesBox()->setValue(37); }},
        {"max_history", [](SettingsDialog& d) { d.keptBox()->setValue(555); }},
        {"max_images", [](SettingsDialog& d) { d.imagesBox()->setValue(7); }},
        {"ocr_languages", [](SettingsDialog& d) { d.ocrLanguagesEdit()->setText(QStringLiteral("en, he")); }},
        {"index_pictures", [](SettingsDialog& d) { toggle(d.indexPicturesBox()); }},
        {"keep_formatting", [](SettingsDialog& d) { toggle(d.keepFormattingBox()); }},
        {"auto_group", [](SettingsDialog& d) { toggle(d.autoGroupBox()); }},
        {"debug_log", [](SettingsDialog& d) { toggle(d.debugLogBox()); }},
        {"keyboard_layouts", [](SettingsDialog& d) { toggle(d.layoutBox(QStringLiteral("he"))); }},
        {"paste_as_text",
         [](SettingsDialog& d) { d.pasteAsEditor(false).list->item(0)->setCheckState(Qt::Unchecked); }},
        {"paste_as_picture",
         [](SettingsDialog& d) { d.pasteAsEditor(true).list->item(0)->setCheckState(Qt::Unchecked); }},
        {"paste_as_others", [](SettingsDialog& d) { toggle(checkBoxWithText(&d, QStringLiteral("Show the other items"))); }},
        {"preview_lines", [](SettingsDialog& d) { d.linesBox()->setValue(3); }},
        {"favorites_limit", [](SettingsDialog& d) { d.favoritesBox()->setValue(12); }},
        {"hotkey",
         [](SettingsDialog& d) { d.shortcutEdit()->setKeySequence(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F9)); }},
        {"popup_position",
         [](SettingsDialog& d) {
             d.positionBox()->setCurrentIndex(d.positionBox()->findData(int(clipitto::PopupPosition::Center)));
         }},
        {"start_focus",
         [](SettingsDialog& d) {
             d.startFocusBox()->setCurrentIndex(d.startFocusBox()->findData(int(clipitto::StartFocus::Unsaved)));
         }},
        {"ignore_case", [](SettingsDialog& d) { toggle(d.ignoreCaseBox()); }},
        {"encrypt_history", [](SettingsDialog& d) { toggle(d.encryptBox()); }},
        {"paste_on_enter", [](SettingsDialog& d) { toggle(d.pasteBox()); }},
        {"latest_after_paste", [](SettingsDialog& d) { toggle(d.latestBox()); }},
        {"keep_passwords", [](SettingsDialog& d) { toggle(d.keepPasswordsBox()); }},
        {"passwords_in_group", [](SettingsDialog& d) { toggle(d.passwordsInGroupBox()); }},
        {"detect_secrets", [](SettingsDialog& d) { toggle(d.detectSecretsBox()); }},
        {"detect_passwords", [](SettingsDialog& d) { toggle(d.detectPasswordsBox()); }},
        {"password_rules", [](SettingsDialog& d) { d.addRuleButton()->click(); }},
        {"auto_forget", [](SettingsDialog& d) { d.addForgetButton()->click(); }},
        {"passwords_per_note", [](SettingsDialog& d) { toggle(d.passwordsPerNoteBox()); }},
        {"password_ask_note", [](SettingsDialog& d) { toggle(d.askNoteBox()); }},
        {"password_unlock_minutes", [](SettingsDialog& d) { d.unlockMinutesBox()->setValue(17); }},
        {"password_clear_seconds", [](SettingsDialog& d) { d.clearSecondsBox()->setValue(12); }},
        {"paste_once_clear_seconds", [](SettingsDialog& d) { d.pasteOnceClearSecondsBox()->setValue(9); }},
        {"password_search_keyword", [](SettingsDialog& d) { d.keywordEdit()->setText(QStringLiteral("pw[]")); }},
        {"lock_on_screen_lock", [](SettingsDialog& d) { toggle(d.lockOnScreenLockBox()); }},
    };
    return changes;
}

// Keys that have no Settings control: only the config file sets them. They must survive a trip through the
// dialog untouched. (windows_hello is set by the unlock-with-Windows-Hello switch, which needs the system's
// support, not by a plain control.)
const std::set<QString>& fileOnlyKeys()
{
    static const std::set<QString> keys = {
        "saved_cap",        "show_saved_on_top", "max_item_bytes",   "max_image_bytes",    "capture_primary",
        "paste_keys",       "click_to_copy",     "hide_on_focus_loss", "search_debounce_ms", "backend",
        "translate_timeout", "windows_hello",
    };
    return keys;
}

// A config where every file-only key has a non-default value.
Config fileOnlyConfig()
{
    Config c;
    c.savedCap = 5;
    c.showSavedOnTop = false;
    c.maxItemBytes = 4096;
    c.maxImageBytes = 1'000'000;
    c.capturePrimary = true;
    c.pasteKeys = clipitto::PasteKeys::ShiftInsert;
    c.clickToCopy = clipitto::ClickMode::Double;
    c.hideOnFocusLoss = false;
    c.searchDebounceMs = 250;
    c.backend = clipitto::BackendChoice::QtClipboard;
    c.translateTimeout = 8;
    return c;
}

// A running daemon over temporary files, and the Settings dialog as the popup's settings button opens it.
struct DaemonEnv {
    QTemporaryDir dir;
    clipitto::DaemonOptions options;
    QStringList messages; // before the daemon: the popup's hook points here until the daemon is gone
    std::unique_ptr<clipitto::Daemon> daemon;

    explicit DaemonEnv(const QByteArray& configFile = {}, bool writeConfig = false)
    {
        options.configPath = dir.filePath(QStringLiteral("config.toml"));
        options.databasePath = dir.filePath(QStringLiteral("data/history.db"));
        options.syntheticPaste = [](clipitto::PasteKeys) { return false; };
        options.debugLogPath = dir.filePath(QStringLiteral("debug.log"));
        if (writeConfig) {
            writeFile(options.configPath, configFile);
        }
        daemon = std::make_unique<clipitto::Daemon>(options, std::make_unique<QuietControl>());
        QString error;
        if (daemon->start(&error) != clipitto::Daemon::StartResult::Ok) {
            qFatal("daemon did not start: %s", qPrintable(error));
        }
        daemon->popup()->setNotify([this](QWidget*, const QString& m) { messages << m; });
    }

    // Changes settings the way the user does: in the dialog, then OK (which saves the file and applies it).
    bool change(const std::function<void(SettingsDialog&)>& edit)
    {
        clipitto::ui::PasswordSupport passwords;
        passwords.keeper = daemon->passwords();
        SettingsDialog d(daemon->config(), *daemon->repository(), clipitto::ui::ShortcutSupport{}, {}, nullptr, passwords);
        d.setNotify([](QWidget*, const QString&) {});
        edit(d);
        d.accept();
        QString error;
        const bool ok = daemon->applySettings(d.config(), &error);
        if (!ok) {
            qWarning("%s", qPrintable(error));
        }
        return ok;
    }
};

} // namespace

// The GUI as a whole: a fresh install, every Settings tab, the keyboard, rendering and reopening, with the
// settings changed and not only the defaults. Offscreen platform; prompts are injected, never real.
class TestGuiGeneral : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    // The popup the user sees with `config`, over a database with `copies` copies (oldest first).
    struct Env {
        TestDb t;
        std::unique_ptr<PopupWindow> popup;
        explicit Env(Config cfg = {}, const QStringList& copies = {})
        {
            for (const QString& c : copies) {
                t.repo->ingest(c);
            }
            cfg.hideOnFocusLoss = false;
            popup = std::make_unique<PopupWindow>(*t.repo, cfg);
        }
        bool show()
        {
            popup->showPopup();
            return QTest::qWaitForWindowActive(popup.get());
        }
        HistoryModel* model() const { return popup->model(); }
    };

private slots:
    // ===== Fresh install ======================================================================

    void anEmptyOrMissingConfigMeansTheDefaults_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<bool>("exists");
        QTest::newRow("no file") << QByteArray() << false;
        QTest::newRow("empty file") << QByteArray() << true;
        QTest::newRow("only comments") << QByteArray("# nothing set yet\n# theme = \"dark\"\n") << true;
        QTest::newRow("blank lines and a BOM") << QByteArray("\xEF\xBB\xBF\n\n   \n") << true;
    }

    void anEmptyOrMissingConfigMeansTheDefaults()
    {
        QFETCH(QByteArray, content);
        QFETCH(bool, exists);
        const QString file = path(QStringLiteral("fresh-%1.toml").arg(QTest::currentDataTag()).replace(' ', '_'));
        if (exists) {
            writeFile(file, content);
        }
        QStringList warnings;
        const Config c = Config::load(file, &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1String("; "))));
        QVERIFY(c == Config{});
    }

    void eachDefaultIsWhatItsLabelSays_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("expected");
        const std::pair<const char*, const char*> rows[] = {
            {"recent_count", "20"},       {"preview_lines", "1"},        {"max_history", "1000"},
            {"max_images", "100"},        {"favorites_limit", "10"},     {"saved_cap", "15"},
            {"show_saved_on_top", "true"}, {"paste_on_enter", "true"},   {"latest_after_paste", "true"},
            {"paste_keys", "\"ctrl+v\""}, {"click_to_copy", "\"single\""}, {"hide_on_focus_loss", "true"},
            {"search_debounce_ms", "100"}, {"hotkey", "\"" CLIPITTO_DEFAULT_HOTKEY "\""},
            {"theme", "\"system\""},      {"popup_position", "\"cursor\""}, {"start_focus", "\"pinned\""},
            {"ignore_case", "false"},     {"encrypt_history", "false"},  {"keep_formatting", "true"},
            {"detect_secrets", "true"},   {"detect_passwords", "false"}, {"auto_group", "false"},
            {"debug_log", "false"},       {"translate_timeout", "5"},    {"capture_primary", "false"},
            {"index_pictures", "true"},   {"keep_passwords", "true"},    {"password_unlock_minutes", "5"},
            {"password_clear_seconds", "2"}, {"paste_once_clear_seconds", "2"}, {"password_search_keyword", "\"p[]\""}, {"backend", "\"auto\""},
            {"lock_on_screen_lock", "true"},
        };
        for (const auto& [key, expected] : rows) {
            QTest::newRow(key) << QString::fromLatin1(key) << QString::fromLatin1(expected);
        }
    }

    void eachDefaultIsWhatItsLabelSays()
    {
        QFETCH(QString, key);
        QFETCH(QString, expected);
        const auto values = keyValues(Config{}.toToml());
        QVERIFY2(values.count(key), qPrintable(key));
        QCOMPARE(values.at(key), expected);
    }

    void aFreshSettingsDialogShowsTheDefaults()
    {
        DialogEnv env(false);
        auto d = env.open(Config{});
        QCOMPARE(d->themeBox()->currentData().toInt(), int(Theme::System));
        QCOMPARE(d->copiesBox()->value(), 20);
        QCOMPARE(d->linesBox()->value(), 1);
        QCOMPARE(d->keptBox()->value(), 1000);
        QCOMPARE(d->imagesBox()->value(), 100);
        QCOMPARE(d->favoritesBox()->value(), 10);
        QCOMPARE(d->positionBox()->currentData().toInt(), int(clipitto::PopupPosition::Cursor));
        QCOMPARE(d->startFocusBox()->currentData().toInt(), int(clipitto::StartFocus::Pinned));
        QVERIFY(d->pasteBox()->isChecked());
        QVERIFY(d->latestBox()->isChecked());
        QVERIFY(d->keepFormattingBox()->isChecked());
        QVERIFY(d->detectSecretsBox()->isChecked());
        QVERIFY(!d->detectPasswordsBox()->isChecked());
        QVERIFY(!d->autoGroupBox()->isChecked());
        QVERIFY(!d->debugLogBox()->isChecked());
        QVERIFY(!d->ignoreCaseBox()->isChecked());
        QVERIFY(!d->encryptBox()->isChecked());
        // The same keys, whatever order the platform spells the modifiers in ("ctrl+alt+v" or "alt+ctrl+v").
        const auto keys = [](QString text) {
            QStringList parts = text.toLower().replace(QLatin1String("meta"), QLatin1String("ctrl"))
                                    .replace(QLatin1String("cmd"), QLatin1String("ctrl")).split(QLatin1Char('+'));
            parts.sort();
            return parts;
        };
        QCOMPARE(keys(d->shortcutEdit()->keySequence().toString(QKeySequence::PortableText)),
                 keys(QStringLiteral(CLIPITTO_DEFAULT_HOTKEY)));
        // Looking and leaving changes nothing, file-only settings included.
        QVERIFY(d->config() == Config{});
    }

    void aFreshDaemonStartsWithTheDefaultsAndAPopup()
    {
        // The whole start-up with no config file and no history: nothing is written, the popup is built with the
        // defaults and opens with an empty list.
        QTemporaryDir dir;
        clipitto::DaemonOptions o;
        o.configPath = dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = dir.filePath(QStringLiteral("data/history.db"));
        o.syntheticPaste = [](clipitto::PasteKeys) { return false; };
        {
            clipitto::Daemon d(o, std::make_unique<QuietControl>());
            QString error;
            QVERIFY2(d.start(&error) == clipitto::Daemon::StartResult::Ok, qPrintable(error));
            QVERIFY(d.config() == Config{});
            QVERIFY(!QFile::exists(o.configPath)); // reading the defaults does not create the file
            QVERIFY(d.popup());
            d.popup()->showPopup();
            QVERIFY(QTest::qWaitForWindowActive(d.popup()));
            QCOMPARE(QApplication::focusWidget(), d.popup()->searchBar());
            QVERIFY(d.popup()->searchBar()->text().isEmpty());
            // Only the collapsed Groups row (Favorites and the default groups inside it); no copies yet.
            QCOMPARE(rowsOf(d.popup()->model()), QStringList{QStringLiteral("@Groups")});
            QVERIFY(!d.popup()->view()->isExpanded(d.popup()->model()->groupsRoot()));
            const QModelIndex root = d.popup()->model()->groupsRoot();
            QStringList inside;
            for (int r = 0; r < d.popup()->model()->rowCount(root); ++r) {
                inside << d.popup()->model()->index(r, 0, root).data(HistoryModel::PathRole).toString();
            }
            QCOMPARE(inside, (QStringList{HistoryModel::favoritesPath(), QStringLiteral("Passwords")}));
            d.popup()->hidePopup();
        }
    }

    void theFirstCopiesAreListedUnderRecentWithoutPinnedOrGroups()
    {
        Env f({}, {QStringLiteral("alpha"), QStringLiteral("beta")});
        QVERIFY(f.show());
        const QStringList rows = rowsOf(f.model());
        QVERIFY(!rows.contains(QStringLiteral("@Groups"))); // no groups and no favorites yet
        QVERIFY(rows.indexOf(QStringLiteral("beta")) < rows.indexOf(QStringLiteral("alpha"))); // newest first
        QVERIFY(!rows.filter(QStringLiteral("#")).join(QString()).contains(QLatin1String("PINNED"), Qt::CaseInsensitive));
    }

    void theFirstPopupListsTwentyCopiesLikeTheLabelSays()
    {
        QStringList copies;
        for (int i = 0; i < 25; ++i) {
            copies << QStringLiteral("copy %1").arg(i);
        }
        Env f({}, copies);
        QVERIFY(f.show());
        QCOMPARE(f.model()->rowCount(), 20); // Config{}.recentCount, nothing else listed
        QCOMPARE(rowsOf(f.model()).first(), QStringLiteral("copy 24"));
    }

    void aFreshPopupSqueezesEveryCopyOntoOneLine()
    {
        Env f({}, {QStringLiteral("one\ntwo\nthree")});
        QVERIFY(f.show());
        const QModelIndex row = f.model()->index(0, 0);
        QVERIFY(!row.data(HistoryModel::TitleRole).toString().contains(QLatin1Char('\n')));
        QVERIFY(row.data(HistoryModel::PreviewRole).toString().isEmpty()); // preview_lines = 1: no further lines
    }

    void aFreshPopupStartsOnTheFirstPinnedCopy()
    {
        Env f({}, {QStringLiteral("old"), QStringLiteral("mid"), QStringLiteral("new")});
        f.t.repo->setPinned(f.t.repo->search(QStringLiteral("old")).front().id, true);
        f.popup->refresh();
        QVERIFY(f.show());
        QCOMPARE(f.popup->view()->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("old"));
        QCOMPARE(rowsOf(f.model()), (QStringList{QStringLiteral("#Pinned"), QStringLiteral("old"), QStringLiteral("#Recent"),
                                                 QStringLiteral("new"), QStringLiteral("mid")}));
    }

    void aFreshPopupSearchIgnoresCaseAndFindsPartsOfWords()
    {
        Env f({}, {QStringLiteral("Hello World"), QStringLiteral("other")});
        QVERIFY(f.show());
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("LO WOR"));
        QTRY_COMPARE(rowsOf(f.model()), QStringList{QStringLiteral("Hello World")});
    }

    void aFreshPopupOpensNearThePointerAndOnTheScreen()
    {
        Env f;
        f.popup->resize(200, 150);
        QVERIFY(f.show());
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        QVERIFY(area.contains(QRect(f.popup->pos(), f.popup->size()).topLeft()));
        QVERIFY(area.contains(QRect(f.popup->pos(), f.popup->size()).bottomRight()));
    }

    void aFreshPopupClosesWhenAnotherWindowTakesTheFocus()
    {
        TestDb t;
        PopupWindow popup(*t.repo, Config{}); // hide_on_focus_loss is on by default
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));
        QWidget other;
        other.show();
        other.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTRY_VERIFY(!popup.isVisible());
    }

    // ===== Every Settings tab =================================================================

    void everyConfigKeyHasAControlOrIsListedAsFileOnly()
    {
        // A key added to Config without a Settings control (and without being listed as file-only here) fails.
        QSet<QString> keys;
        for (const auto& [key, value] : keyValues(Config{}.toToml())) {
            keys.insert(key);
        }
        QFile example(QStringLiteral(CLIPITTO_EXAMPLE_CONFIG));
        QVERIFY(example.open(QIODevice::ReadOnly));
        static const QRegularExpression keyLine(QStringLiteral("^([a-z_]+)\\s*="), QRegularExpression::MultilineOption);
        auto it = keyLine.globalMatch(QString::fromUtf8(example.readAll()));
        while (it.hasNext()) {
            keys.insert(it.next().captured(1));
        }
        example.close();
        QStringList unhandled;
        for (const QString& key : keys) {
            if (!controlChanges().count(key) && !fileOnlyKeys().count(key)) {
                unhandled << key;
            }
        }
        unhandled.sort();
        QVERIFY2(unhandled.isEmpty(), qPrintable(QStringLiteral("no Settings control and not listed as file-only: ")
                                                 + unhandled.join(QLatin1String(", "))));
        // And the lists name no key that is gone.
        for (const auto& [key, change] : controlChanges()) {
            QVERIFY2(keys.contains(key), qPrintable(key));
        }
        for (const QString& key : fileOnlyKeys()) {
            QVERIFY2(keys.contains(key), qPrintable(key));
        }
    }

    void theExampleConfigDocumentsEveryKey()
    {
        QFile example(QStringLiteral(CLIPITTO_EXAMPLE_CONFIG));
        QVERIFY(example.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(example.readAll());
        example.close();
        QStringList missing;
        for (const auto& [key, value] : keyValues(Config{}.toToml())) {
            if (!text.contains(QRegularExpression(QStringLiteral("^%1\\s*=").arg(key), QRegularExpression::MultilineOption))) {
                missing << key;
            }
        }
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("not in data/config.example.toml: ") + missing.join(QLatin1String(", "))));
    }

    void eachControlSavesAndReloads_data()
    {
        QTest::addColumn<QString>("key");
        for (const auto& [key, change] : controlChanges()) {
            QTest::newRow(qPrintable(key)) << key;
        }
    }

    void eachControlSavesAndReloads()
    {
        QFETCH(QString, key);
        DialogEnv env;
        const QString file = path(QStringLiteral("cfg-%1.toml").arg(key));
        auto first = env.open(Config{});
        QVERIFY(first->config() == Config{});
        controlChanges().at(key)(*first);
        const Config changed = first->config();
        QCOMPARE(changedKeys(Config{}, changed), QStringList{key}); // the control is bound to this key, and only it
        QString error;
        QVERIFY2(changed.save(file, &error), qPrintable(error));
        first.reset();

        // Reopened from the saved file: the control shows what was saved, and nothing else moved.
        QStringList warnings;
        const Config loaded = Config::load(file, &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1String("; "))));
        auto second = env.open(loaded);
        QVERIFY(second->config() == changed);
        QCOMPARE(changedKeys(Config{}, second->config()), QStringList{key});
    }

    void settingsWithoutAControlSurviveTheDialog()
    {
        const Config odd = fileOnlyConfig();
        QCOMPARE(changedKeys(Config{}, odd).size(), 11); // every file-only key (but windows_hello) differs
        DialogEnv env;
        auto d = env.open(odd);
        QVERIFY(d->config() == odd);
        controlChanges().at("recent_count")(*d);
        const Config c = d->config();
        QCOMPARE(changedKeys(odd, c), QStringList{QStringLiteral("recent_count")});
    }

    void passwordControlsAreDisabledUntilThereIsAMasterPassword()
    {
        DialogEnv without(false);
        auto off = without.open(Config{});
        const QList<QWidget*> dependent = {off->keepPasswordsBox(), off->passwordsInGroupBox(), off->askNoteBox(),
                                           off->detectSecretsBox(), off->detectPasswordsBox(), off->passwordsPerNoteBox(),
                                           off->unlockMinutesBox(), off->clearSecondsBox(), off->keywordEdit(),
                                           off->rulesPanel(), off->helloBox()};
        for (QWidget* w : dependent) {
            QVERIFY2(!w->isEnabled(), qPrintable(w->metaObject()->className()));
        }
        QVERIFY(off->setMasterButton()->isEnabled()); // the way out is offered
        QVERIFY(off->masterStatus()->text().contains(QLatin1String("not kept"))); // and the state is said

        DialogEnv with;
        auto on = with.open(Config{});
        for (QWidget* w : {static_cast<QWidget*>(on->keepPasswordsBox()), static_cast<QWidget*>(on->askNoteBox()),
                           static_cast<QWidget*>(on->detectSecretsBox()), static_cast<QWidget*>(on->unlockMinutesBox()),
                           static_cast<QWidget*>(on->keywordEdit()), static_cast<QWidget*>(on->rulesPanel())}) {
            QVERIFY2(w->isEnabled(), qPrintable(w->metaObject()->className()));
        }
        QVERIFY(!on->helloBox()->isEnabled()); // no Windows Hello / Touch ID support here
    }

    void cleanup()
    {
        clipitto::ui::applyTheme(Theme::System); // a theme test must not colour the next one
    }

    void tabsAreThereAndEveryTabHasControls()
    {
        DialogEnv env;
        auto d = env.open(Config{});
        QStringList titles;
        for (int i = 0; i < d->tabs()->count(); ++i) {
            titles << d->tabs()->tabText(i);
        }
        QVERIFY(titles.mid(0, 4) == (QStringList{QStringLiteral("General"), QStringLiteral("Groups"),
                                                 QStringLiteral("Passwords"), QStringLiteral("Backup")}));
        QVERIFY(titles.contains(QStringLiteral("Paste as")));
        for (int i = 0; i < d->tabs()->count(); ++i) {
            QWidget* page = d->tabs()->widget(i);
            const int controls = page->findChildren<QAbstractButton*>().size() + page->findChildren<QSpinBox*>().size()
                + page->findChildren<QComboBox*>().size() + page->findChildren<QLineEdit*>().size()
                + page->findChildren<QListWidget*>().size();
            QVERIFY2(controls > 0, qPrintable(titles.at(i)));
        }
    }

    void everySettingExplainsItselfInALabelOrATooltip()
    {
        DialogEnv env;
        auto d = env.open(Config{});
        for (const QCheckBox* box : d->findChildren<QCheckBox*>()) {
            QVERIFY2(!box->text().trimmed().isEmpty(), "a check box without text");
        }
        // Spin boxes and fields carry their meaning in a tooltip (the row label is not part of the widget).
        for (QWidget* w : QList<QWidget*>{d->copiesBox(), d->keptBox(), d->imagesBox(), d->linesBox(), d->favoritesBox(),
                                          d->clearSecondsBox(), d->keywordEdit(), d->ocrLanguagesEdit(),
                                          d->positionBox(), d->startFocusBox()}) {
            QVERIFY2(!w->toolTip().isEmpty(), w->metaObject()->className());
        }
        for (QCheckBox* w : {d->ignoreCaseBox(), d->latestBox(), d->pasteBox(), d->keepFormattingBox(), d->indexPicturesBox(),
                             d->debugLogBox(), d->autoGroupBox(), d->detectSecretsBox(), d->detectPasswordsBox(),
                             d->passwordsPerNoteBox(), d->askNoteBox()}) {
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->text()));
        }
    }

    void settingsThatDoNotApplyHereAreDisabledAndSayWhy()
    {
        DialogEnv env;
        // No keyring: the encryption box is disabled, says so, and the (i) holds the reason.
        auto d = env.open(Config{}, /*encryptionAvailable=*/false);
        QVERIFY(!d->encryptBox()->isEnabled());
        QVERIFY(d->encryptionStatus()->text().contains(QLatin1String("Not available")));
        QVERIFY(d->encryptionInfo()->toolTip().contains(QLatin1String("no keyring here")));
        // A desktop where the shortcut can't be set by ClipItTo: read-only, with the note shown and in the (i).
        clipitto::ui::ShortcutSupport readOnly;
        readOnly.editable = false;
        readOnly.note = QStringLiteral("Bind clipitto --toggle yourself.");
        SettingsDialog fixed(Config{}, *env.t.repo, readOnly);
        QVERIFY(!fixed.shortcutEdit()->isEnabled());
        QVERIFY(fixed.shortcutStatus()->text().contains(QLatin1String("--toggle")));
        QVERIFY(fixed.shortcutInfo()->toolTip().contains(QLatin1String("--toggle")));
        // Windows Hello / Touch ID: off, because this system has none (Linux, or no Hello set up).
        QVERIFY(!d->helloBox()->isEnabled());
        QVERIFY(!d->helloBox()->isChecked());
    }

    void closingTheDialogWithoutOkLeavesTheFileAlone()
    {
        const QString file = path(QStringLiteral("cancel.toml"));
        const QByteArray original = "# mine\ntheme = \"dark\"\n";
        writeFile(file, original);
        DialogEnv env;
        auto d = env.open(Config::load(file));
        controlChanges().at("recent_count")(*d);
        d->reject();
        QCOMPARE(readFile(file), original); // nothing saves until the owner takes config() and saves it
    }

    // ===== Save with no change ================================================================

    void savingWithoutChangingAnythingLeavesTheFileIdentical_data()
    {
        QTest::addColumn<QByteArray>("file");
        QFile example(QStringLiteral(CLIPITTO_EXAMPLE_CONFIG));
        QVERIFY(example.open(QIODevice::ReadOnly));
        const QByteArray full = example.readAll();
        example.close();
        QTest::newRow("the complete example") << full;
        QTest::newRow("a few keys and comments")
            << QByteArray("# my settings\ntheme = \"dark\"   # I like it dark\nrecent_count = 25\n\nfuture_key = 1\n");
        QTest::newRow("windows line endings") << QByteArray("theme = \"light\"\r\nrecent_count = 30\r\n");
        QTest::newRow("a comment only") << QByteArray("# nothing set\n");
        QTest::newRow("settings in the wrong order") << QByteArray("preview_lines = 2\nrecent_count = 40\n");
    }

    void savingWithoutChangingAnythingLeavesTheFileIdentical()
    {
        QFETCH(QByteArray, file);
        DaemonEnv env(file, true);
        QVERIFY(env.change([](SettingsDialog&) {})); // OK, with nothing touched
        QCOMPARE(readFile(env.options.configPath), file);
    }

    void savingWithoutAFileCreatesOneWithTheDefaults()
    {
        DaemonEnv env;
        QVERIFY(!QFile::exists(env.options.configPath));
        QVERIFY(env.change([](SettingsDialog&) {}));
        QVERIFY(!readFile(env.options.configPath).isEmpty());
        QVERIFY(Config::load(env.options.configPath) == Config{});
    }

    // ===== Settings change what the GUI does ==================================================

    void theNumberOfCopiesListedFollowsTheSetting()
    {
        DaemonEnv env;
        for (int i = 0; i < 12; ++i) {
            env.daemon->repository()->ingest(QStringLiteral("copy %1").arg(i));
        }
        PopupWindow* popup = env.daemon->popup();
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        const auto copies = [popup] { return rowsOf(popup->model()).filter(QRegularExpression(QStringLiteral("^copy "))).size(); };
        QCOMPARE(copies(), 12); // (the Groups row is there too: the default Passwords group)
        QVERIFY(env.change([](SettingsDialog& d) { d.copiesBox()->setValue(5); }));
        QTRY_COMPARE(copies(), 5);
        QCOMPARE(rowsOf(popup->model()).at(1), QStringLiteral("copy 11")); // the newest stay
        QVERIFY(env.change([](SettingsDialog& d) { d.copiesBox()->setValue(8); }));
        QTRY_COMPARE(copies(), 8);
        popup->hidePopup();
    }

    void theLinesShownPerCopyFollowTheSetting()
    {
        DaemonEnv env;
        env.daemon->repository()->ingest(QStringLiteral("first\nsecond\nthird\nfourth"));
        PopupWindow* popup = env.daemon->popup();
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        QVERIFY(popup->model()->firstEntry().data(HistoryModel::PreviewRole).toString().isEmpty());
        QVERIFY(env.change([](SettingsDialog& d) { d.linesBox()->setValue(3); }));
        const QStringList more = popup->model()->firstEntry().data(HistoryModel::PreviewRole).toString().split(QLatin1Char('\n'));
        QCOMPARE(more.size(), 2); // 3 lines per copy: the title and two more
        popup->hidePopup();
    }

    void theStartingSelectionFollowsTheSetting_data()
    {
        QTest::addColumn<int>("focus");
        QTest::addColumn<QString>("expected");
        QTest::newRow("pinned") << int(clipitto::StartFocus::Pinned) << QStringLiteral("pinned one");
        QTest::newRow("grouped") << int(clipitto::StartFocus::Grouped) << QStringLiteral("grouped one");
        QTest::newRow("unsaved") << int(clipitto::StartFocus::Unsaved) << QStringLiteral("newest plain");
    }

    void theStartingSelectionFollowsTheSetting()
    {
        QFETCH(int, focus);
        QFETCH(QString, expected);
        DaemonEnv env;
        auto* repo = env.daemon->repository();
        repo->setPinned(repo->ingest(QStringLiteral("pinned one")).id, true);
        repo->setGroup(repo->ingest(QStringLiteral("grouped one")).id, QStringLiteral("Work"));
        repo->ingest(QStringLiteral("newest plain"));
        QVERIFY(env.change([focus](SettingsDialog& d) {
            d.startFocusBox()->setCurrentIndex(d.startFocusBox()->findData(focus));
        }));
        PopupWindow* popup = env.daemon->popup();
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        QCOMPARE(popup->view()->currentIndex().data(HistoryModel::TitleRole).toString(), expected);
        popup->hidePopup();
    }

    void thePopupPositionFollowsTheSetting_data()
    {
        QTest::addColumn<int>("position");
        QTest::addColumn<bool>("left");
        QTest::addColumn<bool>("top");
        QTest::newRow("top-left") << int(clipitto::PopupPosition::TopLeft) << true << true;
        QTest::newRow("bottom-right") << int(clipitto::PopupPosition::BottomRight) << false << false;
    }

    void thePopupPositionFollowsTheSetting()
    {
        QFETCH(int, position);
        QFETCH(bool, left);
        QFETCH(bool, top);
        DaemonEnv env;
        PopupWindow* popup = env.daemon->popup();
        popup->resize(200, 150);
        QVERIFY(env.change([position](SettingsDialog& d) {
            d.positionBox()->setCurrentIndex(d.positionBox()->findData(position));
        }));
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        const QPoint centre = QRect(popup->pos(), popup->size()).center();
        QCOMPARE(centre.x() < area.center().x(), left);
        QCOMPARE(centre.y() < area.center().y(), top);
        popup->hidePopup();
    }

    void theFavoritesLimitFollowsTheSetting()
    {
        DaemonEnv env;
        auto* repo = env.daemon->repository();
        const qint64 a = repo->ingest(QStringLiteral("alpha")).id;
        const qint64 b = repo->ingest(QStringLiteral("beta")).id;
        QVERIFY(env.change([](SettingsDialog& d) { d.favoritesBox()->setValue(1); }));
        PopupWindow* popup = env.daemon->popup();
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        popup->view()->setFocus();
        popup->view()->setCurrentIndex(popup->model()->findEntry(a));
        QTest::keyClick(popup->view(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(repo->get(a)->favorite);
        popup->view()->setCurrentIndex(popup->model()->findEntry(b));
        QTest::keyClick(popup->view(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!repo->get(b)->favorite); // the second star is refused...
        QCOMPARE(env.messages.size(), 1); // ...and the popup says why
        QVERIFY(env.messages.first().contains(QLatin1String("already has 1")));
        popup->hidePopup();
    }

    void theThemeFollowsTheSettingAndThePopupRepaints()
    {
        DaemonEnv env;
        env.daemon->repository()->ingest(QStringLiteral("some text"));
        PopupWindow* popup = env.daemon->popup();
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        const auto lightness = [popup] { return popup->palette().color(QPalette::Window).lightness(); };
        QVERIFY(env.change([](SettingsDialog& d) { d.themeBox()->setCurrentIndex(d.themeBox()->findData(int(Theme::Dark))); }));
        QTRY_VERIFY2(lightness() < 100, "dark theme: dark window");
        QVERIFY(env.change([](SettingsDialog& d) { d.themeBox()->setCurrentIndex(d.themeBox()->findData(int(Theme::Light))); }));
        QTRY_VERIFY2(lightness() > 180, "light theme: light window");
        popup->hidePopup();
    }

    void settingsOnlyTheFileCanChangeStillWork_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<int>("check"); // which behaviour to look at
        QTest::newRow("hide_on_focus_loss = false") << QByteArray("hide_on_focus_loss = false\n") << 0;
        QTest::newRow("show_saved_on_top = false") << QByteArray("show_saved_on_top = false\n") << 1;
        QTest::newRow("saved_cap = 2") << QByteArray("saved_cap = 2\n") << 2;
    }

    void settingsOnlyTheFileCanChangeStillWork()
    {
        QFETCH(QByteArray, line);
        QFETCH(int, check);
        DaemonEnv env(line, true);
        auto* repo = env.daemon->repository();
        for (int i = 0; i < 4; ++i) {
            repo->setPinned(repo->ingest(QStringLiteral("pin %1").arg(i)).id, true);
        }
        repo->ingest(QStringLiteral("plain"));
        PopupWindow* popup = env.daemon->popup();
        popup->showPopup();
        QVERIFY(QTest::qWaitForWindowActive(popup));
        const QStringList rows = rowsOf(popup->model());
        if (check == 0) { // the popup stays open when another window takes the focus
            QWidget other;
            other.show();
            other.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&other));
            QTest::qWait(150);
            QVERIFY(popup->isVisible());
        } else if (check == 1) {
            QVERIFY(!rows.contains(QStringLiteral("#Pinned")));
            QVERIFY(rows.contains(QStringLiteral("plain")));
        } else {
            QCOMPARE(rows.indexOf(QStringLiteral("#Recent")), 4); // Groups row, #Pinned, two pins, #Recent
        }
        popup->hidePopup();
    }

    // ===== Keyboard ===========================================================================

    void shiftEnterOpensThePasteAsMenuFromTheSearchBoxAndTheList_data()
    {
        QTest::addColumn<bool>("fromList");
        QTest::newRow("search box") << false;
        QTest::newRow("list") << true;
    }

    void shiftEnterOpensThePasteAsMenuFromTheSearchBoxAndTheList()
    {
        QFETCH(bool, fromList);
        Env f({}, {QStringLiteral("hello world")});
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        QWidget* target = fromList ? static_cast<QWidget*>(f.popup->view()) : static_cast<QWidget*>(f.popup->searchBar());
        if (fromList) {
            f.popup->view()->setFocus();
        }
        QStringList items;
        int tries = 0;
        std::function<void()> look = [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (const QAction* a : menu->actions()) {
                    if (!a->isSeparator()) {
                        items << a->text();
                    }
                }
                menu->close();
            } else if (++tries < 50) {
                QTimer::singleShot(20, look);
            }
        };
        QTimer::singleShot(20, look);
        QTest::keyClick(target, Qt::Key_Return, Qt::ShiftModifier);
        QVERIFY2(items.size() >= 5, "the Paste as menu lists the transforms");
        QCOMPARE(activated.size(), 0); // looking at the menu pastes nothing
        QVERIFY(f.popup->isVisible()); // and does not close the popup
    }

    void escapeClosesAtOnceWhateverIsOnScreen_data()
    {
        QTest::addColumn<int>("state");
        QTest::newRow("a search with results") << 0;
        QTest::newRow("a search with none") << 1;
        QTest::newRow("the list has the focus") << 2;
        QTest::newRow("the groups are open") << 3;
    }

    void escapeClosesAtOnceWhateverIsOnScreen()
    {
        QFETCH(int, state);
        Env f({}, {QStringLiteral("alpha"), QStringLiteral("beta")});
        f.t.repo->setGroup(f.t.repo->search(QStringLiteral("beta")).front().id, QStringLiteral("Work"));
        f.popup->refresh();
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        if (state == 0) {
            QTest::keyClicks(f.popup->searchBar(), QStringLiteral("alp"));
            QTRY_COMPARE(rowsOf(f.model()), QStringList{QStringLiteral("alpha")});
        } else if (state == 1) {
            QTest::keyClicks(f.popup->searchBar(), QStringLiteral("zzz"));
            QTRY_VERIFY(rowsOf(f.model()).isEmpty());
        } else if (state == 2) {
            f.popup->view()->setFocus();
        } else {
            QTest::keyClick(f.popup->searchBar(), Qt::Key_Right); // opens the Groups row
            QVERIFY(f.popup->view()->isExpanded(f.model()->groupsRoot()));
        }
        QWidget* focused = QApplication::focusWidget();
        QVERIFY(focused);
        QTest::keyClick(focused, Qt::Key_Escape);
        QVERIFY(!f.popup->isVisible()); // one Esc closes: it does not first clear the search
        QCOMPARE(activated.size(), 0);
    }

    void pageKeysHomeAndEndMoveThroughALongListAndStopOnCopies()
    {
        QStringList copies;
        for (int i = 0; i < 60; ++i) {
            copies << QStringLiteral("copy %1").arg(i);
        }
        Env f({}, copies);
        f.popup->resize(400, 220);
        QVERIFY(f.show());
        f.popup->view()->setFocus();
        auto* view = f.popup->view();
        const auto onACopy = [&] { return f.model()->kindAt(view->currentIndex()) == HistoryModel::Kind::Entry; };
        QVERIFY(onACopy());
        const int start = view->currentIndex().row();
        QTest::keyClick(view, Qt::Key_PageDown);
        QVERIFY(onACopy());
        const int afterPage = view->currentIndex().row();
        QVERIFY2(afterPage > start, "Page Down moves down");
        QTest::keyClick(view, Qt::Key_PageDown);
        QVERIFY(view->currentIndex().row() > afterPage);
        const int afterTwo = view->currentIndex().row();
        QTest::keyClick(view, Qt::Key_PageUp); // how far a page is depends on the platform's row height
        QVERIFY(view->currentIndex().row() < afterTwo);
        QVERIFY(view->currentIndex().row() >= start);
        QVERIFY(onACopy());
        QTest::keyClick(view, Qt::Key_End);
        QVERIFY(onACopy());
        QCOMPARE(view->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("copy 40")); // 20 listed, newest first: the 20th is copy 40
        QVERIFY(view->visualRect(view->currentIndex()).intersects(view->viewport()->rect())); // scrolled to it
        QTest::keyClick(view, Qt::Key_Down); // nothing below
        QCOMPARE(view->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("copy 40"));
        QTest::keyClick(view, Qt::Key_Home);
        QVERIFY(onACopy());
        QCOMPARE(view->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("copy 59"));
    }

    void pinAndStarKeysMoveACopyBetweenTheSectionsAndBack()
    {
        Env f({}, {QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
        QVERIFY(f.show());
        f.popup->view()->setFocus();
        auto* view = f.popup->view();
        QTest::keyClick(view, Qt::Key_Down); // "two"
        QCOMPARE(view->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("two"));
        QTest::keyClick(view, Qt::Key_P, Qt::ControlModifier);
        QCOMPARE(rowsOf(f.model()), (QStringList{QStringLiteral("#Pinned"), QStringLiteral("two"), QStringLiteral("#Recent"),
                                                  QStringLiteral("three"), QStringLiteral("one")}));
        QTest::keyClick(view, Qt::Key_S, Qt::ControlModifier); // also star it: Favorites appears, the pin stays
        const QModelIndex favorites = f.model()->findGroup(HistoryModel::favoritesPath());
        QVERIFY(favorites.isValid());
        QCOMPARE(f.model()->rowCount(favorites), 1);
        QVERIFY(rowsOf(f.model()).contains(QStringLiteral("#Pinned")));
        QTest::keyClick(view, Qt::Key_P, Qt::ControlModifier); // unpin
        QVERIFY(!rowsOf(f.model()).contains(QStringLiteral("#Pinned")));
        QTest::keyClick(view, Qt::Key_S, Qt::ControlModifier); // unstar
        const QModelIndex emptied = f.model()->findGroup(HistoryModel::favoritesPath());
        QVERIFY(!emptied.isValid() || f.model()->rowCount(emptied) == 0); // nothing starred, nothing listed
        const Entry two = *f.t.repo->search(QStringLiteral("two")).begin();
        QVERIFY(!two.pinned() && !two.favorite);
    }

    void searchFiltersFindWhatTheyName_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("img:") << "img:" << QStringList{QStringLiteral("Image 10×10")};
        QTest::newRow("text:") << "text: thing" << QStringList{QStringLiteral("old thing"), QStringLiteral("noted thing"), QStringLiteral("grouped thing"), QStringLiteral("starred thing"), QStringLiteral("pinned thing")};
        QTest::newRow("pin:") << "pin:" << QStringList{QStringLiteral("pinned thing")};
        QTest::newRow("star:") << "star:" << QStringList{QStringLiteral("starred thing")};
        QTest::newRow("group:") << "group:" << QStringList{QStringLiteral("grouped thing")};
        QTest::newRow("group:work") << "group:work" << QStringList{QStringLiteral("grouped thing")};
        QTest::newRow("note:") << "note:" << QStringList{QStringLiteral("noted thing")};
        QTest::newRow("note:text") << "note:remember" << QStringList{QStringLiteral("noted thing")};
        QTest::newRow("today:") << "today: thing" << QStringList{QStringLiteral("noted thing"), QStringLiteral("grouped thing"), QStringLiteral("starred thing"), QStringLiteral("pinned thing")};
        QTest::newRow("since:3d") << "since:3d" << QStringList{QStringLiteral("noted thing"), QStringLiteral("grouped thing"), QStringLiteral("starred thing"), QStringLiteral("pinned thing"), QStringLiteral("Image 10×10")};
        QTest::newRow("since:30d") << "since:30d old" << QStringList{QStringLiteral("old thing")};
        QTest::newRow("combined") << "pin: img:" << QStringList{};
    }

    void searchFiltersFindWhatTheyName()
    {
        QFETCH(QString, query);
        QFETCH(QStringList, expected);
        TestDb t;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        auto* repo = t.repo.get();
        t.clock = now.addDays(-10);
        repo->ingest(QStringLiteral("old thing"));
        t.clock = now;
        repo->setPinned(repo->ingest(QStringLiteral("pinned thing")).id, true);
        repo->setFavorite(repo->ingest(QStringLiteral("starred thing")).id, true);
        repo->setGroup(repo->ingest(QStringLiteral("grouped thing")).id, QStringLiteral("Work"));
        const qint64 noted = repo->ingest(QStringLiteral("noted thing")).id;
        repo->setNote(noted, QStringLiteral("remember this"));
        QImage img(10, 10, QImage::Format_RGB32);
        img.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        img.save(&buffer, "PNG");
        clipitto::ImageClip clip;
        clip.png = png;
        clip.thumbnail = png;
        clip.width = 10;
        clip.height = 10;
        clip.pixelHash = QStringLiteral("h");
        repo->ingestImage(clip);

        PopupWindow popup(*repo, Config{});
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));
        QTest::keyClicks(popup.searchBar(), query);
        QStringList got;
        QTRY_VERIFY_WITH_TIMEOUT((got = rowsOf(popup.model())).size() == expected.size() || query == QLatin1String("combined"), 5000);
        QStringList sortedGot = got;
        QStringList sortedExpected = expected;
        sortedGot.sort();
        sortedExpected.sort();
        QCOMPARE(sortedGot, sortedExpected);
    }

    void ctrlCommaOpensSettingsFromTheListAndNotFromAPrompt()
    {
        Env f({}, {QStringLiteral("alpha")});
        QSignalSpy requested(f.popup.get(), &PopupWindow::settingsRequested);
        QVERIFY(f.show());
        f.popup->view()->setFocus();
        QTest::keyClick(f.popup->view(), Qt::Key_Comma, Qt::ControlModifier);
        QCOMPARE(requested.size(), 1);
        QTest::keyClick(f.popup->view(), Qt::Key_Comma); // without Ctrl a comma is text for the search box
        QCOMPARE(requested.size(), 1);
        QCOMPARE(f.popup->searchBar()->text(), QStringLiteral(","));
    }

    void deleteRemovesAPlainCopyAndOnlyHidesAStarredOneFromRecent()
    {
        Env f({}, {QStringLiteral("starred"), QStringLiteral("plain")});
        f.t.repo->setFavorite(f.t.repo->search(QStringLiteral("starred")).front().id, true);
        f.popup->refresh();
        f.popup->setConfirmPrompt([](QWidget*, const QString&) { return true; });
        QVERIFY(f.show());
        f.popup->view()->setFocus();
        QCOMPARE(f.popup->view()->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("plain"));
        QTest::keyClick(f.popup->view(), Qt::Key_Delete);
        QVERIFY(f.t.repo->search(QStringLiteral("plain")).empty()); // really gone
        QCOMPARE(f.popup->view()->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("starred"));
        QTest::keyClick(f.popup->view(), Qt::Key_Delete);
        QVERIFY(!f.t.repo->search(QStringLiteral("starred")).empty()); // still in Favorites
        QVERIFY(!rowsOf(f.model()).contains(QStringLiteral("starred"))); // but not in Recent any more
    }

    // ===== Rendering ==========================================================================

    static int distinctColours(const QImage& image)
    {
        QSet<QRgb> seen;
        for (int y = 0; y < image.height(); y += 2) {
            for (int x = 0; x < image.width(); x += 2) {
                seen.insert(image.pixel(x, y));
            }
        }
        return int(seen.size());
    }

    void longTextRowsAreElidedAndSizedAndPaint()
    {
        Config cfg;
        cfg.previewLines = 3;
        const QString longLine = QString(5000, QLatin1Char('x')) + QStringLiteral(" end");
        Env f(cfg, {longLine, QStringLiteral("line one\nline two\nline three\nline four\nline five"),
                    QString(200000, QLatin1Char('y')), QStringLiteral("short")});
        f.popup->resize(420, 400);
        QVERIFY(f.show());
        auto* view = f.popup->view();
        const int width = view->viewport()->width();
        for (int r = 0; r < f.model()->rowCount(); ++r) {
            const QModelIndex row = f.model()->index(r, 0);
            const QSize hint = view->sizeHintForIndex(row);
            QVERIFY2(hint.height() > 0 && hint.height() < 200, "a row is a few lines tall, never the whole text");
            QVERIFY2(view->visualRect(row).width() <= width, "and never wider than the list");
            QVERIFY(row.data(HistoryModel::TitleRole).toString().size() <= 400); // the model caps what it hands out
        }
        const QImage shot = f.popup->grab().toImage();
        QVERIFY(!shot.isNull());
        QVERIFY2(distinctColours(shot) > 4, "the popup is painted, not blank");
    }

    void emojiAndRightToLeftTextKeepTheirTextAndPaint()
    {
        const QString hebrew = QStringLiteral("שלום עולם, זה טקסט בעברית");
        const QString emoji = QStringLiteral("party \U0001F389\U0001F680 time ❤️ \U0001F468‍\U0001F469‍\U0001F467");
        const QString mixed = QStringLiteral("abc אבג 123 דה xyz");
        Env f({}, {hebrew, emoji, mixed});
        QVERIFY(f.show());
        auto* view = f.popup->view();
        const QImage before = f.popup->grab().toImage();
        QCOMPARE(rowsOf(f.model()), (QStringList{mixed, emoji, hebrew}));
        for (int r = 0; r < f.model()->rowCount(); ++r) {
            const QSize hint = view->sizeHintForIndex(f.model()->index(r, 0));
            QVERIFY2(hint.height() > 10, qPrintable(QString::number(hint.height())));
            QVERIFY(view->visualRect(f.model()->index(r, 0)).width() <= view->viewport()->width());
        }
        QVERIFY(distinctColours(before) > 4);
        // Searching in Hebrew finds the Hebrew copy, and the emoji copy by its words.
        f.popup->searchBar()->setText(QStringLiteral("עברית")); // QTest can only type ASCII
        QTRY_COMPARE(rowsOf(f.model()), QStringList{hebrew});
        QVERIFY(distinctColours(f.popup->grab().toImage()) > 4);
    }

    void switchingTheThemeRepaintsTheListLightOrDark()
    {
        Env f({}, {QStringLiteral("alpha"), QStringLiteral("beta")});
        QVERIFY(f.show());
        const auto brightness = [&](Theme theme) {
            clipitto::ui::applyTheme(theme);
            QTest::qWait(50);
            const QImage shot = f.popup->grab().toImage();
            qint64 sum = 0;
            int n = 0;
            for (int y = 0; y < shot.height(); y += 3) {
                for (int x = 0; x < shot.width(); x += 3) {
                    sum += qGray(shot.pixel(x, y));
                    ++n;
                }
            }
            return int(sum / qMax(1, n));
        };
        const int dark = brightness(Theme::Dark);
        const int light = brightness(Theme::Light);
        QVERIFY2(dark < 110, qPrintable(QString::number(dark)));
        QVERIFY2(light > 150, qPrintable(QString::number(light)));
        QVERIFY(distinctColours(f.popup->grab().toImage()) > 4); // not blank in either
    }

    // ===== Reopening ==========================================================================

    void reopeningAfterUseStartsFromTheTopAgain_data()
    {
        QTest::addColumn<int>("how");
        QTest::newRow("after Esc") << 0;
        QTest::newRow("after choosing a copy") << 1;
        QTest::newRow("after a search with no result") << 2;
    }

    void reopeningAfterUseStartsFromTheTopAgain()
    {
        QFETCH(int, how);
        QStringList copies;
        for (int i = 0; i < 40; ++i) {
            copies << QStringLiteral("copy %1").arg(i, 2, 10, QLatin1Char('0'));
        }
        Env f({}, copies);
        f.t.repo->setPinned(f.t.repo->search(QStringLiteral("copy 03")).front().id, true);
        f.t.repo->setGroup(f.t.repo->search(QStringLiteral("copy 07")).front().id, QStringLiteral("Work"));
        f.popup->resize(400, 260);
        f.popup->refresh();
        QVERIFY(f.show());
        auto* view = f.popup->view();
        const QString top = view->currentIndex().data(HistoryModel::TitleRole).toString();
        QCOMPARE(top, QStringLiteral("copy 03")); // the first pinned copy

        // Use it: open the groups, scroll to the bottom, then leave one of three ways.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_End);
        QVERIFY(view->verticalScrollBar()->value() > 0);
        view->expand(f.model()->groupsRoot());
        if (how == 0) {
            QTest::keyClick(view, Qt::Key_Escape);
        } else if (how == 1) {
            QTest::keyClick(view, Qt::Key_Return);
        } else {
            QTest::keyClicks(f.popup->searchBar(), QStringLiteral("zzz"));
            QTRY_VERIFY(rowsOf(f.model()).isEmpty());
            QTest::keyClick(f.popup->searchBar(), Qt::Key_Escape);
        }
        QVERIFY(!f.popup->isVisible());

        QVERIFY(f.show());
        QVERIFY(f.popup->searchBar()->text().isEmpty());
        QCOMPARE(f.popup->focusWidget(), f.popup->searchBar()); // typing goes to the search box
        QVERIFY(!view->isExpanded(f.model()->groupsRoot()));
        QTRY_COMPARE(view->verticalScrollBar()->value(), 0);
        const QStringList rows = rowsOf(f.model());
        QCOMPARE(rows.first(), QStringLiteral("@Groups"));
        QVERIFY(rows.contains(QStringLiteral("#Pinned")));
        QCOMPARE(view->currentIndex().data(HistoryModel::TitleRole).toString(), top);
        // Pinned and the Groups row are on screen, not scrolled away.
        const QRect port = view->viewport()->rect();
        QVERIFY(port.intersects(view->visualRect(f.model()->groupsRoot())));
        QVERIFY(port.intersects(view->visualRect(f.model()->index(rows.indexOf(QStringLiteral("#Pinned")), 0))));
    }

    void reopeningShowsCopiesMadeWhileItWasClosedAndForgetsTheSelection()
    {
        Env f({}, {QStringLiteral("alpha"), QStringLiteral("beta")});
        QVERIFY(f.show());
        f.popup->view()->setFocus();
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // onto the second copy
        QTest::keyClick(f.popup->view(), Qt::Key_Escape);
        f.t.repo->ingest(QStringLiteral("fresh"));
        f.popup->historyChanged(); // what the daemon calls for a new copy, shown or not
        QVERIFY(f.show());
        QCOMPARE(rowsOf(f.model()).first(), QStringLiteral("fresh"));
        QCOMPARE(f.popup->view()->currentIndex().data(HistoryModel::TitleRole).toString(), QStringLiteral("fresh"));
    }
};

QTEST_MAIN(TestGuiGeneral)
#include "tst_gui_general.moc"
