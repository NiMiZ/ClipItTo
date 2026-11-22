// Regex search ("re:<pattern>"): parsing, matching, filters, passwords, limits, the hint, cancel, and what is logged.
#include "Config.h"
#include "FakeKeyStore.h"
#include "HistoryModel.h"
#include "PasswordKeeper.h"
#include "PerfData.h"
#include "PopupWindow.h"
#include "RegexSearch.h"
#include "SearchBar.h"
#include "SearchController.h"
#include "StallWatchdog.h"
#include "TestDb.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QTest>

using clipitto::Config;
using clipitto::RegexQuery;
using clipitto::ui::HistoryModel;
using clipitto::ui::PopupWindow;
using clipitto::ui::SearchController;

namespace {

QStringList g_log; // every message the Qt logging system saw while a test ran
void collect(QtMsgType, const QMessageLogContext&, const QString& message)
{
    g_log << message;
}

QStringList found(SearchController& c, HistoryModel& m, const QString& query)
{
    c.setQuery(query);
    c.refresh(); // synchronous
    QStringList titles;
    for (int i = 0; i < m.rowCount(); ++i) {
        titles << m.index(i, 0).data(HistoryModel::TitleRole).toString();
    }
    return titles;
}

} // namespace

class TestRegexSearch : public QObject {
    Q_OBJECT

private slots:
    // ---------------- parsing ----------------

    void theTokenIsFoundAmongFilters()
    {
        RegexQuery q = RegexQuery::parse(QStringLiteral("group:work re:foo\\d+ budget"));
        QVERIFY(q.present);
        QVERIFY2(q.error.isEmpty(), qPrintable(q.error));
        QCOMPARE(q.pattern, QStringLiteral("foo\\d+"));
        QCOMPARE(q.rest, QStringLiteral("group:work budget"));
        q = RegexQuery::parse(QStringLiteral("RE:^a$"));
        QVERIFY(q.present && q.error.isEmpty());
        QCOMPARE(q.rest, QString());
    }

    void aQuotedPatternMayHaveSpaces()
    {
        const RegexQuery q = RegexQuery::parse(QStringLiteral("star: re:\"error \\d+ at\" since:1d"));
        QVERIFY(q.present && q.error.isEmpty());
        QCOMPARE(q.pattern, QStringLiteral("error \\d+ at"));
        QCOMPARE(q.rest, QStringLiteral("star: since:1d"));
    }

    void aKeyAloneAndOtherWordsAreOrdinaryText()
    {
        QVERIFY(!RegexQuery::parse(QStringLiteral("Re: meeting notes")).present);
        QVERIFY(!RegexQuery::parse(QStringLiteral("are:this")).present);
        QVERIFY(!RegexQuery::parse(QStringLiteral("hello world")).present);
    }

    void badPatternsGetAShortReason()
    {
        const RegexQuery q = RegexQuery::parse(QStringLiteral("re:("));
        QVERIFY(q.present);
        QVERIFY(!q.error.isEmpty());
        QVERIFY2(q.error.size() < 60, qPrintable(q.error));
        QVERIFY(RegexQuery::parse(QStringLiteral("re:[a-")).error.size() > 0);
        QVERIFY(RegexQuery::parse(QStringLiteral("re:a re:b")).error.size() > 0);
        QVERIFY(RegexQuery::parse(QStringLiteral("re:\"\"")).error.size() > 0);
        // A leading verb could raise the work limit: refused.
        QVERIFY(RegexQuery::parse(QStringLiteral("re:(*LIMIT_MATCH=999999999)a")).error.size() > 0);
        QVERIFY(RegexQuery::parse(QStringLiteral("re:(*NO_JIT)a")).error.size() > 0);
    }

    void aTooLongPatternIsRefusedAtTheLimit()
    {
        QVERIFY(RegexQuery::parse(QStringLiteral("re:") + QString(RegexQuery::kMaxPatternChars, QLatin1Char('a'))).error.isEmpty());
        const RegexQuery q = RegexQuery::parse(QStringLiteral("re:") + QString(RegexQuery::kMaxPatternChars + 1, QLatin1Char('a')));
        QVERIFY(q.present);
        QVERIFY(q.error.contains(QStringLiteral("too long")));
    }

    // ---------------- matching ----------------

