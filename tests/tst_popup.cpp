#include "Config.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "TestDb.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSignalSpy>
#include <QTest>

using clipitto::Config;
using clipitto::Entry;
using clipitto::ui::HistoryModel;
using clipitto::ui::PopupWindow;

// Keyboard/mouse behavior of the popup on the offscreen platform.
class TestPopup : public QObject {
    Q_OBJECT

private:
    struct Fixture {
        TestDb t;
        std::unique_ptr<PopupWindow> popup;
        qint64 favId = 0;

        explicit Fixture(Config cfg = {})
        {
            favId = t.repo->ingest(QStringLiteral("ssh -J bastion app")).id;
            t.repo->ingest(QStringLiteral("alpha"));
            t.repo->ingest(QStringLiteral("beta"));
            t.repo->ingest(QStringLiteral("gamma ray"));
            t.repo->setFavorite(favId, true);
            cfg.hideOnFocusLoss = false; // covered by focusLossHidesWhenEnabled
            popup = std::make_unique<PopupWindow>(*t.repo, cfg);
        }

        bool show()
        {
            popup->showPopup();
            return QTest::qWaitForWindowActive(popup.get());
        }

        HistoryModel* model() const { return popup->model(); }
        int currentRow() const { return popup->view()->currentIndex().row(); }
        QString currentContent() const
        {
            const Entry* e = model()->entryAt(currentRow());
            return e ? e->content : QString();
        }
    };

private slots:
    void showFocusesSearchAndSelectsFirstEntry()
    {
        Fixture f;
        QVERIFY(f.show());
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
        // Rows: #Saved, ssh, #Recent, gamma ray, beta, alpha
        QCOMPARE(f.model()->rowCount(), 6);
        QCOMPARE(f.currentRow(), 1);
    }

