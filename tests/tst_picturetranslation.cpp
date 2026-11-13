// Picture search (reading the text in copied pictures) and translation, as the user meets them: a real Daemon (its
// pipeline, repository, popup, worker pools and clipboard writer) with a fake text recognition (DaemonOptions::fakeOcr)
// and a fake translation engine (DaemonOptions::fakeTranslation), because CI has neither Tesseract nor CTranslate2.
// What is checked is ClipItTo's behaviour around the engines (which pack, which text goes in, what is pasted, what is
// stored), not the quality of a model. Texts are synthetic and never logged.
#include "ControlChannel.h"
#include "Daemon.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "TranslateCheck.h"
#include "Translate.h"
#include "db/Repository.h"

#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <mutex>

using clipitto::Daemon;
using clipitto::DaemonOptions;
using clipitto::FakeOcr;
using clipitto::FakeTranslation;
using clipitto::ui::PopupWindow;
namespace tr = clipitto::translate;

namespace {

class FakeControl : public clipitto::ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

QString hebrew(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

tr::Pack pack(const QString& from, const QString& fromName)
{
    tr::Pack p;
    p.id = from + QStringLiteral("-en");
    p.from = from;
    p.to = QStringLiteral("en");
    p.fromName = fromName;
    p.toName = QStringLiteral("English");
    return p;
}

QByteArray picture(int n)
{
    QImage img(60 + n, 30, QImage::Format_RGB32);
    img.fill(QColor::fromHsv(n * 53 % 360, 200, 200));
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

// A fake recognition: what it was asked, and what it answers.
struct Ocr {
    std::atomic<int> calls{0};
    std::atomic<bool> onGuiThread{false};
    std::atomic<bool> hold{false}; // blocks until released
    std::atomic<bool> fail{false};
    std::mutex mutex;
    QStringList languages; // of the last call
    QString answer = QStringLiteral("Invoice 4711 total due");
    QThread* gui = nullptr;

    std::optional<QString> run(const QByteArray&, const QStringList& langs, QString* error)
    {
        ++calls;
        if (QThread::currentThread() == gui) {
            onGuiThread = true;
        }
        while (hold) {
            QThread::msleep(2);
        }
        QString out;
        {
            std::lock_guard<std::mutex> lock(mutex);
            languages = langs;
            out = answer;
        }
        if (fail) {
            if (error) {
                *error = QStringLiteral("no language data");
            }
            return std::nullopt;
        }
        return out;
    }
};

// A fake translation engine that records what it is given and answers "[<pack>]<text>" (or what `reply` says).
struct Engine {
    std::mutex mutex;
    QStringList texts;
    QStringList packs;
    std::function<std::optional<QString>(const QString&, QString*)> reply;

    std::optional<QString> run(const tr::Pack& p, const QString& text, QString* error, const std::atomic<bool>&)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            texts << text;
            packs << p.id;
        }
        if (reply) {
            return reply(text, error);
        }
        return QStringLiteral("[%1]%2").arg(p.id, text);
    }
    int calls()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return int(texts.size());
    }
};

struct Rig {
    QTemporaryDir dir;
    std::shared_ptr<Ocr> ocr = std::make_shared<Ocr>();
    std::shared_ptr<Engine> engine = std::make_shared<Engine>();
    std::unique_ptr<Daemon> daemon;
    QStringList warnings;
    QStringList quiet;
    bool pasteChosen = false;

