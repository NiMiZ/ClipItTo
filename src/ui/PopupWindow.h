#pragma once

#include "Entry.h"
#include "HistoryModel.h"
#include "Prompts.h"

#include <QSet>
#include <QWidget>

#include <functional>
#include <optional>

class QDialog;
class QLabel;
class QMenu;
class QToolButton;

namespace clipitto {
struct Config;
namespace db {
class Repository;
}
} // namespace clipitto

namespace clipitto::ui {

class HistoryView;
class SearchBar;
class SearchController;

// The popup: search bar and settings button on top; below, the collapsed group tree,
// saved items and recent copies. Built once and shown/hidden on demand. It edits
// favorites, groups and notes itself; copying the chosen entry back to the clipboard is
// the owner's job (connect entryActivated), and happens before the popup hides.
class PopupWindow : public QWidget {
    Q_OBJECT

public:
    // Asks the user for a value. `choices` is empty for free text. nullopt = cancelled.
    using Prompt = std::function<std::optional<QString>(QWidget* parent, const QString& current,
                                                        const QStringList& choices)>;
    using Notify = std::function<void(QWidget* parent, const QString& message)>;

    PopupWindow(db::Repository& repo, const Config& config, QWidget* parent = nullptr);
    ~PopupWindow() override;

    void applyConfig(const Config& config);

    void showPopup(const QString& activationToken = {});
    void hidePopup();
    void togglePopup(const QString& activationToken = {});

    // A clip was stored while we may be visible: refresh the default view.
    void historyChanged();
    // Re-reads everything (e.g. after groups changed in the settings), keeping the selection.
    void refresh();

    // Runs a dialog on top of the popup without the popup hiding on focus loss.
    int execDialog(QDialog& dialog);

    void setGroupPrompt(Prompt prompt) { m_groupPrompt = std::move(prompt); }
    void setNotePrompt(Prompt prompt) { m_notePrompt = std::move(prompt); }
    void setTextPrompt(TextPrompt prompt) { m_textPrompt = std::move(prompt); }
    void setConfirmPrompt(ConfirmPrompt prompt) { m_confirm = std::move(prompt); }
    void setNotify(Notify notify) { m_notify = std::move(notify); }

    QMenu* createContextMenu(const Entry& entry, QWidget* parent);
    QMenu* createGroupMenu(const QString& path, QWidget* parent); // "" = the Groups row

    SearchBar* searchBar() const { return m_search; }
    HistoryView* view() const { return m_view; }
    HistoryModel* model() const { return m_model; }
    SearchController* controller() const { return m_controller; }
    QToolButton* settingsButton() const { return m_settingsButton; }

signals:
    void entryActivated(const clipitto::Entry& entry);
    void visibilityChanged(bool visible);
    void settingsRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    // What to select again after the model is rebuilt.
    struct Selection {
        HistoryModel::Kind kind = HistoryModel::Kind::Entry;
        qint64 id = 0;
        QString path; // group path (groups; copies inside the group tree)
    };

    const Entry* entryFor(const QModelIndex& index) const;
    Selection selectionOf(const QModelIndex& index) const;
    void activate(const QModelIndex& index);
    void openGroups();
    void toggleFavorite(qint64 id);
    void editGroup(qint64 id);
    void editNote(qint64 id);
    void remove(const QModelIndex& index);
    void newGroup(const QString& parentPath);
    void renameGroup(const QString& path);
    void deleteGroup(const QString& path);
    void showContextMenu(const QModelIndex& index, const QPoint& globalPos);
    // focusList: the user just acted on the list (edit, menu, dialog), so it gets focus back;
    // background refreshes leave focus where it is.
    void refreshSelecting(std::optional<Selection> selection, bool focusList = true);
    void onResultsUpdated();
    void placeOnScreen();

    db::Repository& m_repo;
    HistoryModel* m_model;
    SearchController* m_controller;
    SearchBar* m_search;
    QToolButton* m_settingsButton;
    HistoryView* m_view;
    QLabel* m_footer;
    Prompt m_groupPrompt;
    Prompt m_notePrompt;
    TextPrompt m_textPrompt;
    ConfirmPrompt m_confirm;
    Notify m_notify;
    bool m_hideOnFocusLoss = true;
    bool m_singleClick = true;
    int m_modalDepth = 0; // dialogs/menus open: don't hide on focus loss
    std::optional<Selection> m_restore; // reselected after the next refresh
    QSet<QString> m_expanded; // expanded groups ("" = the Groups row); reset when shown
};

} // namespace clipitto::ui
