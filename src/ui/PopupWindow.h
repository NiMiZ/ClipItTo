#pragma once

#include "Entry.h"

#include <QWidget>

#include <functional>
#include <optional>

class QLabel;
class QMenu;

namespace clipitto {
struct Config;
namespace db {
class Repository;
}
} // namespace clipitto

namespace clipitto::ui {

class HistoryModel;
class HistoryView;
class SearchBar;
class SearchController;

// The popup: search bar on top, history below. Built once and shown/hidden on demand.
// It edits favorites/groups/notes itself; copying the chosen entry back to the clipboard is
// the owner's job (connect entryActivated), and happens before the popup hides.
class PopupWindow : public QWidget {
    Q_OBJECT

public:
    // Asks the user for a value. `choices` is empty for free text. nullopt = cancelled.
    using Prompt = std::function<std::optional<QString>(QWidget* parent, const QString& current,
                                                        const QStringList& choices)>;

    PopupWindow(db::Repository& repo, const Config& config, QWidget* parent = nullptr);
    ~PopupWindow() override;

    void applyConfig(const Config& config);

    void showPopup(const QString& activationToken = {});
    void hidePopup();
    void togglePopup(const QString& activationToken = {});

    // A clip was stored while we may be visible: refresh the default view.
    void historyChanged();

    void setGroupPrompt(Prompt prompt) { m_groupPrompt = std::move(prompt); }
    void setNotePrompt(Prompt prompt) { m_notePrompt = std::move(prompt); }

    QMenu* createContextMenu(const Entry& entry, QWidget* parent);

    SearchBar* searchBar() const { return m_search; }
    HistoryView* view() const { return m_view; }
    HistoryModel* model() const { return m_model; }
    SearchController* controller() const { return m_controller; }

signals:
    void entryActivated(const clipitto::Entry& entry);
    void visibilityChanged(bool visible);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    const Entry* entryFor(const QModelIndex& index) const;
    void activate(const QModelIndex& index);
    void toggleFavorite(qint64 id);
    void editGroup(qint64 id);
    void editNote(qint64 id);
    void remove(qint64 id);
    void showContextMenu(const QModelIndex& index, const QPoint& globalPos);
    void afterEdit(qint64 selectId);
    void onResultsUpdated();
    void placeOnScreen();

    db::Repository& m_repo;
    HistoryModel* m_model;
    SearchController* m_controller;
    SearchBar* m_search;
    HistoryView* m_view;
    QLabel* m_footer;
    Prompt m_groupPrompt;
    Prompt m_notePrompt;
    bool m_hideOnFocusLoss = true;
    bool m_singleClick = true;
    int m_modalDepth = 0; // dialogs/menus open: don't hide on focus loss
    qint64 m_restoreId = 0; // entry to reselect after the next refresh
};

} // namespace clipitto::ui
