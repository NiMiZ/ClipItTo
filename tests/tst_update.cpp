// "Check for updates": the parsing, the rules about what may be opened, the texts, the once-a-day automatic check and the
// remembered "Later". The network is a fake (canned answers); no test here opens a socket.

#include "Config.h"
#include "Daemon.h"
#include "PackagedApp.h"
#include "SelfTestChecks.h"
#include "UpdateCheck.h"
#include "UpdateController.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace clipitto;
using namespace clipitto::update;

namespace {

class FakeFetcher final : public Fetcher {
public:
    FetchResult answer;
    QList<QUrl> urls;
    QList<QByteArray> agents;
    void get(const QUrl& url, const QByteArray& userAgent, std::function<void(FetchResult)> done) override
    {
        urls.append(url);
        agents.append(userAgent);
        QTimer::singleShot(0, [done = std::move(done), a = answer] { done(a); });
    }
};

FetchResult ok(const QByteArray& json)
{
    FetchResult r;
    r.status = 200;
    r.body = json;
    return r;
}

QByteArray newer(const QString& url = QStringLiteral("https://cohenzco.com/portfolio/nimiz/clipitto"))
{
    return QByteArray(R"({"app":"clipitto","latest":"0.19.0","current":"0.18.0","updateAvailable":true,)"
                      R"("releasedAt":null,"download":null,"url":")") +
           url.toUtf8() + "\"}";
}

const QByteArray kCurrent = R"({"app":"clipitto","latest":"0.18.0","current":"0.18.0","updateAvailable":false,"url":"https://cohenzco.com/x"})";

struct Shown {
    QStringList asked;
    QStringList informed;
    bool download = false;
};

} // namespace

