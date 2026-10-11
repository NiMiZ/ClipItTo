#include "Config.h"
#include "SettingsDialog.h"
#include "TestDb.h"

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
        QVERIFY(edited.theme == Theme::Light);
        QCOMPARE(edited.recentCount, 50);
        QCOMPARE(edited.previewLines, 3);
        QCOMPARE(edited.maxHistory, 77);
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
