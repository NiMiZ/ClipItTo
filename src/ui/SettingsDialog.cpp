#include "SettingsDialog.h"

#include "Paths.h"

#include "GroupPath.h"
#include "Hotkey.h"
#include "PasswordKeeper.h"
#include "AutoForget.h"
#include "PasswordRules.h"
#include "PasteKeys.h"
#include "PlatformKeys.h"
#include "TextTransform.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <algorithm>

#include <QCheckBox>
#include <QDir>
#include <QDate>
#include <QDialog>
#include <QFileDialog>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTabBar>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
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
                               StartupSupport startup, TranslationSupport translation, SyncSupport sync)
    : QDialog(parent)
    , m_config(config)
    , m_repo(repo)
    , m_hotkey(config.hotkey)
    , m_theme(new QComboBox(this))
    , m_copies(new QSpinBox(this))
    , m_kept(new QSpinBox(this))
    , m_images(new QSpinBox(this))
    , m_ocrLanguages(new QLineEdit(this))
    , m_indexPictures(new QCheckBox(tr("Search inside pictures"), this))
    , m_keepFormatting(new QCheckBox(tr("Keep formatting of copied text"), this))
    , m_debugLog(new QCheckBox(tr("Write a diagnostic log (for troubleshooting)"), this))
    , m_lines(new QSpinBox(this))
    , m_favorites(new QSpinBox(this))
    , m_position(new QComboBox(this))
    , m_startFocus(new QComboBox(this))
    , m_encrypt(new QCheckBox(tr("Encrypt the history on disk"), this))
    , m_startupSupport(std::move(startup))
    , m_startup(new QCheckBox(tr("Start ClipItTo when I sign in"), this))
    , m_paste(new QCheckBox(tr("Paste it into the window I came from"), this))
    , m_latest(new QCheckBox(tr("Make it the latest copy in the history"), this))
    , m_pasteKeysPerApp(new QCheckBox(tr("Choose the paste keys by the program (Ctrl+Shift+V in terminals)"), this))
    , m_ignoreCase(new QCheckBox(tr("Ignore letter case (ABC = abc)"), this))
    , m_detectSecrets(new QCheckBox(tr("Keep copies that look like secrets as password copies"), this))
    , m_detectPasswords(new QCheckBox(tr("Keep copies that look like generated passwords as password copies"), this))
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
    , m_passwordsInRecent(new QCheckBox(tr("Show passwords entries in Recent"), this))
    , m_askNote(new QCheckBox(tr("After copying a password, ask for a name"), this))
    , m_lockOnScreenLock(new QCheckBox(tr("Lock the passwords when the screen locks or the PC sleeps"), this))
    , m_unlockMinutes(new QSpinBox(this))
    , m_clearSeconds(new QSpinBox(this))
    , m_pasteOnceClearSeconds(new QSpinBox(this))
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
    m_syncSupport = std::move(sync);
    m_syncPrompt = defaultSyncPassphrasePrompt();
    m_folderPrompt = [](QWidget* parent, const QString& current) {
        return QFileDialog::getExistingDirectory(parent, tr("Sync folder"), current);
    };
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

    m_ocrLanguages->setText(config.ocrLanguages);
    m_ocrLanguages->setPlaceholderText(tr("automatic"));
    m_ocrLanguages->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9_,+ -]{0,64}")), m_ocrLanguages));
    m_ocrLanguages->setToolTip(QStringLiteral("<p>%1</p>")
                                   .arg(tr("Languages that \"Paste text from picture\" reads, like <b>en, he</b> "
                                           "(empty: automatic). Windows reads the languages installed for your account "
                                           "(Settings › Time & language) and uses the first of these it has; macOS "
                                           "detects the language by itself when this is empty; on Linux it is English "
                                           "plus your locale's language, and the language data has to be installed "
                                           "(for Hebrew: tesseract-ocr-heb).")));
    form->addRow(tr("Read pictures in:"), m_ocrLanguages);
    m_indexPictures->setChecked(config.indexPictures);
    m_indexPictures->setToolTip(QStringLiteral("<p>%1</p>")
                                    .arg(tr("Read the text in every copied picture (on this computer, in the background) "
                                            "and keep it with the picture, so typing a word finds the screenshot that "
                                            "shows it. Turning this off deletes the text that was read; the pictures "
                                            "stay. It is stored in the history file, so it is encrypted with it.")));
    form->addRow(QString(), m_indexPictures);
    m_keepFormatting->setChecked(config.keepFormatting);
    m_keepFormatting->setToolTip(QStringLiteral("<p>%1</p>")
                                     .arg(tr("When a copy has formatting (bold, links, lists, tables…), also keep its "
                                             "formatted version, so Shift+Enter › <b>With formatting</b> pastes it "
                                             "as it was. Plain text stays the default. Turning this off deletes the "
                                             "formatted versions kept so far; the text stays. They are stored in the "
                                             "history file, so they are encrypted with it. Copies marked by a "
                                             "password manager never keep any.")));
    form->addRow(QString(), m_keepFormatting);
    m_debugLog->setChecked(config.debugLog);
    m_debugLog->setToolTip(
        QStringLiteral("<p>%1</p><p>%2</p>")
            .arg(tr("When something does not work (a missing name box, a copy that was not kept), turn this on, repeat it, and "
                    "send the file. ClipItTo writes what it does, with the time, to <b>%1</b> (and an older part next to it). "
                    "Turning this off stops the log and deletes the files; quitting ClipItTo leaves them.")
                     .arg(QDir::toNativeSeparators(paths::debugLogFile()).toHtmlEscaped()),
                 tr("It does not contain what you copied (text, pictures, notes, group names). It does contain file paths "
                    "(with your user name), error messages and the times things happened, so look at it before you send it.")));
    form->addRow(QString(), m_debugLog);
    m_selfTestButton = new QPushButton(tr("Check this installation…"), this);
    m_selfTestButton->setObjectName(QStringLiteral("selfTestButton"));
    m_selfTestButton->setToolTip(QStringLiteral("<p>%1</p>")
                                     .arg(tr("When something does not work, this tells you what works and what does not: whether "
                                             "copies are seen, the shortcut is registered, the keyring answers, paste works, and "
                                             "more, each with what to do. It puts a short test text on the clipboard. The "
                                             "report never contains what you copied.")));
    m_selfTestButton->setEnabled(false); // until the daemon gives it the checks (setSelfTest)
    connect(m_selfTestButton, &QPushButton::clicked, this, &SettingsDialog::openSelfTest);
    form->addRow(QString(), m_selfTestButton);
    // Keyboard layouts that Paste as… can convert text to and from (text typed with the wrong layout).
    m_layoutsSetting = config.keyboardLayouts;
    const QStringList chosenLayouts = texttransform::enabledLayouts(config.keyboardLayouts, QLocale::system().bcp47Name());
    auto* layoutRow = new QHBoxLayout;
    for (const texttransform::Layout& layout : texttransform::layouts()) {
        auto* box = new QCheckBox(texttransform::layoutName(layout.id), this);
        box->setProperty("layout", layout.id);
        box->setChecked(chosenLayouts.contains(layout.id));
        m_layoutBoxes.append(box);
        layoutRow->addWidget(box);
    }
    layoutRow->addStretch(1);
    auto* layoutWidget = new QWidget(this);
    layoutWidget->setLayout(layoutRow);
    layoutRow->setContentsMargins(0, 0, 0, 0);
    layoutWidget->setToolTip(QStringLiteral("<p>%1</p>")
                                 .arg(tr("Paste as… (Shift+Enter) can fix text that was typed with the wrong keyboard "
                                         "layout: copy <b>xccv</b> and paste it as the Hebrew keyboard, <b>סבבה</b>. "
                                         "Tick the languages you type in. With one, the menu offers the opposite of the "
                                         "text's language (English ⇄ that language); with several it offers a submenu "
                                         "of them. Empty: the language of your system.")));
    form->addRow(tr("Keyboard layouts for Paste as…:"), layoutWidget);

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

    // Paste keys per program (roadmap 8): terminals do not paste with Ctrl+V.
    m_pasteKeysPerApp->setChecked(config.pasteKeysPerApp);
    m_pasteKeysPerApp->setToolTip(
        tr("ClipItTo types the paste keys into the program you came from. Well-known terminals get Ctrl+Shift+V "
           "(Shift+Insert in Windows consoles); every other program gets the keys of paste_keys in the config file "
           "(Ctrl+V). Your own rules below come first."));
    form->addRow(QString(), m_pasteKeysPerApp);
    m_pasteKeysPanel = new QWidget(this);
    auto* pasteKeysOuter = new QVBoxLayout(m_pasteKeysPanel);
    pasteKeysOuter->setContentsMargins(0, 0, 0, 0);
    auto* pasteKeysIntro = new QLabel(
        tr("Your own rules, tried before the built-in terminals. The program is its executable name (Windows), "
           "window class (X11) or app id; letter case does not matter, <b>*</b> matches any text. "
           "<b>Works on:</b> Windows, X11 and GNOME Wayland with the ClipItTo extension (log out and back in once "
           "after updating it). <b>Does not work on:</b> macOS (always ⌘V) and other Wayland desktops: there "
           "the keys of paste_keys are used for every program."),
        m_pasteKeysPanel);
    pasteKeysIntro->setWordWrap(true);
    pasteKeysIntro->setTextFormat(Qt::RichText);
    pasteKeysOuter->addWidget(pasteKeysIntro);
    m_pasteKeysLayout = new QVBoxLayout;
    pasteKeysOuter->addLayout(m_pasteKeysLayout);
    m_addPasteKeys = new QPushButton(tr("Add a program"), m_pasteKeysPanel);
    m_addPasteKeys->setAutoDefault(false);
    m_addPasteKeys->setToolTip(tr("Up to %1 programs").arg(pastekeys::kMaxRules));
    connect(m_addPasteKeys, &QPushButton::clicked, this, [this] {
        addPasteKeysRow(PasteKeyRule());
        refreshPasteKeys();
    });
    auto* pasteKeysAdd = new QHBoxLayout;
    pasteKeysAdd->addWidget(m_addPasteKeys);
    pasteKeysAdd->addStretch(1);
    pasteKeysOuter->addLayout(pasteKeysAdd);
    m_pasteKeysError = new QLabel(m_pasteKeysPanel);
    m_pasteKeysError->setWordWrap(true);
    m_pasteKeysError->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_pasteKeysError->hide();
    pasteKeysOuter->addWidget(m_pasteKeysError);
    for (const PasteKeyRule& rule : config.pasteKeysApps) {
        addPasteKeysRow(rule);
    }
    m_pasteKeysPanel->setEnabled(config.pasteKeysPerApp);
    connect(m_pasteKeysPerApp, &QCheckBox::toggled, m_pasteKeysPanel, &QWidget::setEnabled);
    connect(m_pasteKeysPerApp, &QCheckBox::toggled, this, [this] { updateOkButton(); });
    m_pasteKeysWindow = addRulesWindow(form, tr("Programs:"), tr("Paste keys per program"), m_pasteKeysPanel,
                                       tr("Edit programs…"));
    m_pasteKeysWindow.button->setEnabled(config.pasteKeysPerApp);
    connect(m_pasteKeysPerApp, &QCheckBox::toggled, m_pasteKeysWindow.button, &QWidget::setEnabled);
    refreshPasteKeys();

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
                                "the top of Recent, saved or not."));
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
    m_groups->setColumnCount(3);
    m_groups->setHeaderLabels({tr("Group"), tr("Copies"), tr("Examples")});
    m_groups->header()->setStretchLastSection(false);
    m_groups->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_groups->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_groups->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
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
    m_autoGroup = new QCheckBox(tr("File new copies in a matching group automatically"), this);
    m_autoGroup->setChecked(config.autoGroup);
    m_autoGroup->setToolTip(QStringLiteral("<p>%1</p>")
                                .arg(tr("ClipItTo learns from the copies you put in your groups (words, a link's site, a "
                                        "path's folder, a command's first word) and files a new copy in the group it "
                                        "matches best, marked <b>auto</b>. It never makes a group, and a copy with no "
                                        "clear match stays in Recent. Moving an auto-filed copy elsewhere, or taking it "
                                        "out of its group, teaches it the other way. Never for passwords or pictures. "
                                        "Everything learned stays in the history file (encrypted with it when history encryption is on). Turning this "
                                        "off keeps the copies already filed.")));
    m_forgetLearned = new QPushButton(tr("Forget what was learned"), this);
    m_forgetLearned->setAutoDefault(false);
    auto* autoRow = new QHBoxLayout;
    autoRow->addWidget(m_autoGroup, 1);
    autoRow->addWidget(m_forgetLearned);
    groupsLayout->addLayout(autoRow);
    connect(m_forgetLearned, &QPushButton::clicked, this, [this] {
        if (!m_confirm(this, tr("Forget everything learned from your groups? Copies already filed stay where they are."))) {
            return;
        }
        try {
            m_repo.forgetAutoLearned();
        } catch (const db::DbError& e) {
            m_notify(this, QString::fromUtf8(e.what()));
        }
        reloadGroups(selectedGroup());
    });
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
    // Until the Passwords group exists its rule is the default: kept out of Recent.
    m_passwordsInRecent->setChecked(m_repo.groups().contains(PasswordKeeper::passwordsGroup()) &&
                                    !m_repo.groupHiddenFromRecent(PasswordKeeper::passwordsGroup()));
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
    m_detectPasswords->setChecked(config.detectPasswords);
    m_detectPasswords->setToolTip(QStringLiteral("<p>%1</p>")
                                      .arg(tr("A single-line copy of 8 to 64 characters without spaces that mixes at "
                                              "least three of lower case, upper case, digits and symbols (like a "
                                              "password a browser or a generator made) is kept encrypted, like a "
                                              "password a password manager copied. It is only a guess from the "
                                              "shape: some ordinary copies, such as an identifier or a code, will be "
                                              "taken too and shown as ●●●● until you use \"Not a password\" in the "
                                              "right-click menu. Off by default.")));
    passwordsForm->addRow(QString(), m_detectPasswords);

    // Own rules: extend the detection above (a copy that fits any rule is kept as a password copy too).
    m_rulesPanel = new QWidget(this);
    auto* rulesOuter = new QVBoxLayout(m_rulesPanel);
    rulesOuter->setContentsMargins(0, 0, 0, 0);
    auto* rulesIntro = new QLabel(tr("Your own rules: a single-line copy without spaces that fits any rule is also "
                                     "kept as a password copy. Counts are minimums (\"2 digits\" means at least 2)."),
                                  m_rulesPanel);
    rulesIntro->setWordWrap(true);
    rulesOuter->addWidget(rulesIntro);
    m_rulesLayout = new QVBoxLayout;
    rulesOuter->addLayout(m_rulesLayout);
    m_addRule = new QPushButton(tr("Add a rule"), m_rulesPanel);
    m_addRule->setToolTip(tr("Up to %1 rules").arg(passwordrules::kMaxRules));
    connect(m_addRule, &QPushButton::clicked, this, [this] {
        addRuleRow(PasswordRule());
        refreshRules();
    });
    auto* addRow = new QHBoxLayout;
    addRow->addWidget(m_addRule);
    addRow->addStretch(1);
    rulesOuter->addLayout(addRow);
    m_ruleError = new QLabel(m_rulesPanel);
    m_ruleError->setWordWrap(true);
    m_ruleError->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_ruleError->hide();
    rulesOuter->addWidget(m_ruleError);
    auto* tryRow = new QHBoxLayout;
    m_ruleTry = new QLineEdit(m_rulesPanel);
    m_ruleTry->setPlaceholderText(tr("Try it: type a sample to test against the rules"));
    m_ruleTry->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    connect(this, &QDialog::finished, m_ruleTry, &QLineEdit::clear);
    m_ruleTry->setMaxLength(passwordrules::kLengthLimit + 64);
    m_ruleTry->setToolTip(tr("Only tested here against the rules above; it is not saved or logged."));
    tryRow->addWidget(m_ruleTry);
    rulesOuter->addLayout(tryRow);
    m_ruleTryResult = new QLabel(m_rulesPanel);
    m_ruleTryResult->setWordWrap(true);
    rulesOuter->addWidget(m_ruleTryResult);
    connect(m_ruleTry, &QLineEdit::textChanged, this, &SettingsDialog::refreshRules);
    m_rulesWindow = addRulesWindow(passwordsForm, tr("Password rules:"), tr("Password rules"), m_rulesPanel, tr("Edit rules…"));
    for (const PasswordRule& rule : config.passwordRules) {
        addRuleRow(rule);
    }
    refreshRules();
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
    // Lock with the screen (separate block).
    m_lockOnScreenLock->setChecked(config.lockOnScreenLock);
    m_lockOnScreenLock->setToolTip(
        QStringLiteral("<p>%1</p>")
            .arg(tr("Locks the passwords at once when your screen locks or the PC goes to sleep, as Lock now does. "
                    "Works on Windows (session lock, disconnect, sleep), macOS (screen lock, sleep) and on Linux "
                    "through systemd-logind and the GNOME or freedesktop screensaver; on Linux it depends on "
                    "those being there, and ClipItTo can't tell when they are not (Settings and --status say "
                    "when this system gives no such events). It has no effect without a master password.")));
    passwordsForm->addRow(QString(), m_lockOnScreenLock);
    m_clearSeconds->setRange(0, 60);
    m_clearSeconds->setSuffix(tr(" s"));
    m_clearSeconds->setSpecialValueText(tr("Never"));
    m_clearSeconds->setValue(config.passwordClearSeconds);
    m_clearSeconds->setToolTip(tr("A password you paste from ClipItTo is taken off the clipboard this long after "
                                  "the paste, but only if the clipboard still holds that password: something you "
                                  "copied meanwhile stays. Never = it stays until you copy something else."));
    passwordsForm->addRow(tr("Clear a pasted password after:"), m_clearSeconds);
    m_pasteOnceClearSeconds->setRange(0, 60);
    m_pasteOnceClearSeconds->setSuffix(tr(" s"));
    m_pasteOnceClearSeconds->setSpecialValueText(tr("Never"));
    m_pasteOnceClearSeconds->setValue(config.pasteOnceClearSeconds);
    m_pasteOnceClearSeconds->setToolTip(tr("A copy marked Paste once is forgotten right after it is pasted from "
                                           "ClipItTo, and taken off the clipboard this long after the paste, but only "
                                           "if the clipboard still holds it. Never = it stays on the clipboard until "
                                           "you copy something else."));
    passwordsForm->addRow(tr("Clear a Paste once copy after:"), m_pasteOnceClearSeconds);

    // Panic key: off by default. The shortcut wipes everything at once, with no question.
    m_shortcutsEditable = shortcut.editable;
    m_panicHotkey = config.panicHotkey;
    m_panicKey = new QCheckBox(tr("Panic key: wipe everything without asking"), this);
    m_panicKey->setChecked(config.panicKey);
    const QString panicHelp = tr("Pressing the panic shortcut deletes the whole history (also pinned, starred, grouped and "
                                 "password copies, like \"Clear everything, also in groups\"), locks the password copies "
                                 "and empties the system clipboard, at once and WITHOUT asking. It cannot be undone. Your "
                                 "groups, settings and master password stay. Off by default. The command "
                                 "\"clipitto --panic\" does the same and works whatever this box says.");
    m_panicKey->setToolTip(panicHelp);
    passwordsForm->addRow(QString(), m_panicKey);
    auto* panicRow = new QHBoxLayout;
    m_panicShortcut = new QKeySequenceEdit(this);
    m_panicShortcut->setKeySequence(sequenceFor(m_panicHotkey));
    m_panicShortcut->setToolTip(panicHelp);
    auto* panicNone = new QPushButton(tr("None"), this);
    panicNone->setToolTip(tr("Remove the panic shortcut"));
    panicRow->addWidget(m_panicShortcut, 1);
    panicRow->addWidget(panicNone);
    passwordsForm->addRow(tr("Panic shortcut:"), panicRow);
    // The same action on a button (with a question, because a button is easy to press by mistake). It happens at
    // once, not when OK is pressed: like "Clear everything", it is not a setting.
    m_wipeNow = new QPushButton(tr("Wipe everything now…"), this);
    m_wipeNow->setAutoDefault(false);
    m_wipeNow->setEnabled(static_cast<bool>(m_passwordSupport.panicNow));
    m_wipeNow->setToolTip(tr("Deletes the whole history now (also pinned, starred, grouped and password copies), locks the "
                             "password copies and empties the system clipboard. Cannot be undone. Happens at once, "
                             "whether or not you press OK."));
    connect(m_wipeNow, &QPushButton::clicked, this, [this] {
        if (!m_passwordSupport.panicNow
            || !m_confirm(this, tr("Wipe everything now?\n\nThis deletes every copy in the history (also pinned, starred, "
                                   "grouped and password copies), locks the password copies and empties the clipboard. "
                                   "It cannot be undone. Your settings, groups and master password stay."))) {
            return;
        }
        m_passwordSupport.panicNow();
    });
    passwordsForm->addRow(QString(), m_wipeNow);
    m_panicStatus = new QLabel(this);
    m_panicStatus->setWordWrap(true);
    m_panicStatus->setEnabled(false); // dim
    passwordsForm->addRow(QString(), m_panicStatus);
    const auto syncPanic = [this] {
        const bool on = m_panicKey->isChecked() && m_shortcutsEditable;
        m_panicShortcut->setEnabled(on);
        if (!m_shortcutsEditable) {
            setNote(m_panicStatus, tr("Bind \"clipitto --panic\" to a shortcut in your desktop's keyboard settings."));
        } else if (m_panicKey->isChecked() && m_panicHotkey.isEmpty()) {
            setNote(m_panicStatus, tr("Panic key is on, but no shortcut is set: choose one."));
        } else if (m_panicKey->isChecked()) {
            setNote(m_panicStatus, tr("Wipes everything without asking: %1").arg(m_panicHotkey));
        } else {
            setNote(m_panicStatus, QString());
        }
    };
    connect(m_panicKey, &QCheckBox::toggled, this, syncPanic);
    connect(panicNone, &QPushButton::clicked, this, [this, syncPanic] {
        m_panicHotkey.clear();
        QSignalBlocker block(m_panicShortcut);
        m_panicShortcut->clear();
        syncPanic();
    });
    connect(m_panicShortcut, &QKeySequenceEdit::keySequenceChanged, this, [this] { onPanicShortcutEdited(); });
    panicNone->setEnabled(m_shortcutsEditable);
    syncPanic();

    // Auto-forget: copies that remove themselves. Pinned, starred, grouped and password copies are never touched.
    m_forgetPanel = new QWidget(this);
    auto* forgetOuter = new QVBoxLayout(m_forgetPanel);
    forgetOuter->setContentsMargins(0, 0, 0, 0);
    auto* forgetIntro = new QLabel(tr("Copies that remove themselves, quietly. Pinned, starred, grouped and password "
                                      "copies are never removed. With no rules nothing is removed."),
                                   m_forgetPanel);
    forgetIntro->setWordWrap(true);
    forgetOuter->addWidget(forgetIntro);
    m_forgetLayout = new QVBoxLayout;
    forgetOuter->addLayout(m_forgetLayout);
    m_addForget = new QPushButton(tr("Add a rule"), m_forgetPanel);
    m_addForget->setToolTip(tr("Up to %1 rules").arg(autoforget::kMaxRules));
    connect(m_addForget, &QPushButton::clicked, this, [this] {
        addForgetRow(AutoForgetRule());
        refreshForget();
    });
    auto* forgetAdd = new QHBoxLayout;
    forgetAdd->addWidget(m_addForget);
    forgetAdd->addStretch(1);
    forgetOuter->addLayout(forgetAdd);
    m_forgetError = new QLabel(m_forgetPanel);
    m_forgetError->setWordWrap(true);
    m_forgetError->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_forgetError->hide();
    forgetOuter->addWidget(m_forgetError);
    m_forgetWindow = addRulesWindow(passwordsForm, tr("Auto-forget:"), tr("Auto-forget rules"), m_forgetPanel, tr("Edit rules…"));
    for (const AutoForgetRule& rule : config.autoForget) {
        addForgetRow(rule);
    }
    refreshForget();
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


    // --- Sync --------------------------------------------------------------------------
    auto* syncPage = new QWidget(this);
    auto* syncLayout = new QVBoxLayout(syncPage);
    auto* syncIntro = new QLabel(
        tr("Keep the history of your PCs in step through a folder that something else already syncs (Syncthing, "
           "Nextcloud, OneDrive, a network share). ClipItTo itself makes <b>no network connection</b>. It writes one "
           "small <b>encrypted</b> file of its own into the folder and reads the other PCs' files. The history file "
           "itself is never put there.<br><br>"
           "<b>Synced:</b> text copies and pictures (up to the size below), with their pins, stars, groups and notes, "
           "and what you delete.<br>"
           "<b>Never synced:</b> password copies, copies kept as secrets (keys, tokens, card numbers) and Paste once "
           "copies."),
        syncPage);
    syncIntro->setWordWrap(true);
    syncIntro->setTextFormat(Qt::RichText);
    syncLayout->addWidget(syncIntro);

    m_syncEnabled = new QCheckBox(tr("Keep my PCs in step through this folder"), syncPage);
    m_syncEnabled->setChecked(config.syncEnabled);
    syncLayout->addWidget(m_syncEnabled);

    auto* syncForm = new QFormLayout;
    m_syncFolder = new QLineEdit(syncPage);
    m_syncFolder->setText(config.syncFolder);
    m_syncFolder->setPlaceholderText(tr("The folder your sync program shares"));
    m_syncFolder->setToolTip(tr("ClipItTo never creates this folder: if it is not there (an unplugged drive or share), "
                                "it waits and tries again."));
    m_syncBrowse = new QPushButton(tr("Browse…"), syncPage);
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(m_syncFolder, 1);
    folderRow->addWidget(m_syncBrowse);
    syncForm->addRow(tr("Folder:"), folderRow);
    m_syncName = new QLineEdit(syncPage);
    m_syncName->setText(config.syncDeviceName);
    m_syncName->setMaxLength(64);
    m_syncName->setPlaceholderText(m_syncSupport.computerName ? m_syncSupport.computerName() : tr("this computer's name"));
    m_syncName->setToolTip(tr("How this PC is named in the other PCs' lists."));
    syncForm->addRow(tr("This PC is called:"), m_syncName);
    m_syncPassphraseState = new QLabel(syncPage);
    m_syncSet = new QPushButton(syncPage);
    m_syncRemove = new QPushButton(tr("Remove…"), syncPage);
    auto* passRow = new QHBoxLayout;
    passRow->addWidget(m_syncPassphraseState, 1);
    passRow->addWidget(m_syncSet);
    passRow->addWidget(m_syncRemove);
    syncForm->addRow(tr("Sync passphrase:"), passRow);
    m_syncMaxCopies = new QSpinBox(syncPage);
    m_syncMaxCopies->setRange(100, 20'000);
    m_syncMaxCopies->setGroupSeparatorShown(true);
    m_syncMaxCopies->setValue(config.syncMaxCopies);
    m_syncMaxCopies->setToolTip(tr("How many of the newest unsaved copies this PC puts in its sync file. Every pinned, "
                                   "starred and grouped copy is in it besides."));
    syncForm->addRow(tr("Copies shared:"), m_syncMaxCopies);
    m_syncPictures = new QCheckBox(tr("Share pictures too, up to"), syncPage);
    m_syncPictures->setChecked(config.syncPictures);
    m_syncPictureKb = new QSpinBox(syncPage);
    m_syncPictureKb->setRange(64, 20'480);
    m_syncPictureKb->setSuffix(tr(" KB"));
    m_syncPictureKb->setGroupSeparatorShown(true);
    m_syncPictureKb->setValue(config.syncPictureMaxKb);
    m_syncPictureKb->setEnabled(config.syncPictures);
    auto* pictureRow = new QHBoxLayout;
    pictureRow->addWidget(m_syncPictures);
    pictureRow->addWidget(m_syncPictureKb);
    pictureRow->addStretch(1);
    syncForm->addRow(QString(), pictureRow);
    m_syncDays = new QSpinBox(syncPage);
    m_syncDays->setRange(1, 365);
    m_syncDays->setSuffix(tr(" days"));
    m_syncDays->setValue(config.syncTombstoneDays);
    m_syncDays->setToolTip(tr("A deletion is announced to the other PCs this long. A PC that stays off longer than this "
                              "can bring a deleted copy back."));
    syncForm->addRow(tr("Announce deletions for:"), m_syncDays);
    syncLayout->addLayout(syncForm);
    m_syncNowButton = new QPushButton(tr("Sync now"), syncPage);
    m_syncStatus = new QLabel(syncPage);
    m_syncStatus->setWordWrap(true);
    m_syncStatus->setTextFormat(Qt::PlainText); // device names come from files in the folder
    m_syncStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* nowRow = new QHBoxLayout;
    nowRow->addWidget(m_syncNowButton);
    nowRow->addStretch(1);
    syncLayout->addLayout(nowRow);
    syncLayout->addWidget(m_syncStatus);
    syncLayout->addStretch();
    connect(m_syncBrowse, &QPushButton::clicked, this, &SettingsDialog::browseSyncFolder);
    connect(m_syncSet, &QPushButton::clicked, this, &SettingsDialog::setSyncPassphrase);
    connect(m_syncRemove, &QPushButton::clicked, this, &SettingsDialog::removeSyncPassphrase);
    connect(m_syncNowButton, &QPushButton::clicked, this, [this] {
        if (m_syncSupport.syncNow) {
            m_syncSupport.syncNow();
        }
        refreshSync();
    });
    connect(m_syncPictures, &QCheckBox::toggled, m_syncPictureKb, &QWidget::setEnabled);
    connect(m_syncFolder, &QLineEdit::textChanged, this, [this] { refreshSync(); });
    auto* syncRefresh = new QTimer(this);
    syncRefresh->setInterval(2000);
    connect(syncRefresh, &QTimer::timeout, this, [this] {
        if (isVisible() && m_tabs->currentWidget() && m_tabs->currentWidget()->isAncestorOf(m_syncStatus)) {
            refreshSync();
        }
    });
    syncRefresh->start();
    refreshSync();
    connect(m_syncEnabled, &QCheckBox::toggled, this, [this] { refreshSync(); }); // ticking it is what first asks the keyring

    // --- Dialog ------------------------------------------------------------------------
    // Every page scrolls: the Passwords page in particular is taller than a small screen, and a window cut off at the
    // bottom hid its last settings. `pageHint` is the biggest page, to size the window (below).
    QSize pageHint;
    const auto addPage = [this, &pageHint](QWidget* page, const QString& title) {
        pageHint = pageHint.expandedTo(page->sizeHint());
        auto* area = new QScrollArea(m_tabs);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setWidget(page);
        m_tabs->addTab(area, title);
    };
    addPage(general, tr("General"));
    addPage(groupsPage, tr("Groups"));
    addPage(passwordsPage, tr("Passwords"));
    addPage(backupPage, tr("Backup"));
    addPage(syncPage, tr("Sync"));
    {
        // --- Paste as: which items the Shift+Enter menu shows, and in what order ---------------------------
        auto* page = new QWidget(this);
        auto* box = new QVBoxLayout(page);
        auto* intro = new QLabel(tr("Choose what the <b>Paste as</b> menu (Shift+Enter, and right-click) shows and in what "
                                    "order: tick to show, drag or use the arrows to move."),
                                 page);
        intro->setWordWrap(true);
        intro->setTextFormat(Qt::RichText);
        box->addWidget(intro);
        m_pasteAsText = createPasteAsEditor(page, box, tr("For text"), pasteas::textItems(), config.pasteAsText);
        m_pasteAsPicture = createPasteAsEditor(page, box, tr("For pictures"), pasteas::pictureItems(), config.pasteAsPicture);
        m_pasteAsOthers = new QCheckBox(tr("Show the other items in the menu, under \"Others\""), page);
        m_pasteAsOthers->setChecked(config.pasteAsOthers);
        box->addWidget(m_pasteAsOthers);
        addPage(page, tr("Paste as"));
    }
    m_translation = std::move(translation);
    if (m_translation.builtIn) {
        // --- Translation: the language packs the popup's Translate menus use --------------------------------
        auto* page = new QWidget(this);
        auto* box = new QVBoxLayout(page);
        auto* intro = new QLabel(
            tr("<p>Translation runs on this computer only: nothing you copy is sent anywhere, and ClipItTo "
               "never downloads anything by itself. Add a <b>language pack</b> (one per direction, e.g. "
               "Chinese → English, English → Chinese, Hebrew → English) from a <code>.argosmodel</code> file. "
               "The list of packs and their links is at <a href=\"https://github.com/argosopentech/argospm-index\">argospm-index</a>; "
               "download one in your browser and choose it here. They are about 70-150 MB each and are credited "
               "to OPUS-MT (CC BY 4.0).</p>"
               "<p>Then copy a text or a picture, press Shift+Enter and choose <b>Translate</b>.</p>"),
            page);
        intro->setWordWrap(true);
        intro->setTextFormat(Qt::RichText);
        intro->setOpenExternalLinks(true);
        box->addWidget(intro);
        m_packList = new QListWidget(page);
        box->addWidget(m_packList, 1);
        auto* row = new QHBoxLayout;
        m_addPack = new QPushButton(tr("Add language pack…"), page);
        m_removePack = new QPushButton(tr("Remove"), page);
        for (QPushButton* b : {m_addPack, m_removePack}) {
            b->setAutoDefault(false);
            row->addWidget(b);
        }
        row->addStretch(1);
        box->addLayout(row);
        m_packPrompt = [](QWidget* parent) {
            return QFileDialog::getOpenFileName(parent, tr("Add a language pack"), QString(),
                                                tr("Language packs (*.argosmodel);;All files (*)"));
        };
        connect(m_addPack, &QPushButton::clicked, this, [this] {
            const QString file = m_packPrompt(this);
            if (file.isEmpty()) {
                return;
            }
            QString error;
            if (!m_translation.install || !m_translation.install(file, &error)) {
                m_notify(this, error.isEmpty() ? tr("The language pack could not be added.") : error);
                return;
            }
            reloadPacks();
        });
        connect(m_removePack, &QPushButton::clicked, this, [this] {
            const QListWidgetItem* item = m_packList->currentItem();
            if (!item || !m_translation.remove) {
                return;
            }
            if (m_confirm(this, tr("Remove the language pack \"%1\"?").arg(item->text()))) {
                m_translation.remove(item->data(Qt::UserRole).toString());
                reloadPacks();
            }
        });
        connect(m_packList, &QListWidget::currentRowChanged, this,
                [this](int row) { m_removePack->setEnabled(row >= 0); });
        reloadPacks();
        addPage(page, tr("Translation"));
    }
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
    // The window is as tall as the tallest page, but never taller than the screen (the pages scroll).
    const int chrome = m_tabs->tabBar()->sizeHint().height() + m_buttons->sizeHint().height() + 70;
    int wantHeight = pageHint.height() + chrome;
    int wantWidth = qMax(520, pageHint.width() + 30);
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const int most = screen->availableGeometry().height() * 9 / 10;
        if (wantHeight > most) {
            wantHeight = most;
            wantWidth += style()->pixelMetric(QStyle::PM_ScrollBarExtent); // room for the scroll bar
        }
    }
    resize(wantWidth, wantHeight);
}

