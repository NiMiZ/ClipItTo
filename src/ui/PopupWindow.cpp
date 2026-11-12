#include "PopupWindow.h"

#include "AppIcon.h"
#include "Config.h"
#include "GroupPath.h"
#include "HistoryDelegate.h"
#include "HistoryView.h"
#include "MacApp.h"
#include "PasswordKeeper.h"
#include "NumberText.h"
#include "PasteAs.h"
#include "PlatformKeys.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "TextTransform.h"
#include "Theme.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCursor>
#include <QHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmapCache>
#include <QPointer>
#include <QPushButton>
#include <QMessageBox>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <memory>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcPopup, "clipitto.popup")

namespace clipitto::ui {

namespace {

using Kind = HistoryModel::Kind;

// Keeps the popup open while a dialog or menu has focus.
// A quiet window for a text that is not a problem: no warning icon, no beep (QMessageBox::warning has both). The
// text can be selected and copied; with `offerPaste` a Paste button is there too.
bool defaultTextViewer(QWidget* parent, const QString& text, bool offerPaste)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("ClipItTo"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text, &dialog);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setMinimumWidth(320);
    label->setMaximumWidth(560);
    layout->addWidget(label);
    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* paste = offerPaste ? buttons->addButton(QObject::tr("Paste"), QDialogButtonBox::AcceptRole) : nullptr;
    QPushButton* close = buttons->addButton(QObject::tr("Close"), QDialogButtonBox::RejectRole);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    (paste ? paste : close)->setDefault(true);
    return dialog.exec() == QDialog::Accepted;
}

class ModalGuard {
public:
    explicit ModalGuard(int& depth) : m_depth(depth) { ++m_depth; }
    ~ModalGuard() { --m_depth; }
    ModalGuard(const ModalGuard&) = delete;
    ModalGuard& operator=(const ModalGuard&) = delete;

private:
    int& m_depth;
};

// The app name at the start of the search row. Dragging it with the left button moves the
// popup (a frameless window has no title bar to drag).
class DragHandle : public QLabel {
public:
    explicit DragHandle(QWidget* parent) : QLabel(parent)
    {
        setCursor(Qt::OpenHandCursor);
        setToolTip(PopupWindow::tr("Drag to move the window"));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QLabel::mousePressEvent(event);
            return;
        }
        m_grab = event->globalPosition().toPoint() - window()->pos();
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_dragging) {
            return;
        }
        if (QGuiApplication::platformName() == QLatin1String("wayland")) {
            // Only the compositor may move a Wayland window; it takes over the drag.
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
            if (auto* popup = qobject_cast<PopupWindow*>(window())) {
                popup->startSystemMove();
            } else if (QWindow* handle = window()->windowHandle()) {
                handle->startSystemMove();
            }
            return;
        }
        // Moved by us, not the window manager, so the popup keeps keyboard focus.
        window()->move(event->globalPosition().toPoint() - m_grab);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        QLabel::mouseReleaseEvent(event);
    }

private:
    QPoint m_grab; // pointer position relative to the window's top-left corner
    bool m_dragging = false;
};

// `topLeft` moved so that a window of `size` stays inside `area`; a window larger than the
// area keeps its top-left corner on it (qBound would be given min > max).
QPoint keepOnScreen(QPoint topLeft, const QSize& size, const QRect& area)
{
    topLeft.setX(std::max(area.left(), std::min(topLeft.x(), area.right() + 1 - size.width())));
    topLeft.setY(std::max(area.top(), std::min(topLeft.y(), area.bottom() + 1 - size.height())));
    return topLeft;
}

// "" stands for the Groups row in the expanded set.
QString expandKey(const HistoryModel& model, const QModelIndex& index)
{
    return model.kindAt(index) == Kind::GroupsRoot ? QString() : model.pathAt(index);
}

// Why a star, pin or group change was refused; empty when it was done. `group` is where
// the star would have gone.
QString refusal(db::Repository::Change change, const db::Repository::Limits& limits, const QString& group)
{
    switch (change) {
    case db::Repository::Change::Done:
        break;
    case db::Repository::Change::FavoritesFull:
        return PopupWindow::tr("Favorites already has %1 starred copies. Unstar one first, or allow more "
                               "in Settings (up to 20).")
            .arg(limits.favorites);
    case db::Repository::Change::GroupStarsFull:
        return PopupWindow::tr("%1 already has %2 starred copies. Unstar one there first.")
            .arg(groups::display(group))
            .arg(limits.starsPerGroup);
    case db::Repository::Change::PinsFull:
        return PopupWindow::tr("You can pin up to %1 copies. Unpin one first.").arg(limits.pins);
    }
    return {};
}

} // namespace

