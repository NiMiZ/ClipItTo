#pragma once

#include "Backup.h"
#include "Config.h"
#include "PasteAs.h"
#include "Translation.h"
#include "Prompts.h"
#include "SelfTestWindow.h"

#include <QDialog>
#include <QPointer>

#include <functional>
#include <optional>
#include <vector>

class QVBoxLayout;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QKeySequenceEdit;
class QLabel;
class QFormLayout;
class QListWidget;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QToolButton;

namespace clipitto {
class PasswordKeeper;
namespace db {
class Repository;
}
} // namespace clipitto

namespace clipitto::ui {

// Whether this platform/session lets ClipItTo set its own global shortcut.
struct ShortcutSupport {
    bool editable = true;
    QString note; // behind the (i) next to the shortcut; also shown under it when not editable
};

// Whether the history can be encrypted here (build support + a reachable system keyring).
struct EncryptionSupport {
    bool available = false;
    QString reason; // why not, when unavailable
    bool active = false; // the history file is encrypted right now
    QString problem; // last failure, if any
};

// Password copies: the master password is set, changed and removed right away (like
// groups); Windows Hello is turned on or off right away too.
struct PasswordSupport {
    PasswordKeeper* keeper = nullptr; // null: no password copies here
    bool helloAvailable = false; // Windows with Windows Hello set up, or a Mac with Touch ID
    QString helloName; // "Windows Hello" or "Touch ID"
    QString helloReason; // why not, when unavailable
    // Turns Windows Hello on (passwords are unlocked by then) or off; `done` gets an error
    // text, empty when it worked.
    std::function<void(bool on, std::function<void(QString error)> done)> setHello;
    // Unlocks with Windows Hello (the prompt); `done(true)` once unlocked. Lets a forgotten
    // master password be replaced.
    std::function<void(std::function<void(bool unlocked)> done)> helloUnlock;
};

// Sync through a folder (Settings > Sync, roadmap 13). The sync passphrase is set, changed and removed right away (like
// the master password); the key made from it lives in the system keyring, never in the config.
struct SyncSupport {
    bool available = false; // a system keyring can hold the key
    QString reason; // why not, when unavailable
    std::function<bool()> hasPassphrase; // a key is stored
    // Joins the folder's group with this passphrase (the files already in the folder must open with it), or with
    // `startNew` makes a new group with a new salt (the other PCs then need the same passphrase). Slow: off the GUI
    // thread. `done` gets an error text, empty when it worked, and whether the folder has files that this passphrase
    // does not open (`mismatch`). The bytes are wiped by the service.
    std::function<void(const QString& folder, QByteArray passphrase, bool startNew,
                       std::function<void(QString error, bool mismatch)> done)>
        setPassphrase;
    std::function<void()> removePassphrase; // forgets the key (this PC stops syncing; the files stay)
    std::function<void()> syncNow;
    std::function<QString()> status; // last sync, devices seen, files ignored and why
    std::function<QString()> computerName; // what an empty device name means
};

// Start with the user's session (the checkbox is shown where it is offered: macOS). Applied
// right away, like groups: there is no config key, the state is the launch agent itself.
struct StartupSupport {
    bool available = false;
    bool enabled = false;
    std::function<bool(bool on, QString* error)> set;
};

// General settings (theme, list size, preview lines, popup position, shortcut, encryption)
// are returned by config() when the dialog is accepted; group changes are applied to the
// repository right away (the Groups tab says so and only offers Close).
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    using Notify = std::function<void(QWidget* parent, const QString& message)>;
    // A file to save to or open ("" = cancelled).
    using FilePrompt = std::function<QString(QWidget* parent, bool save, const QString& suggestedName)>;
    // Which parts of a backup that holds both to import (nullopt = cancelled).
    using PartsPrompt = std::function<std::optional<backup::Parts>(QWidget* parent, const backup::Info& info)>;

    SettingsDialog(const Config& config, db::Repository& repo, ShortcutSupport shortcut,
                   EncryptionSupport encryption = {}, QWidget* parent = nullptr, PasswordSupport passwords = {},
                   StartupSupport startup = {}, TranslationSupport translation = {}, SyncSupport sync = {});

