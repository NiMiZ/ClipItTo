#include "Daemon.h"

#include "DebugLog.h"

#include <algorithm>

#include "CapturePipeline.h"
#include "GnomeShortcut.h"
#include "Hotkey.h"
#include "LoginItem.h"
#include "MacPaste.h"
#include "Notify.h"
#include "SettingsDialog.h"
#include "SyncService.h"
#include "Theme.h"
#include "ClipboardWriter.h"
#include "PopupWindow.h"
#include "WlPasteBackend.h"
#include "ImageCodec.h"
#include "Ocr.h"
#include "Translate.h"
#include "SyntheticPaste.h"
#include "WindowsHello.h"
#include "Paths.h"
#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QThreadPool>
#include <QTime>
#include <QTimer>
#include <QtConcurrent>

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
    case SessionEnv::Display::MacOS:
        return QStringLiteral("macos");
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
    m_shortcutPool.setMaxThreadCount(1);
    m_clearPassword = new QTimer(this);
    m_clearPassword->setSingleShot(true);
    connect(m_clearPassword, &QTimer::timeout, this, &Daemon::clearPastedPassword);
    m_forgetMore = new QTimer(this);
    m_forgetMore->setSingleShot(true);
    connect(m_forgetMore, &QTimer::timeout, this, [this] { runAutoForget(); });
    m_forgetTimer = new QTimer(this);
    m_forgetTimer->setInterval(m_options.autoForgetIntervalMs);
    connect(m_forgetTimer, &QTimer::timeout, this, [this] { runAutoForget(); });
    // Quitting with a pasted password still waiting: it leaves the clipboard now, not after the program is gone.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &Daemon::clearPastedPassword);
    m_helloPool.setMaxThreadCount(1);
    m_translatePool.setMaxThreadCount(1);
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

void Daemon::clearPastedPassword()
{
    m_clearPassword->stop();
    if (!m_writer) {
        return;
    }
    // Only if the clipboard still holds our paste: never wipe what the user copied afterwards. (Nothing about
    // the text is logged: only what happened.)
    switch (m_writer->clearPassword()) {
    case ClipboardWriter::ClearResult::Cleared:
        qCDebug(lcDaemon) << "pasted password or Paste once copy taken off the clipboard";
        break;
    case ClipboardWriter::ClearResult::Changed:
        qCDebug(lcDaemon) << "pasted password or Paste once copy not cleared: the clipboard holds something else now";
        break;
    case ClipboardWriter::ClearResult::Unsupported:
        qCWarning(lcDaemon) << "pasted password or Paste once copy could not be taken off the clipboard on this system";
        break;
    case ClipboardWriter::ClearResult::NothingPending:
        break;
    }
}

qint64 Daemon::runAutoForget()
{
    if (!m_forgetter || !m_repo || m_converting || m_forgetter->rules().isEmpty()) {
        return 0;
    }
    if (m_pipeline && m_pipeline->isPaused()) {
        return 0; // the database belongs to a conversion
    }
    try {
        const qint64 removed = m_forgetter->run();
        if (m_forgetter->hasMore()) {
            // A big backlog (a new rule on a long history) goes on in small steps, so the window never freezes.
            if (!m_forgetMore->isActive()) {
                m_forgetMore->start(100);
            }
        }
        if (removed > 0) {
            qCDebug(lcDaemon) << "auto-forget removed" << removed << "copies"; // a count, never contents
            if (m_popup) {
                m_popup->historyChanged(); // silent: the list just follows
            }
        }
        return removed;
    } catch (const db::DbError& e) {
        qCWarning(lcDaemon) << "auto-forget failed:" << e.what();
        return 0;
    }
}

