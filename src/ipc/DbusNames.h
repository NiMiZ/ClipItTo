#pragma once

// Header-only so the CLI can use it without linking the daemon-side IPC library.
namespace clipitto::ipc {

inline constexpr char kServiceName[] = "io.github.nimiz.ClipItTo";
inline constexpr char kObjectPath[] = "/io/github/nimiz/ClipItTo";
inline constexpr char kInterface[] = "io.github.nimiz.ClipItTo";

} // namespace clipitto::ipc
