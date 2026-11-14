#pragma once

#include "SelfTest.h"

#include <QDialog>

#include <functional>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace clipitto::ui {

// What the Settings window needs to show "Check this installation": the daemon makes the runner (the checks run there).
struct SelfTestSupport {
    // A runner that is not started yet (or the one already running), given the window's paste test box. Null: no self-test here.
    std::function<selftest::Runner*(const selftest::PasteBox&)> create;
    // Puts text on the system clipboard as an own write, so the capture does not record it as a copy. Null: the plain clipboard.
    std::function<void(const QString&)> copyText;
};

// The results of the self-test, one line per check (PASS / WARN / FAIL / SKIPPED, what was found, what to do), a Copy report
// button, and the box the paste check types into. Shown from Settings > General > Check this installation.
class SelfTestWindow : public QDialog {
    Q_OBJECT

public:
    explicit SelfTestWindow(SelfTestSupport support, QWidget* parent = nullptr);

    void run(); // starts the checks (again)
    bool isRunning() const { return m_running; }
    QString reportText() const { return m_report.text(); }
    const selftest::Report& report() const { return m_report; }

    QListWidget* resultList() const { return m_list; }
    QPushButton* copyButton() const { return m_copy; }
    QPushButton* runButton() const { return m_run; }
    QPlainTextEdit* pasteBox() const { return m_pasteBox; }
    QLabel* summaryLabel() const { return m_summary; }
    QLabel* privacyLabel() const { return m_privacy; }

signals:
    void finished(const clipitto::selftest::Report& report); // a run ended

private:
    void copyReport();
    void addResult(const selftest::Result& result);

    SelfTestSupport m_support;
    QListWidget* m_list;
    QLabel* m_summary;
    QLabel* m_privacy;
    QPlainTextEdit* m_pasteBox;
    QPushButton* m_copy;
    QPushButton* m_run;
    selftest::Report m_report;
    bool m_running = false;
};

} // namespace clipitto::ui
