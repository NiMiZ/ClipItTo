# ClipItTo in the Microsoft Store

For the maintainer. Back to the [README](../README.md) · [releasing](releasing.md).

**Why:** Microsoft signs the packages it publishes, so the Store listing shows a publisher and Windows does not say "Unknown
publisher". This is a spike: everything below is written and partly checked, but **no Store submission was made and no
package was installed on a PC** (see [What was and was not checked](#what-was-and-was-not-checked)).

The Store build is the same program as the installer build, wrapped as an **MSIX** package that runs as a *full-trust desktop
app* (the "desktop bridge"): `clipitto-daemon.exe` is the app, the CLI `clipitto.exe` comes along, and the files are the very
ones the Inno Setup installer packs (`cmake --install`, then windeployqt).

## What is in the repository

| File | What it is |
|---|---|
| `packaging/windows/msix/AppxManifest.xml.in` | The package manifest template. `scripts/make-msix.ps1` fills in the `@...@` variables. |
| `packaging/windows/msix/Assets/*.png` | Logos (committed). Made by `make-assets.py` from the same artwork as `ClipItTo.ico` (needs Pillow; run it only when the artwork changes). |
| `scripts/make-msix.ps1` | Stages the files, writes the manifest, runs `makepri` and `makeappx` (Windows SDK) and writes `dist\<ver>\ClipItTo-<ver>.msix`. |
| `.github/workflows/ci.yml` | The Windows job runs the script after the installer step, on the staged folder that job already built (no second build). |
| `src/core/PackagedApp.{h,cpp}` | `packaging::isPackaged()`: asks Windows (`GetCurrentPackageFullName`) whether the process has a package identity. |

### The package identity (placeholders until Partner Center)

Three values come from Partner Center (*Product management > Product identity*). They are read from environment variables, or
the script parameters of the same name, so no code changes when you have them:

| Variable | Partner Center field | Example |
|---|---|---|
| `MSIX_IDENTITY_NAME` | Package/Identity/Name | `12345NiMiZ.ClipItTo` |
| `MSIX_PUBLISHER` | Package/Identity/Publisher | `CN=01234567-89AB-CDEF-0123-456789ABCDEF` |
| `MSIX_PUBLISHER_DISPLAY_NAME` | Package/Properties/PublisherDisplayName | `NiMiZ` |

Without them the script uses `PLACEHOLDER-FromPartnerCenter.ClipItTo` / `CN=PLACEHOLDER-FromPartnerCenter` and prints a warning;
`-RequireStoreIdentity` makes that an error (use it for the package you upload). In CI the values come from **repository
variables** with those names (*Settings > Secrets and variables > Actions > Variables*; they are not secret, so no secret is needed).
`MSIX_STARTUP_ENABLED` (`true` by default, or `false`) sets whether the start-at-sign-in task starts switched on.

### Building

```
scripts\build-windows.ps1 -Stage                       # build, tests, stage (build\windows-stage), as for the installer
scripts\make-msix.ps1 [-StageDir <dir>] [-OutputDir <dir>] [-RequireStoreIdentity]
```

Output: `dist\<version>\ClipItTo-<version>.msix`, **unsigned** (that is what Partner Center wants; Microsoft signs it after
certification). The version is `X.Y.Z.0` from `CMakeLists.txt`: the Store reserves the last part and it must be `0`, and every
upload must be higher than the last one, which the normal release numbering gives.
Needs the Windows SDK (`makeappx.exe`, `makepri.exe`, `signtool.exe`: `winget install Microsoft.WindowsSDK.10.0.22621`); the CI
runner has it. In CI the file is the artifact `clipitto-windows-msix` (kept 3 days), uploaded on tag builds and on a manual run
(*Actions > CI > Run workflow*); it is **never** added to the GitHub Release and no step publishes it anywhere.

### Trying the package on your own PC (sideloading)

A Store-bound package is unsigned and cannot be installed. For a local test, `-TestSign` makes a second package signed with a
self-signed certificate:

```
scripts\make-msix.ps1 -TestSign
```

It creates the certificate `CN=ClipItTo Test` in `Cert:\CurrentUser\My` (valid 12 months, code-signing use only), exports
`ClipItTo-Test.cer` and writes `ClipItTo-<ver>-test.msix`. The test package has its own identity (`ClipItToTest`, publisher `CN=ClipItTo Test`),
because the publisher in the manifest must equal the certificate's subject; it can therefore not collide with the Store
package. Then, in an **elevated** PowerShell:

```
Import-Certificate -FilePath dist\<ver>\ClipItTo-Test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople   # trust it, once
Add-AppxPackage dist\<ver>\ClipItTo-<ver>-test.msix
```

Sideloading needs *Settings > Apps > Advanced app settings > Choose where to get apps* not set to "Store only", or Developer
Mode (Windows 10 2004 and later normally allow it). Quit the installed ClipItTo first: two daemons fight over the hotkey.
**Afterwards remove everything:** `scripts\make-msix.ps1 -CleanupTest` (elevated) removes the installed test package and the
certificate from `Cert:\LocalMachine\TrustedPeople` and `Cert:\CurrentUser\My`; delete the `.cer` and `.msix` files yourself.
Uninstalling the package also deletes its private data folder (see below).

## How the Store build differs in behavior

- **No update check.** `packaging::isPackaged()` is true: the tray item *Check for updates...*, the Settings button and the
  *Look for a new version...* box are gone, the automatic check never runs, the code never contacts cohenzco.com, and
  *Check this installation* says the Store updates the app. The Store's policy and its own updating make a second mechanism
  wrong. Because of this the manifest has **no `internetClient` capability**: the packaged app makes no internet connection at
  all. (`update_check_auto` stays in the config file, ignored.)
- **Start at sign-in** is the manifest's `startupTask` (task id `ClipItToStartup`), the replacement for the installer's Startup-folder
  shortcut. It is on by default (`MSIX_STARTUP_ENABLED`). The user turns it off in *Settings > Apps > Startup* or Task Manager.
  ClipItTo itself writes neither a shortcut nor a `Run` registry value (on Windows only the installer ever created the startup
  entry), so there was nothing in the code to redirect. There is no checkbox for it inside ClipItTo on Windows (installer or
  Store); switching it from inside the app would need the WinRT `StartupTask` API (not done).
- **The `clipitto` command** is an app execution alias (`clipitto.exe`) for `bin\clipitto.exe`, so `clipitto --status` works in a new terminal
  once the package is installed. It runs with the package's identity (same private data as the daemon). The installer build does not
  put `clipitto` on the PATH, so there is no clash between the two. Aliases are per user and appear in
  *Settings > Apps > Advanced app settings > App execution aliases*.
- **Capabilities:** `runFullTrust` (restricted; see the certification notes) and `privateNetworkClientServer` (Receive from phone
  listens for connections from the local network). For a full-trust process these capabilities are declarations (the process is not
  in the app sandbox, and the Windows Firewall prompt for the listener behaves as for any program); they feed the Store listing's
  "This app can..." text.
- **Files and data paths.** The installation folder (`C:\Program Files\WindowsApps\...`) is read-only: ClipItTo never wrote next to its
  exe (only reads: it finds `clipitto.exe` there). Settings, history, debug log and translation packs are written through
  `%APPDATA%\clipitto` and `%LOCALAPPDATA%\clipitto`, as always. **MSIX redirects such writes of a packaged app** to a private
  folder, `%LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalCache\Roaming\clipitto` and `...\LocalCache\Local\clipitto` (and HKCU registry
  writes likewise). The decision here is to **accept this** and not ask Microsoft for the restricted `unvirtualizedResources`
  capability. Consequences:
  - The Store build and the installer build have **separate histories and settings** once the Store build writes something.
    Windows is expected to let the packaged app *read* an existing `%LOCALAPPDATA%\clipitto\history.db` or `%APPDATA%\clipitto\config.toml`
    of the installer build and to copy it into the private folder on the first change (copy on write). That mix is confusing and
    **not verified**: tell testers to use *one* of the two builds, or to export a backup (Settings) and import it. Do not run both at once.
  - **Uninstalling the Store build deletes its history, settings and language packs** (the installer build keeps them). Say so in
    the listing and keep the backup export in mind.
  - The README's "delete those folders to erase everything" does not apply to the Store build: uninstall it.
  - Files in places the user picks (backup export, the Sync folder, pictures saved from the popup) are not redirected.
  - `clipitto --status` and the log print the usual `%APPDATA%\...` path, which is the logical path; the bytes are in the private folder.
  - Windows Credential Manager (the history-encryption key) and Windows Hello are APIs, not files; expected to work unchanged. Not verified.
- **Not in the Store build:** no installer wizard, no "start now" page, no portable folder; the GNOME extension and the Linux/macOS bits
  are irrelevant. Updates arrive through the Store, which closes a running ClipItTo to replace it.

## What you must do in Partner Center

1. **Register** at <https://partner.microsoft.com/dashboard/registration> as an **individual** developer (Microsoft account, a small one-time
   registration fee (about US$19; check the current amount), identity verification with an ID document and a selfie/photo of
   yourself, a verified email and phone). Verification can take days. Use the publisher name you want shown (it can be your
   name or a brand; it is the *PublisherDisplayName*).
2. **Reserve the app name:** *Apps and games > New product > MSIX or PWA app*, name `ClipItTo`. A name is held for three months
   unless you submit. If it is taken, pick another and put it in the listing; the in-app name stays ClipItTo.
3. **Read the identity:** *Product management > Product identity*. Copy the three values into the table above
   (`MSIX_IDENTITY_NAME`, `MSIX_PUBLISHER`, `MSIX_PUBLISHER_DISPLAY_NAME`). In GitHub: *Settings > Secrets and variables > Actions >
   Variables*, add the three names. Nothing else changes; build again (tag, or *Run workflow*) and download `clipitto-windows-msix`.
   Check that the manifest in the downloaded package carries those values (rename to `.zip` and look at `AppxManifest.xml`).
4. **Start a submission** and fill in:
   - **Pricing and availability:** free; markets; visibility public (or "hidden, link only" for the first tests).
   - **Properties:** category *Productivity* (or *Utilities & tools*); privacy policy URL (below); website
     (`https://github.com/NiMiZ/ClipItTo` or cohenzco.com); support contact; "Product declarations" (nothing needs checking for ClipItTo
     beyond what is true: it does not use accessibility features); system requirements (Windows 10 1809 or later, 64-bit).
     The restricted capability `runFullTrust` asks for a justification here or in the certification notes.
   - **Age ratings:** the IARC questionnaire. Answers for ClipItTo: no violence, no sex, no gambling, no user-generated content shared
     with others, no online interaction, no location sharing, no purchases. Expect "3+ / Everyone".
   - **Packages:** upload `ClipItTo-<ver>.msix`. Partner Center shows the identity it read; it must match step 3.
   - **Store listing (English first):** description (what it does: clipboard history, groups, favorites, search, password-copy
     handling, local only, no accounts, no telemetry), *What's new* (copy `docs/releases/vX.Y.Z.md`), at least **1 screenshot** (more is
     better; 4 or more; PNG, 1366 x 768 or larger, of the popup, Settings, groups, Receive from phone), a Store logo 1:1 at 300 x 300
     (resize `Assets\Square310x310Logo.png`), search terms, copyright line (`Copyright (C) 2026 NiMiZ. Licensed under the GPL-3.0.`).
   - **Notes for certification:** paste the text below.
