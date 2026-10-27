# Review: setting the GNOME shortcut at startup

**Scope:** the commits on `feat/start-focus` that are not on `main`:

- `644f5b2` ClipItTo 0.4.0: sync configured GNOME shortcut on startup
- `3b1fe88` ClipItTo 0.4.1: bump patch version
- `60c4ef6` Snap: stage the toml++ runtime library

Only `644f5b2` changes the app's code. It adds one line, a call to
`applyDesktopShortcut()` at the end of `Daemon::start()`
(`src/daemon/Daemon.cpp:214`). The other changes are tests, `snap/`, `install.sh` and
version numbers.

**Checked against:** the project's cross-platform C++ rules: OS-specific code stays
behind a check in one place, a feature never silently does nothing, and slow work
never runs on the GUI thread.

## Findings

### 1. The popup can freeze at startup while `gsettings` runs

**Where:** `applyDesktopShortcut()` (`src/daemon/Daemon.cpp:549`) calls
`applyGnomeShortcut()` (`src/daemon/GnomeShortcut.cpp`). That function runs the
`gsettings` program four or five times, one after another, on the GUI thread. Each run
waits up to 3 seconds (`waitForFinished(3000)`, line 19).

**Problem:**

- Before `644f5b2`, these calls ran only when the hotkey changed in Settings. Now they
  also run on every start. On GNOME that is usually at login, while the desktop is still
  loading.
- If `gsettings` or dconf is slow or hangs, the popup and tray icon stop responding for
  up to about 15 seconds.
- All three shortcut values (name, command, binding) are rewritten on every start,
  even when nothing has changed.

**Proposed fix:**

1. Run `applyGnomeShortcut()` on a worker thread with
   `QtConcurrent::run` + `QFutureWatcher`, the way picture encoding already does
   (`ImageCodec`). Set the `shortcut:` status line when it finishes. The function uses
   only `QProcess` and strings, so it is safe off the GUI thread.
2. In `applyGnomeShortcut()`, read the current `binding` and `command` first, and write
   only the values that differ. A normal start then runs `gsettings` read-only.
3. Make sure a start followed quickly by a reload can't apply an older hotkey after a
   newer one. One way: keep a counter and ignore results from earlier requests.

**Test:** `tst_daemon` already covers this with the fake `gsettings`. Wait for the result
with `QTRY_COMPARE` on the stored binding, and add a case where nothing is written when
the shortcut already matches. The fake program can log the commands it was given.

### 2. On Linux desktops other than GNOME, the hotkey status is missing

**Where:** `applyDesktopShortcut()` returns early when the session is not GNOME
(`src/daemon/Daemon.cpp:553`) and does not set any status. This was already the case
before these commits.

**Problem:** on KDE, Sway or other desktops, nothing sets up the hotkey, and
`clipitto --status` says nothing about it. The rules require such a case to be reported
clearly instead of silently doing nothing.

**Proposed fix:** on Linux sessions that are not GNOME, set the status line to something
like `shortcut: not set automatically; bind "clipitto --toggle" in your desktop's
keyboard settings`. Windows keeps its own `hotkey:` status from `main.cpp`. A sentence
in the README would also help.

**Test:** `tst_daemon`: with `isGnome = false` on Linux, `--status` contains the
`shortcut:` line.

## Suggested solutions

### S1. Set the shortcut on one worker thread, writing only what changed (finding 1)

**a) `GnomeShortcut.cpp`: read before writing.** `gsettings get` prints strings in
single quotes (`'ClipItTo'`). The fake program in the tests prints them without quotes, so
strip the quotes before comparing:

```cpp
// Writes `value` only when the key holds something else, so a normal start is read-only.
bool setIfChanged(const QString& gsettings, const QString& entry, const QString& key, const QString& value,
                  QString* error)
{
    QString current;
    if (run(gsettings, {QStringLiteral("get"), entry, key}, &current, nullptr)) {
        if (current.size() >= 2 && current.startsWith(QLatin1Char('\'')) && current.endsWith(QLatin1Char('\''))) {
            current = current.mid(1, current.size() - 2);
        }
        if (current == value) {
            return true;
        }
    }
    return run(gsettings, {QStringLiteral("set"), entry, key, value}, nullptr, error);
}
```

The three `run(gsettings, {"set", entry, ...})` calls at the end of
`applyGnomeShortcut()` then become `setIfChanged(...)`. When everything already matches, a
start runs four `get`s and no `set`, and dconf is never written.

**b) `Daemon`: run it on a one-thread pool.** A pool limited to one thread runs the jobs
in the order they were queued. A reload that comes in during a start therefore always
writes last. The counter is still needed, so that an older job's result doesn't overwrite
the status line.

`Daemon.h`:

```cpp
#include <QThreadPool>
...
signals:
    void shortcutApplied(); // after each GNOME shortcut update, whether or not it worked
...
private:
    QThreadPool m_shortcutPool; // one thread: shortcut updates run in order, off the GUI thread
    quint64 m_shortcutRequest = 0; // results of older requests don't overwrite the status
```

In the constructor: `m_shortcutPool.setMaxThreadCount(1);`

`Daemon.cpp` (includes `<QFutureWatcher>` and `<QtConcurrent>`, as in `ImageCodec.cpp`):

