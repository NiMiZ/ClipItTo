#include "SettingsDialog.h"

#include "GroupPath.h"
#include "Hotkey.h"
#include "PasswordKeeper.h"
#include "PlatformKeys.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCheckBox>
#include <QDate>
#include <QDialog>
#include <QFileDialog>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
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

// On a Mac, Qt's "Ctrl" is the ⌘ key and its "Meta" is ⌃, while the config names the keys as
// they are labelled (Ctrl, Alt, Shift, Cmd): translate between the two spellings.
QString swapMacModifiers(const QString& shortcut, bool toQt)
{
#ifdef Q_OS_MACOS
    QStringList parts = shortcut.split(QLatin1Char('+'));
    for (int i = 0; i + 1 < parts.size(); ++i) { // the last part is the key
        const QString mod = parts[i].trimmed().toLower();
        if (toQt && (mod == QLatin1String("ctrl") || mod == QLatin1String("control"))) {
            parts[i] = QStringLiteral("Meta");
        } else if (toQt && (mod == QLatin1String("cmd") || mod == QLatin1String("command") || mod == QLatin1String("win")
                            || mod == QLatin1String("super") || mod == QLatin1String("meta"))) {
            parts[i] = QStringLiteral("Ctrl");
        } else if (!toQt && mod == QLatin1String("ctrl")) {
            parts[i] = QStringLiteral("Cmd");
        } else if (!toQt && mod == QLatin1String("meta")) {
            parts[i] = QStringLiteral("Ctrl");
        }
    }
    return parts.join(QLatin1Char('+'));
#else
    Q_UNUSED(toQt)
    return shortcut;
#endif
}