Daemon::~Daemon()
{
    clearPastedPassword(); // quit before the timer: leave nothing behind
    debuglog::stop(); // closes the log file (it stays on disk until the setting is turned off)
    hello::cancelPending(); // a Windows Hello prompt left open must not hold up quitting
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
    applyDebugLog();
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
        m_repo->setIgnoreCase(m_config.ignoreCase);
        m_repo->setSecretsPerNote(m_config.passwordsPerNote);
        m_repo->prune(m_config.maxHistory); // the limits may have been lowered since last run
        m_repo->pruneImages(m_config.maxImages);
        m_forgetter = std::make_unique<autoforget::Pruner>(*m_repo, m_options.autoForgetClock);
        m_forgetter->setRules(m_config.autoForget);
        runAutoForget();
        m_forgetTimer->start();
        m_sync = std::make_unique<SyncService>(*m_repo, m_options.syncKeyStore);
        m_sync->setBlockedCheck([this] { return m_converting; });
        if (m_options.syncPollMs > 0) {
            m_sync->setIntervals(m_options.syncPollMs, m_options.syncDebounceMs);
        }
        m_sync->applyConfig(m_config);
    } catch (const db::DbError& e) {
        m_repo.reset();
        m_db.reset();
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return StartResult::Failed;
    }

    m_passwords = std::make_unique<PasswordKeeper>(*m_repo, m_options.matchKeyStore);
    m_passwords->reload();
    m_pipeline = std::make_unique<CapturePipeline>(*m_repo, m_config.maxHistory, m_config.maxItemBytes);
    m_pipeline->setImageLimits(m_config.maxImages, m_config.maxImageBytes);
    m_pipeline->setKeepFormatting(m_config.keepFormatting);
    m_pipeline->setAutoGrouping(m_config.autoGroup);
    if (!m_config.keepFormatting) {
        m_repo->clearAllRich(); // turned off while the daemon was down or converting: nothing stays behind
    }
    m_control->setPasswordsWantedProvider([this] { return m_pipeline && m_pipeline->keepsPasswords(); });
    connect(m_passwords.get(), &PasswordKeeper::vaultChanged, this, &Daemon::applyPasswordSettings);
    connect(m_control, &ControlChannel::lockPasswordsRequested, m_passwords.get(), &PasswordKeeper::lock);
    connect(m_control, &ControlChannel::ingestRequested, m_pipeline.get(),
            [this](const QString& text, const QStringList& mimeTypes, const QString& html) {
                m_pipeline->process(text, mimeTypes, html);
            });
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
    m_popup->setPasswords(m_passwords.get());
    setUpWindowsHello();
    setUpOcr();
    connect(m_pipeline.get(), &CapturePipeline::stored, this, [this](qint64, bool inserted) {
        if (inserted) {
            indexNextPicture();
        }
    });
    QTimer::singleShot(3000, this, [this] { indexNextPicture(); }); // the pictures from before
    setUpTranslation();
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
    // Before the list is read: what is due is gone when the popup shows.
    connect(m_popup.get(), &ui::PopupWindow::aboutToShow, this, [this] { runAutoForget(); });
    connect(m_popup.get(), &ui::PopupWindow::entryActivated, this, &Daemon::onEntryActivated);
    connect(m_popup.get(), &ui::PopupWindow::onceActivated, this, &Daemon::onOnceActivated);
    connect(m_popup.get(), &ui::PopupWindow::textActivated, this, &Daemon::onTextActivated);
    connect(m_popup.get(), &ui::PopupWindow::richActivated, this, &Daemon::onRichActivated);
    connect(m_popup.get(), &ui::PopupWindow::textEdited, this, &Daemon::onTextEdited);
    m_popup->setMaxItemBytes(m_config.maxItemBytes);
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
    connect(m_sync.get(), &SyncService::historyChanged, m_popup.get(), &ui::PopupWindow::historyChanged);
    connect(m_control, &ControlChannel::syncRequested, m_sync.get(), &SyncService::syncNow);
    connect(m_pipeline.get(), &CapturePipeline::autoFiled, m_popup.get(), &ui::PopupWindow::historyChanged);
    connect(m_pipeline.get(), &CapturePipeline::passwordSkipped, this, [this] {
        if (m_passwordNoticeShown) {
            return;
        }
        m_passwordNoticeShown = true;
        // Shown the next time the popup opens (a copy is usually made with the popup closed).
        auto* once = new QObject(this);
        connect(m_popup.get(), &ui::PopupWindow::visibilityChanged, once, [this, once](bool visible) {
            if (visible) {
                once->deleteLater();
                QTimer::singleShot(0, m_popup.get(), [this] {
                    m_popup->showMessage(tr("A password was copied but not kept: set a master password in "
                                            "Settings › Passwords to keep password copies, encrypted."));
                });
            }
        });
    });
    // "Ask for a name after copying a password": open the popup with a name box on the new copy.
    connect(m_pipeline.get(), &CapturePipeline::passwordStored, this, [this](qint64 id, const QString& source) {
        if (!m_config.passwordAskNote) {
            qCInfo(lcDaemon) << "name box not asked: password_ask_note is off";
        } else if (!m_popup) {
            qCInfo(lcDaemon) << "name box not asked: no popup";
        } else {
            m_popup->askPasswordNote(id, source);
        }
    });
    // The same name box after a copy was turned into a password by hand (it has no source app to suggest).
    connect(m_popup.get(), &ui::PopupWindow::passwordMarked, this, [this](qint64 id) {
        if (!m_config.passwordAskNote) {
            qCInfo(lcDaemon) << "name box not asked: password_ask_note is off";
        } else if (!m_popup) {
            qCInfo(lcDaemon) << "name box not asked: no popup";
        } else {
            m_popup->askPasswordNote(id, QString());
        }
    });
    connect(m_popup.get(), &ui::PopupWindow::passwordNameEnded, this, [this](const QString& outcome) {
        m_lastNameBox = outcome + QStringLiteral(" at ") + QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    });
    applyPasswordSettings();

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
    applyDesktopShortcut();
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
    m_capture.backend->setReadMarkedText(m_pipeline->keepsPasswords());
    m_capture.backend->setReadRichText(m_config.keepFormatting);
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
        m_integration->activatePopup(m_config.popupPosition);
    }
    emit m_control->visibilityChanged(visible);
}

