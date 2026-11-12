#pragma once

// Event-loop watchdog for the `perf` tests: measures the longest time the GUI thread went without processing events.
//
// A 5 ms timer ticks on the thread that creates the watchdog. Whenever the event loop is blocked (by work in a slot, a
// synchronous call in the test body, a lock, a blocking read) the ticks stop; the gap between two ticks, minus the tick
// interval, is a stall. `finish()` also counts the time since the last tick, so a test that does its work
// synchronously and then returns (without spinning the loop again) still sees it.
//
//   StallWatchdog wd;                      // starts at once
//   ... work, QTest::qWait(...), QTRY_...  // the loop must be spun for ticks to be seen
//   QVERIFY_NO_STALL(wd, "popup open");    // fails if the longest stall is over the budget; prints it either way
//
// Budget: 100 ms by default. Scaled by the environment: CI (CI=true, GITHUB_ACTIONS) x5, loose because shared runners
// pause; CLIPITTO_PERF_STRICT=1 x0.5 for tighter limits on a developer machine. A test can ask for another base budget.
// If CLIPITTO_PERF_REPORT=<file> is set, one line per measurement ("name<TAB>max ms<TAB>budget ms") is appended to it.
// Never put clipboard contents in a name or a message.

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cstdio>

class StallWatchdog : public QObject {
public:
    static constexpr int kTickMs = 5;

    // Base budget in ms before the CI / strict scaling.
    explicit StallWatchdog(int baseBudgetMs = 100, QObject* parent = nullptr)
        : QObject(parent), m_budgetMs(scaled(baseBudgetMs))
    {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(kTickMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
        restart();
    }

    // Forget what was measured and start again (the loop must be running for ticks).
    void restart()
    {
        m_max = 0;
        m_clock.start();
        m_last = 0;
        m_timer.start();
    }

    int budgetMs() const { return m_budgetMs; }
    // When the longest stall ended, in ms since start (to place it next to what the test did).
    qint64 maxStallEndedAtMs() const { return m_maxAt; }

    // The longest stall so far, in ms (the open gap since the last tick included).
    qint64 maxStallMs() const
    {
        const qint64 open = m_timer.isActive() ? m_clock.elapsed() - m_last - kTickMs : 0;
        return std::max<qint64>(0, std::max(m_max, open));
    }

    // Stops measuring; returns the longest stall. Counts the gap since the last tick.
    qint64 finish()
    {
        if (m_timer.isActive()) {
            m_max = maxStallMs();
            m_timer.stop();
        }
        return m_max;
    }

    // Prints (and optionally records) the result; true when it is within the budget.
    bool report(const char* name)
    {
        const qint64 max = finish();
        std::fprintf(stderr, "PERF stall %-44s max %5lld ms  (budget %d ms)\n", name, static_cast<long long>(max), m_budgetMs);
        const QByteArray file = qgetenv("CLIPITTO_PERF_REPORT");
        if (!file.isEmpty()) {
            QFile out(QString::fromLocal8Bit(file));
            if (out.open(QIODevice::Append | QIODevice::Text)) {
                out.write(QByteArray(name) + '\t' + QByteArray::number(max) + '\t' + QByteArray::number(m_budgetMs) + '\n');
            }
        }
        return max <= m_budgetMs;
    }

    static constexpr bool sanitizerBuild()
    {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
        return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
        return true;
#else
        return false;
#endif
#else
        return false;
#endif
    }

    // `baseMs` after the environment's scaling: for a test that has its own limit (a wall-clock limit, say).
    static int scaled(int baseMs)
    {
        double factor = 1.0;
        if (qEnvironmentVariableIsSet("CI") || qEnvironmentVariableIsSet("GITHUB_ACTIONS")) {
            factor = 5.0;
        } else if (qEnvironmentVariable("CLIPITTO_PERF_STRICT") == QLatin1String("1")) {
            factor = 0.5;
        }
        if (sanitizerBuild()) {
            factor *= 3.0; // an AddressSanitizer / ThreadSanitizer build runs several times slower
        }
        return std::max(1, static_cast<int>(baseMs * factor));
    }

    // Spins the event loop for `ms` (so ticks are seen), without a QTest dependency on a window.
    static void spin(int ms) { QTest::qWait(ms); }

private:
    void tick()
    {
        const qint64 now = m_clock.elapsed();
        if (now - m_last - kTickMs > m_max) {
            m_max = now - m_last - kTickMs;
            m_maxAt = now; // when the longest stall so far ended
        }
        m_last = now;
    }

    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    qint64 m_max = 0;
    qint64 m_maxAt = 0;
    int m_budgetMs;
};

// The longest GUI-thread stall since `wd` started must be within its budget.
#define QVERIFY_NO_STALL(wd, name)                                                                                 \
    do {                                                                                                           \
        const qint64 maxStall_ = (wd).finish();                                                                    \
        const bool within_ = (wd).report(name);                                                                    \
        QVERIFY2(within_, qPrintable(QStringLiteral("the GUI thread was blocked for %1 ms in \"%2\" (budget %3 ms)") \
                                         .arg(maxStall_).arg(QString::fromLatin1(name)).arg((wd).budgetMs())));    \
    } while (false)

// A wall-clock limit for a synchronous call: `what` must take at most `baseMs` (scaled like the stall budget).
#define QVERIFY_FAST(baseMs, name, what)                                                                           \
    do {                                                                                                           \
        QElapsedTimer fast_;                                                                                       \
        fast_.start();                                                                                             \
        what;                                                                                                      \
        const qint64 took_ = fast_.elapsed();                                                                      \
        const int limit_ = StallWatchdog::scaled(baseMs);                                                          \
        std::fprintf(stderr, "PERF time  %-44s took %5lld ms  (limit %d ms)\n", name, static_cast<long long>(took_), limit_); \
        QVERIFY2(took_ <= limit_, qPrintable(QStringLiteral("\"%1\" took %2 ms (limit %3 ms)")                      \
                                                  .arg(QString::fromLatin1(name)).arg(took_).arg(limit_)));         \
    } while (false)
