#include "UpdateController.h"

#include <QDesktopServices>
#include <QMessageBox>
#include <QPushButton>

namespace clipitto::ui {

UpdatePresenter messageBoxPresenter()
{
    UpdatePresenter presenter;
    presenter.askDownload = [](QWidget* parent, const QString& text) {
        QMessageBox box(QMessageBox::Information, QObject::tr("Check for updates"), text, QMessageBox::NoButton, parent);
        QPushButton* download = box.addButton(QObject::tr("Download"), QMessageBox::AcceptRole);
        box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
        box.setDefaultButton(download);
        box.exec();
        return box.clickedButton() == download;
    };
    presenter.inform = [](QWidget* parent, const QString& text) {
        QMessageBox::information(parent, QObject::tr("Check for updates"), text);
    };
    return presenter;
}

UpdateController::UpdateController(std::shared_ptr<update::Fetcher> fetcher, QString version, QString statePath, QObject* parent)
    : QObject(parent)
    , m_fetcher(std::move(fetcher))
    , m_version(std::move(version))
    , m_statePath(std::move(statePath))
    , m_presenter(messageBoxPresenter())
    , m_openUrl([](const QUrl& url) { QDesktopServices::openUrl(url); })
    , m_clock([] { return QDateTime::currentDateTimeUtc(); })
{
}

void UpdateController::checkNow(QWidget* parent)
{
    if (busy()) {
        return;
    }
    if (!start(/*silent=*/false, parent) && m_presenter.inform) {
        m_presenter.inform(parent, update::failedText());
    }
}

bool UpdateController::autoCheck(bool enabled)
{
    if (!enabled || busy()) {
        return false;
    }
    update::State state = update::loadState(m_statePath);
    const QDateTime now = m_clock();
    if (!update::autoCheckDue(state, now)) {
        return false;
    }
    // Remembered before the request, so a failure is not retried until tomorrow (no retry loop).
    state.lastAutoCheck = now;
    update::saveState(m_statePath, state);
    return start(/*silent=*/true, nullptr);
}

bool UpdateController::start(bool silent, QWidget* parent)
{
    if (!m_checker) {
        if (!m_fetcher) {
            m_fetcher = update::createNetworkFetcher();
        }
        m_checker = std::make_unique<update::Checker>(m_fetcher, m_version);
    }
    return m_checker->check([this, silent, guard = QPointer<QWidget>(parent)](update::Result result) {
        show(result, silent, guard);
    });
}

void UpdateController::show(const update::Result& result, bool silent, QWidget* parent)
{
    switch (result.outcome) {
    case update::Outcome::Failed:
        if (!silent && m_presenter.inform) {
            m_presenter.inform(parent, update::failedText());
        }
        return;
    case update::Outcome::UpToDate:
        if (!silent && m_presenter.inform) {
            m_presenter.inform(parent, update::upToDateText(result.current));
        }
        return;
    case update::Outcome::Available:
        break;
    }
    update::State state = update::loadState(m_statePath);
    if (silent && state.laterVersion == result.latest) {
        return; // put off with Later: not asked again for this version
    }
    if (!m_presenter.askDownload) {
        return;
    }
    if (m_presenter.askDownload(parent, update::availableText(result.latest, result.current))) {
        // Only the page the server named, and only on cohenzco.com (parseResponse checked); nothing is downloaded or run.
        if (update::isTrustedUrl(result.url.toString()) && m_openUrl) {
            m_openUrl(result.url);
        }
    } else {
        state.laterVersion = result.latest;
        update::saveState(m_statePath, state);
    }
}

} // namespace clipitto::ui
