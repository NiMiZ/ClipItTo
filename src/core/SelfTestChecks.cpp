#include "SelfTestChecks.h"

#include "SecretScan.h"
#include "db/Migrator.h"
#include "db/Sqlite.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QStorageInfo>

namespace clipitto::selftest {

namespace {

QString onOff(bool on)
{
    return on ? QStringLiteral("on") : QStringLiteral("off");
}

QString sizeText(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("unknown");
    }
    if (bytes >= (1LL << 30)) {
        return QStringLiteral("%1 GB").arg(double(bytes) / double(1LL << 30), 0, 'f', 1);
    }
    if (bytes >= (1LL << 20)) {
        return QStringLiteral("%1 MB").arg(double(bytes) / double(1LL << 20), 0, 'f', 1);
    }
    return QStringLiteral("%1 KB").arg(bytes / 1024);
}

QString sessionName(const SessionEnv& session)
{
    switch (session.display) {
    case SessionEnv::Display::Wayland: return QStringLiteral("Wayland");
    case SessionEnv::Display::X11: return QStringLiteral("X11");
    case SessionEnv::Display::Windows: return QStringLiteral("Windows");
    case SessionEnv::Display::MacOS: return QStringLiteral("macOS");
    case SessionEnv::Display::Unknown: break;
    }
    return QStringLiteral("unknown");
}

} // namespace

// --- Probes ---------------------------------------------------------------------------------------

DbFacts probeDatabase(const QString& path, const QByteArray& key)
{
    DbFacts facts;
    const QFileInfo info(path);
    facts.fileBytes = info.exists() ? info.size() : -1;
    const QStorageInfo disk(info.absolutePath());
    facts.freeBytes = disk.isValid() ? disk.bytesAvailable() : -1;
    facts.encrypted = !key.isEmpty();
    if (!info.exists()) {
        facts.error = QStringLiteral("the history file does not exist yet");
        return facts;
    }
    try {
        db::Db database(path, key);
        facts.opened = true;
        facts.expectedSchema = int(db::builtinMigrations().size() ? db::builtinMigrations().back().version : 0);
        facts.schema = db::schemaVersion(database);
        {
            db::Stmt stmt(database, "PRAGMA quick_check(5)");
            QStringList lines;
            while (stmt.step()) {
                lines << stmt.colText(0).left(160);
            }
            facts.quickCheck = lines.isEmpty() ? QStringLiteral("no answer") : lines.join(QStringLiteral("; "));
        }
        try {
            db::Transaction probe(database); // BEGIN IMMEDIATE takes the write lock; rolled back, nothing is written
            facts.writable = true;
        } catch (const db::DbError& e) {
            facts.writeError = QString::fromUtf8(e.what());
            facts.writeBusy = (e.code() & 0xff) == 5 || (e.code() & 0xff) == 6; // SQLITE_BUSY, SQLITE_LOCKED
        }
    } catch (const db::DbError& e) {
        facts.opened = false;
        facts.error = QString::fromUtf8(e.what());
    }
    return facts;
}

ConfigFacts probeConfig(const QString& path)
{
    ConfigFacts facts;
    facts.exists = QFileInfo::exists(path);
    if (facts.exists) {
        Config::load(path, &facts.warnings);
    }
    return facts;
}

LogFacts probeLogFile(const QString& path)
{
    LogFacts facts;
    const QString probe = path + QStringLiteral(".selftest");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(probe);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        facts.error = file.errorString();
        return facts;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool wrote = file.write("ok\n") == 3;
    file.close(); // Windows: closed before it is deleted
    QFile::remove(probe);
    facts.writable = wrote;
    if (!wrote) {
        facts.error = QStringLiteral("writing failed (disk full?)");
    }
    return facts;
}

SyncFacts probeSyncFolder(const QString& folder, bool passphrasePresent)
{
    SyncFacts facts;
    facts.passphrasePresent = passphrasePresent;
    facts.folderSet = !folder.trimmed().isEmpty();
    if (facts.folderSet) {
        const QFileInfo info(folder);
        facts.folderExists = info.exists() && info.isDir();
        facts.folderWritable = facts.folderExists && info.isWritable();
    }
    return facts;
}

// --- Checks ---------------------------------------------------------------------------------------

