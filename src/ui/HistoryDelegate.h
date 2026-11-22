#pragma once

#include <QStyledItemDelegate>

namespace clipitto::ui {

class FileThumbnails;

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

    // Where the thumbnails of picture files in a file list come from (not owned); none: always the file icon.
    void setThumbnails(FileThumbnails* thumbnails) { m_thumbnails = thumbnails; }

private:
    FileThumbnails* m_thumbnails = nullptr;
};

} // namespace clipitto::ui