QKeySequence sequenceFor(const QString& hotkey)
{
    return QKeySequence::fromString(swapMacModifiers(hotkey, /*toQt=*/true), QKeySequence::PortableText);
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
                               EncryptionSupport encryption, QWidget* parent, PasswordSupport passwords,
                               StartupSupport startup)
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
    , m_startupSupport(std::move(startup))
    , m_startup(new QCheckBox(tr("Start ClipItTo when I sign in"), this))
    , m_paste(new QCheckBox(tr("Paste it into the window I came from"), this))
    , m_latest(new QCheckBox(tr("Make it the latest copy in the history"), this))
    , m_ignoreCase(new QCheckBox(tr("Ignore letter case (ABC = abc)"), this))
    , m_detectSecrets(new QCheckBox(tr("Keep copies that look like secrets as password copies"), this))
    , m_passwordsPerNote(new QCheckBox(tr("Keep the same password separately when its note differs"), this))
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
    , m_passwordsInRecent(new QCheckBox(tr("Also show them in Recent"), this))
    , m_askNote(new QCheckBox(tr("After copying a password, ask for a name"), this))
    , m_unlockMinutes(new QSpinBox(this))
    , m_clearSeconds(new QSpinBox(this))
    , m_hello(new QCheckBox(tr("Unlock with Windows Hello too"), this))
    , m_keyword(new QLineEdit(this))
    , m_masterStatus(new QLabel(this))
    , m_setMaster(new QPushButton(this))
    , m_removeMaster(new QPushButton(tr("Remove…"), this))
    , m_resetMaster(new QPushButton(tr("Forgot it…"), this))
    , m_useOpen(new QPushButton(tr("Use without one…"), this))
    , m_openWarning(new QLabel(this))
    , m_backupCopies(new QCheckBox(tr("Copies, groups and passwords"), this))
    , m_backupSettings(new QCheckBox(tr("Settings"), this))
    , m_export(new QPushButton(tr("Export…"), this))
    , m_import(new QPushButton(tr("Import…"), this))
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
    m_ignoreCase->setChecked(config.ignoreCase);
    m_ignoreCase->setToolTip(tr("Texts that differ only in capital letters are one copy. The wording you copied "
                                "last is the one kept; turning this on merges the copies already there."));
    form->addRow(QString(), m_ignoreCase);

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
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        // A choice that silently does nothing is worse than none: say so where it is chosen.
        auto* waylandNote = new QLabel(tr("<b>Wayland:</b> applications cannot place their own windows, so on most "
                                          "desktops the desktop decides where the popup opens, whatever is chosen "
                                          "here. On GNOME the ClipItTo extension places it (log out and back in "
                                          "after an update). X11, Windows and macOS need nothing extra."),
                                       this);
        waylandNote->setWordWrap(true);
        waylandNote->setTextFormat(Qt::RichText);
        form->addRow(QString(), waylandNote);
    }

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
    if (m_startupSupport.available) {
        QSignalBlocker block(m_startup);
        m_startup->setChecked(m_startupSupport.enabled);
        form->addRow(tr("Startup:"), m_startup);
        connect(m_startup, &QCheckBox::toggled, this, [this](bool on) {
            QString error;
            if (!m_startupSupport.set || !m_startupSupport.set(on, &error)) {
                QSignalBlocker revert(m_startup);
                m_startup->setChecked(!on); // it didn't work: show what is true
                m_notify(this, tr("Could not change the startup setting: %1").arg(error));
            }
        });
    } else {
        m_startup->hide();
    }
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
    auto* hint = new QLabel(platformKeys(tr("<b>Changes to groups are saved immediately.</b> A copy can be put in a "
                                            "group from the popup (Ctrl+G or right-click). Move up/down sets the order "
                                            "in the popup (Alt+↑/↓ there); Favorites always comes first.")),
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
    masterRow->addWidget(m_resetMaster);
    masterRow->addWidget(m_useOpen);
    const QString hello = m_passwordSupport.helloName.isEmpty() ? tr("Windows Hello") : m_passwordSupport.helloName;
    m_hello->setText(tr("Unlock with %1 too").arg(hello));
    m_resetMaster->setToolTip(tr("Replace the master password after unlocking with %1. "
                                 "Your password copies are kept.").arg(hello));
    for (QPushButton* b : {m_setMaster, m_removeMaster, m_resetMaster, m_useOpen}) {
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
    m_openWarning->setWordWrap(true);
    m_openWarning->setTextFormat(Qt::RichText);
    m_openWarning->setText(tr("<b style=\"color:#d03b3b\">Not secure.</b> Without a master password, password "
                              "copies are only hidden: anyone who can read your history file, or use your account, "
                              "can read them. Set a master password to protect them."));
    passwordsForm->addRow(QString(), m_openWarning);
    m_keepPasswords->setChecked(config.keepPasswords);
    passwordsForm->addRow(tr("Password copies:"), m_keepPasswords);
    m_passwordsInGroup->setChecked(config.passwordsInGroup);
    passwordsForm->addRow(QString(), m_passwordsInGroup);
    m_passwordsInRecent->setChecked(!m_repo.groupHiddenFromRecent(PasswordKeeper::passwordsGroup()));
    // A paragraph makes Qt treat a tooltip as rich text, which wraps instead of one long line.
    m_passwordsInRecent->setToolTip(QStringLiteral("<p>%1</p>")
                                        .arg(tr("Password copies in the Passwords group also appear in the Recent list, "
                                                "shown as asterisks (●●●●●●●●), never as text. Off: they appear only in "
                                                "the group and in the %1 search.")
                                                 .arg(config.passwordSearchKeyword.toHtmlEscaped())));
    passwordsForm->addRow(QString(), m_passwordsInRecent);
    connect(m_passwordsInGroup, &QCheckBox::toggled, this, &SettingsDialog::updatePasswordDependents);
    m_detectSecrets->setChecked(config.detectSecrets);
    m_detectSecrets->setToolTip(QStringLiteral("<p>%1</p>")
                                    .arg(tr("A copy that looks like a private key, an API key or access token (AWS, "
                                            "GitHub, GitLab, Slack, Google, Stripe, Anthropic, OpenAI, npm, a JSON "
                                            "web token) or a card number is kept encrypted, like a password a "
                                            "password manager copied, instead of as plain text. It guesses from "
                                            "the shape of the text, so now and then it takes an ordinary copy "
                                            "(use \"Not a password\" in the right-click menu to undo that).")));
    passwordsForm->addRow(QString(), m_detectSecrets);
    m_passwordsPerNote->setChecked(config.passwordsPerNote);
    m_passwordsPerNote->setToolTip(QStringLiteral("<p>%1</p>")
                                       .arg(tr("Off: copying a password you already have only moves it to the top, "
                                               "whatever its note. On: the same password with a different note "
                                               "(for example copied from another app, or under another name) is "
                                               "kept as a separate copy; the same password with the same note is "
                                               "still one copy.")));
    passwordsForm->addRow(QString(), m_passwordsPerNote);
    m_askNote->setChecked(config.passwordAskNote);
    m_askNote->setToolTip(QStringLiteral("<p>%1</p>")
                              .arg(tr("Right after you copy a new password, ClipItTo opens with a box to name it. The "
                                      "app it was copied from is suggested (grey text that goes away when you type); "
                                      "leave it empty to keep that name. Copying the same password again doesn't ask.")));
    passwordsForm->addRow(QString(), m_askNote);
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
    QString helloText = tr("Your face, fingerprint or PIN (%1) unlocks the password copies; the "
                           "master password keeps working too. Turning it on asks for the master password once.").arg(hello)
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
        if (m_passwordSupport.keeper && m_passwordSupport.keeper->isSetUp() && !m_passwordSupport.keeper->isOpen()) {
            changeMasterPassword();
        } else {
            setMasterPassword();
        }
    });
    connect(m_removeMaster, &QPushButton::clicked, this, &SettingsDialog::removeMasterPassword);
    connect(m_useOpen, &QPushButton::clicked, this, &SettingsDialog::useWithoutMasterPassword);
    connect(m_resetMaster, &QPushButton::clicked, this, &SettingsDialog::resetMasterPassword);
    connect(m_hello, &QCheckBox::toggled, this, &SettingsDialog::onHelloToggled);
    updatePasswordPage();

    // --- Backup ------------------------------------------------------------------------
    auto* backupPage = new QWidget(this);
    auto* backupLayout = new QVBoxLayout(backupPage);
    auto* backupText = new QLabel(
        tr("Export your copies and/or settings to a file, or import them from one (on this or another "
           "computer). Importing adds to what is here: copies that are already in the history are left as "
           "they are.<br><br>"
           "<b>Passwords</b> are encrypted in the file with your master password, so the file opens on any "
           "computer, with that password. Everything else in the file is <b>plain text</b>: keep it private."),
        backupPage);
    backupText->setWordWrap(true);
    backupText->setTextFormat(Qt::RichText);
    backupLayout->addWidget(backupText);
    m_backupCopies->setChecked(true);
    m_backupSettings->setChecked(true);
    m_backupCopies->setToolTip(tr("The history, with notes, stars, pins, groups, pictures and password copies."));
    m_backupSettings->setToolTip(tr("The settings of the General and Passwords tabs. The shortcut, backend, "
                                    "history encryption and Windows Hello / Touch ID stay as they are on this computer."));
    backupLayout->addWidget(new QLabel(tr("Export includes:"), backupPage));
    backupLayout->addWidget(m_backupCopies);
    backupLayout->addWidget(m_backupSettings);
    auto* backupButtons = new QHBoxLayout;
    backupButtons->addWidget(m_export);
    backupButtons->addWidget(m_import);
    backupButtons->addStretch();
    backupLayout->addLayout(backupButtons);
    backupLayout->addStretch();
    connect(m_export, &QPushButton::clicked, this, &SettingsDialog::exportBackup);
    connect(m_import, &QPushButton::clicked, this, &SettingsDialog::importBackup);

    m_filePrompt = [](QWidget* parent, bool save, const QString& suggestedName) {
        const QString filter = tr("ClipItTo backup (*.json)");
        return save ? QFileDialog::getSaveFileName(parent, tr("Export"), suggestedName, filter)
                    : QFileDialog::getOpenFileName(parent, tr("Import"), QString(), filter);
    };
    m_partsPrompt = [](QWidget* parent, const backup::Info& info) -> std::optional<backup::Parts> {
        QDialog dialog(parent);
        dialog.setWindowTitle(tr("Import"));
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(tr("This backup has both. What do you want to import?"), &dialog));
        auto* copies = new QCheckBox(tr("Copies (%1, with %2 password copies)").arg(info.copies).arg(info.passwords), &dialog);
        auto* settings = new QCheckBox(tr("Settings"), &dialog);
        copies->setChecked(true);
        settings->setChecked(true);
        layout->addWidget(copies);
        layout->addWidget(settings);
        auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(box);
        if (dialog.exec() != QDialog::Accepted) {
            return std::nullopt;
        }
        return backup::Parts{copies->isChecked(), settings->isChecked()};
    };

    // --- Dialog ------------------------------------------------------------------------
    m_tabs->addTab(general, tr("General"));
    m_tabs->addTab(groupsPage, tr("Groups"));
    m_tabs->addTab(passwordsPage, tr("Passwords"));
    m_tabs->addTab(backupPage, tr("Backup"));
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
    if (m_imported) {
        return *m_imported;
    }
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
    c.ignoreCase = m_ignoreCase->isChecked();
    c.detectSecrets = m_detectSecrets->isChecked();
    c.passwordsPerNote = m_passwordsPerNote->isChecked();
    c.keepPasswords = m_keepPasswords->isChecked();
    c.passwordsInGroup = m_passwordsInGroup->isChecked();
    c.passwordAskNote = m_askNote->isChecked();
    c.passwordUnlockMinutes = m_unlockMinutes->value();
    c.passwordClearSeconds = m_clearSeconds->value();
    c.windowsHello = m_passwordSupport.keeper && m_passwordSupport.keeper->helloEnabled();
    if (m_keyword->hasAcceptableInput()) {
        c.passwordSearchKeyword = m_keyword->text();
    }
    return c;
}

