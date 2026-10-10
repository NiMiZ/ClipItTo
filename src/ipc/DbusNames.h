#pragma once

// Header-only so the CLI can use it without linking the daemon-side IPC library.
namespace clipitto::ipc {

inline constexpr char kServiceName[] = "io.github.nimiz.ClipItTo";
inline constexpr char kObjectPath[] = "/io/github/nimiz/ClipItTo";
inline constexpr char kInterface[] = "io.github.nimiz.ClipItTo";

// Exported by the ClipItTo GNOME Shell extension (gnome-extension/clipitto@nimiz.github.io).
inline constexpr char kShellServiceName[] = "io.github.nimiz.ClipItTo.Shell";
inline constexpr char kShellObjectPath[] = "/io/github/nimiz/ClipItTo/Shell";
inline constexpr char kShellInterface[] = "io.github.nimiz.ClipItTo.Shell";

} // namespace clipitto::ipc
