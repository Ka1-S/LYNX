# Round 165n (store165) — LYNX on the Microsoft Store (MSIX)

User (2026-10-27): "monta aí o Microsoft, é de graça". Goal: Windows users install LYNX from the Microsoft Store
(free individual developer account; the Store signs the MSIX after certification), so no SmartScreen / antivirus
warnings. The user creates the Partner Center account and submits; this round prepares everything else.

Evidence markers: **[DOC]** Microsoft Learn (MSIX docs), **[CODE]** LYNX source, **[TEST]** test run (local or CI),
**[REG]** registry/OS state read on this PC.

## Where I stopped

- 19:05 started: briefs read, worktree ../lynx-wt-store165 on feature/store165 (from main 7367aca4).
- Developer Mode is OFF on this PC [REG: HKLM\...\AppModelUnlock has no AllowDevelopmentWithoutDevLicense]; an
  unsigned MSIX needs an elevated PowerShell [DOC unsigned-package] → no local install possible without UAC / a
  setting change. Plan: real install test on the GitHub Windows runner (admin, -AllowUnsigned with the test OID
  publisher), a local `--packageinfo` check with a simulated package identity, and ask the user (via the coordinator)
  about Developer Mode.
- 19:20 done (committed, pushed wip/store165): MSIX manifest template + assets + layout script, release.yml pack +
  install test, runtime branches (src/platform/Package.*, core/StorePackage.h, tests/Store165Tests.inc), docs/STORE.md,
  store/ kit (EN/pt-BR listing, 5 screenshots 1920x1080, logos, CHECKLIST.pt-BR.md, winget draft), PRIVACY.md and
  RELEASE.md notes. Website change on origin/wip/store165-site (not landed, not deployed).
- Waiting: GitHub Actions backlog (every repo run queued since ~21:40 UTC) for the Release run 40592904988 on
  wip/store165; local Windows build (build-win-store165) queued in buildslot.

## 1. Facts that shaped the design

1. **AppData virtualization (Windows 10 1903+)** [DOC behind-the-scenes]: *new* files under AppData\Local / Roaming go to
   a per-package private folder and are merged back only for the package's processes; files that already exist in the
   real AppData are opened and modified in place; every HKCU write is copy-on-write. → Explorer would not see crash
   reports / autosaves written to `%APPDATA%\LYNX`. Decision: a packaged LYNX uses
   `%LOCALAPPDATA%\Packages\<family>\LocalCache\Roaming\LYNX` (the redirect target itself — a real path) as its
   settings folder, with a one-time copy of the installer build's preferences. `unvirtualizedResources` (restricted
   capability, extra Store review) was not needed.
2. **Unsigned install** [DOC unsigned-package]: needs the publisher OID `2.25.311729368913984317654407730594956997722`
   and, for a package with executables, an elevated PowerShell. Developer Mode (loose `-Register`) is off here [REG].
   → the real install test runs on the GitHub Windows runner (admin), never on the user's PC.
3. **No Windows self-updater exists in LYNX** [CODE grep]: nothing to switch off for the Store build.
4. **Explorer file open**: `MainComponent::openFile` handles .lynx, .flp, .mid/.midi, .zip — not lone audio files
   [CODE] → the manifest associates .lynx, .flp, .mid, .midi only.
5. **LYNX ASIO registration** for other hosts points at the exe folder, which changes on every Store update → not
   offered in the Store build (hint text says so; new pt-BR string).
