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
- 20:15 coordinator: Store name **LYNX SHARE** reserved ("LYNX" taken); repo variables set to the real identity
  (KA1WARE.LYNXSHARE, CN=BACBB786-…, KA1WARE; family KA1WARE.LYNXSHARE_sj8hjwabzpdr8, Store ID 9NQRFBFDCX6N). Done:
  Properties/DisplayName from MSIX_DISPLAY_NAME (default LYNX SHARE), Start-menu name stays LYNX; layout script defaults
  = the real identity; docs/checklist updated; website branch points at 9NQRFBFDCX6N.
- Still waiting (20:20): Release run 40592904988 queued (Actions backlog, ~2 runners for the whole repo); local
  Windows build queued behind 4 others on the winbuild lock. Next: local --packageinfo check with
  LYNX_SIMULATE_PACKAGE_FAMILY, CI evidence, land.
- 21:10 coordinator: listings in 13 more languages → store/listings/*.md (15 files + es-mx reuse) written (3 translator
  sub-agents, checked: structure, limits, no vendor names in search terms, UI-language notice), scripts/
  store-listing-csv.py fills Partner Center's exported CSV (tested with a synthetic export: 16 language columns,
  screenshots/logos copied, CJK lines joined without spaces). Partner Center accepts listing languages the package
  doesn't declare ("Manage additional languages") → manifest Resources unchanged (en-us, pt-br).
- 21:08 Windows compile check (zig, fast lane) of every changed C++ file: no errors, no warnings. Full local Windows
  build dropped (winbuild queue > 2 h); land.sh builds it. MSIX workflow steps set continue-on-error until a CI run has
  proven them (Actions backlog: Release run 40592904988 still queued at 21:10).
- 21:12 merged origin/main, landing via land.sh.
- 00:32 land #1 conflicted (Paths.cpp, tests.cpp), land #2 failed: I had merged LOCAL main while it held tune165's merge
  (rolled back later) → its LYNX Tune/Vox tests failed, plus I18nData.h not regenerated. Fixed: branch reset to my last
  commit, merged only pushed main, regenerated I18nData.h (tools/make_i18n.py), Store165 test include moved off the end
  of tests.cpp's list; wip/store165 force-pushed once (my own wip branch, to drop tune165's leaked commits).
- 02:40 land #3 queued behind ~12 landings on the merge lock; Release run 40592904988: Version, Build LYNX.exe,
  Linux, macOS green; Windows "Self-test and package" (the MSIX job) still queued on GitHub's runners.

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
