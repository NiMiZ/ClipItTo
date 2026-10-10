# ClipItTo
Clipboard manager with history, groups and favorites for Linux

## Building

Requires Ubuntu 24.04+ (or equivalent): Qt 6.4+, SQLite 3.35+, toml++ 3.

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev libsqlite3-dev libtomlplusplus-dev
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (address/UB sanitizers, warnings as errors).
Search benchmark: `build/debug/tests/bench_search`.
