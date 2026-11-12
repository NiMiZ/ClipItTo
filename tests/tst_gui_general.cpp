#include "Config.h"
#include "Daemon.h"
#include "FakeKeyStore.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PasswordKeeper.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "SettingsDialog.h"
#include "TestDb.h"
#include "ControlChannel.h"
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
#include <QTest>

#include <functional>
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
        {"passwords_per_note", [](SettingsDialog& d) { toggle(d.passwordsPerNoteBox()); }},
        {"password_ask_note", [](SettingsDialog& d) { toggle(d.askNoteBox()); }},
        {"password_unlock_minutes", [](SettingsDialog& d) { d.unlockMinutesBox()->setValue(17); }},
        {"password_clear_seconds", [](SettingsDialog& d) { d.clearSecondsBox()->setValue(12); }},
        {"password_search_keyword", [](SettingsDialog& d) { d.keywordEdit()->setText(QStringLiteral("pw[]")); }},
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
            {"password_clear_seconds", "30"}, {"password_search_keyword", "\"p[]\""}, {"backend", "\"auto\""},
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
        QCOMPARE(d->shortcutEdit()->keySequence().toString(QKeySequence::PortableText).toLower().replace(QLatin1String("meta"), QLatin1String("ctrl")),
                 QStringLiteral(CLIPITTO_DEFAULT_HOTKEY).toLower().replace(QLatin1String("cmd"), QLatin1String("ctrl")));
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
};

QTEST_MAIN(TestGuiGeneral)
#include "tst_gui_general.moc"