void SettingsDialog::setSelfTest(SelfTestSupport support)
{
    m_selfTest = std::move(support);
    m_selfTestButton->setEnabled(m_selfTest.create != nullptr);
}

void SettingsDialog::openSelfTest()
{
    if (!m_selfTest.create) {
        return;
    }
    if (!m_selfTestWindow) {
        m_selfTestWindow = new SelfTestWindow(m_selfTest, this);
        m_selfTestWindow->setAttribute(Qt::WA_DeleteOnClose);
        m_selfTestWindow->setModal(false);
    }
    m_selfTestWindow->show();
    m_selfTestWindow->raise();
    m_selfTestWindow->activateWindow();
    if (!m_selfTestWindow->isRunning()) {
        m_selfTestWindow->run();
    }
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
    c.ocrLanguages = m_ocrLanguages->text().trimmed();
    c.indexPictures = m_indexPictures->isChecked();
    c.keepFormatting = m_keepFormatting->isChecked();
    c.autoGroup = m_autoGroup->isChecked();
    c.debugLog = m_debugLog->isChecked();
    {
        QStringList ids;
        for (const QCheckBox* box : m_layoutBoxes) {
            if (box->isChecked()) {
                ids << box->property("layout").toString();
            }
        }
        // Unchanged from what the setting meant: keep it as it was (empty stays "the system language's").
        const QStringList before = texttransform::enabledLayouts(m_layoutsSetting, QLocale::system().bcp47Name());
        c.keyboardLayouts = ids == before ? m_layoutsSetting : (ids.isEmpty() ? QStringLiteral("none") : ids.join(QStringLiteral(", ")));
    }
    c.pasteAsText = pasteAsSetting(m_pasteAsText, m_config.pasteAsText);
    c.pasteAsPicture = pasteAsSetting(m_pasteAsPicture, m_config.pasteAsPicture);
    c.pasteAsOthers = m_pasteAsOthers->isChecked();
    c.previewLines = m_lines->value();
    c.favoritesLimit = m_favorites->value();
    c.hotkey = m_hotkey;
    c.popupPosition = PopupPosition(m_position->currentData().toInt());
    c.startFocus = StartFocus(m_startFocus->currentData().toInt());
    c.encryptHistory = m_encrypt->isChecked();
    c.pasteOnEnter = m_paste->isChecked();
    c.latestAfterPaste = m_latest->isChecked();
    c.pasteKeysPerApp = m_pasteKeysPerApp->isChecked();
    c.pasteKeysApps.clear();
    for (const PasteKeyRule& rule : pasteKeysValues()) {
        if (pastekeys::validate(rule).isEmpty()) {
            c.pasteKeysApps << rule;
        }
    }
    c.ignoreCase = m_ignoreCase->isChecked();
    c.detectSecrets = m_detectSecrets->isChecked();
    c.detectPasswords = m_detectPasswords->isChecked();
    c.passwordRules.clear();
    for (const PasswordRule& rule : ruleValues()) {
        if (passwordrules::validate(rule).isEmpty()) {
            c.passwordRules << rule;
        }
    }
    c.autoForget.clear();
    for (const AutoForgetRule& rule : forgetValues()) {
        if (autoforget::validate(rule).isEmpty()) {
            c.autoForget << rule;
        }
    }
    c.passwordsPerNote = m_passwordsPerNote->isChecked();
    c.keepPasswords = m_keepPasswords->isChecked();
    c.passwordsInGroup = m_passwordsInGroup->isChecked();
    c.passwordAskNote = m_askNote->isChecked();
    c.lockOnScreenLock = m_lockOnScreenLock->isChecked();
    c.passwordUnlockMinutes = m_unlockMinutes->value();
    c.passwordClearSeconds = m_clearSeconds->value();
    c.pasteOnceClearSeconds = m_pasteOnceClearSeconds->value();
    c.panicKey = m_panicKey->isChecked();
    c.panicHotkey = m_panicHotkey;
    c.windowsHello = m_passwordSupport.keeper && m_passwordSupport.keeper->helloEnabled();
    c.syncEnabled = m_syncEnabled->isChecked();
    c.syncFolder = m_syncFolder->text().trimmed();
    c.syncDeviceName = m_syncName->text().trimmed();
    c.syncMaxCopies = m_syncMaxCopies->value();
    c.syncPictures = m_syncPictures->isChecked();
    c.syncPictureMaxKb = m_syncPictureKb->value();
    c.syncTombstoneDays = m_syncDays->value();
    if (m_keyword->hasAcceptableInput()) {
        c.passwordSearchKeyword = m_keyword->text();
    }
    return c;
}

