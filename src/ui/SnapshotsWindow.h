#pragma once

#include "Prompts.h"
#include "db/Repository.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace clipitto::ui {

// What a "save as snapshot" did, in words: how many copies went in and how many were skipped and why (passwords, pictures,
// Paste once copies, too large, over the limit); or why nothing was made. Never the text of a copy.
QString snapshotSummary(const db::Repository::SnapshotResult& result, const QString& name);
// The refusal for a name or a limit ("A snapshot with that name exists."); empty for Done.
QString snapshotStatusText(db::Repository::SnapshotStatus status);

// Snapshots (roadmap 29, docs/snapshots-design.md): the list (name, number of copies, date), Restore, Rename, Delete (asks
// first). It shows names and counts only, never the text of a copy. Built fresh each time it is opened.
class SnapshotsWindow : public QDialog {
    Q_OBJECT

public:
    SnapshotsWindow(db::Repository& repo, TextPrompt textPrompt, ConfirmPrompt confirm, QWidget* parent = nullptr);

    // The actions of the buttons, on the selected snapshot (tests call them).
    void restoreSelected();
    void renameSelected();
    void deleteSelected();
    void reload();
    QString status() const;

signals:
    void historyChanged(); // a restore put copies back

private:
    qint64 selectedId() const;
    QString selectedName() const;
    void updateButtons();
    void setStatus(const QString& text);

    db::Repository& m_repo;
    TextPrompt m_text;
    ConfirmPrompt m_confirm;
    QListWidget* m_list;
    QComboBox* m_order;
    QCheckBox* m_intoGroup;
    QPushButton* m_restore;
    QPushButton* m_rename;
    QPushButton* m_delete;
    QLabel* m_status;
};

} // namespace clipitto::ui
