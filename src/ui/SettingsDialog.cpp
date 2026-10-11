#include "SettingsDialog.h"

#include "GroupPath.h"
#include "Hotkey.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace clipitto::ui {

namespace {

const QString kDefaultHotkey = Config{}.hotkey;

// Measured with tests/bench_encryption (see README, "Encrypted history").
constexpr const char* kEncryptionNote = QT_TRANSLATE_NOOP(
    "clipitto::ui::SettingsDialog",
    "256-bit encryption (ChaCha20-Poly1305); the key is kept in your system keyring, so there is no "
    "password to type. Cost: about 25 ms more when ClipItTo starts and under 1 ms more per saved copy; searching is just as fast. Turning it on or off converts the history once. Copies made before turning it on may "
    "remain readable in free disk space; for that, use full-disk encryption.");

QKeySequence sequenceFor(const QString& hotkey)
{
    return QKeySequence::fromString(hotkey, QKeySequence::PortableText);
}

} // namespace

SettingsDialog::SettingsDialog(const Config& config, db::Repository& repo, ShortcutSupport shortcut,
                               EncryptionSupport encryption, QWidget* parent)
    : QDialog(parent)
    , m_config(config)
    , m_repo(repo)
    , m_hotkey(config.hotkey)
    , m_theme(new QComboBox(this))
    , m_copies(new QSpinBox(this))
    , m_lines(new QSpinBox(this))
    , m_favorites(new QSpinBox(this))
    , m_position(new QComboBox(this))
    , m_encrypt(new QCheckBox(tr("Encrypt the history on disk"), this))
    , m_paste(new QCheckBox(tr("Paste it into the window I came from"), this))
    , m_tabs(new QTabWidget(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
    , m_shortcut(new QKeySequenceEdit(this))
    , m_shortcutStatus(new QLabel(this))
    , m_groups(new QTreeWidget(this))
    , m_subGroupButton(new QPushButton(tr("New sub-group…"), this))
    , m_renameButton(new QPushButton(tr("Rename or move…"), this))
    , m_deleteButton(new QPushButton(tr("Delete"), this))
    , m_upButton(new QPushButton(tr("Move up"), this))
    , m_downButton(new QPushButton(tr("Move down"), this))
    , m_textPrompt(defaultTextPrompt())
    , m_confirm(defaultConfirmPrompt())
    , m_notify([](QWidget* p, const QString& message) { QMessageBox::warning(p, QStringLiteral("ClipItTo"), message); })
{
    setWindowTitle(tr("ClipItTo Settings"));
    resize(480, 440);

    // --- General -----------------------------------------------------------------------
    auto* general = new QWidget(this);
    auto* form = new QFormLayout(general);

    m_theme->addItem(tr("Same as the system"), int(Theme::System));
    m_theme->addItem(tr("Light"), int(Theme::Light));
    m_theme->addItem(tr("Dark"), int(Theme::Dark));
    m_theme->setCurrentIndex(m_theme->findData(int(config.theme)));
    form->addRow(tr("Theme:"), m_theme);

    m_copies->setRange(1, 500);
    m_copies->setValue(config.recentCount);
    m_copies->setToolTip(tr("How many recent copies are listed when the search box is empty"));
    form->addRow(tr("Copies listed:"), m_copies);

    m_lines->setRange(1, 10);
    m_lines->setValue(config.previewLines);
    m_lines->setToolTip(tr("1 squeezes each copy onto one line; more shows its first lines"));
    form->addRow(tr("Preview lines per copy:"), m_lines);

    m_favorites->setRange(1, 20);
    m_favorites->setValue(config.favoritesLimit);
    m_favorites->setToolTip(tr("Starred copies that are in no group are kept in Favorites, up to this many. "
                               "A group holds up to 10 starred copies, listed first."));
    form->addRow(tr("Favorites limit:"), m_favorites);

    m_paste->setChecked(config.pasteOnEnter);
    m_paste->setToolTip(tr("Off: choosing a copy only puts it on the clipboard. On GNOME Wayland, "
                           "pasting needs the ClipItTo GNOME Shell extension."));
    form->addRow(tr("After choosing a copy:"), m_paste);

    m_position->addItem(tr("Near the mouse pointer"), int(PopupPosition::Cursor));
    m_position->addItem(tr("Center of the screen"), int(PopupPosition::Center));
    m_position->setCurrentIndex(m_position->findData(int(config.popupPosition)));
    m_position->setToolTip(tr("On Wayland desktops the desktop decides where the popup opens"));
    form->addRow(tr("Open the popup:"), m_position);

    auto* shortcutRow = new QHBoxLayout;
    m_shortcut->setKeySequence(sequenceFor(config.hotkey));
    auto* resetShortcut = new QPushButton(tr("Default"), this);
    resetShortcut->setToolTip(kDefaultHotkey);
    shortcutRow->addWidget(m_shortcut, 1);
    shortcutRow->addWidget(resetShortcut);
    form->addRow(tr("Shortcut:"), shortcutRow);
    m_shortcutStatus->setWordWrap(true);
    m_shortcutStatus->setEnabled(false); // dim
    m_shortcutStatus->setText(shortcut.note);
    form->addRow(QString(), m_shortcutStatus);
    if (!shortcut.editable) {
        m_shortcut->setEnabled(false);
        resetShortcut->setEnabled(false);
    }
    connect(m_shortcut, &QKeySequenceEdit::keySequenceChanged, this, &SettingsDialog::onShortcutEdited);

    m_encrypt->setChecked(config.encryptHistory);
    form->addRow(tr("Privacy:"), m_encrypt);
    auto* encryptNote = new QLabel(this);
    encryptNote->setWordWrap(true);
    encryptNote->setEnabled(false); // dim
    QString note = encryption.available ? tr(kEncryptionNote) : tr("Not available: %1").arg(encryption.reason);
    if (!encryption.available) {
        m_encrypt->setEnabled(false); // keep the current state visible but unchangeable
    }
    if (config.encryptHistory && !encryption.active) {
        // The setting asks for it, the file is not: say so plainly, never let it look done.
        note = tr("<b>Currently NOT encrypted</b>%1. ")
                   .arg(encryption.problem.isEmpty() ? QString() : QStringLiteral(": ") + encryption.problem.toHtmlEscaped())
            + note.toHtmlEscaped();
        encryptNote->setEnabled(true);
    }
    encryptNote->setText(note);
    form->addRow(QString(), encryptNote);
    connect(resetShortcut, &QPushButton::clicked, this, [this] { m_shortcut->setKeySequence(sequenceFor(kDefaultHotkey)); });

    // --- Groups ------------------------------------------------------------------------
    auto* groupsPage = new QWidget(this);
    auto* groupsLayout = new QVBoxLayout(groupsPage);
    m_groups->setColumnCount(2);
    m_groups->setHeaderLabels({tr("Group"), tr("Copies")});
    m_groups->header()->setStretchLastSection(false);
    m_groups->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_groups->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    groupsLayout->addWidget(m_groups, 1);
    auto* buttons = new QHBoxLayout;
    auto* newButton = new QPushButton(tr("New group…"), this);
    for (QPushButton* b : {newButton, m_subGroupButton, m_renameButton, m_deleteButton}) {
        b->setAutoDefault(false);
        buttons->addWidget(b);
    }
    groupsLayout->addLayout(buttons);
    auto* order = new QHBoxLayout;
    for (QPushButton* b : {m_upButton, m_downButton}) {
        b->setAutoDefault(false);
        order->addWidget(b);
    }
    order->addStretch(1);
    groupsLayout->addLayout(order);
    auto* hint = new QLabel(tr("<b>Changes to groups are saved immediately.</b> A copy can be put in a "
                               "group from the popup (Ctrl+G or right-click). Move up/down sets the order "
                               "in the popup (Alt+↑/↓ there); Favorites always comes first."),
                            this);
    hint->setWordWrap(true);
    groupsLayout->addWidget(hint);
    connect(newButton, &QPushButton::clicked, this, &SettingsDialog::newGroup);
    connect(m_subGroupButton, &QPushButton::clicked, this, &SettingsDialog::newSubGroup);
    connect(m_renameButton, &QPushButton::clicked, this, &SettingsDialog::renameSelectedGroup);
    connect(m_deleteButton, &QPushButton::clicked, this, &SettingsDialog::deleteSelectedGroup);
    connect(m_upButton, &QPushButton::clicked, this, [this] { moveSelectedGroup(-1); });
    connect(m_downButton, &QPushButton::clicked, this, [this] { moveSelectedGroup(1); });
    connect(m_groups, &QTreeWidget::itemDoubleClicked, this, [this] { renameSelectedGroup(); });
    connect(m_groups, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool any = !selectedGroup().isEmpty();
        for (QPushButton* b : {m_subGroupButton, m_renameButton, m_deleteButton}) {
            b->setEnabled(any);
        }
        // Only where there is a sibling to swap with.
        const QList<QTreeWidgetItem*> items = m_groups->selectedItems();
        QTreeWidgetItem* item = items.isEmpty() ? nullptr : items.first();
        const int row = item ? (item->parent() ? item->parent()->indexOfChild(item) : m_groups->indexOfTopLevelItem(item)) : -1;
        const int siblings = item ? (item->parent() ? item->parent()->childCount() : m_groups->topLevelItemCount()) : 0;
        m_upButton->setEnabled(row > 0);
        m_downButton->setEnabled(row >= 0 && row < siblings - 1);
    });
    reloadGroups();

    // --- Dialog ------------------------------------------------------------------------
    m_tabs->addTab(general, tr("General"));
    m_tabs->addTab(groupsPage, tr("Groups"));
    // Group edits are already saved, so Cancel would promise an undo it can't give: the
    // Groups tab only offers Close (which also keeps any General changes, like OK).
    QPushButton* close = m_buttons->addButton(QDialogButtonBox::Close);
    close->setToolTip(tr("Close the settings, keeping all changes"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    const auto showButtonsFor = [this, close](int tab) {
        const bool groupsTab = tab == 1;
        m_buttons->button(QDialogButtonBox::Ok)->setVisible(!groupsTab);
        m_buttons->button(QDialogButtonBox::Cancel)->setVisible(!groupsTab);
        close->setVisible(groupsTab);
    };
    connect(m_tabs, &QTabWidget::currentChanged, this, showButtonsFor);
    showButtonsFor(0);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(m_buttons);
}

Config SettingsDialog::config() const
{
    Config c = m_config;
    c.theme = Theme(m_theme->currentData().toInt());
    c.recentCount = m_copies->value();
    c.previewLines = m_lines->value();
    c.favoritesLimit = m_favorites->value();
    c.hotkey = m_hotkey;
    c.popupPosition = PopupPosition(m_position->currentData().toInt());
    c.encryptHistory = m_encrypt->isChecked();
    c.pasteOnEnter = m_paste->isChecked();
    return c;
}

void SettingsDialog::onShortcutEdited()
{
    const QKeySequence sequence = m_shortcut->keySequence();
    if (sequence.isEmpty()) {
        return;
    }
    // One key combination is a shortcut; QKeySequenceEdit would record up to four.
    const QKeySequence first(sequence[0]);
    if (sequence.count() > 1) {
        QSignalBlocker block(m_shortcut);
        m_shortcut->setKeySequence(first);
    }
    const auto parsed = parseHotkey(first.toString(QKeySequence::PortableText));
    if (parsed) {
        m_hotkey = parsed->canonical;
        m_shortcutStatus->setText(tr("Opens the popup: %1").arg(m_hotkey));
    } else {
        m_shortcutStatus->setText(tr("%1 can't be used: use Ctrl, Alt, Shift or Super with a letter, digit, "
                                     "F-key or ` - = [ ] ; ' , . /. Keeping %2.")
                                      .arg(first.toString(QKeySequence::NativeText), m_hotkey));
    }
}

QString SettingsDialog::selectedGroup() const
{
    const QList<QTreeWidgetItem*> items = m_groups->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();
}

void SettingsDialog::selectGroup(const QString& path)
{
    for (QTreeWidgetItemIterator it(m_groups); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == path) {
            m_groups->setCurrentItem(*it);
            return;
        }
    }
}

void SettingsDialog::reloadGroups(const QString& select)
{
    m_groups->clear();
    QHash<QString, QTreeWidgetItem*> byPath;
    try {
        for (const auto& g : m_repo.groupTree()) {
            QTreeWidgetItem* parent = byPath.value(groups::parent(g.path));
            auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_groups);
            item->setText(0, groups::leaf(g.path));
            item->setText(1, g.clipCount ? QString::number(g.clipCount) : QString());
            item->setData(0, Qt::UserRole, g.path);
            byPath.insert(g.path, item);
        }
    } catch (const db::DbError& e) {
        m_notify(this, QString::fromUtf8(e.what()));
    }
    m_groups->expandAll();
    // Always select something real: otherwise Qt shows a focus frame on the first row
    // while nothing is selected and the buttons are disabled.
    if (!select.isEmpty()) {
        selectGroup(select);
    }
    if (m_groups->selectedItems().isEmpty() && m_groups->topLevelItemCount() > 0) {
        m_groups->setCurrentItem(m_groups->topLevelItem(0));
    }
    emit m_groups->itemSelectionChanged(); // refresh button states
}

void SettingsDialog::createGroup(const QString& parentPath)
{
    const auto name = m_textPrompt(this, parentPath.isEmpty() ? tr("New group") : tr("New sub-group"),
                                   parentPath.isEmpty()
                                       ? tr("Name (use / for sub-groups, e.g. Work/Servers):")
                                       : tr("Name of the new group inside %1:").arg(groups::display(parentPath)),
                                   QString());
    if (!name || groups::normalize(*name).isEmpty()) {
        return;
    }
    try {
        const QString path = m_repo.createGroup(parentPath.isEmpty() ? *name : parentPath + groups::kSeparator + *name);
        m_groupsChanged = true;
        reloadGroups(path);
    } catch (const db::DbError& e) {
        m_notify(this, QString::fromUtf8(e.what()));
    }
}

void SettingsDialog::newGroup()
{
    createGroup(QString());
}

void SettingsDialog::newSubGroup()
{
    const QString parent = selectedGroup();
    if (!parent.isEmpty()) {
        createGroup(parent);
    }
}

void SettingsDialog::renameSelectedGroup()
{
    const QString path = selectedGroup();
    if (path.isEmpty()) {
        return;
    }
    const auto target = m_textPrompt(this, tr("Rename group"),
                                     tr("New name, or a full path to move it (e.g. Work/Servers):"), path);
    if (!target) {
        return;
    }
    QString error;
    bool ok = false;
    try {
        ok = m_repo.renameGroup(path, *target, &error);
    } catch (const db::DbError& e) {
        error = QString::fromUtf8(e.what());
    }
    if (!ok) {
        m_notify(this, tr("Could not rename the group: %1").arg(error));
        return;
    }
    m_groupsChanged = true;
    reloadGroups(groups::normalize(*target));
}

void SettingsDialog::moveSelectedGroup(int delta)
{
    const QString path = selectedGroup();
    if (path.isEmpty()) {
        return;
    }
    try {
        if (m_repo.moveGroup(path, delta)) {
            m_groupsChanged = true;
            reloadGroups(path);
        }
    } catch (const db::DbError& e) {
        m_notify(this, QString::fromUtf8(e.what()));
    }
}

void SettingsDialog::deleteSelectedGroup()
{
    const QString path = selectedGroup();
    if (path.isEmpty()
        || !m_confirm(this, tr("Delete the group %1 and its sub-groups?\nTheir copies stay in the history.")
                                .arg(groups::display(path)))) {
        return;
    }
    try {
        m_repo.deleteGroup(path);
        m_groupsChanged = true;
        reloadGroups(groups::parent(path));
    } catch (const db::DbError& e) {
        m_notify(this, QString::fromUtf8(e.what()));
    }
}

} // namespace clipitto::ui
