# Check this installation (roadmap 31)

User view: `docs/usage.md` "Check this installation". `clipitto --selftest`, Settings > General > *Check this installation...*.

## Code
- `core/SelfTest.{h,cpp}`: `Result` (PASS/WARN/FAIL/SKIPPED + detail + advice), `Report` (text, `toMap`), `Runner` (one check after
  another; each has `timeoutMs`; a check that has not answered becomes FAIL "did not answer in N s", a late answer is dropped).
  A `Check` has `run` (blocking, runs on a worker pool of its own, never touches the GUI) or `start(done)` (GUI thread, may answer later).
- `core/SelfTestChecks.{h,cpp}`: `Env` (what the daemon knows + probe callbacks), one function per check from facts to `Result`
  (`keyringCheck`, `databaseResult`, ...), the real probes (`probeDatabase`, `probeConfig`, `probeLogFile`, `probeSyncFolder`),
  and `buildChecks(env)` (the order of the report). Add a check: a facts struct or Env field, a `*Result` function, a line in
  `buildChecks`, tests in `tests/tst_selftest.cpp`.
- `daemon/DaemonSelfTest.cpp`: `Daemon::selfTestEnv` (fills the Env; blocking probes copy what they need, the Daemon is not touched
  on a worker thread), `createSelfTest` (one run at a time; a second caller gets the running one), `probeCapture` (live test copy),
  `probePaste` (types the paste keys into the Settings window's box), `selfTestSupport` (what Settings needs).
- Control channel: `ControlChannel::setSelfTestProvider` / `runSelfTest(reply)`; D-Bus method `SelfTest` is a delayed reply
  (`setDelayedReply`; a call inside one connection cannot be delayed, tests use a second connection), the local socket answers the line
  later. The CLI waits up to 300 s; exit 1 only when `fail` > 0.
- `ui/SelfTestWindow`: the list, *Copy report* (through `Daemon`'s writer so it is an own write and not captured), the paste test box.

## Rules
- A report never holds clipboard contents, window titles, passwords, key material, or names of copies. Paths of the config, history
  and log are allowed; the user's sync folder is replaced by `<sync folder>` in the sync status line. Keys read from a keyring are
  wiped at once and never put in a string.
- The live capture check puts `ClipItTo self-test copy <hex>` on the clipboard WITHOUT the own-write token (the capture must see it),
  finds it by hash (`findByHash`, never reads other copies) and removes it with `forgetNow` (secure delete). The sync tombstone it
  leaves (when sync is on) is local only (a hash of the random text).
- Keyring reads go through the announcing stores; `Daemon::announceKeyring` is safe to call from a worker (it hops to the GUI thread).
- No new config key. Tests must not use tight wall-clock limits; the hanging-check tests use 300 ms limits against 1.5 s sleeps.
