#pragma once

#include <QLineEdit>

namespace clipitto::ui {

class SearchBar : public QLineEdit {
    Q_OBJECT

public:
    explicit SearchBar(QWidget* parent = nullptr);

signals:
    void focusListRequested(); // Down arrow
    void activateRequested(); // Enter: activate the current list row
    void openGroupsRequested(); // Right arrow in an empty search box

protected:
    void keyPressEvent(QKeyEvent* event) override;
};

} // namespace clipitto::ui
