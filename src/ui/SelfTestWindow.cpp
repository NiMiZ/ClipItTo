#include "SelfTestWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace clipitto::ui {

SelfTestWindow::SelfTestWindow(SelfTestSupport support, QWidget* parent)
    : QDialog(parent)
    , m_support(std::move(support))
    , m_list(new QListWidget(this))
    , m_summary(new QLabel(this))
    , m_privacy(new QLabel(this))
    , m_pasteBox(new QPlainTextEdit(this))
    , m_copy(new QPushButton(tr("Copy report"), this))
    , m_run(new QPushButton(tr("Check again"), this))
{
    setWindowTitle(tr("Check this installation"));
    setObjectName(QStringLiteral("selfTestWindow"));
    m_list->setObjectName(QStringLiteral("selfTestList"));
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setAlternatingRowColors(true);
    m_pasteBox->setObjectName(QStringLiteral("selfTestPasteBox"));
    m_pasteBox->setPlaceholderText(tr("Test box: the paste check types into this box"));
    m_pasteBox->setMaximumHeight(60);
    m_copy->setObjectName(QStringLiteral("selfTestCopy"));
    m_run->setObjectName(QStringLiteral("selfTestRun"));
    m_privacy->setObjectName(QStringLiteral("selfTestPrivacy"));
    m_privacy->setWordWrap(true);
    m_privacy->setEnabled(false); // dim
    m_privacy->setText(tr("The report lists versions, the platform, on/off settings, counts, language ids and the paths of "
                          "ClipItTo's own files. It never contains what you copied, window titles, passwords, keys or the names "
                          "of copies. The checks put a short test text on the clipboard and take it out of the history again; "
                          "the clipboard may keep that test text."));
    m_summary->setObjectName(QStringLiteral("selfTestSummary"));
    m_summary->setWordWrap(true);

    auto* buttons = new QHBoxLayout;
    auto* close = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_copy);
    buttons->addWidget(m_run);
    buttons->addStretch(1);
    buttons->addWidget(close);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_pasteBox);
    layout->addWidget(m_privacy);
    layout->addLayout(buttons);
    resize(760, 560);

    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_copy, &QPushButton::clicked, this, &SelfTestWindow::copyReport);
    connect(m_run, &QPushButton::clicked, this, &SelfTestWindow::run);
    m_copy->setEnabled(false);
    m_run->setEnabled(m_support.create != nullptr);
}

void SelfTestWindow::run()
{
    if (m_running || !m_support.create) {
        return;
    }
    // The box the paste check types into: emptied and focused right before the keys go out.
    const QPointer<QPlainTextEdit> box = m_pasteBox;
    selftest::PasteBox pasteBox;
    pasteBox.prepare = [box, window = QPointer<SelfTestWindow>(this)] {
        if (box && window) {
            box->clear();
            window->activateWindow();
            box->setFocus(Qt::OtherFocusReason);
        }
    };
    pasteBox.text = [box] { return box ? box->toPlainText() : QString(); };
    pasteBox.hasFocus = [box, window = QPointer<SelfTestWindow>(this)] {
        return box && window && box->hasFocus() && window->isActiveWindow();
    };
    selftest::Runner* runner = m_support.create(pasteBox);
    if (!runner) {
        return;
    }
    m_running = true;
    m_run->setEnabled(false);
    m_copy->setEnabled(false);
    m_list->clear();
    m_report = {};
    m_summary->setText(tr("Checking…"));
    connect(runner, &selftest::Runner::checkStarted, this,
            [this](int, const QString& title) { m_summary->setText(tr("Checking: %1…").arg(title)); });
    connect(runner, &selftest::Runner::checkFinished, this, [this](int, const selftest::Result& result) { addResult(result); });
    connect(runner, &selftest::Runner::finished, this, [this](const selftest::Report& report) {
        m_report = report;
        m_running = false;
        m_run->setEnabled(true);
        m_copy->setEnabled(true);
        m_summary->setText(tr("%1 passed, %2 warnings, %3 failed, %4 skipped.")
                               .arg(report.count(selftest::Outcome::Pass)).arg(report.count(selftest::Outcome::Warn))
                               .arg(report.count(selftest::Outcome::Fail)).arg(report.count(selftest::Outcome::Skipped)));
        emit finished(report);
    });
    if (!runner->isRunning()) {
        runner->start();
    }
}

void SelfTestWindow::addResult(const selftest::Result& result)
{
    QString text = QStringLiteral("%1 %2  %3: %4").arg(selftest::outcomeSymbol(result.outcome), selftest::outcomeWord(result.outcome),
                                                       result.title, result.detail);
    if (!result.advice.isEmpty()) {
        text += tr("\n      What to do: %1").arg(result.advice);
    }
    auto* item = new QListWidgetItem(text, m_list);
    switch (result.outcome) {
    case selftest::Outcome::Fail: item->setForeground(QColor(0xd0, 0x30, 0x30)); break;
    case selftest::Outcome::Warn: item->setForeground(QColor(0xc0, 0x80, 0x00)); break;
    case selftest::Outcome::Pass:
    case selftest::Outcome::Skipped: break;
    }
    m_list->scrollToBottom();
}

void SelfTestWindow::copyReport()
{
    // Through the daemon when it can: an own write, so the capture does not record the report as a copy.
    const QString text = m_report.text();
    if (m_support.copyText) {
        m_support.copyText(text);
    } else {
        QApplication::clipboard()->setText(text);
    }
}

} // namespace clipitto::ui
