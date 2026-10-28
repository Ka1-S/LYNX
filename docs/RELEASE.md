# Release steps

Official release files come from GitHub Actions (`.github/workflows/release.yml`, see `docs/CODE_SIGNING.md`):
a tag push builds `LYNX.exe` from the tagged source, runs the self-test on Windows and attaches to a draft release:

- `LYNX-x.y.z-setup-x64.exe` — per-user installer (NSIS, `resources/installer/LYNX.nsi`, no admin rights): installs to
  `%LOCALAPPDATA%\Programs\LYNX`, Start-menu shortcut, optional desktop shortcut, `.lynx` association, optional
  "Open with LYNX" for `.flp` (FL Studio stays the default program), uninstaller in *Apps & features*. The workflow
  also installs and uninstalls it silently as a check.
- `LYNX-x.y.z-windows-x64.zip` — portable build (extract and run), with `Demo.lynx`.
- `LYNX-x.y.z-x86_64.AppImage` — Linux, one file (no "linux" in the name: AppImage naming rule; embeds AppImageUpdate information, with its `.zsync` next to it; the AppImage
  catalog warns otherwise). Built in an Ubuntu 22.04 container, so it needs only glibc 2.35; the job fails when
  `scripts/check-linux-compat.sh` finds a newer glibc symbol or a dependency on the system's libstdc++, or when
  `scripts/check-appimage-start.sh` sees it quit within 12 s offline. Carries AppStream metadata
  (`resources/linux/com.lynx_share.LYNX.appdata.xml`).
- `LYNX-x.y.z-linux-x86_64.tar.gz` — Linux folder with `install.sh` (per-user install).
- `LYNX-x.y.z-macos-universal.dmg` — macOS, ad-hoc signed.
- `LYNX-x.y.z-store.msix` — the Microsoft Store package (unsigned; the Store signs it), uploaded by hand in Partner
  Center. The workflow installs a test-identity copy of it and runs LYNX inside the package (`docs/STORE.md`).
- `LYNX.exe` and `SHA256SUMS.txt` (SHA-256 of every file above).

`workflow_dispatch` (Actions → Release → Run workflow, or `gh workflow run release.yml --ref <branch>`) builds and
checks everything without a tag and keeps the files as workflow artifacts only.

Steps:
1. Bump the version in `CMakeLists.txt` (`project(LYNX VERSION x.y.z)`, the single source) and the download badge in
   `README.md`.
2. Locally: `scripts/build-local.sh "/mnt/c/Program Files/Image-Line/FL Studio 2026/Data"` → `0 failures`;
   `flock ~/.cache/lynx-winbuild.lock scripts/build-windows-zig.sh`; from a Windows folder `LYNX.exe --selftest r.txt`
   (`result=OK`) and `LYNX.exe --scantest <folder with a VST3/CLAP> r.txt` (a test CLAP: `tools/testplugin`).
3. Optional local package check: `scripts/make-installer.sh` (defaults to `build-win/LYNX.exe`) writes the installer,
   the zip and `SHA256SUMS.txt` to `dist/`. NSIS: `makensis` on PATH, `$MAKENSIS`, or the NSIS zip release unpacked
   into `~/opt/nsis-<version>` (its `makensis.exe` runs through WSL interop). Quick installer check:
   `setup.exe /S /D=C:\some\test\folder`, then `C:\some\test\folder\Uninstall.exe /S`.
4. `git tag vx.y.z-alpha && git push origin vx.y.z-alpha`; when the workflow finishes, edit the draft release notes
   and publish it.
5. Microsoft Store: upload `LYNX-x.y.z-alpha-store.msix` as a new submission (`docs/STORE.md` › *Updating the Store
   version*).
