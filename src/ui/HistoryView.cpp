#include "HistoryView.h"

#include "HistoryModel.h"

#include <QKeyEvent>
#include <QPainter>

namespace clipitto::ui {

HistoryView::HistoryView(QWidget* parent) : QTreeView(parent)
{
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(18);
    setItemsExpandable(true);
    setExpandsOnDoubleClick(false); // clicks are handled by the popup
    setUniformRowHeights(false);
    setAnimated(false);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setMouseTracking(true); // hover highlight
    viewport()->setAttribute(Qt::WA_Hover);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFrameShape(QFrame::NoFrame);

    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = indexAt(pos);
        if (index.flags() & Qt::ItemIsEnabled) {
            selectIndex(index);
            emit contextMenuRequested(index, viewport()->mapToGlobal(pos));
        }
    });
}

void HistoryView::setHistoryModel(HistoryModel* model)
{
    m_model = model;
    setModel(model);
}

void HistoryView::selectIndex(const QModelIndex& index)
{
    if (index.isValid()) {
        setCurrentIndex(index); // SingleSelection: also selects
        scrollTo(index);
    }
}

void HistoryView::selectFirstEntry()
{
    if (m_model) {
        selectIndex(m_model->firstEntry());
        if (currentIndex() == firstSelectable() || currentIndex().row() <= 2) {
            scrollToTop(); // keep the Groups row and a leading section header visible
        }
    }
}

void HistoryView::selectFirstRow()
{
    selectIndex(firstSelectable());
    scrollToTop();
}

QModelIndex HistoryView::firstSelectable() const
{
    if (!m_model || m_model->rowCount() == 0) {
        return {};
    }
    const QModelIndex first = m_model->index(0, 0);
    return (first.flags() & Qt::ItemIsSelectable) ? first : nextSelectable(first, true);
}

QModelIndex HistoryView::lastSelectable() const
{
    if (!m_model || m_model->rowCount() == 0) {
        return {};
    }
    // The last visible row: descend into expanded last children.
    QModelIndex last = m_model->index(m_model->rowCount() - 1, 0);
    while (isExpanded(last) && m_model->rowCount(last) > 0) {
        last = m_model->index(m_model->rowCount(last) - 1, 0, last);
    }
    return (last.flags() & Qt::ItemIsSelectable) ? last : nextSelectable(last, false);
}

QModelIndex HistoryView::nextSelectable(QModelIndex from, bool down) const
{
    for (QModelIndex i = down ? indexBelow(from) : indexAbove(from); i.isValid();
         i = down ? indexBelow(i) : indexAbove(i)) {
        if (i.flags() & Qt::ItemIsSelectable) {
            return i;
        }
    }
    return {};
}

void HistoryView::toggleExpanded(const QModelIndex& index)
{
    if (m_model && m_model->isExpandable(index)) {
        setExpanded(index, !isExpanded(index));
    }
}

void HistoryView::setEmptyText(const QString& text)
{
    m_emptyText = text;
    viewport()->update();
}

void HistoryView::paintEvent(QPaintEvent* event)
{
    QTreeView::paintEvent(event);
    if (m_model && m_model->rowCount() == 0 && !m_emptyText.isEmpty()) {
        QPainter painter(viewport());
        painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        painter.drawText(viewport()->rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
                         m_emptyText);
    }
}

QModelIndex HistoryView::moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers)
{
    if (!m_model) {
        return QTreeView::moveCursor(action, modifiers);
    }
    const QModelIndex current = currentIndex();
    QModelIndex target;
    switch (action) {
    case MoveUp:
    case MovePrevious:
        target = current.isValid() ? nextSelectable(current, false) : lastSelectable();
        break;
    case MoveDown:
    case MoveNext:
        target = current.isValid() ? nextSelectable(current, true) : firstSelectable();
        break;
    case MoveHome:
        target = firstSelectable();
        break;
    case MoveEnd:
        target = lastSelectable();
        break;
    case MoveLeft:
    case MoveRight:
        return QTreeView::moveCursor(action, modifiers); // collapse / expand / parent / child
    default: {
        // Page moves: let the tree pick a row, then settle on the nearest selectable one.
        const QModelIndex index = QTreeView::moveCursor(action, modifiers);
        if (!index.isValid() || (index.flags() & Qt::ItemIsSelectable)) {
            return index;
        }
        target = nextSelectable(index, true);
        if (!target.isValid()) {
            target = nextSelectable(index, false);
        }
    }
    }
    return target.isValid() ? target : current;
}

void HistoryView::keyPressEvent(QKeyEvent* event)
{
    const QModelIndex current = currentIndex();
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;

    if (mods == Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_S:
            emit toggleFavoriteRequested(current);
            return;
        case Qt::Key_G:
            emit editGroupRequested(current);
            return;
        case Qt::Key_E:
            emit editNoteRequested(current);
            return;
        default:
            break;
        }
    }

    switch (event->key()) {
    case Qt::Key_Up:
        if (!current.isValid() || !nextSelectable(current, false).isValid()) {
            emit focusSearchRequested();
            return;
        }
        break;
    case Qt::Key_Right:
        // Right on a collapsed group opens it (and on the Groups row from anywhere in it).
        if (m_model && m_model->isExpandable(current) && !isExpanded(current)) {
            expand(current);
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_model && m_model->isExpandable(current)) {
            toggleExpanded(current);
        } else {
            emit activateRequested(current);
        }
        return;
    case Qt::Key_Delete:
        emit deleteRequested(current);
        return;
    case Qt::Key_Menu:
        if (current.isValid()) {
            emit contextMenuRequested(current, viewport()->mapToGlobal(visualRect(current).center()));
        }
        return;
    case Qt::Key_Backspace:
        emit typedText(event);
        return;
    default:
        if (!event->text().isEmpty() && event->text().at(0).isPrint()
            && !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            emit typedText(event);
            return;
        }
        break;
    }
    QTreeView::keyPressEvent(event);
}

} // namespace clipitto::ui
