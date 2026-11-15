# Receive from phone (roadmap 28)

User view: `docs/usage.md` "Send a picture or text from your phone". Settings > General > *Receive from phone...*, `clipitto --phone`
(control method `ReceiveFromPhone` on D-Bus and the local socket, signal `ControlChannel::phoneRequested`).

## Code
- `phone/PhoneNet.{h,cpp}`: pure functions. `isLocalAddress` (loopback, RFC1918, 169.254/16, ::1, fc00::/7, fe80::/10; IPv4-mapped
  IPv6 judged as IPv4), `newToken` (128 bits from `QRandomGenerator::system()`, 32 hex), `constantTimeEqual`, `buildUrl`,
  `lanCandidates` / `looksVirtual` (the address to show, from a plain `InterfaceInfo` table so it is testable).
- `phone/PhonePage.cpp`: the one self-contained HTML page (no external address; `tst_phone` asserts it). Converts to JPEG <= 2400 px on
  the phone (HEIC from iPhone galleries), falls back to the original file, sends pictures one after another, polls `status?id=`.
- `phone/PhoneServer.{h,cpp}`: hand-written HTTP/1.1 on `QTcpServer`. One request per connection (`Connection: close`). Routes only
  `GET /<token>/`, `POST /<token>/upload`, `POST /<token>/text`, `GET /<token>/status[?id=N]`; all else a bare 404. Limits in
  `PhoneServer::Config` (4 connections, 10 s idle, 120 s per connection, 8 KB headers -> 431, body 20 MB / text 100 KB -> 413,
  30 uploads -> 429, 6 not yet answered -> 503, 5 minutes + Extend, 30 minutes at most). It knows nothing about pictures: it emits
  `received(id, kind, body)` and shows what `setItemState` says.
- `capture/ImageCodec`: `decodeUntrusted` (format from content, whitelist JPEG/PNG/WebP/GIF/BMP, size from the header before any pixel,
  > `kMaxImagePixels` (80 MP) refused, Qt allocation limit, EXIF orientation).
- `daemon/DaemonPhone.cpp`: `openPhone`, `stopPhone`, the receive pipeline. Picture: decode + encode on `m_phonePool` (one thread),
  `CapturePipeline::processImage` (size caps, dedupe, pruning) -> read with the same engine as "Paste text from picture"
  (`m_helloPool`, `FakeOcr` in tests) -> `setPictureText` (so the search index does not read it again; `m_suppressIndex` covers the
  `stored` signal) -> text through `CapturePipeline::process` (secret/password detection and filters apply). Text typed on the phone
  goes through `process` too. Nothing touches the clipboard, so the capture never sees these a second time; *Copy text* and *Paste*
  use `ClipboardWriter::setText` (an own write). No OCR: the picture is kept and the reason is `ocrUnavailableReason()` (same text the popup shows).
- `ui/PhoneWindow`: QR (`qrImage`), address, combo of other addresses, countdown, Stop / Extend, one row per item.
  Owned by the daemon; Settings passes itself as parent (its modal dialog blocks input to other windows), so closing Settings closes it.

## Rules
- Off until the window is opened; never started at program start. Stops on Stop, expiry, window close, `--quit`/`aboutToQuit`, panic (panic also
  closes the window and drops items still being read via `m_panicEpoch`).
- Logs: counts and sizes at debug level only. Never the token, address, port, text or picture. The address is shown in the window only
  (the CLI prints only that the window is open). The token is wiped when the server stops.
- A received picture is data: decoded from memory, never written to a file, never executed.
- `tst_phone` (pure functions + the HTTP layer with a real client on 127.0.0.1), `tst_phone_daemon` (uploads, window, panic, quit, Settings),
  `tst_dbus` / `tst_localipc` (control channel and CLI). Tests listen on the loopback only (`phoneConfig.listenOn`) and fake the
  interfaces (`phoneInterfaces`); waits are generous (`StallWatchdog::scaled`).
- Packaging: QtNetwork is always linked now (deb depends on `libqt6network6t64`, snap `network-bind` + `libqt6network6`).

