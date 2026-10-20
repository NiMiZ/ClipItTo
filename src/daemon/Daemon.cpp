#include "Daemon.h"

#include "CapturePipeline.h"
#include "GnomeShortcut.h"
#include "Hotkey.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "ClipboardWriter.h"
#include "PopupWindow.h"
#include "WlPasteBackend.h"
#include "ImageCodec.h"
#include "SyntheticPaste.h"
#include "Paths.h"
#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QThreadPool>
#include <QTimer>

Q_LOGGING_CATEGORY(lcDaemon, "clipitto.daemon")

namespace clipitto {

namespace {

QString displayName(SessionEnv::Display display)
{
    switch (display) {
    case SessionEnv::Display::Wayland:
        return QStringLiteral("wayland");
    case SessionEnv::Display::X11:
        return QStringLiteral("x11");
    case SessionEnv::Display::Windows:
        return QStringLiteral("windows");
    case SessionEnv::Display::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

db::Repository::Limits repositoryLimits(const Config& config)
{
    db::Repository::Limits limits;
    limits.favorites = config.favoritesLimit;
    return limits;
}

} // namespace

Daemon::Daemon(DaemonOptions options, std::unique_ptr<ControlChannel> control,
               std::unique_ptr<DesktopIntegration> integration, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_control(control.release())
    , m_integration(integration.release())
{
    m_control->setParent(this);
    m_control->setStatusProvider([this] { return status(); });
    connect(m_control, &ControlChannel::reloadRequested, this, &Daemon::reloadConfig);
    if (m_integration) {
        m_integration->setParent(this);
        // The extension can appear or vanish at any time: re-pick the capture backend.
        connect(m_integration, &DesktopIntegration::availabilityChanged, this, [this] {
            if (m_pipeline) {
                startCapture();
            }
        });
    }
}

Daemon::~Daemon()
{
    // Picture decodes still running use Qt's image plugins: let them finish while Qt is up.
    QThreadPool::globalInstance()->waitForDone();
    if (m_worker.joinable()) {
        m_worker.join(); // a conversion in flight: let it finish, the file stays consistent
    }
    if (m_pipeline && m_pipeline->isPaused()) {
        m_pipeline->setPaused(false); // store what was copied during that conversion
    }
    if (m_repo) {
        try {
            m_repo->optimize();
        } catch (const db::DbError& e) {
            qCWarning(lcDaemon) << "optimize on shutdown failed:" << e.what();
        }
    }
}

void Daemon::loadConfig()
{
    QStringList warnings;
    m_config = Config::load(m_options.configPath, &warnings);
    for (const QString& w : warnings) {
        qCWarning(lcDaemon).noquote() << m_options.configPath + QStringLiteral(": ") + w;
    }
}

Daemon::StartResult Daemon::start(QString* error)
{
    // Claim single-instance ownership first: a second instance must not touch the database.
    const StartResult controlResult = m_control->start(error);
    if (controlResult != StartResult::Ok) {
        return controlResult;
    }

    loadConfig();

    try {
        openHistory();
        db::migrate(*m_db);
        m_repo = std::make_unique<db::Repository>(*m_db);
        m_repo->setLimits(repositoryLimits(m_config));
        m_repo->prune(m_config.maxHistory); // the limits may have been lowered since last run
        m_repo->pruneImages(m_config.maxImages);
    } catch (const db::DbError& e) {
        m_repo.reset();
        m_db.reset();
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return StartResult::Failed;
    }

    m_pipeline = std::make_unique<CapturePipeline>(*m_repo, m_config.maxHistory, m_config.maxItemBytes);
    m_pipeline->setImageLimits(m_config.maxImages, m_config.maxImageBytes);
    connect(m_control, &ControlChannel::ingestRequested, m_pipeline.get(), &CapturePipeline::process);
    // A picture from a capture helper (the GNOME extension), decoded off the GUI thread.
    connect(m_control, &ControlChannel::ingestImageRequested, this,
            [this](const QByteArray& data, const QString& mimeType, const QStringList& mimeTypes) {
                // One at a time and within the size limit: a flood of calls can't pile up
                // decodes in memory. (Who may call is checked by the D-Bus channel.)
                // Marked secrets are refused before anything is decoded.
                if (!m_pipeline || m_pipeline->maxImages() == 0 || data.size() > m_config.maxImageBytes
                    || m_decodingImage || !capture::isSupportedImageType(mimeType)
                    || ClipFilter::isSensitive(mimeTypes)) {
                    return;
                }
                m_decodingImage = true;
                const QDateTime copiedAt = QDateTime::currentDateTimeUtc();
                capture::encodeImageDataAsync(data, mimeType, m_config.maxImageBytes, this,
                                              [this, mimeTypes, copiedAt](std::optional<ImageClip> clip) {
                    m_decodingImage = false;
                    if (clip && m_pipeline) {
                        clip->copiedAt = copiedAt;
                        m_pipeline->processImage(*clip, mimeTypes);
                    }
                });
            });
    startCapture();

    m_writer = std::make_unique<ClipboardWriter>(QGuiApplication::clipboard(), m_pipeline->filter(), m_integration);
    ui::applyTheme(m_config.theme);
    m_popup = std::make_unique<ui::PopupWindow>(*m_repo, m_config);
    connect(m_popup.get(), &ui::PopupWindow::settingsRequested, this, &Daemon::openSettings);
    connect(this, &Daemon::encryptionFinished, m_popup.get(), [this](bool ok, const QString& error) {
        if (!ok) {
            m_popup->showMessage(tr("Could not change the history encryption: %1").arg(error));
        }
    });
    connect(m_control, &ControlChannel::toggleRequested, m_popup.get(), &ui::PopupWindow::togglePopup);
    connect(m_control, &ControlChannel::showRequested, m_popup.get(), &ui::PopupWindow::showPopup);
    connect(m_control, &ControlChannel::hideRequested, m_popup.get(), &ui::PopupWindow::hidePopup);
    connect(m_popup.get(), &ui::PopupWindow::visibilityChanged, this, &Daemon::onPopupVisibilityChanged);
    connect(m_popup.get(), &ui::PopupWindow::entryActivated, this, &Daemon::onEntryActivated);
    connect(m_popup.get(), &ui::PopupWindow::aboutToShow, this, [] { rememberPasteTarget(); });
    // popup_position = last: where the popup closed, kept next to the history across runs.
    const QString positionFile = QFileInfo(m_options.databasePath).dir().filePath(QStringLiteral("popup-position"));
    if (QFile file(positionFile); file.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> xy = file.readAll().simplified().split(' ');
        bool okX = false;
        bool okY = false;
        const int x = xy.value(0).toInt(&okX);
        const int y = xy.value(1).toInt(&okY);
        if (okX && okY) {
            m_popup->setLastPosition(QPoint(x, y));
        }
    }
    connect(m_popup.get(), &ui::PopupWindow::closedAt, this, [positionFile](QPoint topLeft) {
        QSaveFile file(positionFile);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray::number(topLeft.x()) + ' ' + QByteArray::number(topLeft.y()) + '\n');
            file.commit();
        }
    });
    connect(m_pipeline.get(), &CapturePipeline::stored, m_popup.get(), &ui::PopupWindow::historyChanged);

    if (m_removeKeyAfterStart) { // the history was decrypted and opened fine: the key is done
        QString removeError;
        if (!m_options.keyStore->remove(&removeError)) {
            qCWarning(lcDaemon).noquote() << "could not remove the old history key:" << removeError;
        }
        m_removeKeyAfterStart = false;
    }
    if (!m_startupWarning.isEmpty()) {
        qCWarning(lcDaemon).noquote() << m_startupWarning;
        // Say it where the user looks, once: the first time the popup opens.
        auto* once = new QObject(this);
        connect(m_popup.get(), &ui::PopupWindow::visibilityChanged, once, [this, once](bool visible) {
            if (visible) {
                once->deleteLater();
                QTimer::singleShot(0, m_popup.get(), [this] { m_popup->showMessage(m_startupWarning); });
            }
        });
    }
    return StartResult::Ok;
}

void Daemon::startCapture()
{
    if (m_capture.backend) {
        m_capture.backend->stop();
    }
    m_capture = capture::selectBackend(m_config, m_options.session, capabilities(), QGuiApplication::clipboard(),
                                       m_options.wlPasteProgram);
    connect(m_capture.backend.get(), &capture::CaptureBackend::captured, m_pipeline.get(),
            &CapturePipeline::process);
    connect(m_capture.backend.get(), &capture::CaptureBackend::capturedImage, m_pipeline.get(),
            &CapturePipeline::processImage);
    m_capture.backend->setMaxImageBytes(m_config.maxImageBytes);
    m_capture.backend->start();

    if (m_capture.degraded) {
        qCWarning(lcDaemon).noquote() << "capture backend" << m_capture.backend->name() << "(degraded):"
                                      << m_capture.note;
    } else {
        qCInfo(lcDaemon).noquote() << "capture backend" << m_capture.backend->name() << "-" << m_capture.note;
    }
}

capture::Capabilities Daemon::capabilities()
{
    capture::Capabilities caps;
    caps.gnomeExtension = integrationAvailable();
    // Probing spawns wl-paste and can take a few hundred ms, so only do it when it could matter.
    const bool wantsWlPaste = m_options.session.display == SessionEnv::Display::Wayland
        && m_config.backend == BackendChoice::Auto && !caps.gnomeExtension;
    if (wantsWlPaste && !m_wlDataControl) {
        m_wlDataControl = capture::WlPasteBackend::probe(m_options.wlPasteProgram);
    }
    caps.wlDataControl = m_wlDataControl.value_or(false);
    return caps;
}

void Daemon::onPopupVisibilityChanged(bool visible)
{
    if (visible && integrationAvailable()) {
        // Wayland compositors won't let us take focus ourselves; the shell can.
        m_integration->activatePopup();
    }
    emit m_control->visibilityChanged(visible);
}

void Daemon::onEntryActivated(const Entry& entry)
{
    // Runs before the popup hides: without the shell integration on Wayland, setting the
    // clipboard needs our focus.
    if (entry.image) {
        QByteArray png;
        try {
            png = m_repo->imageData(entry.id);
        } catch (const db::DbError& e) {
            qCWarning(lcDaemon) << "failed to read the picture:" << e.what();
        }
        if (png.isEmpty()) {
            return;
        }
        m_writer->setImage(png, entry.contentHash.mid(int(qstrlen("image:"))));
    } else {
        m_writer->setText(entry.content);
    }
    if (m_config.latestAfterPaste) {
        // As if it had been copied again: the latest in the history. (The clipboard's echo
        // of our own write is ignored by the capture filter either way.)
        try {
            m_repo->touch(entry.id);
        } catch (const db::DbError& e) {
            qCWarning(lcDaemon) << "failed to update last_used:" << e.what();
        }
    }

    if (m_config.pasteOnEnter) {
        // Give the previously focused window time to get focus back after the popup hides.
        QTimer::singleShot(80, this, [this] {
            if (integrationAvailable()) {
                m_integration->paste(m_config.pasteKeys);
            } else if (!(m_options.syntheticPaste ? m_options.syntheticPaste(m_config.pasteKeys)
                                                  : pasteWithSyntheticKeys(m_config.pasteKeys))) {
                qCInfo(lcDaemon) << "paste_on_enter is not supported in this session "
                                    "(needs the GNOME Shell extension on Wayland)";
            }
        });
    }
}

void Daemon::reloadConfig()
{
    reload(false);
}

bool Daemon::encryptionAvailable(QString* why) const
{
    const auto fail = [why](const QString& reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (!db::encryptionSupported()) {
        return fail(tr("this build has no encryption support"));
    }
    if (!m_options.keyStore) {
        return fail(tr("this build has no system keyring support"));
    }
    return m_options.keyStore->available(why);
}

QByteArray Daemon::historyKey(bool create, QString* error) const
{
    QString readError;
    if (const auto stored = m_options.keyStore->read(&readError)) {
        return *stored;
    }
    if (!readError.isEmpty() || !create) {
        if (error) {
            *error = readError.isEmpty() ? tr("no key is stored in the system keyring") : readError;
        }
        return {};
    }
    // A random 256-bit key: high entropy, so no password-strengthening is needed.
    std::array<quint32, 8> words{};
    QRandomGenerator::system()->fillRange(words.data(), qsizetype(words.size()));
    QByteArray key(reinterpret_cast<const char*>(words.data()), qsizetype(sizeof(words)));
    words.fill(0);
    QString keyError;
    // Read it back before trusting it: a keyring that only kept it for this session (or
    // dropped it) would make the history unreadable after the next login.
    if (!m_options.keyStore->write(key, &keyError)) {
        db::wipe(key);
        if (error) {
            *error = keyError;
        }
        return {};
    }
    std::optional<QByteArray> stored = m_options.keyStore->read(&keyError);
    if (!stored || *stored != key) {
        db::wipe(key);
        if (stored) {
            db::wipe(*stored);
        }
        if (error) {
            *error = keyError.isEmpty() ? tr("the system keyring did not keep the key") : keyError;
        }
        return {};
    }
    db::wipe(*stored);
    return key;
}

void Daemon::openHistory()
{
    const QString& path = m_options.databasePath;
    if (!paths::ensureParentDir(path)) {
        throw db::DbError("cannot create directory for " + path.toStdString(), 0);
    }
    const db::FileState state = db::probeFile(path);
    const bool encryptedOnDisk = state == db::FileState::Encrypted;
    const bool wanted = m_config.encryptHistory;
    m_encryptionError.clear();

    QByteArray key;
    if (encryptedOnDisk || wanted) {
        QString why;
        if (!encryptionAvailable(&why)) {
            if (encryptedOnDisk) {
                throw db::DbError(("the history is encrypted, but " + why).toStdString(), 0);
            }
            m_encryptionError = why; // asked for, can't be done here: stay plain, say why
        } else {
            QString keyError;
            key = historyKey(/*create=*/!encryptedOnDisk, &keyError);
            if (key.isEmpty() && encryptedOnDisk) {
                throw db::DbError(("the history is encrypted, but its key could not be read from the "
                                   "system keyring: " + keyError).toStdString(), 0);
            }
            m_encryptionError = keyError;
        }
    }

    m_db = std::make_unique<db::Db>(path, encryptedOnDisk ? key : QByteArray());
    db::configureConnection(*m_db);
    m_encrypted = encryptedOnDisk;
    // Bring the file in line with the setting. This runs before any window exists. A failed
    // conversion leaves the file as it was (journaled) and must not stop the daemon.
    try {
        if (wanted && !encryptedOnDisk && !key.isEmpty()) {
            m_db->rekey(key);
            m_encrypted = true;
        } else if (!wanted && encryptedOnDisk) {
            m_db->rekey({});
            m_encrypted = false;
            m_removeKeyAfterStart = true; // once the rest of the start has worked
        }
    } catch (const db::DbError& e) {
        m_encryptionError = QString::fromUtf8(e.what());
        qCWarning(lcDaemon).noquote() << "history encryption change at startup failed:" << m_encryptionError;
    }
    db::wipe(key);
    if (wanted && !m_encrypted) {
        m_startupWarning = tr("Your history is NOT encrypted, although encryption is turned on in the settings: %1")
                               .arg(m_encryptionError.isEmpty() ? tr("unknown reason") : m_encryptionError);
    }
    for (const char* suffix : {"", "-wal", "-shm"}) {
        paths::restrictToOwner(path + QLatin1String(suffix));
    }
}

void Daemon::setEncryption(bool on)
{
    if (on == m_encrypted || m_converting || !m_db) {
        return;
    }
    QByteArray key;
    if (on) {
        QString error;
        if (!encryptionAvailable(&error) || (key = historyKey(/*create=*/true, &error)).isEmpty()) {
            m_encryptionError = error;
            revertEncryptionSetting();
            emit encryptionFinished(false, error);
            return;
        }
    }
    // The conversion rewrites every page: do it off the GUI thread. Meanwhile nothing else
    // touches the database: capture waits in memory and the popup shows a notice.
    m_converting = true;
    m_encryptionError.clear();
    m_pipeline->setPaused(true);
    m_popup->setBusy(on ? tr("Encrypting the history…") : tr("Decrypting the history…"));
    db::Db* db = m_db.get();
    // The key moves into the worker (no copy stays here), which wipes it when done.
    m_worker = std::thread([this, db, key = std::move(key), on]() mutable {
        QString error;
        try {
            db->rekey(key);
        } catch (const db::DbError& e) {
            error = QString::fromUtf8(e.what());
        }
        db::wipe(key);
        QMetaObject::invokeMethod(this, [this, on, error] { finishConversion(on, error); }, Qt::QueuedConnection);
    });
}

void Daemon::revertEncryptionSetting()
{
    // Settings must show what is true on disk, not the request that failed.
    if (m_config.encryptHistory == m_encrypted) {
        return;
    }
    m_config.encryptHistory = m_encrypted;
    QString error;
    if (!m_config.save(m_options.configPath, &error)) {
        qCWarning(lcDaemon).noquote() << "could not save the reverted encryption setting:" << error;
    }
}

void Daemon::finishConversion(bool encrypted, const QString& error)
{
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_converting = false;
    if (error.isEmpty()) {
        m_encrypted = encrypted;
        if (!encrypted) {
            QString removeError;
            if (!m_options.keyStore->remove(&removeError)) {
                qCWarning(lcDaemon).noquote() << "could not remove the old history key:" << removeError;
            }
        }
        qCInfo(lcDaemon) << "history" << (encrypted ? "encrypted" : "decrypted");
    } else {
        m_encryptionError = error;
        qCWarning(lcDaemon).noquote() << "history encryption change failed:" << error;
        revertEncryptionSetting();
    }
    m_pipeline->setPaused(false); // store what was copied meanwhile
    m_popup->setBusy(QString());
    emit encryptionFinished(error.isEmpty(), error);
}

void Daemon::openSettings()
{
    if (!m_popup) {
        return;
    }
    if (m_converting) {
        m_popup->showMessage(tr("The history is being converted; try again in a moment."));
        return;
    }
    ui::ShortcutSupport support;
    if (m_options.session.display == SessionEnv::Display::Windows) {
        support.note = tr("ClipItTo registers this shortcut itself.");
    } else if (m_options.session.isGnome) {
        support.note = tr("Saved as the GNOME keyboard shortcut for ClipItTo.");
    } else {
        support.editable = false;
        support.note = tr("Bind \"clipitto --toggle\" to a shortcut in your desktop's keyboard settings.");
    }
    ui::EncryptionSupport encryption;
    encryption.available = encryptionAvailable(&encryption.reason);
    encryption.active = m_encrypted;
    encryption.problem = m_encryptionError;
    ui::SettingsDialog dialog(m_config, *m_repo, support, encryption, m_popup.get());
    if (m_popup->execDialog(dialog) == QDialog::Accepted) {
        QString error;
        if (!applySettings(dialog.config(), &error)) {
            qCWarning(lcDaemon).noquote() << "saving settings failed:" << error;
            m_popup->showMessage(tr("Could not save the settings: %1").arg(error));
        }
    } else if (dialog.groupsChanged()) {
        m_popup->refresh();
    }
}

bool Daemon::applySettings(const Config& config, QString* error)
{
    if (!config.save(m_options.configPath, error)) {
        return false;
    }
    reload(/*forceShortcut=*/true);
    return true;
}

QString Daemon::cliCommand() const
{
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("clipitto"));
    return (QFileInfo(local).isExecutable() ? local : QStringLiteral("clipitto")) + QStringLiteral(" --toggle");
}

void Daemon::applyDesktopShortcut()
{
    // Windows registers the hotkey in-process (see main.cpp, configReloaded); GNOME keeps
    // it as a custom keyboard shortcut; elsewhere the user binds `clipitto --toggle`.
    if (!m_options.session.isGnome || m_options.session.display == SessionEnv::Display::Windows) {
        return;
    }
    const auto hotkey = parseHotkey(m_config.hotkey);
    QString error;
    if (!hotkey || !applyGnomeShortcut(hotkey->gtkAccelerator, cliCommand(), &error, m_options.gsettingsProgram)) {
        qCWarning(lcDaemon).noquote() << "could not set the GNOME shortcut:" << error;
        setStatusValue(QStringLiteral("shortcut"), QStringLiteral("not set: ") + error);
    } else {
        setStatusValue(QStringLiteral("shortcut"), m_config.hotkey + QStringLiteral(" (GNOME)"));
    }
}

void Daemon::reload(bool forceShortcut)
{
    const Config previous = m_config;
    loadConfig();
    qCInfo(lcDaemon) << "configuration reloaded";
    if (!m_pipeline) {
        return;
    }
    m_pipeline->setLimits(m_config.maxHistory, m_config.maxItemBytes);
    m_pipeline->setImageLimits(m_config.maxImages, m_config.maxImageBytes);
    if (m_capture.backend) {
        m_capture.backend->setMaxImageBytes(m_config.maxImageBytes);
    }
    m_repo->setLimits(repositoryLimits(m_config));
    if (m_config.theme != previous.theme) {
        ui::applyTheme(m_config.theme);
    }
    m_popup->applyConfig(m_config);
    if (forceShortcut || m_config.hotkey != previous.hotkey) {
        applyDesktopShortcut();
    }
    if (m_config.encryptHistory != previous.encryptHistory) {
        setEncryption(m_config.encryptHistory);
    }
    if (m_config.maxImages < previous.maxImages && !m_converting) {
        m_repo->pruneImages(m_config.maxImages); // 0: "don't record pictures" removes the unsaved ones
    }
    if (m_config.maxHistory < previous.maxHistory && !m_converting) {
        m_repo->prune(m_config.maxHistory);
    }
    if (m_config.backend != previous.backend || m_config.capturePrimary != previous.capturePrimary) {
        startCapture();
    }
    emit configReloaded();
}

QVariantMap Daemon::status() const
{
    QVariantMap s = m_extraStatus;
    s[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    s[QStringLiteral("session")] = displayName(m_options.session.display);
    s[QStringLiteral("gnome")] = m_options.session.isGnome;
    s[QStringLiteral("config")] = m_options.configPath;
    s[QStringLiteral("database")] = m_options.databasePath;
    s[QStringLiteral("max_history")] = m_config.maxHistory;
    s[QStringLiteral("visible")] = m_popup && m_popup->isVisible();
    s[QStringLiteral("desktop_integration")] = m_integration
        ? QStringLiteral("%1 (%2)").arg(m_integration->name(),
                                        m_integration->isAvailable() ? QStringLiteral("connected")
                                                                     : QStringLiteral("not running"))
        : QStringLiteral("none");
    if (m_capture.backend) {
        s[QStringLiteral("backend")] = m_capture.backend->name();
        s[QStringLiteral("backend_degraded")] = m_capture.degraded;
        s[QStringLiteral("backend_note")] = m_capture.note;
    }
    QString encryption = m_converting ? QStringLiteral("converting")
                         : m_encrypted ? QStringLiteral("on (system keyring)")
                                       : QStringLiteral("off");
    if (!m_encryptionError.isEmpty()) {
        encryption += QStringLiteral(" (problem: %1)").arg(m_encryptionError);
    }
    s[QStringLiteral("encryption")] = encryption;
    if (m_repo && !m_converting) { // during a conversion the database belongs to the worker
        try {
            s[QStringLiteral("items")] = m_repo->count();
            s[QStringLiteral("unsaved_items")] = m_repo->countUnsaved();
            s[QStringLiteral("images")] = m_repo->countImages();
        } catch (const db::DbError& e) {
            s[QStringLiteral("error")] = QString::fromUtf8(e.what());
        }
    }
    return s;
}

} // namespace clipitto