void SettingsDialog::withMasterPassword(const QString& purpose, std::function<void(QByteArray password)> proceed)
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || !keeper->isSetUp() || keeper->isOpen()) {
        proceed({});
        return;
    }
    std::optional<QString> typed = m_passwordPrompt(this, tr("Master password (needed to %1):").arg(purpose));
    if (!typed) {
        return;
    }
    QByteArray bytes = PasswordVault::passwordBytes(*typed);
    QByteArray copy = bytes;
    m_busy = true;
    updatePasswordPage();
    keeper->unlock(std::move(bytes), [this, proceed = std::move(proceed), copy = std::move(copy),
                                       self = QPointer<SettingsDialog>(this)](bool ok) mutable {
        if (!self) {
            PasswordVault::wipe(copy);
            return;
        }
        m_busy = false;
        updatePasswordPage();
        if (!ok) {
            PasswordVault::wipe(copy);
            m_notify(this, tr("Wrong master password."));
            return;
        }
        proceed(copy);
        PasswordVault::wipe(copy);
    });
}

void SettingsDialog::exportBackup()
{
    const backup::Parts parts{m_backupCopies->isChecked(), m_backupSettings->isChecked()};
    if (!parts.any()) {
        m_notify(this, tr("Tick what to export: copies, settings, or both."));
        return;
    }
    withMasterPassword(tr("export"), [this, parts](QByteArray password) {
        const QString path = m_filePrompt(this, /*save=*/true,
                                          QStringLiteral("clipitto-backup-%1.json").arg(QDate::currentDate().toString(Qt::ISODate)));
        if (path.isEmpty()) {
            return;
        }
        QString error;
        backup::PasswordExport mode = backup::PasswordExport::Encrypted;
        PasswordKeeper* keeper = m_passwordSupport.keeper;
        if (keeper && keeper->isOpen() && parts.copies && m_repo.countSecrets() > 0) {
            mode = m_confirm(this, tr("There is no master password, so password copies can only be exported as "
                                      "PLAIN TEXT: anyone who gets the file can read them.\n\nInclude the "
                                      "password copies in clear text? (No: export without them.)"))
                ? backup::PasswordExport::Clear
                : backup::PasswordExport::Omit;
        }
        // The settings to export are the ones in the dialog now.
        if (!backup::exportTo(path, m_repo, config(), keeper, parts, password, &error, mode)) {
            m_notify(this, error);
            return;
        }
        m_inform(this, tr("Exported to %1.").arg(path));
    });
}

