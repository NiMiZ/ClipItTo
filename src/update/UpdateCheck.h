#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

// "Check for updates". The app asks https://cohenzco.com/api/apps/clipitto/latest whether a newer version exists and, if so,
// offers to open the download page in the browser. It never downloads or runs anything.
//
// Privacy rules (README, Privacy): the request happens only when the user clicks "Check for updates..." or has turned on the
// automatic check (off by default). It carries the app's version and the platform name in the query, and a User-Agent
// "ClipItTo/<version>": no cookies, no id, no clipboard data. The logs say that a check ran and its outcome class, nothing else.
//
// The network is behind `Fetcher`, so tests inject canned answers and never touch the network.
namespace clipitto::update {

constexpr const char* kApiBase = "https://cohenzco.com/api/apps/clipitto/latest";
constexpr const char* kTrustedPrefix = "https://cohenzco.com/"; // the only place the Download button may lead
constexpr int kTimeoutMs = 8000;
constexpr qint64 kMaxBodyBytes = 64 * 1024;
constexpr int kAutoIntervalHours = 24;

// What the transport returned. status 0 = no answer (error, timeout, refused redirect). tooLarge: the body passed the cap.
struct FetchResult {
    int status = 0;
    QByteArray body;
    bool tooLarge = false;
};

class Fetcher {
public:
    virtual ~Fetcher() = default;
    // One GET, in the background; `done` is called once, on the calling thread's event loop. No retry.
    virtual void get(const QUrl& url, const QByteArray& userAgent, std::function<void(FetchResult)> done) = 0;
};

// The real transport: QNetworkAccessManager; HTTPS only, 8 s, no cookies, no cache, no redirects, body capped.
std::unique_ptr<Fetcher> createNetworkFetcher();

enum class Outcome { Available, UpToDate, Failed };

struct Result {
    Outcome outcome = Outcome::Failed;
    QString latest;
    QString current; // what the server says; falls back to the version we sent
    QUrl url; // set only for Available, and only on cohenzco.com
};

QString platformName(); // "windows", "linux" or "macos"
QUrl requestUrl(const QString& version, const QString& platform);
bool isTrustedUrl(const QString& url);
// The pure part: status, size cap, JSON shape, "url" check. Anything unexpected is Failed.
Result parseResponse(const FetchResult& response, const QString& sentVersion);

// The texts (exactly as in the API document); the dialogs and the tests use these.
QString availableText(const QString& latest, const QString& current);
QString upToDateText(const QString& current);
QString failedText();

// What is remembered between runs: the last automatic check and the version the user chose "Later" for. Stays on this
// computer; nothing here is ever sent.
struct State {
    QDateTime lastAutoCheck;
    QString laterVersion;
};
State loadState(const QString& path);
bool saveState(const QString& path, const State& state);
// The automatic check runs at most once a day.
bool autoCheckDue(const State& state, const QDateTime& now);

// One check at a time.
class Checker {
public:
    Checker(std::shared_ptr<Fetcher> fetcher, QString version, QString platform = platformName());
    ~Checker() { *m_alive = false; }
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;

    bool busy() const { return m_busy; }
    // Starts a check; `done` is called once with the result (not at all if the Checker is gone). Returns false, and does
    // nothing, while one is running or when the version is empty (a build without a version sends nothing).
    bool check(std::function<void(Result)> done);

private:
    std::shared_ptr<Fetcher> m_fetcher;
    QString m_version;
    QString m_platform;
    bool m_busy = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace clipitto::update
