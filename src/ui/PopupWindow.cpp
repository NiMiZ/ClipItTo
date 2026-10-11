#include "PopupWindow.h"

#include "AppIcon.h"
#include "Config.h"
#include "GroupPath.h"
#include "HistoryDelegate.h"
#include "HistoryView.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "Theme.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCursor>
#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QMessageBox>
#include <QScreen>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <utility>

Q_LOGGING_CATEGORY(lcPopup, "clipitto.popup")

namespace clipitto::ui {

namespace {

using Kind = HistoryModel::Kind;

std::optional<QString> askGroup(QWidget* parent, const QString& current, const QStringList& choices)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(PopupWindow::tr("Assign group"));
    dialog.setLabelText(PopupWindow::tr("Group (use / for sub-groups, e.g. Work/Servers; empty to remove):"));
    dialog.setComboBoxItems(choices);
    dialog.setComboBoxEditable(true);
    dialog.setTextValue(current);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.textValue();
}

std::optional<QString> askNote(QWidget* parent, const QString& current, const QStringList&)
{
    bool ok = false;
    const QString text = QInputDialog::getText(parent, PopupWindow::tr("Edit note"),
                                               PopupWindow::tr("Title shown instead of the clipboard text:"),
                                               QLineEdit::Normal, current, &ok);
    return ok ? std::optional<QString>(text) : std::nullopt;
}

// Keeps the popup open while a dialog or menu has focus.
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
    DragHandle(std::function<void()> moved, QWidget* parent) : QLabel(parent), m_moved(std::move(moved))
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
            if (QWindow* handle = window()->windowHandle()) {
                handle->startSystemMove();
            }
            return;
        }
        // Moved by us, not the window manager, so the popup keeps keyboard focus.
        window()->move(event->globalPosition().toPoint() - m_grab);
        m_moved();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        QLabel::mouseReleaseEvent(event);
    }

private:
    std::function<void()> m_moved;
    QPoint m_grab; // pointer position relative to the window's top-left corner
    bool m_dragging = false;
};

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
    , m_dragHandle(new DragHandle([this] { m_userPosition = pos(); }, this))
    , m_view(new HistoryView(this))
    , m_footer(new QLabel(this))
    , m_groupPrompt(askGroup)
    , m_notePrompt(askNote)
    , m_textPrompt(defaultTextPrompt())
    , m_confirm(defaultConfirmPrompt())
    , m_notify([](QWidget* parent, const QString& message) {
        QMessageBox::warning(parent, QStringLiteral("ClipItTo"), message);
    })
{
    setWindowTitle(QStringLiteral("ClipItTo"));
    resize(560, 480);

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
    m_settingsButton->setToolTip(tr("Settings (Ctrl+,)"));
    connect(m_settingsButton, &QToolButton::clicked, this, &PopupWindow::settingsRequested);
    auto* settingsShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma), this);
    connect(settingsShortcut, &QShortcut::activated, this, &PopupWindow::settingsRequested);

    m_footer->setText(tr("Enter copy · → open · Ctrl+S ★ · Ctrl+P pin · Ctrl+G group · Ctrl+E note · "
                         "Alt+↑↓ move · Del remove"));
    m_footer->setToolTip(tr("Alt+↑/↓ moves a group, or a pinned copy, up or down. Esc closes."));
    m_footer->setFont(scaledFont(m_footer->font(), 0.85));
    m_footer->setEnabled(false); // dim

    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto* handle = static_cast<QLabel*>(m_dragHandle);
    const int iconSize = handle->fontMetrics().height();
    handle->setText(QStringLiteral("ClipItTo"));
    handle->setFont(scaledFont(handle->font(), 0.9));
    auto* icon = new DragHandle([this] { m_userPosition = pos(); }, this);
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
    top->addWidget(m_settingsButton);
    auto* inner = new QVBoxLayout(frame);
    inner->setContentsMargins(8, 8, 8, 6);
    inner->setSpacing(6);
    inner->addLayout(top);
    inner->addWidget(m_view, 1);
    inner->addWidget(m_footer);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(frame);

    m_search->installEventFilter(this);
    m_view->installEventFilter(this);

    connect(m_search, &QLineEdit::textChanged, m_controller, &SearchController::setQuery);
    connect(m_controller, &SearchController::updated, this, &PopupWindow::onResultsUpdated);

    connect(m_search, &SearchBar::focusListRequested, this, [this] {
        m_view->setFocus();
        m_view->selectFirstRow(); // the row right under the search box (Groups, if any)
    });
    connect(m_search, &SearchBar::openGroupsRequested, this, &PopupWindow::openGroups);
    connect(m_search, &SearchBar::activateRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh(); // Enter right after typing: use the up-to-date results
        }
        // From the search box, Enter always copies: the selected copy, else the first one.
        const QModelIndex current = m_view->currentIndex();
        activate(entryFor(current) ? current : m_model->firstEntry());
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
    m_controller->applyConfig(config);
    m_hideOnFocusLoss = config.hideOnFocusLoss;
    m_singleClick = config.clickToCopy == ClickMode::Single;
    m_centered = config.popupPosition == PopupPosition::Center;
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
    }
    if (!activationToken.isEmpty() && QGuiApplication::platformName() == QLatin1String("wayland")) {
        // Lets Qt's Wayland plugin activate us with the compositor-issued xdg-activation token.
        qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
    }
    show();
    raise();
    activateWindow();
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
    const QRect area = screen->availableGeometry();
    if (m_userPosition) {
        // Where the user dragged it, as long as that is still on a screen.
        const QRect wanted(*m_userPosition, size());
        if (QScreen* there = QGuiApplication::screenAt(wanted.center())) {
            const QRect bounds = there->availableGeometry();
            move(qBound(bounds.left(), wanted.left(), bounds.right() - width()),
                 qBound(bounds.top(), wanted.top(), bounds.bottom() - height()));
            return;
        }
        m_userPosition.reset(); // that screen is gone
    }
    if (m_centered) {
        // Same place every time: centered horizontally, in the upper third.
        move(area.left() + (area.width() - width()) / 2, area.top() + (area.height() - height()) / 3);
        return;
    }
    QPoint topLeft = cursor - QPoint(width() / 2, 24);
    topLeft.setX(qBound(area.left(), topLeft.x(), area.right() - width()));
    topLeft.setY(qBound(area.top(), topLeft.y(), area.bottom() - height()));
    move(topLeft);
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
    const QModelIndex target = index.isValid() ? index : m_model->firstEntry();
    if (m_model->isExpandable(target)) {
        m_view->toggleExpanded(target);
        return;
    }
    const Entry* e = entryFor(target);
    if (!e) {
        return; // a header or an empty group
    }
    const Entry chosen = *e; // receivers may refresh the model
    emit entryActivated(chosen);
    hidePopup();
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

