#pragma once

#include <QListView>

class QKeyEvent;

namespace clipitto::ui {

class HistoryModel;

class HistoryView : public QListView {
    Q_OBJECT

public:
    explicit HistoryView(QWidget* parent = nullptr);

    void setHistoryModel(HistoryModel* model);
    void selectRow(int row); // makes `row` current and selected, scrolling to it
    void selectFirstEntry();
    void setEmptyText(const QString& text); // shown when the model has no rows

signals:
    void focusSearchRequested(); // Up on the first entry
    void typedText(QKeyEvent* event); // printable key or Backspace: belongs to the search bar
    void activateRequested(const QModelIndex& index);
    void toggleFavoriteRequested(const QModelIndex& index);
    void editGroupRequested(const QModelIndex& index);
    void editNoteRequested(const QModelIndex& index);
    void deleteRequested(const QModelIndex& index);
    void contextMenuRequested(const QModelIndex& index, const QPoint& globalPos);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
    void paintEvent(QPaintEvent* event) override;

private:
    HistoryModel* m_model = nullptr;
    QString m_emptyText;
};

} // namespace clipitto::ui