    // Check this installation (General tab, roadmap 31): the button opens a window with the self-test's results.
    void setSelfTest(SelfTestSupport support);
    QPushButton* selfTestButton() const { return m_selfTestButton; }
    SelfTestWindow* selfTestWindow() const { return m_selfTestWindow; }
    void openSelfTest(); // what the button does

    Config config() const;
    bool groupsChanged() const { return m_groupsChanged; }

    void setTextPrompt(TextPrompt prompt) { m_textPrompt = std::move(prompt); }
    void setConfirmPrompt(ConfirmPrompt prompt) { m_confirm = std::move(prompt); }
    void setNotify(Notify notify) { m_notify = std::move(notify); }
    void setInform(Notify inform) { m_inform = std::move(inform); } // news that isn't a problem
    void setPasswordPrompt(PasswordPrompt prompt) { m_passwordPrompt = std::move(prompt); }
    void setMasterPasswordPrompt(MasterPasswordPrompt prompt) { m_masterPrompt = std::move(prompt); }
    void setFilePrompt(FilePrompt prompt) { m_filePrompt = std::move(prompt); }
    // The language pack file to add (Translation tab); "" = cancelled.
    using PackPrompt = std::function<QString(QWidget* parent)>;
    void setPackPrompt(PackPrompt prompt) { m_packPrompt = std::move(prompt); }
    QListWidget* packList() const { return m_packList; }
    QPushButton* addPackButton() const { return m_addPack; }
    QPushButton* removePackButton() const { return m_removePack; }
    void setPartsPrompt(PartsPrompt prompt) { m_partsPrompt = std::move(prompt); }

    // Sync tab. The folder prompt returns "" when cancelled.
    using FolderPrompt = std::function<QString(QWidget* parent, const QString& current)>;
    void setFolderPrompt(FolderPrompt prompt) { m_folderPrompt = std::move(prompt); }
    void setSyncPassphrasePrompt(SyncPassphrasePrompt prompt) { m_syncPrompt = std::move(prompt); }
    void browseSyncFolder();
    void setSyncPassphrase();
    void removeSyncPassphrase();
    void startSyncPassphrase(const QString& folder, QByteArray bytes, bool startNew);
    void refreshSync(); // button states and the status text
    QCheckBox* syncEnabledBox() const { return m_syncEnabled; }
    QLineEdit* syncFolderEdit() const { return m_syncFolder; }
    QLineEdit* syncNameEdit() const { return m_syncName; }
    QSpinBox* syncMaxCopiesBox() const { return m_syncMaxCopies; }
    QCheckBox* syncPicturesBox() const { return m_syncPictures; }
    QSpinBox* syncPictureKbBox() const { return m_syncPictureKb; }
    QSpinBox* syncDaysBox() const { return m_syncDays; }
    QPushButton* syncBrowseButton() const { return m_syncBrowse; }
    QPushButton* syncPassphraseButton() const { return m_syncSet; }
    QPushButton* syncRemoveButton() const { return m_syncRemove; }
    QPushButton* syncNowButton() const { return m_syncNowButton; }
    QLabel* syncStatusLabel() const { return m_syncStatus; }
    QLabel* syncPassphraseLabel() const { return m_syncPassphraseState; }

    // Backup tab. With a master password set, both ask for it first (even when unlocked).
    void exportBackup();
    void importBackup();
    QCheckBox* backupCopiesBox() const { return m_backupCopies; }
    QCheckBox* backupSettingsBox() const { return m_backupSettings; }
    QPushButton* exportButton() const { return m_export; }
    QPushButton* importButton() const { return m_import; }

    // Master password actions (the buttons on the Passwords tab).
    void setMasterPassword();
    void changeMasterPassword();
    void removeMasterPassword();
    void useWithoutMasterPassword();
    void resetMasterPassword();
    QString helloLabel() const;

    // Group actions (the buttons on the Groups tab).
    void newGroup();
    void newSubGroup();
    void renameSelectedGroup();
    void deleteSelectedGroup();
    void moveSelectedGroup(int delta); // -1 = up
    void selectGroup(const QString& path);
    QString selectedGroup() const;