void Daemon::onEntryActivated(const Entry& entry)
{
    pasteEntry(entry, entry.pasteOnce); // a copy marked Paste once is forgotten after this paste
}

void Daemon::onOnceActivated(const Entry& entry)
{
    pasteEntry(entry, true); // Ctrl+Shift+Enter: paste and forget, marked or not
}

void Daemon::pasteEntry(const Entry& entry, bool once)
{
    // Runs before the popup hides: without the shell integration on Wayland, setting the
    // clipboard needs our focus.
    if (once) {
        m_clearPassword->stop(); // a clear for an earlier paste must not take this one away; started after the paste
    }
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
        m_writer->setImage(png, entry.contentHash.mid(int(qstrlen("image:"))), once);
    } else if (entry.secret) {
        // The popup asked for the unlock already; it may have expired just now.
        std::optional<QByteArray> plain = m_passwords ? m_passwords->reveal(entry.id) : std::nullopt;
        if (!plain) {
            return;
        }
        QString text = QString::fromUtf8(*plain);
        PasswordVault::wipe(*plain);
        m_writer->setPassword(text);
        PasswordVault::wipe(text);
        if (!once && m_config.passwordClearSeconds > 0) {
            m_clearPassword->start(m_config.passwordClearSeconds * 1000);
        }
    } else if (once) {
        m_writer->setPassword(entry.content); // marked like a password: other managers skip it; cleared after the paste
    } else {
        m_writer->setText(entry.content);
    }
    if (m_config.latestAfterPaste && !entry.isSaved()) {
        // As if it had been copied again: the latest in the history. A filed copy (starred,
        // pinned or in a group) stays where it is: Recent is not reshuffled by using it. (The clipboard's echo
        // of our own write is ignored by the capture filter either way.)
        try {
            m_repo->touch(entry.id);
        } catch (const db::DbError& e) {
            qCWarning(lcDaemon) << "failed to update last_used:" << e.what();
        }
    }

    if (!once) {
        pasteIfWanted();
        return;
    }
    // Paste once: the copy is forgotten only when the paste keys really went out, and the clipboard is emptied
    // (if it still holds it) a few seconds after that. Otherwise the copy stays, and so does the clipboard (the
    // user may paste by hand).
    const qint64 id = entry.id;
    const bool secret = entry.secret;
    pasteIfWanted([this, id, secret](bool sent) {
        if (!m_popup) {
            return;
        }
        if (!sent) {
            qCInfo(lcDaemon) << "paste once: the paste keys were not sent, the copy is kept";
            m_popup->flash(tr("Paste once: the copy was kept, because the paste keys could not be sent."));
            if (secret && m_config.passwordClearSeconds > 0) {
                m_clearPassword->start(m_config.passwordClearSeconds * 1000); // a password never lingers
            }
            return;
        }
        if (m_config.pasteOnceClearSeconds > 0) {
            m_clearPassword->start(m_config.pasteOnceClearSeconds * 1000);
        }
        m_popup->consumePasteOnce(id);
    });
}

void Daemon::onTextEdited(qint64 id, const QString& text)
{
    // Only the newest copy is what the clipboard holds (or would be pasted next): the system
    // clipboard follows its new text. The writer tells the capture filter, so no new entry appears.
    std::optional<qint64> newest;
    try {
        newest = m_repo->newestId();
    } catch (const db::DbError& e) {
        qCWarning(lcDaemon) << "failed to find the newest copy:" << e.what();
        return;
    }
    if (newest != id) {
        return;
    }
    m_writer->setText(text);
    const QString title = QStringLiteral("ClipItTo");
    const QString body = tr("ClipItTo: the clipboard now holds the edited text");
    const bool shown = m_options.notify ? m_options.notify(title, body) : desktopNotify(title, body);
    if (!shown && m_popup) {
        m_popup->flash(body);
    }
}

