#include "SearchBar.h"

#include "PasswordVault.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>

namespace clipitto::ui {

SearchBar::SearchBar(QWidget* parent) : QLineEdit(parent)
{
    setPlaceholderText(tr("Search clipboard history…"));
    setClearButtonEnabled(true);
    connect(this, &QLineEdit::textChanged, this, &SearchBar::onTextChanged);
}

void SearchBar::setPasswordKeyword(const QString& keyword)
{
    if (keyword.isEmpty() || keyword == m_keyword) {
        return;
    }
    if (m_passwordMode) {
        leavePasswordMode();
    }
    m_keyword = keyword;
}

void SearchBar::leavePasswordMode()
{
    if (!m_passwordMode) {
        return;
    }
    m_passwordMode = false;
    clipitto::PasswordVault::wipe(m_secret);
    setAcceptDrops(true);
    emit passwordQueryChanged(QString(), false);
    if (text().startsWith(prefix())) {
        m_updating = true;
        setText(m_keyword);
        m_updating = false;
    }
}

void SearchBar::onTextChanged(const QString& text)
{
    if (m_updating) {
        return;
    }
    if (m_passwordMode) {
        // Cleared or edited past the keyword (the clear button, select-all + type, ...).
        if (!text.startsWith(prefix())) {
            m_passwordMode = false;
            clipitto::PasswordVault::wipe(m_secret);
            setAcceptDrops(true);
            emit passwordQueryChanged(QString(), false);
        }
        return;
    }
    if (text.startsWith(prefix())) {
        // Typed, or pasted in one go ("p[] hunter2"): what follows goes out of the line edit.
        m_passwordMode = true;
        m_secret = text.mid(prefix().size());
        setAcceptDrops(false);
        showMasked();
        emit passwordQueryChanged(m_secret, true);
    }
}

void SearchBar::showMasked()
{
    m_updating = true;
    setText(prefix() + QString(m_secret.size(), QLatin1Char('*')));
    setCursorPosition(int(text().size()));
    m_updating = false;
}

void SearchBar::appendSecret(const QString& typed)
{
    QString clean = typed;
    clean.remove(QLatin1Char('\n')).remove(QLatin1Char('\r'));
    if (clean.isEmpty()) {
        return;
    }
    m_secret += clean;
    clipitto::PasswordVault::wipe(clean);
    showMasked();
    emit passwordQueryChanged(m_secret, true);
}

void SearchBar::inputMethodEvent(QInputMethodEvent* event)
{
    if (!m_passwordMode) {
        QLineEdit::inputMethodEvent(event);
        return;
    }
    appendSecret(event->commitString()); // never shown as a preedit string
    event->accept();
}

void SearchBar::contextMenuEvent(QContextMenuEvent* event)
{
    if (m_passwordMode) {
        event->accept(); // no copy/paste menu over a hidden password
        return;
    }
    QLineEdit::contextMenuEvent(event);
}

void SearchBar::keyPressEvent(QKeyEvent* event)
{
    if (m_passwordMode) {
        // Only typing, Backspace and paste edit the hidden text; the cursor stays at the end.
        if (event->matches(QKeySequence::Paste)) {
            appendSecret(QGuiApplication::clipboard()->text());
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            if (m_secret.isEmpty()) {
                leavePasswordMode(); // back to "p[]", a normal search
            } else {
                m_secret.chop(1);
                showMasked();
                emit passwordQueryChanged(m_secret, true);
            }
            return;
        }
        const QString typed = event->text();
        const bool control = event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
        if (!typed.isEmpty() && typed.at(0).isPrint() && !control) {
            appendSecret(typed);
            return;
        }
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_Delete:
            return; // no moving into, or editing inside, the hidden text
        default:
            break;
        }
        if (control && event->key() != Qt::Key_A) {
            return; // no cut/copy/undo of the masked text either
        }
    }
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