    QComboBox* themeBox() const { return m_theme; }
    QSpinBox* copiesBox() const { return m_copies; }
    QSpinBox* keptBox() const { return m_kept; }
    QSpinBox* imagesBox() const { return m_images; }
    QLineEdit* ocrLanguagesEdit() const { return m_ocrLanguages; }
    QCheckBox* indexPicturesBox() const { return m_indexPictures; }
    QCheckBox* keepFormattingBox() const { return m_keepFormatting; }
    QCheckBox* debugLogBox() const { return m_debugLog; }
    QCheckBox* layoutBox(const QString& id) const;
    QToolButton* encryptionInfo() const { return m_encryptionInfo; }
    QToolButton* shortcutInfo() const { return m_shortcutInfo; }
    QLabel* encryptionStatus() const { return m_encryptionStatus; }
    QSpinBox* linesBox() const { return m_lines; }
    QSpinBox* favoritesBox() const { return m_favorites; }
    QComboBox* positionBox() const { return m_position; }
    QComboBox* startFocusBox() const { return m_startFocus; }
    QCheckBox* encryptBox() const { return m_encrypt; }
    QCheckBox* startupBox() const { return m_startup; }
    QCheckBox* pasteBox() const { return m_paste; }
    QCheckBox* latestBox() const { return m_latest; }
    // Paste keys per program (General tab): the switch and one row per rule "program = keys".
    QCheckBox* pasteKeysPerAppBox() const { return m_pasteKeysPerApp; }
    QWidget* pasteKeysPanel() const { return m_pasteKeysPanel; }
    QPushButton* addPasteKeysButton() const { return m_addPasteKeys; }
    QLabel* pasteKeysErrorLabel() const { return m_pasteKeysError; }
    int pasteKeysRowCount() const { return int(m_pasteKeysRows.size()); }
    QLineEdit* pasteKeysPatternEdit(int row) const { return m_pasteKeysRows.at(size_t(row)).pattern; }
    QComboBox* pasteKeysChordBox(int row) const { return m_pasteKeysRows.at(size_t(row)).chord; }
    QPushButton* pasteKeysRemoveButton(int row) const { return m_pasteKeysRows.at(size_t(row)).remove; }
    QCheckBox* ignoreCaseBox() const { return m_ignoreCase; }
    QCheckBox* passwordsPerNoteBox() const { return m_passwordsPerNote; }
    QCheckBox* detectSecretsBox() const { return m_detectSecrets; }
    QCheckBox* detectPasswordsBox() const { return m_detectPasswords; }
    // Own password rules (Passwords tab): rows can be added/removed; `Try it` tests a sample against them.
    QWidget* rulesPanel() const { return m_rulesPanel; }
    QDialog* rulesDialog() const { return m_rulesWindow.dialog; }
    QDialog* forgetDialog() const { return m_forgetWindow.dialog; }
    QDialog* pasteKeysDialog() const { return m_pasteKeysWindow.dialog; }
    QLabel* rulesSummary() const { return m_rulesWindow.summary; }
    QPushButton* addRuleButton() const { return m_addRule; }
    QLabel* ruleErrorLabel() const { return m_ruleError; }
    QLineEdit* ruleTryEdit() const { return m_ruleTry; }
    QLabel* ruleTryResult() const { return m_ruleTryResult; }
    int ruleRowCount() const { return int(m_ruleRows.size()); }
    // Spin boxes of a rule row, in order: min length, max length, lower, upper, digits, symbols.
    QList<QSpinBox*> ruleSpinBoxes(int row) const;
    QCheckBox* ruleEndBox(int row) const { return m_ruleRows.at(size_t(row)).end; }
    QPushButton* ruleRemoveButton(int row) const { return m_ruleRows.at(size_t(row)).remove; }
    // Auto-forget rules (Passwords tab): one row per rule, a simple form (kind, amount, text or secret kind).
    QWidget* forgetPanel() const { return m_forgetPanel; }
    QPushButton* addForgetButton() const { return m_addForget; }
    QLabel* forgetErrorLabel() const { return m_forgetError; }
    int forgetRowCount() const { return int(m_forgetRows.size()); }
    QComboBox* forgetTypeBox(int row) const { return m_forgetRows.at(size_t(row)).type; }
    QSpinBox* forgetAmountBox(int row) const { return m_forgetRows.at(size_t(row)).amount; }
    QComboBox* forgetScopeBox(int row) const { return m_forgetRows.at(size_t(row)).scope; }
    QLineEdit* forgetTextEdit(int row) const { return m_forgetRows.at(size_t(row)).text; }
    QComboBox* forgetSecretBox(int row) const { return m_forgetRows.at(size_t(row)).secret; }
    QPushButton* forgetRemoveButton(int row) const { return m_forgetRows.at(size_t(row)).remove; }
    QDialogButtonBox* buttons() const { return m_buttons; }
    QTabWidget* tabs() const { return m_tabs; }
    QKeySequenceEdit* shortcutEdit() const { return m_shortcut; }
    QLabel* shortcutStatus() const { return m_shortcutStatus; }
    QLabel* versionLabel() const { return m_version; }
    QTreeWidget* groupTree() const { return m_groups; }
    QCheckBox* autoGroupBox() const { return m_autoGroup; }
    QPushButton* forgetLearnedButton() const { return m_forgetLearned; }
    QCheckBox* keepPasswordsBox() const { return m_keepPasswords; }
    QCheckBox* passwordsInGroupBox() const { return m_passwordsInGroup; }
    QCheckBox* passwordsInRecentBox() const { return m_passwordsInRecent; }
    QCheckBox* askNoteBox() const { return m_askNote; }
    QCheckBox* lockOnScreenLockBox() const { return m_lockOnScreenLock; }
    QSpinBox* unlockMinutesBox() const { return m_unlockMinutes; }
    QSpinBox* clearSecondsBox() const { return m_clearSeconds; }
    QSpinBox* pasteOnceClearSecondsBox() const { return m_pasteOnceClearSeconds; }
    QCheckBox* panicKeyBox() const { return m_panicKey; }
    QKeySequenceEdit* panicShortcutEdit() const { return m_panicShortcut; }
    QLabel* panicShortcutStatus() const { return m_panicStatus; }
    QCheckBox* helloBox() const { return m_hello; }
    QLineEdit* keywordEdit() const { return m_keyword; }
    QLabel* masterStatus() const { return m_masterStatus; }
    QPushButton* setMasterButton() const { return m_setMaster; }
    QPushButton* removeMasterButton() const { return m_removeMaster; }
    QPushButton* useOpenButton() const { return m_useOpen; }
    QPushButton* resetMasterButton() const { return m_resetMaster; }

