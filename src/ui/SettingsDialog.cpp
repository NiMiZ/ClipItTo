#include "SettingsDialog.h"

#include "GroupPath.h"
#include "Hotkey.h"
#include "PasswordKeeper.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
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

// An (i) icon whose explanation shows as a tooltip on hover, or right away on click, so
// longer notes don't take room in the window. `html` may hold simple markup.
QToolButton* makeInfoButton(QWidget* parent)
{
    auto* button = new QToolButton(parent);
    const QIcon icon = parent->style()->standardIcon(QStyle::SP_MessageBoxInformation);
    if (icon.isNull()) {
        button->setText(QStringLiteral("ⓘ"));
    } else {
        button->setIcon(icon);
        button->setIconSize(QSize(16, 16));
    }
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setAccessibleName(SettingsDialog::tr("More information"));
    QObject::connect(button, &QToolButton::clicked, button, [button] {
        QToolTip::showText(button->mapToGlobal(QPoint(0, button->height())), button->toolTip(), button);
    });
    return button;
}

void setInfo(QToolButton* button, const QString& html)
{
    // A paragraph makes Qt treat it as rich text, which wraps instead of one long line.
    button->setToolTip(QStringLiteral("<p>%1</p>").arg(html));
}

// A dim, wrapping note under a field, hidden while it has nothing to say.
void setNote(QLabel* label, const QString& text)
{
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

} // namespace

