#pragma once

#include <QString>

namespace clipitto::debuglog {

// Copies every Qt log message that is enabled (info and debug lines of the clipitto.* categories, warnings) to a file, with a
// time stamp, as well as to the usual place (stderr). For the diagnostic log (Settings, or `clipitto-daemon --verbose`): on
// Windows the daemon has no console, so without a file its log can't be seen at all.
//
// The file is emptied at start and created readable by its owner only (Linux, macOS; on Windows the per-user data folder's
// permissions apply, as for the history), and a symbolic link at the path is refused. It is capped: when it reaches `maxBytes`
// it becomes `<path>.1` (replacing an older one) and a new file starts, so the newest lines are always there and the two
// files hold at most twice `maxBytes`.
//
// What it holds is what ClipItTo logs: what happened and why, file paths (with your user name), error messages and the times.
// Messages are written never to carry what was copied; that is a rule for the code, not something this file can check.
// Returns false when the file can't be opened.
bool start(const QString& path, qint64 maxBytes = 2 * 1024 * 1024);
// Restores the message handler that was installed before start() and closes the file (the files stay on disk).
void stop();
// Deletes the log file and the rotated one.
void remove(const QString& path);

} // namespace clipitto::debuglog
