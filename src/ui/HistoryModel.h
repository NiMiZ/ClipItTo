#pragma once

#include "Config.h"
#include "Entry.h"

#include <QAbstractItemModel>

#include <memory>
#include <vector>

namespace clipitto::ui {

// One-line preview of clipboard content: whitespace (including newlines) collapsed, capped.
QString previewText(const QString& content, int maxChars = 300);

// The first `count` non-blank lines, each squeezed and capped; a trailing "…" on the last
// one means there is more.
QStringList previewLines(const QString& content, int count, int maxChars = 300);

// Compact age: "now", "5m", "3h", "2d", then a short date.
QString relativeTime(const QDateTime& then, const QDateTime& now);

// What the popup lists, as a tree:
//   Groups                (when there are groups or favorites; collapsed by default)
//     ★ Favorites         (always first: starred copies that are in no group)
//       <copy>
//     Work                (group: its sub-groups first, then its copies, starred first)
//       Servers
//         <copy>
//       <copy>
//   PINNED                (section header, inert)
//     <copy> ...          (sections are flat: their copies are top-level rows)
//   RECENT
//   <copy> ...
class HistoryModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role {
        TitleRole = Qt::UserRole + 1, // note if set, else the (first line of the) content
        PreviewRole, // further lines, '\n'-separated; empty if there are none
        FavoriteRole,
        GroupRole,
        IdRole, // 0 for non-copies
        IsHeaderRole,
        LastUsedRole,
        KindRole, // int(Kind)
        PathRole, // group path (groups, and copies inside the group tree)
        CountRole, // copies in a group and its sub-groups
        HasNoteRole, // the title is the user's note (drawn bold), not the content
        PinnedRole,
        ThumbnailRole, // QPixmap for a picture copy, null otherwise
        SecretRole, // a password copy: shown hidden (●●●●) unless revealed
    };

    // What a hidden password shows instead of its text.
    static QString hiddenPassword() { return QStringLiteral("●●●●●●●●"); }

    enum class Kind { Header, Entry, GroupsRoot, Group, Favorites };

    // The Favorites node's path: never a real group path (those have no leading '/').
    static QString favoritesPath() { return QStringLiteral("/favorites"); }

    struct Section {
        QString header; // empty: no header row
        std::vector<Entry> entries;
    };

    struct GroupData {
        QString path;
        int clipCount = 0; // direct copies (informational; recomputed from the entries)
    };

    explicit HistoryModel(QObject* parent = nullptr);
    ~HistoryModel() override;

    // 1 squeezes the whole text onto one line; more shows that many lines per copy.
    void setPreviewLines(int lines);
    int previewLineCount() const { return m_previewLines; }

    // Replaces everything. With no groups and no favorites there is no Groups row.
    void setContent(std::vector<Section> sections, const std::vector<GroupData>& groups = {},
                    const std::vector<Entry>& groupedEntries = {}, const std::vector<Entry>& favorites = {});

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    Kind kindAt(const QModelIndex& index) const;
    const Entry* entryAt(const QModelIndex& index) const; // nullptr for anything but a copy
    QString pathAt(const QModelIndex& index) const;
    bool isExpandable(const QModelIndex& index) const; // a group (Favorites, the Groups row) with children

    QModelIndex groupsRoot() const; // invalid when there are no groups
    QModelIndex findGroup(const QString& path) const;
    QModelIndex firstEntry() const; // first copy in the sections
    // The first copy of that kind in the sections (pinned / in a group / unsaved), else the
    // first copy: where the selection starts (start_focus).
    QModelIndex firstEntry(StartFocus focus) const;
    // Shows a password copy's text in place of ●●●● (wherever it is listed); an empty text
    // hides it again. clearRevealed() hides every one and wipes the texts.
    void setRevealed(qint64 id, const QString& text);
    void clearRevealed();
    bool hasRevealed() const { return m_revealedCount > 0; }

    // A copy by id: in the sections first, else in the group tree (inside `path` if given).
    QModelIndex findEntry(qint64 id, const QString& groupPath = {}) const;

private:
    struct Node;
    Node* nodeFor(const QModelIndex& index) const;
    QModelIndex indexFor(const Node* node) const;
    std::unique_ptr<Node> makeEntryNode(const Entry& entry, Node* parent) const;

    template <typename F>
    void forEachEntryNode(Node* node, const F& visit);

    std::unique_ptr<Node> m_root;
    int m_previewLines = 1;
    int m_revealedCount = 0;
};

} // namespace clipitto::ui