void Daemon::announceKeyring(const QString& purpose)
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime last = m_keyringAsked.value(purpose);
    m_keyringAsked.insert(purpose, now);
    if (last.isValid() && last.secsTo(now) < 600) {
        return;
    }
    const bool history = purpose == QLatin1String("history");
    const bool syncing = purpose == QLatin1String("sync");
    qCInfo(lcDaemon) << "asking the system keyring for the" << purpose << "key";
    const QString title = QStringLiteral("ClipItTo");
    const QString body = history
        ? tr("ClipItTo asks your keyring for the key that protects your history. If the keyring asks you to unlock "
             "it, that is why. (Turn off encryption in Settings to stop this.)")
        : syncing
        ? tr("ClipItTo asks your keyring for the key of your sync files; it is made from your sync passphrase, which is "
             "not stored. If the keyring asks you to unlock it, that is why. (Turn off sync in Settings to stop this.)")
        : tr("ClipItTo asks your keyring for the key that finds a copied password again; it is not your master "
             "password. If the keyring asks you to unlock it, that is why.");
    const bool shown = m_options.notify ? m_options.notify(title, body) : desktopNotify(title, body);
    if (!shown && m_popup) {
        m_popup->flash(body);
    }
}

void Daemon::onRichActivated(const Entry& entry)
{
    // Runs before the popup hides, like onEntryActivated. A copy whose formatted version is gone (or was never
    // kept) is pasted as plain text.
    QString html;
    try {
        html = m_repo->richText(entry.id);
    } catch (const db::DbError& e) {
        qCWarning(lcDaemon) << "failed to read the formatted copy:" << e.what();
    }
    if (html.isEmpty()) {
        pasteEntry(entry, false);
        return;
    }
    m_writer->setRich(entry.content, html); // a marked copy is not consumed by a paste with changes (Paste as)
    if (m_config.latestAfterPaste && !entry.isSaved()) {
        try {
            m_repo->touch(entry.id);
        } catch (const db::DbError& e) {
            qCWarning(lcDaemon) << "failed to update last_used:" << e.what();
        }
    }
    pasteIfWanted();
}

void Daemon::onTextActivated(const QString& text)
{
    m_writer->setText(text); // a transformed copy: the stored one keeps its place and text
    pasteIfWanted();
}

void Daemon::pasteIfWanted(std::function<void(bool sent)> done)
{
    if (!m_config.pasteOnEnter) {
        if (done) {
            done(false);
        }
        return;
    }
    // Give the previously focused window time to get focus back after the popup hides.
    QTimer::singleShot(80, this, [this, done = std::move(done)] {
        bool sent = true;
        if (integrationAvailable()) {
            m_integration->paste(m_config.pasteKeys); // a call over D-Bus with no answer: taken as sent
        } else if (!(m_options.syntheticPaste ? m_options.syntheticPaste(m_config.pasteKeys)
                                              : pasteWithSyntheticKeys(m_config.pasteKeys))) {
            qCInfo(lcDaemon) << "paste_on_enter is not supported in this session "
                                "(needs the GNOME Shell extension on Wayland)";
            sent = false;
        }
        if (done) {
            done(sent);
        }
    });
}

