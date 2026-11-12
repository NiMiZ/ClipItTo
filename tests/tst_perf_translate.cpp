// Stalling in translation (the stall the user sees today). The real worker plumbing runs here: the Daemon's translate
// pool, the time-limit guard (translate::runGuarded, also what the CTranslate2 engine goes through), the answer back on
// the GUI thread and the popup's own timeout. Only the engine itself is a fake that can be slow, hang, ignore
// cancellation or take a long time to "load".
//
// This build has no CTranslate2 (translation is off unless CLIPITTO_TRANSLATE_PREFIX is set), so the real model's speed
// is not measured here; what is measured is that nothing in ClipItTo around the engine blocks the GUI thread.
#include "ControlChannel.h"
#include "Daemon.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "StallWatchdog.h"
#include "Translate.h"
#include "db/Repository.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtConcurrent>

#include <atomic>
#include <memory>

using clipitto::Daemon;
using clipitto::DaemonOptions;
using clipitto::FakeTranslation;
using clipitto::ui::PopupWindow;
namespace tr = clipitto::translate;

namespace {

class FakeControl : public clipitto::ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

// A fake engine; its knobs are atomics because the engine runs on the guard's thread.
struct Engine {
    std::atomic<int> calls{0};
    std::atomic<int> sleepMs{0}; // each call takes this long (checks `cancelled` every few ms)
    std::atomic<int> loadMs{0}; // the first call also "loads the model" for this long
    std::atomic<bool> hang{false}; // blocks until `release`
    std::atomic<bool> cooperative{true}; // a hang ends when `cancelled` is set
    std::atomic<bool> release{false};
    std::atomic<bool> sawCancel{false};
    std::atomic<bool> ranOnGuiThread{false};
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    QString hangWord; // a text that hangs (the others do not)
    QThread* gui = nullptr;

    std::optional<QString> run(const tr::Pack& pack, const QString& text, QString* error, const std::atomic<bool>& cancelled)
    {
        if (QThread::currentThread() == gui) {
            ranOnGuiThread = true;
        }
        const int n = ++calls;
        const int now = ++running;
        int seen = maxRunning;
        while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
        }
        struct Done {
            std::atomic<int>& r;
            ~Done() { --r; }
        } done{running};
        const auto wait = [&](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) {
                if (cancelled.load() && cooperative) {
                    sawCancel = true;
                    return false;
                }
                QThread::msleep(2);
            }
            return true;
        };
        if (n == 1 && loadMs > 0 && !wait(loadMs)) {
            if (error) {
                *error = QStringLiteral("cancelled");
            }
            return std::nullopt;
        }
        if (hang || (!hangWord.isEmpty() && text == hangWord)) {
            while (!release) {
                if (cancelled.load() && cooperative) {
                    sawCancel = true;
                    if (error) {
                        *error = QStringLiteral("cancelled");
                    }
                    return std::nullopt;
                }
                QThread::msleep(2);
            }
        }
        if (sleepMs > 0 && !wait(sleepMs)) {
            if (error) {
                *error = QStringLiteral("cancelled");
            }
            return std::nullopt;
        }
        return QStringLiteral("[%1] %2").arg(pack.id, text);
    }
};

struct Rig {
    QTemporaryDir dir;
    std::shared_ptr<Engine> engine = std::make_shared<Engine>();
    std::unique_ptr<Daemon> daemon;
    QStringList warnings;
    QStringList quiet;

