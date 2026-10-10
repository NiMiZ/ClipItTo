# ClipItTo
Clipboard manager with history, groups and favorites for Linux

## Building

Requires Ubuntu 24.04+ (or equivalent): Qt 6.4+, SQLite 3.35+, toml++ 3.

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev libgl-dev libsqlite3-dev libtomlplusplus-dev
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (address/UB sanitizers, warnings as errors).
Search benchmark: `build/debug/tests/bench_search`.

## Running

```bash
build/debug/src/daemon/clipitto-daemon &      # background daemon (later: systemd --user unit)
build/debug/src/cli/clipitto --status          # what the daemon is doing
build/debug/src/cli/clipitto --toggle          # bind this to a global shortcut
```

The CLI talks to the daemon over D-Bus (`io.github.nimiz.ClipItTo`, see
[the interface](src/ipc/io.github.nimiz.ClipItTo.xml)). `kill -HUP` or `clipitto --reload`
re-reads `~/.config/clipitto/config.toml`; history lives in `~/.local/share/clipitto/history.db`.