5. **Submit.** Certification usually takes up to a few business days. A rejection arrives as a report; fix and re-upload (the version
   must go up, so each re-upload is a new `X.Y.Z.0` or you submit a new version with the same files and a higher number: the
   Store has no "same version again").

**Privacy policy URL:** the README's Privacy section, <https://github.com/NiMiZ/ClipItTo#privacy>. **It must be reachable without a login.** While the
repository is private ([releasing](releasing.md) notes it is) that URL answers 404 and certification fails: make the repository
public first, or host the policy text on cohenzco.com and give that URL. The text itself is accurate for the Store build too, with one
difference: the update-check sentences do not apply to it (the Store build never connects to the internet); consider a sentence
"The Microsoft Store version has no update check" in the README.

### Notes for certification (text to paste)

> ClipItTo is a clipboard history manager (open source, GPL-3.0). It needs the restricted capability **runFullTrust** because it is
> a Win32/Qt desktop program that must (1) watch the system clipboard of all programs, (2) register a global hotkey with
> RegisterHotKey to open its popup, (3) paste a chosen item into the previously active window, with SendInput, and (4) run from a
> tray icon. None of this is possible inside the app container. It installs no keyboard hook and records no keystrokes.
> **privateNetworkClientServer:** the optional "Receive from phone" feature (Settings > General) starts a small web server only after
> the user opens that window, only accepts connections from the local network (loopback and private addresses; others are dropped), uses a random
> token, and stops after 5 minutes (30 at most). It is never on at program start. Without using it the app opens no network port.
> The packaged app has no internet access and no telemetry: there is no update check in this build. Clipboard contents are stored
> only in local files of the user's account and are never logged or sent anywhere. Copies that password managers mark as secret
> are skipped, or stored encrypted when the user sets a master password. **Donations:** Settings > "Support ClipItTo..." opens the
> developer's PayPal.me page in the user's web browser. It is an optional voluntary donation to the open-source developer. Nothing in
> the app is sold, locked or unlocked by it, no payment is made inside the app, and the app handles no payment data. To test: start the app (tray icon), copy some text, press
> Ctrl+Alt+V, pick the item. Source code: https://github.com/NiMiZ/ClipItTo

