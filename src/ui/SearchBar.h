#pragma once

#include <QLineEdit>

namespace clipitto::ui {

class SearchBar : public QLineEdit {
    Q_OBJECT

public:
    explicit SearchBar(QWidget* parent = nullptr);

    // Typing the keyword and a space (e.g. "p[] ") switches to a password search: what follows
    // is kept here, never in the line edit, which shows "p[] ****" instead.
    void setPasswordKeyword(const QString& keyword);
    QString passwordKeyword() const { return m_keyword; }
    bool isPasswordMode() const { return m_passwordMode; }
    QString passwordQuery() const { return m_secret; }
    void leavePasswordMode(); // wipes what was typed; the box keeps only the keyword

signals:
    void focusListRequested(); // Page Down
    // Up (-1) / Down (+1): move the list's cursor one row from where it is, into the list.
    void stepRequested(int step);
    void activateRequested(); // Enter: activate the current list row
    void pasteAsRequested(); // Shift+Enter: choose a transform for the current row
    void pasteOnceRequested(); // Ctrl+Shift+Enter: paste the current row and forget it
    void openGroupsRequested(); // Right arrow in an empty search box
    // Delete with nothing after the text cursor: delete the copy selected in the list.
    void deleteCurrentRequested();
    void renameRequested(); // F2: rename the group, or edit the note of the copy, selected in the list
    void editGroupRequested(); // Ctrl+G: the group of the selected copy (on a group: rename it)
    void toggleFavoriteRequested(); // Ctrl+S: star / unstar the selected copy
    void togglePinRequested(); // Ctrl+P: pin / unpin it
    void editNoteRequested(); // Ctrl+E: add or edit the note of the copy selected in the list
    // A password search's text changed; also when entering it (empty) and leaving (`active`
    // false). Normal searches go through textChanged as before, which the owner ignores while
    // isPasswordMode().
    void passwordQueryChanged(const QString& query, bool active);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QString prefix() const { return m_keyword + QLatin1Char(' '); }
    void onTextChanged(const QString& text);
    void showMasked(); // prefix + one '*' per character, cursor at the end
    void appendSecret(const QString& typed);

    QString m_keyword = QStringLiteral("p[]");
    QString m_secret;
    bool m_passwordMode = false;
    bool m_updating = false; // our own setText while masking
};

} // namespace clipitto::ui