Result daemonCheck(const Env& env)
{
    QString detail = QStringLiteral("ClipItTo %1 on %2, session: %3").arg(env.version, env.platform, sessionName(env.session));
    if (env.session.isGnome) {
        detail += QStringLiteral(", desktop: GNOME");
    }
    if (env.session.display == SessionEnv::Display::Unknown) {
        return Result::warn(detail, QStringLiteral("The session type could not be told (XDG_SESSION_TYPE is not set), so ClipItTo "
                                                   "guesses what to use. Start it from your desktop session, not from a bare terminal "
                                                   "or a service without the session's environment."));
    }
    return Result::pass(detail);
}

Result captureBackendCheck(const Env& env)
{
    const QString name = env.status.value(QStringLiteral("backend")).toString();
    if (name.isEmpty()) {
        return Result::fail(QStringLiteral("no capture backend is running"),
                            QStringLiteral("Copies cannot be recorded. Restart ClipItTo; if it stays like this, turn on the "
                                           "diagnostic log in Settings and look for \"capture backend\"."));
    }
    const QString note = env.status.value(QStringLiteral("backend_note")).toString();
    if (!env.status.value(QStringLiteral("backend_degraded")).toBool()) {
        return Result::pass(note.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, note));
    }
    QString advice;
    if (env.session.display == SessionEnv::Display::Wayland && env.session.isGnome) {
        advice = QStringLiteral("Install and enable the ClipItTo GNOME Shell extension (see docs/install.md); GNOME does not let "
                                "a background program watch the clipboard. Log out and in once after installing it.");
    } else if (env.session.display == SessionEnv::Display::Wayland) {
        advice = QStringLiteral("Install wl-clipboard (the wl-paste program), and use a desktop whose compositor offers the "
                                "data-control protocol (KDE, Sway, Hyprland, ...).");
    } else {
        advice = QStringLiteral("See docs/platforms.md for what this session needs.");
    }
    return Result::warn(QStringLiteral("%1 (degraded): %2").arg(name, note), advice);
}

Result captureCopyResult(CaptureOutcome outcome)
{
    switch (outcome) {
    case CaptureOutcome::Seen:
        return Result::pass(QStringLiteral("a test text put on the clipboard was recorded, and removed from the history again"));
    case CaptureOutcome::SeenNotRemoved:
        return Result::warn(QStringLiteral("the test copy was recorded, but it could not be removed from the history again"),
                            QStringLiteral("Delete the copy that starts with \"ClipItTo self-test copy\" yourself."));
    case CaptureOutcome::HistoryFull:
        return Result::skipped(QStringLiteral("the history is full: a test copy would push your oldest copy out. Not tested; "
                                              "raise max_history or clear some copies to run it"));
    case CaptureOutcome::NotSeen:
        return Result::fail(QStringLiteral("a test text was put on the clipboard, but ClipItTo did not record it"),
                            QStringLiteral("Capture does not see copies. See the capture backend check above; on GNOME Wayland "
                                           "enable the ClipItTo extension. Turn on the diagnostic log and run this check again."));
    case CaptureOutcome::CannotWrite:
        return Result::warn(QStringLiteral("the test text could not be put on the clipboard from here"),
                            QStringLiteral("This session lets only a window with focus write the clipboard. Run the check from "
                                           "Settings (Check this installation) instead of the command line."));
    case CaptureOutcome::Paused:
        return Result::skipped(QStringLiteral("capture is paused (the history is being converted); try again in a moment"));
    case CaptureOutcome::NoHistory:
        return Result::skipped(QStringLiteral("there is no history database to record into"));
    }
    return {};
}