    explicit Rig(int timeoutSeconds = 1, int popupTimeoutMs = 30000)
    {
        engine->gui = QThread::currentThread();
        QFile cfg(dir.filePath(QStringLiteral("config.toml")));
        if (!cfg.open(QIODevice::WriteOnly)) {
            qFatal("cannot write the config");
        }
        cfg.write(QByteArray("hide_on_focus_loss = false\ntranslate_timeout = ") + QByteArray::number(timeoutSeconds) + "\n");
        cfg.close();
        auto fake = std::make_shared<FakeTranslation>();
        tr::Pack zh;
        zh.id = QStringLiteral("zh-en");
        zh.from = QStringLiteral("zh");
        zh.to = QStringLiteral("en");
        zh.fromName = QStringLiteral("Chinese");
        zh.toName = QStringLiteral("English");
        tr::Pack he = zh;
        he.id = QStringLiteral("he-en");
        he.from = QStringLiteral("he");
        he.fromName = QStringLiteral("Hebrew");
        fake->packs = {zh, he};
        auto e = engine;
        fake->engine = [e](const tr::Pack& p, const QString& t, QString* err, const std::atomic<bool>& c) {
            return e->run(p, t, err, c);
        };
        DaemonOptions o;
        o.configPath = dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = dir.filePath(QStringLiteral("data/history.db"));
        o.syntheticPaste = [](clipitto::PasteKeys) { return false; };
        o.notify = [](const QString&, const QString&) { return true; };
        o.fakeTranslation = fake;
        daemon = std::make_unique<Daemon>(o, std::make_unique<FakeControl>());
        if (daemon->start() != Daemon::StartResult::Ok) {
            qFatal("the daemon did not start");
        }
        popup()->setTranslateTimeoutMs(popupTimeoutMs);
        popup()->setNotify([this](QWidget*, const QString& m) { warnings << m; });
        popup()->setTextViewer([this](QWidget*, const QString& t, bool) {
            quiet << t;
            return false;
        });
    }

    ~Rig()
    {
        engine->release = true; // let any left-behind run end before the daemon (its pool) goes
        engine->cooperative = true;
        QElapsedTimer t;
        t.start();
        while (tr::guardBusy() && t.elapsed() < 5000) {
            QThread::msleep(5);
        }
        daemon.reset();
    }

    PopupWindow* popup() const { return daemon->popup(); }
    bool show()
    {
        popup()->showPopup();
        return QTest::qWaitForWindowActive(popup());
    }
};

QString hebrewWord()
{
    return QString::fromUtf8("מכשול");
}

} // namespace