// Debug lines are on by default in Qt, so the off state is set explicitly. The file is for people who cannot read a console (the
// Windows daemon has none): written while --verbose is given or debug_log is on, and deleted when the setting is turned off.
void Daemon::applyDebugLog()
{
    const bool want = m_options.verbose || m_config.debugLog;
    if (int(want) == m_debugLogOn) {
        return;
    }
    const QString path = m_options.debugLogPath.isEmpty() ? paths::debugLogFile() : m_options.debugLogPath;
    const bool first = m_debugLogOn < 0;
    m_debugLogOn = int(want);
    if (want) {
        QLoggingCategory::setFilterRules(QStringLiteral("clipitto.*.debug=true"));
        paths::ensureParentDir(path);
        if (debuglog::start(path)) {
            qCInfo(lcDaemon).noquote() << "diagnostic log written to" << path;
            return;
        }
        qCWarning(lcDaemon).noquote() << "could not open the diagnostic log file" << path;
        m_debugLogOn = 0; // and the debug lines go off again below, as if it had not been asked for
    }
    QLoggingCategory::setFilterRules(QStringLiteral("clipitto.*.debug=false"));
    debuglog::stop();
    if (!first && !want) {
        debuglog::remove(path); // turned off: nothing is kept
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
    if (!m_config.keepFormatting && m_repo) {
        m_repo->clearAllRich(); // turned off during the conversion
    }
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
    if (m_options.session.display == SessionEnv::Display::Windows || m_options.session.display == SessionEnv::Display::MacOS) {
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
    ui::PasswordSupport passwords;
    passwords.keeper = m_passwords.get();
    passwords.helloAvailable = m_helloAvailable;
    passwords.helloReason = m_helloReason;
    passwords.helloName = hello::displayName();
    passwords.setHello = [this](bool on, std::function<void(QString)> done) { setWindowsHello(on, std::move(done)); };
    passwords.helloUnlock = [this](std::function<void(bool)> done) { helloUnlock(std::move(done)); };
    ui::StartupSupport startup;
    if (m_options.session.display == SessionEnv::Display::MacOS) {
        startup.available = true;
        startup.enabled = loginItem().isEnabled();
        startup.set = [this](bool on, QString* error) { return loginItem().setEnabled(on, error); };
    }
    ui::SyncSupport syncing;
    if (m_sync) {
        SyncService* service = m_sync.get();
        syncing.available = service->available(&syncing.reason);
        syncing.hasPassphrase = [service] { return service->hasPassphrase(); };
        syncing.setPassphrase = [service](const QString& folder, QByteArray passphrase, bool startNew,
                                          std::function<void(QString, bool)> done) {
            service->setPassphrase(folder, std::move(passphrase), startNew, std::move(done));
        };
        syncing.removePassphrase = [service] { service->removePassphrase(); };
        syncing.syncNow = [this, service] {
            // The folder typed in the dialog is only used once Settings is accepted: sync what is saved.
            service->applyConfig(m_config);
            service->syncNow();
        };
        syncing.status = [service] { return service->statusText(); };
        syncing.computerName = [] { return QSysInfo::machineHostName(); };
    }
    ui::SettingsDialog dialog(m_config, *m_repo, support, encryption, m_popup.get(), passwords, startup, translationSupport(),
                              std::move(syncing));
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

LoginItem Daemon::loginItem() const
{
    return LoginItem(m_options.loginItemPath.isEmpty() ? LoginItem::defaultPlistPath() : m_options.loginItemPath,
                     QCoreApplication::applicationFilePath());
}

QString Daemon::cliCommand() const
{
    // In an AppImage the binaries live in a mount that is gone after it quits: the shortcut
    // runs the AppImage itself, which hands --toggle to the CLI.
    if (const QString appImage = qEnvironmentVariable("APPIMAGE"); !appImage.isEmpty()) {
        return QLatin1Char('"') + appImage + QStringLiteral("\" --toggle");
    }
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("clipitto"));
    return (QFileInfo(local).isExecutable() ? local : QStringLiteral("clipitto")) + QStringLiteral(" --toggle");
}

void Daemon::applyDesktopShortcut()
{
    // Windows and macOS register the hotkey in-process (see main.cpp, configReloaded) and
    // report it as "hotkey"; GNOME keeps it as a custom keyboard shortcut; elsewhere the user
    // binds `clipitto --toggle` themselves.
    if (m_options.session.display == SessionEnv::Display::Windows || m_options.session.display == SessionEnv::Display::MacOS) {
        return;
    }
    if (!m_options.session.isGnome) {
        setStatusValue(QStringLiteral("shortcut"),
                       QStringLiteral("not set automatically: bind \"clipitto --toggle\" in your desktop's keyboard settings"));
        return;
    }
    const auto hotkey = parseHotkey(m_config.hotkey);
    if (!hotkey) {
        qCWarning(lcDaemon).noquote() << "could not set the GNOME shortcut: unknown hotkey" << m_config.hotkey;
        setStatusValue(QStringLiteral("shortcut"), QStringLiteral("not set: unknown hotkey ") + m_config.hotkey);
        emit shortcutApplied();
        return;
    }
    // gsettings runs several times and each run may take seconds: keep it off the GUI thread.
    const quint64 request = ++m_shortcutRequest;
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, request, shown = m_config.hotkey] {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (request != m_shortcutRequest) {
            return; // a newer hotkey is queued behind this one
        }
        if (!error.isEmpty()) {
            qCWarning(lcDaemon).noquote() << "could not set the GNOME shortcut:" << error;
            setStatusValue(QStringLiteral("shortcut"), QStringLiteral("not set: ") + error);
        } else {
            setStatusValue(QStringLiteral("shortcut"), shown + QStringLiteral(" (GNOME)"));
        }
        emit shortcutApplied();
    });
    // Copies only: the job never touches the Daemon, so it is safe on the pool thread.
    watcher->setFuture(QtConcurrent::run(&m_shortcutPool, [accelerator = hotkey->gtkAccelerator,
                                                           command = cliCommand(),
                                                           program = m_options.gsettingsProgram] {
        QString error;
        if (applyGnomeShortcut(accelerator, command, &error, program)) {
            return QString();
        }
        return error.isEmpty() ? QStringLiteral("gsettings failed") : error;
    }));
}

namespace {

// Runs `work` on `pool` and hands its result to `then` on `context`'s thread (dropped if
// `context` is gone by then).
template <typename Result>
void runOn(QThreadPool* pool, QObject* context, std::function<Result()> work, std::function<void(Result)> then)
{
    auto* watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [watcher, then = std::move(then)] {
        QFuture<Result> future = watcher->future();
        then(future.takeResult()); // taken, not copied: it may be a key
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(pool, std::move(work)));
}

} // namespace

