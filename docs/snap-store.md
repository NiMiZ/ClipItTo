# Publishing the snap (Snap Store / Ubuntu App Center)

For the maintainer. Back to the [README](../README.md).

## One time

1. A Snapcraft account and the registered name: `snapcraft register clipitto` (done).
2. A login token that can only upload and release this snap, as a repository secret:
   `snapcraft export-login --snaps clipitto --acls package_access,package_push,package_release creds.txt`, then
   Settings › Secrets and variables › Actions › new secret `SNAPCRAFT_STORE_CREDENTIALS` with the file's contents. Delete
   `creds.txt`. The token expires after a while (the default is a year): export a new one then.
3. On snapcraft.io › clipitto › Listing: screenshots (the popup with a few copies, Settings), a category (Utilities),
   and the website / source / issues links (the snap's metadata already carries them).

From then on every `vX.Y.Z` tag uploads the snap to the **candidate** channel after the GitHub release is out.
In the store, promote a revision from candidate to stable when it has been tried (`snapcraft release clipitto <revision> stable`,
or the Releases page).

## The first upload is reviewed by a person

ClipItTo asks for permissions that the store does not grant automatically. Without them the snap installs, but the
features below don't work, and the store holds the first revision for review. Ask for them on the
[Snapcraft forum](https://forum.snapcraft.io) (category *store-requests*) with this text:

> **Snap:** clipitto. A clipboard history manager (GPL-3.0, https://github.com/NiMiZ/ClipItTo). It makes no network
> connections; history stays on the user's computer.
>
> **Requests**
> - `password-manager-service` (auto-connect): the optional "encrypt history" and "password vault" features keep their keys
>   in the user's keyring (Secret Service) instead of next to the data.
> - `desktop-legacy`: reading and writing the X11 clipboard and sending the paste keystroke to the previous window on X11
>   sessions. A clipboard manager cannot work without it on X11.
> - a `dbus` **slot** named `io.github.nimiz.ClipItTo` on the session bus: the `clipitto` command and the GNOME Shell
>   extension (installed separately, not part of the snap) talk to the background daemon over this name, and the daemon
>   activates on demand. The name is the reverse-DNS name of the project.
> - `home` (already default-allowed for the user): choosing a file for backup export and import.

Be ready to answer follow-up questions in the thread; reviewers usually reply within a few days.

## What the snap cannot do

A snap cannot install a GNOME Shell extension, so on GNOME Wayland clipboard monitoring and popup placement need the
`.deb` or the AppImage (the store description says so). X11 and other desktops work.
