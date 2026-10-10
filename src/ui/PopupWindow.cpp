#include "PopupWindow.h"

#include "Config.h"
#include "HistoryDelegate.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCursor>
#include <QGuiApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QScreen>
#include <QVBoxLayout>

Q_LOGGING_CATEGORY(lcPopup, "clipitto.popup")

namespace clipitto::ui {

namespace {

std::optional<QString> askGroup(QWidget* parent, const QString& current, const QStringList& choices)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(PopupWindow::tr("Assign group"));
    dialog.setLabelText(PopupWindow::tr("Group (leave empty to remove):"));
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

} // namespace

PopupWindow::PopupWindow(db::Repository& repo, const Config& config, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_repo(repo)
    , m_model(new HistoryModel(this))
    , m_controller(new SearchController(repo, *m_model, this))
    , m_search(new SearchBar(this))
    , m_view(new HistoryView(this))
    , m_footer(new QLabel(this))
    , m_groupPrompt(askGroup)
    , m_notePrompt(askNote)
{
    setWindowTitle(QStringLiteral("ClipItTo"));
    resize(560, 480);

    m_view->setHistoryModel(m_model);
    m_view->setItemDelegate(new HistoryDelegate(m_view));

    m_footer->setText(tr("Enter copy · Ctrl+S ★ · Ctrl+G group · Ctrl+E note · Del remove · Esc close"));
    m_footer->setFont(scaledFont(m_footer->font(), 0.85));
    m_footer->setEnabled(false); // dim

    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto* inner = new QVBoxLayout(frame);
    inner->setContentsMargins(8, 8, 8, 6);
    inner->setSpacing(6);
    inner->addWidget(m_search);
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
        m_view->selectFirstEntry();
    });
    connect(m_search, &SearchBar::activateRequested, this, [this] {
        if (m_controller->isPending()) {
            m_controller->refresh(); // Enter right after typing: use the up-to-date results
        }
        activate(m_view->currentIndex());
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
        if (m_singleClick) {
            activate(index);
        }
    });
    connect(m_view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!m_singleClick) {
            activate(index);
        }
    });
    connect(m_view, &HistoryView::toggleFavoriteRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            toggleFavorite(e->id);
        }
    });
    connect(m_view, &HistoryView::editGroupRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            editGroup(e->id);
        }
    });
    connect(m_view, &HistoryView::editNoteRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            editNote(e->id);
        }
    });
    connect(m_view, &HistoryView::deleteRequested, this, [this](const QModelIndex& index) {
        if (const Entry* e = entryFor(index)) {
            remove(e->id);
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
}

void PopupWindow::showPopup(const QString& activationToken)
{
    if (!isVisible()) {
        m_restoreId = 0;
        if (m_search->text().isEmpty()) {
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

void PopupWindow::historyChanged()
{
    if (isVisible() && m_search->text().trimmed().isEmpty()) {
        if (const Entry* e = entryFor(m_view->currentIndex())) {
            m_restoreId = e->id;
        }
        m_controller->refresh();
    }
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
    QPoint topLeft = cursor - QPoint(width() / 2, 24);
    topLeft.setX(qBound(area.left(), topLeft.x(), area.right() - width()));
    topLeft.setY(qBound(area.top(), topLeft.y(), area.bottom() - height()));
    move(topLeft);
}

const Entry* PopupWindow::entryFor(const QModelIndex& index) const
{
    return index.isValid() ? m_model->entryAt(index.row()) : nullptr;
}

void PopupWindow::activate(const QModelIndex& index)
{
    const Entry* e = entryFor(index);
    if (!e) {
        e = m_model->entryAt(m_model->firstEntryRow());
    }
    if (!e) {
        return;
    }
    const Entry chosen = *e; // receivers may refresh the model
    emit entryActivated(chosen);
    hidePopup();
}

void PopupWindow::toggleFavorite(qint64 id)
{
    try {
        if (const auto e = m_repo.get(id)) {
            m_repo.setFavorite(id, !e->favorite);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "toggle favorite failed:" << err.what();
    }
    afterEdit(id);
}

void PopupWindow::editGroup(qint64 id)
{
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
            m_repo.setGroup(id, *group);
        }
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "assign group failed:" << err.what();
    }
    afterEdit(id);
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
    afterEdit(id);
}

void PopupWindow::remove(qint64 id)
{
    // Keep the cursor in place: select the neighbor that slides into this row.
    const int row = m_model->rowForId(id);
    int neighbor = m_model->nextEntryRow(row, +1);
    if (neighbor < 0) {
        neighbor = m_model->nextEntryRow(row, -1);
    }
    const Entry* next = m_model->entryAt(neighbor);
    const qint64 nextId = next ? next->id : 0;
    try {
        m_repo.remove(id);
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "delete failed:" << err.what();
    }
    afterEdit(nextId);
}

void PopupWindow::afterEdit(qint64 selectId)
{
    m_restoreId = selectId;
    m_controller->refresh();
    activateWindow(); // dialogs may have taken focus
    m_view->setFocus();
}

void PopupWindow::onResultsUpdated()
{
    m_view->setEmptyText(m_controller->query().trimmed().isEmpty()
                             ? tr("Nothing copied yet. Copied text will show up here.")
                             : tr("No matches"));
    const int row = m_restoreId ? m_model->rowForId(m_restoreId) : -1;
    m_restoreId = 0;
    if (row >= 0) {
        m_view->selectRow(row);
    } else {
        m_view->selectFirstEntry();
    }
}

QMenu* PopupWindow::createContextMenu(const Entry& entry, QWidget* parent)
{
    const qint64 id = entry.id;
    auto* menu = new QMenu(parent);
    menu->addAction(entry.favorite ? tr("Remove from favorites") : tr("★ Add to favorites"),
                    this, [this, id] { toggleFavorite(id); });

    QMenu* groups = menu->addMenu(tr("Group"));
    QStringList existing;
    try {
        existing = m_repo.groups();
    } catch (const db::DbError& err) {
        qCWarning(lcPopup) << "listing groups failed:" << err.what();
    }
    for (const QString& group : existing) {
        QAction* action = groups->addAction(group, this, [this, id, group] {
            try {
                m_repo.setGroup(id, group);
            } catch (const db::DbError& err) {
                qCWarning(lcPopup) << "assign group failed:" << err.what();
            }
            afterEdit(id);
        });
        action->setCheckable(true);
        action->setChecked(group == entry.group);
    }
    if (!existing.isEmpty()) {
        groups->addSeparator();
    }
    groups->addAction(tr("New group…"), this, [this, id] { editGroup(id); });
    if (!entry.group.isEmpty()) {
        groups->addAction(tr("Remove from group"), this, [this, id] {
            try {
                m_repo.setGroup(id, QString());
            } catch (const db::DbError& err) {
                qCWarning(lcPopup) << "remove group failed:" << err.what();
            }
            afterEdit(id);
        });
    }

    menu->addAction(entry.note.isEmpty() ? tr("Add note…") : tr("Edit note…"), this, [this, id] { editNote(id); });
    menu->addSeparator();
    menu->addAction(tr("Delete"), this, [this, id] { remove(id); });
    return menu;
}

void PopupWindow::showContextMenu(const QModelIndex& index, const QPoint& globalPos)
{
    const Entry* e = entryFor(index);
    if (!e) {
        return;
    }
    QMenu* menu = createContextMenu(*e, this);
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
