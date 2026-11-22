#pragma once

#include <optional>

// Is this process running as a packaged (MSIX) app, i.e. the Microsoft Store build? Windows only; false everywhere else.
// The Store updates a packaged app by itself and its policy forbids an app that updates itself or sends users elsewhere to
// update, so the "Check for updates" features are off in that case (Daemon, Settings, tray, self test).
namespace clipitto::packaging {

// The pure part, for tests: what GetCurrentPackageFullName(&length, nullptr) returned for a zero-length buffer.
// ERROR_INSUFFICIENT_BUFFER (122): there is a package and the buffer was too small. ERROR_SUCCESS (0): same, with a name.
// APPMODEL_ERROR_NO_PACKAGE (15700): a plain exe. Anything else (an unknown error): not treated as packaged.
bool packagedFromResult(long result);

// The real answer (asked once). The override is for tests: a value replaces the answer, nullopt restores it.
bool isPackaged();
void setPackagedOverrideForTest(std::optional<bool> value);

} // namespace clipitto::packaging