PopupWindow::PopupWindow(db::Repository& repo, const Config& config, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_repo(repo)
    , m_model(new HistoryModel(this))
    , m_controller(new SearchController(repo, *m_model, this))
    , m_search(new SearchBar(this))
    , m_settingsButton(new QToolButton(this))
    , m_clearButton(new QToolButton(this))
    , m_dragHandle(new DragHandle(this))
    , m_view(new HistoryView(this))
    , m_footer(new QLabel(this))
    , m_undoButton(new QToolButton(this))
    , m_undoTimer(new QTimer(this))
    , m_groupPrompt(askGroup)
    , m_editPrompt(defaultEditPrompt())
    , m_textPrompt(defaultTextPrompt())
    , m_confirm(defaultConfirmPrompt())
    , m_deletePassword(defaultDeletePasswordPrompt())
    , m_qrView(defaultQrView())
    , m_notify([](QWidget* parent, const QString& message) {
        QMessageBox::warning(parent, QStringLiteral("ClipItTo"), message);
    })
    , m_viewText(defaultTextViewer)
    , m_passwordPrompt(defaultPasswordPrompt())
    , m_namePrompt(defaultNamePrompt())
{
    setWindowTitle(QStringLiteral("ClipItTo"));
    resize(560, 480);

    m_moveEnd = new QTimer(this);
    m_moveEnd->setSingleShot(true);
    connect(m_moveEnd, &QTimer::timeout, this, &PopupWindow::endSystemMove);

    m_view->setHistoryModel(m_model);
    m_view->setItemDelegate(new HistoryDelegate(m_view));

    const QIcon gear = QIcon::fromTheme(QStringLiteral("preferences-system"),
                                        QIcon::fromTheme(QStringLiteral("emblem-system")));
    if (gear.isNull()) {
        m_settingsButton->setText(QStringLiteral("⚙")); // ⚙
    } else {
        m_settingsButton->setIcon(gear);
    }
    m_settingsButton->setAutoRaise(true);
    m_settingsButton->setFocusPolicy(Qt::NoFocus); // keyboard: Ctrl+,
    m_settingsButton->setToolTip(platformKeys(tr("Settings (Ctrl+,)")));
    connect(m_settingsButton, &QToolButton::clicked, this, &PopupWindow::settingsRequested);
    auto* settingsShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma), this);
    connect(settingsShortcut, &QShortcut::activated, this, &PopupWindow::settingsRequested);
    // Ctrl+T: show the selected text translated, without pasting it.
    auto* translateShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this);
    connect(translateShortcut, &QShortcut::activated, this, &PopupWindow::previewTranslation);

    // Clear: the button empties Recent (copies in no group are deleted, filed ones only leave
    // Recent); its menu also offers deleting everything. Both ask first.
    const QIcon clearIcon = QIcon::fromTheme(QStringLiteral("edit-clear-history"),
                                             QIcon::fromTheme(QStringLiteral("edit-clear-all"),
                                                              QIcon::fromTheme(QStringLiteral("edit-clear"))));
    if (clearIcon.isNull()) {
        m_clearButton->setText(tr("Clear"));
    } else {
        m_clearButton->setIcon(clearIcon);
    }
    m_clearButton->setAutoRaise(true);
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(tr("Clear Recent (starred, pinned and grouped copies stay in their groups)"));
    m_clearButton->setPopupMode(QToolButton::MenuButtonPopup);
    auto* clearMenu = new QMenu(m_clearButton);
    clearMenu->addAction(tr("Clear Recent (copies in groups stay in them)"), this, [this] { clearHistory(false); });
    clearMenu->addAction(tr("Clear everything, also in groups…"), this, [this] { clearHistory(true); });
    connect(clearMenu, &QMenu::aboutToShow, this, [this] { ++m_modalDepth; });
    connect(clearMenu, &QMenu::aboutToHide, this, [this] { --m_modalDepth; });
    m_clearButton->setMenu(clearMenu);
    connect(m_clearButton, &QToolButton::clicked, this, [this] { clearHistory(false); });

    m_footerHint = platformKeys(tr("Enter copy · → open · Ctrl+S ★ · Ctrl+P pin · Ctrl+G group · Ctrl+E edit · "
                                   "Alt+↑↓ move · Del remove"));
    m_footer->setText(m_footerHint);
    m_footer->setToolTip(platformKeys(tr("Alt+↑/↓ moves a group, or a pinned copy, up or down. Esc closes.")));
    m_footer->setFont(scaledFont(m_footer->font(), 0.85));
    m_footer->setEnabled(false); // dim
    m_footer->setWordWrap(true); // a second line rather than a wider popup (fonts vary)
    // Paste once: after a copy was forgotten, Undo for kUndoMs (button, or Ctrl+Shift+Z; plain Ctrl+Z belongs to the
    // search box).
    m_undoButton->setObjectName(QStringLiteral("undoOnceButton"));
    m_undoButton->setText(tr("Undo"));
    m_undoButton->setAutoRaise(true);
    m_undoButton->setFocusPolicy(Qt::NoFocus);
    m_undoButton->setVisible(false);
    m_undoButton->setToolTip(platformKeys(tr("Put the copy back (Ctrl+Shift+Z)")));
    connect(m_undoButton, &QToolButton::clicked, this, [this] { undoPasteOnce(); });
    auto* undoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), this);
    connect(undoShortcut, &QShortcut::activated, this, [this] { undoPasteOnce(); });
    m_undoTimer->setSingleShot(true);
    m_undoTimer->setInterval(kUndoMs);
    connect(m_undoTimer, &QTimer::timeout, this, &PopupWindow::endUndo);

    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto* handle = static_cast<QLabel*>(m_dragHandle);
    const int iconSize = handle->fontMetrics().height();
    handle->setText(QStringLiteral("ClipItTo"));
    handle->setFont(scaledFont(handle->font(), 0.9));
    auto* icon = new DragHandle(this);
    icon->setPixmap(appIcon(currentThemeIsDark()).pixmap(iconSize, iconSize));
    onThemeApplied([icon = QPointer<QLabel>(icon), iconSize] {
        if (icon) {
            icon->setPixmap(appIcon(currentThemeIsDark()).pixmap(iconSize, iconSize));
        }
    });
    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    top->addWidget(icon);
    top->addWidget(m_dragHandle);
    top->addSpacing(4);
    top->addWidget(m_search, 1);
    top->addWidget(m_clearButton);
    top->addWidget(m_settingsButton);
    auto* inner = new QVBoxLayout(frame);
    inner->setContentsMargins(8, 8, 8, 6);
    inner->setSpacing(6);
    inner->addLayout(top);
    inner->addWidget(m_view, 1);
    auto* footerRow = new QHBoxLayout;
    footerRow->setContentsMargins(0, 0, 0, 0);
    footerRow->addWidget(m_footer, 1);
    footerRow->addWidget(m_undoButton);
    inner->addLayout(footerRow);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(frame);

    m_search->installEventFilter(this);
    m_view->installEventFilter(this);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!m_search->isPasswordMode()) { // a password search shows only "p[] ****" there
            m_controller->setQuery(text);
        }
    });
    connect(m_search, &SearchBar::passwordQueryChanged, m_controller, &SearchController::setPasswordQuery);
    m_revealTimer = new QTimer(this);
    m_revealTimer->setSingleShot(true);
    m_revealTimer->setInterval(kRevealMs);
    connect(m_revealTimer, &QTimer::timeout, m_model, &HistoryModel::clearRevealed);
    connect(m_controller, &SearchController::updated, this, &PopupWindow::onResultsUpdated);

    connect(m_search, &SearchBar::focusListRequested, this, [this] {
        m_view->setFocus();
        m_view->selectFirstRow(); // the row right under the search box (Groups, if any)
    });
    // Up/Down in the search box move the cursor from where it is: when the popup opens it is
    // on the latest copy, so Down goes to the next copy and Up to the Groups row above it.
    connect(m_search, &SearchBar::stepRequested, this, [this](int step) {
        const QModelIndex current = m_view->currentIndex();
        QModelIndex target = current.isValid() ? m_view->nextSelectable(current, step > 0)
                                               : (step > 0 ? m_view->firstSelectable() : QModelIndex());
        if (!target.isValid()) {
            if (step < 0 || !current.isValid()) {
                return; // nothing above: stay in the search box
            }
            target = current; // already the last row
        }
        m_view->setFocus();
        m_view->selectIndex(target);
    });
    connect(m_search, &SearchBar::openGroupsRequested, this, &PopupWindow::openGroups);
    connect(m_search, &SearchBar::deleteCurrentRequested, this, [this] {
        if (m_controller->isPending()) {
            // The list still shows the previous search: bring it up to date, and let the user
            // see what is selected before a Delete removes anything.
            m_controller->refresh();
            return;
        }
        if (entryFor(m_view->currentIndex())) {
            remove(m_view->currentIndex(), /*focusList=*/false); // typing stays in the search box
        }
    });
    connect(m_search, &SearchBar::renameRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        renameIndex(m_view->currentIndex());
    });
    connect(m_search, &SearchBar::editNoteRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        if (const Entry* e = entryFor(m_view->currentIndex())) {
            editNote(e->id);
        }
    });
    // Ctrl+G, Ctrl+S and Ctrl+P work from the search box as they do in the list.
    connect(m_search, &SearchBar::editGroupRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        const QModelIndex index = m_view->currentIndex();
        if (const Entry* e = entryFor(index)) {
            editGroup(e->id);
        } else if (m_model->kindAt(index) == Kind::Group) {
            renameGroup(m_model->pathAt(index));
        }
    });
    connect(m_search, &SearchBar::toggleFavoriteRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        if (const Entry* e = entryFor(m_view->currentIndex())) {
            toggleFavorite(e->id);
        }
    });
    connect(m_search, &SearchBar::togglePinRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        if (const Entry* e = entryFor(m_view->currentIndex())) {
            togglePin(e->id);
        }
    });
    connect(m_search, &SearchBar::activateRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh(); // Enter right after typing: use the up-to-date results
        }
        if (m_controller->passwordSearchNeedsUnlock() && !m_model->firstEntry().isValid()) {
            // A locked password search found nothing: unlock, and it searches inside them.
            withPasswordsUnlocked([this] { m_controller->refresh(); });
            return;
        }
        // From the search box, Enter always copies: the selected copy, else the first one.
        const QModelIndex current = m_view->currentIndex();
        activate(entryFor(current) ? current : m_model->firstEntry(m_startFocus));
    });
    connect(m_view, &HistoryView::focusSearchRequested, this, [this] {
        m_search->setFocus();
        m_search->end(false);
    });
    connect(m_view, &HistoryView::typedText, this, [this](QKeyEvent* event) {
        m_search->setFocus();
        QKeyEvent copy(event->type(), event->key(), event->modifiers(), event->text());
        QCoreApplication::sendEvent(m_search, &copy);
    });
    connect(m_view, &HistoryView::activateRequested, this, &PopupWindow::activate);
    connect(m_view, &HistoryView::pasteOnceRequested, this, &PopupWindow::activateOnce);
    connect(m_search, &SearchBar::pasteOnceRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        const QModelIndex current = m_view->currentIndex();
        activateOnce(entryFor(current) ? current : m_model->firstEntry(m_startFocus));
    });
    connect(m_view, &HistoryView::pasteAsRequested, this, &PopupWindow::pasteAs);
    connect(m_search, &SearchBar::pasteAsRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh();
        }
        const QModelIndex current = m_view->currentIndex();
        pasteAs(entryFor(current) ? current : m_model->firstEntry(m_startFocus));
    });
    connect(m_view, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (m_model->isExpandable(index)) {
            m_view->toggleExpanded(index);
        } else if (m_singleClick) {
            activate(index);
        }
    });
    connect(m_view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!m_singleClick && entryFor(index)) {
            activate(index);
        }
    });
    connect(m_view, &QTreeView::expanded, this,
            [this](const QModelIndex& index) { m_expanded.insert(expandKey(*m_model, index)); });
    connect(m_view, &QTreeView::collapsed, this,
            [this](const QModelIndex& index) { m_expanded.remove(expandKey(*m_model, index)); });
    connect(m_view, &HistoryView::toggleFavoriteRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            toggleFavorite(e->id);
        }
    });
    connect(m_view, &HistoryView::togglePinRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            togglePin(e->id);
        }
    });
    connect(m_view, &HistoryView::moveRequested, this, &PopupWindow::moveItem);
    connect(m_view, &HistoryView::editGroupRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            editGroup(e->id);
        } else if (m_model->kindAt(index) == Kind::Group) {
            renameGroup(m_model->pathAt(index));
        }
    });
    connect(m_view, &HistoryView::editNoteRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            editNote(e->id);
        }
    });
    connect(m_view, &HistoryView::renameRequested, this, &PopupWindow::renameIndex);
    connect(m_view, &HistoryView::deleteRequested, this, [this](const QModelIndex& index) {
        if (entryFor(index)) {
            remove(index);
        } else if (m_model->kindAt(index) == Kind::Group) {
            deleteGroup(m_model->pathAt(index));
        }
    });
    connect(m_view, &HistoryView::contextMenuRequested, this, &PopupWindow::showContextMenu);

    applyConfig(config);
}

PopupWindow::~PopupWindow() = default;

void PopupWindow::applyConfig(const Config& config)
{
    m_keyboardLayouts = texttransform::enabledLayouts(config.keyboardLayouts, QLocale::system().bcp47Name());
    m_pasteAsText = config.pasteAsText;
    m_pasteAsPicture = config.pasteAsPicture;
    m_pasteAsOthers = config.pasteAsOthers;
    m_controller->applyConfig(config);
    m_hideOnFocusLoss = config.hideOnFocusLoss;
    m_singleClick = config.clickToCopy == ClickMode::Single;
    m_position = config.popupPosition;
    m_startFocus = config.startFocus;
    m_search->setPasswordKeyword(config.passwordSearchKeyword);
    if (isVisible()) {
        refresh(); // preview lines or counts may have changed
    }
}

void PopupWindow::showPopup(const QString& activationToken)
{
    if (!isVisible()) {
        emit aboutToShow();
        m_restore.reset();
        m_expanded.clear(); // the group tree starts collapsed
        if (isBusy()) {
            // keep the busy message; no queries
        } else if (m_search->text().isEmpty()) {
            m_controller->refresh();
        } else {
            m_search->clear(); // refreshes through textChanged
        }
        placeOnScreen();
        showUndoNotice(); // a Paste once removal within the last 10 seconds can still be undone
    }
    if (!activationToken.isEmpty() && QGuiApplication::platformName() == QLatin1String("wayland")) {
        // Lets Qt's Wayland plugin activate us with the compositor-issued xdg-activation token.
        qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
    }
    show();
    // The view had no size while hidden, so scrolling then did not stick; once laid out it
    // had scrolled to the selected Recent copy and hid the Groups row above it.
    m_view->keepTopVisible();
    QTimer::singleShot(0, m_view, [view = m_view] { view->keepTopVisible(); });
    raise();
    activateWindow();
    bringAppToFront(); // macOS: a menu-bar app must activate itself to take keyboard focus
    m_search->setFocus();
}

