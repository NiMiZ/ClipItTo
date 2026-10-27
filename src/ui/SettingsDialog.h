#pragma once

#include "Backup.h"
#include "Config.h"
#include "Prompts.h"

#include <QDialog>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QKeySequenceEdit;
class QLabel;
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
    bool helloAvailable = false; // Windows with Windows Hello set up
    QString helloReason; // why not, when unavailable
    // Turns Windows Hello on (passwords are unlocked by then) or off; `done` gets an error
    // text, empty when it worked.
    std::function<void(bool on, std::function<void(QString error)> done)> setHello;
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
                   StartupSupport startup = {});

    Config config() const;
    bool groupsChanged() const { return m_groupsChanged; }

    void setTextPrompt(TextPrompt prompt) { m_textPrompt = std::move(prompt); }
    void setConfirmPrompt(ConfirmPrompt prompt) { m_confirm = std::move(prompt); }
    void setNotify(Notify notify) { m_notify = std::move(notify); }
    void setInform(Notify inform) { m_inform = std::move(inform); } // news that isn't a problem
    void setPasswordPrompt(PasswordPrompt prompt) { m_passwordPrompt = std::move(prompt); }
    void setMasterPasswordPrompt(MasterPasswordPrompt prompt) { m_masterPrompt = std::move(prompt); }
    void setFilePrompt(FilePrompt prompt) { m_filePrompt = std::move(prompt); }
    void setPartsPrompt(PartsPrompt prompt) { m_partsPrompt = std::move(prompt); }

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
    QDialogButtonBox* buttons() const { return m_buttons; }
    QTabWidget* tabs() const { return m_tabs; }
    QKeySequenceEdit* shortcutEdit() const { return m_shortcut; }
    QLabel* shortcutStatus() const { return m_shortcutStatus; }
    QLabel* versionLabel() const { return m_version; }
    QTreeWidget* groupTree() const { return m_groups; }
    QCheckBox* keepPasswordsBox() const { return m_keepPasswords; }
    QCheckBox* passwordsInGroupBox() const { return m_passwordsInGroup; }
    QCheckBox* passwordsInRecentBox() const { return m_passwordsInRecent; }
    QCheckBox* askNoteBox() const { return m_askNote; }
    QSpinBox* unlockMinutesBox() const { return m_unlockMinutes; }
    QSpinBox* clearSecondsBox() const { return m_clearSeconds; }
    QCheckBox* helloBox() const { return m_hello; }
    QLineEdit* keywordEdit() const { return m_keyword; }
    QLabel* masterStatus() const { return m_masterStatus; }
    QPushButton* setMasterButton() const { return m_setMaster; }
    QPushButton* removeMasterButton() const { return m_removeMaster; }

    // Saves the one setting that lives in the history (whether password copies are in Recent).
    void accept() override;

private:
    void onShortcutEdited();
    void reloadGroups(const QString& select = {});
    void createGroup(const QString& parentPath);
    void updatePasswordPage(); // master password state, buttons
    void updatePasswordDependents(); // "also show in Recent" follows "put in the Passwords group"
    void onHelloToggled(bool on);
    // Asks for the master password when one is set (and unlocks with it), then calls
    // `proceed` with its bytes (empty when there is none). Not called when cancelled or wrong.
    void withMasterPassword(const QString& purpose, std::function<void(QByteArray password)> proceed);
    void finishImport(const QString& path, const backup::Info& info, const QByteArray& masterPassword);

    Config m_config;
    db::Repository& m_repo;
    QString m_hotkey; // last valid shortcut, canonical spelling
    bool m_groupsChanged = false;

    QComboBox* m_theme;
    QSpinBox* m_copies;
    QSpinBox* m_kept;
    QSpinBox* m_images;
    QSpinBox* m_lines;
    QSpinBox* m_favorites;
    QComboBox* m_position;
    QComboBox* m_startFocus;
    QCheckBox* m_encrypt;
    StartupSupport m_startupSupport;
    QCheckBox* m_startup;
    QCheckBox* m_paste;
    QCheckBox* m_latest;
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
    QSpinBox* m_unlockMinutes;
    QSpinBox* m_clearSeconds;
    QCheckBox* m_hello;
    QLineEdit* m_keyword;
    QLabel* m_masterStatus;
    QPushButton* m_setMaster; // "Set…" or "Change…"
    QPushButton* m_removeMaster;
    bool m_busy = false; // a master password is being derived
    QCheckBox* m_backupCopies;
    QCheckBox* m_backupSettings;
    QPushButton* m_export;
    QPushButton* m_import;
    std::optional<Config> m_imported; // settings taken from a backup: what config() returns

    TextPrompt m_textPrompt;
    ConfirmPrompt m_confirm;
    Notify m_notify;
    Notify m_inform;
    PasswordPrompt m_passwordPrompt;
    MasterPasswordPrompt m_masterPrompt;
    FilePrompt m_filePrompt;
    PartsPrompt m_partsPrompt;
};

} // namespace clipitto::ui