    void downFromSearchFocusesFirstEntry()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QCOMPARE(QApplication::focusWidget(), f.popup->view());
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app"));
    }

    void arrowsTraverseAndSkipHeaders()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // over "#Recent"
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
        QTest::keyClick(f.popup->view(), Qt::Key_End);
        QCOMPARE(f.currentContent(), QStringLiteral("alpha"));
        QTest::keyClick(f.popup->view(), Qt::Key_Up);
        QCOMPARE(f.currentContent(), QStringLiteral("beta"));
        QTest::keyClick(f.popup->view(), Qt::Key_Home);
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app"));
    }

    void upFromFirstEntryReturnsToSearch()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
    }

    void typingInListGoesToSearch()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClicks(f.popup->view(), QStringLiteral("bet"));
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
        QCOMPARE(f.popup->searchBar()->text(), QStringLiteral("bet"));
        QTest::keyClick(f.popup->view(), Qt::Key_Backspace);
        QCOMPARE(f.popup->searchBar()->text(), QStringLiteral("be"));
    }

    void searchFiltersTheList()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("gamma"));
        QTRY_COMPARE(f.model()->rowCount(), 1);
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
    }

    void escapeHides()
    {
        Fixture f;
        QSignalSpy visibility(f.popup.get(), &PopupWindow::visibilityChanged);
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Escape);
        QVERIFY(!f.popup->isVisible());
        QCOMPARE(visibility.size(), 2);
        QCOMPARE(visibility.last().at(0).toBool(), false);

        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Escape);
        QVERIFY(!f.popup->isVisible());
    }

    void enterActivatesSelectedEntryAndHides()
    {
        Fixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.at(0).at(0).value<Entry>().content, QStringLiteral("beta"));
        QVERIFY(!f.popup->isVisible());
    }

    void enterInSearchUsesFreshResults()
    {
        Fixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("alp"));
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Return); // before the debounce fires
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.at(0).at(0).value<Entry>().content, QStringLiteral("alpha"));
    }

    void clickActivates()
    {
        Fixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        const QModelIndex gamma = f.model()->index(3);
        QTest::mouseClick(f.popup->view()->viewport(), Qt::LeftButton, {},
                          f.popup->view()->visualRect(gamma).center());
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.at(0).at(0).value<Entry>().content, QStringLiteral("gamma ray"));
    }

    void doubleClickModeIgnoresSingleClick()
    {
        Config cfg;
        cfg.clickToCopy = clipitto::ClickMode::Double;
        Fixture f(cfg);
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        const QPoint pos = f.popup->view()->visualRect(f.model()->index(3)).center();
        QTest::mouseClick(f.popup->view()->viewport(), Qt::LeftButton, {}, pos);
        QCOMPARE(activated.size(), 0);
        QTest::mouseDClick(f.popup->view()->viewport(), Qt::LeftButton, {}, pos);
        QCOMPARE(activated.size(), 1);
    }

    void ctrlSTogglesFavoriteAndKeepsSelection()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // gamma ray
        QTest::keyClick(f.popup->view(), Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
        QVERIFY(f.model()->entryAt(f.currentRow())->favorite);
        QCOMPARE(f.t.repo->saved(10).size(), 2u);
    }

    void ctrlGAndCtrlEUsePrompts()
    {
        Fixture f;
        QStringList offeredGroups;
        f.popup->setGroupPrompt([&](QWidget*, const QString&, const QStringList& choices) {
            offeredGroups = choices;
            return std::optional<QString>(QStringLiteral("Work"));
        });
        f.popup->setNotePrompt([](QWidget*, const QString&, const QStringList&) {
            return std::optional<QString>(QStringLiteral("my title"));
        });
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_End); // alpha
        QTest::keyClick(f.popup->view(), Qt::Key_G, Qt::ControlModifier);
        QTest::keyClick(f.popup->view(), Qt::Key_E, Qt::ControlModifier);

        const auto alpha = f.t.repo->search(QStringLiteral("alpha")).front();
        QCOMPARE(alpha.group, QStringLiteral("Work"));
        QCOMPARE(alpha.note, QStringLiteral("my title"));
        QVERIFY(offeredGroups.isEmpty()); // no groups existed yet
        QCOMPARE(f.currentContent(), QStringLiteral("alpha")); // followed into the Saved section
        QCOMPARE(f.model()->index(f.currentRow()).data(HistoryModel::TitleRole).toString(), QStringLiteral("my title"));
    }

    void cancelledPromptChangesNothing()
    {
        Fixture f;
        f.popup->setNotePrompt([](QWidget*, const QString&, const QStringList&) { return std::optional<QString>(); });
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_E, Qt::ControlModifier);
        QVERIFY(f.t.repo->get(f.favId)->note.isEmpty());
    }

    void deleteRemovesAndSelectsNeighbor()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // gamma ray
        QTest::keyClick(f.popup->view(), Qt::Key_Delete);
        QCOMPARE(f.t.repo->count(), 3);
        QCOMPARE(f.currentContent(), QStringLiteral("beta"));
    }

    void contextMenuActions()
    {
        Fixture f;
        QVERIFY(f.show());
        const Entry beta = *f.model()->entryAt(4);
        std::unique_ptr<QMenu> menu(f.popup->createContextMenu(beta, nullptr));
        QStringList texts;
        QAction* deleteAction = nullptr;
        for (QAction* a : menu->actions()) {
            texts << a->text();
            if (a->text() == QLatin1String("Delete")) {
                deleteAction = a;
            }
        }
        QVERIFY(texts.contains(QStringLiteral("★ Add to favorites")));
        QVERIFY(texts.contains(QStringLiteral("Group")));
        QVERIFY(texts.contains(QStringLiteral("Add note…")));
        QVERIFY(deleteAction);
        deleteAction->trigger();
        QVERIFY(!f.t.repo->get(beta.id));
    }

    void reopeningClearsTheQuery()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("gamma"));
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Escape);
        QVERIFY(f.show());
        QVERIFY(f.popup->searchBar()->text().isEmpty());
        QCOMPARE(f.model()->rowCount(), 6);
    }

    void newClipRefreshesVisibleDefaultView()
    {
        Fixture f;
        QVERIFY(f.show());
        f.t.repo->ingest(QStringLiteral("fresh"));
        f.popup->historyChanged();
        QCOMPARE(f.model()->entryAt(3)->content, QStringLiteral("fresh"));
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app")); // selection kept
    }

    void focusLossHidesWhenEnabled()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("x"));
        Config cfg;
        cfg.hideOnFocusLoss = true;
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));

        QWidget other;
        other.show();
        other.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTRY_VERIFY(!popup.isVisible());
    }
};

QTEST_MAIN(TestPopup)
#include "tst_popup.moc"
