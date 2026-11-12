# Encrypted history and password copies

Back to the [README](../README.md).

## When ClipItTo uses the keyring

Only two features use the system keyring, and ClipItTo asks it only when one of them needs it:

- **Encrypted history** keeps its key there. It is read at every start while the history is encrypted (the history
  can't be opened without it).
- **Password copies** keep a second key there, used only to recognise the same password copied again and to find a
  whole password while locked. It is read the first time a password is copied or searched, not at start. Your master
  password and the passwords themselves never go to the keyring, so password copies work without one (no
  duplicate detection, no whole-password search while locked).

With both off, ClipItTo never touches the keyring. Before it does, a notification says what it is asking for, at
most every ten minutes, because the keyring (GNOME Keyring, KWallet) may show its own "unlock" window.
`clipitto --status` has a *keyring* line with the last time each key was asked for.

## Encrypted history

Settings › *Encrypt the history on disk* encrypts `history.db` (copies, notes, groups, the
search index) with 256-bit ChaCha20-Poly1305 via the bundled
[SQLite3 Multiple Ciphers](https://github.com/utelle/SQLite3MultipleCiphers). The random key
is kept in the system keyring (GNOME Keyring / KWallet through Secret Service, or the
Windows Credential Manager), so there is no password to type; without a reachable keyring
the option says why it is unavailable. Turning it on or off converts the history once, in
the background. It protects the file at rest (backups, other accounts, a stolen disk
without full-disk encryption), not against programs running as you, which can read the
clipboard and the keyring anyway. Copies made before turning it on may still be
recoverable from free disk space (old journal blocks); only full-disk encryption covers
that. If the keyring entry is lost, the history cannot be read. History files are created
readable by your account only.

Measured cost (`build/debug/tests/bench_encryption`, debug build, 1000 copies):

| | plain | encrypted |
|---|---|---|
| open + first read | 0.6 ms | 27 ms |
| store one copy | 0.37 ms | 0.88 ms |
| search | 4.0 ms | 4.2 ms |
| one-time conversion | | ~50 ms |

The build downloads the SQLite3 Multiple Ciphers amalgamation (pinned version and SHA-256);
offline builds can set `FETCHCONTENT_SOURCE_DIR_SQLITE3MC` to an unpacked copy, or
configure with `-DCLIPITTO_ENCRYPTION=OFF` to use the system SQLite.

## Password copies

Password managers (KeePassXC, Bitwarden, KeePass, 1Password, ...) mark what they copy as
secret. Without a master password ClipItTo skips those copies, as before; the popup says once
how to keep them. Once you set a master password in Settings › *Passwords*:

- **Kept encrypted.** A marked copy is encrypted the moment it arrives (X25519 + XChaCha20-Poly1305,
  via the built-in [Monocypher](https://monocypher.org)) and only that is stored. It works while
  locked: copies are encrypted to a public key, and only unlocking can open them.
- **Hidden.** It is listed as `●●●●` with a padlock, its note set to the app it came from
  (Windows, X11, GNOME; not with wl-paste). Ordinary search finds it by note or group only.
- **Unlocked with the master password.** Choosing one (Enter), *Show password* (10 s in its row)
  and *Not a password* ask for it first. An unlock lasts 5 minutes (`password_unlock_minutes`);
  `clipitto --lock` or *Lock passwords* ends it. Unlocking takes about 0.1 s (Argon2id, 64 MiB).
  On Windows, *Unlock with Windows Hello too* lets your face, fingerprint or PIN do it.
- **Pasted marked, then cleared.** A password pasted back carries the password-manager markers,
  so other clipboard managers and Windows' clipboard history skip it too (not through the GNOME
  extension, which can put only one format on the clipboard). It leaves the clipboard
  2 s after the paste (`password_clear_seconds`; 0 = never, up to 60) but only if the clipboard still holds it:
  something you copied meanwhile stays (Windows and X11/Wayland check that ClipItTo still owns the clipboard and
  that it holds the password; macOS compares the pasteboard's change count; through the GNOME extension, which
  cannot be read back, only a newer copy that ClipItTo saw counts). The clearing is not recorded as a copy, and
  one still waiting when ClipItTo quits is done then. Earlier versions had 30 s as the default and an hour as the limit; a value over 60 in the file is reported and the default used.
- **Searched with a keyword.** Type `p[] ` (the keyword and a space; Settings can change it):
  what follows shows as asterisks and searches inside your passwords while unlocked, or for a
  whole password while locked. Locked searching needs the system keyring, which keeps the key
  for it; the history file alone can't be used to test guesses, and with the keyring each guess
  still costs a memory-hard hash (Argon2id, 16 MiB).
- Right-click *Mark as password* turns any copy into one (its text is wiped from the file);
  *Put new password copies in the "Passwords" group* files them there (on by default). With
  that on, *Show passwords entries in Recent* chooses whether they also appear in the Recent list
  (as `●●●●`) or only in the group and the `p[]` search.
- *After copying a password, ask for a name* (on by default): when a **new** password is
  copied, ClipItTo opens with a box to name it. The app it came from is suggested as grey
  text that goes away when you type; leave the box empty (or cancel) to keep that name. The
  window closes again afterwards if it wasn't open. (On Windows the window may not take the
  focus when it opens by itself.)

**Moving a copy into the Passwords group** (Ctrl+G, or right-click › Group) makes it a password copy at the same time,
so there is no need to move it *and* mark it. It needs a master password (or the no-master mode): without one the copy is
not moved, because plain text in a group called Passwords would only look protected. *Mark as password* in the right-click
menu stays for copies that should keep their place. If the same password is already kept, that copy stays and is the one
in the group.

**Copies that look like secrets.** Once passwords are kept, a copy that no password manager marked but
that looks like a private key, an API key or access token (AWS, GitHub, GitLab, Slack, Google, Stripe,
Anthropic, OpenAI, npm, a JSON web token) or a whole-copy card number (Luhn-checked) is kept encrypted too,
and shown as `●●●●` with the kind of secret as its note ("GitHub token"). It judges by shape, so now and
then it takes an ordinary copy: *Not a password* in the right-click menu turns it back into text. Turn it
off with *Keep copies that look like secrets as password copies* (`detect_secrets`). Without a master
password nothing changes.

**Copies that look like generated passwords (off by default).** A copy from a browser's "new password"
field carries no marker, so nothing tells it from text. Tick *Keep copies that look like generated
passwords as password copies* (`detect_passwords`) and a single-line copy of 8 to 64 characters without
spaces that mixes at least three of lower case, upper case, digits and symbols (with at least four letters)
is kept like any password copy, with the note "Looks like a password". URLs, paths, email addresses,
colours, UUIDs, hashes, dates and names made of plain words and numbers (`getUserName2`, `Password123`) are
left alone, and so are passphrases of words (`correct-horse-battery`). **This is a guess and will take some
ordinary copies** (an unusual identifier, a code, a token of an unknown service); they then show as `●●●●`
until you use *Not a password* in the right-click menu. That is why it is off. Like the option above, it
needs a master password (or *Use without one…*); otherwise the box is disabled.

**Your own rules.** When the guess is too loose or too strict, define what counts as a password yourself
(Settings › Passwords › *Password rules*, config key `password_rules`, up to 5). A rule is a length range
(6 to 256; shorter rules would catch ordinary words), minimum counts of lower case letters, upper case letters, digits and symbols, and *digits at the
end*. Counts are minimums: "2 digits" means at least 2, and with a length of 3 or more it is not satisfied
by two digits alone. A single-line copy without spaces that fits any rule is kept as a password copy with
the note "Matches your password rule". Rules extend the built-in detection; they never turn it off. Type a
sample in the *Try it* box to see which rule keeps it (the sample is only checked on screen, never saved or
logged). Impossible rules (shortest above longest, counts above the longest length) are refused and OK stays
disabled. Like the options above, rules need a master password or *Use without one…*.

**Auto-forget and password copies.** The `auto_forget` rules (Settings › Passwords › *Auto-forget*, see
[usage.md](usage.md)) never remove a password copy and never read one: a password copy is deleted only by you
(*Delete*, *Clear history*, *Turn off…*). A secret-looking copy that is *not* a password copy (no master password,
or `detect_secrets` off) is plain text and can be removed by a *Looks like a secret* rule after N minutes, which is a
good use for it.

**Choosing a master password.** The dialog lists the rules and ticks them as you type: at least 8
characters with an uppercase letter, a lowercase letter and a digit; no form of "password", "admin" or
"root" (so `P@ssw0rd123` and `ADM1N` are refused), not your user name, and no three digits in a row
counting up or down (`123`, `654`; `12` is fine). A few unrelated words with a number make a strong one.

**Windows Hello and Touch ID.** *Unlock with Windows Hello too* (Windows) or *Unlock with Touch ID too*
(Mac, Touch ID set up in System Settings) lets a face, fingerprint or PIN open the password copies instead
of typing the master password; turning it on asks for the master password once. On Windows the key comes
from a Windows Hello signature. **On a Mac Touch ID is a gate, not a lock:** ClipItTo asks for your
fingerprint before it reads a random key from your login Keychain, but the app is signed ad hoc and so
can't tie that key to the Secure Enclave. A program running as you could read the Keychain item without
the fingerprint; the master password still protects the file itself.

**Forgot it? (Windows Hello / Touch ID)** If it is turned on, Settings › Passwords shows *Forgot it…*:
unlock with it, choose a new master password, and every password copy is kept under it. (Hello / Touch ID has
to be turned on again afterwards.) Without Hello, a forgotten master password can't be recovered.

**Without a master password.** *Use without one…* (Settings › Passwords) keeps password copies hidden
(`●●●●`) but **not protected**: they are only obscured, so anyone who can read your history file or use
your account can read them. ClipItTo asks you to confirm, and says "Not secure" on that page. There is
nothing to unlock, and Windows Hello is off. An export then asks whether to write the password copies as
**plain text** into the file; answer *No* to export without them. *Set…* later moves the existing copies
to a real master password; *Turn off…* deletes them.

The master password is never stored or shown; only a key derived from it unlocks. Changing it
also changes the key every password copy is sealed with. **If you forget it, the password
copies can't be recovered**: *Remove…* deletes them and starts over.

This protects them in the history file and from people using your computer while they are
locked. It doesn't protect them from programs running as you: those can read the clipboard
when a password is copied, and on GNOME a copied password travels from the shell extension to
the daemon over your session's D-Bus. Passwords don't lock by themselves when the screen locks
yet; use `clipitto --lock` (e.g. from your screen-lock hook) or a short unlock time.

```bash
clipitto --toggle | --show | --hide     # what the shortcut runs
clipitto --lock                         # lock the password copies now
clipitto --status                       # backend, item counts, extension state
clipitto --reload                       # after editing ~/.config/clipitto/config.toml
```

Settings: see [config.example.toml](../data/config.example.toml) (installed under
`/usr/share/doc/clipitto/`). History lives in `~/.local/share/clipitto/history.db`.
Choosing a copy also pastes it into the window you came from; turn that off in Settings
(*After choosing a copy: Paste it into the window I came from*) or with `paste_on_enter = false` to only copy it. On GNOME Wayland
pasting needs the bundled extension.
