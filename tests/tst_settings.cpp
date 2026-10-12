#include "Config.h"
#include "SettingsDialog.h"
#include "TestDb.h"
#include "Theme.h"
#include "AppIcon.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>
#include <QTreeWidget>

using clipitto::Config;
using clipitto::Theme;
using clipitto::ui::SettingsDialog;
using clipitto::ui::ShortcutSupport;

class TestSettings : public QObject {
    Q_OBJECT

private:
    static QStringList treePaths(QTreeWidget* tree)
    {
        QStringList paths;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            paths << (*it)->data(0, Qt::UserRole).toString();
        }
        return paths;
    }

private slots:
    void themeChangesAreAnnounced()
    {
        // Static: the listener stays registered after this test (there is no unregister).
        static int calls = 0;
        calls = 0;
        clipitto::ui::onThemeApplied([] { ++calls; });
        clipitto::ui::applyTheme(Theme::Dark);
        QVERIFY(clipitto::ui::currentThemeIsDark());
        QCOMPARE(calls, 1);
        clipitto::ui::applyTheme(Theme::Light);
        QVERIFY(!clipitto::ui::currentThemeIsDark());
        QCOMPARE(calls, 2);
        // The icon's two variants differ (the tray switches between them).
        const QImage light = clipitto::ui::appIcon(false).pixmap(32, 32).toImage();
        const QImage dark = clipitto::ui::appIcon(true).pixmap(32, 32).toImage();
        QVERIFY(light != dark);
        clipitto::ui::applyTheme(Theme::System);
    }

    void lightThemeIsLightEvenOnADarkDesktop()
    {
        // Fusion's standardPalette() follows the desktop (dark there): Light must not use it.
        clipitto::ui::applyTheme(Theme::Dark);
        clipitto::ui::applyTheme(Theme::Light);
        const QPalette palette = QApplication::palette();
        QCOMPARE(palette.color(QPalette::Window), clipitto::ui::lightPalette().color(QPalette::Window));
        QCOMPARE(palette.color(QPalette::Base), clipitto::ui::lightPalette().color(QPalette::Base));
        QVERIFY(palette.color(QPalette::Window).lightness() > 200);
        QVERIFY(palette.color(QPalette::WindowText).lightness() < 80);
        clipitto::ui::applyTheme(Theme::System);
    }

    void showsAndReturnsTheConfig()
    {
        TestDb t;
        Config c;
        c.theme = Theme::Dark;
        c.recentCount = 30;
        c.previewLines = 2;
        c.hotkey = QStringLiteral("Ctrl+`");
        c.maxHistory = 77; // not on the dialog: must survive untouched
        SettingsDialog d(c, *t.repo, {});
        QCOMPARE(d.themeBox()->currentData().toInt(), int(Theme::Dark));
        QCOMPARE(d.copiesBox()->value(), 30);
        QCOMPARE(d.linesBox()->value(), 2);
        QCOMPARE(d.shortcutEdit()->keySequence().toString(QKeySequence::PortableText), QStringLiteral("Ctrl+`"));
        QVERIFY(d.config() == c);

        d.themeBox()->setCurrentIndex(d.themeBox()->findData(int(Theme::Light)));
        d.copiesBox()->setValue(50);
        d.linesBox()->setValue(3);
        const Config edited = d.config();
        QVERIFY(d.latestBox()->isChecked()); // a chosen copy becomes the latest by default
        d.latestBox()->setChecked(false);
        QVERIFY(!d.config().latestAfterPaste);
        QCOMPARE(d.positionBox()->count(), 7); // pointer, center, four corners, last place
        d.positionBox()->setCurrentIndex(d.positionBox()->findData(int(clipitto::PopupPosition::BottomRight)));
        QVERIFY(d.config().popupPosition == clipitto::PopupPosition::BottomRight);
        QVERIFY(d.pasteBox()->isChecked()); // pasting is the default
        d.pasteBox()->setChecked(false);
        QVERIFY(!d.config().pasteOnEnter);
        d.favoritesBox()->setValue(15);
        QCOMPARE(d.favoritesBox()->maximum(), 20);
        QVERIFY(edited.theme == Theme::Light);
        QCOMPARE(d.config().favoritesLimit, 15);
        QCOMPARE(edited.recentCount, 50);
        QCOMPARE(edited.previewLines, 3);
        QCOMPARE(edited.maxHistory, 77);
    }

    void showsTheRunningVersion()
    {
        TestDb t;
        const QString before = QCoreApplication::applicationVersion();
        QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
        SettingsDialog d(Config{}, *t.repo, {});
        QCOMPARE(d.versionLabel()->text(), QStringLiteral("ClipItTo 9.8.7"));
        QCoreApplication::setApplicationVersion(before);
    }

    void capturesAShortcut()
    {
        TestDb t;
        SettingsDialog d(Config{}, *t.repo, {});
        d.shortcutEdit()->setKeySequence(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft));
        QCOMPARE(d.config().hotkey, QStringLiteral("Ctrl+`"));
        QVERIFY(d.shortcutStatus()->text().contains(QLatin1String("Ctrl+`")));

        // Only the first combination counts.
        d.shortcutEdit()->setKeySequence(QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT, Qt::Key_X), QKeyCombination(Qt::Key_Y)));
        QCOMPARE(d.config().hotkey, QStringLiteral("Ctrl+Alt+X"));
        QCOMPARE(d.shortcutEdit()->keySequence().count(), 1);

        // A key without modifiers is refused; the previous shortcut stays.
        d.shortcutEdit()->setKeySequence(QKeySequence(Qt::Key_A));
        QCOMPARE(d.config().hotkey, QStringLiteral("Ctrl+Alt+X"));
        QVERIFY(d.shortcutStatus()->text().contains(QLatin1String("can't be used")));

        // "Default" restores Ctrl+Alt+V.
        for (QPushButton* b : d.findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("Default")) {
                b->click();
            }
        }
        QCOMPARE(d.config().hotkey, QStringLiteral("Ctrl+Alt+V"));
    }

    void shortcutCanBeReadOnly()
    {
        TestDb t;
        ShortcutSupport support;
        support.editable = false;
        support.note = QStringLiteral("Bind it yourself");
        SettingsDialog d(Config{}, *t.repo, support);
        QVERIFY(!d.shortcutEdit()->isEnabled());
        QCOMPARE(d.shortcutStatus()->text(), QStringLiteral("Bind it yourself"));
    }

    void managesGroups()
    {
        TestDb t;
        t.repo->createGroup(QStringLiteral("Work/Servers"));
        const auto id = t.repo->ingest(QStringLiteral("x")).id;
        t.repo->setGroup(id, QStringLiteral("Work"));

        SettingsDialog d(Config{}, *t.repo, {});
        QStringList answers = {QStringLiteral("Home"), QStringLiteral("Prod"), QStringLiteral("Jobs")};
        d.setTextPrompt([&](QWidget*, const QString&, const QString&, const QString&) {
            return std::optional<QString>(answers.takeFirst());
        });
        d.setConfirmPrompt([](QWidget*, const QString&) { return true; });
        QCOMPARE(treePaths(d.groupTree()), (QStringList{QStringLiteral("Work"), QStringLiteral("Work/Servers")}));
        QVERIFY(!d.groupsChanged());

        d.newGroup();
        QCOMPARE(d.selectedGroup(), QStringLiteral("Home"));
        d.selectGroup(QStringLiteral("Work/Servers"));
        d.newSubGroup();
        QCOMPARE(d.selectedGroup(), QStringLiteral("Work/Servers/Prod"));
        d.selectGroup(QStringLiteral("Work"));
        d.renameSelectedGroup();
        QCOMPARE(t.repo->get(id)->group, QStringLiteral("Jobs"));
        QCOMPARE(treePaths(d.groupTree()), (QStringList{QStringLiteral("Home"), QStringLiteral("Jobs"),
                                                        QStringLiteral("Jobs/Servers"), QStringLiteral("Jobs/Servers/Prod")}));
        d.selectGroup(QStringLiteral("Jobs/Servers"));
        d.deleteSelectedGroup();
        QCOMPARE(t.repo->groups(), (QStringList{QStringLiteral("Home"), QStringLiteral("Jobs")}));
        QVERIFY(d.groupsChanged());
    }

    void groupsTabSelectsTheFirstGroupAndOnlyOffersClose()
    {
        TestDb t;
        t.repo->createGroup(QStringLiteral("Alpha"));
        t.repo->createGroup(QStringLiteral("Beta"));
        SettingsDialog d(Config{}, *t.repo, {});
        QCOMPARE(d.selectedGroup(), QStringLiteral("Alpha")); // no frame-without-selection
        auto* ok = d.buttons()->button(QDialogButtonBox::Ok);
        auto* cancel = d.buttons()->button(QDialogButtonBox::Cancel);
        auto* close = d.buttons()->button(QDialogButtonBox::Close);
        d.show();
        QVERIFY(ok->isVisible() && cancel->isVisible() && !close->isVisible());
        d.tabs()->setCurrentIndex(1);
        QVERIFY(!ok->isVisible() && !cancel->isVisible() && close->isVisible());
        close->click();
        QCOMPARE(d.result(), int(QDialog::Accepted));
    }

    void positionAndEncryption()
    {
        TestDb t;
        Config c;
        c.popupPosition = clipitto::PopupPosition::Center;
        c.encryptHistory = true;
        SettingsDialog available(c, *t.repo, {}, {true, {}, false, {}});
        QCOMPARE(available.positionBox()->currentData().toInt(), int(clipitto::PopupPosition::Center));
        QVERIFY(available.encryptBox()->isEnabled() && available.encryptBox()->isChecked());
        available.encryptBox()->setChecked(false);
        available.positionBox()->setCurrentIndex(0);
        QVERIFY(!available.config().encryptHistory);
        QVERIFY(available.config().popupPosition == clipitto::PopupPosition::Cursor);

        SettingsDialog unavailable(Config{}, *t.repo, {}, {false, QStringLiteral("no keyring here"), false, {}});
        QVERIFY(!unavailable.encryptBox()->isEnabled());
        bool reasonShown = false;
        for (QLabel* l : unavailable.findChildren<QLabel*>()) {
            reasonShown = reasonShown || l->text() == QLatin1String("Not available: no keyring here");
        }
        QVERIFY(reasonShown);
    }

    void requestedButInactiveEncryptionIsStatedPlainly()
    {
        TestDb t;
        Config c;
        c.encryptHistory = true;
        clipitto::ui::EncryptionSupport s{false, QStringLiteral("no keyring"), false, QStringLiteral("no keyring")};
        SettingsDialog d(c, *t.repo, {}, s);
        bool warned = false;
        for (QLabel* l : d.findChildren<QLabel*>()) {
            warned = warned || l->text().contains(QLatin1String("Currently NOT encrypted"));
        }
        QVERIFY(warned);
    }

    void groupErrorsAreReported()
    {
        TestDb t;
        t.repo->createGroup(QStringLiteral("A"));
        t.repo->createGroup(QStringLiteral("B"));
        SettingsDialog d(Config{}, *t.repo, {});
        d.setTextPrompt([](QWidget*, const QString&, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("B"));
        });
        QString reported;
        d.setNotify([&](QWidget*, const QString& message) { reported = message; });
        d.selectGroup(QStringLiteral("A"));
        d.renameSelectedGroup();
        QVERIFY(reported.contains(QLatin1String("already exists")));
        QVERIFY(!d.groupsChanged());
    }
};

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
