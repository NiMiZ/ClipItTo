#include "HistoryDelegate.h"

#include "HistoryModel.h"

#include <QApplication>
#include <QPainter>

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

} // namespace

QSize HistoryDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const QFontMetrics fm(option.font);
    if (index.data(HistoryModel::IsHeaderRole).toBool()) {
        return {0, QFontMetrics(caption(option.font)).height() + 2 * kVPad};
    }
    const bool twoLines = !index.data(HistoryModel::PreviewRole).toString().isEmpty();
    return {0, (twoLines ? 2 : 1) * fm.height() + (twoLines ? 2 : 0) + 2 * kVPad + 2};
}

void HistoryDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    painter->save();
    const QRect r = option.rect;
    const QPalette& pal = option.palette;

    if (index.data(HistoryModel::IsHeaderRole).toBool()) {
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

    QRect content = r.adjusted(kHPad, kVPad, -kHPad, -kVPad);
    const QFontMetrics fm(option.font);

    // Leading star column, kept even when empty so titles line up.
    if (index.data(HistoryModel::FavoriteRole).toBool()) {
        painter->setPen(selected ? text : pal.color(QPalette::Link));
        painter->drawText(QRect(content.left(), content.top(), kStarWidth, fm.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("★"));
    }
    content.setLeft(content.left() + kStarWidth);

    // Trailing age and group chip on the first line.
    const QString age = relativeTime(index.data(HistoryModel::LastUsedRole).toDateTime(),
                                     QDateTime::currentDateTimeUtc());
    int right = content.right();
    if (!age.isEmpty()) {
        const int w = fm.horizontalAdvance(age);
        painter->setPen(dim);
        painter->drawText(QRect(right - w, content.top(), w, fm.height()), Qt::AlignRight | Qt::AlignVCenter, age);
        right -= w + kGap;
    }
    const QString group = index.data(HistoryModel::GroupRole).toString();
    if (!group.isEmpty()) {
        const QFont chipFont = scaledFont(option.font, 0.85);
        const QFontMetrics cfm(chipFont);
        const QString label = cfm.elidedText(group, Qt::ElideRight, 120);
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
    const QRect titleRect(content.left(), content.top(), right - content.left(), fm.height());
    if (preview.isEmpty()) {
        painter->setPen(text);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(title, Qt::ElideRight, titleRect.width()));
    } else {
        const QFont titleFont = bold(option.font);
        const QFontMetrics tfm(titleFont);
        painter->setFont(titleFont);
        painter->setPen(text);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          tfm.elidedText(title, Qt::ElideRight, titleRect.width()));
        painter->setFont(option.font);
        painter->setPen(dim);
        const QRect previewRect(content.left(), content.top() + fm.height() + 2, content.width(), fm.height());
        painter->drawText(previewRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(preview, Qt::ElideRight, previewRect.width()));
    }
    painter->restore();
}

} // namespace clipitto::ui