Result hotkeyCheck(const Env& env)
{
    const bool native = env.session.display == SessionEnv::Display::Windows || env.session.display == SessionEnv::Display::MacOS;
    const QString key = native ? QStringLiteral("hotkey") : QStringLiteral("shortcut");
    const QString value = env.status.value(key).toString();
    Result result;
    if (value.isEmpty()) {
        result = Result::warn(QStringLiteral("the shortcut is not registered yet"),
                              QStringLiteral("Wait a moment and run the check again. If it stays like this, press OK in "
                                             "Settings to register it, or reload with: clipitto --reload"));
    } else if (value.startsWith(QLatin1String("unavailable"))) {
        result = Result::fail(value,
                              QStringLiteral("Another program probably uses this shortcut. Choose a different one in "
                                             "Settings > General."));
    } else if (value.startsWith(QLatin1String("not set automatically"))) {
        result = Result::warn(QStringLiteral("ClipItTo cannot register a shortcut on this desktop"),
                              QStringLiteral("In your desktop's keyboard settings, add a custom shortcut that runs: "
                                             "clipitto --toggle"));
    } else if (value.startsWith(QLatin1String("not set"))) {
        result = Result::fail(value, QStringLiteral("Choose the shortcut again in Settings > General; if it keeps failing, "
                                                    "bind \"clipitto --toggle\" in your desktop's keyboard settings."));
    } else {
        result = Result::pass(QStringLiteral("%1 is registered").arg(value));
    }
    if (env.config.panicKey) {
        const QString panic = env.status.value(QStringLiteral("panic_hotkey")).toString();
        const bool bad = panic.isEmpty() || panic.startsWith(QLatin1String("unavailable")) || panic.startsWith(QLatin1String("not set"));
        result.detail += QStringLiteral("; panic key: %1").arg(panic.isEmpty() ? QStringLiteral("not registered") : panic);
        if (bad && result.outcome == Outcome::Pass) {
            result.outcome = Outcome::Warn;
            result.advice = QStringLiteral("The panic shortcut is not usable. Choose another in Settings > Passwords, or bind "
                                           "\"clipitto --panic\" yourself.");
        }
    }
    return result;
}

Result pasteResult(PasteOutcome outcome, const Env& env)
{
    Q_UNUSED(env)
    switch (outcome) {
    case PasteOutcome::Received:
        return Result::pass(QStringLiteral("the paste keys were typed and the test box received the text"));
    case PasteOutcome::NotReceived:
        return Result::fail(QStringLiteral("the paste keys were sent, but the test box did not receive the text"),
                            QStringLiteral("Pasting into other programs will not work. On Windows, check that no program running "
                                           "as administrator is in front; on macOS allow ClipItTo under Privacy & Security > "
                                           "Accessibility; on Linux X11 the XTest extension is needed."));
    case PasteOutcome::CannotType:
        return Result::warn(QStringLiteral("this session cannot type the paste keys"),
                            QStringLiteral("On GNOME Wayland install and enable the ClipItTo extension. Elsewhere on Wayland "
                                           "choosing a copy puts it on the clipboard and you press Ctrl+V yourself."));
    case PasteOutcome::NoFocus:
        return Result::skipped(QStringLiteral("the Settings window could not take the keyboard focus, so nothing was typed (to "
                                              "avoid pasting into another program)"));
    case PasteOutcome::Disabled:
        return Result::skipped(QStringLiteral("paste_on_enter is off: choosing a copy does not paste it"));
    }
    return {};
}

Result pasteWithoutBoxCheck(const Env& env)
{
    if (!env.config.pasteOnEnter) {
        return pasteResult(PasteOutcome::Disabled, env);
    }
    if (env.session.display == SessionEnv::Display::MacOS) {
        const QString permission = env.status.value(QStringLiteral("accessibility")).toString();
        if (permission.startsWith(QLatin1String("granted"))) {
            return Result::pass(QStringLiteral("the Accessibility permission is granted (typing a paste is tested in Settings)"));
        }
        return Result::warn(QStringLiteral("the Accessibility permission is not granted"),
                            QStringLiteral("Allow ClipItTo in System Settings > Privacy & Security > Accessibility; without it "
                                           "choosing a copy puts it on the clipboard but cannot press Cmd+V."));
    }
    return Result::skipped(QStringLiteral("typing a paste is tested in a box in the Settings window: open Settings > Check "
                                          "this installation"));
}

