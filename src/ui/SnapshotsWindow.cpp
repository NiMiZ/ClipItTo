#include "SnapshotsWindow.h"

#include "db/Sqlite.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLoggingCategory>
#include <QPushButton>
#include <QVBoxLayout>

Q_LOGGING_CATEGORY(lcSnapshots, "clipitto.snapshots")

namespace clipitto::ui {

using db::Repository;

namespace {
QString copies(int n)
{
    return n == 1 ? QObject::tr("1 copy") : QObject::tr("%1 copies").arg(n);
}
} // namespace

QString snapshotStatusText(Repository::SnapshotStatus status)
{
    switch (status) {
    case Repository::SnapshotStatus::Done:
        return {};
    case Repository::SnapshotStatus::NameEmpty:
        return QObject::tr("The snapshot needs a name.");
    case Repository::SnapshotStatus::NameTooLong:
        return QObject::tr("The name is too long (at most %1 characters).").arg(Repository::kMaxSnapshotNameChars);
    case Repository::SnapshotStatus::NameTaken:
        return QObject::tr("A snapshot with that name exists already.");
    case Repository::SnapshotStatus::TooManySnapshots:
        return QObject::tr("There are %1 snapshots already, the most there can be. Delete one first.")
            .arg(Repository::kMaxSnapshots);
    case Repository::SnapshotStatus::Missing:
        return QObject::tr("That snapshot is gone.");
    case Repository::SnapshotStatus::Nothing:
        return QObject::tr("Nothing to save.");
    }
    return {};
}

QString snapshotSummary(const Repository::SnapshotResult& r, const QString& name)
{
    QStringList skipped;
    if (r.skippedPasswords > 0) {
        skipped << (r.skippedPasswords == 1 ? QObject::tr("1 password") : QObject::tr("%1 passwords").arg(r.skippedPasswords));
    }
    if (r.skippedPictures > 0) {
        skipped << (r.skippedPictures == 1 ? QObject::tr("1 picture") : QObject::tr("%1 pictures").arg(r.skippedPictures));
    }
    if (r.skippedOnce > 0) {
        skipped << QObject::tr("%1 Paste once").arg(r.skippedOnce);
    }
    if (r.skippedTooLarge > 0) {
        skipped << QObject::tr("%1 over 1 MB").arg(r.skippedTooLarge);
    }
    if (r.skippedOverLimit > 0) {
        skipped << QObject::tr("%1 over the limit of %2 copies").arg(r.skippedOverLimit).arg(Repository::kMaxSnapshotItems);
    }
    const QString skippedText = skipped.isEmpty()
        ? QString()
        : QObject::tr(" Skipped (never put in a snapshot): %1.").arg(skipped.join(QStringLiteral(", ")));
    if (r.status == Repository::SnapshotStatus::Done) {
        const QString already = r.alreadyIn > 0 ? QObject::tr(" (%1 already in it)").arg(r.alreadyIn) : QString();
        return QObject::tr("Saved %1 in the snapshot \"%2\"%3.").arg(copies(r.saved), name, already) + skippedText;
    }
    if (r.status == Repository::SnapshotStatus::Nothing) {
        return QObject::tr("Nothing was saved: no text copy to put in a snapshot.") + skippedText;
    }
    return snapshotStatusText(r.status) + skippedText;
}

SnapshotsWindow::SnapshotsWindow(Repository& repo, TextPrompt textPrompt, ConfirmPrompt confirm, QWidget* parent)
    : QDialog(parent)
    , m_repo(repo)
    , m_text(std::move(textPrompt))
    , m_confirm(std::move(confirm))
    , m_list(new QListWidget(this))
    , m_order(new QComboBox(this))
    , m_intoGroup(new QCheckBox(tr("Put them in a group named after the snapshot"), this))
    , m_restore(new QPushButton(tr("&Restore"), this))
    , m_rename(new QPushButton(tr("Re&name…"), this))
    , m_delete(new QPushButton(tr("&Delete…"), this))
    , m_status(new QLabel(this))
{
    setWindowTitle(tr("Snapshots"));
    setObjectName(QStringLiteral("snapshotsWindow"));
    m_list->setObjectName(QStringLiteral("snapshotList"));
    m_order->setObjectName(QStringLiteral("snapshotOrder"));
    m_intoGroup->setObjectName(QStringLiteral("snapshotIntoGroup"));
    m_restore->setObjectName(QStringLiteral("snapshotRestore"));
    m_rename->setObjectName(QStringLiteral("snapshotRename"));
    m_delete->setObjectName(QStringLiteral("snapshotDelete"));
    m_status->setObjectName(QStringLiteral("snapshotStatus"));
    m_status->setWordWrap(true);
    m_order->addItem(tr("Order: as copied (the one copied last on top)"), false);
    m_order->addItem(tr("Order: as saved (the first one on top)"), true);
    m_restore->setDefault(true);
    m_restore->setToolTip(tr("Put the copies back in the history. Copies that are there already are not doubled, "
                             "only moved to the top."));

    auto* buttons = new QVBoxLayout;
    buttons->addWidget(m_restore);
    buttons->addWidget(m_rename);
    buttons->addWidget(m_delete);
    buttons->addStretch(1);
    auto* close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    buttons->addWidget(close);
    auto* top = new QHBoxLayout;
    top->addWidget(m_list, 1);
    top->addLayout(buttons);
    auto* intro = new QLabel(tr("A snapshot keeps its own copies: it stays when the history is pruned or cleared. "
                                "Passwords, pictures and Paste once copies are never put in one. Deleting a copy from the history, or auto-forget, "
                                "does not remove it from a snapshot."),
                             this);
    intro->setWordWrap(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(top, 1);
    layout->addWidget(m_order);
    layout->addWidget(m_intoGroup);
    layout->addWidget(m_status);
    resize(560, 380);

    connect(m_restore, &QPushButton::clicked, this, [this] { restoreSelected(); });
    connect(m_rename, &QPushButton::clicked, this, [this] { renameSelected(); });
    connect(m_delete, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] { updateButtons(); });
    connect(m_list, &QListWidget::itemActivated, this, [this] { restoreSelected(); });
    reload();
}

QString SnapshotsWindow::status() const
{
    return m_status->text();
}

void SnapshotsWindow::setStatus(const QString& text)
{
    m_status->setText(text);
}

qint64 SnapshotsWindow::selectedId() const
{
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

QString SnapshotsWindow::selectedName() const
{
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(Qt::UserRole + 1).toString() : QString();
}

void SnapshotsWindow::updateButtons()
{
    const bool any = selectedId() != 0;
    m_restore->setEnabled(any);
    m_rename->setEnabled(any);
    m_delete->setEnabled(any);
}

void SnapshotsWindow::reload()
{
    const qint64 keep = selectedId();
    m_list->clear();
    try {
        for (const auto& s : m_repo.snapshots()) {
            auto* item = new QListWidgetItem(
                tr("%1  ·  %2  ·  %3").arg(s.name, copies(s.items), s.created.toLocalTime().toString(Qt::TextDate)), m_list);
            item->setData(Qt::UserRole, s.id);
            item->setData(Qt::UserRole + 1, s.name);
            if (s.id == keep) {
                m_list->setCurrentItem(item);
            }
        }
    } catch (const db::DbError& err) {
        qCWarning(lcSnapshots) << "listing snapshots failed:" << err.what();
    }
    if (!m_list->currentItem() && m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    if (m_list->count() == 0) {
        setStatus(tr("No snapshots yet. Right-click a group or a copy and choose \"Save as snapshot…\"."));
    }
    updateButtons();
}

void SnapshotsWindow::restoreSelected()
{
    const qint64 id = selectedId();
    if (id == 0) {
        return;
    }
    Repository::SnapshotRestoreOptions options;
    options.savedOrder = m_order->currentData().toBool();
    options.intoGroup = m_intoGroup->isChecked();
    Repository::SnapshotRestoreResult r;
    try {
        r = m_repo.restoreSnapshot(id, options);
    } catch (const db::DbError& err) {
        qCWarning(lcSnapshots) << "restoring a snapshot failed:" << err.what();
        setStatus(tr("Restoring failed. Details are in the log."));
        return;
    }
    if (!r.ok) {
        setStatus(snapshotStatusText(Repository::SnapshotStatus::Missing));
        reload();
        return;
    }
    qCDebug(lcSnapshots) << "snapshot restored:" << r.added << "added," << r.alreadyThere << "already there"; // counts only
    QString text = tr("Put back %1").arg(copies(r.added));
    if (r.alreadyThere > 0) {
        text += tr(" (%1 were in the history already and moved to the top)").arg(r.alreadyThere);
    }
    text += QLatin1Char('.');
    if (options.intoGroup && r.filed > 0) {
        text += tr(" %1 filed in the group \"%2\".").arg(r.filed).arg(r.group);
    }
    setStatus(text);
    emit historyChanged();
}

void SnapshotsWindow::renameSelected()
{
    const qint64 id = selectedId();
    if (id == 0) {
        return;
    }
    const auto name = m_text(this, tr("Rename snapshot"), tr("New name:"), selectedName());
    if (!name) {
        return;
    }
    try {
        const auto status = m_repo.renameSnapshot(id, *name);
        setStatus(status == Repository::SnapshotStatus::Done ? tr("Renamed.") : snapshotStatusText(status));
    } catch (const db::DbError& err) {
        qCWarning(lcSnapshots) << "renaming a snapshot failed:" << err.what();
        setStatus(tr("Renaming failed. Details are in the log."));
    }
    reload();
}

void SnapshotsWindow::deleteSelected()
{
    const qint64 id = selectedId();
    if (id == 0) {
        return;
    }
    if (!m_confirm(this, tr("Delete the snapshot \"%1\"?\nThe copies in it are removed from the snapshot. "
                            "Copies already put back in the history stay there. This can't be undone.")
                             .arg(selectedName()))) {
        return;
    }
    try {
        setStatus(m_repo.deleteSnapshot(id) ? tr("Deleted.") : snapshotStatusText(Repository::SnapshotStatus::Missing));
    } catch (const db::DbError& err) {
        qCWarning(lcSnapshots) << "deleting a snapshot failed:" << err.what();
        setStatus(tr("Deleting failed. Details are in the log."));
    }
    reload();
}

} // namespace clipitto::ui