void SettingsDialog::importBackup()
{
    withMasterPassword(tr("import"), [this](QByteArray password) {
        const QString path = m_filePrompt(this, /*save=*/false, QString());
        if (path.isEmpty()) {
            return;
        }
        const backup::Info info = backup::inspect(path);
        if (!info.valid) {
            m_notify(this, info.error);
            return;
        }
        finishImport(path, info, password);
    });
}

void SettingsDialog::finishImport(const QString& path, const backup::Info& info, const QByteArray& masterPassword)
{
    backup::Parts parts = info.parts;
    if (info.parts.copies && info.parts.settings) {
        const auto chosen = m_partsPrompt(this, info);
        if (!chosen || !chosen->any()) {
            return;
        }
        parts = *chosen;
    }

    // Password copies: they need a master password here and the backup's password.
    QByteArray backupPassword = masterPassword;
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (parts.copies && info.passwords > 0) {
        if (!keeper || !keeper->isSetUp()) {
            if (!m_confirm(this, tr("This backup has %n password copies, but no master password is set here, so "
                                    "they can't be imported. Import the rest?", nullptr, info.passwords))) {
                return;
            }
        } else if (!info.passwordsClear && (backupPassword.isEmpty() || !backup::passwordOpens(path, backupPassword))) {
            PasswordVault::wipe(backupPassword);
            std::optional<QString> typed =
                m_passwordPrompt(this, tr("The backup's password copies use another master password. Enter it:"));
            if (typed) {
                backupPassword = PasswordVault::passwordBytes(*typed);
            }
            if (!typed || !backup::passwordOpens(path, backupPassword)) {
                PasswordVault::wipe(backupPassword);
                if (!m_confirm(this, tr("The password copies can't be opened with that. Import the rest without them?"))) {
                    return;
                }
            }
        }
    }

    QString error;
    const auto report = backup::importFrom(path, parts, m_repo, keeper, backupPassword, &error);
    PasswordVault::wipe(backupPassword);
    if (!report) {
        m_notify(this, error);
        return;
    }
    QStringList lines;
    if (parts.copies) {
        m_groupsChanged = true;
        lines << tr("%n copies added", nullptr, report->added)
              << tr("%n already there", nullptr, report->alreadyThere);
        if (report->passwordsAdded > 0) {
            lines << tr("%n password copies added", nullptr, report->passwordsAdded);
        }
        if (report->passwordsSkipped > 0) {
            lines << tr("%n password copies skipped", nullptr, report->passwordsSkipped);
        }
        reloadGroups();
        // The Passwords group's rule may have come with the copies.
        m_passwordsInRecent->setChecked(!m_repo.groupHiddenFromRecent(PasswordKeeper::passwordsGroup()));
    }
    if (report->settings) {
        // What belongs to this computer stays as it is here.
        Config merged = *report->settings;
        const Config mine = config();
        merged.hotkey = mine.hotkey;
        merged.backend = mine.backend;
        merged.encryptHistory = mine.encryptHistory;
        merged.windowsHello = mine.windowsHello;
        m_imported = merged;
        lines << tr("settings imported");
    }
    m_inform(this, tr("Imported: %1.").arg(lines.join(tr(", "))));
    if (m_imported) {
        accept(); // the settings apply now
    }
}

