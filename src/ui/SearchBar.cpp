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
        emit stepRequested(1);
        return;
    case Qt::Key_Up:
        emit stepRequested(-1);
        return;
    case Qt::Key_PageDown:
        emit focusListRequested();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit activateRequested();
        return;
    case Qt::Key_Delete:
        // With nothing to delete after the cursor (an empty box, or at its end), Delete acts
        // on the copy selected below; elsewhere it edits the text as usual.
        if (event->modifiers() == Qt::NoModifier && cursorPosition() == text().size() && !hasSelectedText()) {
            emit deleteCurrentRequested();
        } else {
            QLineEdit::keyPressEvent(event);
        }
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
