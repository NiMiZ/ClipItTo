#include "HistoryModel.h"

#include <QLocale>

namespace clipitto::ui {

QString previewText(const QString& content, int maxChars)
{
    // Look at a bounded prefix so huge clips don't cost a full scan per repaint.
    QString text = content.left(maxChars * 2).simplified();
    if (text.size() > maxChars) {
        text.truncate(maxChars);
        text += QChar(0x2026); // …
    }
    return text;
}

QString relativeTime(const QDateTime& then, const QDateTime& now)
{
    if (!then.isValid()) {
        return {};
    }
    const qint64 secs = then.secsTo(now);
    if (secs < 60) {
        return QStringLiteral("now");
    }
    if (secs < 3600) {
        return QStringLiteral("%1m").arg(secs / 60);
    }
    if (secs < 86400) {
        return QStringLiteral("%1h").arg(secs / 3600);
    }
    if (secs < 7 * 86400) {
        return QStringLiteral("%1d").arg(secs / 86400);
    }
    return QLocale().toString(then.toLocalTime().date(), QLocale::ShortFormat);
}

void HistoryModel::setRows(std::vector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

int HistoryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant HistoryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size())) {
        return {};
    }
    const Row& row = m_rows[std::size_t(index.row())];
    if (row.isHeader()) {
        switch (role) {
        case Qt::DisplayRole:
        case TitleRole:
            return row.header;
        case IsHeaderRole:
            return true;
        case IdRole:
            return qint64(0);
        default:
            return {};
        }
    }

    const Entry& e = row.entry;
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return e.note.isEmpty() ? previewText(e.content) : e.note;
    case Qt::ToolTipRole:
        return e.content.left(2000);
    case PreviewRole:
        return e.note.isEmpty() ? QString() : previewText(e.content);
    case FavoriteRole:
        return e.favorite;
    case GroupRole:
        return e.group;
    case IdRole:
        return e.id;
    case IsHeaderRole:
        return false;
    case LastUsedRole:
        return e.lastUsed;
    default:
        return {};
    }
}

Qt::ItemFlags HistoryModel::flags(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()) || m_rows[std::size_t(index.row())].isHeader()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

const Entry* HistoryModel::entryAt(int row) const
{
    if (row < 0 || row >= int(m_rows.size()) || m_rows[std::size_t(row)].isHeader()) {
        return nullptr;
    }
    return &m_rows[std::size_t(row)].entry;
}

int HistoryModel::rowForId(qint64 id) const
{
    for (int i = 0; i < int(m_rows.size()); ++i) {
        if (!m_rows[std::size_t(i)].isHeader() && m_rows[std::size_t(i)].entry.id == id) {
            return i;
        }
    }
    return -1;
}

int HistoryModel::firstEntryRow() const
{
    return nextEntryRow(-1, +1);
}

int HistoryModel::nextEntryRow(int row, int direction) const
{
    for (int i = row + direction; i >= 0 && i < int(m_rows.size()); i += direction) {
        if (!m_rows[std::size_t(i)].isHeader()) {
            return i;
        }
    }
    return -1;
}

} // namespace clipitto::ui
