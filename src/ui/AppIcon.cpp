#include "AppIcon.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace clipitto::ui {

namespace {

QPixmap paint(int size, bool dark)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 32.0, size / 32.0); // draw on a 32x32 grid

    // Board
    p.setPen(Qt::NoPen);
    p.setBrush(dark ? QColor(0x6a, 0xa2, 0xf0) : QColor(0x3d, 0x7e, 0xd6));
    p.drawRoundedRect(QRectF(5, 4, 22, 26), 4, 4);
    // Paper
    p.setBrush(dark ? QColor(0x23, 0x26, 0x2b) : QColor(0xf7, 0xf9, 0xfc));
    p.drawRoundedRect(QRectF(8, 8, 16, 19), 2, 2);
    // Clip
    p.setBrush(dark ? QColor(0xe4, 0xe8, 0xee) : QColor(0x2b, 0x2f, 0x36));
    p.drawRoundedRect(QRectF(11, 2, 10, 6), 2, 2);
    // Lines of text
    p.setBrush(dark ? QColor(0x8f, 0x9b, 0xab) : QColor(0x9a, 0xa5, 0xb5));
    for (int i = 0; i < 3; ++i) {
        p.drawRoundedRect(QRectF(11, 12 + i * 4.5, i == 2 ? 7 : 10, 2), 1, 1);
    }
    return pixmap;
}

} // namespace

QIcon appIcon(bool dark)
{
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48, 64, 128, 256}) {
        icon.addPixmap(paint(size, dark));
    }
    return icon;
}

} // namespace clipitto::ui
