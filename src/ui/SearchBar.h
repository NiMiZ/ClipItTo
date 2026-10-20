#pragma once

#include <QLineEdit>

namespace clipitto::ui {

class SearchBar : public QLineEdit {
    Q_OBJECT

public:
    explicit SearchBar(QWidget* parent = nullptr);

signals:
    void focusListRequested(); // Page Down
    // Up (-1) / Down (+1): move the list's cursor one row from where it is, into the list.
    void stepRequested(int step);
    void activateRequested(); // Enter: activate the current list row
    void openGroupsRequested(); // Right arrow in an empty search box
    // Delete with nothing after the text cursor: delete the copy selected in the list.
    void deleteCurrentRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
};

} // namespace clipitto::ui
