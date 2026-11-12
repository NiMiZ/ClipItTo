// Stalling in the popup and in search: opening with a big history, reopening, scrolling, mixed rows, thumbnails, the
// hover preview, and search over 50k copies. Everything the popup does on the GUI thread is measured with the
// event-loop watchdog (StallWatchdog.h).
#include "CapturePipeline.h"
#include "Config.h"
#include "FakeKeyStore.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PasswordKeeper.h"
#include "PerfData.h"
#include "PopupWindow.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "StallWatchdog.h"
#include "TestDb.h"

#include <QElapsedTimer>
#include <QHelpEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::Config;
using clipitto::ui::HistoryModel;
using clipitto::ui::PopupWindow;
using clipitto::ui::SearchController;

class TestPerfPopup : public QObject {
    Q_OBJECT

    static Config quietConfig()
    {
        Config cfg;
        cfg.hideOnFocusLoss = false;
        return cfg;
    }

    // The 50k history is built once for the read-only search tests (about 5 s).
    TestDb& big()
    {
        if (!m_big) {
            m_big = std::make_unique<TestDb>();
            QElapsedTimer t;
            t.start();
            perf::fill(m_big->db, *m_big->repo, 0, 50000);
            perf::note("fill 50k copies (ms)", QString::number(t.elapsed()));
        }
        return *m_big;
    }

    std::unique_ptr<TestDb> m_big;
    std::unique_ptr<TestDb> m_grow;
    int m_grown = 0;

private slots:
    // ---------------- Popup ----------------

    void openWithManyCopies_data()
    {
        QTest::addColumn<int>("count");
        QTest::newRow("1k") << 1000;
        QTest::newRow("10k") << 10000;
        QTest::newRow("50k") << 50000;
    }

    void openWithManyCopies()
    {
        QFETCH(int, count);
        if (!m_grow) {
            m_grow = std::make_unique<TestDb>();
        }
        perf::fill(m_grow->db, *m_grow->repo, m_grown, count);
        m_grown = count;
        const Config cfg = quietConfig();
        PopupWindow popup(*m_grow->repo, cfg);
        StallWatchdog wd;
        popup.showPopup();
        QTest::qWait(150);
        QVERIFY(popup.isVisible());
        QVERIFY(popup.model()->rowCount() > 0);
        QVERIFY_NO_STALL(wd, qPrintable(QStringLiteral("popup open, %1 copies").arg(count)));
    }

