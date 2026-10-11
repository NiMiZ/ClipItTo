#include "HistoryDelegate.h"

#include "HistoryModel.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace clipitto::ui {

QFont scaledFont(QFont font, qreal factor)
{
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() * factor);
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(qMax(1, qRound(font.pixelSize() * factor)));
    }
    return font;
}

namespace {

constexpr int kHPad = 10;
constexpr int kVPad = 6;
constexpr int kStarWidth = 18;
constexpr int kGap = 8;
constexpr int kArrowBox = 16;

using Kind = HistoryModel::Kind;

QFont bold(QFont font)
{
    font.setBold(true);
    return font;
}

QFont caption(QFont font)
{
    font = scaledFont(font, 0.85);
    font.setCapitalization(QFont::AllUppercase);
    font.setBold(true);
    return font;
}

int previewLineCount(const QModelIndex& index)
{
    const QString preview = index.data(HistoryModel::PreviewRole).toString();
    return preview.isEmpty() ? 0 : int(preview.count(QLatin1Char('\n'))) + 1;
}

// A chevron pointing right (collapsed) or down (expanded), centered in `box`.
void drawChevron(QPainter* painter, const QRectF& box, bool expanded, const QColor& color)
{
    const QPointF c = box.center();
    const qreal s = 3.5;
    QPainterPath path;
    if (expanded) {
        path.moveTo(c.x() - s, c.y() - s / 2);
        path.lineTo(c.x(), c.y() + s / 2);
        path.lineTo(c.x() + s, c.y() - s / 2);
    } else {
        path.moveTo(c.x() - s / 2, c.y() - s);
        path.lineTo(c.x() + s / 2, c.y());
        path.lineTo(c.x() - s / 2, c.y() + s);
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    painter->restore();
}

} // namespace

QSize HistoryDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const QFontMetrics fm(option.font);
    switch (Kind(index.data(HistoryModel::KindRole).toInt())) {
    case Kind::Header:
        return {0, QFontMetrics(caption(option.font)).height() + 2 * kVPad};
    case Kind::GroupsRoot:
    case Kind::Group:
    case Kind::Favorites:
        return {0, fm.height() + 2 * kVPad + 2};
    case Kind::Entry:
        break;
    }
    const int extra = previewLineCount(index);
    return {0, (1 + extra) * fm.height() + extra * 2 + 2 * kVPad + 2};
}

void HistoryDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    painter->save();
    const QRect r = option.rect;
    const QPalette& pal = option.palette;
    const Kind kind = Kind(index.data(HistoryModel::KindRole).toInt());

    if (kind == Kind::Header) {
        painter->setFont(caption(option.font));
        painter->setPen(pal.color(QPalette::Disabled, QPalette::Text));
        painter->drawText(r.adjusted(kHPad, 0, -kHPad, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(HistoryModel::TitleRole).toString());
        painter->restore();
        return;
    }

    // Background: the platform style handles selection and hover consistently.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const QWidget* widget = option.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

    const bool selected = option.state & QStyle::State_Selected;
    const QColor text = pal.color(selected ? QPalette::HighlightedText : QPalette::Text);
    QColor dim = text;
    dim.setAlphaF(0.6);
    const QFontMetrics fm(option.font);
    QRect content = r.adjusted(kHPad, kVPad, -kHPad, -kVPad);

    if (kind == Kind::GroupsRoot || kind == Kind::Group || kind == Kind::Favorites) {
        // Name, copy count, and the expand arrow at the end of the row.
        const bool expandable = index.model()->rowCount(index) > 0;
        const QRect arrow(content.right() - kArrowBox + 1, content.top(), kArrowBox, fm.height());
        if (expandable) {
            drawChevron(painter, arrow, option.state & QStyle::State_Open, kind == Kind::GroupsRoot ? text : dim);
        }
        int right = arrow.left() - kGap;
        const int count = index.data(HistoryModel::CountRole).toInt();
        const QString countText = count > 0 ? QString::number(count) : QString();
        if (!countText.isEmpty()) {
            const int w = fm.horizontalAdvance(countText);
            painter->setPen(dim);
            painter->drawText(QRect(right - w, content.top(), w, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                              countText);
            right -= w + kGap;
        }
        int left = content.left();
        if (kind == Kind::Favorites) {
            const QString star = QStringLiteral("★");
            painter->setPen(selected ? text : pal.color(QPalette::Link));
            painter->drawText(QRect(left, content.top(), kStarWidth, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                              star);
            left += kStarWidth;
        }
        const QFont font = kind == Kind::GroupsRoot ? caption(option.font) : bold(option.font);
        painter->setFont(font);
        painter->setPen(kind == Kind::GroupsRoot && !selected ? pal.color(QPalette::Disabled, QPalette::Text) : text);
        const QRect label(left, content.top(), right - left, fm.height());
        painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(index.data(HistoryModel::TitleRole).toString(),
                                                        Qt::ElideRight, label.width()));
        painter->restore();
        return;
    }

    // A copy. Top-level rows keep a leading star column (empty or not) so titles line up.
    // Inside the group tree there is no such column: a copy's text starts where its sibling
    // sub-groups' names start, so the nesting reads right, and the star goes to the end.
    const bool inTree = index.parent().isValid();
    const bool favorite = index.data(HistoryModel::FavoriteRole).toBool();
    const QColor starColor = selected ? text : pal.color(QPalette::Link);
    if (!inTree) {
        if (favorite) {
            painter->setPen(starColor);
            painter->drawText(QRect(content.left(), content.top(), kStarWidth, fm.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("★"));
        }
        content.setLeft(content.left() + kStarWidth);
    }

    // Trailing age (and, in the tree, the star) on the first line.
    const QString age = relativeTime(index.data(HistoryModel::LastUsedRole).toDateTime(),
                                     QDateTime::currentDateTimeUtc());
    int right = content.right();
    if (!age.isEmpty()) {
        const int w = fm.horizontalAdvance(age);
        painter->setPen(dim);
        painter->drawText(QRect(right - w, content.top(), w, fm.height()), Qt::AlignRight | Qt::AlignVCenter, age);
        right -= w + kGap;
    }
    if (inTree && favorite) {
        const int w = fm.horizontalAdvance(QStringLiteral("★"));
        painter->setPen(starColor);
        painter->drawText(QRect(right - w, content.top(), w, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                          QStringLiteral("★"));
        right -= w + kGap;
    }
    // Inside the group tree the group is already obvious from the position.
    const QString group = inTree ? QString() : index.data(HistoryModel::GroupRole).toString();
    if (!group.isEmpty()) {
        const QFont chipFont = scaledFont(option.font, 0.85);
        const QFontMetrics cfm(chipFont);
        const QString label = cfm.elidedText(QString(group).replace(QLatin1Char('/'), QStringLiteral(" › ")),
                                             Qt::ElideLeft, 140);
        const int w = cfm.horizontalAdvance(label) + 12;
        const QRect chip(right - w, content.top() + (fm.height() - cfm.height() - 2) / 2, w, cfm.height() + 2);
        QColor chipBg = selected ? text : pal.color(QPalette::Highlight);
        chipBg.setAlphaF(0.18);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(chipBg);
        painter->drawRoundedRect(chip, chip.height() / 2.0, chip.height() / 2.0);
        painter->setFont(chipFont);
        painter->setPen(text);
        painter->drawText(chip, Qt::AlignCenter, label);
        painter->setFont(option.font);
        right -= w + kGap;
    }

    const QString title = index.data(HistoryModel::TitleRole).toString();
    const QString preview = index.data(HistoryModel::PreviewRole).toString();
    const bool hasNote = index.data(HistoryModel::HasNoteRole).toBool();
    const QRect titleRect(content.left(), content.top(), right - content.left(), fm.height());
    const QFont titleFont = hasNote ? bold(option.font) : option.font;
    painter->setFont(titleFont);
    painter->setPen(text);
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, titleRect.width()));
    painter->setFont(option.font);
    painter->setPen(dim);
    int y = content.top() + fm.height() + 2;
    for (const QString& line : preview.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QRect lineRect(content.left(), y, content.width(), fm.height());
        painter->drawText(lineRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(line, Qt::ElideRight, lineRect.width()));
        y += fm.height() + 2;
    }
    painter->restore();
}

} // namespace clipitto::ui
