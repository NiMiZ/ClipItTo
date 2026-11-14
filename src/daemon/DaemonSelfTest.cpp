// The daemon's part of "Check this installation" (roadmap 31): it fills the environment the checks look at, and runs the two
// checks that need the running program (a test copy through the capture, a test paste into the Settings window's box).
// The checks themselves and their wording are in core/SelfTestChecks.cpp.

#include "Daemon.h"

#include "CapturePipeline.h"
#include "ClipboardWriter.h"
#include "Ocr.h"
#include "Paths.h"
#include "SyncService.h"
#include "SyntheticPaste.h"
#include "Translate.h"
#include "WindowsHello.h"
#include "PopupWindow.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QClipboard>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTimer>

Q_LOGGING_CATEGORY(lcSelfTest, "clipitto.selftest")

namespace clipitto {

namespace {

// Looks like nothing a user copies, and nothing a secret scanner takes for a password. The text identifies our own test
// copy in the history; it is looked up by its hash, so no other copy is ever read.
QString uniqueMarker(const QString& what)
{
    return QStringLiteral("ClipItTo %1 %2").arg(what).arg(QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
}

} // namespace

selftest::Env Daemon::selfTestEnv(const selftest::PasteBox* box)
{
    selftest::Env env;
    env.version = QCoreApplication::applicationVersion();
    env.platform = QSysInfo::prettyProductName();
    env.session = m_options.session;
    env.status = status();
    env.status[QStringLiteral("debug_log")] = m_debugLogOn == 1;
    env.config = m_config;
    env.configPath = m_options.configPath;
    env.databasePath = m_options.databasePath;
    env.logPath = m_options.debugLogPath.isEmpty() ? paths::debugLogFile() : m_options.debugLogPath;
    env.historyConverting = m_converting;

    env.historyKeys = m_options.keyStore;
    env.matchKeys = m_options.matchKeyStore;
    env.syncKeys = m_options.syncKeyStore;
    env.historyKeyNeeded = m_encrypted || m_config.encryptHistory;
    env.matchKeyNeeded = m_passwords && m_passwords->isSetUp();
    env.syncKeyNeeded = m_config.syncEnabled;

    env.passwordsAvailable = m_passwords != nullptr;
    if (m_passwords) {
        env.passwordsSetUp = m_passwords->isSetUp();
        env.passwordState = !m_passwords->isSetUp() ? QStringLiteral("none (no master password)")
                            : m_passwords->isOpen() ? QStringLiteral("open (no master password: not secure)")
                            : m_passwords->isUnlocked() ? QStringLiteral("unlocked (%1 min left)").arg((m_passwords->secondsLeft() + 59) / 60)
                                                        : QStringLiteral("locked");
        env.canMatch = m_passwords->canMatch();
        env.matchProblem = m_passwords->matchProblem();
    }
    env.lastNameBox = m_lastNameBox;
    env.lockWatching = m_options.screenLockWatcher && m_options.screenLockWatcher->isWatching();
    env.helloName = hello::displayName();

    // Probes that block run on a worker thread: they copy what they need (the Daemon is not touched there).
    const QString dbPath = m_options.databasePath;
    const bool encrypted = m_encrypted;
    env.database = [dbPath, encrypted, store = m_options.keyStore] {
        QByteArray key;
        if (encrypted) {
            QString error;
            std::optional<QByteArray> read = store ? store->read(&error) : std::nullopt;
            if (!read) {
                selftest::DbFacts facts;
                facts.error = QStringLiteral("the history is encrypted, but its key could not be read from the keyring%1")
                                  .arg(error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(error));
                return facts;
            }
            key = *read;
            db::wipe(*read);
        }
        selftest::DbFacts facts = selftest::probeDatabase(dbPath, key);
        db::wipe(key);
        return facts;
    };
    env.configFile = [path = env.configPath] { return selftest::probeConfig(path); };
    env.logFile = [path = env.logPath] { return selftest::probeLogFile(path); };

    const std::shared_ptr<FakeTranslation> fakeTranslation = m_options.fakeTranslation;
    env.translation = [fakeTranslation] {
        selftest::TranslationFacts facts;
        facts.builtIn = fakeTranslation ? true : translate::builtIn();
        if (!facts.builtIn) {
            return facts;
        }
        const QList<translate::Pack> packs = fakeTranslation ? fakeTranslation->packs : translate::PackStore().installed();
        for (const translate::Pack& pack : packs) {
            // Loadable: the files the engine opens are there (a fake pack has no folder and counts as loadable).
            const bool loadable = pack.path.isEmpty()
                || (QFile::exists(pack.path + QStringLiteral("/model/model.bin"))
                    && QFile::exists(pack.path + QStringLiteral("/sentencepiece.model")));
            facts.packs.push_back({pack.id, loadable});
        }
        return facts;
    };

    const std::shared_ptr<FakeOcr> fakeOcr = m_options.fakeOcr;
    env.ocr = [fakeOcr, languages = ocr::parseLanguages(m_config.ocrLanguages)] {
        selftest::OcrFacts facts;
        facts.languages = languages;
        if (fakeOcr) {
            facts.builtIn = facts.available = true;
            return facts;
        }
        facts.builtIn = ocr::builtIn();
        if (facts.builtIn) {
            facts.available = ocr::available(&facts.why);
        }
        return facts;
    };

    if (m_options.session.display == SessionEnv::Display::Windows || m_options.session.display == SessionEnv::Display::MacOS) {
        // Found out when the daemon started (the one thread the WinRT calls belong to): not asked again here.
        env.helloAvailable = [ok = m_helloAvailable, why = m_helloReason](QString* reason) {
            if (reason) {
                *reason = why;
            }
            return ok;
        };
    }
    env.syncFacts = [folder = m_config.syncFolder, store = m_options.syncKeyStore] {
        bool present = false;
        if (store) {
            QString error;
            std::optional<QByteArray> key = store->read(&error);
            present = key.has_value();
            if (key) {
                db::wipe(*key);
            }
        }
        return selftest::probeSyncFolder(folder, present);
    };

    env.pasteTarget = [this] { return currentPasteTarget(); };
    if (m_integration) {
        env.extensionProbe = [this](std::function<void(selftest::ExtensionAnswer)> done) {
            if (!m_integration) {
                done(selftest::ExtensionAnswer::NotAnswering);
                return;
            }
            m_integration->probeFocusedApp([done = std::move(done)](DesktopIntegration::FocusedAppAnswer answer) {
                done(answer == DesktopIntegration::FocusedAppAnswer::Answers ? selftest::ExtensionAnswer::Answers
                     : answer == DesktopIntegration::FocusedAppAnswer::TooOld ? selftest::ExtensionAnswer::TooOld
                                                                              : selftest::ExtensionAnswer::NotAnswering);
            });
        };
    }
    env.captureProbe = [this](std::function<void(selftest::CaptureOutcome)> done) { probeCapture(std::move(done)); };
    if (box && box->prepare && box->text) {
        env.pasteProbe = [this, box = *box](std::function<void(selftest::PasteOutcome)> done) { probePaste(box, std::move(done)); };
    }
    return env;
}

ui::SelfTestSupport Daemon::selfTestSupport()
{
    ui::SelfTestSupport support;
    support.create = [this](const selftest::PasteBox& box) { return createSelfTest(&box); };
    support.copyText = [this](const QString& text) {
        if (m_writer) {
            m_writer->setText(text); // an own write: the capture does not record the report as a copy
        }
    };
    return support;
}

selftest::Runner* Daemon::createSelfTest(const selftest::PasteBox* box)
{
    if (m_selfTest && m_selfTest->isRunning()) {
        return m_selfTest;
    }
    const selftest::Env env = selfTestEnv(box);
    const QString header = QStringLiteral("%1 | %2 | %3").arg(QDateTime::currentDateTime().toString(Qt::ISODate), env.version, env.platform);
    auto* runner = new selftest::Runner(selftest::buildChecks(env), header, this);
    connect(runner, &selftest::Runner::finished, runner, &QObject::deleteLater);
    m_selfTest = runner;
    return runner;
}

void Daemon::probeCapture(std::function<void(selftest::CaptureOutcome)> done)
{
    using selftest::CaptureOutcome;
    if (!m_repo || !m_pipeline) {
        done(CaptureOutcome::NoHistory);
        return;
    }
    if (m_converting || m_pipeline->isPaused()) {
        done(CaptureOutcome::Paused);
        return;
    }
    const QString marker = uniqueMarker(QStringLiteral("self-test copy"));
    const QString hash = db::Repository::hashOf(marker);

    struct State {
        bool finished = false;
        QMetaObject::Connection stored;
        QTimer timer;
    };
    auto state = std::make_shared<State>();
    state->timer.setSingleShot(true);
    const auto finish = [this, state, done = std::move(done)](CaptureOutcome outcome) {
        if (state->finished) {
            return;
        }
        state->finished = true;
        QObject::disconnect(state->stored);
        state->timer.stop();
        if (!integrationAvailable()) {
            // Our test text is taken off the clipboard again, but only if we still own it: something copied meanwhile stays.
            QClipboard* clipboard = QGuiApplication::clipboard();
            if (clipboard->ownsClipboard()) {
                clipboard->clear();
            }
        }
        done(outcome);
    };
    // Only copies that were just stored are looked at, and only to compare their hash with the test copy's: nothing else is read.
    state->stored = connect(m_pipeline.get(), &CapturePipeline::stored, this, [this, state, hash, finish](qint64 id, bool) {
        if (state->finished || !m_repo) {
            return;
        }
        try {
            const std::optional<qint64> ours = m_repo->findByHash(hash);
            if (!ours || *ours != id) {
                return;
            }
            m_repo->forgetNow(id); // secure delete: pages zeroed, search index and write-ahead log folded in
        } catch (const db::DbError& e) {
            qCWarning(lcSelfTest) << "the test copy could not be removed:" << e.what();
        }
        if (m_popup) {
            m_popup->historyChanged();
        }
        finish(CaptureOutcome::Seen);
    });
    QObject::connect(&state->timer, &QTimer::timeout, this, [this, state, hash, finish] {
        // Not seen in time. Take the test copy out anyway if it did get in (a late capture must not stay in the history).
        try {
            if (m_repo) {
                if (const auto ours = m_repo->findByHash(hash)) {
                    m_repo->forgetNow(*ours);
                }
            }
        } catch (const db::DbError&) {
        }
        finish(CaptureOutcome::NotSeen);
    });
    state->timer.start(5000);

    // Written without telling the capture filter, which is the point: the capture has to see this as a copy.
    if (integrationAvailable()) {
        m_integration->setClipboard(marker);
    } else {
        QClipboard* clipboard = QGuiApplication::clipboard();
        clipboard->setText(marker);
        if (m_options.session.display == SessionEnv::Display::Wayland && !clipboard->ownsClipboard()) {
            // Wayland lets only a window with focus own the clipboard: from the CLI nothing was written.
            finish(CaptureOutcome::CannotWrite);
            return;
        }
    }
    qCInfo(lcSelfTest) << "test copy put on the clipboard";
}

void Daemon::probePaste(const selftest::PasteBox& box, std::function<void(selftest::PasteOutcome)> done)
{
    using selftest::PasteOutcome;
    if (!m_config.pasteOnEnter || !m_writer) {
        done(PasteOutcome::Disabled);
        return;
    }
    const QString marker = uniqueMarker(QStringLiteral("paste test"));
    box.prepare();
    m_writer->setText(marker); // an own write: the capture does not record it as a copy
    // The box needs a moment to hold the focus; then the keys go to whatever has it (that is, the box).
    QTimer::singleShot(250, this, [this, box, marker, done = std::move(done)]() mutable {
        rememberPasteTarget(); // Windows brings the remembered window back first: that must be the Settings window
        bool sent = true;
        if (integrationAvailable()) {
            m_integration->paste(m_config.pasteKeys);
        } else if (!(m_options.syntheticPaste ? m_options.syntheticPaste(m_config.pasteKeys)
                                              : pasteWithSyntheticKeys(m_config.pasteKeys))) {
            sent = false;
        }
        if (!sent) {
            done(PasteOutcome::CannotType);
            return;
        }
        // Poll the box for the marker, for up to three seconds.
        auto* poll = new QTimer(this);
        auto remaining = std::make_shared<int>(30);
        connect(poll, &QTimer::timeout, this, [poll, box, marker, remaining, done = std::move(done)]() mutable {
            const bool got = box.text().contains(marker);
            if (got || --*remaining <= 0) {
                poll->deleteLater();
                if (done) {
                    auto finish = std::move(done);
                    done = nullptr;
                    finish(got ? PasteOutcome::Received : PasteOutcome::NotReceived);
                }
            }
        });
        poll->start(100);
    });
}

} // namespace clipitto
