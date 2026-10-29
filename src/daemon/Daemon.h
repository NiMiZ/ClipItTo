#pragma once

#include "BackendSelector.h"
#include "Config.h"
#include "ControlChannel.h"
#include "DesktopIntegration.h"
#include "Entry.h"
#include "KeyStore.h"
#include "PasswordKeeper.h"
#include "SessionEnv.h"
#include "Translation.h"

#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace clipitto {

class CapturePipeline;
class LoginItem;
class ClipboardWriter;

namespace ui {
class PopupWindow;
}

namespace db {
class Db;
class Repository;
} // namespace db

struct DaemonOptions {
    QString configPath;
    QString databasePath;
    SessionEnv session;
    QString wlPasteProgram = QStringLiteral("wl-paste");
    QString gsettingsProgram = QStringLiteral("gsettings"); // GNOME shortcut updates
    QString loginItemPath; // macOS launch agent; empty: ~/Library/LaunchAgents (tests set one)
    std::shared_ptr<KeyStore> keyStore; // holds the history key; null: encryption unavailable
    std::shared_ptr<KeyStore> matchKeyStore; // password copies' match key; null: no keyring
    // Pastes with synthetic keys where there is no desktop integration (tests replace it, so
    // they never type real keys); returns false where that isn't possible.
    std::function<bool(PasteKeys)> syntheticPaste;
};

// Owns everything that lives for the whole session: config, database, capture, the popup,
// the platform control channel (D-Bus on Linux) and the optional desktop integration
// (the GNOME Shell extension on Linux).
class Daemon : public QObject {
    Q_OBJECT

public:
    using StartResult = ControlChannel::StartResult;

    Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control,
           std::unique_ptr<DesktopIntegration> integration = nullptr, QObject* parent = nullptr);
    ~Daemon() override;

    StartResult start(QString* error = nullptr);
    void reloadConfig();
    // Opens the settings dialog over the popup (the popup's ⚙ button).
    void openSettings();
    // Saves the settings to the config file and applies them. On GNOME the shortcut is
    // (re)applied even when unchanged, so the first save creates it.
    bool applySettings(const Config& config, QString* error = nullptr);
    QVariantMap status() const;
    // Adds platform details (e.g. the Windows hotkey) to status().
    void setStatusValue(const QString& key, const QVariant& value) { m_extraStatus.insert(key, value); }

    bool isConverting() const { return m_converting; } // history being encrypted/decrypted
    bool isHistoryEncrypted() const { return m_encrypted; }
    // Whether encryption can be turned on here (build support + reachable keyring).
    bool encryptionAvailable(QString* why = nullptr) const;

    const Config& config() const { return m_config; }
    db::Repository* repository() const { return m_repo.get(); }
    ControlChannel* control() const { return m_control; }
    ui::PopupWindow* popup() const { return m_popup.get(); } // null until start() succeeded
    PasswordKeeper* passwords() const { return m_passwords.get(); } // null until start() succeeded

signals:
    void configReloaded();
    void encryptionFinished(bool ok, const QString& error); // after a runtime switch
    void shortcutApplied(); // after each GNOME shortcut update, whether or not it worked

private:
    void startCapture();
    void loadConfig();
    void reload(bool forceShortcut);
    void openHistory();
    QByteArray historyKey(bool create, QString* error) const;
    void setEncryption(bool on);
    void finishConversion(bool encrypted, const QString& error);
    void revertEncryptionSetting(); // after a failed runtime switch: config follows the file
    void applyDesktopShortcut();
    // Password copies follow the settings and the vault: kept or not, which group, unlock time.
    void applyPasswordSettings();
    void setUpWindowsHello(); // availability, and the popup's Hello unlock
    void setUpTranslation(); // the popup's Translate menus
    ui::TranslationSupport translationSupport();
    void setUpOcr(); // whether text can be read from pictures here, and the popup's menu entry
    void onTextActivated(const QString& text);
    void pasteIfWanted();
    void helloUnlock(std::function<void(bool)> done);
    void setWindowsHello(bool on, std::function<void(QString error)> done);
    QString cliCommand() const;
    LoginItem loginItem() const; // macOS: start at login
    void onEntryActivated(const Entry& entry);
    void onPopupVisibilityChanged(bool visible);
    bool integrationAvailable() const { return m_integration && m_integration->isAvailable(); }
    capture::Capabilities capabilities();

    DaemonOptions m_options;
    Config m_config;
    std::unique_ptr<db::Db> m_db;
    std::unique_ptr<db::Repository> m_repo;
    std::unique_ptr<PasswordKeeper> m_passwords;
    std::unique_ptr<CapturePipeline> m_pipeline;
    capture::Selection m_capture;
    std::unique_ptr<ClipboardWriter> m_writer;
    std::unique_ptr<ui::PopupWindow> m_popup; // declared last: destroyed first, it uses m_repo
    ControlChannel* m_control; // child of this
    DesktopIntegration* m_integration = nullptr; // child of this; may be null
    std::optional<bool> m_wlDataControl; // probed lazily, once
    QVariantMap m_extraStatus;
    bool m_encrypted = false;
    bool m_decodingImage = false; // an IngestImage picture is being decoded
    bool m_converting = false;
    QString m_encryptionError;
    QString m_startupWarning; // shown once, the first time the popup opens
    bool m_removeKeyAfterStart = false;
    bool m_passwordNoticeShown = false; // "set a master password", once per session
    QTimer* m_clearPassword = nullptr; // takes a pasted password off the clipboard again
    std::thread m_worker; // runs a conversion; the database belongs to it meanwhile
    QThreadPool m_shortcutPool; // one thread: GNOME shortcut updates run in order, off the GUI thread
    QThreadPool m_translatePool; // one thread: translating takes a second or two
    QThreadPool m_helloPool; // one thread: Windows Hello and text-recognition calls (blocking, WinRT; never on the GUI thread)
    bool m_helloAvailable = false;
    QString m_helloReason;
    quint64 m_shortcutRequest = 0; // results of older requests don't overwrite the status
};

} // namespace clipitto
