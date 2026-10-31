# Round 166b — in-app update check + Update button (v0.1.21)

LYNX-own feature (FL Studio has its own update check through Image-Line's account system; nothing to match): the spec is
the user-approved "In-app update check" section of the 0.1.21 plan.

## Where I stopped
- Implemented: core/UpdateCheck (versions, release JSON, flavours, assets, checksums, release notes, settings),
  ui/Updater (check, bar, What's new, Update per flavour, `--apply-update` helper), Settings › General › Updates,
  Help › Check for updates..., PRIVACY.md, docs/STORE.md, NSIS silent-update choices, release.yml note, 15 languages.
- Next: Windows end-to-end test with a local fixture (see "Verification"), then land.

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
(filled in below as it is done)