void PopupWindow::hidePopup()
{
    hide();
}

void PopupWindow::togglePopup(const QString& activationToken)
{
    if (isVisible()) {
        hidePopup();
    } else {
        showPopup(activationToken);
    }
}

void PopupWindow::setBusy(const QString& message)
{
    m_busyMessage = message;
    m_controller->setSuspended(isBusy()); // also drops a search still waiting for its delay
    m_search->setEnabled(!isBusy());
    m_view->setEnabled(!isBusy());
    if (isBusy()) {
        m_model->setContent({});
        m_view->setEmptyText(message);
    } else {
        m_controller->refresh();
        if (isVisible()) {
            m_search->setFocus();
        }
    }
}

void PopupWindow::showMessage(const QString& message)
{
    ModalGuard guard(m_modalDepth);
    m_notify(this, message);
}

void PopupWindow::historyChanged()
{
    if (isBusy()) {
        return;
    }
    if (isVisible() && m_search->text().trimmed().isEmpty()) {
        refresh();
    }
}

void PopupWindow::refresh()
{
    if (isBusy()) {
        return;
    }
    refreshSelecting(selectionOf(m_view->currentIndex()), /*focusList=*/false);
}

int PopupWindow::execDialog(QDialog& dialog)
{
    int result = 0;
    {
        ModalGuard guard(m_modalDepth);
        result = dialog.exec();
    }
    activateWindow();
    m_search->setFocus();
    return result;
}

void PopupWindow::placeOnScreen()
{
    // Place it at its final size: the layout may still enlarge it to its minimum.
    if (layout()) {
        layout()->activate();
    }
    // Wayland clients cannot position top-level windows; the compositor places us.
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
        return;
    }
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }
    // Center and the corners use the screen the pointer is on. A corner keeps 5% of the
    // screen's width (and height) away from the sides it is near, not glued to the edge.
    const QRect area = screen->availableGeometry();
    const int marginX = area.width() * 5 / 100;
    const int marginY = area.height() * 5 / 100;
    switch (m_position) {
    case PopupPosition::Last:
        if (m_lastPosition) {
            // As long as that place is still on a screen; else near the pointer.
            const QRect wanted(*m_lastPosition, size());
            if (QScreen* there = QGuiApplication::screenAt(wanted.center())) {
                move(keepOnScreen(wanted.topLeft(), size(), there->availableGeometry()));
                return;
            }
        }
        break;
    case PopupPosition::Center:
        // Centered horizontally, in the upper third: where the eye looks first.
        move(keepOnScreen(QPoint(area.left() + (area.width() - width()) / 2, area.top() + (area.height() - height()) / 3),
                          size(), area));
        return;
    case PopupPosition::TopLeft:
        move(keepOnScreen(area.topLeft() + QPoint(marginX, marginY), size(), area));
        return;
    case PopupPosition::TopRight:
        move(keepOnScreen(QPoint(area.right() + 1 - marginX - width(), area.top() + marginY), size(), area));
        return;
    case PopupPosition::BottomLeft:
        move(keepOnScreen(QPoint(area.left() + marginX, area.bottom() + 1 - marginY - height()), size(), area));
        return;
    case PopupPosition::BottomRight:
        move(keepOnScreen(QPoint(area.right() + 1 - marginX - width(), area.bottom() + 1 - marginY - height()),
                          size(), area));
        return;
    case PopupPosition::Cursor:
        break;
    }
    move(keepOnScreen(cursor - QPoint(width() / 2, 24), size(), area));
}

const Entry* PopupWindow::entryFor(const QModelIndex& index) const
{
    return m_model->entryAt(index);
}

PopupWindow::Selection PopupWindow::selectionOf(const QModelIndex& index) const
{
    Selection s;
    s.kind = m_model->kindAt(index);
    s.path = m_model->pathAt(index);
    if (const Entry* e = entryFor(index)) {
        s.id = e->id;
    }
    return s;
}

void PopupWindow::activate(const QModelIndex& index)
{
    activateWith(index, false);
}

void PopupWindow::activateOnce(const QModelIndex& index)
{
    activateWith(index, true);
}

// `once`: Ctrl+Shift+Enter, which pastes and forgets a copy whether it is marked or not. (Enter on a marked copy
// is the plain signal: the owner sees the mark on the entry.)
void PopupWindow::activateWith(const QModelIndex& index, bool once)
{
    const QModelIndex target = index.isValid() ? index : m_model->firstEntry();
    if (m_model->isExpandable(target)) {
        if (!once) {
            m_view->toggleExpanded(target);
        }
        return;
    }
    const Entry* e = entryFor(target);
    if (!e) {
        return; // a header or an empty group
    }
    const Entry chosen = *e; // receivers may refresh the model
    const auto paste = [this, chosen, once] {
        if (once) {
            emit onceActivated(chosen);
        } else {
            emit entryActivated(chosen);
        }
        hidePopup();
    };
    if (chosen.secret) {
        withPasswordsUnlocked(paste);
        return;
    }
    paste();
}

