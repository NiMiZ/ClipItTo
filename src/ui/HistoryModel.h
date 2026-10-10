#pragma once

#include "Entry.h"

#include <QAbstractListModel>

#include <vector>

namespace clipitto::ui {

// One-line preview of clipboard content: whitespace (including newlines) collapsed, capped.
QString previewText(const QString& content, int maxChars = 300);

// Compact age: "now", "5m", "3h", "2d", then a short date.
QString relativeTime(const QDateTime& then, const QDateTime& now);

// Rows shown in the popup: clipboard entries, optionally separated by section headers.
class HistoryModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        TitleRole = Qt::UserRole + 1, // note if set, else content preview
        PreviewRole, // content preview when a note is set, else empty
        FavoriteRole,
        GroupRole,
        IdRole, // 0 for headers
        IsHeaderRole,
        LastUsedRole,
    };

    struct Row {
        QString header; // non-empty: this row is a section header
        Entry entry;
        bool isHeader() const { return !header.isEmpty(); }
    };

    using QAbstractListModel::QAbstractListModel;

    void setRows(std::vector<Row> rows);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    const Entry* entryAt(int row) const; // nullptr for headers and out-of-range rows
    int rowForId(qint64 id) const; // -1 if absent
    int firstEntryRow() const; // -1 if there are no entries
    // Next entry row from `row` in direction +1/-1, skipping headers; -1 if none.
    int nextEntryRow(int row, int direction) const;

private:
    std::vector<Row> m_rows;
};

} // namespace clipitto::ui