Result databaseResult(const DbFacts& facts, const Env& env)
{
    if (env.historyConverting) {
        return Result::skipped(QStringLiteral("the history is being converted (encrypted or decrypted); try again in a moment"));
    }
    const QString where = env.databasePath;
    if (!facts.opened) {
        return Result::fail(QStringLiteral("%1 could not be opened: %2").arg(where, facts.error),
                            QStringLiteral("Quit ClipItTo, then restore the history from a backup (Settings > Backup) or move the "
                                           "file away so a new one is made. If the history is encrypted, the key in your keyring "
                                           "must be the one it was made with."));
    }
    QString detail = QStringLiteral("%1: %2%3, schema %4, integrity check: %5, %6, %7 free")
                         .arg(where, sizeText(facts.fileBytes), facts.encrypted ? QStringLiteral(" (encrypted)") : QString())
                         .arg(facts.schema).arg(facts.quickCheck, facts.writable ? QStringLiteral("writable") : QStringLiteral("NOT writable"),
                                                sizeText(facts.freeBytes));
    if (facts.quickCheck != QLatin1String("ok")) {
        return Result::fail(detail, QStringLiteral("The history file is damaged. Export what you can (Settings > Backup), then move "
                                                   "the file away so a new one is made, and import the backup."));
    }
    if (facts.expectedSchema > 0 && facts.schema > facts.expectedSchema) {
        return Result::fail(detail, QStringLiteral("The history was made by a newer ClipItTo than this one (%1). Update ClipItTo.")
                                        .arg(env.version));
    }
    if (!facts.writable && facts.writeBusy) {
        return Result::warn(detail, QStringLiteral("The history is busy right now (ClipItTo is writing to it). Check again in a moment."));
    }
    if (!facts.writable) {
        return Result::fail(detail + QStringLiteral(" (%1)").arg(facts.writeError),
                            QStringLiteral("The history cannot be written: another program holds it, the disk is read-only or "
                                           "the file's permissions are wrong."));
    }
    if (facts.freeBytes >= 0 && facts.freeBytes < 10LL * 1024 * 1024) {
        return Result::fail(detail, QStringLiteral("The disk is almost full; new copies will fail to be saved. Free some space."));
    }
    if (facts.freeBytes >= 0 && facts.freeBytes < 200LL * 1024 * 1024) {
        return Result::warn(detail, QStringLiteral("The disk is nearly full. Free some space."));
    }
    const QVariant items = env.status.value(QStringLiteral("items"));
    if (items.isValid()) {
        detail += QStringLiteral(", %1 copies").arg(items.toLongLong());
    }
    return Result::pass(detail);
}

Result configResult(const ConfigFacts& facts, const Env& env)
{
    if (!facts.exists) {
        return Result::pass(QStringLiteral("%1 does not exist: the built-in defaults are used (it is made when you press OK in "
                                           "Settings)").arg(env.configPath));
    }
    if (facts.warnings.isEmpty()) {
        return Result::pass(QStringLiteral("%1 reads without a problem").arg(env.configPath));
    }
    return Result::warn(QStringLiteral("%1 has %2 problem(s): %3").arg(env.configPath).arg(facts.warnings.size())
                            .arg(facts.warnings.join(QStringLiteral(" | "))),
                        QStringLiteral("The settings named above are ignored and use their defaults. Fix or delete those lines "
                                       "in the file, or press OK in Settings to write it again."));
}

Result logResult(const LogFacts& facts, const Env& env)
{
    const bool on = env.config.debugLog || env.status.value(QStringLiteral("debug_log")).toBool();
    const QString state = QStringLiteral("the diagnostic log is %1, its file is %2").arg(onOff(on), env.logPath);
    if (facts.writable) {
        return Result::pass(state + QStringLiteral(" and can be written"));
    }
    const QString detail = QStringLiteral("%1 and cannot be written: %2").arg(state, facts.error);
    const QString advice = QStringLiteral("Free some disk space or fix the permissions of that folder; without it a problem "
                                          "cannot be investigated.");
    return on ? Result::fail(detail, advice) : Result::warn(detail, advice);
}

Result keyringCheck(KeyStore* store, bool needed, bool missingIsFail, const QString& what, const QString& neededFor)
{
    if (!store) {
        return needed ? Result::fail(QStringLiteral("no keyring is built into this program, but %1 needs it").arg(neededFor),
                                     QStringLiteral("Use a ClipItTo build with keyring support."))
                      : Result::skipped(QStringLiteral("no keyring in this build; %1 is not in use").arg(neededFor));
    }
    QString why;
    if (!store->available(&why)) {
        if (needed) {
            return Result::fail(QStringLiteral("the keyring does not answer (%1), but %2 needs it").arg(why, neededFor),
                                QStringLiteral("Start or unlock your keyring (GNOME Keyring, KWallet, Keychain, Credential "
                                               "Manager) and run the check again."));
        }
        return Result::skipped(QStringLiteral("the keyring does not answer (%1); it matters only if you use %2").arg(why, neededFor));
    }
    if (!needed) {
        return Result::pass(QStringLiteral("the keyring answers (not needed now: %1 is off)").arg(neededFor));
    }
    QString error;
    std::optional<QByteArray> key = store->read(&error);
    const bool present = key.has_value();
    if (key) {
        db::wipe(*key);
    }
    const QString asks = store->mayAsk() ? QStringLiteral(" (it may ask you to unlock it)") : QString();
    if (!error.isEmpty()) {
        return Result::fail(QStringLiteral("reading the %1 failed: %2").arg(what, error),
                            QStringLiteral("Unlock your keyring and run the check again. If you cancelled its prompt, that is "
                                           "why."));
    }
    if (!present) {
        const QString detail = QStringLiteral("the keyring answers%1, but holds no %2").arg(asks, what);
        return missingIsFail ? Result::fail(detail, QStringLiteral("The key is lost: restore the data from a backup. A new key "
                                                                  "cannot open what the old one protected."))
                             : Result::warn(detail, QStringLiteral("It is made when it is first needed; nothing to do unless this "
                                                                  "stays so after you set the feature up."));
    }
    return Result::pass(QStringLiteral("the keyring answers%1 and holds the %2").arg(asks, what));
}

