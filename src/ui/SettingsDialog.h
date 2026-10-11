#pragma once

#include "Config.h"
#include "Prompts.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;

namespace clipitto {
namespace db {
class Repository;
}
} // namespace clipitto

namespace clipitto::ui {

// Whether this platform/session lets ClipItTo set its own global shortcut.
struct ShortcutSupport {
    bool editable = true;
    QString note; // shown under the shortcut field
};

// Whether the history can be encrypted here (build support + a reachable system keyring).
struct EncryptionSupport {
    bool available = false;
    QString reason; // why not, when unavailable
    bool active = false; // the history file is encrypted right now
    QString problem; // last failure, if any
};

// General settings (theme, list size, preview lines, popup position, shortcut, encryption)
// are returned by config() when the dialog is accepted; group changes are applied to the
// repository right away (the Groups tab says so and only offers Close).
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    using Notify = std::function<void(QWidget* parent, const QString& message)>;

    SettingsDialog(const Config& config, db::Repository& repo, ShortcutSupport shortcut,
                   EncryptionSupport encryption = {}, QWidget* parent = nullptr);

    Config config() const;
    bool groupsChanged() const { return m_groupsChanged; }

    void setTextPrompt(TextPrompt prompt) { m_textPrompt = std::move(prompt); }
    void setConfirmPrompt(ConfirmPrompt prompt) { m_confirm = std::move(prompt); }
    void setNotify(Notify notify) { m_notify = std::move(notify); }

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
    QSpinBox* linesBox() const { return m_lines; }
    QSpinBox* favoritesBox() const { return m_favorites; }
    QComboBox* positionBox() const { return m_position; }
    QCheckBox* encryptBox() const { return m_encrypt; }
    QCheckBox* pasteBox() const { return m_paste; }
    QDialogButtonBox* buttons() const { return m_buttons; }
    QTabWidget* tabs() const { return m_tabs; }
    QKeySequenceEdit* shortcutEdit() const { return m_shortcut; }
    QLabel* shortcutStatus() const { return m_shortcutStatus; }
    QTreeWidget* groupTree() const { return m_groups; }

private:
    void onShortcutEdited();
    void reloadGroups(const QString& select = {});
    void createGroup(const QString& parentPath);

    Config m_config;
    db::Repository& m_repo;
    QString m_hotkey; // last valid shortcut, canonical spelling
    bool m_groupsChanged = false;

    QComboBox* m_theme;
    QSpinBox* m_copies;
    QSpinBox* m_lines;
    QSpinBox* m_favorites;
    QComboBox* m_position;
    QCheckBox* m_encrypt;
    QCheckBox* m_paste;
    QTabWidget* m_tabs;
    QDialogButtonBox* m_buttons;
    QKeySequenceEdit* m_shortcut;
    QLabel* m_shortcutStatus;
    QTreeWidget* m_groups;
    QPushButton* m_subGroupButton;
    QPushButton* m_renameButton;
    QPushButton* m_deleteButton;
    QPushButton* m_upButton;
    QPushButton* m_downButton;

    TextPrompt m_textPrompt;
    ConfirmPrompt m_confirm;
    Notify m_notify;
};

} // namespace clipitto::ui