void SettingsDialog::browseSyncFolder()
{
    const QString chosen = m_folderPrompt(this, m_syncFolder->text());
    if (!chosen.isEmpty()) {
        m_syncFolder->setText(QDir::toNativeSeparators(chosen));
    }
}

void SettingsDialog::refreshSync()
{
    // The keyring is asked only once sync is on (or a folder is chosen): opening Settings must not touch it, because
    // that can show the keyring's unlock window or ClipItTo's note about it for a feature that is off.
    const bool lookUp = (m_syncEnabled && m_syncEnabled->isChecked()) || !m_syncFolder->text().trimmed().isEmpty();
    const bool has = lookUp && m_syncSupport.hasPassphrase && m_syncSupport.hasPassphrase();
    const bool folder = !m_syncFolder->text().trimmed().isEmpty();
    m_syncPassphraseState->setText(!m_syncSupport.available
                                       ? tr("Needs the system keyring: %1").arg(m_syncSupport.reason)
                                       : (has ? tr("Set (the key is kept in the system keyring)") : tr("Not set")));
    m_syncSet->setText(has ? tr("Change…") : tr("Set…"));
    m_syncSet->setEnabled(m_syncSupport.available && m_syncSupport.setPassphrase && !m_syncBusy);
    m_syncRemove->setEnabled(has && !m_syncBusy);
    m_syncNowButton->setEnabled(has && folder && m_syncSupport.syncNow && !m_syncBusy);
    QString status = m_syncSupport.status ? m_syncSupport.status() : QString();
    if (m_syncBusy) {
        status = tr("Working out the key from the passphrase…");
    } else if (!has && status.isEmpty()) {
        status = tr("Choose the folder, then set the same sync passphrase on each PC.");
    }
    m_syncStatus->setText(status);
}