void SettingsDialog::accept()
{
    if (m_passwordsInRecent->isEnabled()) {
        try {
            m_repo.setGroupHiddenFromRecent(PasswordKeeper::passwordsGroup(), !m_passwordsInRecent->isChecked());
        } catch (const db::DbError& err) {
            qWarning() << "saving the Recent setting failed:" << err.what();
        }
    }
    QDialog::accept();
}

void SettingsDialog::updatePasswordDependents()
{
    // Only a password copy that is in the Passwords group can be kept out of Recent.
    m_passwordsInRecent->setEnabled(m_passwordsInGroup->isEnabled() && m_passwordsInGroup->isChecked());
}

void SettingsDialog::updatePasswordPage()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    const bool setUp = keeper && keeper->isSetUp();
    const bool open = setUp && keeper->isOpen();
    if (!keeper) {
        m_masterStatus->setText(tr("Not available here."));
    } else if (!setUp) {
        m_masterStatus->setText(tr("Not set: password copies are not kept."));
    } else if (open) {
        qint64 count = 0;
        try {
            count = m_repo.countSecrets();
        } catch (const db::DbError&) {
        }
        m_masterStatus->setText(tr("None (not secure). %n password copies kept.", nullptr, int(count)));
    } else {
        qint64 count = 0;
        try {
            count = m_repo.countSecrets();
        } catch (const db::DbError&) {
        }
        m_masterStatus->setText(tr("Set. %n password copies kept.", nullptr, int(count)));
    }
    m_setMaster->setText(setUp && !open ? tr("Change…") : tr("Set…"));
    m_setMaster->setEnabled(keeper && !m_busy);
    m_removeMaster->setText(open ? tr("Turn off…") : tr("Remove…"));
    m_removeMaster->setEnabled(setUp && !m_busy);
    m_resetMaster->setVisible(setUp && !open && keeper->helloEnabled() && m_passwordSupport.helloUnlock);
    m_resetMaster->setEnabled(!m_busy);
    m_useOpen->setVisible(!setUp);
    m_useOpen->setEnabled(keeper && !m_busy);
    m_openWarning->setVisible(open);
    m_askNote->setEnabled(setUp);
    m_detectSecrets->setEnabled(setUp);
    m_passwordsPerNote->setEnabled(setUp);
    for (QWidget* w : {static_cast<QWidget*>(m_keepPasswords), static_cast<QWidget*>(m_passwordsInGroup),
                       static_cast<QWidget*>(m_unlockMinutes), static_cast<QWidget*>(m_clearSeconds),
                       static_cast<QWidget*>(m_keyword)}) {
        w->setEnabled(setUp);
    }
    updatePasswordDependents();
    const QSignalBlocker block(m_hello);
    m_hello->setChecked(setUp && keeper->helloEnabled());
    m_hello->setEnabled(setUp && !open && m_passwordSupport.helloAvailable && m_passwordSupport.setHello && !m_busy);
}