void PopupWindow::assignGroup(qint64 id, const QString& group, Selection follow)
{
    QString problem;
    try {
        const auto change = m_repo.setGroup(id, group);
        problem = refusal(change, m_repo.limits(), groups::normalize(group));
        if (problem.isEmpty() && follow.kind == Kind::Entry && follow.id == id) {
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
        std::optional<QString> note;
        {
            ModalGuard guard(m_modalDepth);
            note = m_notePrompt(this, e->note, {});
        }
        if (note) {
            m_repo.setNote(id, *note);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "edit note failed:" << err.what();
    }
    refreshSelecting(selectionOf(m_view->currentIndex()));
}

void PopupWindow::remove(const QModelIndex& index)
{
    const Entry* e = entryFor(index);
    if (!e) {
        return;
    }
    const qint64 id = e->id;
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
        m_repo.remove(id);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "delete failed:" << err.what();
    }
    refreshSelecting(next);
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
    m_view->setEmptyText(m_controller->query().trimmed().isEmpty()
                             ? tr("Nothing copied yet. Copied text will show up here.")
                             : tr("No matches"));

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
        m_view->selectFirstEntry();
    }
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

    menu->addAction(entry.note.isEmpty() ? tr("Add note…") : tr("Edit note…"), this, [this, id] { editNote(id); });
    menu->addSeparator();
    menu->addAction(tr("Delete"), this, [this, id] {
        const QModelIndex index = m_model->findEntry(id);
        remove(index.isValid() ? index : m_view->currentIndex());
    });
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
    menu->addAction(tr("Rename or move…"), this, [this, path] { renameGroup(path); });
    menu->addAction(tr("Move up (Alt+↑)"), this, [this, path] { moveGroup(path, -1); });
    menu->addAction(tr("Move down (Alt+↓)"), this, [this, path] { moveGroup(path, 1); });
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

void PopupWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isVisible() && !isActiveWindow() && m_hideOnFocusLoss
        && m_modalDepth == 0) {
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
    emit visibilityChanged(false);
}

} // namespace clipitto::ui
