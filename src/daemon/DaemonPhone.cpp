// "Receive from phone" (roadmap 28): the daemon's side. It owns the server and feeds the window; a picture from the phone is
// decoded safely, stored through the normal picture path, its text is read with the same engine as "Paste text from
// picture" and added as a text copy through the capture pipeline. Only counts and sizes are logged: never the address, the
// token, the text or the picture.

#include "Daemon.h"

#include "CapturePipeline.h"
#include "ClipboardWriter.h"
#include "ImageCodec.h"
#include "Ocr.h"
#include "PopupWindow.h"
#include "SyntheticPaste.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QtConcurrent>

Q_LOGGING_CATEGORY(lcDaemonPhone, "clipitto.daemon.phone")

namespace clipitto {

namespace {

// What the pipeline did with one synchronous call.
struct StoreProbe {
    bool stored = false;
    bool inserted = false;
    qint64 id = 0;
    bool rejected = false;
    bool passwordSkipped = false;
    bool passwordKept = false;
};

template <typename F>
StoreProbe probePipeline(CapturePipeline& pipeline, F&& call)
{
    StoreProbe probe;
    const auto c1 = QObject::connect(&pipeline, &CapturePipeline::stored, &pipeline, [&probe](qint64 id, bool inserted) {
        probe.stored = true;
        probe.id = id;
        probe.inserted = inserted;
    });
    const auto c2 = QObject::connect(&pipeline, &CapturePipeline::rejected, &pipeline, [&probe](ClipFilter::Verdict) { probe.rejected = true; });
    const auto c3 = QObject::connect(&pipeline, &CapturePipeline::passwordSkipped, &pipeline, [&probe] { probe.passwordSkipped = true; });
    const auto c4 = QObject::connect(&pipeline, &CapturePipeline::passwordStored, &pipeline, [&probe](qint64, const QString&) { probe.passwordKept = true; });
    call();
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    QObject::disconnect(c3);
    QObject::disconnect(c4);
    return probe;
}

struct Decoded {
    std::optional<ImageClip> clip; // to keep in the history
    QByteArray ocrPng; // what is read (the same picture, even when it is too large to keep)
    QByteArray thumbnail;
    bool tooLargeToKeep = false;
    QString error;
};

QString stopText(phone::PhoneServer::StopReason reason)
{
    using R = phone::PhoneServer::StopReason;
    switch (reason) {
    case R::Expired: return QCoreApplication::translate("clipitto::Daemon", "The address has expired. Nothing is listening now.");
    case R::User: return QCoreApplication::translate("clipitto::Daemon", "Stopped. Nothing is listening now.");
    case R::Quit:
    case R::Panic: return QCoreApplication::translate("clipitto::Daemon", "Stopped.");
    }
    return {};
}

} // namespace

ui::PhoneSupport Daemon::phoneSupport()
{
    ui::PhoneSupport support;
    support.open = [this](QWidget* parent) { openPhone(parent); };
    return support;
}

QString Daemon::ocrUnavailableReason() const
{
    if (m_options.fakeOcr) {
        return m_options.fakeOcr->unavailable ? (m_options.fakeOcr->reason.isEmpty() ? tr("Text recognition is not available here.")
                                                                                     : m_options.fakeOcr->reason)
                                              : QString();
    }
    if (ocr::builtIn()) {
        return {};
    }
    QString why;
    ocr::available(&why); // the same text the popup shows
    return why.isEmpty() ? tr("Text recognition is not available here.") : why;
}

void Daemon::openPhone(QWidget* parent)
{
    if (!m_popup || !m_pipeline) {
        return;
    }
    if (m_converting) {
        m_popup->showMessage(tr("The history is being converted; try again in a moment."));
        return;
    }
    if (m_phone && m_phone->isListening() && m_phoneWindow) {
        m_phoneWindow->show();
        m_phoneWindow->raise();
        m_phoneWindow->activateWindow();
        return;
    }
    stopPhone(phone::PhoneServer::StopReason::User, /*closeWindow=*/true);

    auto* window = new ui::PhoneWindow(parent);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setModal(false);
    m_phoneWindow = window;
    connect(window, &ui::PhoneWindow::stopRequested, this, [this] { stopPhone(phone::PhoneServer::StopReason::User); });
    connect(window, &ui::PhoneWindow::extendRequested, this, [this] {
        if (m_phone) {
            m_phone->extend();
        }
    });
    connect(window, &ui::PhoneWindow::copyRequested, this, [this](const QString& text) {
        if (m_writer) {
            m_writer->setText(text); // an own write: the capture does not record it again
        }
    });
    connect(window, &ui::PhoneWindow::pasteRequested, this, [this](const QString& text) { pasteFromPhone(text); });
    // Closing the window (or the dialog that owns it) ends the session.
    connect(window, &QDialog::finished, this, [this] { stopPhone(phone::PhoneServer::StopReason::User); });
    connect(window, &ui::PhoneWindow::closing, this, [this] { stopPhone(phone::PhoneServer::StopReason::User); });

    const QList<phone::InterfaceInfo> interfaces = m_options.phoneInterfaces ? m_options.phoneInterfaces() : phone::systemInterfaces();
    const QList<phone::Candidate> candidates = phone::lanCandidates(interfaces);
    if (candidates.isEmpty()) {
        window->setUnavailable(tr("No local network address was found. Connect this computer to the same Wi-Fi or network as the phone, then try again."));
    } else {
        m_phone = std::make_unique<phone::PhoneServer>(m_options.phoneConfig.value_or(phone::PhoneServer::Config{}), this);
        QString error;
        if (!m_phone->start(&error)) {
            window->setUnavailable(tr("The local web server could not start: %1").arg(error));
            m_phone.reset();
        } else {
            QStringList urls;
            QStringList labels;
            for (const phone::Candidate& c : candidates) {
                urls << phone::buildUrl(c.address, m_phone->port(), m_phone->token());
                labels << (c.likelyVirtual ? tr("%1, probably virtual").arg(c.interfaceName) : c.interfaceName);
            }
            phone::PhoneServer* server = m_phone.get();
            window->setSession(urls, labels, [server] { return server->remainingMs(); });
            connect(server, &phone::PhoneServer::received, this, &Daemon::onPhoneReceived);
            connect(server, &phone::PhoneServer::stopped, window, [window](phone::PhoneServer::StopReason reason) {
                window->sessionEnded(stopText(reason));
            });
            qCInfo(lcDaemonPhone) << "receive from phone: started," << candidates.size() << "address(es)";
        }
    }
    rememberPasteTarget(); // the program that has the focus now gets "Paste"
    if (integrationAvailable()) {
        m_integration->rememberPasteTarget();
    }
    window->show();
    window->raise();
    window->activateWindow();
}

void Daemon::stopPhone(phone::PhoneServer::StopReason reason, bool closeWindow)
{
    ++m_phoneSession; // results of items from this session are dropped
    if (m_phone && m_phone->isListening()) {
        m_phone->stop(reason);
        qCInfo(lcDaemonPhone) << "receive from phone: stopped";
    }
    if (closeWindow && m_phoneWindow) {
        QPointer<ui::PhoneWindow> window = m_phoneWindow;
        window->disconnect(this);
        window->close();
        if (window) {
            window->deleteLater();
        }
    }
}

void Daemon::pasteFromPhone(const QString& text)
{
    if (!m_writer) {
        return;
    }
    m_writer->setText(text);
    if (m_phoneWindow) {
        m_phoneWindow->showMinimized(); // the program that had the focus gets it back
    }
    QTimer::singleShot(150, this, [this] { pasteIfWanted(); }); // after the focus has moved
}

void Daemon::onPhoneReceived(int id, phone::PhoneServer::Kind kind, const QByteArray& body)
{
    if (!m_pipeline || !m_phoneWindow) {
        phoneItemFailed(id, tr("ClipItTo cannot take it now."));
        return;
    }
    if (kind == phone::PhoneServer::Kind::Text) {
        onPhoneText(id, body);
    } else {
        onPhonePicture(id, body);
    }
}

void Daemon::phoneItemFailed(int id, const QString& message)
{
    if (m_phone) {
        m_phone->setItemState(id, phone::PhoneServer::State::Failed, 0, message);
    }
    if (m_phoneWindow) {
        m_phoneWindow->setItemStatus(id, tr("Failed: %1").arg(message));
    }
}

void Daemon::phoneItemDone(int id, int chars, const QString& note)
{
    if (m_phone) {
        m_phone->setItemState(id, phone::PhoneServer::State::Done, chars, note);
    }
    if (m_phoneWindow) {
        QString status = tr("Done: %n characters", nullptr, chars);
        if (!note.isEmpty()) {
            status += QStringLiteral(" (%1)").arg(note);
        }
        m_phoneWindow->setItemStatus(id, status);
    }
}

void Daemon::onPhoneText(int id, const QByteArray& body)
{
    const QString text = QString::fromUtf8(body);
    m_phoneWindow->addItem(id, QImage());
    const StoreProbe probe = probePipeline(*m_pipeline, [&] { m_pipeline->process(text, {QStringLiteral("text/plain")}); });
    if (!probe.passwordSkipped) {
        m_phoneWindow->setItemText(id, text); // a text that was refused as a password is not left on the screen
    }
    QString note;
    if (probe.passwordKept) {
        note = tr("kept as a password copy");
    } else if (probe.passwordSkipped) {
        note = tr("looks like a password: not kept, set a master password in Settings to keep passwords");
    } else if (probe.rejected || !probe.stored) {
        note = tr("not added to the history");
    }
    phoneItemDone(id, int(text.size()), note);
}

void Daemon::onPhonePicture(int id, const QByteArray& body)
{
    m_phoneWindow->addItem(id, QImage());
    const qint64 maxBytes = m_config.maxImageBytes;
    const quint64 epoch = m_panicEpoch;
    const quint64 session = m_phoneSession;
    auto* watcher = new QFutureWatcher<Decoded>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, id, epoch, session] {
        const Decoded decoded = watcher->result();
        watcher->deleteLater();
        if (epoch != m_panicEpoch || !m_pipeline) {
            return; // a panic came meanwhile: nothing is stored
        }
        if (session != m_phoneSession) {
            return; // the session ended (Stop, closed window): this picture is dropped, not stored
        }
        if (!decoded.error.isEmpty()) {
            phoneItemFailed(id, decoded.error);
            return;
        }
        if (m_phoneWindow) {
            m_phoneWindow->setItemThumbnail(id, QImage::fromData(decoded.thumbnail));
        }
        // The picture first, through the normal path (size limits, duplicates, pruning).
        qint64 pictureId = 0;
        QString kept;
        if (decoded.clip) {
            m_suppressIndex = true; // its text is set below; the search must not read it a second time
            const StoreProbe probe = probePipeline(*m_pipeline, [&] { m_pipeline->processImage(*decoded.clip, {QStringLiteral("image/png")}); });
            m_suppressIndex = false;
            pictureId = probe.stored ? probe.id : 0;
            if (!probe.stored) {
                kept = tr("the picture was not kept in the history");
            }
        } else {
            kept = tr("the picture is too large to keep in the history");
        }
        const QString unavailable = ocrUnavailableReason();
        if (!unavailable.isEmpty()) {
            phoneItemFailed(id, (kept.isEmpty() ? tr("The picture was kept in your history, but its text could not be read: %1")
                                                : tr("The text could not be read: %1")).arg(unavailable));
            return;
        }
        if (m_phone) {
            m_phone->setItemState(id, phone::PhoneServer::State::Reading);
        }
        if (m_phoneWindow) {
            m_phoneWindow->setItemStatus(id, tr("Reading the text…"));
        }
        const QStringList languages = ocr::parseLanguages(m_config.ocrLanguages);
        const std::shared_ptr<FakeOcr> fakeOcr = m_options.fakeOcr;
        const QByteArray png = decoded.ocrPng;
        auto* reader = new QFutureWatcher<std::pair<QString, QString>>(this);
        connect(reader, &QFutureWatcherBase::finished, this, [this, reader, id, epoch, session, pictureId, kept] {
            const auto result = reader->result();
            reader->deleteLater();
            if (epoch != m_panicEpoch || !m_pipeline || session != m_phoneSession) {
                return;
            }
            if (!result.second.isEmpty()) {
                phoneItemFailed(id, tr("The picture was kept, but its text could not be read: %1").arg(result.second));
                return;
            }
            const QString text = result.first;
            if (pictureId && m_config.indexPictures) {
                try {
                    m_repo->setPictureText(pictureId, text);
                } catch (const db::DbError& e) {
                    qCWarning(lcDaemonPhone) << "setting the picture's text failed:" << e.what();
                }
            }
            if (m_phoneWindow) {
                m_phoneWindow->setItemText(id, text);
            }
            if (text.trimmed().isEmpty()) {
                phoneItemDone(id, 0, tr("no text found in the picture"));
                return;
            }
            // A text copy through the pipeline: secret detection and the filters apply. It does not touch the clipboard,
            // so the capture never sees it a second time.
            const StoreProbe probe = probePipeline(*m_pipeline, [&] { m_pipeline->process(text, {QStringLiteral("text/plain")}); });
            QString note = kept;
            if (probe.passwordKept) {
                note = tr("kept as a password copy");
            } else if (probe.passwordSkipped) {
                note = tr("looks like a password: not kept");
            } else if (probe.rejected || !probe.stored) {
                note = tr("the text was not added to the history");
            }
            phoneItemDone(id, int(text.size()), note);
        });
        reader->setFuture(QtConcurrent::run(&m_helloPool, [png, languages, fakeOcr] {
            QString error;
            const std::optional<QString> text = fakeOcr && fakeOcr->recognize ? fakeOcr->recognize(png, languages, &error)
                                                                              : ocr::recognize(png, languages, &error);
            return text ? std::pair<QString, QString>(*text, QString())
                        : std::pair<QString, QString>(QString(), error.isEmpty() ? QStringLiteral("Text recognition failed.") : error);
        }));
    });
    watcher->setFuture(QtConcurrent::run(&m_phonePool, [body, maxBytes] {
        Decoded d;
        const QImage image = capture::decodeUntrusted(body, &d.error);
        if (image.isNull()) {
            return d;
        }
        d.clip = capture::encodeImage(image, maxBytes);
        if (d.clip) {
            d.ocrPng = d.clip->png;
            d.thumbnail = d.clip->thumbnail;
            return d;
        }
        // Larger than the history keeps: still read, not kept.
        constexpr qint64 kReadLimit = 256LL * 1024 * 1024;
        const std::optional<ImageClip> big = capture::encodeImage(image, kReadLimit);
        if (!big) {
            d.error = QCoreApplication::translate("clipitto::Daemon", "The picture could not be read.");
            return d;
        }
        d.ocrPng = big->png;
        d.thumbnail = big->thumbnail;
        d.tooLargeToKeep = true;
        return d;
    }));
}

} // namespace clipitto