void SettingsDialog::useWithoutMasterPassword()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || keeper->isSetUp() || m_busy) {
        return;
    }
    if (!m_confirm(this, tr("NOT SECURE. Without a master password your password copies are only hidden in the "
                            "list. Anyone who can read your history file or use your account can read them, and "
                            "an export can contain them as plain text.\n\nKeep password copies without a master "
                            "password anyway? You can set a master password later."))) {
        return;
    }
    m_busy = true;
    updatePasswordPage();
    keeper->useWithoutMasterPassword([this, self = QPointer<SettingsDialog>(this)](bool ok) {
        if (!self) {
            return;
        }
        m_busy = false;
        updatePasswordPage();
        if (!ok) {
            m_notify(this, tr("Could not turn this on."));
        }
    });
}

QString SettingsDialog::helloLabel() const
{
    return m_passwordSupport.helloName.isEmpty() ? tr("Windows Hello") : m_passwordSupport.helloName;
}

void SettingsDialog::resetMasterPassword()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || !keeper->isSetUp() || keeper->isOpen() || !keeper->helloEnabled() || !m_passwordSupport.helloUnlock
        || m_busy) {
        return;
    }
    m_busy = true;
    updatePasswordPage();
    m_passwordSupport.helloUnlock([this, keeper, self = QPointer<SettingsDialog>(this)](bool unlocked) {
        if (!self) {
            return;
        }
        m_busy = false;
        updatePasswordPage();
        if (!unlocked) {
            m_notify(this, tr("%1 did not unlock the passwords.").arg(helloLabel()));
            return;
        }
        std::optional<MasterPasswordAnswer> answer = m_masterPrompt(this, /*change=*/false);
        if (!answer) {
            return;
        }
        QByteArray chosen = PasswordVault::passwordBytes(answer->chosen);
        m_busy = true;
        updatePasswordPage();
        keeper->resetMasterPassword(std::move(chosen), [this, self](bool ok) {
            if (!self) {
                return;
            }
            m_busy = false;
            updatePasswordPage();
            if (ok) {
                m_inform(this, tr("Master password replaced. Your password copies are kept; turn %1 on "
                                  "again to use it with the new one.").arg(helloLabel()));
            } else {
                m_notify(this, tr("Could not replace the master password."));
            }
        });
    });
}