void SettingsDialog::setSyncPassphrase()
{
    if (!m_syncSupport.available || !m_syncSupport.setPassphrase || m_syncBusy) {
        return;
    }
    const QString folder = m_syncFolder->text().trimmed();
    if (folder.isEmpty()) {
        m_notify(this, tr("Choose the sync folder first: a passphrase that matches the other PCs' is found from the "
                          "files already in it."));
        return;
    }
    const bool change = m_syncSupport.hasPassphrase && m_syncSupport.hasPassphrase();
    std::optional<QString> typed = m_syncPrompt(this, change);
    if (!typed) {
        return;
    }
    if (typed->size() < kMinSyncPassphrase) {
        PasswordVault::wipe(*typed);
        m_notify(this, tr("The sync passphrase needs at least %1 characters.").arg(kMinSyncPassphrase));
        return;
    }
    QByteArray bytes = PasswordVault::passwordBytes(*typed); // wipes `typed`
    startSyncPassphrase(folder, std::move(bytes), /*startNew=*/false);
}

void SettingsDialog::startSyncPassphrase(const QString& folder, QByteArray bytes, bool startNew)
{
    QByteArray again = bytes; // for the second try, when the files in the folder need a new group
    m_syncBusy = true;
    refreshSync();
    m_syncSupport.setPassphrase(folder, std::move(bytes), startNew,
                                [this, self = QPointer<SettingsDialog>(this), folder, again = std::move(again),
                                 startNew](QString error, bool mismatch) mutable {
        if (!self) {
            PasswordVault::wipe(again);
            return;
        }
        m_syncBusy = false;
        refreshSync();
        if (error.isEmpty()) {
            PasswordVault::wipe(again);
            m_inform(this, tr("Sync passphrase set. Turn sync on and press Sync now."));
        } else if (mismatch && !startNew
                   && m_confirm(this, tr("The sync files in this folder do not open with this passphrase.\n\nStart a new "
                                         "sync group with it? Those files are then ignored (you can delete them), and "
                                         "every other PC must be given this passphrase too."))) {
            startSyncPassphrase(folder, std::move(again), /*startNew=*/true);
        } else {
            PasswordVault::wipe(again);
            m_notify(this, error);
        }
    });
}