SettingsDialog::SettingsDialog(const Config& config, db::Repository& repo, ShortcutSupport shortcut,
                               EncryptionSupport encryption, QWidget* parent, PasswordSupport passwords)
    : QDialog(parent)
    , m_config(config)
    , m_repo(repo)
    , m_hotkey(config.hotkey)
    , m_theme(new QComboBox(this))
    , m_copies(new QSpinBox(this))
    , m_kept(new QSpinBox(this))
    , m_images(new QSpinBox(this))
    , m_lines(new QSpinBox(this))
    , m_favorites(new QSpinBox(this))
    , m_position(new QComboBox(this))
    , m_startFocus(new QComboBox(this))
    , m_encrypt(new QCheckBox(tr("Encrypt the history on disk"), this))
    , m_paste(new QCheckBox(tr("Paste it into the window I came from"), this))
    , m_latest(new QCheckBox(tr("Make it the latest copy in the history"), this))
    , m_tabs(new QTabWidget(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
    , m_shortcut(new QKeySequenceEdit(this))
    , m_shortcutStatus(new QLabel(this))
    , m_shortcutInfo(makeInfoButton(this))
    , m_encryptionInfo(makeInfoButton(this))
    , m_encryptionStatus(new QLabel(this))
    , m_version(new QLabel(this))
    , m_groups(new QTreeWidget(this))
    , m_subGroupButton(new QPushButton(tr("New sub-group…"), this))
    , m_renameButton(new QPushButton(tr("Rename or move…"), this))
    , m_deleteButton(new QPushButton(tr("Delete"), this))
    , m_upButton(new QPushButton(tr("Move up"), this))
    , m_downButton(new QPushButton(tr("Move down"), this))
    , m_passwordSupport(std::move(passwords))
    , m_keepPasswords(new QCheckBox(tr("Keep copies marked by password managers, encrypted"), this))
    , m_passwordsInGroup(new QCheckBox(tr("Put new password copies in the \"Passwords\" group"), this))
    , m_unlockMinutes(new QSpinBox(this))
    , m_clearSeconds(new QSpinBox(this))
    , m_hello(new QCheckBox(tr("Unlock with Windows Hello too"), this))
    , m_keyword(new QLineEdit(this))
    , m_masterStatus(new QLabel(this))
    , m_setMaster(new QPushButton(this))
    , m_removeMaster(new QPushButton(tr("Remove…"), this))
    , m_textPrompt(defaultTextPrompt())
    , m_confirm(defaultConfirmPrompt())
    , m_notify([](QWidget* p, const QString& message) { QMessageBox::warning(p, QStringLiteral("ClipItTo"), message); })
    , m_inform([](QWidget* p, const QString& message) { QMessageBox::information(p, QStringLiteral("ClipItTo"), message); })
    , m_passwordPrompt(defaultPasswordPrompt())
    , m_masterPrompt(defaultMasterPasswordPrompt())
{
    setWindowTitle(tr("ClipItTo Settings"));
    setMinimumWidth(480); // the height follows the contents (see the end)

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
    m_copies->setToolTip(tr("How many recent copies the popup lists when the search box is empty (1-500). "
                            "Searching looks through the whole history."));
    form->addRow(tr("Copies listed:"), m_copies);

    m_kept->setRange(10, 1'000'000);
    m_kept->setGroupSeparatorShown(true);
    m_kept->setValue(config.maxHistory);
    m_kept->setToolTip(tr("How many copies the history keeps before the oldest are deleted (10-1,000,000). "
                          "Pinned, starred and grouped copies are always kept and don't count."));
    form->addRow(tr("Copies kept in history:"), m_kept);

    m_images->setRange(0, 100'000);
    m_images->setGroupSeparatorShown(true);
    m_images->setSpecialValueText(tr("Don't record pictures")); // shown for 0
    m_images->setValue(config.maxImages);
    m_images->setToolTip(tr("How many copied pictures the history keeps, apart from text copies (0-100,000; 0 "
                            "records no pictures). Pinned, starred and grouped pictures are always kept. "
                            "Pictures larger than %1 MB are not recorded.")
                             .arg(config.maxImageBytes / (1024 * 1024)));
    form->addRow(tr("Pictures kept in history:"), m_images);

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
    m_latest->setChecked(config.latestAfterPaste);
    m_latest->setToolTip(tr("As if you had copied it again. Off: it keeps its place in Recent."));
    form->addRow(QString(), m_latest);

    m_position->addItem(tr("Near the mouse pointer"), int(PopupPosition::Cursor));
    m_position->addItem(tr("Center of the screen"), int(PopupPosition::Center));
    m_position->addItem(tr("Top left corner"), int(PopupPosition::TopLeft));
    m_position->addItem(tr("Top right corner"), int(PopupPosition::TopRight));
    m_position->addItem(tr("Bottom left corner"), int(PopupPosition::BottomLeft));
    m_position->addItem(tr("Bottom right corner"), int(PopupPosition::BottomRight));
    m_position->addItem(tr("Where it was last (drag it by its name)"), int(PopupPosition::Last));
    m_position->setCurrentIndex(m_position->findData(int(config.popupPosition)));
    m_position->setToolTip(tr("On Wayland desktops the desktop decides where the popup opens"));
    form->addRow(tr("Open the popup:"), m_position);

    m_startFocus->addItem(tr("Pinned copies"), int(StartFocus::Pinned));
    m_startFocus->addItem(tr("Group copies"), int(StartFocus::Grouped));
    m_startFocus->addItem(tr("Unsaved history"), int(StartFocus::Unsaved));
    m_startFocus->setCurrentIndex(m_startFocus->findData(int(config.startFocus)));
    m_startFocus->setToolTip(tr("Where the selection starts when the popup opens and after each search: on the "
                                "first copy of that kind (the first copy when there is none). Unsaved history: "
                                "copies that aren't pinned, starred or in a group."));
    form->addRow(tr("Start on:"), m_startFocus);

    auto* shortcutRow = new QHBoxLayout;
    m_shortcut->setKeySequence(sequenceFor(config.hotkey));
    auto* resetShortcut = new QPushButton(tr("Default"), this);
    resetShortcut->setToolTip(kDefaultHotkey);
    shortcutRow->addWidget(m_shortcut, 1);
    shortcutRow->addWidget(resetShortcut);
    shortcutRow->addWidget(m_shortcutInfo);
    form->addRow(tr("Shortcut:"), shortcutRow);
    setInfo(m_shortcutInfo, shortcut.note.toHtmlEscaped());
    m_shortcutInfo->setVisible(!shortcut.note.isEmpty());
    m_shortcutStatus->setWordWrap(true);
    m_shortcutStatus->setEnabled(false); // dim
    // Shown only when it matters: why the field can't be changed here; later, feedback on
    // a newly typed shortcut.
    setNote(m_shortcutStatus, shortcut.editable ? QString() : shortcut.note);
    form->addRow(QString(), m_shortcutStatus);
    if (!shortcut.editable) {
        m_shortcut->setEnabled(false);
        resetShortcut->setEnabled(false);
    }
    connect(m_shortcut, &QKeySequenceEdit::keySequenceChanged, this, &SettingsDialog::onShortcutEdited);

    m_encrypt->setChecked(config.encryptHistory);
    auto* encryptRow = new QHBoxLayout;
    encryptRow->addWidget(m_encrypt);
    encryptRow->addWidget(m_encryptionInfo);
    encryptRow->addStretch(1);
    form->addRow(tr("Privacy:"), encryptRow);
    // The explanation lives behind the (i); the window only says something when it's wrong.
    QString info = tr(kEncryptionNote).toHtmlEscaped();
    QString status;
    if (!encryption.available) {
        m_encrypt->setEnabled(false); // keep the current state visible but unchangeable
        status = tr("Not available on this system.");
        info = tr("<b>Not available:</b> %1").arg(encryption.reason.toHtmlEscaped()) + QStringLiteral("<br><br>") + info;
    }
    if (config.encryptHistory && !encryption.active) {
        // The setting asks for it, the file is not: say so plainly, never let it look done.
        status = tr("<b>Currently NOT encrypted.</b>") + (encryption.available ? QString() : QLatin1Char(' ') + status);
        if (!encryption.problem.isEmpty()) {
            info = tr("<b>Last problem:</b> %1").arg(encryption.problem.toHtmlEscaped()) + QStringLiteral("<br><br>") + info;
        }
    }
    setInfo(m_encryptionInfo, info);
    m_encryptionStatus->setWordWrap(true);
    m_encryptionStatus->setEnabled(config.encryptHistory && !encryption.active); // a warning isn't dimmed
    setNote(m_encryptionStatus, status);
    form->addRow(QString(), m_encryptionStatus);
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

    // --- Passwords ---------------------------------------------------------------------
    auto* passwordsPage = new QWidget(this);
    auto* passwordsForm = new QFormLayout(passwordsPage);
    auto* masterRow = new QHBoxLayout;
    masterRow->addWidget(m_masterStatus, 1);
    masterRow->addWidget(m_setMaster);
    masterRow->addWidget(m_removeMaster);
    for (QPushButton* b : {m_setMaster, m_removeMaster}) {
        b->setAutoDefault(false);
    }
    auto* masterInfo = makeInfoButton(this);
    setInfo(masterInfo, tr("Copies that password managers (KeePassXC, Bitwarden, KeePass, 1Password, ...) mark as "
                           "secret are kept encrypted once a master password is set, and shown as ●●●●. "
                           "Showing or pasting one asks for the master password. The master password "
                           "is never stored: if you forget it, the password copies can't be recovered "
                           "(remove it to start over). It protects them in the history file and from "
                           "others using your computer while they are locked; programs you run can still "
                           "read the clipboard when a password is copied.")
                            .toHtmlEscaped());
    masterRow->addWidget(masterInfo);
    passwordsForm->addRow(tr("Master password:"), masterRow);
    m_keepPasswords->setChecked(config.keepPasswords);
    passwordsForm->addRow(tr("Password copies:"), m_keepPasswords);
    m_passwordsInGroup->setChecked(config.passwordsInGroup);
    passwordsForm->addRow(QString(), m_passwordsInGroup);
    m_unlockMinutes->setRange(1, 240);
    m_unlockMinutes->setSuffix(tr(" min"));
    m_unlockMinutes->setValue(config.passwordUnlockMinutes);
    passwordsForm->addRow(tr("An unlock lasts:"), m_unlockMinutes);
    m_clearSeconds->setRange(0, 3600);
    m_clearSeconds->setSuffix(tr(" s"));
    m_clearSeconds->setSpecialValueText(tr("Never"));
    m_clearSeconds->setValue(config.passwordClearSeconds);
    m_clearSeconds->setToolTip(tr("A password you paste from ClipItTo is taken off the clipboard after this, "
                                  "unless you copied something else meanwhile."));
    passwordsForm->addRow(tr("Clear a pasted password after:"), m_clearSeconds);
    auto* helloRow = new QHBoxLayout;
    helloRow->addWidget(m_hello);
    auto* helloInfo = makeInfoButton(this);
    helloRow->addWidget(helloInfo);
    helloRow->addStretch(1);
    passwordsForm->addRow(QString(), helloRow);
    QString helloText = tr("Your face, fingerprint or Windows Hello PIN unlocks the password copies; the "
                           "master password keeps working too. Turning it on asks for the master password once.")
                            .toHtmlEscaped();
    if (!m_passwordSupport.helloAvailable) {
        helloText = tr("<b>Not available:</b> %1").arg(m_passwordSupport.helloReason.toHtmlEscaped())
            + QStringLiteral("<br><br>") + helloText;
    }
    setInfo(helloInfo, helloText);
    m_keyword->setText(config.passwordSearchKeyword);
    m_keyword->setMaxLength(16);
    m_keyword->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\S{1,16}")), m_keyword));
    m_keyword->setToolTip(tr("Type it in the search box with a space after it to search inside your password "
                             "copies; what you type next is shown as asterisks."));
    passwordsForm->addRow(tr("Password search keyword:"), m_keyword);
    connect(m_setMaster, &QPushButton::clicked, this, [this] {
        if (m_passwordSupport.keeper && m_passwordSupport.keeper->isSetUp()) {
            changeMasterPassword();
        } else {
            setMasterPassword();
        }
    });
    connect(m_removeMaster, &QPushButton::clicked, this, &SettingsDialog::removeMasterPassword);
    connect(m_hello, &QCheckBox::toggled, this, &SettingsDialog::onHelloToggled);
    updatePasswordPage();

    // --- Dialog ------------------------------------------------------------------------
    m_tabs->addTab(general, tr("General"));
    m_tabs->addTab(groupsPage, tr("Groups"));
    m_tabs->addTab(passwordsPage, tr("Passwords"));
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
    // Which version is running, so it's clear what the settings belong to.
    const QString version = QCoreApplication::applicationVersion();
    m_version->setText(version.isEmpty() ? QStringLiteral("ClipItTo") : QStringLiteral("ClipItTo %1").arg(version));
    m_version->setEnabled(false); // dim
    m_version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(m_version);
    bottom->addStretch(1);
    bottom->addWidget(m_buttons);
    layout->addLayout(bottom);
    // As tall as the contents need (wrapped notes included), instead of a fixed size that
    // cut text off with larger fonts.
    resize(qMax(520, sizeHint().width()), sizeHint().height());
}

Config SettingsDialog::config() const
{
    Config c = m_config;
    c.theme = Theme(m_theme->currentData().toInt());
    c.recentCount = m_copies->value();
    c.maxHistory = m_kept->value();
    c.maxImages = m_images->value();
    c.previewLines = m_lines->value();
    c.favoritesLimit = m_favorites->value();
    c.hotkey = m_hotkey;
    c.popupPosition = PopupPosition(m_position->currentData().toInt());
    c.startFocus = StartFocus(m_startFocus->currentData().toInt());
    c.encryptHistory = m_encrypt->isChecked();
    c.pasteOnEnter = m_paste->isChecked();
    c.latestAfterPaste = m_latest->isChecked();
    c.keepPasswords = m_keepPasswords->isChecked();
    c.passwordsInGroup = m_passwordsInGroup->isChecked();
    c.passwordUnlockMinutes = m_unlockMinutes->value();
    c.passwordClearSeconds = m_clearSeconds->value();
    c.windowsHello = m_passwordSupport.keeper && m_passwordSupport.keeper->helloEnabled();
    if (m_keyword->hasAcceptableInput()) {
        c.passwordSearchKeyword = m_keyword->text();
    }
    return c;
}

void SettingsDialog::updatePasswordPage()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    const bool setUp = keeper && keeper->isSetUp();
    if (!keeper) {
        m_masterStatus->setText(tr("Not available here."));
    } else if (!setUp) {
        m_masterStatus->setText(tr("Not set: password copies are not kept."));
    } else {
        qint64 count = 0;
        try {
            count = m_repo.countSecrets();
        } catch (const db::DbError&) {
        }
        m_masterStatus->setText(tr("Set. %n password copies kept.", nullptr, int(count)));
    }
    m_setMaster->setText(setUp ? tr("Change…") : tr("Set…"));
    m_setMaster->setEnabled(keeper && !m_busy);
    m_removeMaster->setEnabled(setUp && !m_busy);
    for (QWidget* w : {static_cast<QWidget*>(m_keepPasswords), static_cast<QWidget*>(m_passwordsInGroup),
                       static_cast<QWidget*>(m_unlockMinutes), static_cast<QWidget*>(m_clearSeconds),
                       static_cast<QWidget*>(m_keyword)}) {
        w->setEnabled(setUp);
    }
    const QSignalBlocker block(m_hello);
    m_hello->setChecked(setUp && keeper->helloEnabled());
    m_hello->setEnabled(setUp && m_passwordSupport.helloAvailable && m_passwordSupport.setHello && !m_busy);
}

void SettingsDialog::setMasterPassword()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || keeper->isSetUp() || m_busy) {
        return;
    }
    std::optional<MasterPasswordAnswer> answer = m_masterPrompt(this, /*change=*/false);
    if (!answer) {
        return;
    }
    QByteArray chosen = PasswordVault::passwordBytes(answer->chosen);
    m_busy = true;
    updatePasswordPage();
    keeper->setMasterPassword(std::move(chosen), [this, self = QPointer<SettingsDialog>(this)](bool ok) {
        if (!self) {
            return;
        }
        m_busy = false;
        updatePasswordPage();
        if (ok) {
            m_inform(this, tr("Master password set. Copies marked by password managers are kept, encrypted, "
                              "from now on."));
        } else {
            m_notify(this, tr("Could not set the master password."));
        }
    });
}

