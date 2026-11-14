#include "SelfTest.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>

namespace clipitto::selftest {

QString outcomeWord(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Pass: return QStringLiteral("PASS");
    case Outcome::Warn: return QStringLiteral("WARN");
    case Outcome::Fail: return QStringLiteral("FAIL");
    case Outcome::Skipped: return QStringLiteral("SKIPPED");
    }
    return {};
}

QString outcomeSymbol(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Pass: return QString(QChar(0x2714));
    case Outcome::Warn: return QString(QChar(0x26A0));
    case Outcome::Fail: return QString(QChar(0x2718));
    case Outcome::Skipped: return QString(QChar(0x2212));
    }
    return {};
}

QString privacyNote()
{
    return QStringLiteral("This report lists versions, the platform, on/off settings, counts, language ids and the paths of "
                          "ClipItTo's own files. It never contains what you copied, window titles, passwords, keys or the "
                          "names of copies.");
}

int Report::count(Outcome outcome) const
{
    int n = 0;
    for (const Result& r : results) {
        n += r.outcome == outcome ? 1 : 0;
    }
    return n;
}

const Result* Report::find(const QString& id) const
{
    for (const Result& r : results) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

QString Report::text() const
{
    QStringList lines;
    lines << QStringLiteral("ClipItTo: check this installation");
    if (!header.isEmpty()) {
        lines << header;
    }
    lines << QString();
    for (const Result& r : results) {
        lines << QStringLiteral("[%1] %2: %3").arg(outcomeWord(r.outcome), r.title, r.detail);
        if (!r.advice.isEmpty()) {
            lines << QStringLiteral("        To do: %1").arg(r.advice);
        }
    }
    lines << QString();
    lines << QStringLiteral("Summary: %1 PASS, %2 WARN, %3 FAIL, %4 SKIPPED")
                 .arg(count(Outcome::Pass)).arg(count(Outcome::Warn)).arg(count(Outcome::Fail)).arg(count(Outcome::Skipped));
    lines << privacyNote();
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QVariantMap Report::toMap() const
{
    QVariantMap map;
    map[QStringLiteral("report")] = text();
    map[QStringLiteral("pass")] = count(Outcome::Pass);
    map[QStringLiteral("warn")] = count(Outcome::Warn);
    map[QStringLiteral("fail")] = count(Outcome::Fail);
    map[QStringLiteral("skipped")] = count(Outcome::Skipped);
    return map;
}

namespace {

// A pool of our own: a check stuck in the keyring keeps its thread, and must not take one from anything else. Never
// destroyed (its destructor would wait for the stuck thread).
QThreadPool* workerPool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(8);
        return p;
    }();
    return pool;
}

} // namespace

Runner::Runner(QList<Check> checks, QString header, QObject* parent)
    : QObject(parent)
    , m_checks(std::move(checks))
{
    m_report.header = std::move(header);
}

Runner::~Runner() = default;

void Runner::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_index = -1;
    m_report.results.clear();
    next();
}

void Runner::next()
{
    ++m_index;
    if (m_index >= m_checks.size()) {
        m_running = false;
        emit finished(m_report);
        return;
    }
    const int index = m_index;
    const Check check = m_checks.at(index);
    const quint64 serial = ++m_serial;
    emit checkStarted(index, check.title);

    const int limit = std::max(50, int(check.timeoutMs * m_scale));
    QTimer::singleShot(limit, this, [this, serial, index, limit] {
        if (serial != m_serial || !m_running || index != m_index) {
            return;
        }
        ++m_serial; // whatever the check says later is dropped
        finishCheck(index, Result::fail(QStringLiteral("did not answer in %1 s").arg(qMax(1, (limit + 500) / 1000)),
                                        QStringLiteral("Something is stuck here (a locked keyring waiting for you, a network "
                                                       "folder that does not answer, or a bug). Turn on the diagnostic log "
                                                       "in Settings and run the check again.")));
    });

    if (check.run) {
        auto* watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, serial, index] {
            Result result = watcher->future().result();
            watcher->deleteLater();
            if (serial == m_serial && m_running && index == m_index) {
                ++m_serial;
                finishCheck(index, std::move(result));
            }
        });
        watcher->setFuture(QtConcurrent::run(workerPool(), [run = check.run] { return run(); }));
    } else if (check.start) {
        check.start([self = QPointer<Runner>(this), serial, index](Result result) {
            if (self && serial == self->m_serial && self->m_running && index == self->m_index) {
                ++self->m_serial;
                self->finishCheck(index, std::move(result));
            }
        });
    } else {
        ++m_serial;
        finishCheck(index, Result::skipped(QStringLiteral("not available")));
    }
}

void Runner::finishCheck(int index, Result result)
{
    result.id = m_checks.at(index).id;
    result.title = m_checks.at(index).title;
    m_report.results.push_back(result);
    emit checkFinished(index, result);
    // Next turn of the event loop: a check that answers at once must not nest the next one inside its own call.
    QTimer::singleShot(0, this, &Runner::next);
}

} // namespace clipitto::selftest