```cpp
void Daemon::applyDesktopShortcut()
{
    // Windows registers the hotkey in-process (see main.cpp, configReloaded) and reports it
    // as "hotkey"; GNOME keeps it as a custom keyboard shortcut; elsewhere the user binds
    // `clipitto --toggle` themselves.
    if (m_options.session.display == SessionEnv::Display::Windows) {
        return;
    }
    if (!m_options.session.isGnome) {
        setStatusValue(QStringLiteral("shortcut"),
                       QStringLiteral("not set automatically: bind \"%1\" in your desktop's keyboard settings")
                           .arg(QStringLiteral("clipitto --toggle")));
        return;
    }
    const auto hotkey = parseHotkey(m_config.hotkey);
    if (!hotkey) {
        setStatusValue(QStringLiteral("shortcut"), QStringLiteral("not set: unknown hotkey ") + m_config.hotkey);
        return;
    }
    const quint64 request = ++m_shortcutRequest;
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, request, shown = m_config.hotkey] {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (request != m_shortcutRequest) {
            return; // a newer hotkey is queued behind this one
        }
        if (!error.isEmpty()) {
            qCWarning(lcDaemon).noquote() << "could not set the GNOME shortcut:" << error;
            setStatusValue(QStringLiteral("shortcut"), QStringLiteral("not set: ") + error);
        } else {
            setStatusValue(QStringLiteral("shortcut"), shown + QStringLiteral(" (GNOME)"));
        }
        emit shortcutApplied();
    });
    // Copies only: the job never touches the Daemon, so it is safe on the pool thread.
    watcher->setFuture(QtConcurrent::run(&m_shortcutPool, [accelerator = hotkey->gtkAccelerator,
                                                           command = cliCommand(),
                                                           program = m_options.gsettingsProgram] {
        QString error;
        if (applyGnomeShortcut(accelerator, command, &error, program)) {
            return QString();
        }
        return error.isEmpty() ? QStringLiteral("gsettings failed") : error;
    }));
}
```

**Notes:**

- When `~QThreadPool` runs, it waits for a running job, so quitting during an update is
  safe. In the worst case (a hung `gsettings`) quitting waits up to 3 s per remaining
  call. Accept that, or check a cancel flag between calls.
- `applySettings()` still returns `true` right away. A shortcut that fails shows up in
  `--status`, the same as it does now.
- `QtConcurrent` is already linked (`cmake/Dependencies.cmake`). No build change is
  needed.

**c) Tests (`tests/tst_daemon.cpp`, `tests/fakebin/gsettings`).**

- `desktopShortcutFollowsTheHotkey`: after `start()`, `applySettings()` and the reload,
  use `QTRY_COMPARE(stored(...), ...)` instead of `QCOMPARE`. Use `QTRY_COMPARE` for the
  status line too.
- Make the fake log each call by adding one line to `tests/fakebin/gsettings`:

  ```sh
  [ -n "${FAKE_GSETTINGS_LOG:-}" ] && printf '%s %s\n' "$cmd" "$*" >> "$FAKE_GSETTINGS_LOG"
  ```

- New test `startupDoesNotRewriteAnUnchangedShortcut`: start once and wait for
  `shortcutApplied` (`QSignalSpy::wait`). Clear the log, then start a second `Daemon` on
  the same store and wait again. The log must contain no `set` line.
- New test `aReloadDuringStartupWins`: start with `Ctrl+Alt+V`, then immediately reload
  with `Ctrl+\``. After both `shortcutApplied` signals, the binding is `<Control>grave`
  and the status is `Ctrl+\` (GNOME)`.
- Optional: make the fake sleep (`FAKE_GSETTINGS_DELAY=1`) and check that
  `Daemon::start()` returns in well under a second.

**Smaller alternative:** do only step (a) and keep the calls on the GUI thread. A normal
start then costs four quick `gsettings get` runs (tens of milliseconds) instead of five
writes, but a hung `gsettings` still freezes the popup. Only the worker thread fixes the
freeze, so (a) + (b) is the recommended fix.

### S2. Report the hotkey on Linux desktops other than GNOME (finding 2)

This is part of the `applyDesktopShortcut()` code above: the separate
`!isGnome` branch sets the `shortcut:` status line. Windows returns before that branch,
so its `hotkey:` status from `main.cpp` stays the only one there.

- **Test:** rename `nonGnomeSessionsLeaveTheShortcutAlone` to
  `nonGnomeSessionsReportTheManualShortcut`. Keep the bogus `gsettingsProgram`, so the test
  also shows that `gsettings` never runs. Replace the last line with:

  ```cpp
  QVERIFY(d.status().value(QStringLiteral("shortcut")).toString().contains(QLatin1String("clipitto --toggle")));
  ```

- **README:** in the shortcut section, add one sentence: on KDE, Sway and other desktops,
  bind `clipitto --toggle` yourself; `clipitto --status` reminds you.

### S3. Housekeeping

```sh
# .gitignore: snapcraft's build folders
parts/
stage/
prime/
*.snap
```

To move the unmerged work onto a fresh branch (WSL git, no force-push needed):

```sh
git switch -c release/0.4.1 origin/main
git cherry-pick 644f5b2 3b1fe88 60c4ef6
# add the S1/S2 fixes and the .gitignore change, commit, push, open a PR into main
```

The `v0.4.0` tag still goes on the merge commit `e7bbc6d`, not on the tip of `main` after
0.4.1 lands.

## What holds

- **Windows:** unaffected. The early return keeps Windows on its own hotkey
  registration in `main.cpp`, so no Windows change is needed for either fix.
- **Platform checks:** the GNOME-or-not check stays in one function in the daemon, not
  spread through the app's code.
- **New tests:** the test added in `644f5b2` sets an old binding first and checks that
  startup replaces it.

## Housekeeping (not code)

- `feat/start-focus` was already merged as PR #13. Move these commits, and the fixes
  above, onto a new branch from `main` (for example `release/0.4.1`) and open a PR from
  there.
- `.gitignore` doesn't list snapcraft's build folders `parts/`, `stage/`, `prime/` or
  `*.snap`. `parts/` alone is 2 GB.
- The `v0.4.0` tag hasn't been pushed yet, so the 0.4.0 release isn't published.
