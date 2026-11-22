// "Check for updates": the daemon's part (the rules and the request are in src/update, the dialogs in ui/UpdateController).
// The network is touched only from checkForUpdates (a click) and from the start-up timer when update_check_auto is on.

#include "Daemon.h"
#include "PackagedApp.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace clipitto {

// The Microsoft Store build (an MSIX package) is updated by the Store, and Store policy does not allow an app to point users
// to another place for updates: no menu item, no Settings rows, no automatic check, no request.
bool Daemon::updatesDisabled()
{
    return packaging::isPackaged();
}

ui::UpdateController* Daemon::updates()
{
    if (!m_updates) {
        const QString statePath = m_options.updateStatePath.isEmpty()
                                      ? QFileInfo(m_options.databasePath).dir().filePath(QStringLiteral("update-state.json"))
                                      : m_options.updateStatePath;
        // The version is the build's own, the one --version and the About text use (set from CLIPITTO_VERSION in main).
        m_updates = std::make_unique<ui::UpdateController>(m_options.updateFetcher, QCoreApplication::applicationVersion(),
                                                           statePath);
    }
    return m_updates.get();
}

void Daemon::checkForUpdates(QWidget* parent)
{
    if (updatesDisabled()) {
        return; // the Store build: the Store updates the app
    }
    updates()->checkNow(parent);
}

ui::UpdateSupport Daemon::updateSupport()
{
    ui::UpdateSupport support;
    if (updatesDisabled()) {
        support.hidden = true;
        return support;
    }
    support.check = [this](QWidget* parent) { checkForUpdates(parent); };
    return support;
}

} // namespace clipitto
