#pragma once

#include <QByteArray>
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

    // An image copy: `content` is its label ("Image 1920×1080", what search matches), the
    // picture itself stays in the database (Repository::imageData) except a small thumbnail.
    bool image = false;
    int imageWidth = 0;
    int imageHeight = 0;
    QByteArray thumbnail; // PNG, at most kThumbnailSize on each side
    QString contentHash; // content_hash: for images "image:<sha256 of the pixels>"

    bool pinned() const { return pinPosition >= 0; }
    // Saved items are immune to pruning and rank first in search.
    bool isSaved() const { return favorite || !group.isEmpty() || pinned(); }

    // The note is shown as the title when set; otherwise the clipboard text itself.
    QString displayTitle() const { return note.isEmpty() ? content : note; }
};

} // namespace clipitto
