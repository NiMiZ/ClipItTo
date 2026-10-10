#include "SearchBar.h"

#include <QKeyEvent>

namespace clipitto::ui {

SearchBar::SearchBar(QWidget* parent) : QLineEdit(parent)
{
    setPlaceholderText(tr("Search clipboard history…"));
    setClearButtonEnabled(true);
}

void SearchBar::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Down:
    case Qt::Key_PageDown:
        emit focusListRequested();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit activateRequested();
        return;
    case Qt::Key_Right:
        if (text().isEmpty()) {
            emit openGroupsRequested();
            return;
        }
        break;
    default:
        QLineEdit::keyPressEvent(event);
    }
}

} // namespace clipitto::ui