    // `config`: more config.toml lines; `packs`: the installed fake packs (default zh and he).
    explicit Rig(const QByteArray& config = {}, QList<tr::Pack> packs = {pack(QStringLiteral("zh"), QStringLiteral("Chinese")), pack(QStringLiteral("he"), QStringLiteral("Hebrew"))})
    {
        ocr->gui = QThread::currentThread();
        writeConfig(config);
        auto fake = std::make_shared<FakeTranslation>();
        fake->packs = std::move(packs);
        auto e = engine;
        fake->engine = [e](const tr::Pack& p, const QString& t, QString* err, const std::atomic<bool>& c) { return e->run(p, t, err, c); };
        auto o = ocr;
        auto fakeOcr = std::make_shared<FakeOcr>();
        fakeOcr->recognize = [o](const QByteArray& png, const QStringList& l, QString* err) { return o->run(png, l, err); };
        DaemonOptions opts;
        opts.configPath = dir.filePath(QStringLiteral("config.toml"));
        opts.databasePath = dir.filePath(QStringLiteral("data/history.db"));
        opts.syntheticPaste = [](clipitto::PasteKeys) { return false; };
        opts.notify = [](const QString&, const QString&) { return true; };
        opts.fakeTranslation = fake;
        opts.fakeOcr = fakeOcr;
        opts.debugLogPath = dir.filePath(QStringLiteral("debug.log"));
        daemon = std::make_unique<Daemon>(opts, std::make_unique<FakeControl>());
        if (daemon->start() != Daemon::StartResult::Ok) {
            qFatal("the daemon did not start");
        }
        popup()->setNotify([this](QWidget*, const QString& m) { warnings << m; });
        popup()->setTextViewer([this](QWidget*, const QString& t, bool) {
            quiet << t;
            return pasteChosen;
        });
    }

    ~Rig()
    {
        ocr->hold = false;
        daemon.reset();
    }

    void writeConfig(const QByteArray& extra)
    {
        QFile cfg(dir.filePath(QStringLiteral("config.toml")));
        if (!cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qFatal("cannot write the config");
        }
        cfg.write("hide_on_focus_loss = false\n" + extra);
        cfg.close();
    }
    void reload(const QByteArray& extra)
    {
        writeConfig(extra);
        emit daemon->control()->reloadRequested();
    }