## Store policy risks found

Read the current [Microsoft Store Policies](https://learn.microsoft.com/windows/apps/publish/store-policies) before submitting; I could not
fetch them from here and list the risks from what ClipItTo does, not from a policy review.

1. **Clipboard access of every program.** A clipboard manager is allowed (many are in the Store), but it is "personal information" handling: the privacy
   policy must exist, be reachable and match the behavior (it does; see the URL problem above). The listing must say plainly that it
   reads and stores what you copy.
2. **Global hotkey and pasting into other windows (SendInput).** Legitimate, but looks like keylogger behavior to a reviewer. The notes
   state that there is no keyboard hook (true: only `RegisterHotKey` and `SendInput` are used in `src/daemon/WindowsHotkey.cpp` and `src/output/SyntheticPaste.cpp`).
3. **A web server on the network.** Opened only by the user and short-lived, but it is an inbound listener: described in the notes; the
   Firewall prompt may confuse reviewers. Hard cap and local-only checks are in `.claude/notes/phone.md`'s threat model. Plain HTTP (no
   TLS) is a documented limit; a reviewer may ask. Be ready to say "local network only, token, 5 minutes".
4. **Password handling wording.** The listing must not promise more than the code does ("secure", "military-grade", "protects all
   passwords"): password detection depends on the password manager marking the copy, and a password copied from a field without a marker is stored
   like any text (see `docs/passwords.md`). Use the README's wording.
5. **Encryption.** ClipItTo uses encryption (SQLite3MC for the optional encrypted history, Monocypher for sealed password copies, a
   master-password key derivation). Partner Center and US export rules (EAR) have encryption questions for some products; ClipItTo's
   cryptography is for protecting the user's own data, with published source, which normally qualifies for the open-source / mass-market
   treatment, but I cannot verify the current form: answer it truthfully when it appears.
6. **"No telemetry" claim.** True for ClipItTo (no analytics at all, and no internet capability in the Store build). The Store itself
   collects install and crash data for the publisher in Partner Center; do not claim "no data is collected about this app anywhere".
7. **Donation button.** *Settings > Support ClipItTo...* opens a PayPal page in the browser. Store policy limits how apps ask for money
   (digital goods and in-app payments must use Microsoft's commerce; links that send users elsewhere to pay are a common rejection reason).
   A donation to the developer is a gray zone. **Kept (owner decision, 2026-11-25), and declared in the certification notes above**; if certification objects, hide the button when
   `packaging::isPackaged()` is true (the same one-line mechanism as the update rows).
8. **GPL-3.0 and the Store's terms.** The Store's distribution terms add conditions (usage rules, the Standard Application License
   Terms) that GPL-3.0 §10 ("no further restrictions") can collide with for third-party copyright holders; as the sole
   copyright holder of ClipItTo you can distribute your own code under the Store terms, but **code from contributors or
   libraries** (Qt under LGPL, CTranslate2, SQLite3MC, ...) keeps its own licenses: check `THIRD-PARTY-NOTICES.md` and that the Store build still lets users replace the
   LGPL libraries (the installer build stays available for that; the Store package is signed so users cannot swap files inside it). Get this checked before the first submission.
9. **Restricted capability.** `runFullTrust` is allowed for desktop-bridge apps but the justification is looked at; a refusal there stops everything.
10. **Behavior the reviewer will notice:** the app starts at sign-in by default (opt-out) and sits in the tray; set `MSIX_STARTUP_ENABLED=false` if you
    want the stricter default.
11. **Translation packs** (Argos models, CC BY 4.0) are not shipped; the user adds them from files. Nothing for the Store to check, but the listing should not
    claim translation works without them.

## Known limits of the Store build against the installer build

- No update check (the Store updates it); no `internetClient`.
- History and settings are private to the package and deleted on uninstall; separate from the installer build's data.
- Autostart is changed in Windows settings, not in ClipItTo; there is no checkbox.
- The CLI is reached through the `clipitto` alias only; running `bin\clipitto.exe` straight from the `WindowsApps` folder (not
  allowed for normal users anyway) would have no package identity and see other data folders.
- The package's files cannot be replaced or patched in place; users who want to swap a library should use the installer or build from source.
- Windows 10 1809 or later, x64 only (like the installer).

## What was and was not checked

Checked (on the Windows PC of the development machine, from WSL, with a stand-in staged folder of two dummy `.exe` files, not a real build):
- `scripts/make-msix.ps1` runs; the manifest passes MakeAppx's schema validation (it found and I fixed two errors: `--` inside an XML comment and the
  `ExecutionAlias` element's namespace); `makepri` writes `resources.pri` from the Assets; `makeappx pack` makes the package; `-TestSign`
  creates the certificate, signs the package with `signtool` ("Successfully signed"); `-CleanupTest` removes the certificate. The script's PowerShell parses.
- On Linux: the build and the tests (`tst_update`: the packaged-app detection through its pure function and the test override, and *Check this installation*
  text; `tst_gui_general`: the hidden Settings rows).

**Not checked** (needs a real Windows build and a person):
- A real `build\windows-stage` packed and **installed** (`Add-AppxPackage` needs the certificate trusted in an elevated shell, which was not done): launch from Start, tray icon, hotkey,
  paste, translation, Windows Hello and Credential Manager, the encryption.
- `GetCurrentPackageFullName` linking and returning "packaged" in the real exe (the Windows part of `PackagedApp.cpp` is compiled only by CI;
  Linux builds the `false` branch). CI on this branch is the first compile.
- The data virtualization behavior described above (private folder, read-through of installer data, deletion on uninstall).
- The startup task actually starting the daemon at sign-in (and `Enabled="true"` behavior; Windows may need the app to have run once);
  the `clipitto` alias working in a terminal with its console output.
- The Windows Firewall prompt for Receive from phone, and a phone reaching the listener from the packaged app.
- Whether Partner Center accepts the package, the capability justification, and certification passing (policy risks above).
- The CI step itself (written, not run); the `windows-2022` runner's SDK has `makeappx`, `makepri` and `signtool` (normally installed under `Windows Kits\10\bin`).
