#pragma once

#include "Config.h"
#include "Entry.h"
#include "TextTransform.h"
#include "Translation.h"
#include "HistoryModel.h"
#include "PasteAs.h"
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
class PasswordKeeper;
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
    void setEditPrompt(EditPrompt prompt) { m_editPrompt = std::move(prompt); }
    // The largest text a copy may be edited to (max_item_bytes).
    void setMaxItemBytes(qint64 bytes) { m_maxItemBytes = bytes; }
    // A short line in place of the footer hints for a few seconds (no modal box).
    void flash(const QString& message);
    void setTextPrompt(TextPrompt prompt) { m_textPrompt = std::move(prompt); }
    void setConfirmPrompt(ConfirmPrompt prompt) { m_confirm = std::move(prompt); }
    void setNotify(Notify notify) { m_notify = std::move(notify); }
    // Shows a text that is not a problem (a translation, a hint): a quiet window without a warning icon or a
    // system beep. With `offerPaste` it has a Paste button; returns true when the user chose it.
    using TextViewer = std::function<bool(QWidget* parent, const QString& text, bool offerPaste)>;
    void setTextViewer(TextViewer viewer) { m_viewText = std::move(viewer); }
    void setPasswordPrompt(PasswordPrompt prompt) { m_passwordPrompt = std::move(prompt); }
    void setNamePrompt(NamePrompt prompt) { m_namePrompt = std::move(prompt); }

    // "Ask for a name after copying a password": opens the popup with the new copy selected
    // and a name box on it (`suggestion`, the app it came from, as placeholder). A typed name
    // becomes its note; with nothing typed (OK, Enter or Cancel) the suggestion is the note. The popup closes again
    // afterwards if it wasn't open, so the focus goes back to where the user was.
    void askPasswordNote(qint64 id, const QString& suggestion);

    // Password copies (null: none): shown hidden; showing, pasting and "not a password" ask
    // for the master password first, unless unlocked. The owner pastes them (entryActivated
    // comes after the unlock).
    void setPasswords(PasswordKeeper* keeper);
    // Windows Hello: tries it and reports whether it unlocked; the master password is asked
    // for when it didn't. Unset: master password only.
    using HelloUnlock = std::function<void(std::function<void(bool unlocked)> done)>;
    void setHelloUnlock(HelloUnlock hello) { m_helloUnlock = std::move(hello); }

    // Offline translation (see Translation.h): "Translate ▸" in the Paste as… menus.
    void setTranslation(TranslationSupport translation) { m_translation = std::move(translation); }

    // "Paste text from picture": reads the text in a stored picture (off the GUI thread, offline) and
    // calls `done(text, error)`; `available`/`reason` say whether this computer can (the menu entry is
    // shown disabled, with the reason, when not).
    using TextFromPicture = std::function<void(qint64 id, std::function<void(const QString& text, const QString& error)> done)>;
    void setTextFromPicture(TextFromPicture read, bool available, const QString& reason)
    {
        m_textFromPicture = std::move(read);
        m_textFromPictureAvailable = available;
        m_textFromPictureReason = reason;
    }
    // Runs `then` once passwords are unlocked (asking for that); not at all if cancelled.
    void withPasswordsUnlocked(std::function<void()> then);
    // Shows a password copy's text in its row for kRevealMs (asks to unlock first).
    void revealPassword(qint64 id);
    static constexpr int kRevealMs = 10000;

    QMenu* createContextMenu(const Entry& entry, QWidget* parent);
    // The transforms for a text copy (Shift+Enter, and a submenu of the right-click menu).
    QMenu* createPasteAsMenu(const Entry& entry, QWidget* parent);
    static void fixMnemonics(QMenu* menu);
    void addKeyboardLayoutEntries(QMenu* menu, const QString& text);
    void pasteAs(const QModelIndex& index);
    void addOthersMenu(QMenu* menu, const QList<pasteas::Item>& items, const QStringList& shown,
                       const std::function<void(QMenu*, const QString&)>& addItem);
    // Reads the text in picture `id` and pastes it, changed by `kind` when given.
    void pasteTextFromPicture(qint64 id, std::optional<texttransform::Kind> kind = std::nullopt);
    QMenu* createPictureMenu(const Entry& picture, QWidget* parent);
    // "Translate ▸" with the installed packs (those that fit `text` when given); disabled with a hint when there is none.
    void addTranslateMenu(QMenu* menu, const QString& title, const QString& text, std::function<void(const QString& packId)> chosen);
    void previewTranslation();
    bool showQuiet(const QString& text, bool offerPaste = false); // see setTextViewer
    // How long a translation may take before the popup stops waiting for it (tests shorten it).
    void setTranslateTimeoutMs(int ms) { m_translateTimeoutMs = ms; }
    void translateAndPaste(const QString& packId, const QString& text);
    void translateWithTimeout(const QString& packId, const QString& text, std::function<void(const QString&, const QString&)> done);
    void translatePictureAndPaste(qint64 id, const QString& packId);
    QMenu* createGroupMenu(const QString& path, QWidget* parent); // "" = the Groups row
    QMenu* createFavoritesMenu(QWidget* parent);
    void addOutOfRecentAction(QMenu* menu, const QString& path); // path "" = Favorites
    void setGroupOutOfRecent(const QString& path, bool out); // "" = Favorites

    SearchBar* searchBar() const { return m_search; }
    HistoryView* view() const { return m_view; }
    HistoryModel* model() const { return m_model; }
    SearchController* controller() const { return m_controller; }
    QToolButton* settingsButton() const { return m_settingsButton; }
    QToolButton* clearButton() const { return m_clearButton; }

    // Deletes the history after asking: unsaved copies, or with `everything` also starred,
    // pinned and grouped ones (groups stay). The Clear button and its menu call these.
    void clearHistory(bool everything);
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
    // "Paste as…": this text (the copy, transformed) goes to the clipboard and into the window; the
    // stored copy is not touched.
    void textActivated(const QString& text);
    // Ctrl+E saved a new text for this copy (its place and time are unchanged).
    void textEdited(qint64 id, const QString& text);
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
    void remove(const QModelIndex& index, bool focusList = true);
    void newGroup(const QString& parentPath);
    void renameGroup(const QString& path);
    void renameIndex(const QModelIndex& index); // F2
    void deleteGroup(const QString& path);
    void showContextMenu(const QModelIndex& index, const QPoint& globalPos);
    void markAsPassword(qint64 id);
    void unmarkPassword(qint64 id);
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
    QToolButton* m_clearButton;
    QWidget* m_dragHandle; // the app name: drag it to move the popup
    HistoryView* m_view;
    QLabel* m_footer;
    Prompt m_groupPrompt;
    EditPrompt m_editPrompt;
    qint64 m_maxItemBytes = 1024 * 1024;
    QString m_footerHint;
    TextPrompt m_textPrompt;
    ConfirmPrompt m_confirm;
    Notify m_notify;
    TextViewer m_viewText;
    int m_translateTimeoutMs = 30000;
    PasswordPrompt m_passwordPrompt;
    NamePrompt m_namePrompt;
    bool m_namingPassword = false; // a name box is open or about to open: one at a time
    PasswordKeeper* m_passwords = nullptr;
    TranslationSupport m_translation;
    QString m_pasteAsText; // paste_as_text / paste_as_picture: which Paste as… items, in what order
    QString m_pasteAsPicture;
    bool m_pasteAsOthers = true; // paste_as_others: an "Others" submenu with the unticked items
    QStringList m_keyboardLayouts; // layouts besides English that Paste as… converts to and from
    HelloUnlock m_helloUnlock;
    TextFromPicture m_textFromPicture;
    bool m_textFromPictureAvailable = false;
    QString m_textFromPictureReason;
    QTimer* m_revealTimer = nullptr; // hides revealed passwords again
    bool m_unlocking = false; // an unlock is running: don't start another
    bool m_hideOnFocusLoss = true;
    bool m_singleClick = true;
    PopupPosition m_position = PopupPosition::Cursor;
    StartFocus m_startFocus = StartFocus::Pinned;
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