## Threat model
Assets: the history, the user's attention (a flood), the machine (memory, CPU), the user's privacy (what was sent, where it came from).
Attacker: anyone on the same LAN/Wi-Fi who is not the user; a program on the PC (out of scope: it can read the DB anyway); the internet (must have no way in).

| Threat | Mitigation | Left |
|---|---|---|
| Someone off the LAN reaches the server | Listens on IPv4 only; each peer address must pass `isLocalAddress`, else the socket is aborted unanswered; no relay, no cloud, no UPnP/port mapping. | A router that forwards a port to the PC (user's act) or a public-IP-on-LAN network; the check is on the peer address only. |
| A LAN neighbour guesses the address | 128-bit token in the path, constant-time compare; any mismatch is a bare 404 identical for every wrong path/method; random port; 5 minutes. | A neighbour who sees the QR/URL (shoulder-surf, sniffing the plain HTTP) can use it until it stops. HTTP is not encrypted (user accepted). |
| Sniffing/injection on Wi-Fi | None beyond the token's lifetime. The token is in the URL path (sent in clear). | Plain HTTP: a passive listener learns the token and the content. Documented. |
| Flooding / resource use | 4 connections, 10 s idle, 120 s lifetime, 8 KB headers, 20 MB bodies, 100 KB text, 30 uploads, 6 pending, one decode at a time. | Up to ~120 MB of bodies in memory at worst; a full 30 uploads fill the history with up to 30 pictures (pruned by `max_images`). |
| Malicious picture (decompression bomb, parser bug) | Format whitelist from content, header size check before decode (80 MP), Qt allocation limit, decode on a worker, re-encoded to PNG (the original bytes are never stored). | Bugs in Qt's image plugins (libjpeg, libpng, ...) are the exposure; the same plugins read any copied picture. |
| Request smuggling / parsing tricks | One request per connection; before the token has matched, EVERY failure (bad request line, odd headers, too long header) is the same bare 404; with the right token, Transfer-Encoding and duplicate/odd Content-Length are 400; no keep-alive; bytes after the body are ignored; no percent-decoding of paths. | |
| Cross-site requests from a web page on the phone/PC (CSRF, DNS rebinding) | The token is unguessable and in the path; no cookies, no CORS headers (a cross-origin page cannot read answers and cannot POST non-simple bodies with the token it does not know); `Cache-Control: no-store`, `Referrer-Policy: no-referrer`, CSP `connect-src 'self'`. | |
| Secrets typed or photographed | The usual pipeline: secret/password detection, filters, auto-forget, panic. The window shows the item (the user's own). | A password photographed from a screen is just text. |
| Token or content in logs / on disk | Debug log: counts and sizes only; token wiped on stop; no temp files; the DB is the only place. | Item text stays in the open window until it is closed (cleared on panic). |
| A phone page that phones home | The page has no external address (tested); CSP forbids other origins. | |
| Left running | 5-minute timer, hard cap of 30 minutes; stops on window close/quit/panic. | The user may Extend repeatedly up to 30 minutes at a time. |

## Reviews (privacy-reviewer, platform-reviewer) and what was done
- Fixed: uniform 404 before the token; address list cleared from the window when the session ends; items cleared on stop; reservation
  accounting; a session counter drops results of a stopped session; stop on screen lock; text refused as a password is not left in the
  window; delayed paste after minimizing; macOS `NSLocalNetworkUsageDescription`; test worker cannot hang the suite.
- Known, not done: listens on all IPv4 interfaces and accepts any private peer (not only the offered subnet); no per-peer connection cap
  (one LAN host can hold the 4 slots, slowloris-style, for the session); no Host/Content-Type check; OCR error text goes to the phone;
  the byte buffers are not wiped (copies are shared with the worker).

## Not done / ideas
- HTTPS (a self-signed certificate warns on phones), PC to phone, non-picture files, a "drop folder" (Syncthing from the phone, watched by ClipItTo).
- The phone page is English only.
- IPv6 listening (the QR carries IPv4 only).
- Real phones and firewalls are untested (docs/untested.md).