void SettingsDialog::removeSyncPassphrase()
{
    if (!m_syncSupport.removePassphrase || m_syncBusy) {
        return;
    }
    if (!m_confirm(this, tr("Remove the sync passphrase from this PC? It stops syncing. The files in the folder and "
                            "your history stay as they are."))) {
        return;
    }
    m_syncSupport.removePassphrase();
    refreshSync();
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
        m_passwordsInRecent->setChecked(m_repo.groups().contains(PasswordKeeper::passwordsGroup()) &&
                                        !m_repo.groupHiddenFromRecent(PasswordKeeper::passwordsGroup()));
    }
    if (report->settings) {
        // What belongs to this computer stays as it is here.
        Config merged = *report->settings;
        const Config mine = config();
        merged.hotkey = mine.hotkey;
        merged.panicKey = mine.panicKey; // a file never turns the panic key on
        merged.panicHotkey = mine.panicHotkey;
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

QList<QSpinBox*> SettingsDialog::ruleSpinBoxes(int row) const
{
    const RuleRow& r = m_ruleRows.at(size_t(row));
    return {r.minLength, r.maxLength, r.lower, r.upper, r.digits, r.symbols};
}

void SettingsDialog::addRuleRow(const PasswordRule& rule)
{
    if (int(m_ruleRows.size()) >= passwordrules::kMaxRules) {
        return;
    }
    RuleRow r;
    r.row = new QWidget(m_rulesPanel);
    auto* v = new QVBoxLayout(r.row);
    v->setContentsMargins(0, 2, 0, 2);
    const auto spin = [&](int min, int max, int value) {
        auto* s = new QSpinBox(r.row);
        s->setRange(min, max);
        s->setValue(value);
        connect(s, &QSpinBox::valueChanged, this, &SettingsDialog::refreshRules);
        return s;
    };
    const int limit = passwordrules::kLengthLimit;
    r.minLength = spin(passwordrules::kMinLength, limit, std::max(rule.minLength, passwordrules::kMinLength));
    r.maxLength = spin(passwordrules::kMinLength, limit, std::max(rule.maxLength, passwordrules::kMinLength));
    r.lower = spin(0, limit, rule.lower);
    r.upper = spin(0, limit, rule.upper);
    r.digits = spin(0, limit, rule.digits);
    r.symbols = spin(0, limit, rule.symbols);
    r.end = new QCheckBox(tr("digits at the end"), r.row);
    r.end->setChecked(rule.digitsAtEnd);
    r.end->setToolTip(tr("The copy must end with a digit"));
    connect(r.end, &QCheckBox::toggled, this, &SettingsDialog::refreshRules);
    r.remove = new QPushButton(tr("Remove"), r.row);
    QWidget* rowWidget = r.row;
    connect(r.remove, &QPushButton::clicked, this, [this, rowWidget] { removeRuleRow(rowWidget); });
    r.title = new QLabel(r.row);

    auto* top = new QHBoxLayout;
    top->addWidget(r.title);
    top->addWidget(r.minLength);
    top->addWidget(new QLabel(tr("to"), r.row));
    top->addWidget(r.maxLength);
    top->addWidget(new QLabel(tr("characters"), r.row));
    top->addStretch(1);
    top->addWidget(r.remove);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(new QLabel(tr("at least:"), r.row));
    for (const auto& [label, box] : {std::pair{tr("lower case"), r.lower}, std::pair{tr("upper case"), r.upper},
                                     std::pair{tr("digits"), r.digits}, std::pair{tr("symbols"), r.symbols}}) {
        bottom->addWidget(new QLabel(label, r.row));
        bottom->addWidget(box);
    }
    bottom->addWidget(r.end);
    bottom->addStretch(1);
    v->addLayout(top);
    v->addLayout(bottom);
    m_rulesLayout->addWidget(r.row);
    m_ruleRows.push_back(r);
}

void SettingsDialog::removeRuleRow(QWidget* row)
{
    for (auto it = m_ruleRows.begin(); it != m_ruleRows.end(); ++it) {
        if (it->row == row) {
            m_ruleRows.erase(it);
            m_rulesLayout->removeWidget(row);
            row->hide();
            row->deleteLater();
            break;
        }
    }
    refreshRules();
}

QList<PasswordRule> SettingsDialog::ruleValues() const
{
    QList<PasswordRule> rules;
    for (const RuleRow& r : m_ruleRows) {
        PasswordRule rule;
        rule.minLength = r.minLength->value();
        rule.maxLength = r.maxLength->value();
        rule.lower = r.lower->value();
        rule.upper = r.upper->value();
        rule.digits = r.digits->value();
        rule.symbols = r.symbols->value();
        rule.digitsAtEnd = r.end->isChecked();
        rules << rule;
    }
    return rules;
}

void SettingsDialog::refreshRules()
{
    if (!m_rulesPanel || !m_ruleError) {
        return;
    }
    const QList<PasswordRule> rules = ruleValues();
    QStringList errors;
    for (qsizetype i = 0; i < rules.size(); ++i) {
        m_ruleRows[size_t(i)].title->setText(tr("Rule %1: length").arg(i + 1));
        const QString why = passwordrules::validate(rules.at(i));
        if (!why.isEmpty()) {
            errors << tr("Rule %1: %2.").arg(i + 1).arg(why);
        }
    }
    setNote(m_ruleError, errors.join(QLatin1Char(' ')));
    m_ruleError->setEnabled(true);
    showRulesSummary(m_rulesWindow, tr("%n rule(s)", "", int(rules.size())), !errors.isEmpty());
    m_addRule->setEnabled(rules.size() < passwordrules::kMaxRules);
    updateOkButton();
    // "Try it": which rule keeps the sample. Only the verdict is shown, never the text.
    const QString sample = m_ruleTry->text();
    if (sample.isEmpty()) {
        setNote(m_ruleTryResult, QString());
    } else if (rules.isEmpty()) {
        setNote(m_ruleTryResult, tr("No rules yet."));
    } else {
        QString result;
        QStringList reasons;
        for (qsizetype i = 0; i < rules.size(); ++i) {
            if (!passwordrules::validate(rules.at(i)).isEmpty()) {
                continue;
            }
            const QString why = passwordrules::whyNot(rules.at(i), sample);
            if (why.isEmpty()) {
                result = tr("Kept as a password copy by rule %1.").arg(i + 1);
                break;
            }
            reasons << tr("rule %1: %2").arg(i + 1).arg(why);
        }
        setNote(m_ruleTryResult, result.isEmpty() ? tr("Not kept by your rules (%1).").arg(reasons.join(QStringLiteral("; ")))
                                                  : result);
    }
}

void SettingsDialog::addForgetRow(const AutoForgetRule& rule)
{
    if (int(m_forgetRows.size()) >= autoforget::kMaxRules) {
        return;
    }
    ForgetRow r;
    r.row = new QWidget(m_forgetPanel);
    auto* h = new QHBoxLayout(r.row);
    h->setContentsMargins(0, 2, 0, 2);
    r.type = new QComboBox(r.row);
    r.type->addItem(tr("Older than"), int(AutoForgetRule::Type::Age));
    r.type->addItem(tr("Text containing"), int(AutoForgetRule::Type::Contains));
    r.type->addItem(tr("Looks like a secret:"), int(AutoForgetRule::Type::Secret));
    r.type->setCurrentIndex(int(rule.type));
    r.amount = new QSpinBox(r.row);
    r.amount->setRange(1, rule.type == AutoForgetRule::Type::Age ? autoforget::kMaxDays : autoforget::kMaxMinutes);
    r.amount->setValue(rule.amount);
    r.unit = new QLabel(r.row);
    r.scope = new QComboBox(r.row);
    r.scope->addItem(tr("any copy"), int(AutoForgetRule::Scope::Any));
    r.scope->addItem(tr("text only"), int(AutoForgetRule::Scope::Text));
    r.scope->addItem(tr("pictures only"), int(AutoForgetRule::Scope::Picture));
    r.scope->setCurrentIndex(int(rule.scope));
    r.text = new QLineEdit(rule.text, r.row);
    r.text->setMaxLength(autoforget::kMaxTextLength);
    r.text->setPlaceholderText(tr("text to look for, e.g. Bearer "));
    r.text->setToolTip(tr("Letter case is ignored. Copies that contain this are removed after the time. At least "
                          "%1 characters; no double quote.").arg(autoforget::kMinTextLength));
    r.secret = new QComboBox(r.row);
    r.secret->addItem(tr("token, key or card number"), int(AutoForgetRule::SecretKind::Any));
    r.secret->addItem(tr("access token"), int(AutoForgetRule::SecretKind::Token));
    r.secret->addItem(tr("key"), int(AutoForgetRule::SecretKind::Key));
    r.secret->addItem(tr("card number"), int(AutoForgetRule::SecretKind::Card));
    r.secret->setCurrentIndex(int(rule.secret));
    r.remove = new QPushButton(tr("Remove"), r.row);
    QWidget* rowWidget = r.row;
    connect(r.remove, &QPushButton::clicked, this, [this, rowWidget] { removeForgetRow(rowWidget); });
    connect(r.type, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshForget);
    connect(r.amount, &QSpinBox::valueChanged, this, &SettingsDialog::refreshForget);
    connect(r.scope, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshForget);
    connect(r.text, &QLineEdit::textChanged, this, &SettingsDialog::refreshForget);
    connect(r.secret, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshForget);
    h->addWidget(r.type);
    h->addWidget(r.text, 1);
    h->addWidget(r.secret);
    h->addWidget(r.amount);
    h->addWidget(r.unit);
    h->addWidget(r.scope);
    h->addStretch(1);
    h->addWidget(r.remove);
    m_forgetLayout->addWidget(r.row);
    m_forgetRows.push_back(r);
}

void SettingsDialog::removeForgetRow(QWidget* row)
{
    for (auto it = m_forgetRows.begin(); it != m_forgetRows.end(); ++it) {
        if (it->row == row) {
            m_forgetRows.erase(it);
            m_forgetLayout->removeWidget(row);
            row->hide();
            row->deleteLater();
            break;
        }
    }
    refreshForget();
}

QList<AutoForgetRule> SettingsDialog::forgetValues() const
{
    QList<AutoForgetRule> rules;
    for (const ForgetRow& r : m_forgetRows) {
        AutoForgetRule rule;
        rule.type = AutoForgetRule::Type(r.type->currentData().toInt());
        rule.amount = r.amount->value();
        if (rule.type == AutoForgetRule::Type::Age) {
            rule.scope = AutoForgetRule::Scope(r.scope->currentData().toInt());
        } else if (rule.type == AutoForgetRule::Type::Contains) {
            rule.text = r.text->text();
        } else {
            rule.secret = AutoForgetRule::SecretKind(r.secret->currentData().toInt());
        }
        rules << rule;
    }
    return rules;
}

void SettingsDialog::refreshForget()
{
    if (!m_forgetPanel || !m_forgetError) {
        return;
    }
    const QList<AutoForgetRule> rules = forgetValues();
    QStringList errors;
    for (qsizetype i = 0; i < rules.size(); ++i) {
        ForgetRow& r = m_forgetRows[size_t(i)];
        const auto type = rules.at(i).type;
        const bool age = type == AutoForgetRule::Type::Age;
        // A day count goes up to kMaxDays, a minute count to kMaxMinutes (the spin box follows the kind).
        const int limit = age ? autoforget::kMaxDays : autoforget::kMaxMinutes;
        if (r.amount->maximum() != limit) {
            r.amount->setMaximum(limit);
        }
        r.unit->setText(age ? tr("days since last used") : tr("minutes after the copy"));
        r.scope->setVisible(age);
        r.text->setVisible(type == AutoForgetRule::Type::Contains);
        r.secret->setVisible(type == AutoForgetRule::Type::Secret);
        const QString why = autoforget::validate(rules.at(i));
        if (!why.isEmpty()) {
            errors << tr("Auto-forget rule %1: %2.").arg(i + 1).arg(why);
        }
    }
    setNote(m_forgetError, errors.join(QLatin1Char(' ')));
    m_forgetError->setEnabled(true);
    showRulesSummary(m_forgetWindow, tr("%n rule(s)", "", int(rules.size())), !errors.isEmpty());
    m_addForget->setEnabled(rules.size() < autoforget::kMaxRules);
    updateOkButton();
}

// A rules editor in its own window: the page only has the row "Label:  [Edit…]  2 rules". The window is created now (so
// every widget exists and config() reads it as before) and is opened by the button.
SettingsDialog::RulesWindow SettingsDialog::addRulesWindow(QFormLayout* form, const QString& label, const QString& title,
                                                           QWidget* panel, const QString& buttonText)
{
    RulesWindow w;
    w.dialog = new QDialog(this);
    w.dialog->setWindowTitle(title);
    auto* layout = new QVBoxLayout(w.dialog);
    layout->addWidget(panel, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, w.dialog);
    connect(buttons, &QDialogButtonBox::rejected, w.dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, w.dialog, &QDialog::accept);
    layout->addWidget(buttons);
    w.dialog->setMinimumWidth(600);
    w.button = new QPushButton(buttonText, this);
    w.button->setAutoDefault(false);
    QDialog* dialog = w.dialog;
    connect(w.button, &QPushButton::clicked, this, [dialog] {
        dialog->open();
        dialog->raise();
    });
    w.summary = new QLabel(this);
    w.summary->setEnabled(false); // dim
    auto* row = new QHBoxLayout;
    row->addWidget(w.button);
    row->addWidget(w.summary, 1);
    form->addRow(label, row);
    return w;
}

void SettingsDialog::showRulesSummary(const RulesWindow& window, const QString& text, bool problem)
{
    if (!window.summary) {
        return;
    }
    if (problem) { // the editor's own message is in the window; the page must not hide that OK is waiting for a fix
        window.summary->setEnabled(true);
        window.summary->setStyleSheet(QStringLiteral("color: #c0392b;"));
        window.summary->setText(tr("A rule is not valid: open the editor to fix it."));
    } else {
        window.summary->setEnabled(false);
        window.summary->setStyleSheet(QString());
        window.summary->setText(text);
    }
}

void SettingsDialog::updateOkButton()
{
    // Each editor can hold an error: OK waits for all of them.
    bool fine = true;
    for (const QLabel* error : {m_ruleError, m_forgetError, m_pasteKeysError}) {
        // The paste keys rules are not saved while their switch is off: their errors do not count then.
        const bool ignored = error == m_pasteKeysError && m_pasteKeysPerApp && !m_pasteKeysPerApp->isChecked();
        fine = fine && (ignored || !error || error->text().isEmpty());
    }
    if (QPushButton* ok = m_buttons ? m_buttons->button(QDialogButtonBox::Ok) : nullptr) {
        ok->setEnabled(fine);
    }
}

void SettingsDialog::addPasteKeysRow(const PasteKeyRule& rule)
{
    if (int(m_pasteKeysRows.size()) >= pastekeys::kMaxRules) {
        return;
    }
    PasteKeysRow r;
    r.row = new QWidget(m_pasteKeysPanel);
    auto* h = new QHBoxLayout(r.row);
    h->setContentsMargins(0, 2, 0, 2);
    r.pattern = new QLineEdit(rule.pattern, r.row);
    r.pattern->setMaxLength(pastekeys::kMaxPatternLength);
    r.pattern->setPlaceholderText(tr("program, e.g. WindowsTerminal.exe, XTerm or org.gnome.Terminal"));
    r.pattern->setToolTip(tr("The executable name (Windows), the window class (X11) or the app id (GNOME). Letter "
                             "case and a trailing .exe do not matter; * matches any text."));
    r.chord = new QComboBox(r.row);
    for (const PasteKeys keys : {PasteKeys::CtrlV, PasteKeys::ShiftInsert, PasteKeys::CtrlShiftV}) {
        r.chord->addItem(pastekeys::chordName(keys), int(keys));
    }
    r.chord->setCurrentIndex(r.chord->findData(int(rule.keys)));
    r.remove = new QPushButton(tr("Remove"), r.row);
    r.remove->setAutoDefault(false);
    QWidget* rowWidget = r.row;
    connect(r.remove, &QPushButton::clicked, this, [this, rowWidget] { removePasteKeysRow(rowWidget); });
    connect(r.pattern, &QLineEdit::textChanged, this, &SettingsDialog::refreshPasteKeys);
    connect(r.chord, &QComboBox::currentIndexChanged, this, &SettingsDialog::refreshPasteKeys);
    h->addWidget(r.pattern, 1);
    h->addWidget(r.chord);
    h->addWidget(r.remove);
    m_pasteKeysLayout->addWidget(r.row);
    m_pasteKeysRows.push_back(r);
}

void SettingsDialog::removePasteKeysRow(QWidget* row)
{
    for (auto it = m_pasteKeysRows.begin(); it != m_pasteKeysRows.end(); ++it) {
        if (it->row == row) {
            m_pasteKeysRows.erase(it);
            m_pasteKeysLayout->removeWidget(row);
            row->hide();
            row->deleteLater();
            break;
        }
    }
    refreshPasteKeys();
}

QList<PasteKeyRule> SettingsDialog::pasteKeysValues() const
{
    QList<PasteKeyRule> rules;
    for (const PasteKeysRow& r : m_pasteKeysRows) {
        PasteKeyRule rule;
        rule.pattern = r.pattern->text().trimmed();
        rule.keys = PasteKeys(r.chord->currentData().toInt());
        rules << rule;
    }
    return rules;
}

void SettingsDialog::refreshPasteKeys()
{
    if (!m_pasteKeysPanel || !m_pasteKeysError) {
        return;
    }
    const QList<PasteKeyRule> rules = pasteKeysValues();
    QStringList errors;
    for (qsizetype i = 0; i < rules.size(); ++i) {
        const QString why = pastekeys::validate(rules.at(i));
        if (!why.isEmpty()) {
            errors << tr("Program %1: %2.").arg(i + 1).arg(why);
        }
    }
    setNote(m_pasteKeysError, errors.join(QLatin1Char(' ')));
    m_pasteKeysError->setEnabled(true);
    showRulesSummary(m_pasteKeysWindow, tr("%n program(s)", "", int(rules.size())), !errors.isEmpty());
    m_addPasteKeys->setEnabled(rules.size() < pastekeys::kMaxRules);
    updateOkButton();
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
    m_lockOnScreenLock->setEnabled(setUp);
    m_detectSecrets->setEnabled(setUp);
    m_detectPasswords->setEnabled(setUp);
    m_rulesPanel->setEnabled(setUp);
    m_rulesWindow.button->setEnabled(setUp);
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
    if (parsed && !m_panicHotkey.isEmpty() && parsed->canonical == m_panicHotkey) {
        QSignalBlocker block(m_shortcut);
        m_shortcut->setKeySequence(sequenceFor(m_hotkey));
        setNote(m_shortcutStatus, tr("%1 is the panic shortcut: choose another one. Keeping %2.")
                                      .arg(parsed->canonical, m_hotkey));
    } else if (parsed) {
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

void SettingsDialog::onPanicShortcutEdited()
{
    const QKeySequence sequence = m_panicShortcut->keySequence();
    if (sequence.isEmpty()) {
        return;
    }
    const QKeySequence first(sequence[0]);
    if (sequence.count() > 1) {
        QSignalBlocker block(m_panicShortcut);
        m_panicShortcut->setKeySequence(first);
    }
    const auto parsed = parseHotkey(swapMacModifiers(first.toString(QKeySequence::PortableText), /*toQt=*/false));
    const auto popup = parseHotkey(m_hotkey);
    QString note;
    if (!parsed) {
        note = tr("%1 can't be used as a shortcut. Keeping %2.").arg(first.toString(QKeySequence::NativeText),
                                                                    m_panicHotkey.isEmpty() ? tr("none") : m_panicHotkey);
    } else if (popup && popup->canonical == parsed->canonical) {
        note = tr("%1 already opens the popup: choose another shortcut for the panic key. Keeping %2.")
                   .arg(parsed->canonical, m_panicHotkey.isEmpty() ? tr("none") : m_panicHotkey);
    }
    if (!note.isEmpty()) {
        QSignalBlocker block(m_panicShortcut);
        m_panicShortcut->setKeySequence(sequenceFor(m_panicHotkey));
        setNote(m_panicStatus, note);
        return;
    }
    m_panicHotkey = parsed->canonical;
    setNote(m_panicStatus, m_panicKey->isChecked() ? tr("Wipes everything without asking: %1").arg(m_panicHotkey)
                                                   : tr("Saved, but the panic key is off."));
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
        QHash<QString, int> examples;
        for (const auto& e : m_repo.autoExamples()) {
            examples.insert(e.group, e.examples);
        }
        for (const auto& g : m_repo.groupTree()) {
            QTreeWidgetItem* parent = byPath.value(groups::parent(g.path));
            auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_groups);
            item->setText(0, groups::leaf(g.path));
            item->setText(1, g.clipCount ? QString::number(g.clipCount) : QString());
            item->setText(2, examples.value(g.path) ? QString::number(examples.value(g.path)) : QString());
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

SettingsDialog::PasteAsEditor SettingsDialog::createPasteAsEditor(QWidget* page, QLayout* into, const QString& title,
                                                                  const QList<pasteas::Item>& items, const QString& setting)
{
    PasteAsEditor e;
    e.items = items;
    auto* column = new QVBoxLayout;
    column->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), page));
    auto* row = new QHBoxLayout;
    e.list = new QListWidget(page);
    e.list->setDragDropMode(QAbstractItemView::InternalMove);
    e.list->setDefaultDropAction(Qt::MoveAction);
    row->addWidget(e.list, 1);
    auto* buttons = new QVBoxLayout;
    e.up = new QPushButton(tr("Move up"), page);
    e.down = new QPushButton(tr("Move down"), page);
    e.reset = new QPushButton(tr("Reset to default"), page);
    for (QPushButton* b : {e.up, e.down, e.reset}) {
        b->setAutoDefault(false);
        buttons->addWidget(b);
    }
    buttons->addStretch(1);
    row->addLayout(buttons);
    column->addLayout(row, 1);
    into->addItem(column);

    QListWidget* list = e.list;
    const auto fill = [list, items](const QString& value) {
        list->clear();
        QHash<QString, QString> labels;
        for (const pasteas::Item& item : items) {
            labels.insert(item.id, pasteas::plain(item.label));
        }
        for (const pasteas::Choice& c : pasteas::resolve(value, items)) {
            auto* item = new QListWidgetItem(labels.value(c.id), list);
            item->setData(Qt::UserRole, c.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(c.shown ? Qt::Checked : Qt::Unchecked);
        }
        list->setCurrentRow(0);
    };
    fill(setting);
    QPushButton* up = e.up;
    QPushButton* down = e.down;
    const auto refresh = [list, up, down] {
        up->setEnabled(list->currentRow() > 0);
        down->setEnabled(list->currentRow() >= 0 && list->currentRow() < list->count() - 1);
    };
    const auto move = [list, refresh](int by) {
        const int row = list->currentRow();
        const int to = row + by;
        if (row < 0 || to < 0 || to >= list->count()) {
            return;
        }
        QListWidgetItem* item = list->takeItem(row);
        list->insertItem(to, item);
        list->setCurrentRow(to);
        refresh();
    };
    connect(e.up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(e.down, &QPushButton::clicked, this, [move] { move(1); });
    connect(e.reset, &QPushButton::clicked, this, [fill, refresh] {
        fill(QString());
        refresh();
    });
    connect(list, &QListWidget::currentRowChanged, this, refresh);
    connect(list->model(), &QAbstractItemModel::rowsMoved, this, refresh);
    refresh();
    return e;
}

QString SettingsDialog::pasteAsSetting(const PasteAsEditor& editor, const QString& loaded) const
{
    QList<pasteas::Choice> now;
    for (int i = 0; i < editor.list->count(); ++i) {
        const QListWidgetItem* item = editor.list->item(i);
        now.append({item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked});
    }
    // Unchanged from what the setting meant: keep it as it was.
    return now == pasteas::resolve(loaded, editor.items) ? loaded : pasteas::serialize(now, editor.items);
}

QCheckBox* SettingsDialog::layoutBox(const QString& id) const
{
    for (QCheckBox* box : m_layoutBoxes) {
        if (box->property("layout").toString() == id) {
            return box;
        }
    }
    return nullptr;
}

void SettingsDialog::reloadPacks()
{
    if (!m_packList) {
        return;
    }
    m_packList->clear();
    if (m_translation.packsFor) {
        for (const TranslationPackInfo& pack : m_translation.packsFor(QString())) {
            auto* item = new QListWidgetItem(pack.label, m_packList);
            item->setData(Qt::UserRole, pack.id);
        }
    }
    m_removePack->setEnabled(m_packList->currentRow() >= 0);
}

} // namespace clipitto::ui