Result passwordChainCheck(const Env& env)
{
    if (!env.passwordsAvailable) {
        return Result::skipped(QStringLiteral("password copies are not available here"));
    }
    // Fixed samples of our own, to show the rules working; the user's text is never involved.
    struct Sample {
        const char* text;
        const char* label;
    };
    static const Sample samples[] = {{"Kd8#mP2qLx9!", "a mixed random string"},
                                     {"hello-world_2", "words joined together"},
                                     {"short1!", "a short string"}};
    QStringList judged;
    for (const Sample& sample : samples) {
        const QString why = secretscan::whyNotGeneratedPassword(QString::fromLatin1(sample.text));
        judged << QStringLiteral("%1: %2").arg(QLatin1String(sample.label),
                                               why.isEmpty() ? QStringLiteral("kept") : QStringLiteral("not kept (%1)").arg(why));
    }
    const QString detail =
        QStringLiteral("master password: %1; name box after a password copy: %2; match key: %3%4; detect_secrets %5, "
                       "detect_passwords %6; last name box: %7. detect_passwords on built-in samples: %8")
            .arg(env.passwordState.isEmpty() ? QStringLiteral("none") : env.passwordState, onOff(env.config.passwordAskNote),
                 env.canMatch ? QStringLiteral("available") : QStringLiteral("not available"),
                 env.canMatch || env.matchProblem.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(env.matchProblem),
                 onOff(env.config.detectSecrets), onOff(env.config.detectPasswords),
                 env.lastNameBox.isEmpty() ? QStringLiteral("none yet") : env.lastNameBox, judged.join(QStringLiteral("; ")));
    if (!env.passwordsSetUp) {
        return Result::warn(detail, QStringLiteral("No master password is set, so copies from password managers are not kept. Set "
                                                   "one in Settings > Passwords to keep them, encrypted."));
    }
    if (!env.config.keepPasswords) {
        return Result::warn(detail, QStringLiteral("Keeping password copies is turned off in Settings > Passwords."));
    }
    if (!env.canMatch) {
        return Result::warn(detail, QStringLiteral("Searching for a whole password while locked is off because the keyring "
                                                   "key is not available (see the keyring check)."));
    }
    if (env.config.passwordAskNote && !env.config.keepPasswords) {
        return Result::warn(detail, QStringLiteral("The name box needs password copies to be kept."));
    }
    return Result::pass(detail);
}

Result pasteTargetCheck(const Env& env)
{
    if (!env.pasteTarget) {
        return Result::skipped(QStringLiteral("this session cannot tell which program gets the paste"));
    }
    const PasteTarget target = env.pasteTarget();
    switch (target.state) {
    case PasteTarget::State::Unsupported:
        return Result::skipped(QStringLiteral("this session cannot tell which program gets the paste (Wayland without the GNOME "
                                              "extension, or macOS): the global paste keys are used"));
    case PasteTarget::State::Unknown:
        return Result::skipped(QStringLiteral("no program was remembered yet: open the popup over a program first, then run "
                                              "this check from Settings"));
    case PasteTarget::State::Known:
        break;
    }
    const pastekeys::Choice choice = pastekeys::choose(env.config.pasteKeys, env.config.pasteKeysPerApp, env.config.pasteKeysApps, target);
    const QString rule = choice.rule.isEmpty()
        ? QStringLiteral("no rule matched, the global %1 is used").arg(pastekeys::chordName(choice.keys))
        : QStringLiteral("%1 rule \"%2\" matched: %3").arg(choice.builtIn ? QStringLiteral("built-in") : QStringLiteral("your"),
                                                           choice.rule, pastekeys::chordName(choice.keys));
    // The program's own name is not put in the report (it shows what the user was doing); the rule that matched is enough.
    return Result::pass(QStringLiteral("the program in front when the popup last opened was recognized; paste_keys_per_app "
                                       "is %1; %2").arg(onOff(env.config.pasteKeysPerApp), rule));
}

