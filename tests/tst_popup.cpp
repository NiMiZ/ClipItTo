#include "Config.h"
#include "HistoryModel.h"
#include "db/Repository.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "TestDb.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

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
        qint64 pinnedId = 0;

        explicit Fixture(Config cfg = {})
        {
            pinnedId = t.repo->ingest(QStringLiteral("ssh -J bastion app")).id;
            t.repo->ingest(QStringLiteral("alpha"));
            t.repo->ingest(QStringLiteral("beta"));
            t.repo->ingest(QStringLiteral("gamma ray"));
            t.repo->setPinned(pinnedId, true);
            cfg.hideOnFocusLoss = false; // covered by focusLossHidesWhenEnabled
            popup = std::make_unique<PopupWindow>(*t.repo, cfg);
        }

        bool show()
        {
            popup->showPopup();
            return QTest::qWaitForWindowActive(popup.get());
        }

        HistoryModel* model() const { return popup->model(); }
        QModelIndex current() const { return popup->view()->currentIndex(); }
        int currentRow() const { return current().row(); }
        QString currentContent() const
        {
            const Entry* e = model()->entryAt(current());
            return e ? e->content : QString();
        }
    };

    // Fixture plus groups: beta in Work, alpha in Work/Servers.
    struct GroupFixture : Fixture {
        qint64 alphaId = 0;
        qint64 betaId = 0;
        GroupFixture()
        {
            alphaId = t.repo->search(QStringLiteral("alpha")).front().id;
            betaId = t.repo->search(QStringLiteral("beta")).front().id;
            t.repo->setGroup(alphaId, QStringLiteral("Work/Servers"));
            t.repo->setGroup(betaId, QStringLiteral("Work"));
        }
    };

