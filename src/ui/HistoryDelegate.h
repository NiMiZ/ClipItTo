#pragma once

#include <QStyledItemDelegate>

namespace clipitto::ui {

// `font` scaled by `factor`, whether it is specified in points or pixels.
QFont scaledFont(QFont font, qreal factor);

// Paints entries as a bold title (note) over a dim preview, or a single line of content,
// with a ★ for favorites, a group chip and the last-used age; headers as small captions.
class HistoryDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

} // namespace clipitto::ui