Result gnomeExtensionResult(ExtensionAnswer answer)
{
    switch (answer) {
    case ExtensionAnswer::Answers:
        return Result::pass(QStringLiteral("the ClipItTo GNOME Shell extension is running and answers"));
    case ExtensionAnswer::TooOld:
        return Result::warn(QStringLiteral("the extension is running but does not know the question about the focused program "
                                           "(an older version is still loaded in GNOME Shell)"),
                            QStringLiteral("Log out and in again so GNOME loads the installed version."));
    case ExtensionAnswer::NotAnswering:
        return Result::fail(QStringLiteral("the ClipItTo GNOME Shell extension does not answer"),
                            QStringLiteral("Install it (docs/install.md) and enable it: gnome-extensions enable "
                                           "clipitto@nimiz.github.io; then log out and in. Without it GNOME Wayland cannot capture "
                                           "copies in the background or paste."));
    }
    return {};
}

Result screenLockCheck(const Env& env)
{
    if (!env.passwordsAvailable) {
        return Result::skipped(QStringLiteral("password copies are not available here"));
    }
    if (!env.config.lockOnScreenLock) {
        return Result::skipped(QStringLiteral("lock_on_screen_lock is off"));
    }
    if (env.lockWatching) {
        return Result::pass(QStringLiteral("watching for the screen locking and the PC sleeping: passwords lock then"));
    }
    return Result::warn(QStringLiteral("lock_on_screen_lock is on, but this system gives ClipItTo no lock events"),
                        QStringLiteral("Passwords lock only when their unlock time runs out. You can lock them yourself with "
                                       "clipitto --lock (bind it to a shortcut)."));
}

Result translationResult(const TranslationFacts& facts)
{
    if (!facts.builtIn) {
        return Result::skipped(QStringLiteral("this build has no translation engine"));
    }
    if (facts.packs.isEmpty()) {
        return Result::skipped(QStringLiteral("no language packs are installed (optional: Settings > Translation)"));
    }
    QStringList ok;
    QStringList bad;
    for (const auto& pack : facts.packs) {
        (pack.loadable ? ok : bad) << pack.id;
    }
    if (!bad.isEmpty()) {
        return Result::warn(QStringLiteral("packs found: %1; cannot be loaded: %2").arg(ok.join(QStringLiteral(", ")), bad.join(QStringLiteral(", "))),
                            QStringLiteral("Remove the damaged pack in Settings > Translation and add it again from its file."));
    }
    return Result::pass(QStringLiteral("packs found and loadable: %1").arg(ok.join(QStringLiteral(", "))));
}

Result ocrResult(const OcrFacts& facts, const Env& env)
{
    Q_UNUSED(env)
    if (!facts.builtIn) {
        return Result::skipped(QStringLiteral("this build cannot read text in pictures (optional)"));
    }
    const QString languages = facts.languages.isEmpty() ? QStringLiteral("automatic") : facts.languages.join(QStringLiteral(", "));
    if (!facts.available) {
        return Result::warn(QStringLiteral("text recognition is built in but not usable: %1; languages asked for: %2")
                                .arg(facts.why.isEmpty() ? QStringLiteral("no reason given") : facts.why, languages),
                            QStringLiteral("Install the language data it names (Windows: a language with OCR in Settings > Time & "
                                           "Language; Linux: the tesseract language packages)."));
    }
    return Result::pass(QStringLiteral("text recognition is ready; languages: %1").arg(languages));
}

