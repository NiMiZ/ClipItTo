#include "UpdateCheck.h"

#include "Paths.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

Q_LOGGING_CATEGORY(lcUpdate, "clipitto.update")

namespace clipitto::update {

namespace {

bool validVersion(const QString& version)
{
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9.+-]{1,40}$"));
    return ok.match(version).hasMatch();
}

const char* outcomeName(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Available:
        return "update available";
    case Outcome::UpToDate:
        return "up to date";
    case Outcome::Failed:
        break;
    }
    return "failed";
}

class NetworkFetcher final : public Fetcher {
public:
    void get(const QUrl& url, const QByteArray& userAgent, std::function<void(FetchResult)> done) override
    {
        if (url.scheme() != QLatin1String("https")) { // HTTPS only; never reached with our own constant
            QTimer::singleShot(0, [done = std::move(done)] { done(FetchResult{}); });
            return;
        }
        if (!m_nam) {
            m_nam = std::make_unique<QNetworkAccessManager>();
            m_nam->setAutoDeleteReplies(false);
        }
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", userAgent);
        request.setRawHeader("Accept", "application/json");
        // Nothing but the request itself: no cookies either way, no cache, and no redirect is followed (a 3xx is a failure).
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        request.setTransferTimeout(kTimeoutMs);

        QNetworkReply* reply = m_nam->get(request);
        struct Shared {
            QByteArray body;
            bool tooLarge = false;
            bool finished = false;
        };
        auto shared = std::make_shared<Shared>();
        auto* timer = new QTimer(reply); // the whole exchange, not only a stalled transfer
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, reply, [reply] { reply->abort(); });
        timer->start(kTimeoutMs);
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, shared] {
            shared->body += reply->readAll();
            if (shared->body.size() > kMaxBodyBytes && !shared->tooLarge) {
                shared->tooLarge = true;
                shared->body.clear();
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, shared, done = std::move(done)] {
            if (shared->finished) {
                return;
            }
            shared->finished = true;
            FetchResult result;
            result.tooLarge = shared->tooLarge;
            if (!shared->tooLarge) {
                shared->body += reply->readAll();
                result.tooLarge = shared->body.size() > kMaxBodyBytes;
                result.body = result.tooLarge ? QByteArray() : shared->body;
                if (reply->error() == QNetworkReply::NoError || reply->error() >= QNetworkReply::ContentAccessDenied) {
                    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                }
            }
            reply->deleteLater();
            done(std::move(result));
        });
    }

private:
    std::unique_ptr<QNetworkAccessManager> m_nam;
};

} // namespace

std::unique_ptr<Fetcher> createNetworkFetcher()
{
    return std::make_unique<NetworkFetcher>();
}

QString platformName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

QUrl requestUrl(const QString& version, const QString& platform)
{
    const QString text = QStringLiteral("%1?version=%2&platform=%3")
                             .arg(QLatin1String(kApiBase), QString::fromLatin1(QUrl::toPercentEncoding(version)),
                                  QString::fromLatin1(QUrl::toPercentEncoding(platform)));
    return QUrl(text, QUrl::StrictMode);
}

bool isTrustedUrl(const QString& url)
{
    if (!url.startsWith(QLatin1String(kTrustedPrefix))) {
        return false;
    }
    // "https://cohenzco.com/..." must really be that host: no user info ("cohenzco.com/@evil" is still a path, but be strict),
    // and the parsed host has to match.
    const QUrl parsed(url, QUrl::StrictMode);
    return parsed.isValid() && parsed.scheme() == QLatin1String("https") && parsed.host() == QLatin1String("cohenzco.com")
           && parsed.userInfo().isEmpty() && parsed.port() == -1;
}

QString availableText(const QString& latest, const QString& current)
{
    return QCoreApplication::translate("clipitto::update", "ClipItTo %1 is available (you have %2)").arg(latest, current);
}

QString upToDateText(const QString& current)
{
    return QCoreApplication::translate("clipitto::update", "You are up to date (%1)").arg(current);
}

QString failedText()
{
    return QCoreApplication::translate("clipitto::update", "Could not check for updates. Try again later.");
}

Result parseResponse(const FetchResult& response, const QString& sentVersion)
{
    Result failed;
    if (response.status != 200 || response.tooLarge || response.body.size() > kMaxBodyBytes) {
        return failed;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(response.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return failed;
    }
    const QJsonObject object = doc.object();
    const QJsonValue available = object.value(QLatin1String("updateAvailable"));
    if (!available.isBool()) {
        return failed;
    }
    Result result;
    result.current = object.value(QLatin1String("current")).isString() ? object.value(QLatin1String("current")).toString()
                                                                          : sentVersion;
    if (result.current.isEmpty() || result.current.size() > 64) {
        result.current = sentVersion;
    }
    if (!available.toBool()) {
        result.outcome = Outcome::UpToDate;
        return result;
    }
    const QString latest = object.value(QLatin1String("latest")).toString();
    const QString url = object.value(QLatin1String("url")).toString();
    if (latest.isEmpty() || latest.size() > 64 || !isTrustedUrl(url)) {
        return failed; // an offer we could not honour safely is not an offer
    }
    result.outcome = Outcome::Available;
    result.latest = latest;
    result.url = QUrl(url, QUrl::StrictMode);
    return result;
}

State loadState(const QString& path)
{
    State state;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return state;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(16 * 1024));
    if (doc.isObject()) {
        state.lastAutoCheck = QDateTime::fromString(doc.object().value(QLatin1String("last_auto_check")).toString(), Qt::ISODate);
        state.laterVersion = doc.object().value(QLatin1String("later_version")).toString().left(64);
    }
    return state;
}

bool saveState(const QString& path, const State& state)
{
    if (!paths::ensureParentDir(path)) {
        return false;
    }
    QJsonObject object;
    object.insert(QLatin1String("last_auto_check"), state.lastAutoCheck.isValid() ? state.lastAutoCheck.toUTC().toString(Qt::ISODate) : QString());
    object.insert(QLatin1String("later_version"), state.laterVersion);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        return false;
    }
    paths::restrictToOwner(path);
    return true;
}

bool autoCheckDue(const State& state, const QDateTime& now)
{
    if (!state.lastAutoCheck.isValid()) {
        return true;
    }
    const qint64 seconds = state.lastAutoCheck.secsTo(now);
    return seconds < 0 /* clock set back */ || seconds >= qint64(kAutoIntervalHours) * 3600;
}

Checker::Checker(std::shared_ptr<Fetcher> fetcher, QString version, QString platform)
    : m_fetcher(std::move(fetcher))
    , m_version(std::move(version))
    , m_platform(std::move(platform))
{
}

bool Checker::check(std::function<void(Result)> done)
{
    if (m_busy || !m_fetcher || !validVersion(m_version)) {
        return false;
    }
    m_busy = true;
    qCInfo(lcUpdate) << "update check started"; // the fact only: not the address, the version or the answer
    const QByteArray userAgent = "ClipItTo/" + m_version.toLatin1();
    std::weak_ptr<bool> alive = m_alive;
    m_fetcher->get(requestUrl(m_version, m_platform), userAgent,
                   [this, alive, done = std::move(done), version = m_version](FetchResult response) {
                       if (alive.expired()) {
                           return;
                       }
                       m_busy = false;
                       const Result result = parseResponse(response, version);
                       qCInfo(lcUpdate) << "update check finished:" << outcomeName(result.outcome);
                       if (done) {
                           done(result);
                       }
                   });
    return true;
}

} // namespace clipitto::update
