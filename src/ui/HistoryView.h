#pragma once

#include <QTreeView>

class QKeyEvent;

namespace clipitto::ui {

class HistoryModel;

// The popup's list: copies in sections plus the expandable group tree. The delegate draws
// the expand arrows (at the end of the row), so the tree's own branch decoration is off.
class HistoryView : public QTreeView {
    Q_OBJECT

public:
    explicit HistoryView(QWidget* parent = nullptr);

    void setHistoryModel(HistoryModel* model);
    void selectIndex(const QModelIndex& index); // current + selected, scrolled into view
    void selectFirstEntry(); // the first copy (what Enter in the search box copies)
    void selectFirstRow(); // the first selectable row (the Groups row when there are groups)
    QModelIndex firstSelectable() const;
    QModelIndex nextSelectable(QModelIndex from, bool down) const; // invalid at either end
    void toggleExpanded(const QModelIndex& index);
    void setEmptyText(const QString& text); // shown when the model has no rows

signals:
    void focusSearchRequested(); // Up on the first row
    void typedText(QKeyEvent* event); // printable key or Backspace: belongs to the search bar
    void activateRequested(const QModelIndex& index); // Enter on a copy
    void toggleFavoriteRequested(const QModelIndex& index);
    void togglePinRequested(const QModelIndex& index);
    void moveRequested(const QModelIndex& index, int delta); // Alt+Up (-1) / Alt+Down (+1)
    void editGroupRequested(const QModelIndex& index);
    void editNoteRequested(const QModelIndex& index);
    void deleteRequested(const QModelIndex& index);
    void contextMenuRequested(const QModelIndex& index, const QPoint& globalPos);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
    void paintEvent(QPaintEvent* event) override;
    void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}

private:
    QModelIndex lastSelectable() const;

    HistoryModel* m_model = nullptr;
    QString m_emptyText;
};

} // namespace clipitto::ui
