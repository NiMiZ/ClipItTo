# Making a release

For the maintainer. Back to the [README](../README.md).

A release is **a version bump merged into `main`, then a tag**. The tag is what starts the release: yes, you need one.
Pushing a tag named `vX.Y.Z` makes GitHub Actions build every package and publish the GitHub Release.

## The steps

1. **Pick the version** (X.Y.Z): fixes only → raise Z (0.9.2 → 0.9.3); a new feature → raise Y (0.9 → 0.10.0).
   The number counts releases, not fixes; the count of fixes goes in the release notes.
2. **Release branch and bump.** From an up-to-date `main`: `git switch -c release/X.Y.Z`. Change the version in
   `CMakeLists.txt` (`VERSION`), `vcpkg.json`, `gnome-extension/clipitto@nimiz.github.io/metadata.json`,
   `packaging/windows/clipitto.iss`, `snap/snapcraft.yaml`, and the `.deb` file names in `docs/install.md` and
   `docs/building.md` (a `sed 's/OLD/NEW/'` over those files does it).
3. **Release notes.** Write `docs/releases/vX.Y.Z.md`, starting with the count ("4 bug fixes, 1 improvement") and the list.
   The release page opens with this file; GitHub's list of merged pull requests follows it.
4. **Pull request.** Push the branch, open a PR into `main`, wait for CI to be green, merge it.
5. **Tag the merge commit.** In a clone of `main` after pulling:
   ```
   git tag -a vX.Y.Z -m "ClipItTo X.Y.Z"      # on the commit you are standing on (main's latest)
   git push origin vX.Y.Z
   ```
   `-a` makes an annotated tag (it carries a message and a date); `git tag` lists them; `git push origin vX.Y.Z` sends one tag.
   To undo a tag that was never released: `git push origin --delete vX.Y.Z` and `git tag -d vX.Y.Z`.
   Without the command line: GitHub › Releases › *Draft a new release* › *Choose a tag* › type `vX.Y.Z` › *Create new tag on
   publish*, target `main`. That also starts the build.
6. **Wait for the build** (about 20-30 minutes; longer when the translation libraries are not cached). The jobs: Linux
   (sanitizers), the `.deb`, the AppImage, the snap, Windows, macOS; then *GitHub Release* publishes the files and
   `SHA256SUMS`, *Install the release* checks them on fresh Ubuntu and Windows machines, and *Snap Store* uploads the snap to the
   `candidate` channel (when the `SNAPCRAFT_STORE_CREDENTIALS` secret exists).
7. **Check:** the release page lists the `.deb`, AppImage, snap, Windows installer, macOS `.dmg`, `install.sh`,
   `install.ps1` and `SHA256SUMS`. Promote the snap to stable in the store when it has been tried.

## Building the release files on your own PC (optional)

GitHub Actions builds and publishes every release; this is a shortcut for making the same files yourself, e.g. to try them
before tagging or when Actions is short of minutes. Files go to `dist/<version>/` (ignored by git).

- **Linux, in WSL or on Linux:** `scripts/make-release.sh [--skip-tests] [--only linux|deb|appimage|snap]` builds and
  smoke-tests the `.deb`, the AppImage and the snap. The AppImage is built under `~/.cache/clipitto-release/appimage`
  (`CLIPITTO_APPIMAGE_BUILD` changes it), because linuxdeploy makes symlinks and a Windows drive mounted in WSL
  (`/mnt/c`, `/mnt/i`) refuses them. The snap needs snapcraft with LXD that can reach the internet.
- **Windows:** `scripts/make-release.sh --only windows` calls `scripts/make-release-windows.ps1` through `powershell.exe`
  (or run that script in PowerShell yourself): build, tests, a smoke run of the staged app, and the Inno Setup installer.
  It needs Visual Studio Build Tools (C++), Qt 6.5.3 for MSVC, vcpkg and Inno Setup (`winget install JRSoftware.InnoSetup`).
- **macOS:** only on a Mac: `scripts/make-release-macos.sh [--skip-tests] [--upload]` builds the `.dmg`; `--upload` adds it to the
  GitHub release (`gh release upload`).
- **Publishing:** `scripts/make-release.sh --publish` writes `SHA256SUMS` and runs `gh release create vX.Y.Z` with the notes from
  `docs/releases/vX.Y.Z.md`; the tag must already be pushed. After the Mac added its `.dmg`, `--sums` rebuilds `SHA256SUMS`.
  Nothing is published without `--publish`. Pushing the tag also starts the Actions build, which publishes the same release
  names: use one path or the other for a given version.

**What has been run (0.14.0, in WSL):** the `.deb` and AppImage steps, to the end (the AppImage ran `--version`). **Not run yet:**
the snap step (snapcraft's LXD container had no network in that session), the Windows script, the macOS script, `--publish`,
`--sums` and `--upload`. Treat those as unverified and read the script before the first real use.

## Why builds fail

- **"Artifact storage quota has been hit"** at the last step of a build job: GitHub's free storage for build files is
  full. Branch and PR runs no longer upload files (only tags do), and those expire after 3 days. A private repository on the
  free plan has only 500 MB of this storage and about 2,000 build minutes a month (macOS counts 10 times). After deleting old files, GitHub recalculates usage every 6-12 hours; re-run the failed jobs then.
- **macOS job cancelled with "The job was not acquired by Runner"**: GitHub had no macOS machine free. Re-run it.
- **macOS "hdiutil: create failed - Resource busy"** or "a pbcopy copy was not captured": rare timing problems on the
  macOS runners; the CI retries both. Re-run if it still happens.
- A real test failure shows in the failing step's log (look for `FAIL!`), and needs a fix.
- **The two *Install the release* jobs are skipped** while the repository is private: they download the release the way a
  stranger does, with no login, and a private repository answers 404. They run again when it is public.
- **macOS job "failed because of a scheduled macOS 14 runner image brownout"**: GitHub is retiring the `macos-14` image
  (November 2, 2026); the build now uses `macos-15`. When a runner image is retired the same happens again: change
  `runs-on` in `.github/workflows/ci.yml`.
