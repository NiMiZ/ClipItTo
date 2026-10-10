#include "HistoryModel.h"

#include "GroupPath.h"

#include <QHash>
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

QStringList previewLines(const QString& content, int count, int maxChars)
{
    QStringList lines;
    bool more = false;
    // Bounded prefix again: enough for `count` long lines plus one to detect "more".
    const QString prefix = content.left((count + 1) * maxChars * 2);
    for (const QString& raw : prefix.split(QLatin1Char('\n'))) {
        const QString line = raw.simplified();
        if (line.isEmpty()) {
            continue;
        }
        if (lines.size() == count) {
            more = true;
            break;
        }
        lines << (line.size() > maxChars ? line.left(maxChars) + QChar(0x2026) : line);
    }
    if ((more || prefix.size() < content.size()) && !lines.isEmpty() && !lines.last().endsWith(QChar(0x2026))) {
        lines.last() += QChar(0x2026);
    }
    return lines;
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

struct HistoryModel::Node {
    Kind kind = Kind::Header;
    QString label; // header text / group leaf name
    QString path; // group path: groups, and copies inside the group tree
    Entry entry;
    QString title;
    QString preview;
    int count = 0;
    Node* parent = nullptr;
    int row = 0;
    std::vector<std::unique_ptr<Node>> children;

    Node* add(std::unique_ptr<Node> child)
    {
        child->parent = this;
        child->row = int(children.size());
        children.push_back(std::move(child));
        return children.back().get();
    }
};

HistoryModel::HistoryModel(QObject* parent) : QAbstractItemModel(parent), m_root(std::make_unique<Node>()) {}

HistoryModel::~HistoryModel() = default;

void HistoryModel::setPreviewLines(int lines)
{
    m_previewLines = qMax(1, lines);
}

std::unique_ptr<HistoryModel::Node> HistoryModel::makeEntryNode(const Entry& entry, Node* parent) const
{
    auto node = std::make_unique<Node>();
    node->kind = Kind::Entry;
    node->entry = entry;
    node->path = parent && parent->kind == Kind::Group ? parent->path : QString();
    // Precomputed once per refresh instead of on every paint.
    if (m_previewLines == 1) {
        node->title = entry.note.isEmpty() ? previewText(entry.content) : entry.note;
        node->preview = entry.note.isEmpty() ? QString() : previewText(entry.content);
    } else {
        QStringList lines = previewLines(entry.content, m_previewLines);
        if (entry.note.isEmpty()) {
            node->title = lines.isEmpty() ? QString() : lines.takeFirst();
        } else {
            node->title = entry.note;
        }
        node->preview = lines.join(QLatin1Char('\n'));
    }
    return node;
}

void HistoryModel::setContent(std::vector<Section> sections, const std::vector<GroupData>& groupList,
                              const std::vector<Entry>& groupedEntries)
{
    beginResetModel();
    m_root = std::make_unique<Node>();

    if (!groupList.empty()) {
        auto rootNode = std::make_unique<Node>();
        rootNode->kind = Kind::GroupsRoot;
        rootNode->label = tr("Groups");
        Node* root = m_root->add(std::move(rootNode));

        // Parents come before children (sorted paths), so each parent node already exists.
        QHash<QString, Node*> byPath;
        for (const GroupData& g : groupList) {
            const QString parentPath = groups::parent(g.path);
            Node* parent = parentPath.isEmpty() ? root : byPath.value(parentPath, root);
            auto node = std::make_unique<Node>();
            node->kind = Kind::Group;
            node->label = groups::leaf(g.path);
            node->path = g.path;
            byPath.insert(g.path, parent->add(std::move(node)));
        }
        // Copies after the sub-groups; counts roll up to every ancestor.
        for (const Entry& e : groupedEntries) {
            Node* group = byPath.value(e.group);
            if (!group) {
                continue;
            }
            group->add(makeEntryNode(e, group));
            for (Node* n = group; n; n = n->parent) {
                ++n->count;
            }
        }
    }

    for (Section& section : sections) {
        if (!section.header.isEmpty()) {
            auto header = std::make_unique<Node>();
            header->kind = Kind::Header;
            header->label = section.header;
            m_root->add(std::move(header));
        }
        for (const Entry& e : section.entries) {
            m_root->add(makeEntryNode(e, m_root.get()));
        }
    }
    endResetModel();
}

HistoryModel::Node* HistoryModel::nodeFor(const QModelIndex& index) const
{
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : m_root.get();
}

QModelIndex HistoryModel::indexFor(const Node* node) const
{
    return node && node != m_root.get() ? createIndex(node->row, 0, const_cast<Node*>(node)) : QModelIndex();
}

QModelIndex HistoryModel::index(int row, int column, const QModelIndex& parent) const
{
    const Node* p = nodeFor(parent);
    if (column != 0 || row < 0 || row >= int(p->children.size())) {
        return {};
    }
    return createIndex(row, 0, p->children[std::size_t(row)].get());
}

QModelIndex HistoryModel::parent(const QModelIndex& child) const
{
    return child.isValid() ? indexFor(nodeFor(child)->parent) : QModelIndex();
}

int HistoryModel::rowCount(const QModelIndex& parent) const
{
    return parent.column() > 0 ? 0 : int(nodeFor(parent)->children.size());
}

int HistoryModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant HistoryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const Node* n = nodeFor(index);
    switch (role) {
    case KindRole:
        return int(n->kind);
    case IsHeaderRole:
        return n->kind == Kind::Header;
    case PathRole:
        return n->path;
    case CountRole:
        return n->count;
    case IdRole:
        return n->kind == Kind::Entry ? n->entry.id : qint64(0);
    default:
        break;
    }
    if (n->kind != Kind::Entry) {
        return role == Qt::DisplayRole || role == TitleRole ? QVariant(n->label) : QVariant();
    }
    const Entry& e = n->entry;
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return n->title;
    case PreviewRole:
        return n->preview;
    case Qt::ToolTipRole:
        return e.content.left(2000);
    case FavoriteRole:
        return e.favorite;
    case HasNoteRole:
        return !e.note.isEmpty();
    case GroupRole:
        return e.group;
    case LastUsedRole:
        return e.lastUsed;
    default:
        return {};
    }
}