void PopupWindow::togglePasteOnce(qint64 id)
{
    try {
        if (const auto e = m_repo.get(id)) {
            m_repo.setPasteOnce(id, !e->pasteOnce);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "paste once mark failed:" << err.what();
    }
    refreshSelecting(Selection{Kind::Entry, id, {}});
}

void PopupWindow::consumePasteOnce(qint64 id)
{
    std::optional<Entry> e;
    try {
        e = m_repo.get(id);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "paste once: reading the copy failed:" << err.what();
        return;
    }
    if (!e) {
        return;
    }
    if (e->pinned() || e->favorite) {
        // Never deleted: the mark is only taken off, and the popup says so quietly.
        try {
            m_repo.setPasteOnce(id, false);
        } catch (const db::DbError& err) {
            qCWarning(lcPopup) << "paste once: removing the mark failed:" << err.what();
        }
        flash(e->pinned() ? tr("Pasted. Pinned copies are not deleted: the Paste once mark was removed.")
                          : tr("Pasted. Starred copies are not deleted: the Paste once mark was removed."));
        refreshSelecting(std::nullopt, /*focusList=*/false);
        return;
    }
    if (e->secret) {
        // Pasted first; then the question, every time. The window that got the paste keeps the keyboard a moment.
        QTimer::singleShot(m_onceAskDelayMs, this, [this, id, self = QPointer<PopupWindow>(this)] {
            if (!self) {
                return;
            }
            std::optional<Entry> now;
            try {
                now = m_repo.get(id);
            } catch (const db::DbError&) {
                return;
            }
            if (!now || !now->secret) {
                return;
            }
            bool yes = false;
            {
                ModalGuard guard(m_modalDepth);
                yes = m_deletePassword(this, now->note.isEmpty()
                                                 ? tr("Delete this password copy?")
                                                 : tr("Delete this password copy?\n%1").arg(now->note));
            }
            if (yes) {
                forgetPasteOnce(id);
            } else {
                try {
                    m_repo.setPasteOnce(id, false); // kept: it is not one-time any more
                } catch (const db::DbError&) {
                }
                refreshSelecting(std::nullopt, /*focusList=*/false);
            }
        });
        return;
    }
    forgetPasteOnce(id);
}

void PopupWindow::forgetPasteOnce(qint64 id)
{
    try {
        std::optional<db::Repository::Removed> removed = m_repo.snapshot(id);
        if (!removed) {
            return;
        }
        if (removed->entry.image) {
            QPixmapCache::remove(QStringLiteral("clipitto-thumb-") + removed->entry.contentHash); // see HistoryModel
        }
        if (!m_repo.forgetNow(id)) {
            return;
        }
        m_undo = std::move(removed);
        m_undoTimer->start();
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "paste once: forgetting the copy failed:" << err.what();
        return;
    }
    qCDebug(lcPopup) << "paste once: a copy was forgotten";
    showUndoNotice();
    refreshSelecting(std::nullopt, /*focusList=*/false);
}

void PopupWindow::showUndoNotice()
{
    m_undoButton->setVisible(m_undo.has_value());
    if (m_undo) {
        m_footer->setText(platformKeys(tr("Pasted and forgotten. Undo: Ctrl+Shift+Z")));
    }
}

void PopupWindow::endUndo()
{
    m_undoTimer->stop();
    m_undo.reset(); // the copy is gone for good, and not kept in memory either
    m_undoButton->setVisible(false);
    m_footer->setText(m_footerHint);
}

bool PopupWindow::undoPasteOnce()
{
    if (!m_undo) {
        return false;
    }
    const db::Repository::Removed removed = std::move(*m_undo);
    endUndo();
    qint64 id = 0;
    try {
        id = m_repo.restore(removed); // without the once mark
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "paste once: undo failed:" << err.what();
        return false;
    }
    flash(tr("Put back."));
    refreshSelecting(Selection{Kind::Entry, id, {}}, /*focusList=*/false);
    return true;
}

void PopupWindow::setPasswords(PasswordKeeper* keeper)
{
    m_passwords = keeper;
    m_controller->setPasswords(keeper);
    if (keeper) {
        connect(keeper, &PasswordKeeper::lockChanged, this, [this](bool unlocked) {
            if (!unlocked) {
                m_model->clearRevealed();
            }
            if (m_controller->isPasswordSearch()) {
                m_controller->refresh(); // unlocked: search inside them now
            }
        });
    }
}

void PopupWindow::withPasswordsUnlocked(std::function<void()> then)
{
    if (!m_passwords || !m_passwords->isSetUp()) {
        showMessage(tr("Set a master password in Settings › Passwords first."));
        return;
    }
    if (m_passwords->isUnlocked()) {
        then();
        return;
    }
    if (m_unlocking) {
        return;
    }
    auto askPassword = [this, then] {
        std::optional<QString> password;
        {
            ModalGuard guard(m_modalDepth);
            password = m_passwordPrompt(this, tr("Master password:"));
        }
        if (!password) {
            return;
        }
        QByteArray bytes = PasswordVault::passwordBytes(*password);
        m_unlocking = true;
        ++m_modalDepth; // the popup stays while the key is derived
        m_passwords->unlock(std::move(bytes), [this, self = QPointer<PopupWindow>(this), then](bool ok) {
            if (!self) {
                return; // closed down meanwhile
            }
            m_unlocking = false;
            --m_modalDepth;
            if (ok) {
                then();
            } else {
                showMessage(tr("Wrong master password."));
            }
        });
    };
    if (m_helloUnlock && m_passwords->helloEnabled()) {
        m_unlocking = true;
        ++m_modalDepth; // the Windows Hello prompt takes the focus
        m_helloUnlock([this, self = QPointer<PopupWindow>(this), then, askPassword](bool unlocked) {
            if (!self) {
                return;
            }
            m_unlocking = false;
            --m_modalDepth;
            if (unlocked) {
                then();
            } else {
                askPassword(); // cancelled or unavailable: the master password still works
            }
        });
        return;
    }
    askPassword();
}

void PopupWindow::revealPassword(qint64 id)
{
    withPasswordsUnlocked([this, id] {
        std::optional<QByteArray> plain = m_passwords ? m_passwords->reveal(id) : std::nullopt;
        if (!plain) {
            return;
        }
        QString text = QString::fromUtf8(*plain);
        PasswordVault::wipe(*plain);
        m_model->setRevealed(id, text);
        PasswordVault::wipe(text);
        m_revealTimer->start();
    });
}

void PopupWindow::askPasswordNote(qint64 id, const QString& suggestion)
{
    if (m_namingPassword || isBusy()) {
        // another box is open (the copy keeps its default note), or the history is busy
        qCInfo(lcPopup) << "name box skipped:" << (m_namingPassword ? "another name box is open" : "history is busy");
        return;
    }
    m_namingPassword = true;
    // From the event loop, not from inside the capture signal that announced the copy.
    QTimer::singleShot(0, this, [this, id, suggestion] {
        const bool wasVisible = isVisible();
        showPopup();
        refreshSelecting(Selection{Kind::Entry, id, {}});
        qCInfo(lcPopup) << "name box shown, popup was" << (wasVisible ? "visible" : "hidden") << "active:" << isActiveWindow();
        std::optional<QString> name;
        {
            ModalGuard guard(m_modalDepth);
            name = m_namePrompt(this, suggestion);
        }
        m_namingPassword = false;
        const char* outcome = name && !name->trimmed().isEmpty() ? "named"
            : lastNamePromptTimedOut()                          ? "timed out"
            : name                                              ? "left empty"
                                                                : "cancelled";
        qCInfo(lcPopup) << "name box ended:" << outcome;
        emit passwordNameEnded(QString::fromLatin1(outcome));
        // OK, Enter or Cancel with nothing typed: the suggestion (the app it came from) is the
        // note. A typed name replaces it.
        const QString chosen = name && !name->trimmed().isEmpty() ? *name : suggestion;
        if (!chosen.trimmed().isEmpty()) {
            try {
                m_repo.setNote(id, chosen);
            } catch (const db::DbError& err) {
                qCWarning(lcPopup) << "naming a password copy failed:" << err.what();
            }
        }
        refreshSelecting(Selection{Kind::Entry, id, {}});
        if (!wasVisible) {
            hidePopup();
        }
    });
}

void PopupWindow::markAsPassword(qint64 id)
{
    if (!m_passwords || !m_passwords->isSetUp()) {
        showMessage(tr("Set a master password in Settings › Passwords first."));
        return;
    }
    const std::optional<qint64> holder = m_passwords->markAsPassword(id);
    refreshSelecting(Selection{Kind::Entry, holder.value_or(id), {}});
    if (holder && *holder == id) { // not when the same password was already kept
        emit passwordMarked(id);
    } else {
        qCInfo(lcPopup) << "name box not asked:"
                        << (holder ? "the same password was already kept" : "marking as password returned nothing");
    }
}

void PopupWindow::unmarkPassword(qint64 id)
{
    withPasswordsUnlocked([this, id] {
        const std::optional<qint64> holder = m_passwords->unmark(id);
        refreshSelecting(Selection{Kind::Entry, holder.value_or(id), {}});
    });
}

void PopupWindow::openGroups()
{
    const QModelIndex root = m_model->groupsRoot();
    if (!root.isValid()) {
        return;
    }
    m_view->setFocus();
    m_view->selectIndex(root);
    m_view->expand(root);
}

void PopupWindow::toggleFavorite(qint64 id)
{
    Selection follow = selectionOf(m_view->currentIndex());
    QString problem;
    try {
        if (const auto e = m_repo.get(id)) {
            problem = refusal(m_repo.setFavorite(id, !e->favorite), m_repo.limits(), e->group);
            if (problem.isEmpty() && e->group.isEmpty() && follow.path == HistoryModel::favoritesPath()) {
                follow.path.clear(); // unstarred: it left Favorites, follow it to its list row
            }
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "toggle favorite failed:" << err.what();
    }
    if (!problem.isEmpty()) {
        showMessage(problem);
    }
    refreshSelecting(follow);
}

void PopupWindow::togglePin(qint64 id)
{
    QString problem;
    try {
        if (const auto e = m_repo.get(id)) {
            problem = refusal(m_repo.setPinned(id, !e->pinned()), m_repo.limits(), QString());
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "toggle pin failed:" << err.what();
    }
    if (!problem.isEmpty()) {
        showMessage(problem);
    }
    refreshSelecting(selectionOf(m_view->currentIndex()));
}

// Is `path` the Passwords group, or one of its sub-groups?
static bool isPasswordsGroup(const QString& path)
{
    const QString normal = groups::normalize(path);
    const QString passwords = PasswordKeeper::passwordsGroup();
    return normal.compare(passwords, Qt::CaseInsensitive) == 0
        || normal.startsWith(passwords + QLatin1Char('/'), Qt::CaseInsensitive);
}

void PopupWindow::assignGroup(qint64 id, const QString& group, Selection follow)
{
    // A text copy put in the Passwords group becomes a password copy at the same time (what "Mark as password" does):
    // no need to do both. "Mark as password" stays for copies that should not move.
    bool alsoMark = false;
    qint64 markedNow = 0; // became a new password copy in this step
    if (isPasswordsGroup(group)) {
        try {
            const auto entry = m_repo.get(id);
            alsoMark = entry && !entry->secret && !entry->image;
            if (!alsoMark) {
                qCInfo(lcPopup) << "name box not asked: the copy is already a password copy, a picture or gone";
            }
        } catch (const db::DbError& err) {
            qCWarning(lcPopup) << "reading the copy failed:" << err.what();
        }
        if (alsoMark && (!m_passwords || !m_passwords->isSetUp())) {
            // A plain text in a group called Passwords would look protected and not be: don't move it.
            showMessage(tr("The Passwords group holds password copies, kept encrypted: set a master password in "
                           "Settings › Passwords first."));
            refreshSelecting(follow);
            return;
        }
    }
    QString problem;
    try {
        const auto change = m_repo.setGroup(id, group);
        problem = refusal(change, m_repo.limits(), groups::normalize(group));
        if (problem.isEmpty() && alsoMark) {
            // The same password may already be kept: that copy stays, and it goes to the group too.
            const qint64 holder = m_passwords->markAsPassword(id).value_or(id);
            if (holder != id) {
                m_repo.setGroup(holder, group);
                qCInfo(lcPopup) << "name box not asked: the same password was already kept";
            } else if (m_repo.get(id) && m_repo.get(id)->secret) {
                markedNow = id;
            } else {
                qCInfo(lcPopup) << "name box not asked: marking as password returned nothing (vault or database)";
            }
            if (follow.kind == Kind::Entry && follow.id == id) {
                follow.id = holder;
            }
        }
        if (problem.isEmpty() && follow.kind == Kind::Entry && (follow.id == id || alsoMark)) {
            // The copy left its place in the group tree: find it again wherever it is now.
            follow.path.clear();
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "assign group failed:" << err.what();
    }
    if (!problem.isEmpty()) {
        showMessage(problem);
    }
    refreshSelecting(follow);
    if (markedNow != 0) {
        emit passwordMarked(markedNow);
    }
}

void PopupWindow::moveGroup(const QString& path, int delta)
{
    try {
        m_repo.moveGroup(path, delta);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "move group failed:" << err.what();
    }
    refreshSelecting(Selection{Kind::Group, 0, path});
}

void PopupWindow::moveItem(const QModelIndex& index, int delta)
{
    const Selection selection = selectionOf(index);
    if (m_model->kindAt(index) == Kind::Group) {
        moveGroup(selection.path, delta);
        return;
    }
    try {
        const Entry* e = entryFor(index);
        if (e && e->pinned() && !index.parent().isValid() && m_controller->query().trimmed().isEmpty()) {
            m_repo.movePinned(e->id, delta); // a row of the Pinned list
        } else {
            return; // Favorites stays first; other copies are ordered by time
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "move failed:" << err.what();
    }
    refreshSelecting(selection);
}

void PopupWindow::editGroup(qint64 id)
{
    Selection follow = selectionOf(m_view->currentIndex());
    try {
        const auto e = m_repo.get(id);
        if (!e) {
            return;
        }
        std::optional<QString> group;
        {
            ModalGuard guard(m_modalDepth);
            group = m_groupPrompt(this, e->group, m_repo.groups());
        }
        if (group) {
            assignGroup(id, *group, follow);
            return;
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "assign group failed:" << err.what();
    }
    refreshSelecting(follow);
}

void PopupWindow::editNote(qint64 id)
{
    try {
        const auto e = m_repo.get(id);
        if (!e) {
            return;
        }
        std::optional<QString> text;
        if (!e->image && !e->secret) {
            text = e->content;
        }
        std::optional<QString> newText;
        const EditApply apply = [&](const CopyEdit& edit) -> QString {
            try {
                if (edit.text && *edit.text != e->content) {
                    switch (m_repo.setContent(id, *edit.text, m_maxItemBytes)) {
                    case db::Repository::ContentChange::Done:
                        newText = *edit.text;
                        break;
                    case db::Repository::ContentChange::Empty:
                        return tr("The text can't be empty.");
                    case db::Repository::ContentChange::TooLarge:
                        return tr("The text is larger than the limit of %1 bytes.").arg(m_maxItemBytes);
                    case db::Repository::ContentChange::Duplicate:
                        return tr("Another copy already has this text.");
                    case db::Repository::ContentChange::NotText:
                    case db::Repository::ContentChange::Missing:
                        return tr("This copy's text can't be edited.");
                    }
                }
                m_repo.setNote(id, edit.note);
            } catch (const db::DbError& err) {
                qCWarning(lcPopup) << "edit copy failed:" << err.what();
                return tr("Could not save: the history is unavailable.");
            }
            return {};
        };
        {
            ModalGuard guard(m_modalDepth);
            m_editPrompt(this, e->note, text, apply);
        }
        if (newText) {
            emit textEdited(id, *newText);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "edit note failed:" << err.what();
    }
    refreshSelecting(selectionOf(m_view->currentIndex()));
}

void PopupWindow::flash(const QString& message)
{
    m_footer->setText(message);
    QTimer::singleShot(5000, this, [this, message] {
        if (m_footer->text() == message) {
            m_footer->setText(m_footerHint);
        }
    });
}

void PopupWindow::clearHistory(bool everything)
{
    if (isBusy()) {
        return;
    }
    qint64 count = 0;
    try {
        count = everything ? m_repo.count() : m_repo.countRecent();
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "counting copies failed:" << err.what();
        return;
    }
    if (count == 0) {
        showMessage(everything ? tr("The history is already empty.") : tr("Recent is already empty."));
        return;
    }
    bool confirmed = false;
    {
        ModalGuard guard(m_modalDepth);
        const QString copies = count == 1 ? tr("1 copy") : tr("%1 copies").arg(count);
        confirmed = m_confirm(this, everything
                                        ? tr("Delete all %1, including starred, pinned and grouped ones?\n"
                                             "Your groups stay, empty. This can't be undone.")
                                              .arg(copies)
                                        : tr("Clear Recent (%1)?\n"
                                             "Starred, pinned and grouped copies stay in their groups; "
                                             "the others are deleted.")
                                              .arg(copies));
    }
    if (!confirmed) {
        refreshSelecting(selectionOf(m_view->currentIndex()), /*focusList=*/false);
        return;
    }
    try {
        if (everything) {
            m_repo.clearHistory(true);
        } else {
            m_repo.clearRecent();
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "clearing the history failed:" << err.what();
    }
    QPixmapCache::clear(); // no thumbnails of cleared pictures stay in memory
    refreshSelecting(std::nullopt, /*focusList=*/false);
    m_search->setFocus();
}

void PopupWindow::remove(const QModelIndex& index, bool focusList)
{
    const Entry* e = entryFor(index);
    if (!e) {
        return;
    }
    const qint64 id = e->id;
    if (e->image) {
        QPixmapCache::remove(QStringLiteral("clipitto-thumb-") + e->contentHash); // see HistoryModel
    }
    // Keep the cursor in place: select the copy that slides into this row.
    std::optional<Selection> next;
    for (QModelIndex i = m_view->indexBelow(index); i.isValid(); i = m_view->indexBelow(i)) {
        if (entryFor(i) && entryFor(i)->id != id) {
            next = selectionOf(i);
            break;
        }
    }
    if (!next) {
        for (QModelIndex i = m_view->indexAbove(index); i.isValid(); i = m_view->indexAbove(i)) {
            if (i.flags() & Qt::ItemIsSelectable) {
                next = selectionOf(i);
                break;
            }
        }
    }
    try {
        // Delete on a starred or grouped copy in the Recent list only takes it off Recent: it
        // stays in its group. Anywhere else (a group, Pinned, or an unfiled copy) it deletes.
        const bool inRecentList = !index.parent().isValid() && !e->pinned() && (e->favorite || !e->group.isEmpty());
        if (inRecentList) {
            m_repo.takeOffRecent(id);
        } else {
            m_repo.remove(id);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "delete failed:" << err.what();
    }
    refreshSelecting(next, focusList);
}

void PopupWindow::newGroup(const QString& parentPath)
{
    std::optional<QString> name;
    {
        ModalGuard guard(m_modalDepth);
        name = m_textPrompt(this, parentPath.isEmpty() ? tr("New group") : tr("New sub-group"),
                            parentPath.isEmpty()
                                ? tr("Name:")
                                : tr("Name of the new group inside %1:").arg(groups::display(parentPath)),
                            QString());
    }
    if (!name || groups::normalize(*name).isEmpty()) {
        refreshSelecting(selectionOf(m_view->currentIndex()));
        return;
    }
    QString created;
    try {
        created = m_repo.createGroup(parentPath.isEmpty() ? *name : parentPath + groups::kSeparator + *name);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "create group failed:" << err.what();
    }
    // Show where it landed.
    m_expanded.insert(QString());
    for (QString p = groups::parent(created); !p.isEmpty(); p = groups::parent(p)) {
        m_expanded.insert(p);
    }
    refreshSelecting(Selection{Kind::Group, 0, created});
}

void PopupWindow::renameIndex(const QModelIndex& index)
{
    if (const Entry* e = entryFor(index)) {
        editNote(e->id);
    } else if (m_model->kindAt(index) == Kind::Group) {
        renameGroup(m_model->pathAt(index)); // not Favorites or the Groups header: other kinds
    }
}

void PopupWindow::renameGroup(const QString& path)
{
    std::optional<QString> target;
    {
        ModalGuard guard(m_modalDepth);
        target = m_textPrompt(this, tr("Rename group"),
                              tr("New name, or a full path to move it (e.g. Work/Servers):"), path);
    }
    if (!target) {
        refreshSelecting(selectionOf(m_view->currentIndex()));
        return;
    }
    QString error;
    bool ok = false;
    try {
        ok = m_repo.renameGroup(path, *target, &error);
    } catch (const db::DbError& err) {
        error = QString::fromUtf8(err.what());
    }
    if (!ok) {
        ModalGuard guard(m_modalDepth);
        m_notify(this, tr("Could not rename the group: %1").arg(error));
        refreshSelecting(selectionOf(m_view->currentIndex()));
        return;
    }
    const QString renamed = groups::normalize(*target);
    // Keep the moved subtree's expansion, under its new name.
    QSet<QString> expanded;
    for (const QString& key : std::as_const(m_expanded)) {
        expanded.insert(!key.isEmpty() && groups::isWithin(key, path) ? renamed + key.mid(path.size()) : key);
    }
    for (QString p = groups::parent(renamed); !p.isEmpty(); p = groups::parent(p)) {
        expanded.insert(p);
    }
    m_expanded = expanded;
    refreshSelecting(Selection{Kind::Group, 0, renamed});
}

void PopupWindow::deleteGroup(const QString& path)
{
    bool confirmed = false;
    {
        ModalGuard guard(m_modalDepth);
        confirmed = m_confirm(this, tr("Delete the group %1 and its sub-groups?\nTheir copies stay in the history.")
                                        .arg(groups::display(path)));
    }
    if (confirmed) {
        try {
            m_repo.deleteGroup(path);
        } catch (const db::DbError& err) {
            qCWarning(lcPopup) << "delete group failed:" << err.what();
        }
    }
    const QString parent = groups::parent(path);
    refreshSelecting(confirmed ? Selection{parent.isEmpty() ? Kind::GroupsRoot : Kind::Group, 0, parent}
                               : selectionOf(m_view->currentIndex()));
}

void PopupWindow::refreshSelecting(std::optional<Selection> selection, bool focusList)
{
    if (isBusy()) {
        return;
    }
    m_restore = std::move(selection);
    m_controller->refresh();
    if (focusList) {
        activateWindow(); // dialogs may have taken focus
        m_view->setFocus();
    }
}

void PopupWindow::onResultsUpdated()
{
    if (m_controller->isPasswordSearch()) {
        const bool canMatch = m_passwords && m_passwords->canMatch();
        m_view->setEmptyText(!m_controller->passwordSearchNeedsUnlock() ? tr("No password copies match.")
                                 : canMatch ? tr("Locked: only a whole password matches. Press Enter to unlock "
                                                 "and search inside your passwords.")
                                            : tr("Locked. Press Enter to unlock and search your passwords."));
    } else {
        m_view->setEmptyText(m_controller->query().trimmed().isEmpty()
                                 ? tr("Nothing copied yet. Copied text will show up here.")
                                 : tr("No matches"));
    }

    // Re-open what was open (the model rebuild collapsed everything).
    const QSet<QString> expanded = m_expanded;
    for (const QString& key : expanded) {
        const QModelIndex group = m_model->findGroup(key);
        if (group.isValid()) {
            m_view->expand(group);
        }
    }
    m_expanded = expanded; // drop nothing that is merely missing right now

    QModelIndex target;
    if (m_restore) {
        switch (m_restore->kind) {
        case Kind::Entry:
            target = m_model->findEntry(m_restore->id, m_restore->path);
            if (!target.isValid()) {
                target = m_model->findEntry(m_restore->id);
            }
            break;
        case Kind::Group:
        case Kind::GroupsRoot:
        case Kind::Favorites:
            target = m_model->findGroup(m_restore->path);
            break;
        case Kind::Header:
            break;
        }
    }
    m_restore.reset();
    if (target.isValid()) {
        m_view->selectIndex(target);
    } else {
        m_view->selectFirstEntry(m_startFocus); // on opening and after each search
    }
}

// Read again when chosen, not when the menu was built: the copy may have become a password meanwhile, and a
// password's text must never reach the screen.
void PopupWindow::showQrCode(qint64 id)
{
    std::optional<Entry> entry;
    try {
        entry = m_repo.get(id);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "reading a copy for its QR code failed:" << err.what();
        return;
    }
    if (!entry || entry->secret || entry->image || entry->content.isEmpty()) {
        return;
    }
    m_qrView(this, entry->content);
}

QMenu* PopupWindow::createContextMenu(const Entry& entry, QWidget* parent)
{
    const qint64 id = entry.id;
    auto* menu = new QMenu(parent);
    menu->addAction(entry.favorite ? tr("Remove star") : tr("★ Star"), this, [this, id] { toggleFavorite(id); });
    menu->addAction(entry.pinned() ? tr("Unpin") : tr("Pin to the top"), this, [this, id] { togglePin(id); });

    QMenu* groupMenu = menu->addMenu(tr("Group"));
    QStringList existing;
    try {
        existing = m_repo.groups();
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "listing groups failed:" << err.what();
    }
    for (const QString& group : existing) {
        QAction* action = groupMenu->addAction(groups::display(group), this, [this, id, group] {
            assignGroup(id, group, Selection{Kind::Entry, id, {}});
        });
        action->setCheckable(true);
        action->setChecked(group == entry.group);
    }
    if (!existing.isEmpty()) {
        groupMenu->addSeparator();
    }
    groupMenu->addAction(tr("New group…"), this, [this, id] { editGroup(id); });
    if (!entry.group.isEmpty()) {
        groupMenu->addAction(tr("Remove from group"), this, [this, id] {
            assignGroup(id, QString(), Selection{Kind::Entry, id, {}});
        });
    }

    menu->addAction(entry.note.isEmpty() ? tr("Add note…\tF2") : tr("Edit note…\tF2"), this, [this, id] { editNote(id); });
    {
        QAction* once = menu->addAction(platformKeys(tr("Paste once\tCtrl+Shift+Enter")), this,
                                        [this, id] { togglePasteOnce(id); });
        once->setCheckable(true);
        once->setChecked(entry.pasteOnce);
        once->setToolTip(tr("Forgotten from the history right after it is pasted from ClipItTo. "
                            "Pinned and starred copies are kept; their mark is removed."));
    }
    if (entry.secret) {
        menu->addSeparator();
        menu->addAction(tr("Show password"), this, [this, id] { revealPassword(id); });
        menu->addAction(tr("Not a password (keep as text)"), this, [this, id] { unmarkPassword(id); });
        if (m_passwords && m_passwords->isUnlocked()) {
            menu->addAction(tr("Lock passwords"), m_passwords, &PasswordKeeper::lock);
        }
    } else if (entry.image) {
        menu->addMenu(createPasteAsMenu(entry, menu)); // Paste text from picture comes first in it
    } else {
        menu->addMenu(createPasteAsMenu(entry, menu));
        menu->addAction(tr("Show as QR code…"), this, [this, id] { showQrCode(id); });
        QAction* mark = menu->addAction(tr("Mark as password"), this, [this, id] { markAsPassword(id); });
        if (!m_passwords || !m_passwords->isSetUp()) {
            mark->setEnabled(false);
            mark->setToolTip(tr("Set a master password in Settings › Passwords first."));
            menu->setToolTipsVisible(true);
        }
    }
    menu->addSeparator();
    menu->addAction(tr("Delete"), this, [this, id] {
        const QModelIndex index = m_model->findEntry(id);
        remove(index.isValid() ? index : m_view->currentIndex());
    });
    return menu;
}

// "Others": a submenu with the items the Paste as setting leaves unticked, unless paste_as_others is off.
void PopupWindow::addOthersMenu(QMenu* menu, const QList<pasteas::Item>& items, const QStringList& shown,
                                const std::function<void(QMenu*, const QString&)>& addItem)
{
    if (!m_pasteAsOthers) {
        return;
    }
    auto* others = new QMenu(tr("Ot&hers"), menu);
    for (const pasteas::Item& item : items) {
        if (!shown.contains(item.id)) {
            addItem(others, item.id);
        }
    }
    if (others->isEmpty()) {
        delete others;
        return;
    }
    fixMnemonics(others);
    if (menu->actions().size() > 0) {
        menu->addSeparator();
    }
    menu->addMenu(others);
}

// The choices for a picture (Shift+Enter, and the right-click menu): the items of the paste_as_picture setting,
// by default its text first, then that text changed, then the picture itself.
QMenu* PopupWindow::createPictureMenu(const Entry& picture, QWidget* parent)
{
    auto* menu = new QMenu(tr("Paste &as"), parent);
    const qint64 id = picture.id;
    const bool canRead = m_textFromPicture && m_textFromPictureAvailable;
    const QString reason = m_textFromPictureReason.isEmpty() ? tr("Text recognition is not available here.")
                                                             : m_textFromPictureReason;
    const QList<pasteas::Item> items = pasteas::pictureItems();
    QHash<QString, QString> labels;
    for (const pasteas::Item& item : items) {
        labels.insert(item.id, item.label);
    }
    const auto addItem = [&](QMenu* target, const QString& itemId) {
        const QString label = labels.value(itemId);
        QAction* action = nullptr;
        if (itemId == QLatin1String(pasteas::kPictureText)) {
            action = target->addAction(label, this, [this, id] { pasteTextFromPicture(id); });
        } else if (itemId == QLatin1String(pasteas::kPictureChanged)) {
            QMenu* changed = target->addMenu(label);
            action = changed->menuAction();
            // The changes that make sense for recognized text; the rest (JSON, URL, Base64) stay for text copies.
            for (const texttransform::Info& info : texttransform::all()) {
                const texttransform::Kind kind = info.kind;
                if (kind != texttransform::Kind::OneLine && kind != texttransform::Kind::CollapseSpaces
                    && kind != texttransform::Kind::Trim && kind != texttransform::Kind::Upper
                    && kind != texttransform::Kind::Lower && kind != texttransform::Kind::Title) {
                    continue;
                }
                changed->addAction(info.label, this, [this, id, kind] { pasteTextFromPicture(id, kind); });
            }
        } else if (itemId == QLatin1String(pasteas::kPictureTranslate)) {
            // Read the text, then translate it with a pack.
            const int before = int(target->actions().size());
            addTranslateMenu(target, label, QString(),
                             [this, id](const QString& packId) { translatePictureAndPaste(id, packId); });
            action = target->actions().size() > before ? target->actions().last() : nullptr;
        } else if (itemId == QLatin1String(pasteas::kPicturePaste)) {
            const Entry copy = picture;
            target->addAction(label, this, [this, copy] {
                emit entryActivated(copy);
                hidePopup();
            });
        }
        if (action && !canRead && itemId != QLatin1String(pasteas::kPicturePaste)) {
            action->setEnabled(false);
            action->setToolTip(reason);
            target->setToolTipsVisible(true);
        }
    };
    const QStringList shown = pasteas::shownIds(m_pasteAsPicture, items);
    for (const QString& itemId : shown) {
        addItem(menu, itemId);
    }
    addOthersMenu(menu, items, shown, addItem);
    return menu;
}

// Orange items and a border, so the Paste as menu is not mistaken for the list of copies. On a light theme the
// orange is darker (about 4.6:1 against white instead of 2.6:1), and the selected row has light text. The popup's
// own palette decides, which follows the theme setting (a menu has its own default palette, not the theme's).
static void styleTransformMenu(QMenu* menu, const QPalette& around)
{
    const bool light = around.color(QPalette::Window).lightness() > 128;
    const QString orange = light ? QStringLiteral("#b85c00") : QStringLiteral("#e08a1e");
    const QString onOrange = light ? QStringLiteral("#ffffff") : QStringLiteral("#1b1b1b");
    menu->setStyleSheet(QStringLiteral("QMenu { border: 1px solid %1; }"
                                       "QMenu::item { color: %1; }"
                                       "QMenu::item:selected { background-color: %1; color: %2; }"
                                       "QMenu::item:disabled { color: gray; background: transparent; }")
                            .arg(orange, onOrange));
}

// The "Number" item for a copy that is one number: a submenu with the number in the other base and as bits
// (pasted when chosen), then the numbers of its set bits (0-based, as in 0x123 = 8 5 1 0) as information lines.
// A number that can only be one base shows only the other base; one that can be both shows both (hex first).
// The Number submenu, grouped by how the text is read: everything about it as a hex number together, then everything about it
// as a decimal one (a text such as 123 can be both). Each group: the number in the other base, as bits, and which bits are set
// (an information line, nothing to paste).
void PopupWindow::addNumberBits(QMenu* target, const QString& text)
{
    const std::optional<numbertext::Number> number = numbertext::parse(text);
    if (!number) {
        return;
    }
    const bool both = number->canBeHex && number->canBeDec;
    QMenu* sub = target->addMenu(tr("&Number"));
    const auto paste = [&](const QString& label, const QString& out) {
        sub->addAction(label, this, [this, out] {
            emit textActivated(out);
            hidePopup();
        });
    };
    bool first = true;
    // `readAsHex`: the text is read as a hex number (then offered as decimal), else as a decimal one (offered as hex).
    const auto group = [&](bool readAsHex, std::uint64_t value) {
        if (both) { // one reading needs no heading
            if (!first) {
                sub->addSeparator();
            }
            sub->addAction(readAsHex ? tr("Read as hex") : tr("Read as decimal"))->setEnabled(false);
        }
        first = false;
        const QString converted = readAsHex ? numbertext::decText(value) : numbertext::hexText(value);
        paste(readAsHex ? tr("To decimal: %1").arg(converted) : tr("To hex: %1").arg(converted), converted);
        const QString bits = numbertext::binaryText(value);
        paste(tr("As bits: %1").arg(bits), bits);
        if (!numbertext::setBits(value).isEmpty()) {
            sub->addAction(tr("Bits set: %1").arg(numbertext::bitsText(value)))->setEnabled(false);
        }
    };
    if (number->canBeHex) {
        group(true, number->hexValue);
    }
    if (number->canBeDec) {
        group(false, number->decValue);
    }
}

QMenu* PopupWindow::createPasteAsMenu(const Entry& entry, QWidget* parent)
{
    if (entry.image) {
        QMenu* picture = createPictureMenu(entry, parent);
        styleTransformMenu(picture, palette());
        fixMnemonics(picture);
        if (picture->isEmpty()) {
            picture->addAction(tr("No items selected: see Settings › Paste as"))->setEnabled(false);
        }
        addPasteOnceAction(picture, entry);
        return picture;
    }
    auto* menu = new QMenu(tr("Paste &as"), parent);
    styleTransformMenu(menu, palette());
    const QString text = entry.content;
    const QList<texttransform::Info> transforms = texttransform::all();
    const auto addItem = [&](QMenu* target, const QString& itemId) {
        if (itemId == QLatin1String(pasteas::kKeyboardLayout)) {
            addKeyboardLayoutEntries(target, text);
        } else if (itemId == QLatin1String(pasteas::kTranslate)) {
            addTranslateMenu(target, tr("Tran&slate"), text,
                             [this, text](const QString& packId) { translateAndPaste(packId, text); },
                             /*withPreview=*/true);
        } else if (itemId == QLatin1String(pasteas::kRich)) {
            if (entry.rich) { // a copy without a formatted version has nothing to offer here
                target->addAction(tr("With &formatting"), this, [this, entry] {
                    emit richActivated(entry);
                    hidePopup();
                });
            }
        } else if (itemId == QLatin1String(pasteas::kNumber)) {
            addNumberBits(target, text);
        } else {
            for (const texttransform::Info& info : transforms) {
                if (info.id != itemId) {
                    continue;
                }
                const texttransform::Kind kind = info.kind;
                target->addAction(info.label, this, [this, text, kind] {
                    QString why;
                    const std::optional<QString> out = texttransform::apply(kind, text, &why);
                    if (!out) {
                        showMessage(why);
                        return;
                    }
                    emit textActivated(*out);
                    hidePopup();
                });
            }
        }
    };
    const QList<pasteas::Item> items = pasteas::textItems();
    const QStringList shown = pasteas::shownIds(m_pasteAsText, items);
    for (const QString& itemId : shown) {
        addItem(menu, itemId);
    }
    addOthersMenu(menu, items, shown, addItem);
    fixMnemonics(menu);
    if (menu->isEmpty()) {
        QAction* hint = menu->addAction(tr("No items selected: see Settings › Paste as"));
        hint->setEnabled(false);
    }
    addPasteOnceAction(menu, entry);
    return menu;
}

// "Paste once" (a toggle) at the end of a Paste as menu.
void PopupWindow::addPasteOnceAction(QMenu* menu, const Entry& entry)
{
    const qint64 id = entry.id;
    menu->addSeparator();
    QAction* once = menu->addAction(tr("Paste on&ce"), this, [this, id] { togglePasteOnce(id); });
    once->setCheckable(true);
    once->setChecked(entry.pasteOnce);
}

// One mnemonic letter per shown item: the later of two items with the same letter loses it.
void PopupWindow::fixMnemonics(QMenu* menu)
{
    const QList<QAction*> actions = menu->actions();
    QStringList texts;
    for (const QAction* a : actions) {
        texts << a->text();
    }
    const QStringList fixed = pasteas::uniqueMnemonics(texts);
    for (qsizetype i = 0; i < actions.size(); ++i) {
        if (!actions[i]->isSeparator() && actions[i]->text() != fixed[i]) {
            actions[i]->setText(fixed[i]);
        }
    }
}

void PopupWindow::addTranslateMenu(QMenu* menu, const QString& title, const QString& text,
                                   std::function<void(const QString& packId)> chosen, bool withPreview)
{
    if (!m_translation.builtIn || !m_translation.packsFor) {
        return;
    }
    const QList<TranslationPackInfo> packs = m_translation.packsFor(text);
    if (packs.isEmpty()) {
        QAction* none = menu->addAction(title);
        none->setEnabled(false);
        none->setToolTip(m_translation.packsFor(QString()).isEmpty()
                             ? tr("No language pack is installed: add one in Settings › Translation.")
                             : tr("No installed language pack translates from the language of this text."));
        menu->setToolTipsVisible(true);
        return;
    }
    QMenu* sub = menu->addMenu(title);
    for (const TranslationPackInfo& pack : packs) {
        const QString id = pack.id;
        sub->addAction(pack.label, this, [chosen, id] { chosen(id); });
    }
    if (withPreview) {
        // The same as Ctrl+T: see the translation first, paste it only if you choose to.
        QAction* show = sub->addAction(tr("Show the translation only\tCtrl+T"), this, [this] { previewTranslation(); });
        show->setObjectName(QStringLiteral("translate_preview"));
        show->setToolTip(tr("Translate and show the result in a window; nothing is pasted unless you choose Paste there."));
        sub->setToolTipsVisible(true);
    }
}

// Ctrl+T: the selected copy translated with the first installed pack that fits its language, shown but not pasted.
void PopupWindow::previewTranslation()
{
    if (isBusy() || !m_translation.builtIn || !m_translation.packsFor || !m_translation.translate) {
        return;
    }
    const Entry* entry = entryFor(m_view->currentIndex());
    if (!entry || entry->image || entry->content.trimmed().isEmpty()) {
        return;
    }
    const QString text = entry->content;
    const QList<TranslationPackInfo> packs = m_translation.packsFor(text);
    if (packs.isEmpty()) {
        showQuiet(m_translation.packsFor(QString()).isEmpty()
                      ? tr("No language pack is installed: add one in Settings › Translation.")
                      : tr("No installed language pack translates from the language of this text."));
        return;
    }
    setBusy(tr("Translating…"));
    translateWithTimeout(packs.first().id, text, [this, self = QPointer<PopupWindow>(this)](const QString& out, const QString& error) {
        if (!self) {
            return;
        }
        setBusy(QString());
        if (!error.isEmpty()) {
            showMessage(error); // a real problem: the warning box
        } else if (out.trimmed().isEmpty()) {
            showQuiet(tr("The translation is empty."));
        } else if (showQuiet(out, /*offerPaste=*/true)) {
            emit textActivated(out); // Paste was chosen in the window
            hidePopup();
        }
    });
}

// Translating runs on a worker thread and cannot be cancelled. If it has not answered after a while the popup stops waiting
// (so it never stays stuck on "Translating…") and says so; a late answer is ignored.
void PopupWindow::translateWithTimeout(const QString& packId, const QString& text,
                                       std::function<void(const QString&, const QString&)> done)
{
    auto finished = std::make_shared<bool>(false);
    QTimer::singleShot(m_translateTimeoutMs, this, [finished, done, this] {
        if (*finished) {
            return;
        }
        *finished = true;
        done(QString(), tr("The translation did not finish in %1 seconds, so ClipItTo stopped waiting. Details are in "
                           "translation/translation.log in ClipItTo's data folder.")
                            .arg(m_translateTimeoutMs / 1000));
    });
    m_translation.translate(packId, text, [finished, done](const QString& out, const QString& error) {
        if (*finished) {
            return; // it came back after the popup gave up
        }
        *finished = true;
        done(out, error);
    });
}

bool PopupWindow::showQuiet(const QString& text, bool offerPaste)
{
    ModalGuard guard(m_modalDepth);
    return m_viewText && m_viewText(this, text, offerPaste);
}

void PopupWindow::translateAndPaste(const QString& packId, const QString& text)
{
    if (!m_translation.translate) {
        return;
    }
    setBusy(tr("Translating…"));
    translateWithTimeout(packId, text, [this, self = QPointer<PopupWindow>(this)](const QString& out, const QString& error) {
        if (!self) {
            return;
        }
        setBusy(QString());
        if (!error.isEmpty()) {
            showMessage(error);
        } else if (out.trimmed().isEmpty()) {
            showQuiet(tr("The translation is empty."));
        } else {
            emit textActivated(out);
            hidePopup();
        }
    });
}

// A picture: its text is read first, then translated; the pack has to fit the language of that text.
void PopupWindow::translatePictureAndPaste(qint64 id, const QString& packId)
{
    if (!m_textFromPicture || !m_textFromPictureAvailable || !m_translation.translate) {
        return;
    }
    setBusy(tr("Reading and translating the text in the picture…"));
    m_textFromPicture(id, [this, packId, self = QPointer<PopupWindow>(this)](const QString& text, const QString& error) {
        if (!self) {
            return;
        }
        if (!error.isEmpty() || text.trimmed().isEmpty()) {
            setBusy(QString());
            if (error.isEmpty()) {
                showQuiet(tr("No text found in the picture."));
            } else {
                showMessage(error);
            }
            return;
        }
        if (m_translation.fits && !m_translation.fits(packId, text)) {
            setBusy(QString());
            showQuiet(tr("The text in the picture does not look like the language this pack translates from."));
            return;
        }
        translateWithTimeout(packId, text, [this, self](const QString& out, const QString& err) {
            if (!self) {
                return;
            }
            setBusy(QString());
            if (!err.isEmpty()) {
                showMessage(err);
            } else if (out.trimmed().isEmpty()) {
                showQuiet(tr("The translation is empty."));
            } else {
                emit textActivated(out);
                hidePopup();
            }
        });
    });
}

// Text typed with the wrong keyboard layout, converted key for key. One layout to go to (the opposite of the
// text's language): one entry. Several: a submenu with each.
void PopupWindow::addKeyboardLayoutEntries(QMenu* menu, const QString& text)
{
    const QString from = texttransform::layoutOf(text, m_keyboardLayouts);
    const QStringList targets = texttransform::convertibleTo(text, m_keyboardLayouts);
    if (m_keyboardLayouts.isEmpty() || targets.isEmpty()) {
        return;
    }
    const auto add = [this, text, from](QMenu* into, const QString& to, const QString& label) {
        into->addAction(label, this, [this, text, from, to] {
            QString why;
            const std::optional<QString> out = texttransform::convertLayout(text, from, to, &why);
            if (!out) {
                showMessage(why);
                return;
            }
            emit textActivated(*out);
            hidePopup();
        });
    };
    // A short preview of the result in the label, so the right layout is easy to pick.
    const auto preview = [&text, &from](const QString& to) {
        const std::optional<QString> out = texttransform::convertLayout(text, from, to);
        QString shown = out.value_or(QString()).simplified();
        if (shown.size() > 24) {
            shown = shown.left(24) + QStringLiteral("…");
        }
        return shown;
    };
    if (targets.size() == 1) {
        add(menu, targets.first(),
            tr("As %1 &keyboard: %2").arg(texttransform::layoutName(targets.first()), preview(targets.first())));
        return;
    }
    QMenu* sub = menu->addMenu(tr("Another &keyboard layout"));
    for (const QString& to : targets) {
        add(sub, to, tr("%1: %2").arg(texttransform::layoutName(to), preview(to)));
    }
}

void PopupWindow::pasteAs(const QModelIndex& index)
{
    const Entry* e = entryFor(index);
    if (!e || e->secret) {
        return; // a group row or a password: nothing to choose from
    }
    QMenu* menu = createPasteAsMenu(*e, this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction* first = nullptr; // the first choice is ready: one Enter picks it
    for (QAction* action : menu->actions()) {
        if (!action->isSeparator() && action->isEnabled()) {
            first = action;
            break;
        }
    }
    const QRect row = m_view->visualRect(index);
    {
        ModalGuard guard(m_modalDepth); // the menu takes the focus; the popup must not hide because of it
        menu->exec(m_view->viewport()->mapToGlobal(row.bottomLeft()), first);
    }
    if (isVisible()) {
        m_search->setFocus();
    }
}

void PopupWindow::pasteTextFromPicture(qint64 id, std::optional<texttransform::Kind> kind)
{
    if (!m_textFromPicture || !m_textFromPictureAvailable) {
        return;
    }
    setBusy(tr("Reading the text in the picture…")); // not a message box: that would wait for a click
    m_textFromPicture(id, [this, kind, self = QPointer<PopupWindow>(this)](const QString& text, const QString& error) {
        if (!self) {
            return;
        }
        setBusy(QString());
        if (!error.isEmpty()) {
            showMessage(error);
        } else if (text.trimmed().isEmpty()) {
            showQuiet(tr("No text found in the picture."));
        } else {
            QString out = text;
            if (kind) {
                QString why;
                const std::optional<QString> changed = texttransform::apply(*kind, text, &why);
                if (!changed) {
                    showMessage(why);
                    return;
                }
                out = *changed;
            }
            emit textActivated(out); // on the clipboard, and pasted like any copy
            hidePopup();
        }
    });
}

void PopupWindow::addOutOfRecentAction(QMenu* menu, const QString& path)
{
    bool out = false;
    try {
        out = path.isEmpty() ? m_repo.favoritesHiddenFromRecent() : m_repo.groupHiddenFromRecent(path);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "reading the Recent rule failed:" << err.what();
    }
    QAction* action = menu->addAction(tr("Keep out of Recent"), this,
                                      [this, path](bool checked) { setGroupOutOfRecent(path, checked); });
    action->setCheckable(true);
    action->setChecked(out);
    action->setToolTip(tr("Copies in this group don't appear in the Recent list (they stay here)."));
}

void PopupWindow::setGroupOutOfRecent(const QString& path, bool out)
{
    try {
        if (path.isEmpty()) {
            m_repo.setFavoritesHiddenFromRecent(out);
        } else {
            m_repo.setGroupHiddenFromRecent(path, out);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "saving the Recent rule failed:" << err.what();
    }
    refreshSelecting(selectionOf(m_view->currentIndex()));
}

QMenu* PopupWindow::createFavoritesMenu(QWidget* parent)
{
    auto* menu = new QMenu(parent);
    addOutOfRecentAction(menu, QString());
    return menu;
}

QMenu* PopupWindow::createGroupMenu(const QString& path, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    if (path.isEmpty()) {
        menu->addAction(tr("New group…"), this, [this] { newGroup(QString()); });
        return menu;
    }
    menu->addAction(tr("New sub-group…"), this, [this, path] { newGroup(path); });
    menu->addAction(tr("Rename group…\tF2"), this, [this, path] { renameGroup(path); });
    menu->addAction(tr("Move up (Alt+↑)"), this, [this, path] { moveGroup(path, -1); });
    menu->addAction(tr("Move down (Alt+↓)"), this, [this, path] { moveGroup(path, 1); });
    addOutOfRecentAction(menu, path);
    menu->addSeparator();
    menu->addAction(tr("Delete group"), this, [this, path] { deleteGroup(path); });
    return menu;
}

void PopupWindow::showContextMenu(const QModelIndex& index, const QPoint& globalPos)
{
    QMenu* menu = nullptr;
    if (const Entry* e = entryFor(index)) {
        menu = createContextMenu(*e, this);
    } else if (m_model->kindAt(index) == Kind::Group || m_model->kindAt(index) == Kind::GroupsRoot) {
        menu = createGroupMenu(expandKey(*m_model, index), this);
    } else if (m_model->kindAt(index) == Kind::Favorites) {
        menu = createFavoritesMenu(this);
    }
    if (!menu) {
        return;
    }
    {
        ModalGuard guard(m_modalDepth);
        menu->exec(globalPos);
    }
    menu->deleteLater();
}

bool PopupWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        hidePopup();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

bool PopupWindow::startSystemMove()
{
    QWindow* handle = windowHandle();
    if (!handle) {
        return false;
    }
    // Set first: the compositor may take focus away as soon as the move starts.
    m_systemMove = true;
    m_pointerLeftDuringMove = false;
    if (!(m_systemMoveStarter ? m_systemMoveStarter(handle) : handle->startSystemMove())) {
        m_systemMove = false;
        return false;
    }
    // Backstop, should no sign of the move's end ever arrive.
    m_moveEnd->start(kSystemMoveTimeoutMs);
    return true;
}

void PopupWindow::endSystemMove()
{
    if (!m_systemMove) {
        return;
    }
    m_systemMove = false;
    m_moveEnd->stop();
    // A focus loss during the move was let pass; if the focus didn't come back, act on it now.
    if (isVisible() && !isActiveWindow() && m_hideOnFocusLoss && m_modalDepth == 0) {
        hidePopup();
    }
}

bool PopupWindow::event(QEvent* event)
{
    if (m_systemMove) {
        if (event->type() == QEvent::Leave) {
            m_pointerLeftDuringMove = true; // the compositor grabbed the pointer
        } else if (event->type() == QEvent::Enter && m_pointerLeftDuringMove) {
            // The compositor gave the pointer back: the move is over. Its focus may follow
            // a moment later, so give that time before deciding to close.
            m_moveEnd->start(kSystemMoveSettleMs);
        }
    }
    return QWidget::event(event);
}

void PopupWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::ActivationChange || !isVisible()) {
        return;
    }
    if (isActiveWindow()) {
        if (m_systemMove) { // a compositor move ends with the focus back here
            m_systemMove = false;
            m_moveEnd->stop();
        }
        return;
    }
    // While the compositor moves us (Wayland) it may hold the focus: no reason to close yet.
    if (m_hideOnFocusLoss && m_modalDepth == 0 && !m_systemMove) {
        hidePopup();
    }
}

void PopupWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    emit visibilityChanged(true);
}

void PopupWindow::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // Nothing of a password stays on screen or in the search box once closed.
    m_revealTimer->stop();
    m_model->clearRevealed();
    m_search->leavePasswordMode();
    m_systemMove = false;
    m_moveEnd->stop();
    if (QGuiApplication::platformName() != QLatin1String("wayland") && m_lastPosition != pos()) {
        m_lastPosition = pos(); // wherever it was placed or dragged to
        emit closedAt(pos());
    }
    emit visibilityChanged(false);
}

} // namespace clipitto::ui