void SettingsDialog::changeMasterPassword()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || !keeper->isSetUp() || m_busy) {
        return;
    }
    std::optional<MasterPasswordAnswer> answer = m_masterPrompt(this, /*change=*/true);
    if (!answer) {
        return;
    }
    QByteArray current = PasswordVault::passwordBytes(answer->current);
    QByteArray chosen = PasswordVault::passwordBytes(answer->chosen);
    m_busy = true;
    updatePasswordPage();
    keeper->changeMasterPassword(std::move(current), std::move(chosen), [this, self = QPointer<SettingsDialog>(this)](bool ok) {
        if (!self) {
            return;
        }
        m_busy = false;
        updatePasswordPage();
        if (ok) {
            m_inform(this, tr("Master password changed. Every password copy now uses a new key%1.")
                               .arg(m_passwordSupport.helloAvailable ? tr("; turn Windows Hello on again to use it")
                                                                     : QString()));
        } else {
            m_notify(this, tr("The current master password is not right."));
        }
    });
}

void SettingsDialog::removeMasterPassword()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || !keeper->isSetUp() || m_busy) {
        return;
    }
    qint64 count = 0;
    try {
        count = m_repo.countSecrets();
    } catch (const db::DbError&) {
    }
    if (!m_confirm(this, tr("Remove the master password and delete all %n password copies? This can't be undone.",
                            nullptr, int(count)))) {
        return;
    }
    if (keeper->helloEnabled() && m_passwordSupport.setHello) {
        m_passwordSupport.setHello(false, [](const QString&) {}); // its Windows Hello key goes too
    }
    keeper->reset();
    updatePasswordPage();
}