    PopupWindow* popup() const { return daemon->popup(); }
    clipitto::db::Repository& repo() const { return *daemon->repository(); }
    qint64 copyText(const QString& text)
    {
        emit daemon->control()->ingestRequested(text, {QStringLiteral("text/plain")});
        return repo().recent(1, false).front().id;
    }
    qint64 copyPicture(int n)
    {
        const qsizetype before = repo().count();
        emit daemon->control()->ingestImageRequested(picture(n), QStringLiteral("image/png"), {QStringLiteral("image/png")});
        for (int i = 0; i < 500 && repo().count() == before; ++i) {
            QTest::qWait(10);
        }
        return repo().recent(1, false).front().id;
    }
    // The entry selected in the popup, as Ctrl+T / the menus see it.
    void select(qint64 id)
    {
        popup()->view()->selectIndex(popup()->model()->findEntry(id));
    }
    bool show()
    {
        popup()->showPopup();
        return QTest::qWaitForWindowActive(popup());
    }
    // The pack labels offered in the "Translate" submenu of the entry (empty: none, or the entry is disabled).
    QStringList translateMenu(qint64 id, QString* tooltip = nullptr, bool* enabled = nullptr, const QString& title = QStringLiteral("Translate"))
    {
        const clipitto::Entry e = *repo().get(id);
        std::unique_ptr<QMenu> menu(popup()->createContextMenu(e, popup()));
        QStringList labels;
        for (QAction* a : menu->actions()) {
            if (!a->menu() || a->text().remove(QLatin1Char('&')) != QLatin1String("Paste as")) {
                continue;
            }
            for (QAction* b : a->menu()->actions()) {
                if (b->text().remove(QLatin1Char('&')) != title) {
                    continue;
                }
                if (tooltip) {
                    *tooltip = b->toolTip();
                }
                if (enabled) {
                    *enabled = b->isEnabled();
                }
                if (b->menu()) {
                    for (QAction* p : b->menu()->actions()) {
                        if (!p->objectName().startsWith(QLatin1String("translate_preview"))) {
                            labels << p->text();
                        }
                    }
                }
            }
        }
        return labels;
    }
    // Chooses `label` in the Translate submenu of a text entry (or the picture entry with `title`).
    bool choose(qint64 id, const QString& label, const QString& title = QStringLiteral("Translate"))
    {
        const clipitto::Entry e = *repo().get(id);
        std::unique_ptr<QMenu> menu(popup()->createContextMenu(e, popup()));
        for (QAction* a : menu->actions()) {
            if (!a->menu() || a->text().remove(QLatin1Char('&')) != QLatin1String("Paste as")) {
                continue;
            }
            for (QAction* b : a->menu()->actions()) {
                if (b->menu() && b->text().remove(QLatin1Char('&')) == title) {
                    for (QAction* p : b->menu()->actions()) {
                        if (p->text() == label) {
                            p->trigger();
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }
};

} // namespace

class TestPictureTranslation : public QObject {
    Q_OBJECT

    static QStringList ids(const std::vector<clipitto::Entry>& found)
    {
        QStringList out;
        for (const auto& e : found) {
            out << QString::number(e.id);
        }
        return out;
    }

private slots:
    // ---------------------------------------------------------------- picture search

    void textInACopiedPictureIsReadInTheBackgroundAndFound()
    {
        Rig r;
        r.ocr->hold = true; // reading is slow: the copy must not wait for it
        QElapsedTimer t;
        t.start();
        const qint64 id = r.copyPicture(1);
        QVERIFY(t.elapsed() < 1000);
        QVERIFY(r.repo().search(QStringLiteral("invoice")).empty()); // not read yet
        r.ocr->hold = false;
        QTRY_VERIFY_WITH_TIMEOUT(!r.repo().search(QStringLiteral("invoice")).empty(), 5000);
        QCOMPARE(ids(r.repo().search(QStringLiteral("invoice 4711"))), QStringList{QString::number(id)});
        QVERIFY(r.repo().search(QStringLiteral("invoice"))[0].image);
        QVERIFY(!r.ocr->onGuiThread);
        QCOMPARE(int(r.ocr->calls), 1);
    }

    void withIndexPicturesOffNothingIsRead()
    {
        Rig r("index_pictures = false\n");
        r.copyPicture(2);
        QTest::qWait(500);
        QCOMPARE(int(r.ocr->calls), 0);
        QVERIFY(r.repo().search(QStringLiteral("invoice")).empty());
        QCOMPARE(r.repo().search(QStringLiteral("image")).size(), size_t(1)); // still found by its label
    }

    void theOcrLanguagesSettingGoesToTheEngine()
    {
        Rig r("ocr_languages = \"en, he\"\n");
        r.copyPicture(3);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        std::lock_guard<std::mutex> lock(r.ocr->mutex);
        QCOMPARE(r.ocr->languages, (QStringList{QStringLiteral("en"), QStringLiteral("he")}));
    }

    void withoutALanguagesSettingTheEngineChoosesItself()
    {
        Rig r;
        r.copyPicture(3);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        std::lock_guard<std::mutex> lock(r.ocr->mutex);
        QVERIFY(r.ocr->languages.isEmpty());
    }

    void aPictureWithoutTextIsSearchableByItsLabelOnly()
    {
        Rig r;
        r.ocr->answer.clear();
        const qint64 id = r.copyPicture(4);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        QTest::qWait(200);
        QVERIFY(r.repo().search(QStringLiteral("invoice")).empty());
        QCOMPARE(ids(r.repo().search(QStringLiteral("image"))), QStringList{QString::number(id)});
        QTest::qWait(300);
        QCOMPARE(int(r.ocr->calls), 1); // "read, nothing found" is remembered: not read again
    }

    void aReadingFailureStopsIndexingUntilTheSettingsChange()
    {
        Rig r;
        r.ocr->fail = true;
        r.copyPicture(5);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        r.copyPicture(6);
        QTest::qWait(300);
        QCOMPARE(int(r.ocr->calls), 1); // not hammered
        r.ocr->fail = false;
        r.reload("ocr_languages = \"en\"\n");
        QTRY_VERIFY_WITH_TIMEOUT(!r.repo().search(QStringLiteral("invoice")).empty(), 5000);
    }

    // Found while testing: a picture that was being read when index_pictures went off was recorded as "read, nothing
    // found" and never indexed again after the setting came back on.
    void switchingIndexingOffWhileAPictureIsReadLeavesItUnread()
    {
        Rig r;
        r.ocr->hold = true;
        r.copyPicture(7);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        r.reload("index_pictures = false\n");
        r.ocr->hold = false;
        QTest::qWait(300);
        QVERIFY(r.repo().search(QStringLiteral("invoice")).empty());
        r.reload("index_pictures = true\n");
        QTRY_VERIFY_WITH_TIMEOUT(!r.repo().search(QStringLiteral("invoice")).empty(), 5000);
        QCOMPARE(int(r.ocr->calls), 2);
    }

    // ---------------------------------------------------------------- translation: which pack

    void aHebrewWordOffersTheHebrewPackOnly()
    {
        Rig r;
        const qint64 id = r.copyText(hebrew("מכשול"));
        QCOMPARE(r.translateMenu(id), QStringList{QStringLiteral("Hebrew → English")});
    }

    void packsForEachScript()
    {
        QList<tr::Pack> all;
        const QList<QPair<QString, QString>> langs = {{"en", "English"}, {"he", "Hebrew"}, {"ru", "Russian"}, {"el", "Greek"},
                                                      {"zh", "Chinese"}, {"ar", "Arabic"}, {"ja", "Japanese"}, {"ko", "Korean"}};
        for (const auto& l : langs) {
            all << pack(l.first, l.second);
        }
        Rig r({}, all);
        const auto menuFor = [&](const QString& text) { return r.translateMenu(r.copyText(text)); };
        QCOMPARE(menuFor(QStringLiteral("Hello world")), QStringList{QStringLiteral("English → English")});
        QCOMPARE(menuFor(hebrew("שלום עולם")), QStringList{QStringLiteral("Hebrew → English")});
        QCOMPARE(menuFor(hebrew("Привет мир")), QStringList{QStringLiteral("Russian → English")});
        QCOMPARE(menuFor(hebrew("Γειά σου κόσμε")), QStringList{QStringLiteral("Greek → English")});
        QCOMPARE(menuFor(hebrew("你好世界")), QStringList{QStringLiteral("Chinese → English")});
        QCOMPARE(menuFor(hebrew("مرحبا بالعالم")), QStringList{QStringLiteral("Arabic → English")});
        QCOMPARE(menuFor(hebrew("こんにちは世界")), QStringList{QStringLiteral("Japanese → English")});
        QCOMPARE(menuFor(hebrew("안녕하세요")), QStringList{QStringLiteral("Korean → English")});
    }

    void aTextOfDigitsOnlyOffersNoPack()
    {
        Rig r;
        const qint64 id = r.copyText(QStringLiteral("4711 0815 12:30"));
        QString tip;
        bool enabled = true;
        QVERIFY(r.translateMenu(id, &tip, &enabled).isEmpty());
        QVERIFY(!enabled);
        QVERIFY(tip.contains(QStringLiteral("No installed language pack"))); // packs exist, none fits
    }

    void aTextThatIsMostlyDigitsWithOneWordStillFollowsItsLetters()
    {
        Rig r;
        const qint64 id = r.copyText(hebrew("12345 67890 11 22 33 44 55 שלום"));
        QCOMPARE(r.translateMenu(id), QStringList{QStringLiteral("Hebrew → English")});
    }

    void mixedLanguageTextFollowsTheMajorityScript()
    {
        Rig r;
        QCOMPARE(r.translateMenu(r.copyText(hebrew("אני משתמש ב-Docker כל יום בעבודה"))), QStringList{QStringLiteral("Hebrew → English")});
        QCOMPARE(r.translateMenu(r.copyText(hebrew("I use kubectl and Docker every day, שלום"))).size(), 0); // Latin: no English pack here
    }

    void theWrongDirectionIsExplainedNotTranslated()
    {
        Rig r({}, {pack(QStringLiteral("he"), QStringLiteral("Hebrew"))});
        const qint64 id = r.copyText(QStringLiteral("hello there"));
        QString tip;
        bool enabled = true;
        QVERIFY(r.translateMenu(id, &tip, &enabled).isEmpty());
        QVERIFY(!enabled);
        QVERIFY(tip.contains(QStringLiteral("No installed language pack translates from the language of this text")));
        QVERIFY(r.show());
        r.select(id);
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->previewTranslation();
        QCOMPARE(r.quiet.size(), 1);
        QVERIFY(r.quiet[0].contains(QStringLiteral("No installed language pack")));
        QCOMPARE(r.engine->calls(), 0);
        QCOMPARE(pasted.size(), 0);
    }

    void aMissingPackIsExplained()
    {
        Rig r({}, {});
        const qint64 id = r.copyText(QStringLiteral("hello"));
        QString tip;
        bool enabled = true;
        r.translateMenu(id, &tip, &enabled);
        QVERIFY(!enabled);
        QVERIFY(tip.contains(QStringLiteral("Settings")));
        QVERIFY(r.show());
        r.select(id);
        r.popup()->previewTranslation();
        QCOMPARE(r.quiet.size(), 1);
        QVERIFY(r.quiet[0].contains(QStringLiteral("No language pack is installed")));
    }

    void aPackRemovedAfterTheMenuWasOpenedIsReportedNotSwallowed()
    {
        Rig r;
        r.copyText(QStringLiteral("x")); // makes the entry list non-empty
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->translateAndPaste(QStringLiteral("xx-en"), QStringLiteral("hello"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.warnings.isEmpty(), 3000);
        QVERIFY(r.warnings[0].contains(QStringLiteral("not installed")));
        QVERIFY(!r.popup()->isBusy());
        QCOMPARE(pasted.size(), 0);
    }

    // ---------------------------------------------------------------- translation: the text that goes in and comes out

    void theEngineGetsTheCopyExactlyAsItWasCopied()
    {
        Rig r;
        const QString text = hebrew("  מכשול  \n\n\tשורה שנייה.\r\n\"ציטוט\" (סוגריים)\n  ");
        const qint64 id = r.copyText(text);
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        QVERIFY(r.choose(id, QStringLiteral("Hebrew → English")));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
        QCOMPARE(r.engine->texts, QStringList{repo_content(r, id)});
        // The stored content may be normalised by the pipeline; the engine must get exactly the stored copy, and the
        // result goes out unchanged (no trimming, no line breaks lost).
        QCOMPARE(pasted.at(0).at(0).toString(), QStringLiteral("[he-en]") + repo_content(r, id));
        QVERIFY(repo_content(r, id).contains(QLatin1Char('\n')));
    }

    void oneCharacterTextsTranslate()
    {
        Rig r;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        const qint64 id = r.copyText(hebrew("א"));
        QVERIFY(r.choose(id, QStringLiteral("Hebrew → English")));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
        QCOMPARE(pasted.at(0).at(0).toString(), QStringLiteral("[he-en]") + hebrew("א"));
        // A single Latin letter has no pack in this rig, and a single digit no letters at all.
        QVERIFY(r.translateMenu(r.copyText(QStringLiteral("a"))).isEmpty());
        QVERIFY(r.translateMenu(r.copyText(QStringLiteral("7"))).isEmpty());
    }

    void anEmptyOrBlankTranslationIsSaidNotPasted()
    {
        Rig r;
        r.engine->reply = [](const QString&, QString*) { return std::optional<QString>(QStringLiteral(" \n ")); };
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->translateAndPaste(QStringLiteral("he-en"), hebrew("שלום"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.quiet.isEmpty(), 3000);
        QCOMPARE(r.quiet[0], QStringLiteral("The translation is empty."));
        QCOMPARE(pasted.size(), 0);
        QVERIFY(!r.popup()->isBusy());
    }

    void anEngineErrorIsShownAndNothingIsPastedOrCopied()
    {
        Rig r;
        r.engine->reply = [](const QString&, QString* err) {
            *err = QStringLiteral("model could not be loaded");
            return std::optional<QString>();
        };
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        QTest::qWait(50);
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->translateAndPaste(QStringLiteral("he-en"), hebrew("שלום"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.warnings.isEmpty(), 3000);
        QCOMPARE(r.warnings[0], QStringLiteral("model could not be loaded"));
        QCOMPARE(pasted.size(), 0);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("before"));
        // And an engine that gives no reason still gets a message.
        r.warnings.clear();
        r.engine->reply = [](const QString&, QString*) { return std::optional<QString>(); };
        r.popup()->translateAndPaste(QStringLiteral("he-en"), hebrew("שלום"));
        QTRY_VERIFY_WITH_TIMEOUT(!r.warnings.isEmpty(), 3000);
        QVERIFY(!r.warnings[0].isEmpty());
    }

    // ---------------------------------------------------------------- translation: the result

    void aTranslatedPasteGoesToTheClipboardWithoutASecondHistoryEntry()
    {
        Rig r;
        const qint64 id = r.copyText(hebrew("מכשול"));
        const qsizetype before = r.repo().count();
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        QVERIFY(r.choose(id, QStringLiteral("Hebrew → English")));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(), QStringLiteral("[he-en]") + hebrew("מכשול"), 3000);
        QTest::qWait(400); // the clipboard monitor sees the write: it must not store it
        QCOMPARE(r.repo().count(), before);
        QCOMPARE(r.repo().get(id)->content, hebrew("מכשול")); // the stored copy stays as it was
        QVERIFY(!r.popup()->isBusy());
        QVERIFY(!r.popup()->isVisible()); // hidden after the paste
    }

    void aTranslationShownOnlyIsNotPastedOrCopied()
    {
        Rig r;
        const qint64 id = r.copyText(hebrew("מכשול"));
        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));
        QTest::qWait(50);
        QVERIFY(r.show());
        r.select(id);
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->previewTranslation();
        QTRY_COMPARE_WITH_TIMEOUT(r.quiet.size(), 1, 3000);
        QCOMPARE(r.quiet[0], QStringLiteral("[he-en]") + hebrew("מכשול"));
        QCOMPARE(pasted.size(), 0);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("untouched"));
    }

    void theSameTextTranslatedTwiceIsTranslatedTwice()
    {
        // No cache: a changed pack or a better model must not serve an old answer.
        Rig r;
        QVERIFY(r.show());
        int n = 0;
        r.engine->reply = [&n](const QString& t, QString*) { return std::optional<QString>(t + QString::number(++n)); };
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        r.popup()->translateAndPaste(QStringLiteral("he-en"), hebrew("שלום"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 3000);
        QVERIFY(r.show());
        r.popup()->translateAndPaste(QStringLiteral("he-en"), hebrew("שלום"));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 2, 3000);
        QCOMPARE(pasted.at(0).at(0).toString(), hebrew("שלום") + QStringLiteral("1"));
        QCOMPARE(pasted.at(1).at(0).toString(), hebrew("שלום") + QStringLiteral("2"));
    }

    // ---------------------------------------------------------------- translating a picture

    void theLanguageOfAPicturesTextDecidesThePack()
    {
        Rig r;
        r.ocr->answer = hebrew("מכשול בדרך");
        const qint64 pic = r.copyPicture(8);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000); // read for the search
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        // Every pack is offered (the text is not known yet); the Chinese one does not fit what was read.
        QCOMPARE(r.translateMenu(pic, nullptr, nullptr, QStringLiteral("Translate text from picture")),
                 (QStringList{QStringLiteral("Chinese → English"), QStringLiteral("Hebrew → English")}));
        QVERIFY(r.choose(pic, QStringLiteral("Chinese → English"), QStringLiteral("Translate text from picture")));
        QTRY_COMPARE_WITH_TIMEOUT(r.quiet.size(), 1, 5000);
        QVERIFY(r.quiet[0].contains(QStringLiteral("does not look like")));
        QCOMPARE(r.engine->calls(), 0);
        QCOMPARE(pasted.size(), 0);
        QVERIFY(!r.popup()->isBusy());
        QVERIFY(r.show());
        QVERIFY(r.choose(pic, QStringLiteral("Hebrew → English"), QStringLiteral("Translate text from picture")));
        QTRY_COMPARE_WITH_TIMEOUT(pasted.size(), 1, 5000);
        QCOMPARE(pasted.at(0).at(0).toString(), QStringLiteral("[he-en]") + hebrew("מכשול בדרך"));
    }

    void aPictureWithoutTextSaysSo()
    {
        Rig r;
        r.ocr->answer.clear();
        const qint64 pic = r.copyPicture(9);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        QVERIFY(r.show());
        QVERIFY(r.choose(pic, QStringLiteral("Hebrew → English"), QStringLiteral("Translate text from picture")));
        QTRY_COMPARE_WITH_TIMEOUT(r.quiet.size(), 1, 5000);
        QCOMPARE(r.quiet[0], QStringLiteral("No text found in the picture."));
        QCOMPARE(r.engine->calls(), 0);
    }

    // ---------------------------------------------------------------- the quality check tool (src/daemon/TranslateCheck)

    void theWordListSkipsCommentsAndBlankLines()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("words.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("\xEF\xBB\xBF# a comment\r\n\xD7\x9E\xD7\x9B\xD7\xA9\xD7\x95\xD7\x9C\r\n\r\n  hello world  \n#another\n42");
        f.close();
        QString error;
        const auto words = clipitto::translatecheck::readWordList(f.fileName(), &error);
        QVERIFY2(words.has_value(), qPrintable(error));
        QCOMPARE(*words, (QStringList{hebrew("מכשול"), QStringLiteral("hello world"), QStringLiteral("42")}));
        QVERIFY(!clipitto::translatecheck::readWordList(dir.filePath(QStringLiteral("missing.txt")), &error));
        QVERIFY(!error.isEmpty());
    }

    void theCheckReportsInputOutputTimeAndFlagsTheBadOnes()
    {
        namespace tc = clipitto::translatecheck;
        tc::Engine e;
        e.packs = [] { return QList<tr::Pack>{pack(QStringLiteral("he"), QStringLiteral("Hebrew")), pack(QStringLiteral("zh"), QStringLiteral("Chinese"))}; };
        e.slowMs = 50;
        e.translate = [](const tr::Pack& p, const QString& text, QString* err) -> std::optional<QString> {
            if (text == QString::fromUtf8("悪い")) {
                *err = QStringLiteral("boom");
                return std::nullopt;
            }
            if (text == hebrew("איטי")) { // slow
                QThread::msleep(250);
            }
            if (text == hebrew("כמו שהיה")) {
                return text;
            }
            return QStringLiteral("T(%1:%2)").arg(p.id, text);
        };
        QStringList lines;
        const auto sum = tc::run({hebrew("מכשול"), hebrew("כמו שהיה"), hebrew("איטי"), QStringLiteral("你好"), QStringLiteral("hello"), QStringLiteral("a\nb")},
                                 QString(), [&lines](const QString& l) { lines << l; }, e);
        QCOMPARE(sum.total, 6);
        QCOMPARE(sum.translated, 5 - 1); // "hello": no English pack; "a\nb" is Latin too
        QCOMPARE(sum.failed, 2);
        QCOMPARE(sum.unchanged, 1);
        QCOMPARE(sum.slow, 1);
        QVERIFY(lines[0].startsWith(hebrew("מכשול") + QStringLiteral(" -> T(he-en:") + hebrew("מכשול") + QStringLiteral(") (")));
        QVERIFY(lines[0].contains(QStringLiteral(" ms, he-en)")));
        QVERIFY(lines[1].endsWith(QStringLiteral("UNCHANGED")));
        QVERIFY(lines[2].endsWith(QStringLiteral("SLOW")));
        QVERIFY(lines[3].contains(QStringLiteral("T(zh-en:")));
        QVERIFY(lines[4].contains(QStringLiteral("NO PACK")));
        QVERIFY(lines.last().startsWith(QStringLiteral("# 6 checked")));
        // One pack only.
        lines.clear();
        const auto onlyHe = tc::run({hebrew("מכשול"), QStringLiteral("你好")}, QStringLiteral("he-en"), [&lines](const QString& l) { lines << l; }, e);
        QCOMPARE(onlyHe.translated, 1);
        QVERIFY(lines[1].contains(QStringLiteral("NO PACK")));
        // An engine error is shown with its reason.
        lines.clear();
        e.packs = [] { return QList<tr::Pack>{pack(QStringLiteral("ja"), QStringLiteral("Japanese"))}; };
        tc::run({QString::fromUtf8("悪い")}, QString(), [&lines](const QString& l) { lines << l; }, e);
        QVERIFY(lines[0].contains(QStringLiteral("ERROR: boom (")));
    }

    void theCheckCommandWritesAFileAndRefusesWhatCannotRun()
    {
        namespace tc = clipitto::translatecheck;
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("words.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n");
        f.close();
        tc::Engine e;
        e.packs = [] { return QList<tr::Pack>{pack(QStringLiteral("he"), QStringLiteral("Hebrew"))}; };
        e.translate = [](const tr::Pack&, const QString& t, QString*) { return std::optional<QString>(t + QStringLiteral("!")); };
        const QString out = dir.filePath(QStringLiteral("report.txt"));
        QCOMPARE(tc::runCommand(f.fileName(), QString(), out, e), 0);
        QFile report(out);
        QVERIFY(report.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(report.readAll());
        report.close();
        QVERIFY(text.contains(hebrew("שלום -> שלום!")));
        QVERIFY(text.contains(QStringLiteral("# 1 checked")));
        QCOMPARE(tc::runCommand(dir.filePath(QStringLiteral("none.txt")), QString(), out, e), 2);
        QCOMPARE(tc::runCommand(f.fileName(), QStringLiteral("zh-en"), out, e), 2); // that pack is not installed
        e.packs = [] { return QList<tr::Pack>{}; };
        QCOMPARE(tc::runCommand(f.fileName(), QString(), out, e), 2); // no packs at all
    }

    // ---------------------------------------------------------------- the panic key: late answers are ignored

    void aPanicWhileATextIsBeingTranslatedDropsTheAnswer()
    {
        Rig r;
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto finished = std::make_shared<std::atomic<int>>(0);
        r.engine->reply = [hold, finished](const QString& text, QString*) {
            while (*hold) {
                QThread::msleep(2);
            }
            ++*finished;
            return std::optional<QString>(text + QStringLiteral(" translated"));
        };
        const qint64 id = r.copyText(hebrew("שלום עולם"));
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        QGuiApplication::clipboard()->clear();
        QVERIFY(r.choose(id, QStringLiteral("Hebrew → English")));
        QTRY_VERIFY(r.popup()->isBusy());
        r.daemon->panic();
        QVERIFY(!r.popup()->isBusy());
        QCOMPARE(r.repo().count(), qint64(0));
        *hold = false;
        QTRY_COMPARE_WITH_TIMEOUT(int(*finished), 1, 5000);
        QTest::qWait(300); // the answer is delivered on the GUI thread, and ignored
        QCOMPARE(pasted.size(), 0);
        QVERIFY(r.warnings.isEmpty());
        QVERIFY(r.quiet.isEmpty());
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
        QCOMPARE(r.repo().count(), qint64(0));
        QVERIFY(!r.popup()->isBusy());
    }

    void aPanicWhileAPicturesTextIsBeingReadDropsTheAnswer()
    {
        Rig r;
        const qint64 pic = r.copyPicture(11);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000); // the read for the search
        r.ocr->answer = hebrew("מכשול בדרך");
        r.ocr->hold = true;
        QVERIFY(r.show());
        QSignalSpy pasted(r.popup(), &PopupWindow::textActivated);
        QGuiApplication::clipboard()->clear();
        QVERIFY(r.choose(pic, QStringLiteral("Hebrew → English"), QStringLiteral("Translate text from picture")));
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 2, 5000);
        QVERIFY(r.popup()->isBusy());
        r.daemon->panic();
        QVERIFY(!r.popup()->isBusy());
        r.ocr->hold = false;
        QTest::qWait(500);
        QCOMPARE(pasted.size(), 0);
        QCOMPARE(r.engine->calls(), 0); // never went on to translate
        QVERIFY(r.warnings.isEmpty());
        QVERIFY(r.quiet.isEmpty());
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
        QCOMPARE(r.repo().count(), qint64(0));
    }

    void aPanicWhileAPictureIsBeingReadForTheSearchLeavesNothingBehind()
    {
        Rig r;
        r.ocr->hold = true;
        r.copyPicture(12);
        QTRY_COMPARE_WITH_TIMEOUT(int(r.ocr->calls), 1, 5000);
        r.daemon->panic();
        QCOMPARE(r.repo().count(), qint64(0));
        r.ocr->hold = false;
        QTest::qWait(500);
        QCOMPARE(r.repo().count(), qint64(0));
        QVERIFY(r.repo().search(QStringLiteral("invoice")).empty());
        QCOMPARE(int(r.ocr->calls), 1); // and no other picture is read afterwards
    }

private:
    static QString repo_content(Rig& r, qint64 id) { return r.repo().get(id)->content; }
};

QTEST_MAIN(TestPictureTranslation)
#include "tst_picturetranslation.moc"
