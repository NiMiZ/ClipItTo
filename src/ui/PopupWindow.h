#pragma once

#include "Config.h"
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
class QTimer;
class QWindow;
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

// The popup: search bar and settings button on top; below, the collapsed group tree
// (Favorites first), pinned and recent copies. Built once and shown/hidden on demand. It
// edits stars, pins, groups and notes itself; copying the chosen entry back to the clipboard is
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
    // Where it last closed, e.g. remembered from an earlier run (popup_position = last).
    void setLastPosition(QPoint topLeft) { m_lastPosition = topLeft; }
    std::optional<QPoint> lastPosition() const { return m_lastPosition; }

    void showPopup(const QString& activationToken = {});
    void hidePopup();
    void togglePopup(const QString& activationToken = {});

    // A clip was stored while we may be visible: refresh the default view.
    void historyChanged();
    // Re-reads everything (e.g. after groups changed in the settings), keeping the selection.
    void refresh();

    // Runs a dialog on top of the popup without the popup hiding on focus loss.
    int execDialog(QDialog& dialog);

    // Non-empty: the history is unavailable for a moment (e.g. being encrypted); the popup
    // shows the message and makes no database calls. Empty: back to normal.
    void setBusy(const QString& message);
    // A warning over the popup (kept open meanwhile). Uses the notify hook (tests replace it).
    void showMessage(const QString& message);
    bool isBusy() const { return !m_busyMessage.isEmpty(); }

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
    QWidget* dragHandle() const { return m_dragHandle; }

    // Hands a drag of the popup to the compositor (Wayland, where only it may move windows).
    // The popup stays open while the compositor holds the focus during the move. Returns
    // false where the platform can't do it. The move counts as over when the popup gets the
    // focus back, when the pointer comes back to it, or after kSystemMoveTimeoutMs; if it is
    // inactive by then (and hide_on_focus_loss is set), it closes.
    bool startSystemMove();
    // Tests: replaces QWindow::startSystemMove() (which the offscreen platform refuses).
    using SystemMoveStarter = std::function<bool(QWindow*)>;
    void setSystemMoveStarter(SystemMoveStarter starter) { m_systemMoveStarter = std::move(starter); }

    static constexpr int kSystemMoveTimeoutMs = 30000;
    static constexpr int kSystemMoveSettleMs = 300; // after the pointer is back, for the focus

signals:
    void entryActivated(const clipitto::Entry& entry);
    void aboutToShow(); // before the popup takes focus (the previous window still has it)
    void closedAt(QPoint topLeft); // where it was when it hid (for popup_position = last)
    void visibilityChanged(bool visible);
    void settingsRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
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
    void togglePin(qint64 id);
    void assignGroup(qint64 id, const QString& group, Selection follow);
    void moveItem(const QModelIndex& index, int delta); // a group, or a pinned copy
    void moveGroup(const QString& path, int delta);
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
    void endSystemMove();

    db::Repository& m_repo;
    HistoryModel* m_model;
    SearchController* m_controller;
    SearchBar* m_search;
    QToolButton* m_settingsButton;
    QWidget* m_dragHandle; // the app name: drag it to move the popup
    HistoryView* m_view;
    QLabel* m_footer;
    Prompt m_groupPrompt;
    Prompt m_notePrompt;
    TextPrompt m_textPrompt;
    ConfirmPrompt m_confirm;
    Notify m_notify;
    bool m_hideOnFocusLoss = true;
    bool m_singleClick = true;
    PopupPosition m_position = PopupPosition::Cursor;
    std::optional<QPoint> m_lastPosition; // where it last closed
    QString m_busyMessage;
    int m_modalDepth = 0; // dialogs/menus open: don't hide on focus loss
    bool m_systemMove = false; // the compositor is moving us: don't hide on focus loss
    bool m_pointerLeftDuringMove = false;
    QTimer* m_moveEnd = nullptr; // ends the move (backstop, or shortly after the pointer is back)
    SystemMoveStarter m_systemMoveStarter;
    std::optional<Selection> m_restore; // reselected after the next refresh
    QSet<QString> m_expanded; // expanded groups ("" = the Groups row); reset when shown
};

} // namespace clipitto::ui
