#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

// "Check this installation" (roadmap 31): a list of small checks, each saying PASS / WARN / FAIL / SKIPPED with a
// plain reason and what to do, run one after the other with a time limit each, so the test itself never hangs.
//
// Privacy: a Result carries versions, platform names, on/off states, counts, ids and the paths of ClipItTo's own files.
// Never what was copied, window titles, passwords, key material or the names of copies. The checks are written to
// that rule, and tst_selftest feeds them a fake keyring and clipboard that hold a marker string and asserts it never shows.
namespace clipitto::selftest {

enum class Outcome { Pass, Warn, Fail, Skipped };

QString outcomeWord(Outcome outcome); // "PASS", "WARN", "FAIL", "SKIPPED"
// A symbol for the list in the window: a check mark, a warning sign, a cross, a dash.
QString outcomeSymbol(Outcome outcome);

struct Result {
    QString id; // "capture_backend": stable, for tests and the CLI
    QString title; // "Capture backend"
    Outcome outcome = Outcome::Skipped;
    QString detail; // what was found, or why it was skipped
    QString advice; // what to do (empty for PASS)

    static Result pass(const QString& detail) { return {{}, {}, Outcome::Pass, detail, {}}; }
    static Result warn(const QString& detail, const QString& advice) { return {{}, {}, Outcome::Warn, detail, advice}; }
    static Result fail(const QString& detail, const QString& advice) { return {{}, {}, Outcome::Fail, detail, advice}; }
    static Result skipped(const QString& reason) { return {{}, {}, Outcome::Skipped, reason, {}}; }
};

struct Report {
    QString header; // version, platform: lines above the list
    QList<Result> results;

    int count(Outcome outcome) const;
    bool failed() const { return count(Outcome::Fail) > 0; }
    const Result* find(const QString& id) const;

    // The text the Copy report button puts on the clipboard (and --selftest prints). Plain, no markup.
    QString text() const;
    // For the control channel: {"report": text, "failed": n, "warned": n, ...}; the CLI prints "report".
    QVariantMap toMap() const;
};

// The sentence the window and the report carry about what is, and is not, in a report.
QString privacyNote();

using Done = std::function<void(Result)>;

// The test box of the Settings window, as the paste check sees it: `prepare` empties it and gives it the focus,
// `text` says what it holds now. The CLI has none.
struct PasteBox {
    std::function<void()> prepare;
    std::function<QString()> text;
    std::function<bool()> hasFocus; // the box is focused in the active window; empty: not checked
};

struct Check {
    QString id;
    QString title;
    int timeoutMs = 10000;
    // Exactly one of the two is set.
    //   run:   may block (keyring, disk, a subprocess); the runner calls it on a worker thread. It must not touch the GUI.
    //   start: runs on the GUI thread and calls `done` later (once) on the GUI thread: for what waits for a signal.
    std::function<Result()> run;
    std::function<void(Done done)> start;
};

// Runs the checks one after the other. Each has its own time limit: a check that has not answered by then becomes a
// FAIL ("did not answer in N s") and the run goes on; its answer, if it comes later, is dropped. A blocked worker thread
// is left to finish by itself (it can't be stopped), which is why the limit exists.
class Runner : public QObject {
    Q_OBJECT

public:
    Runner(QList<Check> checks, QString header, QObject* parent = nullptr);
    ~Runner() override;

    void start();
    bool isRunning() const { return m_running; }
    // Scales every check's time limit (tests on a slow runner; 1.0 otherwise).
    void setTimeScale(double scale) { m_scale = scale; }

signals:
    void checkStarted(int index, const QString& title);
    void checkFinished(int index, const clipitto::selftest::Result& result);
    void finished(const clipitto::selftest::Report& report);

private:
    void next();
    void finishCheck(int index, Result result);

    QList<Check> m_checks;
    Report m_report;
    int m_index = -1;
    quint64 m_serial = 0; // identifies the check being waited for; a late answer to an older one is dropped
    bool m_running = false;
    double m_scale = 1.0;
};

} // namespace clipitto::selftest

Q_DECLARE_METATYPE(clipitto::selftest::Result)
Q_DECLARE_METATYPE(clipitto::selftest::Report)