    // Groups, stars and pins load all their copies when the popup refreshes (the group tree): 3k grouped copies.
    void openWithManyGroupedCopies()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 3000);
        {
            clipitto::db::Transaction tx(t.db);
            const auto recent = t.repo->recent(3000, false);
            int i = 0;
            for (const auto& e : recent) {
                t.repo->setGroup(e.id, QStringLiteral("G%1").arg(i++ % 20));
            }
            tx.commit();
        }
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        StallWatchdog wd(250); // linear in the grouped copies: the group tree loads them all
        popup.showPopup();
        QTest::qWait(150);
        popup.refresh();
        QTest::qWait(50);
        QVERIFY_NO_STALL(wd, "popup open, 3k grouped copies");
    }

    void reopenFiftyTimes()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 5000);
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QTest::qWait(100);
        popup.hidePopup();
        const qint64 rssBefore = perf::residentKb();
        QVector<qint64> times;
        StallWatchdog wd;
        for (int i = 0; i < 50; ++i) {
            QElapsedTimer one;
            one.start();
            popup.showPopup();
            QTest::qWait(10);
            times << one.elapsed();
            popup.hidePopup();
            QTest::qWait(5);
        }
        const qint64 rssGrowth = perf::residentKb() - rssBefore;
        qint64 first = 0;
        qint64 last = 0;
        for (int i = 0; i < 10; ++i) {
            first += times[i];
            last += times[40 + i];
        }
        perf::note("reopen: first 10 / last 10 (ms)", QStringLiteral("%1 / %2").arg(first).arg(last));
        perf::note("reopen: RSS growth (KiB)", QString::number(rssGrowth));
        QVERIFY2(last <= first * 3 + 200, "reopening slows down");
        if (rssBefore > 0 && !perf::underSanitizer()) { // a sanitizer keeps freed memory back, so RSS says nothing
            QVERIFY2(rssGrowth < 30 * 1024, "reopening 50 times grows memory");
        }
        QVERIFY_NO_STALL(wd, "popup reopen x50");
    }

    // The list never holds more than the search limit (100 rows) plus headers, so "50k rows" cannot be scrolled: the
    // longest list is a 100-row search result, scrolled here by keys and by the scroll bar.
    void fastScrollThroughAFullList()
    {
        TestDb& t = big();
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        QTest::keyClicks(popup.searchBar(), QStringLiteral("docker"));
        QTRY_VERIFY_WITH_TIMEOUT(popup.model()->rowCount() >= 100, 3000);
        StallWatchdog wd;
        for (int i = 0; i < 120; ++i) {
            QTest::keyClick(popup.view(), Qt::Key_Down);
        }
        for (int i = 0; i < 10; ++i) {
            QTest::keyClick(popup.view(), Qt::Key_PageDown);
        }
        auto* bar = popup.view()->verticalScrollBar();
        for (int v = bar->minimum(); v <= bar->maximum(); v += qMax(1, bar->pageStep() / 2)) {
            bar->setValue(v);
            QCoreApplication::processEvents();
        }
        QVERIFY_NO_STALL(wd, "popup scroll through 100 rows");
    }

    void mixedRows()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 200);
        for (int i = 0; i < 10; ++i) {
            t.repo->ingestImage(perf::fakePicture(i, 3840, 2160));
        }
        for (int i = 0; i < 5; ++i) {
            t.repo->ingest(QString(200000, QLatin1Char('a' + i)) + QStringLiteral(" long %1").arg(i)); // 200 KB, no breaks
        }
        for (int i = 0; i < 5; ++i) {
            QStringList files;
            for (int f = 0; f < 3000; ++f) {
                files << QStringLiteral("/home/user/projects/some/deep/folder/file_%1_%2.txt").arg(i).arg(f);
            }
            t.repo->ingest(files.join(QLatin1Char('\n')));
        }
        Config cfg = quietConfig();
        cfg.recentCount = 40;
        PopupWindow popup(*t.repo, cfg);
        StallWatchdog wd;
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        QTest::qWait(100);
        const QPixmap shot = popup.grab(); // paints every visible row
        QVERIFY(!shot.isNull());
        for (int i = 0; i < 30; ++i) {
            QTest::keyClick(popup.view(), Qt::Key_Down);
        }
        QVERIFY_NO_STALL(wd, "popup mixed rows (pictures, 200 KB, 3000 lines)");
    }

    // Thumbnails are decoded when a row is first painted (HistoryModel::ThumbnailRole, then cached): small PNGs, on the
    // GUI thread. With 40 pictures in the list the first paint must stay quick.
    void thumbnailsFirstPaint()
    {
        TestDb t;
        for (int i = 0; i < 40; ++i) {
            t.repo->ingestImage(perf::fakePicture(i));
        }
        Config cfg = quietConfig();
        cfg.recentCount = 40;
        PopupWindow popup(*t.repo, cfg);
        StallWatchdog wd;
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        popup.grab();
        for (int i = 0; i < 40; ++i) {
            QTest::keyClick(popup.view(), Qt::Key_Down);
        }
        popup.grab();
        QVERIFY_NO_STALL(wd, "popup 40 thumbnails first paint");
    }

    void hoverPreviewOfABigPicture()
    {
        TestDb t;
        const auto id = t.repo->ingestImage(perf::fakePicture(1, 7680, 4320)).id;
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        const QModelIndex image = popup.model()->findEntry(id);
        QVERIFY(image.isValid());
        auto* view = popup.view();
        StallWatchdog wd;
        for (int i = 0; i < 20; ++i) {
            QHelpEvent hover(QEvent::ToolTip, view->visualRect(image).center(),
                             view->viewport()->mapToGlobal(view->visualRect(image).center()));
            QCoreApplication::sendEvent(view->viewport(), &hover);
            QTest::qWait(5);
        }
        QVERIFY(view->imagePreview() && view->imagePreview()->isVisible());
        QVERIFY_NO_STALL(wd, "popup hover preview of an 8K picture");
    }

    void hideAndShowDuringASearch()
    {
        TestDb& t = big();
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        StallWatchdog wd;
        for (int round = 0; round < 10; ++round) {
            QTest::keyClicks(popup.searchBar(), QStringLiteral("dep"));
            popup.hidePopup();
            QTest::qWait(30);
            popup.showPopup();
            QTest::qWait(30);
        }
        QTest::qWait(300);
        QVERIFY(popup.isVisible());
        QVERIFY_NO_STALL(wd, "popup hide/show during search x10");
    }

    // ---------------- Search ----------------

    void typingInFiftyThousandCopies()
    {
        TestDb& t = big();
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        StallWatchdog wd;
        const QString typed = QStringLiteral("docker compose n4999");
        for (const QChar c : typed) {
            QTest::keyClick(popup.searchBar(), c.toLatin1());
            QTest::qWait(40);
        }
        QTest::qWait(400);
        QVERIFY_NO_STALL(wd, "search: typing in 50k copies");
    }

    void theFilters_data()
    {
        QTest::addColumn<QString>("query");
        QTest::newRow("img") << QStringLiteral("img:");
        QTest::newRow("img word") << QStringLiteral("img: 1920");
        QTest::newRow("group") << QStringLiteral("group:work");
        QTest::newRow("any group") << QStringLiteral("group:");
        QTest::newRow("since") << QStringLiteral("since:3d");
        QTest::newRow("since+word") << QStringLiteral("since:2h docker");
        QTest::newRow("short word (LIKE)") << QStringLiteral("gi");
        QTest::newRow("two letters+number") << QStringLiteral("n4");
        QTest::newRow("text:") << QStringLiteral("text: kubectl");
    }

    void theFilters()
    {
        QFETCH(QString, query);
        TestDb& t = big();
        HistoryModel model;
        SearchController controller(*t.repo, model);
        StallWatchdog wd;
        controller.setQuery(query);
        controller.refresh();
        QTest::qWait(30);
        QVERIFY_NO_STALL(wd, qPrintable(QStringLiteral("search: %1 over 50k").arg(query)));
    }

    // p[] with 5k password copies, unlocked: every password is opened to match.
    void passwordSearchWithFiveThousandUnlocked()
    {
        TestDb t;
        auto keys = std::make_shared<FakeKeyStore>();
        clipitto::PasswordKeeper keeper(*t.repo, keys);
        keeper.setKdfCost({64, 1});
        std::optional<bool> ok;
        keeper.setMasterPassword("master", [&ok](bool r) { ok = r; });
        QTRY_VERIFY(ok.has_value());
        const auto& v = keeper.vault();
        QElapsedTimer seal;
        seal.start();
        // One sealed blob reused (sealing costs ~1.4 ms each); opening each is what a p[] search pays for.
        const QByteArray sealed = v.seal("pw-x-xyz");
        for (int i = 0; i < 5000; ++i) { // ingestSecret opens its own transaction
            t.repo->ingestSecret(sealed, v.match(QByteArray("pw-") + QByteArray::number(i)), QStringLiteral("App%1").arg(i % 50), QString());
        }
        ok.reset();
        keeper.unlock("master", [&ok](bool r) { ok = r; });
        QTRY_VERIFY(ok.has_value());
        QVERIFY(keeper.isUnlocked());
        perf::note("seal 5000 passwords (ms)", QString::number(seal.elapsed()));
        HistoryModel model;
        SearchController controller(*t.repo, model);
        controller.setPasswords(&keeper);
        // One selective query: every sealed password is opened (X25519) to look for it, on the GUI thread.
        StallWatchdog wd;
        controller.setPasswordQuery(QStringLiteral("zzz-not-there"), true);
        controller.refresh();
        QTest::qWait(30);
        const qint64 max = wd.finish();
        const bool within = wd.report("search: p[] with 5k unlocked passwords");
        QVERIFY2(max < StallWatchdog::scaled(3000), "a p[] search over 5k passwords takes seconds");
        // KNOWN STALL (not fixed, needs the search off the GUI thread or an unlocked in-memory index of the
        // passwords, which must not exist): about 80 us per password, so ~400 ms (debug) per keystroke at 5k.
        if (!within) { // a known stall: expected to fail until the search leaves the GUI thread; passing is fine too
            QEXPECT_FAIL("", "p[] opens every sealed password on the GUI thread (linear in the number of passwords)", Continue);
        }
        QVERIFY(within);
    }

    // A burst of copies arrives while the user types: neither blocks the other.
    void searchDuringACopyBurst()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 20000);
        clipitto::CapturePipeline pipeline(*t.repo, 100000, 1 << 20);
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        connect(&pipeline, &clipitto::CapturePipeline::stored, &popup, [&popup](qint64, bool) { popup.historyChanged(); });
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        int sent = 0;
        QTimer burst;
        burst.setInterval(2);
        connect(&burst, &QTimer::timeout, &burst, [&] {
            pipeline.process(perf::sampleText(100000 + sent), {QStringLiteral("text/plain")});
            if (++sent >= 200) {
                burst.stop();
            }
        });
        StallWatchdog wd;
        burst.start();
        for (const QChar c : QStringLiteral("docker commit")) {
            QTest::keyClick(popup.searchBar(), c.toLatin1());
            QTest::qWait(25);
        }
        QTRY_VERIFY_WITH_TIMEOUT(sent >= 200, 10000);
        QTest::qWait(300);
        QVERIFY_NO_STALL(wd, "search: typing during a 200-copy burst");
    }

    // Typing fast: queries are debounced, so far fewer run than keys were pressed.
    void staleSearchesAreDropped()
    {
        TestDb& t = big();
        const Config cfg = quietConfig();
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        const int before = popup.controller()->queriesRun();
        // A query that every row matches (the test vocabulary is 18 words): FTS ranks all 50k on the GUI thread.
        StallWatchdog wd(250);
        QTest::keyClicks(popup.searchBar(), QStringLiteral("kubectl deploy server"));
        QTest::qWait(500);
        const int ran = popup.controller()->queriesRun() - before;
        perf::note("queries run for 21 keys typed at once", QString::number(ran));
        QVERIFY2(ran <= 3, "every key started a query");
        QVERIFY_NO_STALL(wd, "search: stale searches dropped");
    }

    // The search index (FTS5) next to the text: a 20k import must not balloon the file.
    void indexSizeAfterALargeImport()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("big.db"));
        qint64 textBytes = 0;
        {
            clipitto::db::Db db(path);
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            clipitto::db::Repository repo(db);
            for (int i = 0; i < 20000; ++i) {
                textBytes += perf::sampleText(i).toUtf8().size();
            }
            perf::fill(db, repo, 0, 20000);
            repo.optimize();
            db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        }
        const qint64 size = QFileInfo(path).size();
        perf::note("text bytes / db file bytes", QStringLiteral("%1 / %2").arg(textBytes).arg(size));
        // By design about 7x: the trigram index that makes substring search work.
        QVERIFY2(size < textBytes * 10, "the database is more than ten times the text it holds");
    }
};

QTEST_MAIN(TestPerfPopup)
#include "tst_perf_popup.moc"