Result syncResult(const SyncFacts& facts, const Env& env)
{
    if (!env.config.syncEnabled) {
        return Result::skipped(QStringLiteral("sync is off"));
    }
    QString line = env.status.value(QStringLiteral("sync")).toString();
    if (!env.config.syncFolder.isEmpty()) {
        line.replace(env.config.syncFolder, QStringLiteral("<sync folder>")); // the folder is the user's, not ClipItTo's
    }
    if (!facts.folderSet) {
        return Result::fail(QStringLiteral("sync is on but no folder is chosen"), QStringLiteral("Choose the shared folder in Settings > Sync."));
    }
    if (!facts.folderExists) {
        return Result::fail(QStringLiteral("the sync folder is not reachable (missing, or a drive or share that is not connected)"),
                            QStringLiteral("Connect the drive or share, or choose another folder in Settings > Sync."));
    }
    if (!facts.folderWritable) {
        return Result::fail(QStringLiteral("the sync folder exists but cannot be written"),
                            QStringLiteral("Check the folder's permissions, or choose another in Settings > Sync."));
    }
    if (!facts.passphrasePresent) {
        return Result::fail(QStringLiteral("no sync passphrase is set on this PC, so sync stays idle"),
                            QStringLiteral("Set the sync passphrase in Settings > Sync (the same on every PC)."));
    }
    const QString detail = QStringLiteral("the folder is reachable and writable, the passphrase key is present; %1").arg(line);
    if (line.contains(QLatin1String("waiting:"))) {
        return Result::warn(detail, QStringLiteral("The last sync did not complete; ClipItTo tries again by itself. Press Sync now in "
                                                   "Settings > Sync."));
    }
    return Result::pass(detail);
}

Result forgetPanicCheck(const Env& env)
{
    const Config& c = env.config;
    QString detail = QStringLiteral("auto-forget: %1").arg(c.autoForget.isEmpty() ? QStringLiteral("off")
                                                                                 : QStringLiteral("on, %1 rule(s)").arg(c.autoForget.size()));
    detail += QStringLiteral("; panic key: %1").arg(onOff(c.panicKey));
    detail += QStringLiteral("; clipitto --panic always works");
    return Result::pass(detail);
}

Result helloResult(bool available, const QString& why, const Env& env)
{
    const QString name = env.helloName.isEmpty() ? QStringLiteral("Windows Hello / Touch ID") : env.helloName;
    if (available) {
        return Result::pass(QStringLiteral("%1 is set up and usable (%2 for unlocking passwords)").arg(name, env.config.windowsHello ? QStringLiteral("on") : QStringLiteral("off")));
    }
    if (env.config.windowsHello) {
        return Result::warn(QStringLiteral("%1 is turned on for unlocking, but not usable: %2").arg(name, why),
                            QStringLiteral("Set it up in the system settings, or unlock with the master password."));
    }
    return Result::skipped(QStringLiteral("%1 is not available (%2); it is optional").arg(name, why));
}

// --- The list -------------------------------------------------------------------------------------