private slots:
    void showFocusesSearchAndSelectsFirstEntry()
    {
        Fixture f;
        QVERIFY(f.show());
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
        // Rows: #Pinned, ssh, #Recent, gamma ray, beta, alpha
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
        const QModelIndex gamma = f.model()->index(3, 0);
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
        const QPoint pos = f.popup->view()->visualRect(f.model()->index(3, 0)).center();
        QTest::mouseClick(f.popup->view()->viewport(), Qt::LeftButton, {}, pos);
        QCOMPARE(activated.size(), 0);
        QTest::mouseDClick(f.popup->view()->viewport(), Qt::LeftButton, {}, pos);
        QCOMPARE(activated.size(), 1);
    }

    void draggingTheAppNameMovesThePopupAndItReopensThere()
    {
        Fixture f;
        QVERIFY(f.show());
        QWidget* handle = f.popup->dragHandle();
        QCOMPARE(qobject_cast<QLabel*>(handle)->text(), QStringLiteral("ClipItTo"));
        // Drag it to the middle of the screen: a place it fits whatever the screen size (a
        // reopened popup is kept fully on screen).
        const QPoint before = f.popup->pos();
        const QRect area = f.popup->screen()->availableGeometry();
        const QPoint delta = area.center() - f.popup->frameGeometry().center();
        QVERIFY(!delta.isNull());
        const QPoint grab = handle->rect().center();
        QTest::mousePress(handle, Qt::LeftButton, {}, grab);
        QTest::mouseMove(handle, grab + delta);
        QTest::mouseRelease(handle, Qt::LeftButton, {}, grab + delta);
        QCOMPARE(f.popup->pos(), before + delta);
        QVERIFY(f.popup->isVisible()); // dragging doesn't close it

        QTest::keyClick(f.popup->searchBar(), Qt::Key_Escape);
        QVERIFY(f.show());
        QCOMPARE(f.popup->pos(), before + delta); // same place, not the pointer's
    }

    void choosingACopyHidesThePopup()
    {
        Fixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("gamma"));
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QVERIFY(!f.popup->isVisible());
    }

    void ctrlSStarsIntoFavorites()
    {
        Fixture f;
        QVERIFY(f.show());
        QVERIFY(!f.model()->groupsRoot().isValid()); // no groups, no favorites yet
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // gamma ray
        QTest::keyClick(f.popup->view(), Qt::Key_S, Qt::ControlModifier);

        const auto gammaId = f.t.repo->search(QStringLiteral("gamma")).front().id;
        QVERIFY(f.t.repo->get(gammaId)->favorite);
        // It moved from Recent into Favorites, the first group, and the cursor followed it.
        const QModelIndex favorites = f.model()->findGroup(HistoryModel::favoritesPath());
        QVERIFY(favorites.isValid());
        QCOMPARE(favorites.parent(), f.model()->groupsRoot());
        QCOMPARE(favorites.row(), 0);
        QCOMPARE(f.model()->rowCount(favorites), 1);
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
        QCOMPARE(f.model()->pathAt(f.current()), HistoryModel::favoritesPath());

        // Ctrl+S again unstars it: back in Recent, Favorites stays (empty) as the first group.
        QTest::keyClick(f.popup->view(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!f.t.repo->get(gammaId)->favorite);
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
        QVERIFY(!f.current().parent().isValid()); // a Recent row
    }

    void fullFavoritesExplainsInsteadOfStarring()
    {
        Config cfg;
        Fixture f(cfg);
        f.t.repo->setLimits({/*favorites=*/1, /*starsPerGroup=*/10, /*pins=*/10});
        f.t.repo->setFavorite(f.t.repo->search(QStringLiteral("beta")).front().id, true);
        QStringList messages;
        f.popup->setNotify([&](QWidget*, const QString& message) { messages << message; });
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down); // the Groups row
        QTest::keyClick(f.popup->view(), Qt::Key_End); // alpha
        QTest::keyClick(f.popup->view(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!f.t.repo->search(QStringLiteral("alpha")).front().favorite);
        QCOMPARE(messages.size(), 1);
        QVERIFY2(messages.first().contains(QStringLiteral("Favorites already has 1")), qPrintable(messages.first()));
    }

    void ctrlPPinsAndAltArrowsReorderPins()
    {
        Fixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down); // ssh (pinned)
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // gamma ray
        QTest::keyClick(f.popup->view(), Qt::Key_P, Qt::ControlModifier);
        // Rows: #Pinned, ssh, gamma ray, #Recent, beta, alpha
        QCOMPARE(f.model()->entryAt(f.model()->index(2, 0))->content, QStringLiteral("gamma ray"));
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));

        QTest::keyClick(f.popup->view(), Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(f.model()->entryAt(f.model()->index(1, 0))->content, QStringLiteral("gamma ray"));
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray")); // the cursor moved with it

        QTest::keyClick(f.popup->view(), Qt::Key_P, Qt::ControlModifier); // unpin
        QCOMPARE(f.t.repo->pinned(10).size(), 1u);
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
    }

    void pinLimitExplains()
    {
        Fixture f;
        for (int i = 0; i < 9; ++i) {
            f.t.repo->setPinned(f.t.repo->ingest(QStringLiteral("pin %1").arg(i)).id, true);
        }
        QStringList messages;
        f.popup->setNotify([&](QWidget*, const QString& message) { messages << message; });
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_End); // alpha, not pinned
        QTest::keyClick(f.popup->view(), Qt::Key_P, Qt::ControlModifier);
        QCOMPARE(f.t.repo->pinned(20).size(), 10u);
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.first().contains(QStringLiteral("up to 10")));
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
        QCOMPARE(f.currentContent(), QStringLiteral("alpha")); // followed into its group
        QCOMPARE(f.current().data(HistoryModel::TitleRole).toString(), QStringLiteral("my title"));
    }

    void cancelledPromptChangesNothing()
    {
        Fixture f;
        f.popup->setNotePrompt([](QWidget*, const QString&, const QStringList&) { return std::optional<QString>(); });
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QTest::keyClick(f.popup->view(), Qt::Key_E, Qt::ControlModifier);
        QVERIFY(f.t.repo->get(f.pinnedId)->note.isEmpty());
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
        const Entry beta = *f.model()->entryAt(f.model()->index(4, 0));
        std::unique_ptr<QMenu> menu(f.popup->createContextMenu(beta, nullptr));
        QStringList texts;
        QAction* deleteAction = nullptr;
        for (QAction* a : menu->actions()) {
            texts << a->text();
            if (a->text() == QLatin1String("Delete")) {
                deleteAction = a;
            }
        }
        QVERIFY(texts.contains(QStringLiteral("★ Star")));
        QVERIFY(texts.contains(QStringLiteral("Pin to the top")));
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
        QCOMPARE(f.model()->entryAt(f.model()->index(3, 0))->content, QStringLiteral("fresh"));
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app")); // selection kept
    }

    // --- Group tree ---------------------------------------------------------------------

    void groupsRowIsFirstAndCollapsed()
    {
        GroupFixture f;
        QVERIFY(f.show());
        const QModelIndex root = f.model()->groupsRoot();
        QCOMPARE(root.row(), 0);
        QVERIFY(!f.popup->view()->isExpanded(root));
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app")); // Enter still copies a copy
    }

    void keyboardWalksTheGroupTree()
    {
        GroupFixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QCOMPARE(f.current(), f.model()->groupsRoot()); // the row right under the search box

        auto* view = f.popup->view();
        QTest::keyClick(view, Qt::Key_Right);
        QVERIFY(view->isExpanded(f.model()->groupsRoot()));
        QTest::keyClick(view, Qt::Key_Down);
        QCOMPARE(f.current(), f.model()->findGroup(HistoryModel::favoritesPath())); // always first
        QTest::keyClick(view, Qt::Key_Down);
        QCOMPARE(f.current(), f.model()->findGroup(QStringLiteral("Work")));
        QTest::keyClick(view, Qt::Key_Return); // Enter on a group opens it
        QVERIFY(view->isExpanded(f.current()));
        QTest::keyClick(view, Qt::Key_Down);
        QCOMPARE(f.current(), f.model()->findGroup(QStringLiteral("Work/Servers")));
        QTest::keyClick(view, Qt::Key_Down); // over the collapsed Servers, to Work's own copy
        QCOMPARE(f.currentContent(), QStringLiteral("beta"));
        QCOMPARE(f.model()->pathAt(f.current()), QStringLiteral("Work"));

        QTest::keyClick(view, Qt::Key_Up);
        QTest::keyClick(view, Qt::Key_Right); // open Servers
        QTest::keyClick(view, Qt::Key_Down);
        QCOMPARE(f.currentContent(), QStringLiteral("alpha"));
        QTest::keyClick(view, Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.at(0).at(0).value<Entry>().content, QStringLiteral("alpha"));
    }

    void leftCollapsesAndUpReturnsToSearch()
    {
        GroupFixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        auto* view = f.popup->view();
        QTest::keyClick(view, Qt::Key_Right);
        QTest::keyClick(view, Qt::Key_Left);
        QVERIFY(!view->isExpanded(f.model()->groupsRoot()));
        QTest::keyClick(view, Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
    }

    void rightInEmptySearchOpensGroups()
    {
        GroupFixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Right);
        QCOMPARE(QApplication::focusWidget(), f.popup->view());
        QCOMPARE(f.current(), f.model()->groupsRoot());
        QVERIFY(f.popup->view()->isExpanded(f.current()));

        // With text, Right just moves the text cursor.
        QTest::keyClick(f.popup->view(), Qt::Key_Up);
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("ab"));
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Left);
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Right);
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
    }

    void enterInSearchCopiesEvenWhenGroupsRowWasSelected()
    {
        GroupFixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down); // Groups row
        QTest::keyClick(f.popup->view(), Qt::Key_Up); // back to search
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.at(0).at(0).value<Entry>().content, QStringLiteral("ssh -J bastion app"));
    }

    void clickTogglesGroups()
    {
        GroupFixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        auto* view = f.popup->view();
        const QModelIndex root = f.model()->groupsRoot();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(root).center());
        QVERIFY(view->isExpanded(root));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(root).center());
        QVERIFY(!view->isExpanded(root));
        QCOMPARE(activated.size(), 0);
    }

    void expansionSurvivesRefreshButNotReopening()
    {
        GroupFixture f;
        QVERIFY(f.show());
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Right);
        f.popup->view()->expand(f.model()->findGroup(QStringLiteral("Work")));
        f.t.repo->ingest(QStringLiteral("fresh"));
        f.popup->historyChanged(); // model rebuilt
        QVERIFY(f.popup->view()->isExpanded(f.model()->groupsRoot()));
        QVERIFY(f.popup->view()->isExpanded(f.model()->findGroup(QStringLiteral("Work"))));

        QTest::keyClick(f.popup->view(), Qt::Key_Escape);
        QVERIFY(f.show());
        QVERIFY(!f.popup->view()->isExpanded(f.model()->groupsRoot()));
    }

    void groupMenuCreatesSubGroup()
    {
        GroupFixture f;
        f.popup->setTextPrompt([](QWidget*, const QString&, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Prod"));
        });
        QVERIFY(f.show());
        std::unique_ptr<QMenu> menu(f.popup->createGroupMenu(QStringLiteral("Work/Servers"), nullptr));
        QAction* create = menu->actions().value(0);
        QCOMPARE(create->text(), QStringLiteral("New sub-group…"));
        create->trigger();
        QVERIFY(f.t.repo->groups().contains(QStringLiteral("Work/Servers/Prod")));
        // The new group is revealed and selected.
        QCOMPARE(f.current(), f.model()->findGroup(QStringLiteral("Work/Servers/Prod")));
        QVERIFY(f.popup->view()->isExpanded(f.model()->findGroup(QStringLiteral("Work/Servers"))));
    }

    void groupsRowMenuCreatesTopLevelGroup()
    {
        GroupFixture f;
        f.popup->setTextPrompt([](QWidget*, const QString&, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Personal"));
        });
        QVERIFY(f.show());
        std::unique_ptr<QMenu> menu(f.popup->createGroupMenu(QString(), nullptr));
        menu->actions().value(0)->trigger();
        QVERIFY(f.t.repo->groups().contains(QStringLiteral("Personal")));
    }

    void altArrowsReorderGroupsButNotFavorites()
    {
        GroupFixture f;
        f.t.repo->createGroup(QStringLiteral("Personal"));
        QVERIFY(f.show());
        auto* view = f.popup->view();
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Right);
        QTest::keyClick(view, Qt::Key_Down); // Favorites
        QTest::keyClick(view, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(f.model()->findGroup(HistoryModel::favoritesPath()).row(), 0); // stays first
        QTest::keyClick(view, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(f.model()->findGroup(HistoryModel::favoritesPath()).row(), 0);

        QTest::keyClick(view, Qt::Key_Down); // Personal (by name, before Work)
        QCOMPARE(f.current(), f.model()->findGroup(QStringLiteral("Personal")));
        QTest::keyClick(view, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(f.t.repo->groups(), (QStringList{"Work", "Work/Servers", "Personal"}));
        QCOMPARE(f.current(), f.model()->findGroup(QStringLiteral("Personal"))); // cursor followed
        QCOMPARE(f.current().row(), 2); // Favorites, Work, Personal
        QTest::keyClick(view, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(f.current().row(), 1);
    }

    void ctrlGRenamesAndDelDeletesAGroup()
    {
        GroupFixture f;
        f.popup->setTextPrompt([](QWidget*, const QString&, const QString&, const QString& current) {
            return current == QLatin1String("Work") ? std::optional<QString>(QStringLiteral("Jobs")) : std::nullopt;
        });
        bool asked = false;
        f.popup->setConfirmPrompt([&](QWidget*, const QString&) {
            asked = true;
            return true;
        });
        QVERIFY(f.show());
        auto* view = f.popup->view();
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Right);
        QTest::keyClick(view, Qt::Key_Down); // Favorites: Ctrl+G and Del do nothing there
        QTest::keyClick(view, Qt::Key_G, Qt::ControlModifier);
        QTest::keyClick(view, Qt::Key_Delete);
        QCOMPARE(f.current(), f.model()->findGroup(HistoryModel::favoritesPath()));
        QTest::keyClick(view, Qt::Key_Down); // Work
        QTest::keyClick(view, Qt::Key_G, Qt::ControlModifier);
        QCOMPARE(f.t.repo->get(f.betaId)->group, QStringLiteral("Jobs"));
        QCOMPARE(f.t.repo->get(f.alphaId)->group, QStringLiteral("Jobs/Servers"));
        QCOMPARE(f.current(), f.model()->findGroup(QStringLiteral("Jobs")));

        QTest::keyClick(view, Qt::Key_Delete);
        QVERIFY(asked);
        QVERIFY(f.t.repo->groups().isEmpty());
        QCOMPARE(f.t.repo->count(), 4); // copies kept
    }

    void renameConflictIsReported()
    {
        GroupFixture f;
        f.t.repo->createGroup(QStringLiteral("Home"));
        f.popup->setTextPrompt([](QWidget*, const QString&, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Home"));
        });
        QString reported;
        f.popup->setNotify([&](QWidget*, const QString& message) { reported = message; });
        QVERIFY(f.show());
        std::unique_ptr<QMenu> menu(f.popup->createGroupMenu(QStringLiteral("Work"), nullptr));
        menu->actions().value(1)->trigger(); // Rename or move…
        QVERIFY(reported.contains(QLatin1String("already exists")));
        QVERIFY(f.t.repo->groups().contains(QStringLiteral("Work")));
    }

    void busyPopupShowsTheNoticeAndBlocksInput()
    {
        Fixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        f.popup->setBusy(QStringLiteral("Encrypting the history…"));
        QCOMPARE(f.model()->rowCount(), 0);
        QVERIFY(!f.popup->searchBar()->isEnabled());
        f.t.repo->ingest(QStringLiteral("arrives meanwhile"));
        f.popup->historyChanged(); // ignored while busy
        QCOMPARE(f.model()->rowCount(), 0);
        QTest::keyClick(f.popup.get(), Qt::Key_Return);
        QCOMPARE(activated.size(), 0);

        f.popup->setBusy(QString());
        QVERIFY(f.popup->searchBar()->isEnabled());
        QCOMPARE(f.model()->entryAt(f.model()->index(3, 0))->content, QStringLiteral("arrives meanwhile"));
    }

    void settingsButtonAndShortcut()
    {
        Fixture f;
        QSignalSpy requested(f.popup.get(), &PopupWindow::settingsRequested);
        QVERIFY(f.show());
        QTest::mouseClick(f.popup->settingsButton(), Qt::LeftButton);
        QCOMPARE(requested.size(), 1);
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Comma, Qt::ControlModifier);
        QCOMPARE(requested.size(), 2);
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