    void validPatternsAndAnchorsNewestFirst()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("https://a.test/old.pdf"));
        t.repo->ingest(QStringLiteral("see https://a.test/x.pdf here"));
        t.repo->ingest(QStringLiteral("http://b.test/new.pdf"));
        t.repo->ingest(QStringLiteral("https://c.test/page.html"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QCOMPARE(found(c, m, QStringLiteral("re:^https?://.*\\.pdf$")),
                 (QStringList{QStringLiteral("http://b.test/new.pdf"), QStringLiteral("https://a.test/old.pdf")}));
        QCOMPARE(c.regexState(), SearchController::RegexState::Ok);
        QVERIFY(c.regexNote().isEmpty());
        QCOMPARE(found(c, m, QStringLiteral("re:\\.pdf")).size(), 3);
        QCOMPARE(found(c, m, QStringLiteral("re:^$")).size(), 0);
    }

    void caseInsensitiveByDefaultAndSwitchedOffWithInlineFlag()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("Invoice 42"));
        t.repo->ingest(QStringLiteral("invoice 43"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QCOMPARE(found(c, m, QStringLiteral("re:INVOICE\\s\\d+")).size(), 2);
        QCOMPARE(found(c, m, QStringLiteral("re:(?-i)Invoice")), (QStringList{QStringLiteral("Invoice 42")}));
        QCOMPARE(found(c, m, QStringLiteral("re:(?-i)INVOICE")).size(), 0);
    }

    void combinedWithFiltersAndWords()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("foo12 alpha")).id;
        const auto b = t.repo->ingest(QStringLiteral("foo7 beta")).id;
        t.repo->ingest(QStringLiteral("foo99 gamma")); // matches, but not in the group
        t.repo->setGroup(a, QStringLiteral("work"));
        t.repo->setGroup(b, QStringLiteral("work"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QCOMPARE(found(c, m, QStringLiteral("group:work re:foo\\d+")).size(), 2);
        QCOMPARE(found(c, m, QStringLiteral("group:work re:foo\\d\\d beta")).size(), 0);
        QCOMPARE(found(c, m, QStringLiteral("group:work re:foo\\d+ beta")), (QStringList{QStringLiteral("foo7 beta")}));
        QCOMPARE(found(c, m, QStringLiteral("re:^foo\\d+ gamma$")).size(), 0); // a space ends the token
        QCOMPARE(found(c, m, QStringLiteral("re:\"^foo\\d+ gamma$\"")).size(), 1);
        QCOMPARE(found(c, m, QStringLiteral("re:foo\\d+ star:")).size(), 0);
        t.repo->setFavorite(b, true);
        QCOMPARE(found(c, m, QStringLiteral("re:foo\\d+ star:")), (QStringList{QStringLiteral("foo7 beta")}));
    }

    void noteAndGroupAreTriedToo()
    {
        TestDb t;
        const auto a = t.repo->ingest(QStringLiteral("plain text")).id;
        t.repo->setNote(a, QStringLiteral("ticket ABC-123"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QCOMPARE(found(c, m, QStringLiteral("re:[a-z]{3}-\\d+")).size(), 1);
    }

    void onlyTheFirstTwentyThousandCharactersAreTried()
    {
        TestDb t;
        t.repo->ingest(QString(RegexQuery::kMaxScanChars - 3, QLatin1Char('x')) + QStringLiteral("needle"));
        t.repo->ingest(QString(RegexQuery::kMaxScanChars + 10, QLatin1Char('x')) + QStringLiteral("needle"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QCOMPARE(found(c, m, QStringLiteral("re:needle")).size(), 0); // only "nee" is inside the limit
        QCOMPARE(found(c, m, QStringLiteral("re:nee")).size(), 1);
        QCOMPARE(found(c, m, QStringLiteral("re:^x+")).size(), 2);
    }

    // ---------------- invalid and over-long patterns ----------------

    void invalidPatternShowsAHintNotAnError()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("something"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QVERIFY(found(c, m, QStringLiteral("re:(")).isEmpty());
        QCOMPARE(c.regexState(), SearchController::RegexState::Invalid);
        QVERIFY2(c.regexNote().startsWith(QStringLiteral("not a valid pattern: ")), qPrintable(c.regexNote()));
        QVERIFY(!c.isRegexRunning());
        // Typing it out: every prefix is calm, and the complete pattern works.
        for (const QString& typed : {QStringLiteral("re:"), QStringLiteral("re:s"), QStringLiteral("re:(s"), QStringLiteral("re:(so)")}) {
            found(c, m, typed);
        }
        QCOMPARE(c.regexState(), SearchController::RegexState::Ok);
        QCOMPARE(found(c, m, QStringLiteral("re:(some)thing")).size(), 1);
        QVERIFY(c.regexNote().isEmpty());
        found(c, m, QStringLiteral("re:") + QString(300, QLatin1Char('a')));
        QCOMPARE(c.regexState(), SearchController::RegexState::Invalid);
        QVERIFY(c.regexNote().contains(QStringLiteral("too long")));
    }

    // ---------------- catastrophic patterns ----------------

    void aCatastrophicPatternEndsWithinTheBudgetAndKeepsWhatWasFound()
    {
        TestDb t;
        const QString heavy = QStringLiteral("see ") + QString(RegexQuery::kMaxScanChars - 20, QLatin1Char('a')) + QLatin1Char('b');
        for (int i = 0; i < 40; ++i) {
            t.repo->ingest(heavy + QString::number(i));
        }
        t.repo->ingest(QStringLiteral("aaa")); // newest: found before the slow ones
        HistoryModel m;
        SearchController c(*t.repo, m);
        QElapsedTimer timer;
        timer.start();
        const QStringList result = found(c, m, QStringLiteral("re:(a+)+$"));
        const qint64 took = timer.elapsed();
        QVERIFY2(took <= StallWatchdog::scaled(300 + 400), qPrintable(QStringLiteral("took %1 ms").arg(took)));
        QCOMPARE(c.regexState(), SearchController::RegexState::Stopped);
        QCOMPARE(c.regexNote(), QStringLiteral("stopped after 0.3 s: simplify the pattern"));
        QVERIFY(result.contains(QStringLiteral("aaa")));
    }

    void aLongListStopsAtTheBudgetWithAnotherNote()
    {
        TestDb t;
        for (int i = 0; i < 300; ++i) {
            t.repo->ingest(QStringLiteral("copy %1").arg(i));
        }
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.setRegexBudgetMs(0); // the budget ends the scan, not the pattern
        found(c, m, QStringLiteral("re:nomatch"));
        QCOMPARE(c.regexState(), SearchController::RegexState::Stopped);
        QVERIFY2(c.regexNote().startsWith(QStringLiteral("stopped after 0.0 s: only the newest ")), qPrintable(c.regexNote()));
    }

    void theNextKeystrokeCancelsTheScan()
    {
        TestDb t;
        const QString heavy = QStringLiteral("see ") + QString(RegexQuery::kMaxScanChars - 20, QLatin1Char('a')) + QLatin1Char('b');
        for (int i = 0; i < 60; ++i) {
            t.repo->ingest(heavy + QString::number(i));
        }
        t.repo->ingest(QStringLiteral("plain words"));
        Config cfg;
        cfg.searchDebounceMs = 0;
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.applyConfig(cfg);
        c.setRegexBudgetMs(5000); // would run for seconds if it were not cancelled
        c.setQuery(QStringLiteral("re:(a+)+$"));
        QTRY_VERIFY(c.isRegexRunning());
        QElapsedTimer timer;
        timer.start();
        c.setQuery(QStringLiteral("plain"));
        QVERIFY(!c.isRegexRunning()); // gone at once, before the new query even runs
        QTRY_COMPARE(m.rowCount(), 1);
        QCOMPARE(c.regexState(), SearchController::RegexState::None);
        QVERIFY(c.regexNote().isEmpty());
        QVERIFY2(timer.elapsed() < 1000, "the cancelled scan kept going");
    }

    // ---------------- passwords and pictures ----------------

    void passwordTextIsNeverMatchedOnlyNoteAndGroup()
    {
        TestDb t;
        auto keys = std::make_shared<FakeKeyStore>();
        clipitto::PasswordKeeper keeper(*t.repo, keys);
        keeper.setKdfCost({64, 1});
        std::optional<bool> ok;
        keeper.setMasterPassword("master", [&ok](bool r) { ok = r; });
        QTRY_VERIFY(ok.has_value());
        const auto& v = keeper.vault();
        t.repo->ingestSecret(v.seal("hunter2-Correct-Horse"), v.match("hunter2-Correct-Horse"), QStringLiteral("bank login"), QString());
        t.repo->ingest(QStringLiteral("ordinary hunter2 text"));
        ok.reset();
        keeper.unlock("master", [&ok](bool r) { ok = r; }); // unlocked: the text could be opened, but must not be
        QTRY_VERIFY(ok.has_value());
        HistoryModel m;
        SearchController c(*t.repo, m);
        c.setPasswords(&keeper);
        QCOMPARE(found(c, m, QStringLiteral("re:hunter2")), (QStringList{QStringLiteral("ordinary hunter2 text")}));
        QCOMPARE(found(c, m, QStringLiteral("re:horse")).size(), 0);
        QCOMPARE(found(c, m, QStringLiteral("re:^password$")).size(), 0); // not even the label
        QCOMPARE(found(c, m, QStringLiteral("re:^bank\\s")).size(), 1); // the note is searchable, like ordinary search
        QVERIFY(m.entryAt(m.index(0, 0))->secret);
        // The p[] flow is as before: a part of the password is found only there.
        c.setPasswordQuery(QStringLiteral("correct"), true);
        c.refresh();
        QCOMPARE(m.rowCount(), 1);
        c.setPasswordQuery(QString(), false);
    }

    void picturesMatchTheirLabelAndNoteOnly()
    {
        TestDb t;
        const auto pic = t.repo->ingestImage(perf::fakePicture(1, 800, 600)).id;
        t.repo->ingest(QStringLiteral("text 800x600"));
        HistoryModel m;
        SearchController c(*t.repo, m);
        QCOMPARE(found(c, m, QStringLiteral("re:^image\\s+800")).size(), 1);
        QVERIFY(m.entryAt(m.index(0, 0))->image);
        QCOMPARE(found(c, m, QStringLiteral("img: re:600")).size(), 1);
        t.repo->setNote(pic, QStringLiteral("whiteboard photo"));
        QCOMPARE(found(c, m, QStringLiteral("re:white.*photo")).size(), 1);
    }

    // ---------------- logging ----------------

    void nothingButTheOutcomeIsLogged()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("zqSecretCopyText 12345"));
        t.repo->ingest(QStringLiteral("see ") + QString(RegexQuery::kMaxScanChars - 20, QLatin1Char('a')) + QLatin1Char('b'));
        HistoryModel m;
        SearchController c(*t.repo, m);
        g_log.clear();
        const QtMessageHandler previous = qInstallMessageHandler(collect);
        found(c, m, QStringLiteral("re:zqSecretCopy\\w+\\s\\d+"));
        found(c, m, QStringLiteral("re:(zqPatternMarker"));
        found(c, m, QStringLiteral("re:(zqHeavy|a+)+$"));
        qInstallMessageHandler(previous);
        QVERIFY(!g_log.isEmpty());
        const QString all = g_log.join(QLatin1Char('\n'));
        QVERIFY2(all.contains(QStringLiteral("regex search: ok")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("regex search: invalid")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("regex search: timed out")), qPrintable(all));
        for (const QString& secret : {QStringLiteral("zqSecretCopy"), QStringLiteral("12345"), QStringLiteral("zqPatternMarker"),
                                      QStringLiteral("zqHeavy"), QStringLiteral("\\d")}) {
            QVERIFY2(!all.contains(secret), qPrintable(secret));
        }
    }

    // ---------------- the popup ----------------

    void thePopupShowsTheHintWhileTypingAnIncompletePattern()
    {
        TestDb t;
        t.repo->ingest(QStringLiteral("foo123"));
        Config cfg;
        cfg.hideOnFocusLoss = false;
        PopupWindow popup(*t.repo, cfg);
        popup.showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(&popup));
        QVERIFY(popup.searchNote()->isHidden());
        QTest::keyClicks(popup.searchBar(), QStringLiteral("re:(foo"));
        QTRY_VERIFY(!popup.searchNote()->isHidden());
        QVERIFY2(popup.searchNote()->text().startsWith(QStringLiteral("not a valid pattern: ")), qPrintable(popup.searchNote()->text()));
        popup.searchBar()->setText(QStringLiteral("re:foo\\d+"));
        QTRY_VERIFY(popup.searchNote()->isHidden());
        QTRY_COMPARE(popup.controller()->regexState(), SearchController::RegexState::Ok);
    }
};

QTEST_MAIN(TestRegexSearch)
#include "tst_regexsearch.moc"