QList<Check> buildChecks(const Env& env)
{
    QList<Check> list;
    const auto sync = [&](const QString& id, const QString& title, std::function<Result()> fn, int timeoutMs = 10000) {
        Check c;
        c.id = id;
        c.title = title;
        c.timeoutMs = timeoutMs;
        c.start = [fn = std::move(fn)](Done done) { done(fn()); }; // quick, no waiting: on the GUI thread
        list.push_back(c);
    };
    const auto worker = [&](const QString& id, const QString& title, std::function<Result()> fn, int timeoutMs = 10000) {
        Check c;
        c.id = id;
        c.title = title;
        c.timeoutMs = timeoutMs;
        c.run = std::move(fn);
        list.push_back(c);
    };
    const auto async = [&](const QString& id, const QString& title, std::function<void(Done)> fn, int timeoutMs) {
        Check c;
        c.id = id;
        c.title = title;
        c.timeoutMs = timeoutMs;
        c.start = std::move(fn);
        list.push_back(c);
    };
    // The facts are copied into the closures: a check may outlive the call that built the list.
    const Env e = env;

    sync(QStringLiteral("daemon"), QStringLiteral("Program and session"), [e] { return daemonCheck(e); });
    sync(QStringLiteral("capture_backend"), QStringLiteral("Capture backend"), [e] { return captureBackendCheck(e); });
    if (e.captureProbe) {
        async(QStringLiteral("capture_copy"), QStringLiteral("Capture sees a copy"),
              [e](Done done) { e.captureProbe([done](CaptureOutcome o) { done(captureCopyResult(o)); }); }, 8000);
    }
    sync(QStringLiteral("hotkey"), QStringLiteral("Global shortcut"), [e] { return hotkeyCheck(e); });
    if (e.pasteProbe) {
        async(QStringLiteral("paste"), QStringLiteral("Paste works"),
              [e](Done done) { e.pasteProbe([done, e](PasteOutcome o) { done(pasteResult(o, e)); }); }, 8000);
    } else {
        sync(QStringLiteral("paste"), QStringLiteral("Paste works"), [e] { return pasteWithoutBoxCheck(e); });
    }
    if (e.database) {
        worker(QStringLiteral("history_db"), QStringLiteral("History database"), [e] { return databaseResult(e.database(), e); }, 20000);
    }
    if (e.configFile) {
        worker(QStringLiteral("config"), QStringLiteral("Settings file"), [e] { return configResult(e.configFile(), e); });
    }
    if (e.logFile) {
        worker(QStringLiteral("debug_log"), QStringLiteral("Diagnostic log"), [e] { return logResult(e.logFile(), e); });
    }
    // The keyrings: a locked one may show its own window and wait for the user, so these get a long limit.
    worker(QStringLiteral("keyring_history"), QStringLiteral("Keyring: history key"),
           [e] { return keyringCheck(e.historyKeys.get(), e.historyKeyNeeded, true, QStringLiteral("history key"), QStringLiteral("history encryption")); },
           40000);
    worker(QStringLiteral("keyring_passwords"), QStringLiteral("Keyring: password match key"),
           [e] { return keyringCheck(e.matchKeys.get(), e.matchKeyNeeded, false, QStringLiteral("password match key"), QStringLiteral("password copies")); },
           40000);
    worker(QStringLiteral("keyring_sync"), QStringLiteral("Keyring: sync key"),
           [e] { return keyringCheck(e.syncKeys.get(), e.syncKeyNeeded, true, QStringLiteral("sync key"), QStringLiteral("sync")); },
           40000);
    sync(QStringLiteral("passwords"), QStringLiteral("Password copies"), [e] { return passwordChainCheck(e); });
    sync(QStringLiteral("paste_target"), QStringLiteral("Paste keys per program"), [e] { return pasteTargetCheck(e); });
    if (e.session.display == SessionEnv::Display::Wayland && e.session.isGnome) {
        if (e.extensionProbe) {
            async(QStringLiteral("gnome_extension"), QStringLiteral("GNOME Shell extension"),
                  [e](Done done) { e.extensionProbe([done](ExtensionAnswer a) { done(gnomeExtensionResult(a)); }); }, 6000);
        } else {
            sync(QStringLiteral("gnome_extension"), QStringLiteral("GNOME Shell extension"),
                 [] { return gnomeExtensionResult(ExtensionAnswer::NotAnswering); });
        }
    } else {
        sync(QStringLiteral("gnome_extension"), QStringLiteral("GNOME Shell extension"),
             [] { return Result::skipped(QStringLiteral("only used on GNOME with Wayland")); });
    }
    sync(QStringLiteral("screen_lock"), QStringLiteral("Lock on screen lock"), [e] { return screenLockCheck(e); });
    if (e.translation) {
        worker(QStringLiteral("translation"), QStringLiteral("Translation packs"), [e] { return translationResult(e.translation()); });
    }
    if (e.ocr) {
        worker(QStringLiteral("ocr"), QStringLiteral("Text in pictures"), [e] { return ocrResult(e.ocr(), e); }, 20000);
    }
    if (e.syncFacts) {
        worker(QStringLiteral("sync"), QStringLiteral("Sync"), [e] { return syncResult(e.syncFacts(), e); }, 40000);
    } else {
        sync(QStringLiteral("sync"), QStringLiteral("Sync"), [e] { return syncResult({}, e); });
    }
    sync(QStringLiteral("forget_panic"), QStringLiteral("Auto-forget and panic key"), [e] { return forgetPanicCheck(e); });
    if (e.helloAvailable) {
        worker(QStringLiteral("hello"), QStringLiteral("Windows Hello / Touch ID"),
               [e] {
                   QString why;
                   const bool ok = e.helloAvailable(&why);
                   return helloResult(ok, why, e);
               },
               20000);
    } else {
        sync(QStringLiteral("hello"), QStringLiteral("Windows Hello / Touch ID"),
             [] { return Result::skipped(QStringLiteral("only on Windows and macOS")); });
    }
    return list;
}

} // namespace clipitto::selftest