class TestPerfTranslate : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        // The guard is process-wide: every test must leave it free for the next one.
        QTRY_VERIFY_WITH_TIMEOUT(!tr::guardBusy(), 6000);
    }

    // The GUI thread is never held while a translation runs (and the engine never runs on it).
    void guiNeverBlockedWhileTranslating()
    {
        Rig r(5);
        r.engine->sleepMs = 700;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("some text"));
        QVERIFY(r.popup()->isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 5000);
        QVERIFY(!r.popup()->isBusy());
        QVERIFY(!r.popup()->isVisible()); // pasted: the popup closed
        QVERIFY(!r.engine->ranOnGuiThread);
        QVERIFY_NO_STALL(wd, "translate: 700 ms engine");
    }

    // The engine's own limit (translate_timeout) fires, the popup says so and is usable again.
    void theEngineTimeoutFiresAndThePopupIsUsableAgain()
    {
        Rig r(1);
        r.engine->hang = true;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        QElapsedTimer t;
        t.start();
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("text"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.warnings.isEmpty(), 4000);
        QVERIFY2(t.elapsed() < 2500, "the 1 s limit must answer in about a second");
        QVERIFY(!r.popup()->isBusy());
        QVERIFY(r.popup()->isVisible());
        QCOMPARE(pasted.size(), 0);
        QVERIFY(r.popup()->searchBar()->isEnabled());
        QVERIFY_NO_STALL(wd, "translate: engine timeout");
    }

    // The popup's own wait is shorter than the engine's: it gives up first, and a late answer is ignored.
    void aLateAnswerIsIgnored()
    {
        Rig r(5, 300);
        r.engine->sleepMs = 1200;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("text"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.warnings.isEmpty(), 3000);
        QVERIFY(!r.popup()->isBusy());
        QTest::qWait(1500); // the engine answers now
        QCOMPARE(pasted.size(), 0);
        QCOMPARE(r.warnings.size(), 1);
        QVERIFY(r.popup()->isVisible());
        QVERIFY_NO_STALL(wd, "translate: late answer");
    }

    // A second translation while the first still runs: it queues behind it on the worker (one thread), neither blocks the
    // GUI thread, both answer, and the engine never runs two at once.
    void aSecondTranslationWhileTheFirstRunsIsQueued()
    {
        Rig r(5);
        r.engine->sleepMs = 400;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("first"));
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("second"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 2, 6000);
        QCOMPARE(r.engine->maxRunning.load(), 1);
        QCOMPARE(r.engine->calls.load(), 2);
        QVERIFY(!r.popup()->isBusy());
        QVERIFY_NO_STALL(wd, "translate: two at once");
    }

    // The first hangs for good (an engine that ignores cancellation): the second is refused with "still running" within
    // its own limit instead of waiting for ever, the GUI thread stays free, and once the hang ends translating works.
    void aSecondTranslationWhileTheFirstIsStuckIsRefused()
    {
        Rig r(1);
        r.engine->hang = true;
        r.engine->cooperative = false;
        QVERIFY(r.show());
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("first"));
        QTRY_VERIFY_WITH_TIMEOUT(r.warnings.size() == 1, 4000);
        QVERIFY(tr::guardBusy()); // the stuck run is still going
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("second"));
        QElapsedTimer t;
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(r.warnings.size() == 2, 4000);
        QVERIFY2(t.elapsed() < 2500, "refused within its own limit");
        QVERIFY(r.warnings.at(1).contains(QStringLiteral("still running")));
        QVERIFY(!r.popup()->isBusy());
        QVERIFY_NO_STALL(wd, "translate: second while stuck");
        // The hang ends: the next translation works again.
        r.engine->hang = false;
        r.engine->release = true;
        QTRY_VERIFY_WITH_TIMEOUT(!tr::guardBusy(), 3000);
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.show();
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("third"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 4000);
    }

    // A run that checks `cancelled` (the CTranslate2 engine does between sentences) is told to stop when the limit
    // passes, so the next translation is not held up behind it.
    void aTimedOutRunIsToldToStopAndFreesTheNext()
    {
        Rig r(1);
        r.engine->hang = true; // cooperative
        QVERIFY(r.show());
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("long"));
        QTRY_VERIFY_WITH_TIMEOUT(r.warnings.size() == 1, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(r.engine->sawCancel.load(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!tr::guardBusy(), 2000);
        r.engine->hang = false;
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.show();
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("next"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
    }

    // Hiding the popup while a translation runs: nothing crashes or stays stuck; the answer is delivered once.
    void hidingThePopupMidTranslation()
    {
        Rig r(5);
        r.engine->sleepMs = 500;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("text"));
        r.popup()->hidePopup();
        QVERIFY(r.popup()->isBusy()); // still waiting, hidden
        r.popup()->showPopup(); // and shown again meanwhile: keeps the busy message, no queries
        QVERIFY(r.popup()->isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 4000);
        QVERIFY(!r.popup()->isBusy());
        QTest::qWait(100);
        QCOMPARE(pasted.size(), 1);
        QVERIFY_NO_STALL(wd, "translate: hide mid-translation");
    }

    // Destroying the popup's daemon while a translation runs: the late answer must not touch a dead popup.
    void closingWhileTranslatingDoesNotCrash()
    {
        auto r = std::make_unique<Rig>(5);
        r->engine->sleepMs = 300;
        QVERIFY(r->show());
        r->popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("text"));
        r.reset(); // waits for the run (<= 300 ms), then everything is gone
        QTest::qWait(50);
    }

    // A very long text is refused at once (it could never finish inside the limit); the worker is not occupied.
    void aVeryLongTextIsRefusedAtOnce()
    {
        Rig r(5);
        QVERIFY(r.show());
        const QString big = QString(1000000, QLatin1Char('a')); // 1 MB
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        QElapsedTimer t;
        t.start();
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), big);
        QVERIFY(t.elapsed() < StallWatchdog::scaled(200));
        QCOMPARE(r.warnings.size(), 1);
        QVERIFY(r.warnings.at(0).contains(QStringLiteral("too long")));
        QVERIFY(!r.popup()->isBusy());
        QCOMPARE(r.engine->calls.load(), 0);
        QVERIFY(!tr::guardBusy());
        QCOMPARE(pasted.size(), 0);
        QVERIFY_NO_STALL(wd, "translate: 1 MB text refused");
        // Just under the cap goes through.
        r.engine->sleepMs = 10;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QString(tr::kMaxTranslateChars, QLatin1Char('b')));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
    }

    // One word that hangs (מכשול): the popup stays usable, other words translate afterwards.
    void aSingleWordThatHangs()
    {
        Rig r(1);
        r.engine->hangWord = hebrewWord();
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("he-en"), hebrewWord());
        QTRY_VERIFY_WITH_TIMEOUT(r.warnings.size() == 1, 4000);
        QVERIFY(r.warnings.at(0).contains(QStringLiteral("took longer")));
        QVERIFY(!r.popup()->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!tr::guardBusy(), 3000); // told to stop, it ended
        r.show();
        r.popup()->translateAndPaste(QStringLiteral("he-en"), QString::fromUtf8("שלום"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
        QVERIFY_NO_STALL(wd, "translate: one hanging word");
    }

    // Many translations in a row: no stall, no pile-up of helper objects, a bounded total time.
    void manyTranslationsInARow()
    {
        Rig r(5);
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 60; ++i) {
            r.show();
            r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("word %1").arg(i));
            QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), i + 1, 3000);
        }
        QVERIFY2(t.elapsed() < StallWatchdog::scaled(6000), "60 instant translations take about a second");
        QTest::qWait(50);
        QVERIFY(r.daemon->findChildren<QFutureWatcherBase*>().isEmpty()); // every watcher cleaned up
        QVERIFY(!r.popup()->isBusy());
        QVERIFY_NO_STALL(wd, "translate: 60 in a row");
    }

    // The model load on first use (a second or two) happens on the worker, not on the GUI thread.
    void theFirstUseLoadsOffTheGuiThread()
    {
        Rig r(5);
        r.engine->loadMs = 800;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        StallWatchdog wd;
        r.popup()->translateAndPaste(QStringLiteral("zh-en"), QStringLiteral("first use"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 4000);
        QVERIFY(!r.engine->ranOnGuiThread);
        QVERIFY_NO_STALL(wd, "translate: first use (800 ms load)");
    }

    // A picture: its text is read first (slow, off the GUI thread), then translated.
    void translatingAPicturesText()
    {
        Rig r(5);
        r.engine->sleepMs = 300;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->setTextFromPicture(
            [](qint64, std::function<void(const QString&, const QString&)> done) {
                auto* watcher = new QFutureWatcher<int>();
                QObject::connect(watcher, &QFutureWatcherBase::finished, [watcher, done] {
                    done(QString::fromUtf8("你好世界 ok"), QString());
                    watcher->deleteLater();
                });
                watcher->setFuture(QtConcurrent::run([] {
                    QThread::msleep(500); // OCR
                    return 0;
                }));
            },
            true, QString());
        StallWatchdog wd;
        r.popup()->translatePictureAndPaste(1, QStringLiteral("zh-en"));
        QVERIFY(r.popup()->isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 5000);
        QVERIFY(!r.popup()->isBusy());
        QVERIFY_NO_STALL(wd, "translate: picture (500 ms OCR + 300 ms)");
    }

    // The pack lookups the popup does on the GUI thread (menu, Ctrl+T) stay cheap even for a huge copy:
    // the script of the text is looked at, which must not scan megabytes.
    void scriptOfAHugeTextIsCheap()
    {
        const QString huge = QString(5 * 1024 * 1024, QLatin1Char('x')); // a 5 MB copy
        tr::Pack zh;
        zh.from = QStringLiteral("zh");
        bool fits = true;
        QVERIFY_FAST(50, "translate: script of a 5 MB text", fits = tr::fits(zh, huge));
        QVERIFY(!fits);
    }

    // PackStore::installed() runs on the GUI thread whenever the menu or Ctrl+T asks: 30 packs stay quick.
    void listingThePacksIsCheap()
    {
        QTemporaryDir dir;
        for (int i = 0; i < 30; ++i) {
            const QString id = QStringLiteral("l%1-en").arg(i, 2, 10, QLatin1Char('0'));
            QVERIFY(QDir().mkpath(dir.filePath(id + QStringLiteral("/model"))));
            QFile meta(dir.filePath(id + QStringLiteral("/metadata.json")));
            QVERIFY(meta.open(QIODevice::WriteOnly));
            meta.write(QByteArray("{\"from_code\":\"l%1\",\"to_code\":\"en\",\"from_name\":\"L\",\"to_name\":\"English\"}")
                           .replace("%1", QByteArray::number(i)));
            meta.close();
            QFile sp(dir.filePath(id + QStringLiteral("/sentencepiece.model")));
            QVERIFY(sp.open(QIODevice::WriteOnly));
            sp.write("x");
        }
        tr::PackStore store(dir.path());
        qsizetype n = 0;
        QVERIFY_FAST(100, "translate: list 30 packs", n = store.installed().size());
        QVERIFY(n >= 0);
    }
};

QTEST_MAIN(TestPerfTranslate)
#include "tst_perf_translate.moc"
