---
name: release
description: Build release packages for Linux and Windows and tag a version. Run manually with /release <version>.
disable-model-invocation: true
argument-hint: "[version, e.g. 1.4.0]"
---

Prepare release $ARGUMENTS:

1. Stop if the working tree is dirty (`git status --porcelain` not empty) and tell me.
2. Update the version in `CMakeLists.txt` (`project(... VERSION $ARGUMENTS)`) and add a `CHANGELOG.md` section summarizing commits since the last tag.
3. Build the release preset for this OS and run the tests. Stop on any failure.
4. Package:
   - Linux: `cpack --preset linux-release` (produce .deb and/or AppImage per the CMake config).
   - Windows: `windeployqt` on the built .exe, then `cpack --preset windows-release` (installer or zip per the CMake config).
5. Show me the artifacts produced and the changelog text. Wait for my "go" before creating the commit and the `v$ARGUMENTS` tag.
6. Don't push. Tell me the push command to run.

Packages for the other OS have to be built on that OS (or in CI); say so at the end.