void SettingsDialog::onHelloToggled(bool on)
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || !m_passwordSupport.setHello || m_busy) {
        return;
    }
    const auto apply = [this, on, self = QPointer<SettingsDialog>(this)] {
        if (!self) {
            return;
        }
        m_busy = true;
        updatePasswordPage();
        m_passwordSupport.setHello(on, [this, self](const QString& error) {
            if (!self) {
                return;
            }
            m_busy = false;
            updatePasswordPage();
            if (!error.isEmpty()) {
                m_notify(this, tr("Windows Hello: %1").arg(error));
            }
        });
    };
    if (!on || keeper->isUnlocked()) {
        apply();
        return;
    }
    // Turning it on wraps the unlocked key: unlock first.
    std::optional<QString> password = m_passwordPrompt(this, tr("Master password:"));
    if (!password) {
        updatePasswordPage(); // unchecks it again
        return;
    }
    QByteArray bytes = PasswordVault::passwordBytes(*password);
    m_busy = true;
    updatePasswordPage();
    keeper->unlock(std::move(bytes), [this, apply, self = QPointer<SettingsDialog>(this)](bool ok) {
        if (!self) {
            return;
        }
        m_busy = false;
        if (ok) {
            apply();
        } else {
            updatePasswordPage();
            m_notify(this, tr("Wrong master password."));
        }
    });
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
        setNote(m_shortcutStatus, tr("Opens the popup: %1").arg(m_hotkey));
    } else {
        setNote(m_shortcutStatus, tr("%1 can't be used: use Ctrl, Alt, Shift or Super with a letter, digit, "
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