    // A "Paste as" list: checkable items in menu order, move buttons, reset.
    struct PasteAsEditor {
        QListWidget* list = nullptr;
        QPushButton* up = nullptr;
        QPushButton* down = nullptr;
        QPushButton* reset = nullptr;
        QList<pasteas::Item> items; // the built-in list
    };
    const PasteAsEditor& pasteAsEditor(bool picture) const { return picture ? m_pasteAsPicture : m_pasteAsText; }

    // Saves the one setting that lives in the history (whether password copies are in Recent).
    void accept() override;

private:
    void onShortcutEdited();
    void onPanicShortcutEdited();
    void reloadGroups(const QString& select = {});
    void createGroup(const QString& parentPath);
    void addRuleRow(const PasswordRule& rule);
    void removeRuleRow(QWidget* row);
    void addPasteKeysRow(const PasteKeyRule& rule);
    void removePasteKeysRow(QWidget* row);
    QList<PasteKeyRule> pasteKeysValues() const; // what the rows say (valid or not)
    void refreshPasteKeys(); // validation message, OK button, Add button
    void updateOkButton(); // OK waits while any rule editor shows an error
    void addForgetRow(const AutoForgetRule& rule);
    void removeForgetRow(QWidget* row);
    QList<AutoForgetRule> forgetValues() const; // what the rows say (valid or not)
    void refreshForget(); // per-row controls, validation message, OK button, Add button
    QList<PasswordRule> ruleValues() const; // what the rows say (valid or not)
    void refreshRules(); // validation message, OK button, Add button, "Try it" result
    void updatePasswordPage(); // master password state, buttons
    void updatePasswordDependents(); // "also show in Recent" follows "put in the Passwords group"
    void onHelloToggled(bool on);
    // Asks for the master password when one is set (and unlocks with it), then calls
    // `proceed` with its bytes (empty when there is none). Not called when cancelled or wrong.
    void withMasterPassword(const QString& purpose, std::function<void(QByteArray password)> proceed);
    void finishImport(const QString& path, const backup::Info& info, const QByteArray& masterPassword);