// Offline translation: the popup and Settings get the language packs and the engine through this struct.
ui::TranslationSupport Daemon::translationSupport()
{
    ui::TranslationSupport support;
    const std::shared_ptr<FakeTranslation> fake = m_options.fakeTranslation;
    support.builtIn = fake ? true : translate::builtIn();
    if (!support.builtIn) {
        return support;
    }
    const auto info = [](const translate::Pack& p) { return ui::TranslationPackInfo{p.id, p.label()}; };
    const auto installed = [fake] { return fake ? fake->packs : translate::PackStore().installed(); };
    support.packsFor = [info, installed](const QString& text) {
        QList<ui::TranslationPackInfo> out;
        for (const translate::Pack& pack : installed()) {
            if (text.isEmpty() || translate::fits(pack, text)) {
                out.push_back(info(pack));
            }
        }
        return out;
    };
    support.fits = [installed](const QString& packId, const QString& text) {
        for (const translate::Pack& pack : installed()) {
            if (pack.id == packId) {
                return translate::fits(pack, text);
            }
        }
        return false;
    };
    support.translate = [this, fake, installed](const QString& packId, const QString& text,
                                                std::function<void(const QString&, const QString&)> done) {
        // Answered at once, on this thread: a text this long can never finish inside the timeout, and its run
        // would keep every later translation waiting (see kMaxTranslateChars).
        if (text.size() > translate::kMaxTranslateChars) {
            done(QString(), tr("The text is too long to translate (more than %1 characters). Select a shorter part.")
                                .arg(translate::kMaxTranslateChars));
            return;
        }
        std::optional<translate::Pack> chosen;
        for (const translate::Pack& pack : installed()) {
            if (pack.id == packId) {
                chosen = pack;
            }
        }
        if (!chosen) {
            done(QString(), tr("That language pack is not installed."));
            return;
        }
        runOn<std::pair<QString, QString>>(
            &m_translatePool, this,
            [pack = *chosen, text, fake, seconds = m_config.translateTimeout] {
                QString error;
                std::optional<QString> out;
                if (fake && fake->engine) {
                    out = translate::runGuarded(
                        [fake, pack, text](QString* err, const std::atomic<bool>& cancelled) { return fake->engine(pack, text, err, cancelled); },
                        &error, std::clamp(seconds, 1, 10) * 1000);
                } else {
                    out = translate::translate(pack, text, &error, seconds);
                }
                if (!out) {
                    return std::pair<QString, QString>(QString(), error.isEmpty() ? QStringLiteral("Translation failed.") : error);
                }
                return std::pair<QString, QString>(*out, QString());
            },
            [done = std::move(done)](std::pair<QString, QString> result) { done(result.first, result.second); });
    };
    support.install = [](const QString& file, QString* error) { return translate::PackStore().install(file, error); };
    support.remove = [](const QString& id) { return translate::PackStore().remove(id); };
    return support;
}

void Daemon::setUpTranslation()
{
    m_popup->setTranslation(translationSupport());
}

void Daemon::indexNextPicture()
{
    if (!m_config.indexPictures || !(m_options.fakeOcr || ocr::builtIn()) || m_indexing || m_indexFailed || m_converting || !m_repo) {
        return;
    }
    qint64 id = 0;
    QByteArray png;
    try {
        id = m_repo->nextPictureToIndex();
        if (id) {
            png = m_repo->imageData(id);
        }
    } catch (const db::DbError& e) {
        qCWarning(lcDaemon) << "search inside pictures:" << e.what();
        return;
    }
    if (!id) {
        return;
    }
    const auto next = [this] { QTimer::singleShot(0, this, [this] { indexNextPicture(); }); };
    if (png.isEmpty()) {
        m_repo->setPictureText(id, QString()); // nothing to read
        next();
        return;
    }
    m_indexing = true;
    const QStringList languages = ocr::parseLanguages(m_config.ocrLanguages);
    const std::shared_ptr<FakeOcr> fakeOcr = m_options.fakeOcr;
    runOn<std::pair<QString, QString>>(
        &m_helloPool, this,
        [png, languages, fakeOcr] {
            QString error;
            const std::optional<QString> text = fakeOcr ? fakeOcr->recognize(png, languages, &error)
                                                        : ocr::recognize(png, languages, &error);
            return text ? std::pair<QString, QString>(*text, QString())
                        : std::pair<QString, QString>(QString(), error.isEmpty() ? QStringLiteral("failed") : error);
        },
        [this, id, next](std::pair<QString, QString> result) {
            m_indexing = false;
            if (!result.second.isEmpty()) {
                // Never the text itself in a log; the reason is the engine's message.
                qCInfo(lcDaemon) << "search inside pictures stopped:" << result.second;
                m_indexFailed = true;
                return;
            }
            if (!m_config.indexPictures) {
                return; // turned off while this one was read: record nothing (a "" would mean "read, nothing found")
            }
            try {
                m_repo->setPictureText(id, result.first);
            } catch (const db::DbError& e) {
                qCWarning(lcDaemon) << "search inside pictures:" << e.what();
                return;
            }
            next();
        });
}

