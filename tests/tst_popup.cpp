#include "Config.h"
#include "HistoryModel.h"
#include "db/Repository.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "Prompts.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "TestDb.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QHelpEvent>
#include <QImage>
#include <QPixmap>
#include <QComboBox>
#include <QEnterEvent>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
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

        // Keyboard focus into the list, the cursor staying where it is (the latest copy).
        void enterList() { popup->view()->setFocus(); }

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

    void arrowsFromSearchStepFromTheLatestCopy()
    {
        // Opened: the cursor is on the latest copy. Down goes to the next copy...
        Fixture f;
        QVERIFY(f.show());
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app"));
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Down);
        QCOMPARE(QApplication::focusWidget(), f.popup->view());
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray")); // over the Recent header

        // ...and Up goes to what is above it: the Groups row, when there are groups.
        GroupFixture g;
        QVERIFY(g.show());
        QCOMPARE(g.currentContent(), QStringLiteral("ssh -J bastion app"));
        QTest::keyClick(g.popup->searchBar(), Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), g.popup->view());
        QCOMPARE(g.current(), g.model()->groupsRoot());

        // With nothing above, Up stays in the search box.
        Fixture h;
        QVERIFY(h.show());
        QTest::keyClick(h.popup->searchBar(), Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), h.popup->searchBar());
        QCOMPARE(h.currentContent(), QStringLiteral("ssh -J bastion app"));
    }

    void arrowsTraverseAndSkipHeaders()
    {
        Fixture f;
        QVERIFY(f.show());
        f.enterList();
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
        f.enterList();
        QTest::keyClick(f.popup->view(), Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
    }

    void typingInListGoesToSearch()
    {
        Fixture f;
        QVERIFY(f.show());
        f.enterList();
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
        f.enterList();
        QTest::keyClick(f.popup->view(), Qt::Key_Escape);
        QVERIFY(!f.popup->isVisible());
    }

    void enterActivatesSelectedEntryAndHides()
    {
        Fixture f;
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        QVERIFY(f.show());
        f.enterList();
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
        Config cfg;
        cfg.popupPosition = clipitto::PopupPosition::Last; // reopen where it was left
        Fixture f(cfg);
        QVERIFY(f.show());
        QWidget* handle = f.popup->dragHandle();
        QCOMPARE(qobject_cast<QLabel*>(handle)->text(), QStringLiteral("ClipItTo"));
        // Small enough for any test screen (the Windows runner's offscreen screen is narrower
        // than the popup), then dragged to the middle: a place it fits, so it reopens there.
        f.popup->resize(200, 150);
        const QRect area = f.popup->screen()->availableGeometry();
        if (f.popup->width() >= area.width() || f.popup->height() >= area.height()) {
            QSKIP(qPrintable(QStringLiteral("the popup (%1x%2 at least) doesn't fit this test screen (%3x%4)")
                                 .arg(f.popup->width()).arg(f.popup->height()).arg(area.width()).arg(area.height())));
        }
        const QPoint before = f.popup->pos();
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

    void popupPositionModes_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("expected"); // which corner of the work area it hugs
        QTest::newRow("top-left") << int(clipitto::PopupPosition::TopLeft) << QStringLiteral("tl");
        QTest::newRow("top-right") << int(clipitto::PopupPosition::TopRight) << QStringLiteral("tr");
        QTest::newRow("bottom-left") << int(clipitto::PopupPosition::BottomLeft) << QStringLiteral("bl");
        QTest::newRow("bottom-right") << int(clipitto::PopupPosition::BottomRight) << QStringLiteral("br");
        QTest::newRow("center") << int(clipitto::PopupPosition::Center) << QStringLiteral("center");
    }

    void popupPositionModes()
    {
        QFETCH(int, mode);
        QFETCH(QString, expected);
        Config cfg;
        cfg.popupPosition = clipitto::PopupPosition(mode);
        Fixture f(cfg);
        f.popup->resize(200, 150); // fits any test screen
        QVERIFY(f.show());
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect r(f.popup->pos(), f.popup->size());
        // 5% of the screen's width / height away from the sides it is near.
        const int mx = area.width() * 5 / 100;
        const int my = area.height() * 5 / 100;
        if (expected == QLatin1String("tl")) {
            QCOMPARE(r.topLeft(), area.topLeft() + QPoint(mx, my));
        } else if (expected == QLatin1String("tr")) {
            QCOMPARE(QPoint(r.right(), r.top()), QPoint(area.right() - mx, area.top() + my));
        } else if (expected == QLatin1String("bl")) {
            QCOMPARE(QPoint(r.left(), r.bottom()), QPoint(area.left() + mx, area.bottom() - my));
        } else if (expected == QLatin1String("br")) {
            QCOMPARE(r.bottomRight(), area.bottomRight() - QPoint(mx, my));
        } else {
            QVERIFY(qAbs(r.center().x() - area.center().x()) <= 1);
        }

        // A position left by dragging doesn't override these modes.
        f.popup->move(area.topLeft() + QPoint(3, 3));
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Escape);
        QVERIFY(f.show());
        QCOMPARE(QRect(f.popup->pos(), f.popup->size()), r);
    }

    void popupLargerThanTheScreenOpensAtItsCorner()
    {
        Fixture f;
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        f.popup->resize(area.width() + 300, area.height() + 300);
        QVERIFY(f.show());
        QCOMPARE(f.popup->pos(), area.topLeft());
    }

    void pictureRowsShowAThumbnailAndPreviewOnHover()
    {
        Fixture f;
        QImage thumb(160, 90, QImage::Format_RGB32);
        thumb.fill(Qt::darkCyan);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        thumb.save(&buffer, "PNG");
        clipitto::ImageClip clip;
        clip.png = png;
        clip.thumbnail = png;
        clip.width = 1600;
        clip.height = 900;
        clip.pixelHash = QStringLiteral("abc");
        const qint64 id = f.t.repo->ingestImage(clip).id;
        QVERIFY(f.show());

        const QModelIndex image = f.model()->findEntry(id); // first in Recent (newest)
        QVERIFY(f.model()->entryAt(image)->image);
        QCOMPARE(image.data(HistoryModel::TitleRole).toString(), QStringLiteral("Image 1600×900"));
        QVERIFY(!image.data(HistoryModel::ThumbnailRole).value<QPixmap>().isNull());
        // At least two lines tall, even with one preview line per copy.
        auto* view = f.popup->view();
        const QModelIndex text = f.model()->findEntry(f.t.repo->search(QStringLiteral("gamma")).front().id);
        QVERIFY(view->visualRect(image).height() > view->visualRect(text).height());

        // Hovering shows the larger preview; a text row shows none.
        QHelpEvent hover(QEvent::ToolTip, view->visualRect(image).center(),
                         view->viewport()->mapToGlobal(view->visualRect(image).center()));
        QCoreApplication::sendEvent(view->viewport(), &hover);
        QVERIFY(view->imagePreview() && view->imagePreview()->isVisible());
        QHelpEvent elsewhere(QEvent::ToolTip, view->visualRect(text).center(),
                             view->viewport()->mapToGlobal(view->visualRect(text).center()));
        QCoreApplication::sendEvent(view->viewport(), &elsewhere);
        QVERIFY(!view->imagePreview()->isVisible());
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
        f.enterList();
        QTest::keyClick(f.popup->view(), Qt::Key_Down); // gamma ray
        QTest::keyClick(f.popup->view(), Qt::Key_S, Qt::ControlModifier);

        const auto gammaId = f.t.repo->search(QStringLiteral("gamma")).front().id;
        QVERIFY(f.t.repo->get(gammaId)->favorite);
        // It is in Favorites, the first group, and stays in Recent with the cursor on it.
        const QModelIndex favorites = f.model()->findGroup(HistoryModel::favoritesPath());
        QVERIFY(favorites.isValid());
        QCOMPARE(favorites.parent(), f.model()->groupsRoot());
        QCOMPARE(favorites.row(), 0);
        QCOMPARE(f.model()->rowCount(favorites), 1);
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray"));
        QVERIFY(!f.current().parent().isValid());
        QCOMPARE(f.model()->entryAt(f.model()->index(0, 0, favorites))->content, QStringLiteral("gamma ray"));

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
        f.enterList(); // the Groups row
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
        f.enterList(); // ssh (pinned)
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
        f.enterList();
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
        f.enterList();
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

    void groupPromptArrowKeysReachEveryGroup_data()
    {
        QTest::addColumn<QStringList>("groups");
        QTest::addColumn<QString>("current");
        QTest::addColumn<int>("downs");
        QTest::addColumn<QString>("expected");
        const QStringList two{QStringLiteral("Home"), QStringLiteral("Work")};
        QTest::newRow("one group, none yet") << QStringList{QStringLiteral("Work")} << QString() << 1
                                             << QStringLiteral("Work");
        QTest::newRow("first of two, none yet") << two << QString() << 1 << QStringLiteral("Home");
        QTest::newRow("second of two, none yet") << two << QString() << 2 << QStringLiteral("Work");
        QTest::newRow("next after the current one") << two << QStringLiteral("Home") << 1 << QStringLiteral("Work");
    }

    void groupPromptArrowKeysReachEveryGroup()
    {
        QFETCH(QStringList, groups);
        QFETCH(QString, current);
        QFETCH(int, downs);
        QFETCH(QString, expected);
        bool driven = false;
        // Runs inside the dialog's event loop. Closes the dialog whatever happens (no hang).
        QTimer::singleShot(0, [&] {
            QWidget* modal = QApplication::activeModalWidget();
            if (modal && QTest::qWaitForWindowActive(modal)) {
                if (auto* combo = modal->findChild<QComboBox*>()) {
                    for (int i = 0; i < downs; ++i) {
                        QTest::keyClick(combo, Qt::Key_Down);
                    }
                    QTest::keyClick(combo, Qt::Key_Return);
                    driven = true;
                    return;
                }
            }
            if (modal) {
                modal->close();
            }
        });
        const std::optional<QString> chosen = clipitto::ui::askGroup(nullptr, current, groups);
        QVERIFY(driven);
        QVERIFY(chosen);
        QCOMPARE(*chosen, expected);
    }

    void cancelledPromptChangesNothing()
    {
        Fixture f;
        f.popup->setNotePrompt([](QWidget*, const QString&, const QStringList&) { return std::optional<QString>(); });
        QVERIFY(f.show());
        f.enterList();
        QTest::keyClick(f.popup->view(), Qt::Key_E, Qt::ControlModifier);
        QVERIFY(f.t.repo->get(f.pinnedId)->note.isEmpty());
    }

    void deleteRemovesAndSelectsNeighbor()
    {
        Fixture f;
        QVERIFY(f.show());
        f.enterList();
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
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Up);
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
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Up);
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
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Up); // Groups row (above the selected first copy)
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

        // A move the platform refused (no compositor here) doesn't keep it open afterwards.
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));
        QVERIFY(!popup.startSystemMove());
        other.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTRY_VERIFY(!popup.isVisible());
    }

    // Wayland: the compositor moves the popup and may hold the focus meanwhile.
    void compositorMoveKeepsThePopupOpenUntilItEnds()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("x"));
        Config cfg;
        cfg.hideOnFocusLoss = true;
        PopupWindow popup(*t.repo, cfg);
        popup.setSystemMoveStarter([](QWindow*) { return true; });
        QWidget other;
        const auto focusOther = [&other] {
            other.show();
            other.activateWindow();
            return QTest::qWaitForWindowActive(&other);
        };
        const auto reactivatePopup = [&popup] {
            popup.activateWindow();
            return QTest::qWaitForWindowActive(&popup);
        };

        // Focus taken during the move, and given back after it: stays open.
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));
        QVERIFY(popup.startSystemMove());
        QVERIFY(focusOther());
        QTest::qWait(50);
        QVERIFY(popup.isVisible());
        QVERIFY(reactivatePopup());
        // ...and the move is over: a later focus loss closes it as usual.
        QVERIFY(focusOther());
        QTRY_VERIFY(!popup.isVisible());

        // Focus taken and never given back: closes once the pointer is back on it.
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));
        QVERIFY(popup.startSystemMove());
        QVERIFY(focusOther());
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&popup, &leave);
        QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
        QCoreApplication::sendEvent(&popup, &enter);
        QVERIFY(popup.isVisible()); // gives the focus a moment to come back
        QTRY_VERIFY_WITH_TIMEOUT(!popup.isVisible(), PopupWindow::kSystemMoveSettleMs + 2000);

        // Focus kept during the move: the pointer coming back ends it, so a later focus loss
        // closes the popup as usual (instead of being taken for part of the move).
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowActive(&popup));
        QVERIFY(popup.startSystemMove());
        QCoreApplication::sendEvent(&popup, &leave);
        QCoreApplication::sendEvent(&popup, &enter);
        QTest::qWait(PopupWindow::kSystemMoveSettleMs + 100);
        QVERIFY(popup.isVisible()); // still active: nothing to close for
        QVERIFY(focusOther());
        QTRY_VERIFY(!popup.isVisible());
    }
};

QTEST_MAIN(TestPopup)
#include "tst_popup.moc"
