#include "Config.h"
#include "FakeKeyStore.h"
#include "HistoryModel.h"
#include "PasswordKeeper.h"
#include "db/Repository.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "Prompts.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "TestDb.h"

#include <QAction>
#include <QClipboard>
#include <QApplication>
#include <QBuffer>
#include <QHelpEvent>
#include <QImage>
#include <QPixmap>
#include <QComboBox>
#include <QEnterEvent>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
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

    struct PasswordFixture : Fixture {
        std::shared_ptr<FakeKeyStore> keys = std::make_shared<FakeKeyStore>();
        clipitto::PasswordKeeper keeper{*t.repo, keys};
        qint64 secretId = 0;
        QStringList messages;
        QStringList prompts;
        std::optional<QString> answer = QStringLiteral("master");

        explicit PasswordFixture(Config cfg = {}) : Fixture(cfg)
        {
            keeper.setKdfCost({64, 1});
            bool set = false;
            keeper.setMasterPassword("master", [&set](bool ok) { set = ok; });
            if (!QTest::qWaitFor([&set] { return set; })) {
                qFatal("the master password was not set");
            }
            const auto& v = keeper.vault();
            secretId = t.repo->ingestSecret(v.seal("hunter2"), v.match("hunter2"), QStringLiteral("KeePassXC"),
                                            QString()).id;
            popup->setPasswords(&keeper);
            popup->setNotify([this](QWidget*, const QString& message) { messages << message; });
            popup->setPasswordPrompt([this](QWidget*, const QString& label) {
                prompts << label;
                return answer;
            });
            popup->refresh();
        }

        QModelIndex secretRow() const { return model()->findEntry(secretId); }
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

    void ctrlEInSearchRequestsNote()
    {
        // On its own: inside a popup the signal opens the (modal) note dialog.
        clipitto::ui::SearchBar bar;
        QSignalSpy spy(&bar, &clipitto::ui::SearchBar::editNoteRequested);
        QTest::keyClick(&bar, Qt::Key_E, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);
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

    void clearButtonKeepsSavedCopiesAndItsMenuClearsEverything()
    {
        Fixture f; // ssh (pinned), alpha, beta, gamma ray
        f.t.repo->setGroup(f.t.repo->search(QStringLiteral("alpha")).front().id, QStringLiteral("Work"));
        QStringList asked;
        bool answer = false;
        f.popup->setConfirmPrompt([&](QWidget*, const QString& question) {
            asked << question;
            return answer;
        });
        QVERIFY(f.show());
        QToolButton* clear = f.popup->clearButton();
        QVERIFY(clear->isVisible());
        QCOMPARE(clear->popupMode(), QToolButton::MenuButtonPopup);

        // Cancelled: nothing changes.
        clear->click();
        QCOMPARE(asked.size(), 1);
        QVERIFY2(asked.last().contains(QLatin1String("Clear Recent (3 copies)")), qPrintable(asked.last()));
        QCOMPARE(f.t.repo->count(), 4);

        // The button: beta and gamma ray go; the pinned and the grouped copy stay (the grouped
        // one only leaves Recent).
        answer = true;
        clear->click();
        QCOMPARE(f.t.repo->count(), 2);
        QCOMPARE(f.t.repo->search(QStringLiteral("gamma")).size(), 0u);
        QCOMPARE(QApplication::focusWidget(), f.popup->searchBar());
        QVERIFY(f.t.repo->recent(10, /*unsavedOnly=*/false).size() == 1); // just the pinned one is left

        // The menu's second entry: everything, also in groups.
        const QList<QAction*> actions = clear->menu()->actions();
        QCOMPARE(actions.size(), 2);
        actions.at(1)->trigger();
        QVERIFY2(asked.last().contains(QLatin1String("Delete all 2 copies")), qPrintable(asked.last()));
        QCOMPARE(f.t.repo->count(), 0);
        QCOMPARE(f.t.repo->groups(), QStringList{QStringLiteral("Work")});

        // Nothing left: says so instead of asking.
        QStringList messages;
        f.popup->setNotify([&](QWidget*, const QString& message) { messages << message; });
        clear->click();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(asked.size(), 3);
    }

    void deleteInRecentKeepsAGroupedCopyInItsGroup()
    {
        GroupFixture f; // beta in Work, alpha in Work/Servers; gamma ray and ssh are not grouped
        QVERIFY(f.show());
        f.popup->view()->selectIndex(f.model()->findEntry(f.betaId));
        QCOMPARE(f.currentContent(), QStringLiteral("beta"));
        QVERIFY(!f.current().parent().isValid()); // the one in the Recent list
        f.popup->searchBar()->setFocus();
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Delete);

        const auto beta = f.t.repo->get(f.betaId);
        QVERIFY(beta.has_value()); // not deleted
        QCOMPARE(beta->group, QStringLiteral("Work"));
        const QModelIndex now = f.model()->findEntry(f.betaId);
        QVERIFY(now.isValid());
        QVERIFY(now.parent().isValid()); // only in its group now
        QCOMPARE(f.t.repo->recentUnpinned(10).size(), 2u); // alpha is still on Recent

        // An unfiled copy is deleted as before.
        f.popup->view()->selectIndex(f.model()->findEntry(f.t.repo->search(QStringLiteral("gamma")).front().id));
        f.popup->searchBar()->setFocus();
        QTest::keyClick(f.popup->searchBar(), Qt::Key_Delete);
        QVERIFY(f.t.repo->search(QStringLiteral("gamma")).empty());
    }

    void groupAndFavoritesMenusKeepTheirCopiesOutOfRecent()
    {
        GroupFixture f;
        f.t.repo->setFavorite(f.t.repo->search(QStringLiteral("gamma")).front().id, true);
        QVERIFY(f.show());
        const auto recentIds = [&] {
            QList<qint64> out;
            for (const Entry& e : f.t.repo->recentUnpinned(10)) {
                out << e.id;
            }
            return out;
        };
        QCOMPARE(recentIds().size(), 3u);

        QMenu* group = f.popup->createGroupMenu(QStringLiteral("Work"), f.popup.get());
        QAction* keepOut = nullptr;
        for (QAction* a : group->actions()) {
            if (a->isCheckable()) {
                keepOut = a;
            }
        }
        QVERIFY(keepOut);
        QVERIFY(!keepOut->isChecked());
        keepOut->trigger();
        QVERIFY(f.t.repo->groupHiddenFromRecent(QStringLiteral("Work")));
        QCOMPARE(recentIds().size(), 1u); // only the starred gamma ray, in no group
        delete group;

        QMenu* favorites = f.popup->createFavoritesMenu(f.popup.get());
        QCOMPARE(favorites->actions().size(), 1);
        QVERIFY(favorites->actions().first()->isCheckable());
        favorites->actions().first()->trigger();
        QVERIFY(f.t.repo->favoritesHiddenFromRecent());
        QCOMPARE(recentIds().size(), 0u);
        delete favorites;

        // The group's menu shows the rule as it is now.
        QMenu* again = f.popup->createGroupMenu(QStringLiteral("Work"), f.popup.get());
        bool checked = false;
        for (QAction* a : again->actions()) {
            checked = checked || (a->isCheckable() && a->isChecked());
        }
        QVERIFY(checked);
        delete again;
    }

    void deleteInTheSearchBoxRemovesTheSelectedCopy()
    {
        Fixture f;
        QVERIFY(f.show());
        auto* search = f.popup->searchBar();
        QCOMPARE(f.currentContent(), QStringLiteral("ssh -J bastion app")); // selected on opening
        QTest::keyClick(search, Qt::Key_Down); // gamma ray
        QTest::keyClick(f.popup->view(), Qt::Key_Up); // back on ssh... in the list
        f.popup->searchBar()->setFocus();
        QTest::keyClick(search, Qt::Key_Delete); // empty box: deletes the selected copy
        QCOMPARE(f.t.repo->count(), 3);
        QVERIFY(f.t.repo->search(QStringLiteral("bastion")).empty());
        QCOMPARE(QApplication::focusWidget(), search); // still typing in the search box
        QCOMPARE(f.currentContent(), QStringLiteral("gamma ray")); // the next copy is selected

        // Right after typing, while the search hasn't run yet, Delete first brings the list
        // up to date instead of deleting what the previous search had selected.
        QTest::keyClicks(search, QStringLiteral("alp"));
        QVERIFY(f.popup->controller()->isPending());
        QTest::keyClick(search, Qt::Key_Delete);
        QCOMPARE(f.t.repo->count(), 3);
        QCOMPARE(f.currentContent(), QStringLiteral("alpha"));
        search->clear();

        // With text: Delete at the end removes the selected match, elsewhere it edits.
        QTest::keyClicks(search, QStringLiteral("beta"));
        f.popup->controller()->refresh();
        QCOMPARE(f.currentContent(), QStringLiteral("beta"));
        search->setCursorPosition(1);
        QTest::keyClick(search, Qt::Key_Delete);
        QCOMPARE(search->text(), QStringLiteral("bta")); // edited, nothing deleted
        QCOMPARE(f.t.repo->count(), 3);
    }

    void startFocusPicksWhereTheSelectionStarts()
    {
        // Fixture: ssh (pinned), then Recent: gamma ray, beta, alpha (alpha put in a group).
        const auto start = [](clipitto::StartFocus focus, const QString& group) {
            Config cfg;
            cfg.startFocus = focus;
            auto f = std::make_unique<Fixture>(cfg);
            if (!group.isEmpty()) {
                f->t.repo->setGroup(f->t.repo->search(QStringLiteral("alpha")).front().id, group);
            }
            return f;
        };
        auto pinned = start(clipitto::StartFocus::Pinned, QStringLiteral("Work"));
        QVERIFY(pinned->show());
        QCOMPARE(pinned->currentContent(), QStringLiteral("ssh -J bastion app"));

        auto grouped = start(clipitto::StartFocus::Grouped, QStringLiteral("Work"));
        QVERIFY(grouped->show());
        QCOMPARE(grouped->currentContent(), QStringLiteral("alpha"));

        auto unsaved = start(clipitto::StartFocus::Unsaved, QStringLiteral("Work"));
        QVERIFY(unsaved->show());
        QCOMPARE(unsaved->currentContent(), QStringLiteral("gamma ray"));
        // After a search too: the first unsaved match, not the pinned one listed above it.
        QTest::keyClicks(unsaved->popup->searchBar(), QStringLiteral("a"));
        unsaved->popup->controller()->refresh();
        QVERIFY(!unsaved->model()->entryAt(unsaved->current())->isSaved());
        // Enter from the search box copies that one.
        QSignalSpy activated(unsaved->popup.get(), &PopupWindow::entryActivated);
        QTest::keyClick(unsaved->popup->searchBar(), Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QVERIFY(!activated.at(0).at(0).value<Entry>().isSaved());

        // Nothing of that kind: the first copy.
        auto none = start(clipitto::StartFocus::Grouped, QString());
        QVERIFY(none->show());
        QCOMPARE(none->currentContent(), QStringLiteral("ssh -J bastion app"));
    }

    void unsavedStartFocusPicksTheNewestMatchOfASearch()
    {
        Config cfg;
        cfg.startFocus = clipitto::StartFocus::Unsaved;
        Fixture f(cfg);
        // Relevance would rank the older, denser match first; "first recent" is the newest one.
        f.t.repo->ingest(QStringLiteral("needle needle needle"));
        f.t.repo->ingest(QStringLiteral("a needle in a very long line of other words to dilute it"));
        QVERIFY(f.show());
        QTest::keyClicks(f.popup->searchBar(), QStringLiteral("needle"));
        f.popup->controller()->refresh();
        QCOMPARE(f.currentContent(), QStringLiteral("a needle in a very long line of other words to dilute it"));
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

    void pasteTextFromPictureReadsAndPastesTheText()
    {
        Fixture f;
        QImage thumb(40, 20, QImage::Format_RGB32);
        thumb.fill(Qt::white);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        thumb.save(&buffer, "PNG");
        clipitto::ImageClip clip;
        clip.png = png;
        clip.thumbnail = png;
        clip.width = 40;
        clip.height = 20;
        clip.pixelHash = QStringLiteral("ocr");
        const qint64 id = f.t.repo->ingestImage(clip).id;
        QVERIFY(f.show());
        const Entry picture = *f.model()->entryAt(f.model()->findEntry(id));
        // The picture's "Paste as" menu (also what Shift+Enter opens): the text first, then it changed, then the picture.
        const auto pasteAs = [&](std::unique_ptr<QMenu>& owner) -> QMenu* {
            owner.reset(f.popup->createContextMenu(picture, f.popup.get()));
            for (QAction* a : owner->actions()) {
                if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                    return a->menu();
                }
            }
            return nullptr;
        };
        // Finds "Paste text from picture" (found, enabled) and clicks it when asked.
        const auto entry = [&](bool click) {
            std::unique_ptr<QMenu> owner;
            QMenu* menu = pasteAs(owner);
            if (!menu || menu->actions().isEmpty()) {
                return std::pair<bool, bool>(false, false);
            }
            QAction* a = menu->actions().first(); // first: the most useful for a picture
            if (a->text().remove(QLatin1Char('&')) != QLatin1String("Paste text from picture")) {
                return std::pair<bool, bool>(false, false);
            }
            const std::pair<bool, bool> state(true, a->isEnabled());
            if (click) {
                a->trigger();
            }
            return state;
        };
        // No text recognition on this computer: the entry is there, disabled.
        QCOMPARE(entry(false), (std::pair<bool, bool>(true, false)));

        qint64 askedFor = 0;
        QString reply = QStringLiteral("hello from a screenshot");
        f.popup->setTextFromPicture(
            [&](qint64 pid, std::function<void(const QString&, const QString&)> done) {
                askedFor = pid;
                done(reply, QString());
            },
            true, QString());
        QSignalSpy pasted(f.popup.get(), &PopupWindow::textActivated);
        QCOMPARE(entry(true), (std::pair<bool, bool>(true, true)));
        QCOMPARE(askedFor, id);
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.at(0).at(0).toString(), reply);

        // A picture without text: nothing is pasted, and the user is told.
        QStringList messages;
        f.popup->setNotify([&](QWidget*, const QString& message) { messages << message; });
        reply = QStringLiteral("  \n ");
        entry(true);
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(messages, QStringList{QStringLiteral("No text found in the picture.")});

        // The text changed: "one line" turns the lines into one. And the picture itself can be pasted.
        reply = QStringLiteral("line one\nline two");
        std::unique_ptr<QMenu> owner;
        QMenu* menu = pasteAs(owner);
        QVERIFY(menu);
        QMenu* changed = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->menu()) {
                changed = a->menu();
            }
        }
        QVERIFY(changed);
        for (QAction* a : changed->actions()) {
            if (a->text().remove(QLatin1Char('&')) == QLatin1String("One line")) {
                a->trigger();
            }
        }
        QCOMPARE(pasted.size(), 2);
        QCOMPARE(pasted.at(1).at(0).toString(), QStringLiteral("line one line two"));
        QSignalSpy picturePasted(f.popup.get(), &PopupWindow::entryActivated);
        menu->actions().last()->trigger();
        QCOMPARE(picturePasted.size(), 1);
        QCOMPARE(picturePasted.at(0).at(0).value<Entry>().id, id);
    }

    void pasteAsKeyboardLayoutFollowsTheSetting()
    {
        Fixture f;
        QVERIFY(f.show());
        const Entry beta = *f.model()->entryAt(f.model()->index(4, 0));
        Entry typed = beta;
        typed.content = QStringLiteral("xccv");
        Entry hebrew = beta;
        hebrew.content = QStringLiteral("סבבה");
        const auto texts = [&](const Entry& e) {
            std::unique_ptr<QMenu> menu(f.popup->createContextMenu(e, f.popup.get()));
            QMenu* paste = nullptr;
            for (QAction* a : menu->actions()) {
                if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                    paste = a->menu();
                }
            }
            QStringList out;
            if (paste) {
                for (QAction* a : paste->actions()) {
                    out << a->text().remove(QLatin1Char('&'));
                }
            }
            return out;
        };
        const auto layouts = [&](const char* setting) {
            clipitto::Config c;
            c.keyboardLayouts = QString::fromLatin1(setting);
            f.popup->applyConfig(c);
        };
        const auto hasKeyboard = [](const QStringList& t) {
            return std::any_of(t.begin(), t.end(), [](const QString& s) { return s.contains(QLatin1String("keyboard")); });
        };

        layouts("none");
        QVERIFY(!hasKeyboard(texts(typed)));

        // One layout: a single entry, the opposite of the text's language, with a preview of the result.
        layouts("he");
        QVERIFY(texts(typed).last().contains(QStringLiteral("As Hebrew keyboard: סבבה")));
        QVERIFY(texts(hebrew).last().contains(QStringLiteral("As English keyboard: xccv")));

        // Several: a submenu of them.
        layouts("he, ru");
        std::unique_ptr<QMenu> menu(f.popup->createContextMenu(typed, f.popup.get()));
        QMenu* paste = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                paste = a->menu();
            }
        }
        QVERIFY(paste);
        QMenu* more = nullptr;
        for (QAction* a : paste->actions()) {
            if (a->menu()) {
                more = a->menu();
            }
        }
        QVERIFY(more);
        QCOMPARE(more->actions().size(), 2);
        QSignalSpy pasted(f.popup.get(), &PopupWindow::textActivated);
        more->actions().first()->trigger();
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.at(0).at(0).toString(), QStringLiteral("סבבה"));
    }

    // A fake translation engine: packs zh→en (Han text) and en→zh (Latin text); "translating" upper-cases and prefixes.
    static clipitto::ui::TranslationSupport fakeTranslation(bool withPacks = true)
    {
        clipitto::ui::TranslationSupport s;
        s.builtIn = true;
        const QList<clipitto::ui::TranslationPackInfo> all = withPacks
            ? QList<clipitto::ui::TranslationPackInfo>{{"zh-en", "Chinese → English"}, {"en-zh", "English → Chinese"}}
            : QList<clipitto::ui::TranslationPackInfo>{};
        s.packsFor = [all](const QString& text) {
            QList<clipitto::ui::TranslationPackInfo> out;
            for (const auto& p : all) {
                const bool han = p.id == "zh-en";
                const bool textHan = text.contains(QChar(0x4F60));
                if (text.isEmpty() || han == textHan) {
                    out.push_back(p);
                }
            }
            return out;
        };
        s.fits = [](const QString& id, const QString& text) { return (id == "zh-en") == text.contains(QChar(0x4F60)); };
        s.translate = [](const QString& id, const QString& text, std::function<void(const QString&, const QString&)> done) {
            done(QStringLiteral("[%1] %2").arg(id, text), QString());
        };
        return s;
    }

    void translateMenuOffersThePacksThatFitTheText()
    {
        Fixture f;
        QVERIFY(f.show());
        const Entry beta = *f.model()->entryAt(f.model()->index(4, 0));
        // No translation in the build: nothing about it in the menu.
        {
            std::unique_ptr<QMenu> owner(f.popup->createContextMenu(beta, f.popup.get()));
            for (QAction* a : owner->actions()) {
                if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                    for (QAction* b : a->menu()->actions()) {
                        QVERIFY(b->text().remove(QLatin1Char('&')) != QLatin1String("Translate"));
                    }
                }
            }
        }

        // With packs: a Latin text offers the English pack only; choosing it translates and pastes.
        f.popup->setTranslation(fakeTranslation());
        QSignalSpy pasted(f.popup.get(), &PopupWindow::textActivated);
        {
            std::unique_ptr<QMenu> owner(f.popup->createContextMenu(beta, f.popup.get()));
            QMenu* paste = nullptr;
            for (QAction* a : owner->actions()) {
                if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                    paste = a->menu();
                }
            }
            QVERIFY(paste);
            QMenu* translate = nullptr;
            for (QAction* b : paste->actions()) {
                if (b->menu() && b->text().remove(QLatin1Char('&')) == QLatin1String("Translate")) {
                    translate = b->menu();
                }
            }
            QVERIFY(translate);
            QCOMPARE(translate->actions().size(), 1);
            QCOMPARE(translate->actions().first()->text(), QStringLiteral("English → Chinese"));
            translate->actions().first()->trigger();
        }
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.at(0).at(0).toString(), QStringLiteral("[en-zh] ") + beta.content);

        // No pack installed: the entry is there but disabled, saying why.
        f.popup->setTranslation(fakeTranslation(/*withPacks=*/false));
        std::unique_ptr<QMenu> owner(f.popup->createContextMenu(beta, f.popup.get()));
        QMenu* paste = nullptr;
        for (QAction* a : owner->actions()) {
            if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                paste = a->menu();
            }
        }
        QVERIFY(paste);
        QAction* none = nullptr;
        for (QAction* b : paste->actions()) {
            if (b->text().remove(QLatin1Char('&')) == QLatin1String("Translate")) {
                none = b;
            }
        }
        QVERIFY(none && !none->isEnabled());
        QVERIFY(none->toolTip().contains(QStringLiteral("Settings")));
    }

    void ctrlTShowsTheTranslationWithoutPasting()
    {
        Fixture f;
        QVERIFY(f.show());
        QStringList messages;
        f.popup->setNotify([&](QWidget*, const QString& message) { messages << message; });
        f.popup->setTranslation(fakeTranslation());
        QSignalSpy pasted(f.popup.get(), &PopupWindow::textActivated);
        f.popup->view()->selectIndex(f.model()->findEntry(f.t.repo->search(QStringLiteral("beta")).front().id));
        f.popup->previewTranslation();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.at(0), QStringLiteral("[en-zh] beta"));
        QCOMPARE(pasted.size(), 0);
    }

    void translateTextFromAPicture()
    {
        Fixture f;
        QImage thumb(40, 20, QImage::Format_RGB32);
        thumb.fill(Qt::white);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        thumb.save(&buffer, "PNG");
        clipitto::ImageClip clip;
        clip.png = png;
        clip.thumbnail = png;
        clip.width = 40;
        clip.height = 20;
        clip.pixelHash = QStringLiteral("tr");
        const qint64 id = f.t.repo->ingestImage(clip).id;
        QVERIFY(f.show());
        const Entry picture = *f.model()->entryAt(f.model()->findEntry(id));
        QString read = QString(QChar(0x4F60)) + QStringLiteral("好");
        f.popup->setTextFromPicture(
            [&](qint64, std::function<void(const QString&, const QString&)> done) { done(read, QString()); }, true, QString());
        f.popup->setTranslation(fakeTranslation());
        QStringList messages;
        f.popup->setNotify([&](QWidget*, const QString& m) { messages << m; });
        QSignalSpy pasted(f.popup.get(), &PopupWindow::textActivated);
        const auto choose = [&](const QString& packLabel) {
            std::unique_ptr<QMenu> owner(f.popup->createContextMenu(picture, f.popup.get()));
            for (QAction* a : owner->actions()) {
                if (!a->menu() || a->text().remove(QLatin1Char('&')) != QLatin1String("Paste as")) {
                    continue;
                }
                for (QAction* b : a->menu()->actions()) {
                    if (b->menu() && b->text().remove(QLatin1Char('&')) == QLatin1String("Translate text from picture")) {
                        for (QAction* p : b->menu()->actions()) {
                            if (p->text() == packLabel) {
                                p->trigger();
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        };
        // Both packs are offered for a picture (its text is not known yet); the fitting one translates.
        QVERIFY(choose(QStringLiteral("Chinese → English")));
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.at(0).at(0).toString(), QStringLiteral("[zh-en] ") + read);
        // A pack for another language than the text: nothing is pasted, the user is told.
        QVERIFY(choose(QStringLiteral("English → Chinese")));
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(messages.size(), 1);
    }

    void pasteAsMenuIsOrangeAndDarkerOnALightTheme()
    {
        Fixture f;
        QVERIFY(f.show());
        const Entry beta = *f.model()->entryAt(f.model()->index(4, 0));
        const auto styleFor = [&](const QColor& window) {
            QPalette p = f.popup->palette();
            p.setColor(QPalette::Window, window);
            f.popup->setPalette(p);
            std::unique_ptr<QMenu> menu(f.popup->createContextMenu(beta, f.popup.get()));
            for (QAction* a : menu->actions()) {
                if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                    return a->menu()->styleSheet();
                }
            }
            return QString();
        };
        const QString dark = styleFor(QColor(30, 30, 30));
        const QString light = styleFor(QColor(250, 250, 250));
        QVERIFY(dark.contains(QStringLiteral("#e08a1e")));
        QVERIFY(dark.contains(QStringLiteral("border: 1px solid")));
        QVERIFY(light.contains(QStringLiteral("#b85c00")));
        QVERIFY(!light.contains(QStringLiteral("#e08a1e")));
        QVERIFY(light.contains(QStringLiteral("border: 1px solid")));
    }

    void pasteAsTransformsTheTextAndLeavesTheCopyAlone()
    {
        Fixture f;
        QVERIFY(f.show());
        const Entry beta = *f.model()->entryAt(f.model()->index(4, 0));
        std::unique_ptr<QMenu> menu(f.popup->createContextMenu(beta, nullptr));
        QMenu* sub = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                sub = a->menu();
            }
        }
        QVERIFY(sub);
        QSignalSpy pasted(f.popup.get(), &PopupWindow::textActivated);
        QSignalSpy plain(f.popup.get(), &PopupWindow::entryActivated);
        for (QAction* a : sub->actions()) {
            if (a->text().remove(QLatin1Char('&')) == QLatin1String("UPPER CASE")) {
                a->trigger();
            }
        }
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(pasted.at(0).at(0).toString(), beta.content.toUpper());
        QCOMPARE(plain.size(), 0); // the stored copy is not "used"
        QCOMPARE(f.t.repo->get(beta.id)->content, beta.content);

        // A picture's "Paste as" has its own choices (text from the picture first), not the text transforms.
        Entry picture = beta;
        picture.image = true;
        std::unique_ptr<QMenu> pictureMenu(f.popup->createContextMenu(picture, nullptr));
        QMenu* pictureChoices = nullptr;
        for (QAction* a : pictureMenu->actions()) {
            if (a->menu() && a->text().remove(QLatin1Char('&')) == QLatin1String("Paste as")) {
                pictureChoices = a->menu();
            }
        }
        QVERIFY(pictureChoices);
        QCOMPARE(pictureChoices->actions().first()->text().remove(QLatin1Char('&')), QStringLiteral("Paste text from picture"));
        for (QAction* a : pictureChoices->actions()) {
            QVERIFY(a->text().remove(QLatin1Char('&')) != QLatin1String("UPPER CASE"));
        }
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

    // --- Password copies ----------------------------------------------------------------

    void namingANewPassword()
    {
        PasswordFixture f;
        QStringList suggestions;
        std::optional<QString> answer;
        bool openedForIt = false;
        qint64 selected = 0;
        f.popup->setNamePrompt([&](QWidget*, const QString& suggestion) {
            suggestions << suggestion;
            openedForIt = f.popup->isVisible(); // the popup is open while the box is
            const Entry* e = f.model()->entryAt(f.current());
            selected = e ? e->id : 0; // ... with the new copy selected
            return answer;
        });
        const auto newPassword = [&](const char* match, const QString& note) {
            const auto& v = f.keeper.vault();
            return f.t.repo->ingestSecret(v.seal(match), v.match(match), note, QString()).id;
        };

        // Closed popup: it opens for the box, and closes again (the focus goes back to the user).
        const qint64 first = newPassword("one", QStringLiteral("Firefox"));
        answer = QStringLiteral("Bank login");
        f.popup->askPasswordNote(first, QStringLiteral("Firefox"));
        QTRY_COMPARE(suggestions.size(), 1);
        QCOMPARE(suggestions.at(0), QStringLiteral("Firefox")); // the placeholder: where it came from
        QVERIFY(openedForIt);
        QCOMPARE(selected, first);
        QTRY_VERIFY(!f.popup->isVisible());
        QCOMPARE(f.t.repo->get(first)->note, QStringLiteral("Bank login"));

        // Empty keeps the default note (the app); cancelled does too.
        const qint64 second = newPassword("two", QStringLiteral("Chrome"));
        answer = QString();
        f.popup->askPasswordNote(second, QStringLiteral("Chrome"));
        QTRY_COMPARE(suggestions.size(), 2);
        QTRY_VERIFY(!f.popup->isVisible());
        QCOMPARE(f.t.repo->get(second)->note, QStringLiteral("Chrome"));
        const qint64 third = newPassword("three", QStringLiteral("Safari"));
        answer = std::nullopt;
        f.popup->askPasswordNote(third, QStringLiteral("Safari"));
        QTRY_COMPARE(suggestions.size(), 3);
        QTRY_VERIFY(!f.popup->isVisible());
        QCOMPARE(f.t.repo->get(third)->note, QStringLiteral("Safari"));

        // Even if the copy has no note yet, nothing typed (OK, Enter or Cancel) saves the
        // suggestion as its note: the default is the app it came from.
        const qint64 bare = newPassword("bare", QString());
        QVERIFY(f.t.repo->get(bare)->note.isEmpty());
        answer = QString();
        f.popup->askPasswordNote(bare, QStringLiteral("Firefox"));
        QTRY_COMPARE(suggestions.size(), 4);
        QTRY_COMPARE(f.t.repo->get(bare)->note, QStringLiteral("Firefox"));
        const qint64 bareCancelled = newPassword("bare2", QString());
        answer = std::nullopt;
        f.popup->askPasswordNote(bareCancelled, QStringLiteral("Edge"));
        QTRY_COMPARE(suggestions.size(), 5);
        QTRY_COMPARE(f.t.repo->get(bareCancelled)->note, QStringLiteral("Edge"));
        answer = QStringLiteral("   "); // only spaces count as nothing too
        const qint64 spaces = newPassword("bare3", QString());
        f.popup->askPasswordNote(spaces, QStringLiteral("Brave"));
        QTRY_COMPARE(suggestions.size(), 6);
        QTRY_COMPARE(f.t.repo->get(spaces)->note, QStringLiteral("Brave"));

        // A popup that was open stays open.
        QVERIFY(f.show());
        const qint64 fourth = newPassword("four", QString());
        answer = QStringLiteral("  Wi-Fi  ");
        f.popup->askPasswordNote(fourth, QString()); // source unknown: no suggestion
        QTRY_COMPARE(suggestions.size(), 7);
        QCOMPARE(suggestions.at(6), QString());
        QTRY_VERIFY(f.t.repo->get(fourth)->note == QStringLiteral("Wi-Fi"));
        QVERIFY(f.popup->isVisible());
    }

    void onlyOneNameBoxAtATime()
    {
        PasswordFixture f;
        int asked = 0;
        qint64 other = 0;
        f.popup->setNamePrompt([&](QWidget*, const QString&) {
            ++asked;
            f.popup->askPasswordNote(other, QStringLiteral("again")); // a second copy while the box is open
            return std::optional<QString>(QStringLiteral("name"));
        });
        const auto& v = f.keeper.vault();
        const qint64 a = f.t.repo->ingestSecret(v.seal("a"), v.match("a"), QStringLiteral("A"), QString()).id;
        other = f.t.repo->ingestSecret(v.seal("b"), v.match("b"), QStringLiteral("B"), QString()).id;
        f.popup->askPasswordNote(a, QStringLiteral("A"));
        f.popup->askPasswordNote(other, QStringLiteral("B")); // before the first box is even open
        QTRY_VERIFY(!f.popup->isVisible() && asked >= 1);
        QTest::qWait(100);
        QCOMPARE(asked, 1); // the second kept its default note
        QCOMPARE(f.t.repo->get(other)->note, QStringLiteral("B"));
        // ... and the next copy can be named again.
        f.popup->askPasswordNote(other, QStringLiteral("B"));
        QTRY_COMPARE(asked, 2);
    }

    void passwordsInThePasswordsGroupCanStayOutOfRecent()
    {
        const auto listedInRecent = [](PasswordFixture& f, qint64 id) {
            for (int row = 0; row < f.model()->rowCount(); ++row) {
                const Entry* e = f.model()->entryAt(f.model()->index(row, 0));
                if (e && e->id == id) {
                    return true;
                }
            }
            return false;
        };
        for (const bool inRecent : {false, true}) {
            Config c;
            c.passwordsInGroup = true;
            PasswordFixture f(c);
            f.t.repo->setGroupHiddenFromRecent(clipitto::PasswordKeeper::passwordsGroup(), !inRecent);
            const auto& v = f.keeper.vault();
            const qint64 grouped = f.t.repo->ingestSecret(v.seal("g"), v.match("g"), QStringLiteral("Bank"),
                                                          clipitto::PasswordKeeper::passwordsGroup()).id;
            f.popup->refresh();
            QCOMPARE(listedInRecent(f, grouped), inRecent);
            QVERIFY(f.model()->findEntry(grouped, clipitto::PasswordKeeper::passwordsGroup()).isValid()); // in the group
            QVERIFY(listedInRecent(f, f.secretId)); // a password that is in no group is listed either way
        }
    }

    void passwordRowsAreHidden()
    {
        PasswordFixture f;
        QVERIFY(f.show());
        const QModelIndex row = f.secretRow();
        QVERIFY(row.isValid());
        QVERIFY(row.data(HistoryModel::SecretRole).toBool());
        QCOMPARE(row.data(HistoryModel::TitleRole).toString(), QStringLiteral("KeePassXC"));
        QCOMPARE(row.data(HistoryModel::PreviewRole).toString(), HistoryModel::hiddenPassword());
        QVERIFY(!row.data(Qt::ToolTipRole).isValid());
    }

    void enterOnALockedPasswordAsksFirst()
    {
        PasswordFixture f;
        QVERIFY(f.show());
        QSignalSpy activated(f.popup.get(), &PopupWindow::entryActivated);
        f.popup->view()->selectIndex(f.secretRow());
        f.enterList();

        f.answer = std::nullopt; // cancelled: nothing happens
        QTest::keyClick(f.popup->view(), Qt::Key_Return);
        QCOMPARE(f.prompts.size(), 1);
        QCOMPARE(activated.size(), 0);
        QVERIFY(f.popup->isVisible());

        f.answer = QStringLiteral("wrong");
        QTest::keyClick(f.popup->view(), Qt::Key_Return);
        QTRY_COMPARE(f.messages.size(), 1);
        QCOMPARE(activated.size(), 0);

        f.answer = QStringLiteral("master");
        QTest::keyClick(f.popup->view(), Qt::Key_Return);
        QTRY_COMPARE(activated.size(), 1);
        QVERIFY(activated.at(0).at(0).value<Entry>().secret);
        QVERIFY(f.keeper.isUnlocked());
        QVERIFY(!f.popup->isVisible());

        // Unlocked: no more questions.
        QVERIFY(f.show());
        f.popup->view()->selectIndex(f.secretRow());
        f.enterList();
        QTest::keyClick(f.popup->view(), Qt::Key_Return);
        QCOMPARE(activated.size(), 2);
        QCOMPARE(f.prompts.size(), 3);
    }

    void showPasswordRevealsForAWhile()
    {
        PasswordFixture f;
        QVERIFY(f.show());
        f.popup->revealPassword(f.secretId);
        QTRY_COMPARE(f.secretRow().data(HistoryModel::PreviewRole).toString(), QStringLiteral("hunter2"));
        QVERIFY(f.model()->hasRevealed());
        // Locking hides it again; so does closing the popup.
        f.keeper.lock();
        QCOMPARE(f.secretRow().data(HistoryModel::PreviewRole).toString(), HistoryModel::hiddenPassword());
        f.answer = QStringLiteral("master");
        f.popup->revealPassword(f.secretId);
        QTRY_VERIFY(f.model()->hasRevealed());
        f.popup->hidePopup();
        QVERIFY(!f.model()->hasRevealed());
    }

    void contextMenuMarksAndUnmarks()
    {
        PasswordFixture f;
        QVERIFY(f.show());
        const qint64 alpha = f.t.repo->search(QStringLiteral("alpha")).front().id;
        const auto actionNamed = [](QMenu* menu, const QString& text) {
            for (QAction* a : menu->actions()) {
                if (a->text() == text) {
                    return a;
                }
            }
            return static_cast<QAction*>(nullptr);
        };
        std::unique_ptr<QMenu> menu(f.popup->createContextMenu(*f.t.repo->get(alpha), f.popup.get()));
        QAction* mark = actionNamed(menu.get(), QStringLiteral("Mark as password"));
        QVERIFY(mark && mark->isEnabled());
        mark->trigger();
        QVERIFY(f.t.repo->get(alpha)->secret);

        menu.reset(f.popup->createContextMenu(*f.t.repo->get(alpha), f.popup.get()));
        QVERIFY(!actionNamed(menu.get(), QStringLiteral("Mark as password")));
        QVERIFY(actionNamed(menu.get(), QStringLiteral("Show password")));
        actionNamed(menu.get(), QStringLiteral("Not a password (keep as text)"))->trigger();
        QTRY_VERIFY(!f.t.repo->get(alpha)->secret);
        QCOMPARE(f.t.repo->get(alpha)->content, QStringLiteral("alpha"));

        // Without a master password the action is there but disabled.
        Fixture plain;
        std::unique_ptr<QMenu> other(plain.popup->createContextMenu(*plain.t.repo->get(alpha), plain.popup.get()));
        QAction* disabled = actionNamed(other.get(), QStringLiteral("Mark as password"));
        QVERIFY(disabled && !disabled->isEnabled());
    }

    void keywordStartsAMaskedPasswordSearch()
    {
        PasswordFixture f;
        QVERIFY(f.show());
        auto* search = f.popup->searchBar();
        QTest::keyClicks(search, QStringLiteral("p[] "));
        QVERIFY(search->isPasswordMode());
        QTRY_COMPARE(f.model()->rowCount(), 1); // only password copies, all of them while empty
        QVERIFY(f.model()->entryAt(f.model()->index(0, 0))->secret);

        QTest::keyClicks(search, QStringLiteral("hunt"));
        QCOMPARE(search->text(), QStringLiteral("p[] ****")); // never the typed text
        QCOMPARE(search->passwordQuery(), QStringLiteral("hunt"));
        // Locked: part of a password doesn't match; Enter offers the unlock, then it does.
        QTRY_VERIFY(!f.popup->controller()->isPending());
        QCOMPARE(f.model()->rowCount(), 0);
        QVERIFY(f.popup->controller()->passwordSearchNeedsUnlock());
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(f.model()->rowCount(), 1);
        QCOMPARE(f.prompts.size(), 1);

        // Backspace edits the hidden text, then leaves the password search.
        for (int i = 0; i < 4; ++i) {
            QTest::keyClick(search, Qt::Key_Backspace);
        }
        QCOMPARE(search->text(), QStringLiteral("p[] "));
        QTest::keyClick(search, Qt::Key_Backspace);
        QVERIFY(!search->isPasswordMode());
        QCOMPARE(search->text(), QStringLiteral("p[]"));

        // Pasted in one go (Ctrl+V): it never enters the line edit.
        search->clear();
        QGuiApplication::clipboard()->setText(QStringLiteral("p[] hunter2"));
        QSignalSpy edits(search, &QLineEdit::textChanged);
        QTest::keyClick(search, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(search->isPasswordMode());
        QCOMPARE(search->passwordQuery(), QStringLiteral("hunter2"));
        for (const auto& args : edits) {
            QVERIFY(!args.at(0).toString().contains(QLatin1String("hunter2")));
        }
        search->clear();
        QVERIFY(!search->isPasswordMode());

        // Set programmatically in one go: masked right away. Closing wipes it.
        search->setText(QStringLiteral("p[] hunter2"));
        QVERIFY(search->isPasswordMode());
        QCOMPARE(search->text(), QStringLiteral("p[] *******"));
        f.popup->hidePopup();
        QVERIFY(!search->isPasswordMode());
        QVERIFY(search->passwordQuery().isEmpty());
    }
};

QTEST_MAIN(TestPopup)
#include "tst_popup.moc"