Qt::ItemFlags HistoryModel::flags(const QModelIndex& index) const
{
    if (!index.isValid() || nodeFor(index)->kind == Kind::Header) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

HistoryModel::Kind HistoryModel::kindAt(const QModelIndex& index) const
{
    return index.isValid() ? nodeFor(index)->kind : Kind::Header;
}

const Entry* HistoryModel::entryAt(const QModelIndex& index) const
{
    const Node* n = index.isValid() ? nodeFor(index) : nullptr;
    return n && n->kind == Kind::Entry ? &n->entry : nullptr;
}

QString HistoryModel::pathAt(const QModelIndex& index) const
{
    return index.isValid() ? nodeFor(index)->path : QString();
}

bool HistoryModel::isExpandable(const QModelIndex& index) const
{
    const Kind kind = kindAt(index);
    return (kind == Kind::GroupsRoot || kind == Kind::Group) && rowCount(index) > 0;
}

QModelIndex HistoryModel::groupsRoot() const
{
    for (const auto& child : m_root->children) {
        if (child->kind == Kind::GroupsRoot) {
            return indexFor(child.get());
        }
    }
    return {};
}

QModelIndex HistoryModel::findGroup(const QString& path) const
{
    const QModelIndex root = groupsRoot();
    if (path.isEmpty() || !root.isValid()) {
        return root;
    }
    // Walk down level by level.
    const Node* node = nodeFor(root);
    for (const QString& level : path.split(groups::kSeparator)) {
        const Node* next = nullptr;
        for (const auto& child : node->children) {
            if (child->kind == Kind::Group && child->label == level) {
                next = child.get();
                break;
            }
        }
        if (!next) {
            return {};
        }
        node = next;
    }
    return indexFor(node);
}

QModelIndex HistoryModel::firstEntry() const
{
    for (const auto& child : m_root->children) {
        if (child->kind == Kind::Entry) {
            return indexFor(child.get());
        }
    }
    return {};
}

QModelIndex HistoryModel::findEntry(qint64 id, const QString& groupPath) const
{
    if (groupPath.isEmpty()) {
        for (const auto& child : m_root->children) {
            if (child->kind == Kind::Entry && child->entry.id == id) {
                return indexFor(child.get());
            }
        }
    }
    // Depth-first through the group tree.
    std::vector<const Node*> stack;
    const QModelIndex root = groupsRoot();
    if (root.isValid()) {
        stack.push_back(nodeFor(root));
    }
    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        for (const auto& child : node->children) {
            if (child->kind == Kind::Entry && child->entry.id == id
                && (groupPath.isEmpty() || child->path == groupPath)) {
                return indexFor(child.get());
            }
            if (child->kind == Kind::Group) {
                stack.push_back(child.get());
            }
        }
    }
    return {};
}

} // namespace clipitto::ui