void Daemon::setUpOcr()
{
    // Whether the build has text recognition is known at once; whether it can read right now (a language
    // installed, the data files) is found out when it is used, and said then. (Starting the engine here
    // ran Windows' WinRT at every start, and in every test.)
    QString why;
    const bool builtIn = m_options.fakeOcr || ocr::builtIn();
    if (!builtIn) {
        ocr::available(&why);
    }
    m_popup->setTextFromPicture(
        [this](qint64 id, std::function<void(const QString&, const QString&)> done) {
            QByteArray png;
            try {
                png = m_repo->imageData(id); // the database stays on this thread
            } catch (const db::DbError& e) {
                qCWarning(lcDaemon) << "failed to read the picture:" << e.what();
            }
            if (png.isEmpty()) {
                done(QString(), tr("The picture could not be read."));
                return;
            }
            const QStringList languages = ocr::parseLanguages(m_config.ocrLanguages);
            const std::shared_ptr<FakeOcr> fakeOcr = m_options.fakeOcr;
            runOn<std::pair<QString, QString>>(
                &m_helloPool, this, // the one WinRT thread (see WindowsHello_win.cpp)
                [png, languages, fakeOcr] {
                    QString error;
                    const std::optional<QString> text = fakeOcr ? fakeOcr->recognize(png, languages, &error)
                                                                : ocr::recognize(png, languages, &error);
                    if (!text) {
                        return std::pair<QString, QString>(
                            QString(), error.isEmpty() ? QStringLiteral("Text recognition failed.") : error);
                    }
                    return std::pair<QString, QString>(*text, QString());
                },
                [done = std::move(done)](std::pair<QString, QString> result) { done(result.first, result.second); });
        },
        builtIn, why);
}

void Daemon::setUpWindowsHello()
{
    runOn<std::pair<bool, QString>>(
        &m_helloPool, this,
        [] {
            QString why;
            const bool ok = hello::available(&why);
            return std::pair<bool, QString>(ok, why);
        },
        [this](std::pair<bool, QString> result) {
            m_helloAvailable = result.first;
            m_helloReason = result.second;
        });
    m_popup->setHelloUnlock([this](std::function<void(bool)> done) { helloUnlock(std::move(done)); });
}

void Daemon::helloUnlock(std::function<void(bool)> done)
{
    runOn<QByteArray>(
        &m_helloPool, this, [] { return hello::unlockKey(/*create=*/false); },
        [this, done = std::move(done)](QByteArray key) {
            const bool ok = !key.isEmpty() && m_passwords && m_passwords->unlockWithHelloKey(key);
            PasswordVault::wipe(key);
            done(ok);
        });
}

void Daemon::setWindowsHello(bool on, std::function<void(QString error)> done)
{
    if (!on) {
        if (m_passwords) {
            m_passwords->setHelloWrapped({});
        }
        runOn<bool>(&m_helloPool, this, [] { hello::removeKey(); return true; }, [done = std::move(done)](bool) {
            done(QString());
        });
        return;
    }
    runOn<std::pair<QByteArray, QString>>(
        &m_helloPool, this,
        [] {
            QString error;
            QByteArray key = hello::unlockKey(/*create=*/true, &error);
            return std::pair<QByteArray, QString>(key, error);
        },
        [this, done = std::move(done)](std::pair<QByteArray, QString> result) {
            QByteArray key = std::move(result.first);
            const QByteArray wrapped = !key.isEmpty() && m_passwords ? m_passwords->wrapForHello(key) : QByteArray();
            PasswordVault::wipe(key);
            if (wrapped.isEmpty()) {
                done(result.second.isEmpty() ? tr("unlock the passwords first") : result.second);
                return;
            }
            m_passwords->setHelloWrapped(wrapped);
            done(QString());
        });
}