class TstUpdate : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString statePath() const { return m_dir.filePath(QStringLiteral("update-state.json")); }

    struct Rig {
        std::shared_ptr<FakeFetcher> fetcher = std::make_shared<FakeFetcher>();
        std::unique_ptr<ui::UpdateController> controller;
        Shown shown;
        QList<QUrl> opened;
        QDateTime now = QDateTime(QDate(2026, 11, 19), QTime(10, 0), Qt::UTC);
    };
    std::unique_ptr<Rig> rig()
    {
        QFile::remove(statePath());
        auto r = std::make_unique<Rig>();
        r->controller = std::make_unique<ui::UpdateController>(r->fetcher, QStringLiteral("0.18.0"), statePath());
        Rig* raw = r.get();
        ui::UpdatePresenter p;
        p.askDownload = [raw](QWidget*, const QString& text) {
            raw->shown.asked << text;
            return raw->shown.download;
        };
        p.inform = [raw](QWidget*, const QString& text) { raw->shown.informed << text; };
        r->controller->setPresenter(p);
        r->controller->setUrlOpener([raw](const QUrl& u) { raw->opened << u; });
        r->controller->setClock([raw] { return raw->now; });
        return r;
    }
    static void settle() { QTest::qWait(20); }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void theRequestCarriesOnlyVersionAndPlatform()
    {
        QCOMPARE(requestUrl(QStringLiteral("0.18.0"), QStringLiteral("linux")).toString(),
                 QStringLiteral("https://cohenzco.com/api/apps/clipitto/latest?version=0.18.0&platform=linux"));
        QVERIFY(requestUrl(QStringLiteral("1.0.0-beta+3"), QStringLiteral("macos")).toString().contains(QLatin1String("version=1.0.0-beta%2B3")));
        const QString p = platformName();
        QVERIFY(p == QLatin1String("windows") || p == QLatin1String("linux") || p == QLatin1String("macos"));
        auto r = rig();
        r->fetcher->answer = ok(kCurrent);
        r->controller->checkNow();
        settle();
        QCOMPARE(r->fetcher->urls.size(), 1);
        QCOMPARE(r->fetcher->agents.first(), QByteArray("ClipItTo/0.18.0"));
        QCOMPARE(r->fetcher->urls.first().scheme(), QStringLiteral("https"));
        QCOMPARE(r->fetcher->urls.first().host(), QStringLiteral("cohenzco.com"));
        QCOMPARE(r->fetcher->urls.first().query(), QStringLiteral("version=0.18.0&platform=%1").arg(platformName()));
    }

    void parsingTheAnswers()
    {
        Result r = parseResponse(ok(newer()), QStringLiteral("0.18.0"));
        QCOMPARE(r.outcome, Outcome::Available);
        QCOMPARE(r.latest, QStringLiteral("0.19.0"));
        QCOMPARE(r.current, QStringLiteral("0.18.0"));
        QCOMPARE(r.url, QUrl(QStringLiteral("https://cohenzco.com/portfolio/nimiz/clipitto")));
        r = parseResponse(ok(kCurrent), QStringLiteral("0.18.0"));
        QCOMPARE(r.outcome, Outcome::UpToDate);
        QCOMPARE(r.current, QStringLiteral("0.18.0"));
        QVERIFY(r.url.isEmpty());
    }

    void failuresAreFailures()
    {
        const QString v = QStringLiteral("0.18.0");
        QCOMPARE(parseResponse(ok("not json"), v).outcome, Outcome::Failed);
        QCOMPARE(parseResponse(ok("[1,2]"), v).outcome, Outcome::Failed);
        QCOMPARE(parseResponse(ok("{}"), v).outcome, Outcome::Failed);
        QCOMPARE(parseResponse(ok(R"({"updateAvailable":"yes"})"), v).outcome, Outcome::Failed);
        QCOMPARE(parseResponse(ok(R"({"updateAvailable":true,"latest":"1.0.0"})"), v).outcome, Outcome::Failed); // no url
        for (int status : {0, 201, 301, 404, 503}) {
            FetchResult r = ok(kCurrent);
            r.status = status;
            QCOMPARE(parseResponse(r, v).outcome, Outcome::Failed);
        }
        FetchResult big = ok(newer());
        big.tooLarge = true;
        QCOMPARE(parseResponse(big, v).outcome, Outcome::Failed);
        FetchResult huge = ok(newer() + QByteArray(int(kMaxBodyBytes), ' '));
        QCOMPARE(parseResponse(huge, v).outcome, Outcome::Failed);
    }

    void onlyCohenzcoUrlsAreOffered()
    {
        const QString v = QStringLiteral("0.18.0");
        for (const char* bad : {"http://cohenzco.com/x", "https://evil.example/x", "https://cohenzco.com.evil.example/x",
                                "https://cohenzco.com@evil.example/", "https://cohenzco.com:8443/x", "file:///etc/passwd",
                                "https://cohenzco.com", "javascript:alert(1)", ""}) {
            QVERIFY2(!isTrustedUrl(QString::fromLatin1(bad)), bad);
            QCOMPARE(parseResponse(ok(newer(QString::fromLatin1(bad))), v).outcome, Outcome::Failed);
        }
        QVERIFY(isTrustedUrl(QStringLiteral("https://cohenzco.com/portfolio/nimiz/clipitto")));
    }

    void theTextsAreAsSpecified()
    {
        QCOMPARE(availableText(QStringLiteral("0.19.0"), QStringLiteral("0.18.0")),
                 QStringLiteral("ClipItTo 0.19.0 is available (you have 0.18.0)"));
        QCOMPARE(upToDateText(QStringLiteral("0.18.0")), QStringLiteral("You are up to date (0.18.0)"));
        QCOMPARE(failedText(), QStringLiteral("Could not check for updates. Try again later."));
    }

    void aClickShowsAvailableThenDownloadOpensTheUrl()
    {
        auto r = rig();
        r->fetcher->answer = ok(newer());
        r->shown.download = true;
        r->controller->checkNow();
        QVERIFY(r->shown.asked.isEmpty()); // in the background: nothing yet
        settle();
        QCOMPARE(r->shown.asked, QStringList{QStringLiteral("ClipItTo 0.19.0 is available (you have 0.18.0)")});
        QCOMPARE(r->opened, QList<QUrl>{QUrl(QStringLiteral("https://cohenzco.com/portfolio/nimiz/clipitto"))});
        QVERIFY(r->shown.informed.isEmpty());
    }

    void laterOpensNothingAndIsRemembered()
    {
        auto r = rig();
        r->fetcher->answer = ok(newer());
        r->shown.download = false;
        r->controller->checkNow();
        settle();
        QVERIFY(r->opened.isEmpty());
        QCOMPARE(r->controller->laterVersion(), QStringLiteral("0.19.0"));
        // A click still answers (the user asked)...
        r->controller->checkNow();
        settle();
        QCOMPARE(r->shown.asked.size(), 2);
        // ...but the automatic check stays quiet for that version.
        r->now = r->now.addDays(2);
        QVERIFY(r->controller->autoCheck(true));
        settle();
        QCOMPARE(r->shown.asked.size(), 2);
        // A still newer version is announced again.
        r->fetcher->answer = ok(newer().replace("0.19.0", "0.20.0"));
        r->now = r->now.addDays(2);
        QVERIFY(r->controller->autoCheck(true));
        settle();
        QCOMPARE(r->shown.asked.size(), 3);
        QVERIFY(r->shown.asked.last().contains(QLatin1String("0.20.0")));
    }

    void aClickWhenUpToDate()
    {
        auto r = rig();
        r->fetcher->answer = ok(kCurrent);
        r->controller->checkNow();
        settle();
        QCOMPARE(r->shown.informed, QStringList{QStringLiteral("You are up to date (0.18.0)")});
        QVERIFY(r->shown.asked.isEmpty());
    }

    void aClickWhenItFails()
    {
        auto r = rig();
        for (int status : {0, 503}) {
            r->fetcher->answer.status = status;
            r->controller->checkNow();
            settle();
        }
        r->fetcher->answer = ok("garbage");
        r->controller->checkNow();
        settle();
        QCOMPARE(r->shown.informed, (QStringList{failedText(), failedText(), failedText()}));
        QCOMPARE(r->fetcher->urls.size(), 3); // one request per click: no retry loop
    }

    void anOfferToAnotherHostIsNeverOpened()
    {
        auto r = rig();
        r->fetcher->answer = ok(newer(QStringLiteral("https://evil.example/x")));
        r->shown.download = true;
        r->controller->checkNow();
        settle();
        QVERIFY(r->opened.isEmpty());
        QVERIFY(r->shown.asked.isEmpty());
        QCOMPARE(r->shown.informed, QStringList{failedText()});
    }

    void theAutomaticCheckIsOffByDefaultAndSilentWithoutNews()
    {
        QVERIFY(!Config().updateCheckAuto);
        QVERIFY(!Config::fromToml("").updateCheckAuto);
        QVERIFY(Config::fromToml("update_check_auto = true\n").updateCheckAuto);
        auto r = rig();
        r->fetcher->answer = ok(newer());
        QVERIFY(!r->controller->autoCheck(false)); // off: not a single request
        settle();
        QVERIFY(r->fetcher->urls.isEmpty());
        // On, but nothing new, or an error: nothing is shown.
        r->fetcher->answer = ok(kCurrent);
        QVERIFY(r->controller->autoCheck(true));
        settle();
        r->now = r->now.addDays(1);
        r->fetcher->answer.status = 503;
        QVERIFY(r->controller->autoCheck(true));
        settle();
        QCOMPARE(r->fetcher->urls.size(), 2);
        QVERIFY(r->shown.asked.isEmpty());
        QVERIFY(r->shown.informed.isEmpty());
    }

    void theAutomaticCheckRunsAtMostOnceADay()
    {
        auto r = rig();
        r->fetcher->answer.status = 503; // a failure counts as a check: no retry until tomorrow
        QVERIFY(r->controller->autoCheck(true));
        settle();
        r->now = r->now.addSecs(23 * 3600);
        QVERIFY(!r->controller->autoCheck(true));
        r->now = r->now.addSecs(2 * 3600);
        QVERIFY(r->controller->autoCheck(true));
        settle();
        QCOMPARE(r->fetcher->urls.size(), 2);
        QVERIFY(autoCheckDue(State{}, r->now));
    }

    void anAutomaticAnnouncementIsShown()
    {
        auto r = rig();
        r->fetcher->answer = ok(newer());
        QVERIFY(r->controller->autoCheck(true));
        settle();
        QCOMPARE(r->shown.asked.size(), 1);
    }

    void stateRoundTrips()
    {
        State s;
        s.lastAutoCheck = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), Qt::UTC);
        s.laterVersion = QStringLiteral("0.19.0");
        QVERIFY(saveState(statePath(), s));
        const State back = loadState(statePath());
        QCOMPARE(back.lastAutoCheck, s.lastAutoCheck);
        QCOMPARE(back.laterVersion, s.laterVersion);
        QVERIFY(loadState(m_dir.filePath(QStringLiteral("missing.json"))).laterVersion.isEmpty());
    }

    // The Microsoft Store build (MSIX) is updated by the Store: the update check is off there, and nothing is requested.
    void theStoreBuildHasNoUpdateCheck()
    {
        using clipitto::packaging::isPackaged;
        using clipitto::packaging::packagedFromResult;
        using clipitto::packaging::setPackagedOverrideForTest;
        // GetCurrentPackageFullName(&len, nullptr): 122 (insufficient buffer) or 0 = packaged; 15700 = a plain exe.
        QVERIFY(packagedFromResult(122));
        QVERIFY(packagedFromResult(0));
        QVERIFY(!packagedFromResult(15700));
        QVERIFY(!packagedFromResult(5));
        QVERIFY(!isPackaged()); // the test programs are plain exes (and this is not Windows on Linux)

        setPackagedOverrideForTest(true);
        QVERIFY(isPackaged());
        QVERIFY(clipitto::Daemon::updatesDisabled());
        selftest::Env env;
        env.config.updateCheckAuto = true;
        const QString text = selftest::updateCheckResult(env).detail;
        QVERIFY2(text.contains(QLatin1String("Microsoft Store")), qPrintable(text));
        QVERIFY(!text.contains(QLatin1String("cohenzco.com")));
        setPackagedOverrideForTest(std::nullopt);
        QVERIFY(!isPackaged());
        QVERIFY(!clipitto::Daemon::updatesDisabled());
        QVERIFY(selftest::updateCheckResult(env).detail.contains(QLatin1String("cohenzco.com")));
    }

    void aBuildWithoutAVersionSendsNothing()
    {
        auto fetcher = std::make_shared<FakeFetcher>();
        Checker checker(fetcher, QString());
        QVERIFY(!checker.check([](Result) {}));
        Checker odd(fetcher, QStringLiteral("1.0 beta&x=1"));
        QVERIFY(!odd.check([](Result) {}));
        QVERIFY(fetcher->urls.isEmpty());
    }
};

QTEST_MAIN(TstUpdate)
#include "tst_update.moc"
