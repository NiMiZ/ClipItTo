#pragma once

#include "UpdateCheck.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

namespace clipitto::ui {

// What Settings needs for "Check for updates...": the daemon starts a check with `parent` as the owner of the dialog.
struct UpdateSupport {
    std::function<void(QWidget* parent)> check; // null: not available here
};

// The dialogs of "Check for updates" (replaceable, so tests never open a real one).
struct UpdatePresenter {
    std::function<bool(QWidget* parent, const QString& text)> askDownload; // true = Download, false = Later
    std::function<void(QWidget* parent, const QString& text)> inform;
};
UpdatePresenter messageBoxPresenter();

// Runs the check and shows the result (src/update/UpdateCheck.h has the rules). A check starts only from checkNow() (the
// user clicked) or autoCheck() (the user turned the setting on; at most once a day; silent unless a new version is out and
// not already put off with Later). Nothing else here ever touches the network.
class UpdateController : public QObject {
    Q_OBJECT
public:
    // fetcher null: the real network transport, created at the first check. version: the build's own (QCoreApplication's
    // applicationVersion, set from CLIPITTO_VERSION). statePath: where the last automatic check and the Later version are kept.
    UpdateController(std::shared_ptr<update::Fetcher> fetcher, QString version, QString statePath, QObject* parent = nullptr);

    void setPresenter(UpdatePresenter presenter) { m_presenter = std::move(presenter); }
    void setUrlOpener(std::function<void(const QUrl&)> opener) { m_openUrl = std::move(opener); }
    void setClock(std::function<QDateTime()> clock) { m_clock = std::move(clock); }

    // The user asked: always answers with a dialog (available / up to date / could not check).
    void checkNow(QWidget* parent = nullptr);
    // Start-up check. Does nothing unless `enabled`, and unless a day has passed since the last one. Returns whether a
    // request was started.
    bool autoCheck(bool enabled);

    bool busy() const { return m_checker && m_checker->busy(); }
    QString laterVersion() const { return update::loadState(m_statePath).laterVersion; }

private:
    bool start(bool silent, QWidget* parent);
    void show(const update::Result& result, bool silent, QWidget* parent);

    std::shared_ptr<update::Fetcher> m_fetcher;
    QString m_version;
    QString m_statePath;
    std::unique_ptr<update::Checker> m_checker;
    UpdatePresenter m_presenter;
    std::function<void(const QUrl&)> m_openUrl;
    std::function<QDateTime()> m_clock;
};

} // namespace clipitto::ui
