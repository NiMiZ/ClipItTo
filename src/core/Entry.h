#pragma once

#include <QDateTime>
#include <QString>
#include <QtGlobal>

namespace clipitto {

struct Entry {
    qint64 id = 0;
    QString content;
    QString note; // tag_or_note; empty when unset
    QString group; // group_name; empty when unset
    bool favorite = false; // starred: in its group (listed first), or in Favorites when ungrouped
    int pinPosition = -1; // order in the Pinned list; -1 when not pinned
    QDateTime createdAt;
    QDateTime lastUsed;

    bool pinned() const { return pinPosition >= 0; }
    // Saved items are immune to pruning and rank first in search.
    bool isSaved() const { return favorite || !group.isEmpty() || pinned(); }

    // The note is shown as the title when set; otherwise the clipboard text itself.
    QString displayTitle() const { return note.isEmpty() ? content : note; }
};

} // namespace clipitto