    PasteAsEditor createPasteAsEditor(QWidget* page, QLayout* into, const QString& title, const QList<pasteas::Item>& items,
                                      const QString& setting);
    QString pasteAsSetting(const PasteAsEditor& editor, const QString& loaded) const;

    Config m_config;
    PasteAsEditor m_pasteAsText;
    PasteAsEditor m_pasteAsPicture;
    QCheckBox* m_pasteAsOthers = nullptr;
    db::Repository& m_repo;
    QString m_hotkey; // last valid shortcut, canonical spelling
    QString m_panicHotkey; // the panic shortcut: last valid one, canonical spelling; empty = none
    bool m_shortcutsEditable = true; // this session lets ClipItTo set shortcuts itself
    QCheckBox* m_panicKey = nullptr;
    QKeySequenceEdit* m_panicShortcut = nullptr;
    QLabel* m_panicStatus = nullptr;
    bool m_groupsChanged = false;
    QCheckBox* m_autoGroup = nullptr;
    QPushButton* m_forgetLearned = nullptr;

    QComboBox* m_theme;
    QSpinBox* m_copies;
    QSpinBox* m_kept;
    QSpinBox* m_images;
    QLineEdit* m_ocrLanguages;
    QCheckBox* m_indexPictures;
    QCheckBox* m_keepFormatting;
    QCheckBox* m_debugLog;
    QPushButton* m_selfTestButton = nullptr;
    QPointer<SelfTestWindow> m_selfTestWindow;
    SelfTestSupport m_selfTest;
    QList<QCheckBox*> m_layoutBoxes; // keyboard layouts for Paste as…, in texttransform::layouts() order
    QString m_layoutsSetting; // keyboard_layouts as loaded
    QSpinBox* m_lines;
    QSpinBox* m_favorites;
    QComboBox* m_position;
    QComboBox* m_startFocus;
    QCheckBox* m_encrypt;
    StartupSupport m_startupSupport;
    QCheckBox* m_startup;
    QCheckBox* m_paste;
    QCheckBox* m_latest;
    QCheckBox* m_pasteKeysPerApp;
    QCheckBox* m_ignoreCase;
    QCheckBox* m_detectSecrets;
    QCheckBox* m_detectPasswords;
    struct RuleRow {
        QWidget* row = nullptr;
        QSpinBox* minLength = nullptr;
        QSpinBox* maxLength = nullptr;
        QSpinBox* lower = nullptr;
        QSpinBox* upper = nullptr;
        QSpinBox* digits = nullptr;
        QSpinBox* symbols = nullptr;
        QCheckBox* end = nullptr;
        QPushButton* remove = nullptr;
        QLabel* title = nullptr;
    };
    struct ForgetRow {
        QWidget* row = nullptr;
        QComboBox* type = nullptr; // Age / Contains / Secret
        QSpinBox* amount = nullptr;
        QLabel* unit = nullptr;
        QComboBox* scope = nullptr; // Age: all / text / pictures
        QLineEdit* text = nullptr; // Contains
        QComboBox* secret = nullptr; // Secret: any / token / key / card
        QPushButton* remove = nullptr;
    };
    struct PasteKeysRow {
        QWidget* row = nullptr;
        QLineEdit* pattern = nullptr;
        QComboBox* chord = nullptr;
        QPushButton* remove = nullptr;
    };
    // A rules editor lives in its own window (opened by a button on the page), not in the page itself.
    struct RulesWindow {
        QDialog* dialog = nullptr;
        QPushButton* button = nullptr;
        QLabel* summary = nullptr;
    };
    RulesWindow addRulesWindow(QFormLayout* form, const QString& label, const QString& title, QWidget* panel,
                               const QString& buttonText);
    void showRulesSummary(const RulesWindow& window, const QString& text, bool problem);
    RulesWindow m_rulesWindow;
    RulesWindow m_forgetWindow;
    RulesWindow m_pasteKeysWindow;
    QWidget* m_pasteKeysPanel = nullptr;
    QVBoxLayout* m_pasteKeysLayout = nullptr;
    std::vector<PasteKeysRow> m_pasteKeysRows;
    QPushButton* m_addPasteKeys = nullptr;
    QLabel* m_pasteKeysError = nullptr;
    QWidget* m_forgetPanel = nullptr;
    QVBoxLayout* m_forgetLayout = nullptr;
    std::vector<ForgetRow> m_forgetRows;
    QPushButton* m_addForget = nullptr;
    QLabel* m_forgetError = nullptr;
    QWidget* m_rulesPanel = nullptr;
    QVBoxLayout* m_rulesLayout = nullptr;
    std::vector<RuleRow> m_ruleRows;
    QPushButton* m_addRule = nullptr;
    QLabel* m_ruleError = nullptr;
    QLineEdit* m_ruleTry = nullptr;
    QLabel* m_ruleTryResult = nullptr;
    QCheckBox* m_passwordsPerNote;
    QTabWidget* m_tabs;
    QDialogButtonBox* m_buttons;
    QKeySequenceEdit* m_shortcut;
    QLabel* m_shortcutStatus; // feedback on an edited shortcut; hidden while empty
    QToolButton* m_shortcutInfo;
    QToolButton* m_encryptionInfo;
    QLabel* m_encryptionStatus; // only when something is wrong (not available, not encrypted)
    QLabel* m_version; // "ClipItTo 0.2.1", next to the buttons
    QTreeWidget* m_groups;
    QPushButton* m_subGroupButton;
    QPushButton* m_renameButton;
    QPushButton* m_deleteButton;
    QPushButton* m_upButton;
    QPushButton* m_downButton;
    PasswordSupport m_passwordSupport;
    QCheckBox* m_keepPasswords;
    QCheckBox* m_passwordsInGroup;
    QCheckBox* m_passwordsInRecent;
    QCheckBox* m_askNote;
    QCheckBox* m_lockOnScreenLock;
    QSpinBox* m_unlockMinutes;
    QSpinBox* m_clearSeconds;
    QSpinBox* m_pasteOnceClearSeconds;
    QCheckBox* m_hello;
    QLineEdit* m_keyword;
    QLabel* m_masterStatus;
    QPushButton* m_setMaster; // "Set…" or "Change…"
    QPushButton* m_removeMaster;
    QPushButton* m_resetMaster; // forgot it: replace it after a Windows Hello unlock
    QPushButton* m_useOpen; // keep passwords without a master password
    QLabel* m_openWarning;
    bool m_busy = false; // a master password is being derived
    QCheckBox* m_backupCopies;
    QCheckBox* m_backupSettings;
    QPushButton* m_export;
    QPushButton* m_import;
    SyncSupport m_syncSupport;
    QCheckBox* m_syncEnabled = nullptr;
    QLineEdit* m_syncFolder = nullptr;
    QLineEdit* m_syncName = nullptr;
    QSpinBox* m_syncMaxCopies = nullptr;
    QCheckBox* m_syncPictures = nullptr;
    QSpinBox* m_syncPictureKb = nullptr;
    QSpinBox* m_syncDays = nullptr;
    QPushButton* m_syncBrowse = nullptr;
    QPushButton* m_syncSet = nullptr; // "Set…" or "Change…"
    QPushButton* m_syncRemove = nullptr;
    QPushButton* m_syncNowButton = nullptr;
    QLabel* m_syncStatus = nullptr;
    QLabel* m_syncPassphraseState = nullptr;
    bool m_syncBusy = false; // a key is being derived
    FolderPrompt m_folderPrompt;
    SyncPassphrasePrompt m_syncPrompt;
    std::optional<Config> m_imported; // settings taken from a backup: what config() returns

    TextPrompt m_textPrompt;
    ConfirmPrompt m_confirm;
    Notify m_notify;
    Notify m_inform;
    PasswordPrompt m_passwordPrompt;
    MasterPasswordPrompt m_masterPrompt;
    FilePrompt m_filePrompt;
    PackPrompt m_packPrompt;
    TranslationSupport m_translation;
    QListWidget* m_packList = nullptr;
    QPushButton* m_addPack = nullptr;
    QPushButton* m_removePack = nullptr;
    void reloadPacks();
    PartsPrompt m_partsPrompt;
};

} // namespace clipitto::ui
