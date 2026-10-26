---
name: clipboard-backends
description: How clipboard monitoring, reading, and writing work on Windows, X11, and Wayland for this clipboard history manager. Use when working on clipboard capture, paste-back, formats (text, images, files, HTML), or sensitive-content filtering.
---

# Clipboard backends

App code talks only to `ClipboardBackend` (start/stop monitoring, `onChanged` callback, `read()`, `write()`). Each OS has its own implementation.

## Windows
- Monitor with `AddClipboardFormatListener(hwnd)` on a hidden message-only window; react to `WM_CLIPBOARDUPDATE`. Call `RemoveClipboardFormatListener` on shutdown. Don't use the old `SetClipboardViewer` chain.
- Dedupe with `GetClipboardSequenceNumber()`; one copy can fire several updates.
- `OpenClipboard` fails when another app holds it. Retry a few times with a short backoff (e.g. 5 x 20 ms) off the GUI thread; never spin.
- Formats: `CF_UNICODETEXT` for text, `"HTML Format"` (registered) for rich text, `CF_DIBV5` / `"PNG"` for images, `CF_HDROP` for file lists.
- **Respect privacy markers.** A copy is marked if the clipboard contains `ExcludeClipboardContentFromMonitorProcessing`, or `CanIncludeInClipboardHistory` / `CanUploadToCloudClipboard` set to 0. Password managers set these. See "Password copies" below.

## Linux, X11
- Monitor with XFixes: `XFixesSelectSelectionInput` on `CLIPBOARD` (and optionally `PRIMARY`, off by default since it fires on every text selection).
- Large data arrives via the `INCR` protocol; handle it or use `QClipboard`, which does.
- On X11 the clipboard content disappears when the source app exits. To keep a "pasteable" history entry, the app must take ownership and serve the data itself.

## Linux, Wayland
- Regular apps only see clipboard changes while focused, so `QClipboard::dataChanged` is not enough for a background history manager.
- Use the data-control protocol: `ext-data-control-v1` (newer) or `zwlr_data_control_manager_v1` (wlroots, KDE). Check which globals the compositor offers at startup.
- GNOME/Mutter doesn't offer data-control. Detect this and show a clear message in the UI instead of failing silently.
- **Respect privacy markers.** Offers that include the MIME type `x-kde-passwordManagerHint` (value `secret`) are marked. See "Password copies" below.

## Password copies (marked copies)
- Never store a marked copy in clear. Without a master password (or with `keep_passwords = false`) it is dropped, as before.
- Read its text only while `CaptureBackend::readMarkedText()` is true (the daemon sets it when password copies are kept); otherwise report only the markers, with empty text, so the pipeline can say why nothing was kept.
- Add the source app as `ClipFilter::sourceMimeType(app)` (`x-clipitto-source:<App>`), looked up only for marked copies (`SourceApp_*.cpp`; the GNOME extension uses the window tracker).
- The pipeline seals the text with `PasswordVault` right away; it never reaches the database, a queue or a log in clear.
- The GNOME extension asks `WantsPasswords` before reading one; marked `Ingest` calls are accepted from GNOME Shell only. Pictures with a marker are always skipped.
- Pasting one back uses `ClipboardWriter::setPassword`, which adds the markers.

## Rules for all backends
- Ignore changes the app caused itself when pasting from history (track a "self write" token/sequence), or every paste creates a duplicate entry.
- Cap entry size (e.g. text 1 MB, images 20 MB; make it configurable) and skip larger items with a log line.
- Normalize before dedupe: same text with different line endings is one entry.
- Never log clipboard contents. Log formats and sizes only.
- Reading clipboard data happens on a worker thread; only the final `ClipboardEntry` crosses to the GUI thread.

## Testing
- Unit-test history logic (dedupe, size caps, privacy filtering) against a fake `ClipboardBackend`; don't require a real clipboard.
- Manual checks per platform: copy text/image/files, copy from a password manager (skipped without a master password; with one, kept hidden with the app's name as note), paste an old entry back (must not duplicate).
