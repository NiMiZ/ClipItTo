#include "HistoryView.h"

#include "HistoryModel.h"

#include <QKeyEvent>
#include <QPainter>

namespace clipitto::ui {

HistoryView::HistoryView(QWidget* parent) : QListView(parent)
{
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
            selectRow(index.row());
            emit contextMenuRequested(index, viewport()->mapToGlobal(pos));
        }
    });
}

void HistoryView::setHistoryModel(HistoryModel* model)
{
    m_model = model;
    setModel(model);
}

void HistoryView::selectRow(int row)
{
    if (!m_model || row < 0 || row >= m_model->rowCount()) {
        return;
    }
    const QModelIndex index = m_model->index(row);
    setCurrentIndex(index); // SingleSelection: also selects
    scrollTo(index);
}

void HistoryView::selectFirstEntry()
{
    if (m_model) {
        const int first = m_model->firstEntryRow();
        if (first >= 0) {
            selectRow(first);
            scrollToTop(); // keep a leading section header visible
        }
    }
}

void HistoryView::setEmptyText(const QString& text)
{
    m_emptyText = text;
    viewport()->update();
}

void HistoryView::paintEvent(QPaintEvent* event)
{
    QListView::paintEvent(event);
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
        return QListView::moveCursor(action, modifiers);
    }
    const int current = currentIndex().isValid() ? currentIndex().row() : -1;
    int target = -1;
    switch (action) {
    case MoveUp:
    case MovePrevious:
        target = m_model->nextEntryRow(current, -1);
        break;
    case MoveDown:
    case MoveNext:
        target = m_model->nextEntryRow(current, +1);
        break;
    case MoveHome:
        target = m_model->firstEntryRow();
        break;
    case MoveEnd:
        target = m_model->nextEntryRow(m_model->rowCount(), -1);
        break;
    default: {
        // Page moves: let QListView pick a row, then settle on the nearest entry.
        const QModelIndex index = QListView::moveCursor(action, modifiers);
        if (!index.isValid() || m_model->entryAt(index.row())) {
            return index;
        }
        const int down = m_model->nextEntryRow(index.row(), +1);
        target = down >= 0 ? down : m_model->nextEntryRow(index.row(), -1);
    }
    }
    return target >= 0 ? m_model->index(target) : currentIndex();
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
        if (m_model && (!current.isValid() || m_model->nextEntryRow(current.row(), -1) < 0)) {
            emit focusSearchRequested();
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit activateRequested(current);
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
    QListView::keyPressEvent(event);
}

} // namespace clipitto::ui