void Daemon::applyPasswordSettings()
{
    if (!m_pipeline || !m_passwords) {
        return;
    }
    m_passwords->setUnlockMinutes(m_config.passwordUnlockMinutes);
    m_pipeline->setDetectSecrets(m_config.detectSecrets);
    m_pipeline->setDetectPasswords(m_config.detectPasswords);
    m_pipeline->setPasswordRules(m_config.passwordRules);
    m_pipeline->setPasswords(&m_passwords->vault(), m_config.keepPasswords,
                             m_config.passwordsInGroup ? PasswordKeeper::passwordsGroup() : QString());
    if (m_config.passwordsInGroup && m_repo) {
        // By default the Passwords group is kept out of Recent; the rule is set once, when the group is made.
        try {
            if (!m_repo->groups().contains(PasswordKeeper::passwordsGroup())) {
                m_repo->setGroupHiddenFromRecent(PasswordKeeper::passwordsGroup(), true);
            }
        } catch (const db::DbError& e) {
            qWarning() << "creating the Passwords group failed:" << e.what();
        }
    }
    if (m_capture.backend) {
        m_capture.backend->setReadMarkedText(m_pipeline->keepsPasswords());
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
    if (m_popup) {
        m_popup->setMaxItemBytes(m_config.maxItemBytes);
    }
    m_pipeline->setImageLimits(m_config.maxImages, m_config.maxImageBytes);
    m_pipeline->setKeepFormatting(m_config.keepFormatting);
    m_pipeline->setAutoGrouping(m_config.autoGroup);
    if (m_capture.backend) {
        m_capture.backend->setMaxImageBytes(m_config.maxImageBytes);
        m_capture.backend->setReadRichText(m_config.keepFormatting);
    }
    m_repo->setLimits(repositoryLimits(m_config));
    try {
        m_repo->setIgnoreCase(m_config.ignoreCase);
    } catch (const db::DbError& e) {
        qCWarning(lcDaemon) << "changing the case rule failed:" << e.what();
    }
    m_repo->setSecretsPerNote(m_config.passwordsPerNote);
    applyPasswordSettings();
    if (m_sync && !m_converting) {
        m_sync->applyConfig(m_config);
    }
    if (m_forgetter && m_config.autoForget != previous.autoForget) {
        m_forgetter->setRules(m_config.autoForget);
        runAutoForget(); // new rules take effect at once
    }
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
    if (m_config.indexPictures != previous.indexPictures || m_config.ocrLanguages != previous.ocrLanguages) {
        m_indexFailed = false;
        if (!m_config.indexPictures && !m_converting) {
            m_repo->clearPictureText();
        } else {
            indexNextPicture();
        }
    }
    if (!m_config.keepFormatting && previous.keepFormatting && !m_converting) {
        m_repo->clearAllRich(); // turned off: the formatted versions kept so far are deleted
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
    if (!m_keyringAsked.isEmpty()) {
        QStringList asked;
        for (auto it = m_keyringAsked.cbegin(); it != m_keyringAsked.cend(); ++it) {
            asked << QStringLiteral("%1 key at %2").arg(it.key(), it.value().toString(QStringLiteral("HH:mm:ss")));
        }
        asked.sort();
        s[QStringLiteral("keyring")] = QStringLiteral("last asked for: ") + asked.join(QStringLiteral(", "));
    }
    if (m_options.session.display == SessionEnv::Display::MacOS) {
        // Auto-paste (⌘V into the previous app) needs this permission; copying doesn't.
        s[QStringLiteral("accessibility")] = macAccessibilityGranted()
            ? QStringLiteral("granted")
            : QStringLiteral("not granted: auto-paste needs it (System Settings › Privacy & Security › Accessibility)");
        s[QStringLiteral("login_item")] = loginItem().isEnabled() ? QStringLiteral("on") : QStringLiteral("off");
    }
    if (m_passwords) {
        QString passwords;
        if (!m_passwords->isSetUp()) {
            passwords = QStringLiteral("off (no master password)");
        } else if (!m_config.keepPasswords) {
            passwords = QStringLiteral("not kept (turned off in the settings)");
        } else if (m_passwords->isOpen()) {
            passwords = QStringLiteral("open (no master password: not secure)");
        } else if (m_passwords->isUnlocked()) {
            passwords = QStringLiteral("unlocked (%1 min left)").arg((m_passwords->secondsLeft() + 59) / 60);
        } else {
            passwords = QStringLiteral("locked");
        }
        if (m_passwords->isSetUp() && !m_passwords->canMatch()) {
            passwords += QStringLiteral("; whole-password search while locked is off (%1)").arg(m_passwords->matchProblem());
        }
        s[QStringLiteral("passwords")] = passwords;
        const auto onOff = [](bool on) { return on ? QStringLiteral("on") : QStringLiteral("off"); };
        s[QStringLiteral("password_feature")] = QStringLiteral("ask_note %1, detect_secrets %2, detect_passwords %3, match key %4, last name box: %5")
                                                    .arg(onOff(m_config.passwordAskNote), onOff(m_config.detectSecrets),
                                                         onOff(m_config.detectPasswords),
                                                         m_passwords->canMatch() ? QStringLiteral("available") : QStringLiteral("not available"),
                                                         m_lastNameBox.isEmpty() ? QStringLiteral("none yet") : m_lastNameBox);
    }
    if (m_sync) {
        s[QStringLiteral("sync")] = m_sync->statusLine();
    }
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
