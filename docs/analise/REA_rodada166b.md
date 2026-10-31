# Round 166b — in-app update check + Update button (v0.1.21)

LYNX-own feature (FL Studio has its own update check through Image-Line's account system; nothing to match): the spec is
the user-approved "In-app update check" section of the 0.1.21 plan.

## Where I stopped
- Implemented: core/UpdateCheck (versions, release JSON, flavours, assets, checksums, release notes, settings),
  ui/Updater (check, bar, What's new, Update per flavour, `--apply-update` helper), Settings › General › Updates,
  Help › Check for updates..., PRIVACY.md, docs/STORE.md, NSIS silent-update choices, release.yml note, 15 languages.
- Verified end to end on Windows (installer, portable, skip, manual check, damaged download, offline) and Linux
  (AppImage) with a local fixture; see "Verification". Landed.
- Left for later: macOS path (.dmg to Downloads + reveal) is built on the shared code but not run on a Mac; the first real
  check against GitHub happens with the 0.1.21 release (0.1.20 has no updater, so 0.1.21 → 0.1.22 is the first real update).

## Design

| Piece | Where | What |
|---|---|---|
| Version compare | `core/UpdateCheck.cpp` `parseVersion` / `compare` | semver 2.0 precedence; the tag `v0.1.21-alpha` vs the running CMake version `0.1.20`; `0.1.20-alpha` < `0.1.20`, so a release build never offers its own tag |
| Release list | `parseReleases` (own small JSON reader, the core has no JUCE) | GitHub `/repos/Ka1-S/LYNX-SHARE/releases?per_page=10`; drafts skipped, prereleases included, tags that are no version dropped, GitHub's `{"message": ...}` errors reported |
| Flavour | `detectFlavour` + `ui/Updater.cpp flavour()` | Windows: packaged (MSIX) → store; `Uninstall.exe` beside LYNX.exe → installer; else portable. macOS. Linux: `$APPIMAGE` → AppImage, else tar.gz. `LYNX_UPDATE_FLAVOUR` forces one (tests) |
| Method | `methodFor` | installer → run setup.exe silently; portable / macOS → download to Downloads and reveal; AppImage → appimageupdatetool (zsync) or the new AppImage beside the old one; tar.gz → open the release page; Store → none (never checks) |
| Asset | `assetFor` | `LYNX-*-setup-x64.exe`, `LYNX-*-windows-x64.zip`, `*.dmg`, `LYNX-*-x86_64.AppImage` (not the .zsync), `LYNX-*-linux-x86_64.tar.gz`; names fixed by release.yml (comment added there) |
| Checksum | `checksumFor` / `Sha256Stream` | every download is checked against the same release's `SHA256SUMS.txt`; no line or a mismatch = no update (the file is deleted) |
| Notes | `releaseNotes` | Markdown → headings / bullets / paragraphs (links keep their text, images, HTML, `**`, backticks go); the What's new window draws headings in the accent colour |
| Settings | `updates.txt` (automatic, skipped, lastCheck), `update-cache.json` | the list is reused for 3 h by the automatic check (GitHub: 60 unauthenticated calls / h / address) |
| Check | `updater::startAutomaticCheck` | 3 s after the window is up (normal start only, never `--opentime` or other command-line modes), worker thread, 10 s connect / 20 s total, silent on failure |
| Test mode | | `LYNX_TEST_MODE=1` never touches the network unless `LYNX_UPDATE_URL` names a fixture; `LYNX_UPDATE_VERSION` fakes the running version |
| Bar | `updater::UpdateBar` under the toolbar | available / downloading (progress line, Cancel) / ready ("Install and restart", "Restart now") / saved ("Show in folder") / failed ("Try again", "Open the release page") / updated; ✕ on "available" = skip this version |
| Installer update | `installAndRestart` + `runApplyHelper` | after the save prompt (`confirmDiscard`, as on quit), LYNX copies itself to `%TEMP%\LYNX-update\LYNX-updater.exe` and starts it with `--apply-update <setup> <folder> <pid> [project]`, then quits. The helper waits for the pid, waits until LYNX.exe can be written (plugin sandbox children), runs `setup.exe /S /D=<folder>` (raw command line: NSIS wants `/D=` last and unquoted), writes `result.txt`, starts LYNX again with the project. The next start shows "LYNX SHARE was updated to x" or why it failed, and deletes the temp files |
| NSIS | `resources/installer/LYNX.nsi` `SilentKeepsChoices` | a silent install keeps the choices of the install it updates: no desktop shortcut / .flp "Open with" entry the user had left out |
| AppImage | `scripts/package-linux.sh` | update information was already embedded (`gh-releases-zsync|Ka1-S|LYNX-SHARE|latest-all|LYNX-*-x86_64.AppImage.zsync`; `latest-all` because every LYNX release is a prerelease — `latest` would skip them) and the .zsync is published |

## Verification
Fixture: `releases.json` in GitHub's shape (a draft 0.2.0 that must be skipped, 0.1.99-alpha with the real 0.1.20 notes as
body, older releases), `SHA256SUMS.txt`, a `LYNX-0.1.99-alpha-setup-x64.exe` built by makensis from `LYNX.nsi` with a marker
file, the portable zip; served by `python3 -m http.server`; `LYNX_UPDATE_URL` points at it, `LYNX_UPDATE_VERSION=0.1.20`
fakes the old version. Every run: fresh `LYNX_PROFILE_DIR`, `LYNX_TEST_MODE=1`, install folder on E:, registry restored after.

| Case | Result |
|---|---|
| Core tests (`Update166bTests.inc`, 7 tests) + a standalone harness of the same file | 129 checks, 0 failures |
| Snapshot (`--snapshot`): bar and What's new | `-update-bar.png`, `-update-notes.png` look right |
| Windows: silent install of 0.1.20 (`/S /D=`) | no desktop shortcut, no .flp "Open with" entry (`SilentKeepsChoices`) |
| Windows installer flavour: bar → What's new → Update | download 0.6 s, checksum OK, helper waits for the pid, `setup /S` in 2 s, DisplayVersion 0.1.99, marker present, relaunched; next start shows "LYNX SHARE was updated to …" (no new check that session), temp files gone |
| What's new with the real 45 KB notes | opens at once (a `TextEditor` took > 1 min to insert them; replaced by an `AttributedString` view) |
| ✕ on the bar | `skipped=0.1.99-alpha` in updates.txt, bar gone; Help › Check for updates still offers it |
| Portable flavour | zip saved to Downloads, bar "saved to … / Show in folder" |
| Damaged download (bad SHA256SUMS) | "The update failed: the download is damaged…", Try again / Open the release page, file deleted |
| Running the newest version, Help › Check for updates | "You're up to date: LYNX SHARE 0.1.99-alpha is the newest version." |
| Offline (nothing listening) | automatic check silent: no bar, no dialog |
| Linux AppImage (WSLg) | new AppImage downloaded and checked beside the old one, chmod +x, "Restart now" |
| GitHub alert markers in notes (`> [!IMPORTANT]`) | dropped (seen raw in the first run; fixed + test) |
