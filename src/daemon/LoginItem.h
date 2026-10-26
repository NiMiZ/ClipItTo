#pragma once

#include <QString>

namespace clipitto {

// Start ClipItTo when the user signs in, on macOS: a launchd agent, a property-list file in
// ~/Library/LaunchAgents that launchd reads at the next login. Plain file handling, so it
// is portable (and tested everywhere); only macOS shows it in Settings.
class LoginItem {
public:
    // `executable`: what launchd starts (the daemon's own path).
    LoginItem(QString plistPath, QString executable);

    static QString defaultPlistPath(); // ~/Library/LaunchAgents/io.github.nimiz.ClipItTo.plist
    static constexpr const char* kLabel = "io.github.nimiz.ClipItTo";

    bool isEnabled() const;
    // On: writes the agent (replacing one that points elsewhere, e.g. after the app moved).
    // Off: removes it. false with `error` when the file can't be written or removed.
    bool setEnabled(bool on, QString* error = nullptr);

    QString plist() const; // the file's content

private:
    QString m_path;
    QString m_executable;
};

} // namespace clipitto