void SettingsDialog::setMasterPassword()
{
    PasswordKeeper* keeper = m_passwordSupport.keeper;
    if (!keeper || (keeper->isSetUp() && !keeper->isOpen()) || m_busy) {
        return;
    }
    const bool fromOpen = keeper->isOpen();
    std::optional<MasterPasswordAnswer> answer = m_masterPrompt(this, /*change=*/false);
    if (!answer) {
        return;
    }
    QByteArray chosen = PasswordVault::passwordBytes(answer->chosen);
    m_busy = true;
    updatePasswordPage();
    const auto finished = [this, self = QPointer<SettingsDialog>(this)](bool ok) {
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
    };
    if (fromOpen) { // the existing password copies move to the new password
        keeper->changeMasterPassword(PasswordKeeper::openPassword(), std::move(chosen), finished);
    } else {
        keeper->setMasterPassword(std::move(chosen), finished);
    }
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
                               .arg(m_passwordSupport.helloAvailable ? tr("; turn %1 on again to use it").arg(helloLabel())
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
    const QString question = keeper->isOpen()
        ? tr("Stop keeping password copies and delete all %n of them? This can't be undone.", nullptr, int(count))
        : tr("Remove the master password and delete all %n password copies? This can't be undone.", nullptr, int(count));
    if (!m_confirm(this, question)) {
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
                m_notify(this, tr("%1: %2").arg(helloLabel(), error));
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
    const auto parsed = parseHotkey(swapMacModifiers(first.toString(QKeySequence::PortableText), /*toQt=*/false));
    if (parsed) {
        m_hotkey = parsed->canonical;
        setNote(m_shortcutStatus, tr("Opens the popup: %1").arg(m_hotkey));
    } else {
#ifdef Q_OS_MACOS
        const QString modifiers = tr("Control, Option, Shift or Command");
#else
        const QString modifiers = tr("Ctrl, Alt, Shift or Super");
#endif
        setNote(m_shortcutStatus, tr("%1 can't be used: use %2 with a letter, digit, "
                                     "F-key or ` - = [ ] ; ' , . /. Keeping %3.")
                                      .arg(first.toString(QKeySequence::NativeText), modifiers, m_hotkey));
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
